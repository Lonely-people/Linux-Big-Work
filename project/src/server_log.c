#include "server_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int write_all(int fd, const char *buffer, size_t length) {
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(fd, buffer + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = EIO;
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

int server_log_open(server_log_t *log, const char *directory, int console) {
    if (log == NULL || directory == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(log, 0, sizeof(*log));
    log->fd = -1;
    log->console = console;
    int path_length = snprintf(log->path, sizeof(log->path),
                               "%s/server.log", directory);
    if (path_length < 0 || (size_t)path_length >= sizeof(log->path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    log->fd = open(log->path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log->fd < 0) {
        return -1;
    }
    return 0;
}

int server_log_write(server_log_t *log, const char *text) {
    if (log == NULL || text == NULL) {
        errno = EINVAL;
        return -1;
    }

    time_t now = time(NULL);
    struct tm local_time;
    if (localtime_r(&now, &local_time) == NULL) {
        return -1;
    }

    char timestamp[64];
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                 &local_time) == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    /* One line, one buffer, one write: O_APPEND makes concurrent children append
     * this line atomically instead of interleaving partial writes. */
    char line[512];
    int length = snprintf(line, sizeof(line), "[%s] %s\n", timestamp, text);
    if (length < 0) {
        return -1;
    }
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
        line[length] = '\0';
        line[length - 1] = '\n';
    }

    if (log->console && write_all(STDOUT_FILENO, line, (size_t)length) < 0) {
        return -1;
    }
    if (log->fd >= 0 && write_all(log->fd, line, (size_t)length) < 0) {
        return -1;
    }
    return 0;
}

void server_log_close(server_log_t *log) {
    if (log == NULL || log->fd < 0) {
        return;
    }
    close(log->fd);
    log->fd = -1;
}
