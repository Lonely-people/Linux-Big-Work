#ifndef PRACTICE_SESSION_H
#define PRACTICE_SESSION_H

#include <stddef.h>

#define SESSION_MAX_MESSAGES 32

typedef struct {
    char *role;
    char *content;
} session_message_t;

typedef struct {
    session_message_t *messages;
    size_t count;
    size_t capacity;
} session_t;

int session_init(session_t *session);
void session_destroy(session_t *session);

int session_add_user(session_t *session, const char *content);
int session_add_assistant(session_t *session, const char *content);
void session_remove_last_user(session_t *session);

size_t session_count(const session_t *session);
const session_message_t *session_message_at(const session_t *session, size_t index);

#endif /* PRACTICE_SESSION_H */
