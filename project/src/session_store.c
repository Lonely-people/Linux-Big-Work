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

static int path_for(char *out, const char *directory, unsigned long id, const char *suffix) {
    int n = snprintf(out, STORE_PATH_MAX, "%s/session_%lu.%s", directory, id, suffix);
    if (n < 0 || n >= STORE_PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int lock_path(const char *path, int nonblock) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | (nonblock ? LOCK_NB : 0)) < 0) {
        int error = errno;
        close(fd); errno = error; return -1;
    }
    return fd;
}

void store_close(stored_session_t *s) {
    if (!s) return;
    if (s->lock_fd >= 0) close(s->lock_fd);
    for (size_t i = 0; i < s->count; ++i) free(s->messages[i].text);
    free(s->messages); free(s->title); free(s->source);
    memset(s, 0, sizeof(*s)); s->lock_fd = -1;
}

static int add_message(stored_session_t *s, char role, int context, const char *text) {
    char *copy = strdup(text);
    if (!copy) return -1;
    stored_message_t *messages = realloc(s->messages, (s->count + 1) * sizeof(*messages));
    if (!messages) { free(copy); return -1; }
    s->messages = messages;
    s->messages[s->count++] = (stored_message_t){role, context, copy};
    return 0;
}

/* Length-prefixed fields are unambiguous even when text contains record headers. */
static int write_field(FILE *file, const char *text) {
    size_t length = strlen(text);
    return fprintf(file, "%zu\n", length) < 0 ||
           fwrite(text, 1, length, file) != length || fputc('\n', file) == EOF ? -1 : 0;
}

static int save_session(stored_session_t *s) {
    char target[STORE_PATH_MAX], temporary[STORE_PATH_MAX];
    if (path_for(target, s->directory, s->id, "dat") < 0 ||
        path_for(temporary, s->directory, s->id, "tmp.XXXXXX") < 0) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "wb");
    if (!file) { int error = errno; close(fd); unlink(temporary); errno = error; return -1; }
    int failed = fprintf(file, "CHAT1 %lu %lld %zu\n", s->id, (long long)s->updated, s->count) < 0;
    if (!failed) failed = write_field(file, s->title) < 0 || write_field(file, s->source) < 0;
    for (size_t i = 0; i < s->count && !failed; ++i) {
        stored_message_t *m = &s->messages[i];
        failed = fprintf(file, "%c %d\n", m->role, m->context) < 0 || write_field(file, m->text) < 0;
    }
    if (!failed) failed = fflush(file) != 0 || fsync(fd) < 0;
    int error = errno;
    if (fclose(file) != 0) { failed = 1; error = errno; }
    if (!failed && rename(temporary, target) < 0) { failed = 1; error = errno; }
    if (failed) { unlink(temporary); errno = error ? error : EIO; return -1; }
    /* After rename the committed snapshot is authoritative, even if directory
     * fsync reports a durability warning: do not roll memory back in that case. */
    int directory_fd = open(s->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd >= 0) {
        if (fsync(directory_fd) < 0) perror("server: sync session directory");
        close(directory_fd);
    }
    return 0;
}

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
        if (!fgets(header, sizeof(header), file) || sscanf(header, "%c %d %c", &role, &context, &extra) != 2 ||
            !strchr("UAX", role) || (context != 0 && context != 1)) { errno = EPROTO; goto done; }
        char *text = read_field(file, (size_t)st.st_size);
        if (!text) goto done;
        int added = add_message(s, role, context, text);
        free(text);
        if (added < 0) goto done;
    }
    if (fgetc(file) != EOF || ferror(file)) { errno = EPROTO; goto done; }
    result = 0;
 done: {
        int error = errno;
        fclose(file);
        if (result < 0) store_close(s);
        errno = error;
    }
    return result;
}

int store_open(const char *directory, unsigned long id, stored_session_t *out) {
    char path[STORE_PATH_MAX];
    if (id == 0 || path_for(path, directory, id, "lock") < 0) { errno = EINVAL; return -1; }
    int fd = lock_path(path, 1);
    if (fd < 0) return -1;
    if (load_session(directory, id, out) < 0) { int error = errno; close(fd); errno = error; return -1; }
    out->lock_fd = fd;
    return 0;
}

static unsigned long filename_id(const char *name, const char *suffix) {
    unsigned long id = 0; char extension[16], extra;
    if (sscanf(name, "session_%lu.%15s%c", &id, extension, &extra) != 2 ||
        strcmp(extension, suffix) != 0) return 0;
    char canonical[80];
    snprintf(canonical, sizeof(canonical), "session_%lu.%s", id, suffix);
    return strcmp(name, canonical) == 0 ? id : 0;
}

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
    if (highest == ULONG_MAX) { errno = EOVERFLOW; return -1; }
    memset(s, 0, sizeof(*s)); s->lock_fd = -1;
    s->id = highest + 1; s->updated = time(NULL);
    if (snprintf(s->directory, sizeof(s->directory), "%s", directory) >= (int)sizeof(s->directory)) {
        errno = ENAMETOOLONG; return -1;
    }
    s->title = strdup("New conversation"); s->source = strdup("");
    char path[STORE_PATH_MAX];
    if (!s->title || !s->source || path_for(path, directory, s->id, "lock") < 0) goto fail;
    s->lock_fd = lock_path(path, 1);
    if (s->lock_fd < 0 || (publish && save_session(s) < 0)) goto fail;
    return 0;
 fail: {
        int error = errno; store_close(s); errno = error; return -1;
    }
}

int store_create(const char *directory, stored_session_t *out) {
    char path[STORE_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.lock", directory) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG; return -1;
    }
    int lock = lock_path(path, 0);
    if (lock < 0) return -1;
    int result = create_locked(directory, out, 1), error = errno;
    close(lock); errno = error; return result;
}

int store_list(const char *directory, store_list_callback callback, void *userdata) {
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        unsigned long id = filename_id(entry->d_name, "dat");
        if (!id) continue;
        stored_session_t s;
        if (load_session(directory, id, &s) < 0) {
            if (callback(id, 0, 1, "[Unreadable session]", userdata) != 0) { result = -1; break; }
            continue;
        }
        char path[STORE_PATH_MAX];
        if (path_for(path, directory, id, "lock") < 0) { store_close(&s); result = -1; break; }
        int fd = lock_path(path, 1);
        int busy = fd < 0;
        if (fd >= 0) close(fd);
        result = callback(id, s.updated, busy, s.title, userdata);
        store_close(&s);
        if (result != 0) break;
    }
    closedir(dir);
    return result;
}

int store_context(const stored_session_t *stored, session_t *context) {
    if (session_init(context) < 0) return -1;
    for (size_t i = 0; i + 1 < stored->count; ++i) {
        const stored_message_t *u = &stored->messages[i], *a = &stored->messages[i + 1];
        if (u->role == 'U' && a->role == 'A' && u->context && a->context) {
            if (session_add_user(context, u->text) < 0 || session_add_assistant(context, a->text) < 0) {
                session_destroy(context); return -1;
            }
            ++i;
        }
    }
    return 0;
}

int store_commit_turn(stored_session_t *s, const char *question, const char *answer) {
    size_t old_count = s->count;
    time_t old_updated = s->updated;
    char *old_title = s->title;
    if (add_message(s, 'U', 1, question) < 0 || add_message(s, 'A', 1, answer) < 0) goto fail;
    if (!*s->source && old_count == 0) {
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
        while (s->count > old_count) free(s->messages[--s->count].text);
        if (s->title != old_title) free(s->title);
        s->title = old_title; s->updated = old_updated;
        errno = error; return -1;
    }
}

/* Legacy files have no escaping. Only exact complete header blocks delimit
 * messages; unmatched text is kept as an X record rather than discarded. */
static int legacy_header(const char *at, const char **body, char *role) {
    if (strlen(at) < 24 || at[0] != '[' || at[20] != ']' || at[21] != '\n') return 0;
    for (int i = 1; i < 20; ++i) {
        char expected = (i == 5 || i == 8) ? '-' : i == 11 ? ' ' : (i == 14 || i == 17) ? ':' : 0;
        if (expected ? at[i] != expected : at[i] < '0' || at[i] > '9') return 0;
    }
    const char *p = at + 22;
    while (*p == '\n') ++p;
    if (strncmp(p, "USER:\n", 6) == 0) { *role = 'U'; *body = p + 6; return 1; }
    if (strncmp(p, "ASSISTANT:\n", 11) == 0) { *role = 'A'; *body = p + 11; return 1; }
    return 0;
}

static int import_file(stored_session_t *s, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    struct stat st;
    if (fstat(fileno(file), &st) < 0 || st.st_size < 0) { fclose(file); return -1; }
    size_t length = (size_t)st.st_size;
    char *text = malloc(length + 1);
    if (!text) { fclose(file); return -1; }
    if (fread(text, 1, length, file) != length || memchr(text, 0, length)) {
        free(text); fclose(file); errno = EPROTO; return -1;
    }
    fclose(file); text[length] = 0;
    s->updated = st.st_mtime;
    const char *start = text;
    char role = 'X';
    int result = 0;
    for (char *p = text; *p; ++p) {
        const char *body; char next_role;
        if ((p == text || p[-1] == '\n') && legacy_header(p, &body, &next_role)) {
            if (p > start || role != 'X') {
                size_t size = (size_t)(p - start);
                /* logger_write_message appends exactly two separator newlines. */
                if (size >= 2 && start[size - 1] == '\n' && start[size - 2] == '\n') size -= 2;
                char *part = strndup(start, size);
                if (!part || add_message(s, role, 0, part) < 0) { free(part); result = -1; break; }
                free(part);
            }
            start = body; role = next_role;
            p = (char *)body - 1;
        }
    }
    if (!result && (start < text + length || role != 'X')) {
        size_t size = (size_t)(text + length - start);
        if (size >= 2 && start[size - 1] == '\n' && start[size - 2] == '\n') size -= 2;
        char *part = strndup(start, size);
        if (!part || add_message(s, role, 0, part) < 0) result = -1;
        free(part);
    }
    for (size_t i = 0; i + 1 < s->count; ++i) {
        if (s->messages[i].role == 'U' && s->messages[i + 1].role == 'A') {
            s->messages[i].context = s->messages[i + 1].context = 1;
            ++i;
        }
    }
    free(text); return result;
}

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

int store_init(const char *directory, const char *legacy_directory) {
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    char path[STORE_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/logs", directory) >= (int)sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    if (snprintf(path, sizeof(path), "%s/.lock", directory) >= (int)sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    int lock = lock_path(path, 0);
    if (lock < 0) return -1;
    DIR *dir = opendir(legacy_directory);
    if (!dir) { int error = errno; close(lock); return error == ENOENT ? 0 : -1; }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!filename_id(entry->d_name, "log")) continue;
        int known = already_imported(directory, entry->d_name);
        if (known < 0) { fprintf(stderr, "server: unreadable store; skipping automatic legacy import\n"); break; }
        if (known) continue;
        stored_session_t s;
        if (create_locked(directory, &s, 0) < 0) { result = -1; break; }
        free(s.source); free(s.title);
        s.source = strdup(entry->d_name);
        char title[280]; snprintf(title, sizeof(title), "[Imported] %s", entry->d_name);
        s.title = strdup(title);
        if (snprintf(path, sizeof(path), "%s/%s", legacy_directory, entry->d_name) >= (int)sizeof(path) ||
            !s.source || !s.title || import_file(&s, path) < 0 || save_session(&s) < 0) {
            char target[STORE_PATH_MAX];
            if (path_for(target, directory, s.id, "dat") == 0) unlink(target);
            store_close(&s); result = -1; break;
        }
        store_close(&s);
    }
    closedir(dir); close(lock); return result;
}
