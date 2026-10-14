/*
 * logger.c — 单个客户端会话聊天记录日志模块实现
 *
 * 每个会话一个追加型日志文件（文件名 session_<会话号>.log）。
 * 每条记录由三部分依次写出（见 logger_write_message）：
 *   "[时间戳]\n\n"  +  "角色:\n"  +  "正文\n\n"，
 */
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 把 length 个字节完整写入 fd。
 * write(2) 可能被信号打断（EINTR）或只写入一部分，这里循环补齐；
 * 全部写完返回 0，出错返回 -1（写 0 字节视为 EIO，防止死循环）。 */
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

/* 打开 <directory>/session_<session_id>.log */
int logger_open(logger_t *logger, const char *directory, unsigned long session_id) {
    if (logger == NULL || directory == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(logger, 0, sizeof(*logger)); /* 整体清零，保证各字段有确定的初始值 */
    logger->fd = -1; /* 打开成功前先记为 -1：打开失败时保持"未打开"状态 */
    logger->session_id = session_id;
    /* 拼接完整日志路径 "<directory>/session_<会话号>.log"；
     * 返回值大于等于缓冲区大小说明路径被截断 */
    int path_length = snprintf(logger->path, sizeof(logger->path),
                               "%s/session_%lu.log", directory, session_id);
    if (path_length < 0 || (size_t)path_length >= sizeof(logger->path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /* O_APPEND ：每次写入都追加到文件末尾，
     * 重新打开同一会话时历史记录不会被覆盖；0644 为新建文件的权限 */
    logger->fd = open(logger->path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logger->fd < 0) {
        return -1;
    }
    return 0;
}

/* 追加一条记录：依次写出 "[时间戳]\n\n角色:\n" 头部、正文、结尾空行 */
static int logger_write_message(logger_t *logger, const char *role, const char *text) {
    if (logger == NULL || logger->fd < 0 || role == NULL || text == NULL) {
        errno = EINVAL; /* 未打开（fd < 0）或参数为空：拒绝写入 */
        return -1;
    }

    time_t now = time(NULL);
    struct tm local_time;
    if (localtime_r(&now, &local_time) == NULL) { /* 可重入版本，避免共享静态缓冲区 */
        return -1;
    }

    char timestamp[64];
    /* 格式化为 "YYYY-MM-DD HH:MM:SS"；返回 0 表示结果放不下 */
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    char header[128];
    /* 头部形如 "[时间戳]\n\n角色:\n" */
    int header_length = snprintf(header, sizeof(header), "[%s]\n\n%s:\n",
                                 timestamp, role);
    /* 头部、正文（不含结尾 '\0'）、结尾空行依次完整写出；任一步失败立即返回 -1 */
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        write_all(logger->fd, header, (size_t)header_length) < 0 ||
        write_all(logger->fd, text, strlen(text)) < 0 ||
        write_all(logger->fd, "\n\n", 2) < 0) {
        return -1;
    }
    return 0;
}

/* 记录一条用户消息（角色名固定为 "USER"） */
int logger_write_user(logger_t *logger, const char *text) {
    return logger_write_message(logger, "USER", text);
}

/* 记录一条模型回复（角色名固定为 "ASSISTANT"） */
int logger_write_assistant(logger_t *logger, const char *text) {
    return logger_write_message(logger, "ASSISTANT", text);
}

/* 关闭日志文件并把 fd 复位为 -1 */
void logger_close(logger_t *logger) {
    if (logger == NULL || logger->fd < 0) {
        return;
    }
    close(logger->fd);
    logger->fd = -1;
}
