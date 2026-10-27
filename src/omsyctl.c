#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "../include/common.h"

#define MAX_PIDS 256

typedef struct {
	char label[STR_MAX];
	char shell[STR_MAX];
	StrList bins;
	char start[STR_MAX];
	StrList stop;
} ConfServiceKey;

/* ---------- configs des services (via conf_load commun) ---------- */

static void service_cb(const char *key, const StrList *v, void *ud) {
	ConfServiceKey *s = ud;
	if (!strcmp(key, "label"))      first_item(v, s->label, sizeof s->label);
	else if (!strcmp(key, "shell")) first_item(v, s->shell, sizeof s->shell);
	else if (!strcmp(key, "start")) first_item(v, s->start, sizeof s->start);
	else if (!strcmp(key, "bins"))  s->bins = *v;
	else if (!strcmp(key, "stop"))  s->stop = *v;
}

static int load_service_config(const char *confdir, const char *name, ConfServiceKey *svc) {
	char filename[2 * STR_MAX + 8];
	snprintf(filename, sizeof filename, "%s/%s.conf", confdir, name);

	if (conf_load(filename, service_cb, svc) != 0)
		return -1;

	if (!svc->label[0]) snprintf(svc->label, sizeof svc->label, "%s", name);
	if (!svc->shell[0]) snprintf(svc->shell, sizeof svc->shell, "/bin/sh");
	return 0;
}

/* ---------- recherche de processus (pgrep, sans shell : portable Linux/macOS) ---------- */

static int pgrep_name(const char *name, pid_t *pids, int max) {
	int fd[2];
	if (pipe(fd) < 0) return 0;

	pid_t pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return 0;
	}
	if (pid == 0) {
		dup2(fd[1], STDOUT_FILENO);
		close(fd[0]);
		close(fd[1]);
		execlp("pgrep", "pgrep", "-x", "--", name, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);

	FILE *f = fdopen(fd[0], "r");
	int n = 0, v;
	while (f && n < max && fscanf(f, "%d", &v) == 1)
		pids[n++] = (pid_t)v;
	if (f) fclose(f);
	waitpid(pid, NULL, 0);
	return n;
}

static int list_pids(const StrList *bins, pid_t *pids, int max) {
	int n = 0;
	for (int i = 0; i < bins->count; i++)
		n += pgrep_name(bins->items[i], pids + n, max - n);
	return n;
}

static int bins_gone(void *ud) {
	pid_t pids[1];
	return list_pids(ud, pids, 1) == 0;
}

static void signal_all(const StrList *bins, int sig) {
	pid_t pids[MAX_PIDS];
	int n = list_pids(bins, pids, MAX_PIDS);
	for (int i = 0; i < n; i++) {
		printf("  kill -%d %d\n", sig, pids[i]);
		kill(pids[i], sig);
	}
}

/* ---------- exécution de commandes ---------- */

/* équivalent de "nohup cmd &" : nouvelle session, SIGHUP ignoré, stdio -> /dev/null */
static void detach_self(void) {
	setsid();
	signal(SIGHUP, SIG_IGN);

	pid_t pid = fork();                          /* double fork : pas de zombie */
	if (pid < 0) _exit(1);
	if (pid > 0) _exit(0);

	int dn = open("/dev/null", O_RDWR);
	dup2(dn, STDIN_FILENO);
	dup2(dn, STDOUT_FILENO);
	dup2(dn, STDERR_FILENO);
	if (dn > 2) close(dn);
}

/* lance `cmd` avec le shell du service ($USER, $HOME... expansés par lui) */
static void run_shell(const ConfServiceKey *svc, const char *cmd, int detach) {
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return;
	}
	if (pid == 0) {
		if (detach) detach_self();
		execlp(svc->shell, svc->shell, "-c", cmd, (char *)NULL);
		perror(svc->shell);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
}

/* ---------- actions ---------- */

static void start(const ConfServiceKey *svc) {
	pid_t pids[MAX_PIDS];

	if (!svc->start[0]) {
		printf("%s : pas de commande de démarrage\n", svc->label);
		return;
	}
	if (list_pids(&svc->bins, pids, MAX_PIDS) > 0) {
		printf("%s : déjà actif (pid %d)\n", svc->label, pids[0]);
		return;
	}

	printf("  $ %s\n", svc->start);
	run_shell(svc, svc->start, 1);

	sleep(1);
	if (list_pids(&svc->bins, pids, MAX_PIDS) > 0)
		printf("%s : lancé (pid %d)\n", svc->label, pids[0]);
	else
		printf("%s : lancé mais aucun processus détecté\n", svc->label);
}

static void stop(const ConfServiceKey *svc) {
	if (svc->stop.count == 0) {
		printf("%s : pas de commande d'arrêt\n", svc->label);
		return;
	}
	for (int i = 0; i < svc->stop.count; i++) {
		printf("  $ %s\n", svc->stop.items[i]);
		run_shell(svc, svc->stop.items[i], 0);
	}

	/* Filet de sécurité : encore actif après 3 s => SIGTERM, puis 15 s d'attente */
	void *bins = (void *)&svc->bins;
	if (svc->bins.count > 0 && !wait_for(bins_gone, bins, 3000)) {
		signal_all(&svc->bins, SIGTERM);
		if (!wait_for(bins_gone, bins, 15000)) {
			printf("%s : toujours actif après 15 s\n", svc->label);
			return;
		}
	}
	printf("%s : arrêté\n", svc->label);
}

static void status(const ConfServiceKey *svc) {
	pid_t pids[MAX_PIDS];
	int n = list_pids(&svc->bins, pids, MAX_PIDS);

	if (n == 0) {
		printf("%s : arrêté\n", svc->label);
		return;
	}
	printf("%s : actif (pid", svc->label);
	for (int i = 0; i < n; i++) printf(" %d", pids[i]);
	printf(")\n");
}

/* ---------- main ---------- */

static void dump_service(const ConfServiceKey *svc, const char *name) {
	printf("%s.conf : %s\n", name, svc->label);
	printf("  shell: %s\n", svc->shell);
	printf("  start: %s\n", svc->start);
	for (int j = 0; j < svc->bins.count; j++)
		printf("  bins[%d]: %s\n", j, svc->bins.items[j]);
	for (int j = 0; j < svc->stop.count; j++)
		printf("  stop[%d]: %s\n", j, svc->stop.items[j]);
}

static int do_service(const char *confdir, const char *action, const char *name) {
	ConfServiceKey svc = {0};
	if (load_service_config(confdir, name, &svc) != 0) {
		fprintf(stderr, "%s : config introuvable\n", name);
		return -1;
	}
	dump_service(&svc, name);

	if (!strcmp(action, "start"))      start(&svc);
	else if (!strcmp(action, "stop"))  stop(&svc);
	else                               status(&svc);
	return 0;
}

static int valid_action(const char *a) {
	return !strcmp(a, "start") || !strcmp(a, "stop") || !strcmp(a, "status");
}

int main(int argc, char **argv) {
	const char *conffile = conf_option(&argc, &argv);
	if (argc < 3 || !valid_action(argv[1])) {
		fprintf(stderr, "Usage : %s [-c omsy.conf] start|stop|status service1 [service2 ...]\n",
		        argv[0]);
		return 1;
	}

	OmsyConf cfg;
	if (omsy_conf_load(conffile, &cfg) != 0)
		return 1;
	if (chdir(cfg.path) != 0) {                  /* les commandes start/stop partent de la racine */
		perror(cfg.path);
		return 1;
	}
	printf("Action: %s\npath: %s\nconf: %s\n", argv[1], cfg.path, cfg.conf);

	int rc = 0;
	for (int i = 2; i < argc; i++)
		if (do_service(cfg.conf, argv[1], argv[i]) != 0)
			rc = 1;
	return rc;
}