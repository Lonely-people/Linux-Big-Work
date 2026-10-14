/*
 * logger.h — 单个客户端会话聊天记录日志模块对外接口
 *
 * 每个会话对应一个独立的日志文件 session_<会话号>.log（位于调用方指定的目录），
 * 用于把该会话中 USER 的消息与 ASSISTANT 的回复按发生顺序存储。
 * 文件以 O_APPEND 打开，重新打开同一会话时不会覆盖或截断历史内容。
 *
 * 每条记录形如：
 *
 *   [YYYY-MM-DD HH:MM:SS]
 *
 *   USER:
 *   <消息正文>
 *
 */
#ifndef PRACTICE_LOGGER_H
#define PRACTICE_LOGGER_H

#include <stddef.h>

/* session 日志文件路径缓冲区大小（含结尾的 '\0'）。 */
#define LOGGER_PATH_MAX 4096

typedef struct {
    int fd;                     /* 日志文件描述符；-1 表示尚未打开或已关闭。 */
    unsigned long session_id;   /* 该日志所属的会话号，用于文件名 session_<会话号>.log。 */
    char path[LOGGER_PATH_MAX]; /* 拼接后的日志文件完整路径。 */
} logger_t;

/*
 * 打开（不存在则创建）某会话的日志文件 <directory>/session_<session_id>.log。
 * directory:  日志文件所在目录；session_id: 会话号，决定文件名。
 * 以 O_APPEND 追加方式打开，同一会话的历史记录不会被覆盖或截断。
 * 成功返回 0；失败返回 -1 并设置 errno（参数为空为 EINVAL，路径过长 ENAMETOOLONG）。
 */
int logger_open(logger_t *logger, const char *directory, unsigned long session_id);

/*
 * 追加一条带时间戳的对话记录：logger_write_user 记录用户消息，
 * logger_write_assistant 记录模型回复，正文原样写入。
 * 记录格式为 "[YYYY-MM-DD HH:MM:SS]\n\n角色:\n正文\n\n"。
 * 成功返回 0；失败返回 -1。
 */
int logger_write_user(logger_t *logger, const char *text);
int logger_write_assistant(logger_t *logger, const char *text);

/* 关闭日志文件并把 fd 复位为 -1。 */
void logger_close(logger_t *logger);

#endif /* PRACTICE_LOGGER_H */
