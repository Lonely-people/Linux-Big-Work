#ifndef PRACTICE_CLIENT_INPUT_H
#define PRACTICE_CLIENT_INPUT_H

#include <stddef.h>

/* 1: message, 0: EOF/interrupted, -1: error. Capacity includes the NUL. */
int client_read_input(char *buffer, size_t capacity);

#endif
