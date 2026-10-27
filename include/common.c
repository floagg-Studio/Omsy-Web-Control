#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include "common.h"

/* ---------- logs ---------- */

void log_msg(const char *fmt, ...) {
	char buf[1024];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	size_t n = strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S ", &tm);
	n += snprintf(buf + n, sizeof buf - n, "[%d] ", getpid());

	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
	va_end(ap);
	if (m < 0) return;

	n += m;
	if (n > sizeof buf - 2) n = sizeof buf - 2;
	buf[n++] = '\n';
	ssize_t r = write(STDERR_FILENO, buf, n);
	(void)r;
}

/* ---------- texte ---------- */

void trim(char *s) {
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
	char *p = s;
	while (isspace((unsigned char)*p)) p++;
	memmove(s, p, strlen(p) + 1);
}

/* "clé = valeur" -> key/value ; -1 si ligne vide, commentaire ou sans '=' */
int parse_line(char *line, char **key, char **value) {
	trim(line);
	if (*line == '\0' || *line == '#') return -1;

	char *eq = strchr(line, '=');
	if (!eq) return -1;

	*eq = '\0';
	trim(line);
	*key = line;
	*value = eq + 1;
	return 0;
}

/* ---------- listes ---------- */

static const char *parse_quoted(const char *s, char *dst, size_t size) {
	size_t n = 0;
	if (*s == '"') s++;
	while (*s && *s != '"') {
		if (*s == '\\' && s[1]) s++;
		if (n < size - 1) dst[n++] = *s;
		s++;
	}
	dst[n] = '\0';
	return *s == '"' ? s + 1 : s;
}

static const char *parse_bare(const char *s, char *dst, size_t size) {
	size_t n = 0;
	while (*s && *s != ',') {
		if (n < size - 1) dst[n++] = *s;
		s++;
	}
	while (n > 0 && isspace((unsigned char)dst[n - 1])) n--;
	dst[n] = '\0';
	return s;
}

void parse_list(const char *s, StrList *out) {
	out->count = 0;
	while (out->count < LIST_MAX) {
		while (*s == ',' || isspace((unsigned char)*s)) s++;
		if (*s == '\0') break;

		char *dst = out->items[out->count++];
		s = (*s == '"') ? parse_quoted(s, dst, STR_MAX)
		                : parse_bare(s, dst, STR_MAX);
	}
}

void first_item(const StrList *l, char *dst, size_t size) {
	snprintf(dst, size, "%s", l->count > 0 ? l->items[0] : "");
}

/* ---------- fichier de conf générique ---------- */

int conf_load(const char *filename, conf_cb cb, void *ud) {
	FILE *f = fopen(filename, "r");
	if (!f) {
		perror(filename);
		return -1;
	}
	char line[4096];
	StrList list;
	while (fgets(line, sizeof line, f)) {
		char *key, *value;
		if (parse_line(line, &key, &value) != 0) continue;
		parse_list(value, &list);
		cb(key, &list, ud);
	}
	fclose(f);
	return 0;
}

/* ---------- configuration commune (omsy.conf) ---------- */

static void omsy_cb(const char *key, const StrList *v, void *ud) {
	OmsyConf *c = ud;
	if (!strcmp(key, "path"))
		first_item(v, c->path, sizeof c->path);
	else if (!strcmp(key, "log"))
		first_item(v, c->log, sizeof c->log);
	else if (!strcmp(key, "run"))
		first_item(v, c->run, sizeof c->run);
	else if (!strcmp(key, "conf") || !strcmp(key, "confdir"))
		first_item(v, c->conf, sizeof c->conf);
}

/* field vide -> def ; relatif -> base/field */
static void resolve(char *field, const char *base, const char *def) {
	char v[STR_MAX];
	snprintf(v, sizeof v, "%s", field[0] ? field : def);
	int n = (v[0] == '/') ? snprintf(field, STR_MAX, "%s", v)
	                      : snprintf(field, STR_MAX, "%s/%s", base, v);
	if (n >= STR_MAX)
		fprintf(stderr, "omsy : chemin trop long, tronqué : %s\n", field);
}

int omsy_conf_load(const char *file, OmsyConf *c) {
	memset(c, 0, sizeof *c);
	if (!file) file = getenv("OMSY_CONF");
	if (!file) file = "omsy.conf";
	int rc = conf_load(file, omsy_cb, c);

	char cwd[STR_MAX];
	if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
	if (c->path[0]) resolve(c->path, cwd, "");
	else            snprintf(c->path, sizeof c->path, "%s", cwd);

	resolve(c->log,  c->path, "share/log");
	resolve(c->run,  c->path, "share/run");
	resolve(c->conf, c->path, "conf");
	return rc;
}

const char *conf_option(int *argc, char ***argv) {
	if (*argc < 3 || strcmp((*argv)[1], "-c") != 0)
		return NULL;
	const char *file = (*argv)[2];
	(*argv)[2] = (*argv)[0];                     /* argv[0] reste le nom du programme */
	*argv += 2;
	*argc -= 2;
	return file;
}

int mkdir_p(const char *path) {
	char tmp[STR_MAX];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
		*p = '/';
	}
	return (mkdir(tmp, 0755) < 0 && errno != EEXIST) ? -1 : 0;
}

/* ---------- processus ---------- */

int pid_alive(pid_t pid) {
	return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

pid_t read_pid_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	int pid = 0;
	if (fscanf(f, "%d", &pid) != 1) pid = 0;
	fclose(f);
	return pid > 0 ? (pid_t)pid : 0;
}

int write_pid_file(const char *path) {
	FILE *f = fopen(path, "w");
	if (!f) {
		log_msg("fopen %s: %s", path, strerror(errno));
		return -1;
	}
	fprintf(f, "%d\n", getpid());
	fclose(f);
	return 0;
}

/* sonde pred() toutes les 100 ms ; 1 dès qu'elle est vraie, 0 après timeout_ms */
int wait_for(int (*pred)(void *), void *ud, int timeout_ms) {
	for (int t = 0; t < timeout_ms; t += 100) {
		if (pred(ud)) return 1;
		usleep(100000);
	}
	return pred(ud);
}