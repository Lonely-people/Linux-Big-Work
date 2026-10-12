#include "protocol.h"
#include "logger.h"
#include "session.h"
#include "ai_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
    int client_fd;
    char *response;
    size_t response_length;
    size_t response_capacity;
} stream_context_t;

static int append_response(stream_context_t *context, const char *text, size_t length) {
    if (length > SIZE_MAX - context->response_length - 1) {
        errno = EOVERFLOW;
        return -1;
    }
    size_t needed = context->response_length + length + 1;
    if (needed > context->response_capacity) {
        size_t capacity = context->response_capacity == 0 ? 1024 : context->response_capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        char *resized = realloc(context->response, capacity);
        if (resized == NULL) return -1;
        context->response = resized;
        context->response_capacity = capacity;
    }
    memcpy(context->response + context->response_length, text, length);
    context->response_length += length;
    context->response[context->response_length] = '\0';
    return 0;
}

static int forward_model_chunk(const char *text, size_t length, void *userdata) {
    stream_context_t *context = userdata;
    char *terminated = malloc(length + 1);
    if (terminated == NULL) return -1;
    // 把要发送的内容存在terminate里
    memcpy(terminated, text, length);
    terminated[length] = '\0';

    // 向客户端发送内容
    int result = protocol_send_data(context->client_fd, terminated);
    free(terminated);
    if (result < 0) return -1;
    return append_response(context, text, length);
}

static void reap_children(int signal_number) {
    (void)signal_number;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* Reap every child that has already exited. */
    }
    errno = saved_errno;
}

static int parse_port(const char *text, unsigned short *port_out) {
    char *end = NULL;
    long value;

    if (text == NULL || port_out == NULL || *text == '\0') {
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) {
        return -1;
    }
    *port_out = (unsigned short)value;
    return 0;
}

/*
这个函数是处理一个客户端的多次请求的函数
*/
static void handle_client(int client_fd, logger_t *logger, session_t *session) {
    // 存下读到的全部内容
    char line[PROTOCOL_MAX_LINE];
    // 去掉表示符之后得到的内容
    char question[PROTOCOL_MAX_INPUT];

    for (;;) {
        // 从客户端端口读取一行内容
        int result = protocol_read_line(client_fd, line, sizeof(line));
        if (result == 0) {
            break;
        }
        if (result == -1) {
            perror("server: recv");
            break;
        }
        if (result == -2) {
            if (protocol_send_error(client_fd, "request line is too long") < 0) {
                break;
            }
            continue;
        }

        if (strcmp(line, "QUIT") == 0) {
            break;
        }
        if (strncmp(line, "Q ", 2) != 0) {
            if (protocol_send_error(client_fd, "expected Q <question> or QUIT") < 0) {
                break;
            }
            continue;
        }

        // 用来将转义字符变成普通的文本
        if (protocol_unescape(line + 2, question, sizeof(question)) < 0) {
            if (protocol_send_error(client_fd, "invalid escaped question") < 0) {
                break;
            }
            continue;
        }

        // 把question写在日志里面
        if (logger_write_user(logger, question) < 0) {
            perror("server: write user log");
        }
        // 把question保存在session日志里面
        if (session_add_user(session, question) < 0) {
            (void)protocol_send_error(client_fd, "cannot store session history");
            continue;
        }
        // 向客户端发出要写入的信号
        if (protocol_send_begin(client_fd) < 0) {
            //如果失败就撤回session里面保存的内容
            session_remove_last_user(session);
            break;
        }

        // 配置的是服务端向模型请求的部分
        stream_context_t stream = {.client_fd = client_fd};
        //这个是模型服务的地址
        const ai_client_config_t config = {
            .host = AI_DEFAULT_HOST,
            .port = AI_DEFAULT_PORT,
            .path = AI_DEFAULT_PATH,
        };
        char error[256] = {0};
        
        // 这个是向模型发出提问，然后模型给出回答的函数.
        // 算的上核心函数，实现了流式生成
        if (ai_client_stream(&config, session, forward_model_chunk, &stream,
                             error, sizeof(error)) < 0) {
            free(stream.response);
            session_remove_last_user(session);
            if (protocol_send_error(client_fd, error[0] == '\0'
                                                ? "model request failed" : error) < 0) {
                break;
            }
            continue;
        }

        // 发送结束
        if (protocol_send_end(client_fd) < 0) {
            free(stream.response);
            session_remove_last_user(session);
            break;
        }
        const char *complete_response = stream.response == NULL ? "" : stream.response;
        // 将模型生成的内容写入session
        if (session_add_assistant(session, complete_response) < 0) {
            perror("server: store assistant history");
        }
        if (logger_write_assistant(logger, complete_response) < 0) {
            perror("server: write assistant log");
        }
        free(stream.response);
    }
}

static int prepare_log_directory(const char *program_path, char *directory,
                                  size_t capacity) {
    char resolved_path[PATH_MAX];
    if (realpath(program_path, resolved_path) == NULL) {
        if (snprintf(directory, capacity, "logs") >= (int)capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        /* The executable is in project/bin; put logs beside that directory. */
        char *bin_directory = strrchr(resolved_path, '/');
        if (bin_directory == NULL) {
            if (snprintf(directory, capacity, "logs") >= (int)capacity) {
                errno = ENAMETOOLONG;
                return -1;
            }
        } else {
            *bin_directory = '\0';
            char *project_directory = strrchr(resolved_path, '/');
            if (project_directory != NULL) {
                *project_directory = '\0';
            }
            if (snprintf(directory, capacity, "%s/logs", resolved_path) >= (int)capacity) {
                errno = ENAMETOOLONG;
                return -1;
            }
        }
    }

    if (mkdir(directory, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}
/*
这个函数创建了一个端口，这个端口接受客户端配置的端口发来的信息，同时这个服务端进程监听这个端口接受的内容
端口地址为 127:0:0:1 9090
*/
static int create_listener(unsigned short port) {
    // 创建了一个TCP socket AF_INET 代表IPv4,
    // 但是这个server_fd只是用来找到这个对应的socket进程的编号
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("server: socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("server: setsockopt");
        close(server_fd);
        return -1;
    }

    // 设置要监听的IPv4地址
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    // 设置端口sin_port，转换为十六进制
    address.sin_port = htons(port);
    // 设置ip地址sin_addr，转换为二进制形式
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        fprintf(stderr, "server: failed to prepare loopback address\n");
        close(server_fd);
        return -1;
    }

    // 将socket进程绑定在address的通信地址下
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("server: bind");
        close(server_fd);
        return -1;
    }

    // 让这个socket进程进入监听状态，随时准备接受客户端配置的端口发来的连接请求
    if (listen(server_fd, 16) < 0) {
        perror("server: listen");
        close(server_fd);
        return -1;
    }
    return server_fd;
}

int main(int argc, char **argv) {
    unsigned short port = PROTOCOL_DEFAULT_PORT;
    // parse_port 里面已经包含了端口的写入
    if (argc > 2 || (argc == 2 && parse_port(argv[1], &port) < 0)) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // 写入日志路径函数
    char log_directory[LOGGER_PATH_MAX];
    if (prepare_log_directory(argv[0], log_directory, sizeof(log_directory)) < 0) {
        perror("server: create log directory");
        return EXIT_FAILURE;
    }

    // child_action是在定义信号产生之后的行为
    struct sigaction child_action;
    memset(&child_action, 0, sizeof(child_action));
    // sa_hander 是在接受到信号之后执行这个函数，核心的配置代码
    child_action.sa_handler = reap_children;
    // 不屏蔽其他信号
    sigemptyset(&child_action.sa_mask);
    child_action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    // SIGCHLD 会在子进程退出之后自动向父进程发出信号
    if (sigaction(SIGCHLD, &child_action, NULL) < 0) {
        perror("server: sigaction(SIGCHLD)");
        return EXIT_FAILURE;
    }

    // 这个是处理服务端向断连的客户端发送信息时，防止报错的处理
    struct sigaction pipe_action;
    memset(&pipe_action, 0, sizeof(pipe_action));
    // 特殊值，代表忽略信号不行动
    pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&pipe_action.sa_mask);
    if (sigaction(SIGPIPE, &pipe_action, NULL) < 0) {
        perror("server: sigaction(SIGPIPE)");
        return EXIT_FAILURE;
    }

    // 创建监听服务端的socket进程
    int server_fd = create_listener(port);
    if (server_fd < 0) {
        return EXIT_FAILURE;
    }
    printf("server listening on 127.0.0.1:%u\n", (unsigned int)port);
    fflush(stdout);

    unsigned long next_session_id = 1;
    for (;;) {
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);
        // 获得和127.0.0.0:9090 建立了连接的客户端端口地址
        // 如果暂时没有客户端连接，就会堵塞在accept这一个代码
        int client_fd = accept(server_fd, (struct sockaddr *)&client_address, &client_length);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("server: accept");
            break;
        }

        // 这里的处理有不小的隐患，重新编号之后不会覆盖旧有日志，而是直接接在原来的日志后面
        unsigned long session_id = next_session_id++;
        pid_t child = fork();
        if (child < 0) {
            perror("server: fork");
            (void)protocol_send_error(client_fd, "server cannot create a session process");
            close(client_fd);
            continue;
        }
        // 子进程进入这个条件分支里面
        if (child == 0) {
            // 关闭不需要监听的socket
            close(server_fd);
            // 定义的结构体，指向一个日志文件
            logger_t logger;
            if (logger_open(&logger, log_directory, session_id) < 0) {
                perror("server: open session log");
                logger.fd = -1;
            }
            // 这个指的是客户端连接期间的对话历史
            session_t session;
            if (session_init(&session) < 0) {
                (void)protocol_send_error(client_fd, "cannot initialize session");
                logger_close(&logger);
                close(client_fd);
                _exit(EXIT_FAILURE);
            }
            handle_client(client_fd, &logger, &session);
            // 做完了之后，子进程会在这里被回收掉
            session_destroy(&session);
            logger_close(&logger);
            close(client_fd);
            _exit(EXIT_SUCCESS);
        }
        // 父进程继续等待下一个来的客户端连接，就能够处理多个不同的客户端请求了
        close(client_fd);
    }

    close(server_fd);
    return EXIT_FAILURE;
}
