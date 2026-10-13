#ifndef PRACTICE_PROTOCOL_H
#define PRACTICE_PROTOCOL_H

/*
 * Small line-oriented protocol shared by the client and server.
 *
 * A frame is always one line.  Payloads use a small escaping scheme so that
 * generated model text can contain newlines without changing frame boundaries.
 */

#include <stddef.h>
#include <sys/types.h>

#define PROTOCOL_DEFAULT_PORT 9090
#define PROTOCOL_MAX_LINE 8192
#define PROTOCOL_MAX_INPUT 4096

/* Send all bytes in a buffer, retrying when send(2) is interrupted. */
int protocol_send_all(int fd, const void *buffer, size_t length);

/*
 * Read one newline-terminated line.
 *
 * Return values:
 *   1  a line was read (the newline is not included)
 *   0  orderly EOF before any bytes were read
 *  -1  read error
 *  -2  line exceeded capacity (the rest of that line is discarded)
 */
int protocol_read_line(int fd, char *buffer, size_t capacity);

/* Escape/unescape frame payloads.  Return 0 on success and -1 on failure. */
int protocol_escape(const char *input, char *output, size_t capacity);
int protocol_unescape(const char *input, char *output, size_t capacity);

/* Generic escaped frame and chunked text for session management/history. */
int protocol_send_frame(int fd, const char *kind, const char *payload);
int protocol_send_text(int fd, const char *kind, const char *text);

int protocol_send_query(int fd, const char *text);
int protocol_send_quit(int fd);
int protocol_send_begin(int fd);
int protocol_send_data(int fd, const char *text);
int protocol_send_end(int fd);
int protocol_send_error(int fd, const char *text);

#endif /* PRACTICE_PROTOCOL_H */
