#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int protocol_send_all(int fd, const void *buffer, size_t length) {
    const char *bytes = (const char *)buffer;
    size_t sent = 0;

    while (sent < length) {
        // result代表发送成功的字节数
        ssize_t result = send(fd, bytes + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

int protocol_read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;
    int overflow = 0;

    if (buffer == NULL || capacity < 2) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        char byte;
        ssize_t result = recv(fd, &byte, 1, 0);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            if (used == 0 && !overflow) {
                return 0;
            }
            break;
        }
        // 读到一行之后就结束
        if (byte == '\n') {
            break;
        }
        if (used + 1 < capacity) {
            buffer[used++] = byte;
        } else {
            overflow = 1;
        }
    }

    buffer[used] = '\0';
    if (used > 0 && buffer[used - 1] == '\r') {
        buffer[--used] = '\0';
    }
    return overflow ? -2 : 1;
}

int protocol_escape(const char *input, char *output, size_t capacity) {
    size_t used = 0;

    if (input == NULL || output == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0'; ++cursor) {
        const char *replacement = NULL;
        char literal[2] = {0, 0};

        switch (*cursor) {
            case '\\': replacement = "\\\\"; break;
            case '\n':  replacement = "\\n"; break;
            case '\r':  replacement = "\\r"; break;
            default:
                literal[0] = (char)*cursor;
                replacement = literal;
                break;
        }

        size_t length = strlen(replacement);
        if (used + length + 1 > capacity) {
            errno = EMSGSIZE;
            return -1;
        }
        memcpy(output + used, replacement, length);
        used += length;
    }

    output[used] = '\0';
    return 0;
}

int protocol_unescape(const char *input, char *output, size_t capacity) {
    size_t used = 0;

    if (input == NULL || output == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    for (size_t index = 0; input[index] != '\0'; ++index) {
        char value = input[index];
        if (value == '\\') {
            char escaped = input[++index];
            if (escaped == '\0') {
                errno = EPROTO;
                return -1;
            }
            switch (escaped) {
                case '\\': value = '\\'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                default:
                    errno = EPROTO;
                    return -1;
            }
        }

        if (used + 1 >= capacity) {
            errno = EMSGSIZE;
            return -1;
        }
        output[used++] = value;
    }

    output[used] = '\0';
    return 0;
}

static int protocol_send_frame(int fd, const char *kind, const char *payload) {
    char escaped[PROTOCOL_MAX_LINE];
    char frame[PROTOCOL_MAX_LINE];

    // 处理转义字符，将文本内容中本身带有的 \n 处理成 \\n 在客户端里面处理的时候能够将正文内容当成一行来处理
    if (protocol_escape(payload, escaped, sizeof(escaped)) < 0) {
        return -1;
    }
    // 将报文类型拼接在前面 
    int length = snprintf(frame, sizeof(frame), "%s %s\n", kind, escaped);
    if (length < 0 || (size_t)length >= sizeof(frame)) {
        errno = EMSGSIZE;
        return -1;
    }
    // 真正执行数据发送的函数，里面的send执行数据发送
    return protocol_send_all(fd, frame, (size_t)length);
}

int protocol_send_query(int fd, const char *text) {
    return protocol_send_frame(fd, "Q", text);
}

int protocol_send_quit(int fd) {
    static const char frame[] = "QUIT\n";
    return protocol_send_all(fd, frame, sizeof(frame) - 1);
}

int protocol_send_begin(int fd) {
    static const char frame[] = "BEGIN\n";
    return protocol_send_all(fd, frame, sizeof(frame) - 1);
}

int protocol_send_data(int fd, const char *text) {
    return protocol_send_frame(fd, "DATA", text);
}

int protocol_send_end(int fd) {
    static const char frame[] = "END\n";
    return protocol_send_all(fd, frame, sizeof(frame) - 1);
}

int protocol_send_error(int fd, const char *text) {
    return protocol_send_frame(fd, "ERROR", text);
}
