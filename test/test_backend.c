/*
 * Simple HTTP backend for proxy testing.
 * Responds with known content based on path.
 * Usage: ./test_backend <port>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>

#define BUF_SIZE 65536

static volatile int running = 1;

static void sig_handler(int sig) { (void)sig; running = 0; }

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void build_response(char *buf, size_t *len, const char *method,
                           const char *path, const char *body, size_t body_len)
{
    const char *status = "200 OK";
    const char *content_type = "text/plain";
    const char *resp_body;
    char content_len[32];

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        resp_body = "Hello from backend\n";
    } else if (strcmp(path, "/echo") == 0) {
        resp_body = "ECHO: request received\n";
    } else if (strcmp(path, "/headers") == 0) {
        resp_body = "Headers test passed\n";
    } else if (strcmp(path, "/large") == 0) {
        static char large[8192];
        memset(large, 'X', sizeof(large) - 1);
        large[sizeof(large) - 1] = '\n';
        resp_body = large;
    } else if (strcmp(path, "/error") == 0) {
        status = "500 Internal Server Error";
        resp_body = "Backend error\n";
    } else {
        status = "404 Not Found";
        resp_body = "Not found\n";
    }

    snprintf(content_len, sizeof(content_len), "%zu", strlen(resp_body));
    *len = (size_t)snprintf(buf, BUF_SIZE,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %s\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, content_type, content_len, resp_body);
}

int main(int argc, char **argv) {
    int port = 9090;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); return 1;
    }
    if (listen(listen_fd, 16) == -1) {
        perror("listen"); return 1;
    }

    set_nonblocking(listen_fd);
    printf("backend listening on :%d\n", port);
    fflush(stdout);

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(listen_fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (fd == -1) continue;

        char buf[BUF_SIZE];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';

            char method[16] = "", path[256] = "";
            sscanf(buf, "%15s %255s", method, path);

            char *body_sep = strstr(buf, "\r\n\r\n");
            char *body = body_sep ? body_sep + 4 : "";
            size_t body_len = body_sep ? (size_t)(n - (body - buf)) : 0;

            char resp[BUF_SIZE];
            size_t resp_len;
            build_response(resp, &resp_len, method, path, body, body_len);

            write(fd, resp, resp_len);
            printf("[%s] %s → %zd bytes\n", method, path, resp_len);
            fflush(stdout);
        }
        close(fd);
    }

    close(listen_fd);
    printf("backend stopped\n");
    return 0;
}
