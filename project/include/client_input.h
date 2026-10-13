#ifndef PRACTICE_CLIENT_INPUT_H
#define PRACTICE_CLIENT_INPUT_H

#include <stddef.h>

/* 1: message, 0: EOF/interrupted, -1: error. Capacity includes the NUL. */
int client_read_input(char *buffer, size_t capacity);

/* Event-based editor. The caller owns terminal mode, signals and repainting. */
typedef struct client_editor client_editor_t;
enum { CLIENT_EDITOR_EDIT, CLIENT_EDITOR_SUBMIT, CLIENT_EDITOR_EXIT,
       CLIENT_EDITOR_PAGE_UP, CLIENT_EDITOR_PAGE_DOWN };
client_editor_t *client_editor_create(size_t capacity);
void client_editor_destroy(client_editor_t *editor);
const char *client_editor_text(const client_editor_t *editor);
const char *client_editor_notice(const client_editor_t *editor);
void client_editor_clear(client_editor_t *editor);
int client_editor_read_key(client_editor_t *editor);
int client_editor_pending(const client_editor_t *editor);
int client_editor_height(client_editor_t *editor, int columns, int maximum);
void client_editor_draw(client_editor_t *editor, int first_row, int height);
#endif
