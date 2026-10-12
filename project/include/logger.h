#ifndef PRACTICE_LOGGER_H
#define PRACTICE_LOGGER_H

#include <stddef.h>

#define LOGGER_PATH_MAX 4096

typedef struct {
    int fd;
    unsigned long session_id;
    char path[LOGGER_PATH_MAX];
} logger_t;

/* Open (or create) the log belonging to one client session. */
int logger_open(logger_t *logger, const char *directory, unsigned long session_id);

/* Append one timestamped USER or ASSISTANT block. */
int logger_write_user(logger_t *logger, const char *text);
int logger_write_assistant(logger_t *logger, const char *text);

void logger_close(logger_t *logger);

#endif /* PRACTICE_LOGGER_H */
