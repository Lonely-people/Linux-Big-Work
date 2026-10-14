#ifndef PRACTICE_SERVER_LOG_H
#define PRACTICE_SERVER_LOG_H

#include <stddef.h>

#define SERVER_LOG_PATH_MAX 4096

typedef struct {
    int fd;      /* Open file descriptor for server.log; -1 when file logging is unavailable. */
    int console; /* Non-zero to mirror every line to stdout. */
    char path[SERVER_LOG_PATH_MAX];
} server_log_t;

/* Open (or create) the append-only server activity log next to the session logs. */
int server_log_open(server_log_t *log, const char *directory, int console);

/* Append one timestamped line. One write(2) per line keeps concurrent children interleaving-free. */
int server_log_write(server_log_t *log, const char *text);

void server_log_close(server_log_t *log);

#endif /* PRACTICE_SERVER_LOG_H */
