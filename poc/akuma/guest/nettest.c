// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
/* Sockets on a kernel on kwemu, against the fake world in testnet.mjs: a timed sleep, a DNS lookup over UDP, a blocking
 * connect and GET, a non-blocking connect with poll and a 100 KB body, a refused connection, and a poll timeout. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); bad++; } } while (0)

static long ms_since(const struct timespec *a) {
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
}

static int get(int fd, const char *path) {
    char req[128];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n", path);
    return send(fd, req, n, 0) == n ? 0 : -1;
}

int main(void) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(nanosleep(&(struct timespec){0, 60 * 1000 * 1000}, NULL) == 0);
    long slept = ms_since(&t0);
    printf("slept %ld ms\n", slept);
    CHECK(slept >= 60 && slept < 5000);

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    int r = getaddrinfo("example.test", "80", &hints, &res);
    CHECK(r == 0 && res);
    if (!res) return 1;
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    char ip[32];
    inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
    printf("example.test = %s\n", ip);
    CHECK(strcmp(ip, "10.0.2.2") == 0);

    /* blocking connect, request, read to end of file */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0);
    CHECK(get(fd, "/hello") == 0);
    char buf[4096];
    long total = 0;
    for (ssize_t n; (n = recv(fd, buf + total, sizeof buf - 1 - total, 0)) > 0;) total += n;
    buf[total] = 0;
    CHECK(strstr(buf, "200 OK") && strstr(buf, "hello from the fake server, path=/hello"));
    printf("blocking GET: %ld bytes\n", total);
    close(fd);

    /* non-blocking connect, poll, a big body */
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    r = connect(fd, res->ai_addr, res->ai_addrlen);
    CHECK(r == -1 && errno == EINPROGRESS);
    struct pollfd p = {.fd = fd, .events = POLLOUT};
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT));
    int err = -1;
    socklen_t el = sizeof err;
    CHECK(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
    CHECK(get(fd, "/big") == 0);
    static char big[110000];
    total = 0;
    for (;;) {
        p.events = POLLIN;
        int pr = poll(&p, 1, 3000);
        CHECK(pr == 1);
        if (pr != 1) break;
        ssize_t n = recv(fd, big + total, sizeof big - 1 - total, 0);
        if (n == 0) break;
        if (n < 0) { CHECK(errno == EAGAIN); continue; }
        total += n;
    }
    printf("non-blocking GET: %ld bytes\n", total);
    CHECK(total > 100000 && total < 100200);
    CHECK(memcmp(big + total - 16, "0123456789abcdef", 16) == 0);
    close(fd);

    /* a closed port is refused */
    struct sockaddr_in dead = *sin;
    dead.sin_port = htons(81);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(fd, (struct sockaddr *)&dead, sizeof dead) == -1 && errno == ECONNREFUSED);
    close(fd);

    /* poll times out on a pipe nobody writes to */
    int pp[2];
    CHECK(pipe(pp) == 0);
    p = (struct pollfd){.fd = pp[0], .events = POLLIN};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(poll(&p, 1, 80) == 0);
    slept = ms_since(&t0);
    printf("poll timeout after %ld ms\n", slept);
    CHECK(slept >= 80 && slept < 5000);

    freeaddrinfo(res);
    printf(bad ? "nettest FAILED\n" : "nettest ok\n");
    return bad ? 1 : 7;
}
