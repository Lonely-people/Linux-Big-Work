/*
 * session_store.h — 会话持久化存储模块对外接口
 *
 * 每个聊天会话在存储目录（服务端为 <project>/sessions）下对应一组文件：
 *   session_<id>.dat   会话内容快照，用“临时文件 + rename”原子替换写入；
 *   session_<id>.lock  会话锁：store_open/store_list 以非阻塞方式尝试持有，
 *                      持有失败即说明该会话正被其他进程（客户端子进程）占用（busy）；
 *   .lock              存储级锁：串行化“分配新编号并创建会话”和旧日志导入。
 * 旧版日志（logger.c 生成的 session_<id>.log）由 store_init 导入，导入去重
 * 依据 source 字段（记录来源日志的文件名）。
 * 除特别说明外，函数成功返回 0，失败返回 -1 并设置 errno。
 */
#ifndef PRACTICE_SESSION_STORE_H
#define PRACTICE_SESSION_STORE_H
#include "session.h"
#include <time.h>

/* 拼接路径用的缓冲区大小（含结尾 '\0'）。 */
#define STORE_PATH_MAX 4096

/* 会话内的一条消息。 */
typedef struct {
    char role;    /* 角色：'U' 用户提问、'A' 助手回答、'X' 无法解析的旧日志文本。 */
    int context;  /* 是否进入模型上下文：只有完整成功的问答轮次为 1。 */
    char *text;   /* 消息正文（堆分配，随会话一起释放）。 */
} stored_message_t;

/* 一个已载入内存的会话及其落盘信息。 */
typedef struct {
    unsigned long id;               /* 会话编号（目录内唯一，从 1 开始）。 */
    time_t updated;                 /* 最后更新时间；导入会话取原日志 mtime。 */
    /* title：标题，默认 "New conversation"，首问后截取生成，导入会话为 "[Imported] <日志名>"；
     * source：来源日志文件名，仅导入会话非空（用于导入去重）。 */
    char *title, *source;
    stored_message_t *messages;     /* 消息数组。 */
    size_t count;                   /* 消息条数。 */
    int lock_fd;                    /* 持有的会话锁 fd；-1 表示未持有锁。 */
    char directory[STORE_PATH_MAX]; /* 会话文件所在目录。 */
} stored_session_t;

/* store_list 的回调：每发现一个会话调用一次；返回非 0 将中止遍历。 */
typedef int (*store_list_callback)(unsigned long id, time_t updated,
                                  int busy, const char *title, void *userdata);

/*
 * 初始化会话存储：创建（已存在则保留）directory 及其 logs 子目录，
 * 并导入 legacy_directory 中形如 session_<id>.log 的旧版日志，已导入过的会跳过。
 * legacy_directory 不存在时按成功处理；失败返回 -1 并设置 errno。
 */
int store_init(const char *directory, const char *legacy_directory);

/*
 * 遍历存储目录中的所有会话，逐个调用 callback（参数含义见回调声明）。
 * 读不了或损坏的 .dat 也会上报，以便界面展示与清理：
 * 此时以 updated=0、busy=1、title="[Unreadable session]" 调用回调。
 * 可读会话的 busy 用非阻塞会话锁探测（被其他进程占用则为 1）。
 * 回调返回非 0 时停止遍历：可读会话以该返回值返回，不可读会话返回 -1。
 */
int store_list(const char *directory, store_list_callback callback, void *userdata);

/*
 * 新建空会话并立即落盘。整个过程由 <directory>/.lock 全局锁保护：
 * 扫描现有最大编号并分配 id=最大编号+1，避免并发创建撞号。
 * 成功后 out 持有该会话的锁（lock_fd），直到 store_close 才释放。
 */
int store_create(const char *directory, stored_session_t *out);

/*
 * 打开编号为 id 的已有会话：先非阻塞获取其会话锁——失败说明正被占用
 * （errno 通常为 EWOULDBLOCK），随后读取并解析 session_<id>.dat 到 out。
 * 成功后 out 持有会话锁（lock_fd），直到 store_close 才释放。
 */
int store_open(const char *directory, unsigned long id, stored_session_t *out);

/* 释放会话资源（锁 fd、消息、标题、来源）并复位为未持有状态；
 * 传入 NULL 安全，可重复调用。 */
void store_close(stored_session_t *session);

/*
 * 由存储的消息重建模型上下文：先在 context 中执行 session_init，
 * 再把“用户提问紧跟助手回答、且二者 context 均为 1”的完整成功轮次依次写入；
 * 失败轮与旧日志文本（X 段）都不会进入上下文。失败返回 -1。
 */
int store_context(const stored_session_t *stored, session_t *context);

/*
 * 提交一轮问答：追加 U/A 两条消息（context=1）；若是全新会话，用首问第一行
 * 生成标题（最多 24 个 UTF-8 字符）；更新时间并原子落盘。
 * 失败时回滚内存中的新增内容，使内存状态与磁盘快照保持一致，返回 -1。
 */
int store_commit_turn(stored_session_t *stored, const char *question, const char *answer);
#endif /* PRACTICE_SESSION_STORE_H */
