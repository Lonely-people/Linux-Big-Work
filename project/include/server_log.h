/*
 * server_log.h — 服务活动日志模块对外接口
 *
 * 服务端主进程与各客户端子进程共用一份追加型日志文件（<日志目录>/server.log），
 * 记录服务的启动、连接、fork、退出、错误等关键事件，供排查问题使用。
 */
#ifndef PRACTICE_SERVER_LOG_H
#define PRACTICE_SERVER_LOG_H

#include <stddef.h>

/* server.log 路径缓冲区大小（含结尾的 '\0'）。 */
#define SERVER_LOG_PATH_MAX 4096

typedef struct {
    int fd;      /* server.log 的文件描述符；-1 表示文件日志不可用（仅控制台输出）。 */
    int console; /* 非 0 时把每一行日志同时输出到 stdout。 */
    char path[SERVER_LOG_PATH_MAX]; /* 拼接后的 server.log 完整路径。 */
} server_log_t;

/*
 * 打开（不存在则创建）日志 <directory>/server.log。
 * directory: 日志文件所在目录。
 * console:   非 0 时写入的每一行会同时镜像输出到 stdout。
 * 成功返回 0；失败返回 -1 并设置 errno（参数为空为 EINVAL，路径过长 ENAMETOOLONG）。
 */
int server_log_open(server_log_t *log, const char *directory, int console);

/*
 * 追加一行日志，格式为 "[YYYY-MM-DD HH:MM:SS] text"。
 * console 非 0 时同一行也会写到 stdout。成功返回 0；失败返回 -1。
 * text 过长时会被截断，但仍然保证以换行符结尾。
 */
int server_log_write(server_log_t *log, const char *text);

/* 关闭日志文件并把 fd 复位为 -1 */
void server_log_close(server_log_t *log);

#endif /* PRACTICE_SERVER_LOG_H */
