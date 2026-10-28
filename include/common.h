#ifndef OMSY_COMMON_H
#define OMSY_COMMON_H

#include <stddef.h>
#include <sys/types.h>

#define STR_MAX   1024
#define LIST_MAX  16

typedef struct {
	char items[LIST_MAX][STR_MAX];
	int count;
} StrList;

/* callback appelé pour chaque ligne "clé = valeurs" d'un fichier de conf */
typedef void (*conf_cb)(const char *key, const StrList *val, void *ud);

/* configuration commune à omsyd et omsyctl (fichier omsy.conf) */
typedef struct {
	char path[STR_MAX];
	char log[STR_MAX];
	char run[STR_MAX];
	char conf[STR_MAX];
	/* NOUVEAU : */
	char bind_addr[64];   /* défaut : 127.0.0.1 */
	int  port;            /* défaut : 8990 */
	int  max_conn;        /* défaut : 64 */
} OmsyConf;

/* ---------- logs ---------- */
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------- texte / configuration ---------- */
void trim(char *s);
int  parse_line(char *line, char **key, char **value);
void parse_list(const char *s, StrList *out);
void first_item(const StrList *l, char *dst, size_t size);
int  conf_load(const char *filename, conf_cb cb, void *ud);

/* file == NULL : $OMSY_CONF, sinon "omsy.conf". Renvoie -1 si illisible
 * (les valeurs par défaut sont quand même remplies). */
int omsy_conf_load(const char *file, OmsyConf *c);

/* extrait "-c fichier" de argv ; renvoie le fichier ou NULL */
const char *conf_option(int *argc, char ***argv);

int mkdir_p(const char *path);

/* ---------- processus ---------- */
int   pid_alive(pid_t pid);
pid_t read_pid_file(const char *path);
int   write_pid_file(const char *path);
int   wait_for(int (*pred)(void *), void *ud, int timeout_ms);

#endif