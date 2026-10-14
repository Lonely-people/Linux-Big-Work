/*
 * server_log.c — 服务活动日志模块实现
 *
 * 服务端主进程 fork 出的每个子进程都与主进程共用同一份 server.log。
 */
#include "server_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 把 length 个字节完整写入 fd。
 * write 可能被信号打断（EINTR）或只写入一部分，使用循环写入直到写完；
 * 全部写完返回 0，出错返回 -1。 */
static int write_all(int fd, const char *buffer, size_t length) {
    size_t written = 0;

    while (written < length) {
        ssize_t result = write(fd, buffer + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue; /* 被信号打断：重试本次写入 */
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

/* 打开 <directory>/server.log */
int server_log_open(server_log_t *log, const char *directory, int console) {
    if (log == NULL || directory == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(log, 0, sizeof(*log)); /* 整体清零，保证各字段有确定的初始值 */
    log->fd = -1;
    log->console = console;

    /* 拼接完整日志路径 "<directory>/server.log"；返回值大于等于缓冲区大小说明被截断 */
    int path_length = snprintf(log->path, sizeof(log->path),
                               "%s/server.log", directory);
    if (path_length < 0 || (size_t)path_length >= sizeof(log->path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /* O_APPEND ：每次写入都追加到文件末尾，
     * 多个子进程共用同一文件时，整行不会互相覆盖或穿插。 */
    log->fd = open(log->path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log->fd < 0) {
        return -1;
    }
    return 0;
}

/* 追加一行带时间戳的日志 */
int server_log_write(server_log_t *log, const char *text) {
    if (log == NULL || text == NULL) {
        errno = EINVAL;
        return -1;
    }

    time_t now = time(NULL);
    struct tm local_time;
    if (localtime_r(&now, &local_time) == NULL) { /* 可重入版本，避免共享静态缓冲区 */
        return -1;
    }

    char timestamp[64];
    /* 格式化为 "YYYY-MM-DD HH:MM:SS"；返回 0 表示结果放不下 */
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                 &local_time) == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    /* 一行一个缓冲区、一次 write 写出：配合 O_APPEND，并发子进程追加整行日志时
     * 不会出现部分写入互相穿插的情况。 */
    char line[512];
    int length = snprintf(line, sizeof(line), "[%s] %s\n", timestamp, text);
    if (length < 0) {
        return -1;
    }
    if ((size_t)length >= sizeof(line)) {
        /* 内容过长被截断：砍掉多余部分，但保证末尾仍是换行符 */
        length = (int)sizeof(line) - 1;
        line[length] = '\0';
        line[length - 1] = '\n';
    }

    if (log->console && write_all(STDOUT_FILENO, line, (size_t)length) < 0) {
        /* console ：同时输出到标准输出 */
        return -1;
    }
    if (log->fd >= 0 && write_all(log->fd, line, (size_t)length) < 0) {
        /* fd < 0 表示文件日志不可用 */
        return -1;
    }
    return 0;
}

/* 关闭日志文件并把 fd 复位为 -1 */
void server_log_close(server_log_t *log) {
    if (log == NULL || log->fd < 0) {
        return; /* 空指针或已关闭 */
    }
    close(log->fd);
    log->fd = -1;
}
