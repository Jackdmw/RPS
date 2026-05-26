/*
 * Proxy test client.
 * Sends HTTP requests to the RPS reverse proxy and verifies responses.
 *
 * Usage: ./test_proxy_client [proxy_host] [proxy_port]
 * Default: 127.0.0.1:8000
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
#include <netdb.h>

static int total_tests  = 0;
static int passed_tests = 0;

#define TEST(name) do { \
    total_tests++; \
    printf("  TEST: %s ... ", name); \
} while(0)

#define PASS() do { \
    passed_tests++; \
    printf("PASSED\n"); \
} while(0)

#define FAIL(fmt, ...) do { \
    printf("FAILED: " fmt "\n", ##__VA_ARGS__); \
} while(0)

static int connect_to(const char *host, int port) {
    struct addrinfo hints, *res, *rp;
    char port_str[8];
    int fd = -1;

    snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/*
 * Send an HTTP request and return the raw response.
 * Caller must free *resp_out.
 */
static int do_request(const char *host, int port,
                      const char *method, const char *path,
                      const char *extra_headers,
                      const char *body,
                      char **resp_out, size_t *resp_len_out) {
    int fd = connect_to(host, port);
    if (fd < 0) {
        return -1;
    }

    char req[8192];
    int req_len;

    if (body && strlen(body) > 0) {
        req_len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "%s"
            "Content-Length: %zu\r\n"
            "\r\n"
            "%s",
            method, path, host,
            extra_headers ? extra_headers : "",
            strlen(body), body);
    } else {
        req_len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "%s"
            "\r\n",
            method, path, host,
            extra_headers ? extra_headers : "");
    }

    if (send(fd, req, (size_t)req_len, 0) < 0) {
        close(fd);
        return -1;
    }

    size_t cap = 65536;
    char *resp = malloc(cap);
    if (!resp) {
        close(fd);
        return -1;
    }

    size_t total = 0;
    for (;;) {
        ssize_t n = recv(fd, resp + total, cap - total - 1, 0);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= cap - 1) {
            cap *= 2;
            char *tmp = realloc(resp, cap);
            if (!tmp) break;
            resp = tmp;
        }
    }
    resp[total] = '\0';
    close(fd);

    *resp_out      = resp;
    *resp_len_out  = total;
    return 0;
}

static int test_basic_get(const char *host, int port) {
    TEST("GET / -> proxy -> backend");
    char *resp = NULL;
    size_t len = 0;

    if (do_request(host, port, "GET", "/", NULL, NULL, &resp, &len) != 0) {
        FAIL("connection failed (is RPS running on %s:%d?)", host, port);
        return -1;
    }

    int status;
    if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1) {
        FAIL("could not parse status line from: %.100s", resp);
        free(resp);
        return -1;
    }

    if (status != 200) {
        FAIL("expected 200, got %d", status);
        free(resp);
        return -1;
    }

    if (!strstr(resp, "\"received\": true")) {
        FAIL("response body missing expected content");
        free(resp);
        return -1;
    }

    PASS();
    free(resp);
    return 0;
}

static int test_post_with_body(const char *host, int port) {
    TEST("POST /api/data -> proxy -> backend");
    char *resp = NULL;
    size_t len = 0;
    const char *body = "{\"key\":\"value\"}";

    if (do_request(host, port, "POST", "/api/data", NULL,
                   body, &resp, &len) != 0) {
        FAIL("connection failed");
        return -1;
    }

    int status;
    if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1) {
        FAIL("could not parse status");
        free(resp);
        return -1;
    }

    if (status != 200) {
        FAIL("expected 200, got %d", status);
        free(resp);
        return -1;
    }

    if (!strstr(resp, "POST")) {
        FAIL("response does not reflect POST method");
        free(resp);
        return -1;
    }

    if (!strstr(resp, "{\"key\":\"value\"}")) {
        FAIL("request body not proxied correctly");
        free(resp);
        return -1;
    }

    PASS();
    free(resp);
    return 0;
}

static int test_custom_header(const char *host, int port) {
    TEST("X-Test-Header -> proxy -> backend");
    char *resp = NULL;
    size_t len = 0;

    if (do_request(host, port, "GET", "/headers",
                   "X-Test-Header: hello-proxy\r\n",
                   NULL, &resp, &len) != 0) {
        FAIL("connection failed");
        return -1;
    }

    int status;
    if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1) {
        FAIL("could not parse status");
        free(resp);
        return -1;
    }

    /* Current backend doesn't echo headers, but the proxy should forward it
     * and we should get a 200. This tests the proxy doesn't break on custom headers. */
    if (status != 200) {
        FAIL("expected 200, got %d", status);
        free(resp);
        return -1;
    }

    PASS();
    free(resp);
    return 0;
}

static int test_not_found_path(const char *host, int port) {
    TEST("GET /nonexistent -> verify proxy still works");
    char *resp = NULL;
    size_t len = 0;

    if (do_request(host, port, "GET", "/nonexistent", NULL,
                   NULL, &resp, &len) != 0) {
        FAIL("connection failed");
        return -1;
    }

    int status;
    if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1) {
        FAIL("could not parse status");
        free(resp);
        return -1;
    }

    if (status != 200) {
        FAIL("expected 200, got %d", status);
        free(resp);
        return -1;
    }

    if (!strstr(resp, "/nonexistent")) {
        FAIL("URI not properly sent to backend");
        free(resp);
        return -1;
    }

    PASS();
    free(resp);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int         port = 8000;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    printf("=== RPS Proxy Test Client ===\n");
    printf("Target: %s:%d\n\n", host, port);

    test_basic_get(host, port);
    test_post_with_body(host, port);
    test_custom_header(host, port);
    test_not_found_path(host, port);

    printf("\n=== Results: %d/%d passed ===\n", passed_tests, total_tests);

    return (passed_tests == total_tests) ? 0 : 1;
}
