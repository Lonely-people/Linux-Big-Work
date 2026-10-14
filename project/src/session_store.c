/*
 * session_store.c — 会话持久化存储模块实现
 *
 * .dat 快照文件格式（CHAT1）：
 *   CHAT1 <id> <updated> <count>\n
 *   <标题长度>\n<标题>\n
 *   <来源长度>\n<来源>\n
 *   之后重复 count 次：<角色> <context>\n<正文长度>\n<正文>\n
 * 字段一律“先十进制长度、后原文”，即使正文里出现记录头样式的文本也不会产生二义。
 * 保存采用“临时文件 + fsync + rename”的原子替换（见 save_session）：任何时刻磁盘上
 * 要么是旧快照、要么是新快照，不会出现写了一半的文件。
 * 旧版日志（logger.c 产出、形如 session_<id>.log）由 store_init 导入，
 * 按“时间戳头 + USER:/ASSISTANT:”切分成消息（见 legacy_header/import_file）。
 */
#include "session_store.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* 把“<directory>/session_<id>.<suffix>”拼进 out；放不下时返回 -1（ENAMETOOLONG）。 */
static int path_for(char *out, const char *directory, unsigned long id, const char *suffix) {
    int n = snprintf(out, STORE_PATH_MAX, "%s/session_%lu.%s", directory, id, suffix);
    if (n < 0 || n >= STORE_PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

/* 打开（不存在则创建）锁文件并加排他锁，成功返回持有锁的 fd。
 * nonblock 非 0 时用 LOCK_NB：锁被占用会立即失败，用于探测会话是否 busy；
 * flock 锁的生命周期跟随 fd，close(fd) 即释放；O_CLOEXEC 防止 fd 泄漏给 exec 的子进程。 */
static int lock_path(const char *path, int nonblock) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | (nonblock ? LOCK_NB : 0)) < 0) {
        int error = errno; /* 先保存 flock 失败的 errno，close 可能覆盖它 */
        close(fd); errno = error; return -1;
    }
    return fd;
}

/* 释放会话资源（锁 fd、消息文本、消息数组、标题、来源）并复位结构；可重复调用。 */
void store_close(stored_session_t *s) {
    if (!s) return;
    if (s->lock_fd >= 0) close(s->lock_fd); /* 关闭 fd 的同时释放 flock 锁 */
    for (size_t i = 0; i < s->count; ++i) free(s->messages[i].text);
    free(s->messages); free(s->title); free(s->source);
    memset(s, 0, sizeof(*s)); s->lock_fd = -1; /* lock_fd = -1 表示未持有锁 */
}

/* 在会话尾部追加一条消息：复制文本，并把消息数组扩容一格。 */
static int add_message(stored_session_t *s, char role, int context, const char *text) {
    char *copy = strdup(text);
    if (!copy) return -1;
    stored_message_t *messages = realloc(s->messages, (s->count + 1) * sizeof(*messages));
    if (!messages) { free(copy); return -1; }
    s->messages = messages;
    s->messages[s->count++] = (stored_message_t){role, context, copy};
    return 0;
}

/* 写出一个长度前缀字段：“<长度>\n<原文>\n”。
 * 正文里即使出现记录头或换行，也能靠长度前缀无歧义地还原。 */
static int write_field(FILE *file, const char *text) {
    size_t length = strlen(text);
    return fprintf(file, "%zu\n", length) < 0 ||
           fwrite(text, 1, length, file) != length || fputc('\n', file) == EOF ? -1 : 0;
}

/* 原子保存整个会话：写临时文件 session_<id>.tmp.XXXXXX → fsync → rename 覆盖正式文件。
 * 任何时刻磁盘上要么是完整旧快照、要么是完整新快照；失败时删除临时文件，旧数据不受影响。 */
static int save_session(stored_session_t *s) {
    char target[STORE_PATH_MAX], temporary[STORE_PATH_MAX];
    if (path_for(target, s->directory, s->id, "dat") < 0 ||
        path_for(temporary, s->directory, s->id, "tmp.XXXXXX") < 0) return -1;
    int fd = mkstemp(temporary); /* 以 0600 权限创建唯一命名的临时文件 */
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "wb"); /* 此后 fd 由 FILE* 托管，fclose 会关闭它 */
    if (!file) { int error = errno; close(fd); unlink(temporary); errno = error; return -1; }
    int failed = fprintf(file, "CHAT1 %lu %lld %zu\n", s->id, (long long)s->updated, s->count) < 0;
    if (!failed) failed = write_field(file, s->title) < 0 || write_field(file, s->source) < 0;
    for (size_t i = 0; i < s->count && !failed; ++i) {
        stored_message_t *m = &s->messages[i];
        failed = fprintf(file, "%c %d\n", m->role, m->context) < 0 || write_field(file, m->text) < 0;
    }
    if (!failed) failed = fflush(file) != 0 || fsync(fd) < 0; /* 先把数据落盘，再改名 */
    int error = errno; /* 后续 close/rename 可能覆盖 errno，先留住首个错误码 */
    if (fclose(file) != 0) { failed = 1; error = errno; }
    if (!failed && rename(temporary, target) < 0) { failed = 1; error = errno; }
    if (failed) { unlink(temporary); errno = error ? error : EIO; return -1; }
    /* rename 成功即以新快照为准：即使随后目录 fsync 报“持久化警告”，也不回滚内存状态。 */
    int directory_fd = open(s->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd >= 0) {
        if (fsync(directory_fd) < 0) perror("server: sync session directory");
        close(directory_fd);
    }
    return 0;
}

/* 读取一个长度前缀字段。头行必须恰好是一个十进制数（不允许多余字符）；
 * 长度不得超过文件总大小，并挡住 (size_t)-1 防止 malloc(length+1) 溢出；
 * 正文后必须紧跟 '\n'，且不允许内嵌 NUL（否则无法作为 C 字符串使用）。 */
static char *read_field(FILE *file, size_t file_size) {
    char header[64], extra;
    size_t length;
    if (!fgets(header, sizeof(header), file) || sscanf(header, "%zu %c", &length, &extra) != 1 ||
        length > file_size || length == (size_t)-1) { errno = EPROTO; return NULL; }
    char *text = malloc(length + 1);
    if (!text) return NULL;
    if (fread(text, 1, length, file) != length || fgetc(file) != '\n' || memchr(text, 0, length)) {
        free(text); errno = EPROTO; return NULL;
    }
    text[length] = 0;
    return text;
}

/* 从 session_<id>.dat 读取并解析会话到 s（不涉锁，锁由调用方负责）。
 * 对文件内容做严格校验：头部字段数、id 与文件名一致、计数合理、角色/context 合法、
 * 末尾无多余数据；任何不符都以 EPROTO 拒绝，避免把损坏文件读进内存。 */
static int load_session(const char *directory, unsigned long id, stored_session_t *s) {
    memset(s, 0, sizeof(*s)); s->lock_fd = -1;
    char path[STORE_PATH_MAX];
    if (path_for(path, directory, id, "dat") < 0) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    struct stat st;
    char header[128], extra;
    unsigned long stored_id;
    long long updated;
    size_t count;
    int result = -1;
    /* 末位 %c 用于捕获多余字符：必须恰好解析出 3 个字段；
     * count 粗校验（每条消息至少占 7 字节），先挡住损坏/恶意的大计数。 */
    if (fstat(fileno(file), &st) < 0 || st.st_size < 0 ||
        !fgets(header, sizeof(header), file) ||
        sscanf(header, "CHAT1 %lu %lld %zu %c", &stored_id, &updated, &count, &extra) != 3 ||
        stored_id != id || count > (size_t)st.st_size / 6) { errno = EPROTO; goto done; }
    s->id = id; s->updated = (time_t)updated;
    if (snprintf(s->directory, sizeof(s->directory), "%s", directory) >= (int)sizeof(s->directory)) {
        errno = ENAMETOOLONG; goto done;
    }
    s->title = read_field(file, (size_t)st.st_size);
    s->source = read_field(file, (size_t)st.st_size);
    if (!s->title || !s->source) goto done;
    for (size_t i = 0; i < count; ++i) {
        char role; int context;
        /* 角色必须是 U/A/X 之一，context 只允许 0 或 1 */
        if (!fgets(header, sizeof(header), file) || sscanf(header, "%c %d %c", &role, &context, &extra) != 2 ||
            !strchr("UAX", role) || (context != 0 && context != 1)) { errno = EPROTO; goto done; }
        char *text = read_field(file, (size_t)st.st_size);
        if (!text) goto done;
        int added = add_message(s, role, context, text);
        free(text); /* add_message 内部已复制一份，这里释放临时缓冲 */
        if (added < 0) goto done;
    }
    if (fgetc(file) != EOF || ferror(file)) { errno = EPROTO; goto done; } /* 读完后必须恰好是文件末尾 */
    result = 0;
 done: {
        int error = errno; /* fclose 可能覆盖 errno，先保存 */
        fclose(file);
        if (result < 0) store_close(s); /* 失败时释放已解析到一半的内容 */
        errno = error;
    }
    return result;
}

/* 打开既有会话（对外接口）：先非阻塞占锁（失败通常是被占用，errno=EWOULDBLOCK），再读 .dat。 */
int store_open(const char *directory, unsigned long id, stored_session_t *out) {
    char path[STORE_PATH_MAX];
    if (id == 0 || path_for(path, directory, id, "lock") < 0) { errno = EINVAL; return -1; }
    int fd = lock_path(path, 1);
    if (fd < 0) return -1;
    if (load_session(directory, id, out) < 0) { int error = errno; close(fd); errno = error; return -1; }
    out->lock_fd = fd; /* 锁随会话一起持有，直到 store_close 才释放 */
    return 0;
}

/* 严格判断文件名是否为规范形式“session_<id>.<suffix>”，是则返回 id，否则返回 0。
 * 除后缀完全匹配外，还要求把 id 重新格式化后的名字与原串逐字符相同，
 * 从而拒绝“session_007.dat”等非规范写法（id 从 1 起，0 可作无效哨兵）。 */
static unsigned long filename_id(const char *name, const char *suffix) {
    unsigned long id = 0; char extension[16], extra;
    if (sscanf(name, "session_%lu.%15s%c", &id, extension, &extra) != 2 ||
        strcmp(extension, suffix) != 0) return 0;
    char canonical[80];
    snprintf(canonical, sizeof(canonical), "session_%lu.%s", id, suffix);
    return strcmp(name, canonical) == 0 ? id : 0;
}

/* 在已持有存储级锁的前提下创建会话：扫描现有 .dat 的最大编号，分配 highest+1。
 * publish 非 0 时立即把空会话落盘；导入流程传 0（先填内容，稍后再统一保存）。 */
static int create_locked(const char *directory, stored_session_t *s, int publish) {
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    unsigned long highest = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        unsigned long id = filename_id(entry->d_name, "dat");
        if (id > highest) highest = id;
    }
    closedir(dir);
    if (highest == ULONG_MAX) { errno = EOVERFLOW; return -1; } /* 编号耗尽 */
    memset(s, 0, sizeof(*s)); s->lock_fd = -1;
    s->id = highest + 1; s->updated = time(NULL);
    if (snprintf(s->directory, sizeof(s->directory), "%s", directory) >= (int)sizeof(s->directory)) {
        errno = ENAMETOOLONG; return -1;
    }
    s->title = strdup("New conversation"); s->source = strdup(""); /* 新建会话：默认标题、空来源 */
    char path[STORE_PATH_MAX];
    if (!s->title || !s->source || path_for(path, directory, s->id, "lock") < 0) goto fail;
    s->lock_fd = lock_path(path, 1); /* 立即占住新会话的锁，创建者在用期间对其他进程显示 busy */
    if (s->lock_fd < 0 || (publish && save_session(s) < 0)) goto fail;
    return 0;
 fail: {
        int error = errno; store_close(s); errno = error; return -1;
    }
}

/* 新建会话（对外接口）：用 <directory>/.lock 全局锁把“扫描编号 + 分配 + 创建”串行化，
 * 避免多个进程同时创建时分配到相同 id。 */
int store_create(const char *directory, stored_session_t *out) {
    char path[STORE_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.lock", directory) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG; return -1;
    }
    int lock = lock_path(path, 0); /* 阻塞等待，保证互斥 */
    if (lock < 0) return -1;
    int result = create_locked(directory, out, 1), error = errno; /* 先捕获 errno 再解锁 */
    close(lock); errno = error; return result;
}

/* 列出全部会话（对外接口）：不可读的 .dat 也回调（占位标题），
 * 可读会话以非阻塞锁探测 busy；回调返回非 0 即停止遍历。 */
int store_list(const char *directory, store_list_callback callback, void *userdata) {
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        unsigned long id = filename_id(entry->d_name, "dat");
        if (!id) continue; /* 忽略非规范命名的文件 */
        stored_session_t s;
        if (load_session(directory, id, &s) < 0) {
            /* 损坏/读不了的会话也上报（updated=0、busy=1、占位标题），便于前端展示与清理 */
            if (callback(id, 0, 1, "[Unreadable session]", userdata) != 0) { result = -1; break; }
            continue;
        }
        char path[STORE_PATH_MAX];
        if (path_for(path, directory, id, "lock") < 0) { store_close(&s); result = -1; break; }
        int fd = lock_path(path, 1); /* 非阻塞试锁：拿得到说明空闲，拿不到说明正被占用 */
        int busy = fd < 0;
        if (fd >= 0) close(fd); /* 仅探测用途，拿到后立刻释放 */
        result = callback(id, s.updated, busy, s.title, userdata);
        store_close(&s);
        if (result != 0) break;
    }
    closedir(dir);
    return result;
}

/* 由存储消息重建模型上下文（对外接口）。 */
int store_context(const stored_session_t *stored, session_t *context) {
    if (session_init(context) < 0) return -1;
    for (size_t i = 0; i + 1 < stored->count; ++i) {
        const stored_message_t *u = &stored->messages[i], *a = &stored->messages[i + 1];
        /* 只收“U 紧跟 A 且二者 context=1”的完整成功轮次；失败轮、X 段一律跳过 */
        if (u->role == 'U' && a->role == 'A' && u->context && a->context) {
            if (session_add_user(context, u->text) < 0 || session_add_assistant(context, a->text) < 0) {
                session_destroy(context); return -1;
            }
            ++i; /* 已与配对中的 A 一起消费，跳过它 */
        }
    }
    return 0;
}

/* 提交一轮问答（对外接口）：追加 U/A 消息、生成标题、更新时间并落盘；失败回滚内存状态。 */
int store_commit_turn(stored_session_t *s, const char *question, const char *answer) {
    size_t old_count = s->count; /* 记下旧状态，落盘失败时用它回滚 */
    time_t old_updated = s->updated;
    char *old_title = s->title;
    if (add_message(s, 'U', 1, question) < 0 || add_message(s, 'A', 1, answer) < 0) goto fail;
    if (!*s->source && old_count == 0) {
        /* 只有全新会话（非导入、且还没有历史消息）才用首问生成标题；
         * 取第一行、最多 24 个 UTF-8 字符（跳过 0x80-0xBF 续字节，避免截断多字节字符）。 */
        size_t bytes = 0, characters = 0;
        while (question[bytes] && question[bytes] != '\n' && question[bytes] != '\r') {
            if (((unsigned char)question[bytes] & 0xc0) != 0x80 && ++characters > 24) break;
            ++bytes;
        }
        s->title = bytes ? strndup(question, bytes) : strdup("New conversation");
        if (!s->title) { s->title = old_title; goto fail; }
    }
    s->updated = time(NULL);
    if (save_session(s) < 0) goto fail;
    if (old_title != s->title) free(old_title);
    return 0;
 fail: {
        int error = errno;
        /* 回滚：丢弃新追加的消息文本，恢复标题与更新时间，使内存状态与磁盘快照一致 */
        while (s->count > old_count) free(s->messages[--s->count].text);
        if (s->title != old_title) free(s->title);
        s->title = old_title; s->updated = old_updated;
        errno = error; return -1;
    }
}

/* 旧版日志没有转义机制：只有完整、精确匹配的头块才用来切分消息；
 * 匹配不上的文本保留为 X 记录，而不是丢弃。 */
static int legacy_header(const char *at, const char **body, char *role) {
    /* 先校验固定形态“[YYYY-MM-DD HH:MM:SS]\n”：数字位必须是数字，分隔位必须是对应的 - / 空格 / : */
    if (strlen(at) < 24 || at[0] != '[' || at[20] != ']' || at[21] != '\n') return 0;
    for (int i = 1; i < 20; ++i) {
        char expected = (i == 5 || i == 8) ? '-' : i == 11 ? ' ' : (i == 14 || i == 17) ? ':' : 0;
        if (expected ? at[i] != expected : at[i] < '0' || at[i] > '9') return 0;
    }
    const char *p = at + 22;
    while (*p == '\n') ++p; /* 跳过时间戳后的空行 */
    if (strncmp(p, "USER:\n", 6) == 0) { *role = 'U'; *body = p + 6; return 1; }
    if (strncmp(p, "ASSISTANT:\n", 11) == 0) { *role = 'A'; *body = p + 11; return 1; }
    return 0;
}

/* 把一份旧版日志整体导入会话：按头块切段，每段作为一条消息（角色取段首标识；
 * 第一个头块之前若有内容，则记成 X 段保留）。导入完成后，把 U 紧邻 A 的消息对
 * 标记为完整轮次（context=1）。 */
static int import_file(stored_session_t *s, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    struct stat st;
    if (fstat(fileno(file), &st) < 0 || st.st_size < 0) { fclose(file); return -1; }
    size_t length = (size_t)st.st_size;
    char *text = malloc(length + 1);
    if (!text) { fclose(file); return -1; }
    if (fread(text, 1, length, file) != length || memchr(text, 0, length)) {
        free(text); fclose(file); errno = EPROTO; return -1; /* 含 NUL 的文件无法按文本切分 */
    }
    fclose(file); text[length] = 0;
    s->updated = st.st_mtime; /* 导入会话沿用原日志的修改时间 */
    const char *start = text; /* 当前消息段的起点 */
    char role = 'X';          /* 当前段的角色；首个头块之前的内容按 X 保留 */
    int result = 0;
    for (char *p = text; *p; ++p) {
        const char *body; char next_role;
        if ((p == text || p[-1] == '\n') && legacy_header(p, &body, &next_role)) {
            /* 只在行首识别头块；把 [start, p) 之间的文本作为上一段消息保存 */
            if (p > start || role != 'X') {
                size_t size = (size_t)(p - start);
                /* logger_write_message 在每条消息末尾恰好多写两个换行，这里剥掉 */
                if (size >= 2 && start[size - 1] == '\n' && start[size - 2] == '\n') size -= 2;
                char *part = strndup(start, size);
                if (!part || add_message(s, role, 0, part) < 0) { free(part); result = -1; break; }
                free(part);
            }
            start = body; role = next_role;
            p = (char *)body - 1; /* 从新段正文继续扫描（循环头的 ++p 会补回 1） */
        }
    }
    if (!result && (start < text + length || role != 'X')) {
        /* 收尾：最后一段没有后续头块，单独处理 */
        size_t size = (size_t)(text + length - start);
        if (size >= 2 && start[size - 1] == '\n' && start[size - 2] == '\n') size -= 2;
        char *part = strndup(start, size);
        if (!part || add_message(s, role, 0, part) < 0) result = -1;
        free(part);
    }
    /* 事后配对：U 紧邻 A 的完整问答标记为可进入模型上下文 */
    for (size_t i = 0; i + 1 < s->count; ++i) {
        if (s->messages[i].role == 'U' && s->messages[i + 1].role == 'A') {
            s->messages[i].context = s->messages[i + 1].context = 1;
            ++i;
        }
    }
    free(text); return result;
}

/* 检查旧日志是否已导入过：遍历存储目录，比对 source 字段（来源日志文件名）。
 * 返回 1 已导入、0 未导入、-1 存储不可读。 */
static int already_imported(const char *directory, const char *source) {
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        unsigned long id = filename_id(entry->d_name, "dat");
        if (!id) continue;
        stored_session_t s;
        if (load_session(directory, id, &s) < 0) { found = -1; break; }
        found = strcmp(s.source, source) == 0;
        store_close(&s);
        if (found) break;
    }
    closedir(dir); return found;
}

/* 初始化存储（对外接口）：建目录、加全局锁，然后导入 legacy_directory 中
 * 尚未导入的 session_<id>.log。导入是“尽力而为”：存储不可读时警告并停止，
 * 但不会让服务器启动失败。 */
int store_init(const char *directory, const char *legacy_directory) {
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    char path[STORE_PATH_MAX];
    /* 每会话活动日志目录（<存储目录>/logs），供 logger 使用 */
    if (snprintf(path, sizeof(path), "%s/logs", directory) >= (int)sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    if (snprintf(path, sizeof(path), "%s/.lock", directory) >= (int)sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    int lock = lock_path(path, 0); /* 与 store_create 互斥：导入与新建不会同时分配编号 */
    if (lock < 0) return -1;
    DIR *dir = opendir(legacy_directory);
    if (!dir) { int error = errno; close(lock); return error == ENOENT ? 0 : -1; } /* 无旧日志目录视为无需导入 */
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!filename_id(entry->d_name, "log")) continue; /* 只认规范的 session_<id>.log */
        int known = already_imported(directory, entry->d_name);
        if (known < 0) { fprintf(stderr, "server: unreadable store; skipping automatic legacy import\n"); break; }
        if (known) continue;
        stored_session_t s;
        if (create_locked(directory, &s, 0) < 0) { result = -1; break; } /* publish=0：导入完成后才落盘 */
        free(s.source); free(s.title);
        s.source = strdup(entry->d_name); /* 去重依据：来源日志文件名 */
        char title[280]; snprintf(title, sizeof(title), "[Imported] %s", entry->d_name);
        s.title = strdup(title);
        if (snprintf(path, sizeof(path), "%s/%s", legacy_directory, entry->d_name) >= (int)sizeof(path) ||
            !s.source || !s.title || import_file(&s, path) < 0 || save_session(&s) < 0) {
            char target[STORE_PATH_MAX];
            if (path_for(target, directory, s.id, "dat") == 0) unlink(target); /* 清理可能已写出的快照 */
            store_close(&s); result = -1; break;
        }
        store_close(&s);
    }
    closedir(dir); close(lock); return result;
}
