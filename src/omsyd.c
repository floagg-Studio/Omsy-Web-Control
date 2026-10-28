#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/common.h"

#define IDLE_TIMEOUT_SEC        30
#define MIN_UPTIME_SEC          5
#define MAX_BACKOFF_SEC         30
#define ORPHAN_CLEANUP_RETRIES  3
#define ORPHAN_CLEANUP_DELAY_MS 1000

static OmsyConf cfg;
static char pid_path[2 * STR_MAX];
static char log_path[2 * STR_MAX];
static const char *prog_name = "omsyd";

/* ======================= Fichier PID ======================= */

/* Format : "superviseur_pid worker_pid" (worker peut être 0) */
static void write_pids(pid_t sup, pid_t worker) {
	char tmp[2 * STR_MAX];
	if (snprintf(tmp, sizeof tmp, "%s.tmp", pid_path) >= (int)sizeof tmp) return;
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	if (worker > 0) fprintf(f, "%d %d\n", (int)sup, (int)worker);
	else            fprintf(f, "%d\n", (int)sup);
	fclose(f);
	rename(tmp, pid_path);
}

static void read_pids(pid_t *sup, pid_t *worker) {
	*sup = 0; *worker = 0;
	FILE *f = fopen(pid_path, "r");
	if (!f) return;
	int s = 0, w = 0;
	int n = fscanf(f, "%d %d", &s, &w);
	fclose(f);
	if (n >= 1 && s > 0) *sup = s;
	if (n >= 2 && w > 0) *worker = w;
}

static int group_alive(pid_t pgid) {
	if (pgid <= 0) return 0;
	return kill(-pgid, 0) == 0 || errno == EPERM;
}

/* Vrai si QUELQU'UN occupe bind_addr:port (nous ou un tiers).
 * SO_REUSEADDR permet de traverser TIME_WAIT sans faux positif. */
static int port_bound(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return 0;
	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)cfg.port),
	};
	if (inet_pton(AF_INET, cfg.bind_addr, &addr.sin_addr) != 1)
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	int r = bind(fd, (struct sockaddr *)&addr, sizeof addr);
	int e = errno;
	close(fd);
	return r < 0 && (e == EADDRINUSE || e == EACCES);
}

/* ======================= Découverte de processus ======================= */

/* Renvoie les pids qui ÉCOUTENT sur cfg.port. */
static int discover_port_pids(pid_t *pids, int max) {
	char cmd[96];
	snprintf(cmd, sizeof cmd,
	         "lsof -nP -t -iTCP:%d -sTCP:LISTEN 2>/dev/null", cfg.port);
	FILE *f = popen(cmd, "r");
	if (!f) return 0;
	int n = 0, pid;
	while (n < max && fscanf(f, "%d", &pid) == 1) {
		if (pid > 0) pids[n++] = pid;
	}
	pclose(f);
	return n;
}

static int process_comm(pid_t pid, char *out, size_t size) {
	char cmd[64];
	snprintf(cmd, sizeof cmd, "ps -p %d -o comm= 2>/dev/null", (int)pid);
	FILE *f = popen(cmd, "r");
	if (!f) return -1;
	if (!fgets(out, size, f)) { pclose(f); return -1; }
	pclose(f);
	trim(out);
	const char *b = strrchr(out, '/');
	if (b) memmove(out, b + 1, strlen(b + 1) + 1);
	return 0;
}

static int is_our_omsyd(pid_t pid) {
	char comm[256];
	if (process_comm(pid, comm, sizeof comm) != 0) return 0;
	return !strcmp(comm, "omsyd") || !strcmp(comm, prog_name);
}

static int wrk_holds_port(pid_t wrk) {
	if (wrk <= 0) return 0;
	pid_t pids[32];
	int n = discover_port_pids(pids, 32);
	for (int i = 0; i < n; i++)
		if (pids[i] == wrk) return 1;
	return 0;
}

/* Tue uniquement les omsyd orphelins sur NOTRE port. Max `attempts` essais. */
static int cleanup_orphans_on_port(int attempts, int delay_ms) {
	if (getenv("OMSY_NO_CLEAN")) {
		fprintf(stderr, "OMSY_NO_CLEAN défini, nettoyage désactivé\n");
		return !port_bound();
	}

	for (int attempt = 1; attempt <= attempts && port_bound(); attempt++) {
		pid_t pids[32];
		int n = discover_port_pids(pids, 32);
		if (n == 0) break;

		int killed_ours = 0, foreign = 0;
		for (int i = 0; i < n; i++) {
			if (!is_our_omsyd(pids[i])) { foreign++; continue; }
			fprintf(stderr, "  orphelin omsyd pid %d → SIGTERM (%d/%d)\n",
			        pids[i], attempt, attempts);
			kill(pids[i], SIGTERM);
			killed_ours = 1;
		}
		if (!killed_ours) {
			fprintf(stderr, "  port tenu par process tiers — ignoré\n");
			return 0;
		}
		usleep(delay_ms * 1000);
	}

	/* Dernier recours : SIGKILL sur nos orphelins survivants */
	if (port_bound()) {
		pid_t pids[32];
		int n = discover_port_pids(pids, 32);
		for (int i = 0; i < n; i++) {
			if (!is_our_omsyd(pids[i])) continue;
			fprintf(stderr, "  orphelin omsyd pid %d → SIGKILL\n", pids[i]);
			kill(pids[i], SIGKILL);
		}
		usleep(500 * 1000);
	}
	return !port_bound();
}

/* ======================= Configuration & répertoires ======================= */

static void load_config(const char *file) {
	if (omsy_conf_load(file, &cfg) != 0)
		fprintf(stderr, "omsyd : config illisible, valeurs par défaut\n");
	snprintf(pid_path, sizeof pid_path, "%s/omsy-server.pid", cfg.run);
	snprintf(log_path, sizeof log_path, "%s/omsy-server.log", cfg.log);
	if (chdir(cfg.path) != 0) { perror(cfg.path); exit(1); }
}

static void make_dirs(void) {
	if (mkdir_p(cfg.run) < 0) perror(cfg.run);
	if (mkdir_p(cfg.log) < 0) perror(cfg.log);
}

/* ======================= Worker ======================= */

static volatile sig_atomic_t running = 1;
static pid_t parent_pid;
static int active_children = 0;

/* FIX CRITIQUE : PAS de kill() dans le handler (sinon boucle infinie :
 * le SIGTERM se réarme indéfiniment et le worker ne rend jamais la main,
 * ce qui laisse le socket LISTEN ouvert après la mort du superviseur). */
static void worker_term(int sig) {
	(void)sig;
	running = 0;
}

static void worker_chld(int sig) {
	(void)sig;
	int saved = errno;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		__atomic_sub_fetch(&active_children, 1, __ATOMIC_SEQ_CST);
	errno = saved;
}

static void worker_signals(void) {
	struct sigaction sa = {0};
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = worker_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);

	sa.sa_handler = worker_chld;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);

	signal(SIGPIPE, SIG_IGN);
}

static int open_listener(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { log_msg("socket: %s", strerror(errno)); return -1; }

	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)cfg.port),
	};
	if (inet_pton(AF_INET, cfg.bind_addr, &addr.sin_addr) != 1) {
		log_msg("bind_addr invalide '%s', fallback 127.0.0.1", cfg.bind_addr);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		log_msg("bind %s:%d: %s", cfg.bind_addr, cfg.port, strerror(errno));
		close(fd); return -1;
	}
	if (listen(fd, 16) < 0) {
		log_msg("listen: %s", strerror(errno));
		close(fd); return -1;
	}
	log_msg("En écoute sur %s:%d (max_conn=%d)",
	        cfg.bind_addr, cfg.port, cfg.max_conn);
	return fd;
}

static void apply_child_limits(void) {
	struct rlimit rl;
	rl.rlim_cur = rl.rlim_max = 256;
	setrlimit(RLIMIT_NOFILE, &rl);
	rl.rlim_cur = rl.rlim_max = 60;
	setrlimit(RLIMIT_CPU, &rl);
	rl.rlim_cur = rl.rlim_max = 1024UL * 1024 * 1024;
	setrlimit(RLIMIT_AS, &rl);
}

static void spawn_handler(int client_fd, int server_fd) {
	struct timeval tv = { .tv_sec = IDLE_TIMEOUT_SEC };
	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	pid_t pid = fork();
	if (pid == 0) {
		close(server_fd);
		signal(SIGPIPE, SIG_DFL);
		apply_child_limits();
		char fd_str[16];
		snprintf(fd_str, sizeof fd_str, "%d", client_fd);
		execlp("python3", "python3", "sys/handler/handler.py", fd_str, NULL);
		log_msg("execlp python3: %s", strerror(errno));
		_exit(127);
	}
	if (pid < 0) {
		log_msg("fork: %s", strerror(errno));
	} else {
		__atomic_add_fetch(&active_children, 1, __ATOMIC_SEQ_CST);
		log_msg("Connexion -> python (pid %d, actifs %d)",
		        pid, __atomic_load_n(&active_children, __ATOMIC_SEQ_CST));
	}
	close(client_fd);
}

static void accept_loop(int server_fd) {
	while (running) {
		if (getppid() != parent_pid) break;

		struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 1000);
		if (pr < 0) {
			if (errno == EINTR) continue;
			log_msg("poll: %s", strerror(errno));
			sleep(1); continue;
		}
		if (pr == 0) continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			log_msg("listener cassé (revents=0x%x)", pfd.revents);
			break;
		}
		if (!(pfd.revents & POLLIN)) continue;

		int client_fd = accept(server_fd, NULL, NULL);
		if (client_fd < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
				continue;
			if (errno == EMFILE || errno == ENFILE) {
				log_msg("trop de fds, pause 1 s");
				sleep(1); continue;
			}
			log_msg("accept: %s", strerror(errno));
			sleep(1); continue;
		}

		if (__atomic_load_n(&active_children, __ATOMIC_SEQ_CST) >= cfg.max_conn) {
			log_msg("Refus : %d handlers actifs (max %d)",
			        active_children, cfg.max_conn);
			close(client_fd);
			continue;
		}
		spawn_handler(client_fd, server_fd);
	}
}

static void worker_loop(pid_t supervisor_pid) __attribute__((noreturn));

static void worker_loop(pid_t supervisor_pid) {
	parent_pid = supervisor_pid;
	setpgid(0, 0);                  /* idempotent — le parent le fait aussi */
	if (getppid() != parent_pid) exit(1);

	worker_signals();
	int server_fd = open_listener();
	if (server_fd < 0) {
		log_msg("worker: bind impossible, sortie");
		exit(1);                    /* le superviseur va backoff/redémarrer */
	}

	accept_loop(server_fd);
	close(server_fd);               /* ← le socket est rendu ICI */

	/* Nettoyage des handlers survivants, hors handler de signal */
	kill(0, SIGTERM);
	for (int i = 0; i < 20; i++) {
		if (__atomic_load_n(&active_children, __ATOMIC_SEQ_CST) == 0) break;
		usleep(100000);
	}
	log_msg("Worker arrêté proprement");
	exit(0);
}

/* ======================= Superviseur ======================= */

static volatile sig_atomic_t stopping = 0;
static volatile pid_t worker_pid = 0;

static void sup_term(int sig) {
	(void)sig;
	stopping = 1;
	pid_t p = worker_pid;
	if (p > 0) kill(-p, SIGTERM);   /* groupe = worker + handlers */
}

static void supervisor_signals(void) {
	struct sigaction sa = {0};
	sa.sa_handler = sup_term;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	signal(SIGHUP, SIG_IGN);
}

static time_t run_worker_once(void) {
	time_t start = time(NULL);
	pid_t me = getpid();
	pid_t pid = fork();
	if (pid < 0) {
		log_msg("fork worker: %s", strerror(errno));
		return 0;
	}
	if (pid == 0) worker_loop(me);  /* ne revient pas */

	/* FIX : supprime la course fork()/setpgid() côté parent. Le groupe
	 * existe dès que fork() a renvoyé, donc kill(-pid, ...) est fiable. */
	setpgid(pid, pid);

	worker_pid = pid;
	write_pids(getpid(), pid);
	if (stopping) kill(-pid, SIGTERM);  /* filet de sécurité */
	log_msg("Worker lancé (pid %d)", pid);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	worker_pid = 0;
	write_pids(getpid(), 0);

	if (WIFSIGNALED(status))
		log_msg("Worker tué par signal %d", WTERMSIG(status));
	else
		log_msg("Worker terminé (code %d)", WEXITSTATUS(status));
	return time(NULL) - start;
}

static void supervisor(void) __attribute__((noreturn));

static void supervisor(void) {
	supervisor_signals();
	write_pids(getpid(), 0);
	log_msg("Superviseur démarré (pid %d)", getpid());

	unsigned backoff = 1;
	while (!stopping) {
		time_t uptime = run_worker_once();
		if (stopping) break;
		if (uptime >= MIN_UPTIME_SEC) backoff = 1;
		else if ((backoff *= 2) > MAX_BACKOFF_SEC) backoff = MAX_BACKOFF_SEC;
		log_msg("Redémarrage dans %u s", backoff);
		sleep(backoff);
	}
	remove(pid_path);
	log_msg("Superviseur arrêté");
	exit(0);
}

/* ======================= Daemonisation ======================= */

static int daemon_up(void *unused) {
	(void)unused;
	pid_t sup, wrk;
	read_pids(&sup, &wrk);
	return pid_alive(sup) && pid_alive(wrk) && port_bound();
}

static void parent_wait_ready(pid_t child) __attribute__((noreturn));

static void parent_wait_ready(pid_t child) {
	waitpid(child, NULL, 0);
	if (wait_for(daemon_up, NULL, 3000)) {
		pid_t sup = 0, wrk = 0;
		read_pids(&sup, &wrk);
		printf("Daemon démarré (PID %d), logs : %s\n", sup, log_path);
		exit(0);
	}
	fprintf(stderr,
	    "Le daemon n'a pas démarré (port lié ? superviseur vivant ?), "
	    "voir %s\n", log_path);
	exit(1);
}

static void redirect_stdio(int log_fd) {
	int dn = open("/dev/null", O_RDWR);
	dup2(dn, STDIN_FILENO);
	dup2(log_fd, STDOUT_FILENO);
	dup2(log_fd, STDERR_FILENO);
	if (dn > 2) close(dn);
	if (log_fd > 2) close(log_fd);
}

static void daemonize(void) {
	int lf = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (lf < 0) { perror(log_path); exit(1); }

	pid_t pid = fork();
	if (pid < 0) { perror("fork"); exit(1); }
	if (pid > 0) parent_wait_ready(pid);

	if (setsid() < 0) exit(1);

	pid = fork();
	if (pid < 0) exit(1);
	if (pid > 0) _exit(0);

	umask(022);
	redirect_stdio(lf);
}

/* ======================= Commandes ======================= */

static int cmd_start(int foreground) {
	make_dirs();

	pid_t old_sup = 0, old_wrk = 0;
	read_pids(&old_sup, &old_wrk);

	/* 1. Instance saine ? */
	if (pid_alive(old_sup) && wrk_holds_port(old_wrk)) {
		printf("Déjà lancé (PID %d)\n", old_sup);
		return 1;
	}

	/* 2. Instance dégradée (sup vivant mais worker mort / port ailleurs) */
	if (pid_alive(old_sup) || group_alive(old_wrk)) {
		fprintf(stderr, "État dégradé, arrêt de l'ancienne instance...\n");
		if (pid_alive(old_sup))   kill(old_sup, SIGTERM);
		if (group_alive(old_wrk)) kill(-old_wrk, SIGTERM);
		for (int i = 0; i < 30; i++) {
			if (!pid_alive(old_sup) && !group_alive(old_wrk)) break;
			usleep(100000);
		}
		if (pid_alive(old_sup))   kill(old_sup, SIGKILL);
		if (group_alive(old_wrk)) kill(-old_wrk, SIGKILL);
		usleep(300000);
	}

	/* 3. Orphelins non référencés sur le port : X tentatives de nettoyage */
	if (port_bound()) {
		fprintf(stderr,
		    "Port %s:%d occupé — nettoyage orphelins (max %d essais)...\n",
		    cfg.bind_addr, cfg.port, ORPHAN_CLEANUP_RETRIES);
		if (!cleanup_orphans_on_port(ORPHAN_CLEANUP_RETRIES,
		                             ORPHAN_CLEANUP_DELAY_MS)) {
			pid_t pids[32];
			int n = discover_port_pids(pids, 32);
			fprintf(stderr, "Port toujours occupé — process tiers :\n");
			for (int i = 0; i < n; i++) {
				char comm[256] = "?";
				process_comm(pids[i], comm, sizeof comm);
				fprintf(stderr, "  pid %d (%s)\n", pids[i], comm);
			}
			fprintf(stderr, "Abandon. Identifiez avec : lsof -i :%d\n",
			        cfg.port);
			return 1;
		}
		printf("Orphelins nettoyés, port %d libéré\n", cfg.port);
	}

	remove(pid_path);
	if (!foreground) daemonize();
	supervisor();
}

static int cmd_stop(void) {
	pid_t sup = 0, wrk = 0;
	read_pids(&sup, &wrk);

	if (!pid_alive(sup) && !group_alive(wrk)) {
		if (port_bound()) {
			fprintf(stderr,
			    "Aucun pid référencé mais port %d occupé — kill manuel requis "
			    "(lsof -i :%d)\n", cfg.port, cfg.port);
			remove(pid_path);
			return 1;
		}
		printf("Pas de daemon en cours\n");
		remove(pid_path);
		return 1;
	}

	/* Cible superviseur + groupe worker (worker + handlers python) */
	if (pid_alive(sup))   kill(sup, SIGTERM);
	if (group_alive(wrk)) kill(-wrk, SIGTERM);

	for (int t = 0; t < 50; t++) {
		if (!pid_alive(sup) && !group_alive(wrk)) {
			printf("Daemon arrêté\n");
			remove(pid_path);
			return 0;
		}
		usleep(100000);
	}

	fprintf(stderr, "Pas d'arrêt après 5 s, SIGKILL\n");
	if (pid_alive(sup))   kill(sup, SIGKILL);
	if (group_alive(wrk)) kill(-wrk, SIGKILL);

	for (int t = 0; t < 20; t++) {
		if (!pid_alive(sup) && !group_alive(wrk)) break;
		usleep(100000);
	}
	remove(pid_path);
	return 0;
}

static int cmd_status(void) {
	pid_t sup = 0, wrk = 0;
	read_pids(&sup, &wrk);
	int sup_live = pid_alive(sup);
	int wrk_live = group_alive(wrk);
	int bound    = port_bound();

	if (sup_live && wrk_live && bound && wrk_holds_port(wrk)) {
		printf("En cours (PID %d, worker %d)\n", sup, wrk);
		return 0;
	}
	if (sup_live || wrk_live || bound) {
		printf("Dégradé (sup=%d %s, worker=%d %s, port %s)\n",
		       sup, sup_live ? "vivant" : "mort",
		       wrk, wrk_live ? "vivant" : "mort",
		       bound ? "occupé" : "libre");
		return 1;
	}
	printf("Arrêté\n");
	return 3;
}

int main(int argc, char **argv) {
	if (argc > 0 && argv[0]) {
		const char *b = strrchr(argv[0], '/');
		prog_name = b ? b + 1 : argv[0];
	}
	load_config(conf_option(&argc, &argv));
	const char *cmd = argc > 1 ? argv[1] : "toggle";

	if (!strcmp(cmd, "start"))  return cmd_start(0);
	if (!strcmp(cmd, "run"))    return cmd_start(1);
	if (!strcmp(cmd, "stop"))   return cmd_stop();
	if (!strcmp(cmd, "status")) return cmd_status();
	if (!strcmp(cmd, "toggle")) {
		pid_t sup = 0, wrk = 0;
		read_pids(&sup, &wrk);
		int healthy = pid_alive(sup) && wrk_holds_port(wrk);
		return healthy ? cmd_stop() : cmd_start(0);
	}

	fprintf(stderr,
	    "Usage: %s [-c omsy.conf] [start|stop|status|run|toggle]\n", argv[0]);
	return 2;
}