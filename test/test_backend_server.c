/*
 * HTTP backend server for reverse proxy testing.
 * Listens on a given port (default 8080), parses the HTTP request,
 * and responds with a JSON-ish body containing the request method,
 * URI, headers, and body for verification.
 *
 * Usage: ./test_backend_server [port]
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

static volatile sig_atomic_t running = 1;

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

static void build_response(char *buf, size_t *len,
                           const char *method, const char *uri,
                           const char *body, size_t body_len) {
    const char *status = "200 OK";
    const char *ct     = "application/json";

    size_t hdr_len = (size_t)snprintf(buf, 8192,
        "HTTP/1.1 %s\r\n"
        "Server: TestBackend/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, ct, body_len);

    memcpy(buf + hdr_len, body, body_len);
    *len = hdr_len + body_len;
}

static int listen_and_serve(int port) {
    int fd, conn_fd;
    struct sockaddr_in addr;
    socklen_t addr_len;
    int opt = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 10) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    printf("[backend] listening on 0.0.0.0:%d\n", port);

    while (running) {
        addr_len = sizeof(addr);
        conn_fd = accept(fd, (struct sockaddr *)&addr, &addr_len);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char recv_buf[8192];
        ssize_t n = recv(conn_fd, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n <= 0) {
            close(conn_fd);
            continue;
        }
        recv_buf[n] = '\0';

        /* Parse request line */
        char method[16] = {0};
        char uri[2048]   = {0};
        sscanf(recv_buf, "%15s %2047s", method, uri);

        /* Find body start (after \r\n\r\n) */
        char *body_start = NULL;
        for (ssize_t i = 0; i < n - 3; i++) {
            if (recv_buf[i]     == '\r' && recv_buf[i+1] == '\n' &&
                recv_buf[i+2] == '\r' && recv_buf[i+3] == '\n') {
                body_start = recv_buf + i + 4;
                break;
            }
        }

        size_t body_len = body_start ? (size_t)(recv_buf + n - body_start) : 0;

        /* Build JSON response body */
        char json_body[4096];
        int  json_len;

        if (body_start && body_len > 0) {
            json_len = snprintf(json_body, sizeof(json_body),
                "{\n"
                "  \"received\": true,\n"
                "  \"method\": \"%s\",\n"
                "  \"uri\": \"%s\",\n"
                "  \"body\": \"%.*s\"\n"
                "}\n",
                method, uri, (int)body_len, body_start);
        } else {
            json_len = snprintf(json_body, sizeof(json_body),
                "{\n"
                "  \"received\": true,\n"
                "  \"method\": \"%s\",\n"
                "  \"uri\": \"%s\",\n"
                "  \"body\": \"\"\n"
                "}\n",
                method, uri);
        }

        char resp_buf[16384];
        size_t resp_len = 0;
        build_response(resp_buf, &resp_len,
                       method, uri, json_body, (size_t)json_len);

        send(conn_fd, resp_buf, resp_len, 0);
        printf("[backend] %s %s -> 200 (%zu bytes response)\n",
               method, uri, resp_len);

        close(conn_fd);
    }

    close(fd);
    printf("[backend] stopped\n");
    return 0;
}

int main(int argc, char *argv[]) {
    int port = 8080;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    return listen_and_serve(port);
}
