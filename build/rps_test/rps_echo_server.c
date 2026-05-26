#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define DEFAULT_PORT    8080
#define MAX_EVENTS      1024
#define MAX_CONN        1024
#define BUF_SIZE        4096

typedef struct {
    int     fd;
    char    buf[BUF_SIZE];
    size_t  buf_len;    /* 已读取待发送的数据长度 */
    size_t  buf_sent;   /* 已发送的字节数 */
} conn_t;

static conn_t connections[MAX_CONN];

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

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
    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen"); return 1;
    }

    set_nonblocking(listen_fd);

    int epfd = epoll_create1(0);
    if (epfd == -1) { perror("epoll_create1"); return 1; }

    struct epoll_event ev, events[MAX_EVENTS];
    ev.events  = EPOLLIN;
    ev.data.ptr = NULL;  /* listen_fd 用 ptr==NULL 标识 */
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("Echo server listening on port %d\n", port);

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            conn_t *c = (conn_t *)events[i].data.ptr;

            if (c == NULL) {
                /* 监听 fd：accept 新连接 */
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }

                    set_nonblocking(client_fd);

                    /* 找一个空闲的 conn 槽位 */
                    int slot = -1;
                    for (int j = 0; j < MAX_CONN; j++) {
                        if (connections[j].fd == 0) { slot = j; break; }
                    }
                    if (slot == -1) {
                        fprintf(stderr, "too many connections\n");
                        close(client_fd);
                        continue;
                    }

                    conn_t *conn = &connections[slot];
                    memset(conn, 0, sizeof(*conn));
                    conn->fd = client_fd;

                    ev.events   = EPOLLIN;
                    ev.data.ptr = conn;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

                    printf("[+] new connection from %s:%d (fd=%d, slot=%d)\n",
                           inet_ntoa(client_addr.sin_addr),
                           ntohs(client_addr.sin_port),
                           client_fd, slot);
                }
                continue;
            }

            /* 已有连接：处理读写事件 */
            uint32_t revents = events[i].events;

            /* ---- 先处理写（发送积压数据） ---- */
            if (revents & EPOLLOUT) {
                while (c->buf_sent < c->buf_len) {
                    ssize_t w = write(c->fd, c->buf + c->buf_sent,
                                      c->buf_len - c->buf_sent);
                    if (w == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) goto check_read;
                        perror("write");
                        goto close_conn;
                    }
                    c->buf_sent += w;
                }
                /* 全部发完，切回只监听读 */
                c->buf_len  = 0;
                c->buf_sent = 0;
                ev.events   = EPOLLIN;
                ev.data.ptr = c;
                epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
            }

check_read:
            /* ---- 再处理读 ---- */
            if (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                /* 只有在没有积压数据时才读新数据 */
                if (c->buf_len == 0) {
                    ssize_t n = read(c->fd, c->buf, sizeof(c->buf));
                    if (n > 0) {
                        c->buf_len  = n;
                        c->buf_sent = 0;

                        /* 尝试直接发送 */
                        while (c->buf_sent < c->buf_len) {
                            ssize_t w = write(c->fd, c->buf + c->buf_sent,
                                              c->buf_len - c->buf_sent);
                            if (w == -1) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    /* 发不完，切到 EPOLLIN|EPOLLOUT */
                                    ev.events   = EPOLLIN | EPOLLOUT;
                                    ev.data.ptr = c;
                                    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
                                    goto next_event;
                                }
                                perror("write");
                                goto close_conn;
                            }
                            c->buf_sent += w;
                        }
                        /* 全部发完 */
                        c->buf_len  = 0;
                        c->buf_sent = 0;
                    } else if (n == 0) {
                        printf("[-] connection closed (fd=%d)\n", c->fd);
                        goto close_conn;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) goto next_event;
                        perror("read");
                        goto close_conn;
                    }
                }
            }

            continue;

close_conn:
            epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
            close(c->fd);
            memset(c, 0, sizeof(*c));
next_event:
            ;
        }
    }

    close(listen_fd);
    close(epfd);
    return 0;
}
