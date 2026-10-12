#include "session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static char *duplicate_string(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

int session_init(session_t *session) {
    if (session == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(session, 0, sizeof(*session));
    return 0;
}

void session_destroy(session_t *session) {
    if (session == NULL) {
        return;
    }
    for (size_t index = 0; index < session->count; ++index) {
        free(session->messages[index].role);
        free(session->messages[index].content);
    }
    free(session->messages);
    memset(session, 0, sizeof(*session));
}

static void remove_message_at(session_t *session, size_t index) {
    free(session->messages[index].role);
    free(session->messages[index].content);
    if (index + 1 < session->count) {
        memmove(&session->messages[index], &session->messages[index + 1],
                (session->count - index - 1) * sizeof(session->messages[0]));
    }
    --session->count;
}

static int ensure_capacity(session_t *session) {
    if (session->count < session->capacity) {
        return 0;
    }
    size_t new_capacity = session->capacity == 0 ? 8 : session->capacity * 2;
    session_message_t *messages = realloc(session->messages,
                                          new_capacity * sizeof(*messages));
    if (messages == NULL) {
        return -1;
    }
    session->messages = messages;
    session->capacity = new_capacity;
    return 0;
}

static int add_message(session_t *session, const char *role, const char *content) {
    if (session == NULL || role == NULL || content == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ensure_capacity(session) < 0) {
        return -1;
    }

    char *role_copy = duplicate_string(role);
    char *content_copy = duplicate_string(content);
    if (role_copy == NULL || content_copy == NULL) {
        free(role_copy);
        free(content_copy);
        return -1;
    }
    session->messages[session->count].role = role_copy;
    session->messages[session->count].content = content_copy;
    ++session->count;

    while (session->count > SESSION_MAX_MESSAGES) {
        /* History is stored as user/assistant pairs, so drop the oldest pair. */
        remove_message_at(session, 0);
        if (session->count > 0 &&
            strcmp(session->messages[0].role, "assistant") == 0) {
            remove_message_at(session, 0);
        }
    }
    return 0;
}

int session_add_user(session_t *session, const char *content) {
    return add_message(session, "user", content);
}

int session_add_assistant(session_t *session, const char *content) {
    return add_message(session, "assistant", content);
}

void session_remove_last_user(session_t *session) {
    if (session == NULL || session->count == 0) {
        return;
    }
    if (strcmp(session->messages[session->count - 1].role, "user") == 0) {
        remove_message_at(session, session->count - 1);
    }
}

size_t session_count(const session_t *session) {
    return session == NULL ? 0 : session->count;
}

const session_message_t *session_message_at(const session_t *session, size_t index) {
    if (session == NULL || index >= session->count) {
        return NULL;
    }
    return &session->messages[index];
}
