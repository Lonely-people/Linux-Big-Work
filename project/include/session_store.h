#ifndef PRACTICE_SESSION_STORE_H
#define PRACTICE_SESSION_STORE_H
#include "session.h"
#include <time.h>

#define STORE_PATH_MAX 4096

typedef struct {
    char role; /* U: user, A: assistant, X: unparsed legacy text */
    int context; /* Only complete successful turns enter model context. */
    char *text;
} stored_message_t;

typedef struct {
    unsigned long id;
    time_t updated;
    char *title, *source;
    stored_message_t *messages;
    size_t count;
    int lock_fd;
    char directory[STORE_PATH_MAX];
} stored_session_t;

typedef int (*store_list_callback)(unsigned long id, time_t updated,
                                  int busy, const char *title, void *userdata);
int store_init(const char *directory, const char *legacy_directory);
int store_list(const char *directory, store_list_callback callback, void *userdata);
int store_create(const char *directory, stored_session_t *out);
int store_open(const char *directory, unsigned long id, stored_session_t *out);
void store_close(stored_session_t *session);
int store_context(const stored_session_t *stored, session_t *context);
int store_commit_turn(stored_session_t *stored, const char *question, const char *answer);
#endif
