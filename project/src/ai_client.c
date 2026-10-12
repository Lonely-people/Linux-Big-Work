#include "ai_client.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define AI_MAX_HEADERS 16384
#define AI_MAX_SSE_LINE 16384
#define AI_MAX_ERROR 256
#define AI_MAX_TOKENS 512

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} string_builder_t;

static void set_error(char *buffer, size_t capacity, const char *format, ...) {
    if (buffer == NULL || capacity == 0) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
}

static int builder_reserve(string_builder_t *builder, size_t additional) {
    if (additional > SIZE_MAX - builder->length - 1) {
        errno = EOVERFLOW;
        return -1;
    }
    size_t required = builder->length + additional + 1;
    if (required <= builder->capacity) {
        return 0;
    }
    size_t capacity = builder->capacity == 0 ? 256 : builder->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *data = realloc(builder->data, capacity);
    if (data == NULL) {
        return -1;
    }
    builder->data = data;
    builder->capacity = capacity;
    return 0;
}

static int builder_append(string_builder_t *builder, const char *text, size_t length) {
    if (builder_reserve(builder, length) < 0) {
        return -1;
    }
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return 0;
}

static int builder_append_char(string_builder_t *builder, char value) {
    return builder_append(builder, &value, 1);
}

static int builder_append_json_string(string_builder_t *builder, const char *text) {
    if (builder_append_char(builder, '"') < 0) {
        return -1;
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        char escaped[7];
        size_t escaped_length = 0;
        switch (*cursor) {
            case '"': memcpy(escaped, "\\\"", 2); escaped_length = 2; break;
            case '\\': memcpy(escaped, "\\\\", 2); escaped_length = 2; break;
            case '\b': memcpy(escaped, "\\b", 2); escaped_length = 2; break;
            case '\f': memcpy(escaped, "\\f", 2); escaped_length = 2; break;
            case '\n': memcpy(escaped, "\\n", 2); escaped_length = 2; break;
            case '\r': memcpy(escaped, "\\r", 2); escaped_length = 2; break;
            case '\t': memcpy(escaped, "\\t", 2); escaped_length = 2; break;
            default:
                if (*cursor < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                    escaped_length = 6;
                } else {
                    escaped[0] = (char)*cursor;
                    escaped_length = 1;
                }
                break;
        }
        if (builder_append(builder, escaped, escaped_length) < 0) {
            return -1;
        }
    }
    return builder_append_char(builder, '"');
}

static int contains_case_insensitive(const char *text, const char *needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0) return 1;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (strncasecmp(cursor, needle, needle_length) == 0) return 1;
    }
    return 0;
}

static int build_request_body(const session_t *session, char **body, size_t *body_length) {
    string_builder_t builder = {0};
    if (builder_append(&builder, "{\"messages\":[", 13) < 0) {
        goto fail;
    }
    for (size_t index = 0; index < session_count(session); ++index) {
        const session_message_t *message = session_message_at(session, index);
        if (index > 0 && builder_append_char(&builder, ',') < 0) {
            goto fail;
        }
        if (builder_append(&builder, "{\"role\":", strlen("{\"role\":")) < 0 ||
            builder_append_json_string(&builder, message->role) < 0 ||
            builder_append(&builder, ",\"content\":", strlen(",\"content\":")) < 0 ||
            builder_append_json_string(&builder, message->content) < 0 ||
            builder_append_char(&builder, '}') < 0) {
            goto fail;
        }
    }
    const char suffix[] = "],\"stream\":true,\"max_tokens\":512,"
                          "\"chat_template_kwargs\":{\"enable_thinking\":false}}";
    if (builder_append(&builder, suffix, strlen(suffix)) < 0) {
        goto fail;
    }
    *body = builder.data;
    *body_length = builder.length;
    return 0;

fail:
    free(builder.data);
    return -1;
}

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

static int connect_backend(const char *host, unsigned short port) {
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    if (getaddrinfo(host, port_text, &hints, &results) != 0) {
        errno = ECONNREFUSED;
        return -1;
    }

    int socket_fd = -1;
    for (struct addrinfo *entry = results; entry != NULL; entry = entry->ai_next) {
        socket_fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        struct timeval timeout = {.tv_sec = 120, .tv_usec = 0};
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(socket_fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(results);
    return socket_fd;
}

static int read_headers(int fd, char *headers, size_t capacity, int *status,
                        int *chunked, size_t *content_length) {
    size_t used = 0;
    while (used + 1 < capacity) {
        // 这个recv就是和模型端通信的代码
        // fd 是听哪一个端口，header+used是把读取到的东西存在哪一个位置， 1 是读取多少字节 
        ssize_t result = recv(fd, headers + used, 1, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = ECONNRESET;
            return -1;
        }
        ++used;
        headers[used] = '\0';
        if (used >= 4 && memcmp(headers + used - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    if (used + 1 >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    if (sscanf(headers, "HTTP/%*s %d", status) != 1) {
        errno = EPROTO;
        return -1;
    }
    *chunked = 0;
    *content_length = 0;
    char *cursor = headers;
    while ((cursor = strstr(cursor, "\r\n")) != NULL) {
        cursor += 2;
        if (*cursor == '\r' && cursor[1] == '\n') {
            break;
        }
        if (strncasecmp(cursor, "Transfer-Encoding:", 18) == 0 &&
            contains_case_insensitive(cursor + 18, "chunked")) {
            *chunked = 1;
        }
        if (strncasecmp(cursor, "Content-Length:", 15) == 0) {
            *content_length = (size_t)strtoull(cursor + 15, NULL, 10);
        }
    }
    return 0;
}

typedef struct {
    int fd;
    int chunked;
    int finished;
    size_t remaining;
    size_t content_remaining;
} body_reader_t;

static int read_body_line(body_reader_t *reader, char *line, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        char byte;
        ssize_t result = recv(reader->fd, &byte, 1, 0);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return -1;
        if (byte == '\n') {
            line[used] = '\0';
            if (used > 0 && line[used - 1] == '\r') line[--used] = '\0';
            return 0;
        }
        line[used++] = byte;
    }
    errno = EMSGSIZE;
    return -1;
}

static ssize_t body_read(body_reader_t *reader, char *buffer, size_t capacity) {
    if (reader->finished) return 0;
    if (!reader->chunked) {
        if (reader->content_remaining == 0 && reader->remaining != 0) {
            reader->finished = 1;
            return 0;
        }
        size_t wanted = capacity;
        if (reader->remaining != 0 && wanted > reader->remaining) wanted = reader->remaining;
        ssize_t result = recv(reader->fd, buffer, wanted, 0);
        if (result == 0) reader->finished = 1;
        if (result > 0 && reader->remaining != 0) reader->remaining -= (size_t)result;
        return result;
    }

    // 读取一个chunk块有多少个字节
    if (reader->remaining == 0) {
        char size_line[64];
        if (read_body_line(reader, size_line, sizeof(size_line)) < 0) return -1;
        char *end = NULL;
        unsigned long long size = strtoull(size_line, &end, 16);
        if (end == size_line) {
            errno = EPROTO;
            return -1;
        }
        if (size == 0) {
            reader->finished = 1;
            return 0;
        }
        reader->remaining = (size_t)size;
    }

    size_t wanted = reader->remaining < capacity ? reader->remaining : capacity;
    // 这里面只会读需要的字节数量
    ssize_t result = recv(reader->fd, buffer, wanted, 0);
    if (result <= 0) return result;
    reader->remaining -= (size_t)result;
    if (reader->remaining == 0) {
        char crlf[2];
        if (recv(reader->fd, crlf, 2, MSG_WAITALL) != 2 || crlf[0] != '\r' || crlf[1] != '\n') {
            errno = EPROTO;
            return -1;
        }
    }
    return result;
}

static int append_utf8(string_builder_t *builder, unsigned int codepoint) {
    char bytes[4];
    size_t length;
    if (codepoint <= 0x7f) {
        bytes[0] = (char)codepoint; length = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3f)); length = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = (char)(0xe0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (codepoint & 0x3f)); length = 3;
    } else if (codepoint <= 0x10ffff) {
        bytes[0] = (char)(0xf0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (codepoint & 0x3f)); length = 4;
    } else {
        errno = EPROTO;
        return -1;
    }
    return builder_append(builder, bytes, length);
}

static int extract_json_string(const char *json, const char *key,
                               char *output, size_t capacity) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *cursor = strstr(json, needle);
    if (cursor == NULL) return 1;
    cursor = strchr(cursor + strlen(needle), ':');
    if (cursor == NULL) return -1;
    ++cursor;
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor != '"') return 1;
    ++cursor;
    string_builder_t builder = {0};
    while (*cursor != '\0' && *cursor != '"') {
        unsigned char raw_byte = (unsigned char)*cursor++;
        if (raw_byte != '\\') {
            if (builder_append(&builder, (const char *)&raw_byte, 1) < 0) {
                free(builder.data);
                return -1;
            }
            continue;
        }

        unsigned int codepoint;
        if (*cursor == '\0') {
            free(builder.data);
            return -1;
        }
        {
            switch (*cursor++) {
                case '"': codepoint = '"'; break;
                case '\\': codepoint = '\\'; break;
                case '/': codepoint = '/'; break;
                case 'b': codepoint = '\b'; break;
                case 'f': codepoint = '\f'; break;
                case 'n': codepoint = '\n'; break;
                case 'r': codepoint = '\r'; break;
                case 't': codepoint = '\t'; break;
                case 'u': {
                    char hex[5] = {cursor[0], cursor[1], cursor[2], cursor[3], '\0'};
                    char *end = NULL;
                    codepoint = (unsigned int)strtoul(hex, &end, 16);
                    if (end != hex + 4) { free(builder.data); return -1; }
                    cursor += 4;
                    break;
                }
                default: free(builder.data); return -1;
            }
        }
        if (append_utf8(&builder, codepoint) < 0) {
            free(builder.data);
            return -1;
        }
    }
    if (*cursor != '"' || builder.length + 1 > capacity) {
        free(builder.data);
        return -1;
    }
    memcpy(output, builder.data, builder.length + 1);
    free(builder.data);
    return 0;
}

static int process_sse_line(const char *line, ai_chunk_callback callback,
                           void *userdata, int *done, char *error, size_t error_capacity) {
    if (strncmp(line, "data:", 5) != 0) return 0;
    const char *payload = line + 5;
    while (*payload == ' ') ++payload;
    if (strcmp(payload, "[DONE]") == 0) {
        *done = 1;
        return 0;
    }
    if (strstr(payload, "\"error\"") != NULL) {
        char message[AI_MAX_ERROR];
        if (extract_json_string(payload, "message", message, sizeof(message)) == 0) {
            set_error(error, error_capacity, "%s", message);
        } else {
            set_error(error, error_capacity, "llama-server returned an error");
        }
        return -1;
    }
    char content[AI_MAX_SSE_LINE];
    // line里面存储的是json文本，所以直接找到content键值对应的字符串值，放入缓冲区
    int result = extract_json_string(payload, "content", content, sizeof(content));
    if (result < 0) {
        set_error(error, error_capacity, "invalid content in model stream");
        return -1;
    }
    // 成功处理之后会把content里面的内容通过callback函数发送到客户端里面
    if (result == 0 && content[0] != '\0' && callback(content, strlen(content), userdata) != 0) {
        set_error(error, error_capacity, "client disconnected during model stream");
        return -1;
    }
        return 0;
}

int ai_client_stream(const ai_client_config_t *config, const session_t *session,
                     ai_chunk_callback callback, void *userdata,
                     char *error_buffer, size_t error_capacity) {
    if (config == NULL || session == NULL || callback == NULL ||
        config->host == NULL || config->path == NULL) {
        set_error(error_buffer, error_capacity, "invalid AI client configuration");
        errno = EINVAL;
        return -1;
    }

    char *body = NULL;
    size_t body_length = 0;
    // 将session里面的历史对话做成json文件
    if (build_request_body(session, &body, &body_length) < 0) {
        set_error(error_buffer, error_capacity, "cannot build model request");
        return -1;
    }
    // 主动连接模型所在的网络端口
    int socket_fd = connect_backend(config->host, config->port);
    if (socket_fd < 0) {
        set_error(error_buffer, error_capacity, "cannot connect to llama-server at %s:%u",
                  config->host, (unsigned int)config->port);
        free(body);
        return -1;
    }

    // http请求头的构造，请求内容在别的地方
    char header[1024];
    int header_length = snprintf(header, sizeof(header),
                                 "POST %s HTTP/1.1\r\n"
                                 "Host: %s:%u\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Accept: text/event-stream\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n\r\n",
                                 config->path, config->host, (unsigned int)config->port,
                                 body_length);
    int result = -1;
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        send_all(socket_fd, header, (size_t)header_length) < 0 ||
        send_all(socket_fd, body, body_length) < 0) {
        set_error(error_buffer, error_capacity, "failed to send model request");
        goto cleanup;
    }


    char headers[AI_MAX_HEADERS];
    int status = 0;
    int chunked = 0;
    size_t content_length = 0;
    // 阅读模型返回的http响应头和返回状态
    if (read_headers(socket_fd, headers, sizeof(headers), &status, &chunked, &content_length) < 0) {
        set_error(error_buffer, error_capacity, "invalid response from llama-server");
        goto cleanup;
    }
    if (status < 200 || status >= 300) {
        set_error(error_buffer, error_capacity, "llama-server HTTP status %d", status);
        goto cleanup;
    }

    body_reader_t reader = {
        .fd = socket_fd,
        .chunked = chunked,
        .remaining = chunked ? 0 : content_length,
        .content_remaining = content_length,
    };
    char body_buffer[4096];
    char line[AI_MAX_SSE_LINE];
    size_t line_length = 0;
    int done = 0;
    while (!done) {
        // 这个是读取正文的部分，把读取到的正文报文存在body_buffer里面
        // byte代表的是bodybuffer实际存了多少字节的东西
        // 即使满了也会在while循环下重新读回来
        ssize_t bytes = body_read(&reader, body_buffer, sizeof(body_buffer));
        if (bytes < 0) {
            set_error(error_buffer, error_capacity, "error reading llama-server stream");
            goto cleanup;
        }
        if (bytes == 0) break;
        for (ssize_t index = 0; index < bytes; ++index) {
            char byte = body_buffer[index];
            // 观察报文发现，都是一行为基准处理的
            if (byte == '\n') {
                line[line_length] = '\0';
                if (line_length > 0 && line[line_length - 1] == '\r') {
                    line[--line_length] = '\0';
                }
                if (process_sse_line(line, callback, userdata, &done,
                                     error_buffer, error_capacity) < 0) {
                    goto cleanup;
                }
                line_length = 0;
            } else if (line_length + 1 < sizeof(line)) {
                // 这个就是把一行的内容放在一个line变量里面
                line[line_length++] = byte;
            } else {
                set_error(error_buffer, error_capacity, "model SSE line is too long");
                goto cleanup;
            }
        }
    }
    if (!done) {
        set_error(error_buffer, error_capacity, "llama-server stream ended before [DONE]");
        goto cleanup;
    }
    result = 0;

cleanup:
    close(socket_fd);
    free(body);
    return result;
}
