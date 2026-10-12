#ifndef PRACTICE_AI_CLIENT_H
#define PRACTICE_AI_CLIENT_H

#include <stddef.h>

#include "session.h"

#define AI_DEFAULT_HOST "127.0.0.1"
#define AI_DEFAULT_PORT 8080
#define AI_DEFAULT_PATH "/v1/chat/completions"

typedef struct {
    const char *host;
    unsigned short port;
    const char *path;
} ai_client_config_t;

/* Return nonzero from the callback to stop the stream. */
typedef int (*ai_chunk_callback)(const char *text, size_t length, void *userdata);

/* Stream one completion.  Error text is written to error_buffer on failure. */
int ai_client_stream(const ai_client_config_t *config,
                     const session_t *session,
                     ai_chunk_callback callback,
                     void *userdata,
                     char *error_buffer,
                     size_t error_capacity);

#endif /* PRACTICE_AI_CLIENT_H */
