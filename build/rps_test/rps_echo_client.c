#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF_SIZE 4096

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> [message]\n", argv[0]);
        return 1;
    }

    const char *host    = argv[1];
    int         port    = atoi(argv[2]);
    const char *message = (argc > 3) ? argv[3] : "hello";

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid address: %s\n", host);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        return 1;
    }

    /* 发送 */
    size_t msg_len = strlen(message);
    ssize_t n = write(fd, message, msg_len);
    if (n < 0) { perror("write"); close(fd); return 1; }
    printf("sent: %s\n", message);

    /* 关闭写端，告诉对端发送完毕 */
    shutdown(fd, SHUT_WR);

    /* 读取回显 */
    char buf[BUF_SIZE];
    printf("recv: ");
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    printf("\n");

    close(fd);
    return 0;
}
