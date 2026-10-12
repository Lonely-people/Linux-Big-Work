#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static int connect_to_server(const char *host, unsigned short port) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("client: socket");
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fprintf(stderr, "client: host must be an IPv4 address: %s\n", host);
        close(socket_fd);
        return -1;
    }
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("client: connect");
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static int receive_reply(int socket_fd) {
    char line[PROTOCOL_MAX_LINE];
    char payload[PROTOCOL_MAX_LINE];
    int got_begin = 0;

    for (;;) {
        int result = protocol_read_line(socket_fd, line, sizeof(line));
        if (result == 0) {
            fprintf(stderr, "client: server closed the connection\n");
            return -1;
        }
        if (result == -1) {
            perror("client: recv");
            return -1;
        }
        if (result == -2) {
            fprintf(stderr, "client: server frame is too long\n");
            return -1;
        }
        if (strcmp(line, "BEGIN") == 0) {
            got_begin = 1;
            fputs("AI: ", stdout);
            fflush(stdout);
            continue;
        }
        if (strncmp(line, "DATA ", 5) == 0) {
            if (!got_begin || protocol_unescape(line + 5, payload, sizeof(payload)) < 0) {
                fprintf(stderr, "client: invalid DATA frame\n");
                return -1;
            }
            fputs(payload, stdout);
            fflush(stdout);
            continue;
        }
        if (strcmp(line, "END") == 0) {
            if (!got_begin) {
                fprintf(stderr, "client: unexpected END frame\n");
                return -1;
            }
            fputc('\n', stdout);
            return 0;
        }
        if (strncmp(line, "ERROR ", 6) == 0) {
            if (protocol_unescape(line + 6, payload, sizeof(payload)) < 0) {
                fprintf(stderr, "client: invalid ERROR frame\n");
            } else {
                fprintf(stderr, "server error: %s\n", payload);
            }
            return 1;
        }

        fprintf(stderr, "client: unknown server frame: %s\n", line);
        return -1;
    }
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    unsigned short port = PROTOCOL_DEFAULT_PORT;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [host] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc == 3 && parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "Usage: %s [host] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct sigaction pipe_action;
    memset(&pipe_action, 0, sizeof(pipe_action));
    pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&pipe_action.sa_mask);
    if (sigaction(SIGPIPE, &pipe_action, NULL) < 0) {
        perror("client: sigaction(SIGPIPE)");
        return EXIT_FAILURE;
    }

    int socket_fd = connect_to_server(host, port);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    char input[PROTOCOL_MAX_INPUT];
    for (;;) {
        fputs("you> ", stdout);
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            (void)protocol_send_quit(socket_fd);
            break;
        }

        size_t length = strlen(input);
        if (length > 0 && input[length - 1] == '\n') {
            input[--length] = '\0';
        } else if (!feof(stdin)) {
            fprintf(stderr, "client: input is limited to %d bytes\n", PROTOCOL_MAX_INPUT - 1);
            int character;
            while ((character = getchar()) != '\n' && character != EOF) {
                /* Discard the remainder of an overlong input line. */
            }
            continue;
        }

        if (strcmp(input, "/quit") == 0) {
            (void)protocol_send_quit(socket_fd);
            break;
        }
        if (protocol_send_query(socket_fd, input) < 0) {
            perror("client: send");
            break;
        }
        if (receive_reply(socket_fd) < 0) {
            break;
        }
    }

    close(socket_fd);
    return EXIT_SUCCESS;
}
