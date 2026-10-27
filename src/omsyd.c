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
#include <netinet/in.h>
#include "../include/common.h"

#define PORT             8990
#define IDLE_TIMEOUT_SEC 60
#define MIN_UPTIME_SEC   5    /* en dessous : on considère que c'est un crash en boucle */
#define MAX_BACKOFF_SEC  30

static OmsyConf cfg;
static char pid_path[2 * STR_MAX];
static char log_path[2 * STR_MAX];

/* config commune + chemins dérivés ; on se place dans la racine (handler.py) */
static void load_config(const char *file) {
	if (omsy_conf_load(file, &cfg) != 0)
		fprintf(stderr, "omsyd : config illisible, valeurs par défaut\n");
	snprintf(pid_path, sizeof pid_path, "%s/omsy-server.pid", cfg.run);
	snprintf(log_path, sizeof log_path, "%s/omsy-server.log", cfg.log);
	if (chdir(cfg.path) != 0) {
		perror(cfg.path);
		exit(1);
	}
}

static void make_dirs(void) {
	if (mkdir_p(cfg.run) < 0) perror(cfg.run);
	if (mkdir_p(cfg.log) < 0) perror(cfg.log);
}

/* ====================================================================
 *  Worker : la boucle accept
 * ==================================================================== */

static volatile sig_atomic_t running = 1;
static pid_t parent_pid;                         /* superviseur */

static void worker_term(int sig) { (void)sig; running = 0; }

static void worker_chld(int sig) {
	(void)sig;
	int saved = errno;
	while (waitpid(-1, NULL, WNOHANG) > 0) {}   /* on récolte les zombies */
	errno = saved;
}

static void worker_signals(void) {
	struct sigaction sa = {0};
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = worker_term;                 /* pas de SA_RESTART */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = worker_chld;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);

	signal(SIGPIPE, SIG_IGN);
}

static int open_listener(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		log_msg("socket: %s", strerror(errno));
		return -1;
	}
	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(PORT)
	};
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		log_msg("bind port %d: %s", PORT, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 16) < 0) {
		log_msg("listen: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/* lance handler.py avec la socket client ; ferme toujours client_fd côté parent */
static void spawn_handler(int client_fd, int server_fd) {
	struct timeval tv = { .tv_sec = IDLE_TIMEOUT_SEC };
	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	pid_t pid = fork();
	if (pid == 0) {
		close(server_fd);
		signal(SIGPIPE, SIG_DFL);
		char fd_str[16];
		snprintf(fd_str, sizeof fd_str, "%d", client_fd);
		execlp("python3", "python3", "sys/handler/handler.py", fd_str, NULL);
		log_msg("execlp python3: %s", strerror(errno));
		_exit(127);
	}
	if (pid < 0) log_msg("fork: %s", strerror(errno));
	else         log_msg("Connexion reçue -> python (pid %d)", pid);
	close(client_fd);
}

static void accept_loop(int server_fd) {
	while (running) {
		/* superviseur mort (ex. SIGKILL) : on ne reste pas orphelin */
		if (getppid() != parent_pid) break;

		/* poll avec timeout : pas de course entre le signal et accept() */
		struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
		if (poll(&pfd, 1, 1000) <= 0) continue;

		int client_fd = accept(server_fd, NULL, NULL);
		if (client_fd < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
			log_msg("accept: %s", strerror(errno));
			sleep(1);                            /* évite de boucler à 100% CPU */
			continue;
		}
		spawn_handler(client_fd, server_fd);
	}
}

static void worker_loop(pid_t supervisor_pid) __attribute__((noreturn));

static void worker_loop(pid_t supervisor_pid) {
	parent_pid = supervisor_pid;
	if (getppid() != parent_pid) exit(1);

	worker_signals();
	int server_fd = open_listener();
	if (server_fd < 0) exit(1);

	log_msg("En écoute sur le port %d", PORT);
	accept_loop(server_fd);

	close(server_fd);
	log_msg("Worker arrêté proprement");
	exit(0);
}

/* ====================================================================
 *  Superviseur : relance le worker
 * ==================================================================== */

static volatile sig_atomic_t stopping = 0;
static volatile pid_t worker_pid = 0;

static void sup_term(int sig) {
	(void)sig;
	stopping = 1;
	pid_t p = worker_pid;
	if (p > 0) kill(p, SIGTERM);                 /* kill() est async-signal-safe */
}

static void supervisor_signals(void) {
	struct sigaction sa = {0};
	sa.sa_handler = sup_term;
	sigemptyset(&sa.sa_mask);                    /* pas de SA_RESTART */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGHUP, SIG_IGN);
}

/* lance un worker, attend sa fin, renvoie sa durée de vie (s) */
static time_t run_worker_once(void) {
	time_t start = time(NULL);
	pid_t me = getpid();
	pid_t pid = fork();
	if (pid < 0) {
		log_msg("fork worker: %s", strerror(errno));
		return 0;
	}
	if (pid == 0) worker_loop(me);

	worker_pid = pid;
	if (stopping) kill(pid, SIGTERM);            /* signal arrivé avant worker_pid */
	log_msg("Worker lancé (pid %d)", pid);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	worker_pid = 0;

	if (WIFSIGNALED(status))
		log_msg("Worker tué par le signal %d", WTERMSIG(status));
	else
		log_msg("Worker terminé avec le code %d", WEXITSTATUS(status));
	return time(NULL) - start;
}

static void supervisor(void) __attribute__((noreturn));

static void supervisor(void) {
	supervisor_signals();
	write_pid_file(pid_path);
	log_msg("Superviseur démarré");

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

/* ====================================================================
 *  Daemonisation
 * ==================================================================== */

static int daemon_up(void *unused) {
	(void)unused;
	return pid_alive(read_pid_file(pid_path));
}

/* processus d'origine : attend que le daemon soit réellement prêt */
static void parent_wait_ready(pid_t child) __attribute__((noreturn));

static void parent_wait_ready(pid_t child) {
	waitpid(child, NULL, 0);
	if (wait_for(daemon_up, NULL, 2000)) {
		printf("Daemon démarré (PID %d), logs : %s\n",
		       read_pid_file(pid_path), log_path);
		exit(0);
	}
	fprintf(stderr, "Le daemon n'a pas démarré, voir %s\n", log_path);
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
	pid = fork();                                /* 2e fork : plus de terminal possible */
	if (pid < 0) exit(1);
	if (pid > 0) _exit(0);

	umask(022);
	redirect_stdio(lf);
}

/* ====================================================================
 *  Commandes
 * ==================================================================== */

static int pid_gone(void *p) { return !pid_alive(*(pid_t *)p); }

static int cmd_start(int foreground) {
	make_dirs();
	pid_t old = read_pid_file(pid_path);
	if (pid_alive(old)) {
		printf("Déjà lancé (PID %d)\n", old);
		return 1;
	}
	remove(pid_path);                            /* pid file périmé */
	if (!foreground) daemonize();
	supervisor();
}

static int cmd_stop(void) {
	pid_t pid = read_pid_file(pid_path);
	if (!pid_alive(pid)) {
		printf("Pas de daemon en cours\n");
		remove(pid_path);
		return 1;
	}
	kill(pid, SIGTERM);
	if (wait_for(pid_gone, &pid, 5000)) {
		printf("Daemon arrêté\n");
		return 0;
	}
	fprintf(stderr, "Pas d'arrêt après 5 s, envoi de SIGKILL\n");
	kill(pid, SIGKILL);
	remove(pid_path);
	return 0;
}

static int cmd_status(void) {
	pid_t pid = read_pid_file(pid_path);
	if (pid_alive(pid)) {
		printf("En cours (PID %d)\n", pid);
		return 0;
	}
	printf("Arrêté\n");
	return 3;
}

int main(int argc, char **argv) {
	load_config(conf_option(&argc, &argv));
	const char *cmd = argc > 1 ? argv[1] : "toggle";

	if (!strcmp(cmd, "start"))  return cmd_start(0);
	if (!strcmp(cmd, "run"))    return cmd_start(1);
	if (!strcmp(cmd, "stop"))   return cmd_stop();
	if (!strcmp(cmd, "status")) return cmd_status();
	if (!strcmp(cmd, "toggle"))
		return pid_alive(read_pid_file(pid_path)) ? cmd_stop() : cmd_start(0);

	fprintf(stderr, "Usage: %s [-c omsy.conf] [start|stop|status|run|toggle]\n", argv[0]);
	return 2;
}