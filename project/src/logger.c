#include "logger.h"

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

int logger_open(logger_t *logger, const char *directory, unsigned long session_id) {
    if (logger == NULL || directory == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(logger, 0, sizeof(*logger));
    logger->fd = -1;
    logger->session_id = session_id;
    int path_length = snprintf(logger->path, sizeof(logger->path),
                               "%s/session_%lu.log", directory, session_id);
    if (path_length < 0 || (size_t)path_length >= sizeof(logger->path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    logger->fd = open(logger->path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logger->fd < 0) {
        return -1;
    }
    return 0;
}

static int logger_write_message(logger_t *logger, const char *role, const char *text) {
    if (logger == NULL || logger->fd < 0 || role == NULL || text == NULL) {
        errno = EINVAL;
        return -1;
    }

    time_t now = time(NULL);
    struct tm local_time;
    if (localtime_r(&now, &local_time) == NULL) {
        return -1;
    }

    char timestamp[64];
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    char header[128];
    int header_length = snprintf(header, sizeof(header), "[%s]\n\n%s:\n",
                                 timestamp, role);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        write_all(logger->fd, header, (size_t)header_length) < 0 ||
        write_all(logger->fd, text, strlen(text)) < 0 ||
        write_all(logger->fd, "\n\n", 2) < 0) {
        return -1;
    }
    return 0;
}

int logger_write_user(logger_t *logger, const char *text) {
    return logger_write_message(logger, "USER", text);
}

int logger_write_assistant(logger_t *logger, const char *text) {
    return logger_write_message(logger, "ASSISTANT", text);
}

void logger_close(logger_t *logger) {
    if (logger == NULL || logger->fd < 0) {
        return;
    }
    close(logger->fd);
    logger->fd = -1;
}
