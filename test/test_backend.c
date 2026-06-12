/*
 * Simple HTTP/1.1 keep-alive backend for proxy testing.
 * Serves multiple requests per connection until client closes or idle timeout.
 * Usage: ./test_backend <port>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#define BUF_SIZE   65536
#define MAX_CLIENTS 64
#define IDLE_TIMEOUT 30  /* 空闲超时（秒），超时后关闭连接 */

static volatile int running = 1;

static void sig_handler(int sig) { (void)sig; running = 0; }

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * 从请求中解析 Connection 头，判断是否 keep-alive。
 * HTTP/1.1 默认 keep-alive，HTTP/1.0 需显式 Connection: keep-alive。
 */
static int want_keepalive(const char *req, ssize_t len) {
    const char *end = req + len;
    const char *p;
    int is_http11 = 0;

    /* 检查 HTTP 版本 */
    p = req;
    while (p < end && *p != '\r' && *p != '\n') {
        if (p + 7 < end && memcmp(p, "HTTP/1.", 7) == 0) {
            if (p[7] == '1') is_http11 = 1;
            break;
        }
        p++;
    }

    /* 查找 Connection: close 或 Connection: keep-alive */
    p = req;
    while (p + 12 < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\r') line_end++;

        if (line_end - p > 10) {
            /* 大小写不敏感比较 "connection:" */
            if ((p[0] == 'C' || p[0] == 'c')
                && (p[1] == 'o' || p[1] == 'O')
                && (p[2] == 'n' || p[2] == 'N')
                && (p[3] == 'n' || p[3] == 'N')
                && (p[4] == 'e' || p[4] == 'E')
                && (p[5] == 'c' || p[5] == 'C')
                && (p[6] == 't' || p[6] == 'T')
                && (p[7] == 'i' || p[7] == 'I')
                && (p[8] == 'o' || p[8] == 'O')
                && (p[9] == 'n' || p[9] == 'N')
                && p[10] == ':')
            {
                const char *val = p + 11;
                while (val < line_end && *val == ' ') val++;
                if (val + 4 < line_end
                    && (val[0] == 'c' || val[0] == 'C')
                    && (val[1] == 'l' || val[1] == 'L')
                    && (val[2] == 'o' || val[2] == 'O')
                    && (val[3] == 's' || val[3] == 'S')
                    && (val[4] == 'e' || val[4] == 'E'))
                {
                    return 0;  /* Connection: close */
                }
            }
        }

        p = line_end;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }

    return is_http11 ? 1 : 0;
}

static void build_response(char *buf, size_t *len, const char *method,
                           const char *path, int keepalive, char * http_requeset)
{
    const char *status       = "200 OK";
    const char *content_type = "text/plain";
    const char *resp_body;
    char        content_len[32];

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        resp_body = "Hello from backend\n";
    } else if (strcmp(path, "/echo") == 0) {
        resp_body = http_requeset;
    } else if (strcmp(path, "/headers") == 0) {
        resp_body = "Headers test passed\n";
    } else if (strcmp(path, "/large") == 0) {
        static char large[8192];
        memset(large, 'X', sizeof(large) - 1);
        large[sizeof(large) - 1] = '\n';
        resp_body = large;
    } else if (strcmp(path, "/error") == 0) {
        status    = "500 Internal Server Error";
        resp_body = "Backend error\n";
    } else {
        status    = "404 Not Found";
        resp_body = "Not found\n";
    }

    snprintf(content_len, sizeof(content_len), "%zu", strlen(resp_body));

    *len = (size_t)snprintf(buf, BUF_SIZE,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %s\r\n"
        "Connection: %s\r\n"
        "\r\n"
        "%s",
        status, content_type, content_len,
        keepalive ? "keep-alive" : "close",
        resp_body);
}

int main(int argc, char **argv) {
    int port = 9090;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) { perror("socket"); return 1; }

    {
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

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
    printf("backend (keep-alive) listening on :%d\n", port);
    fflush(stdout);

    /* 客户端连接状态 */
    struct {
        int    fd;
        time_t start_time;     /* 连接建立时间 */
        time_t last_active;
        int    req_count;      /* 此连接已服务的请求数 */
    } clients[MAX_CLIENTS];
    int client_count = 0;

    while (running) {
        fd_set         rfds;
        int            max_fd = listen_fd;
        struct timeval tv     = { .tv_sec = 1, .tv_usec = 0 };
        int            i;

        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);

        for (i = 0; i < client_count; i++) {
            if (clients[i].fd > 0) {
                FD_SET(clients[i].fd, &rfds);
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
            }
        }

        if (select(max_fd + 1, &rfds, NULL, NULL, &tv) < 0) continue;

        /* accept 新连接 */
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in client_addr;
            socklen_t          client_len = sizeof(client_addr);
            int                fd;

            fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (fd != -1) {
                set_nonblocking(fd);

                /* 找空位放入 */
                for (i = 0; i < client_count; i++) {
                    if (clients[i].fd == 0) {
                        clients[i].fd          = fd;
                        clients[i].start_time  = time(NULL);
                        clients[i].last_active = time(NULL);
                        clients[i].req_count   = 0;
                        fd = 0; break;
                    }
                }
                if (fd > 0) {
                    /* 没有空位，扩容 */
                    if (client_count < MAX_CLIENTS) {
                        clients[client_count].fd          = fd;
                        clients[client_count].start_time  = time(NULL);
                        clients[client_count].last_active = time(NULL);
                        clients[client_count].req_count   = 0;
                        client_count++;
                    } else {
                        printf("too many clients, dropping\n");
                        close(fd);
                    }
                }

                printf("new connection (total: %d)\n", client_count);
                fflush(stdout);
            }
        }

        /* 处理已有连接的请求 */
        time_t now = time(NULL);

        for (i = 0; i < client_count; i++) {
            if (clients[i].fd == 0) continue;

            /* 空闲超时检查 */
            if (now - clients[i].last_active > IDLE_TIMEOUT) {
                printf("[close] fd=%d  reason=idle_timeout  alive=%lds  reqs=%d\n",
                       clients[i].fd,
                       (long)(now - clients[i].start_time),
                       clients[i].req_count);
                close(clients[i].fd);
                clients[i].fd = 0;
                continue;
            }

            if (!FD_ISSET(clients[i].fd, &rfds)) continue;

            int  fd       = clients[i].fd;
            char buf[BUF_SIZE];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);

            if (n <= 0) {
                /* 客户端关闭 (n==0) 或连接中断 (n<0) */
                const char *reason = (n == 0) ? "client_close" : "connection_reset";
                printf("[close] fd=%d  reason=%s  alive=%lds  reqs=%d  errno=%d(%s)\n",
                       fd, reason,
                       (long)(now - clients[i].start_time),
                       clients[i].req_count,
                       (n < 0) ? errno : 0,
                       (n < 0) ? strerror(errno) : "");
                close(fd);
                clients[i].fd = 0;
                continue;
            }

            clients[i].req_count++;

            buf[n]     = '\0';
            clients[i].last_active = now;

            char method[16] = "", path[256] = "";
            sscanf(buf, "%15s %255s", method, path);

            int keepalive = want_keepalive(buf, n);

            char   resp[BUF_SIZE];
            size_t resp_len;
            build_response(resp, &resp_len, method, path, keepalive, buf);

            write(fd, resp, resp_len);
            printf("[%s] %s → %zd bytes  %s\n",
                   method, path, resp_len,
                   keepalive ? "(keep-alive)" : "(close)");
            fflush(stdout);

            if (!keepalive) {
                /* 客户端要求关闭，发送完响应后关闭 */
                printf("[close] fd=%d  reason=connection_close  alive=%lds  reqs=%d\n",
                       fd,
                       (long)(now - clients[i].start_time),
                       clients[i].req_count);
                shutdown(fd, SHUT_WR);
                close(fd);
                clients[i].fd = 0;
            }
            /* keepalive: fd 保持 open，等待 select 触发下一次读 */
        }
    }

    /* 清理 */
    {
        time_t now = time(NULL);
        for (int i = 0; i < client_count; i++) {
            if (clients[i].fd > 0) {
                printf("[close] fd=%d  reason=shutdown  alive=%lds  reqs=%d\n",
                       clients[i].fd,
                       (long)(now - clients[i].start_time),
                       clients[i].req_count);
                close(clients[i].fd);
            }
        }
    }
    close(listen_fd);
    printf("backend stopped\n");
    return 0;
}
