/**
 * 用法：
  # 默认：每次只发 1 字节，无延迟（极端拆包）
  ./rps_echo_client 127.0.0.1 8080

  # 每次发 3 字节，间隔 100us
  ./rps_echo_client -c 3 -d 100 127.0.0.1 8080

  选项：
  - -c N — chunk 大小（默认 1 字节），模拟不同粒度的拆包
  - -d N — chunk 间延迟（微秒），模拟慢速客户端/网络延迟

  交互示例：
  connected to 127.0.0.1:8080  chunk=1 byte(s)  delay=0 us
  > GET / HTTP/1.1\r\nHost: localhost\r\n\r\n
  [sent 40 bytes in 1-byte chunks]
  < HTTP/1.1 200 OK ...
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define BUF_SIZE (256 * 1024)



/* unescape \r \n \t \\ \0 into real bytes. returns new length. */
static size_t unescape(char *buf, size_t len) {
    size_t w = 0;
    for (size_t r = 0; r < len; r++) {
        if (buf[r] == '\\' && r + 1 < len) {
            switch (buf[r + 1]) {
            case 'r':  buf[w++] = '\r'; r++; break;
            case 'n':  buf[w++] = '\n'; r++; break;
            case 't':  buf[w++] = '\t'; r++; break;
            case '0':  buf[w++] = '\0'; r++; break;
            case '\\': buf[w++] = '\\'; r++; break;
            default:   buf[w++] = buf[r]; break;
            }
        } else {
            buf[w++] = buf[r];
        }
    }
    return w;
}

int main(int argc, char **argv) {
    int chunk_size = 1;
    int delay_us   = 0;
    const char *host;
    int port;

    /* parse optional args: -c chunk_size -d delay_us */
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            chunk_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delay_us = atoi(argv[++i]);
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
        i++;
    }

    if (argc - i < 2) {
        fprintf(stderr, "Usage: %s [-c chunk_size] [-d delay_us] <host> <port>\n", argv[0]);
        fprintf(stderr, "  -c  chunk size in bytes (default 1)\n");
        fprintf(stderr, "  -d  delay between chunks in microseconds (default 0)\n");
        return 1;
    }

    if (chunk_size < 1) chunk_size = 1;

    host = argv[i];
    port = atoi(argv[i + 1]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) { perror("socket"); return 1; }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt SO_REUSEADDR");
    }

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

    printf("connected to %s:%d  chunk=%d byte(s)  delay=%d us\n",
           host, port, chunk_size, delay_us);
    printf("(Ctrl+D to quit, Ctrl+C to interrupt)\n\n");

    char send_buf[BUF_SIZE];
    char recv_buf[BUF_SIZE];

    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(send_buf, sizeof(send_buf), stdin) == NULL) {
            if (ferror(stdin)) perror("fgets");
            break;
        }

        size_t len = strlen(send_buf);
        if (len == 0) continue;

        /* strip the trailing \n added by stdin Enter key */
        if (len > 0 && send_buf[len - 1] == '\n') {
            send_buf[--len] = '\0';
        }

        /* unescape \r \n \t \\ \0 to real bytes */
        len = unescape(send_buf, len);

        if (len == 0) continue;

        /* send in tiny chunks to simulate TCP fragmentation */
        size_t sent = 0;
        while (sent < len
) {
            size_t remain = len - sent;
            size_t nbytes = (chunk_size < (int)remain) ? (size_t)chunk_size : remain;

            ssize_t n = write(fd, send_buf + sent, nbytes);
            if (n <= 0) {
                if (n == 0) {
                    fprintf(stderr, "\nserver closed connection\n");
                } else {
                    perror("write");
                }
                goto done;
            }
            sent += (size_t)n;

            if (delay_us > 0) usleep((unsigned int)delay_us);
        }

        printf("[sent %zu bytes in %d-byte chunks]\n", sent, chunk_size);

        /* read echo */
        size_t received = 0;
        while (received < len
) {
            ssize_t n = read(fd, recv_buf + received, sizeof(recv_buf) - 1 - received);
            if (n <= 0) {
                if (n == 0) {
                    fprintf(stderr, "server closed connection\n");
                } else {
                    if (errno == EINTR) continue;
                    perror("read");
                }
                goto done;
            }
            received += (size_t)n;
        }
        recv_buf[received] = '\0';

        /* strip trailing newline for display */
        size_t dlen = received;
        while (dlen > 0 && (recv_buf[dlen - 1] == '\n' || recv_buf[dlen - 1] == '\r')) {
            recv_buf[--dlen] = '\0';
        }
        printf("< %s\n\n", recv_buf);
    }

done:
    close(fd);
    printf("disconnected\n");
    return 0;
}
