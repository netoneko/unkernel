// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
/* Processes on a kernel on kwemu: pipe, fork, dup2, execve, wait4, blocking both ways, kill. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;
static volatile int shared = 1;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); bad++; } } while (0)

static long drain(int fd, char *out, long cap) {
    long total = 0;
    for (ssize_t r; (r = read(fd, out + total, cap - total)) > 0;) total += r;
    return total;
}

int main(void) {
    int st, p[2];
    char buf[256];

    /* fork + dup2 + execve: the child's stdout is the pipe, the program comes from the file system. */
    CHECK(pipe(p) == 0);
    pid_t me = getpid();
    pid_t c = fork();
    if (c == 0) {
        shared = 99; /* a private copy: the parent must never see this */
        if (getppid() != me) _exit(101);
        close(p[0]);
        dup2(p[1], 1);
        close(p[1]);
        execl("/bin/echoargs", "echoargs", "alpha", "beta", (char *)0);
        _exit(100);
    }
    CHECK(c > 0 && c != me);
    close(p[1]);
    long n = drain(p[0], buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    close(p[0]);
    printf("child said: %s", buf);
    CHECK(strcmp(buf, "echoargs: alpha beta\n") == 0);
    CHECK(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 3);
    CHECK(shared == 1);

    /* Backpressure: 20000 bytes through a 4096-byte pipe, so the writer blocks when full and the reader when empty. */
    int q[2];
    CHECK(pipe(q) == 0);
    c = fork();
    if (c == 0) {
        close(q[0]);
        char chunk[1000];
        for (int i = 0; i < 20; i++) {
            memset(chunk, 'a' + i, sizeof chunk);
            for (long off = 0; off < 1000;) {
                ssize_t w = write(q[1], chunk + off, 1000 - off);
                if (w <= 0) _exit(102);
                off += w;
            }
        }
        _exit(5);
    }
    close(q[1]);
    static char big[20000 + 8];
    n = drain(q[0], big, sizeof big);
    printf("backpressure: read %ld bytes\n", n);
    CHECK(n == 20000);
    for (long i = 0; i < n; i++)
        if (big[i] != 'a' + i / 1000) { CHECK(!"byte order"); break; }
    CHECK(waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 5);

    /* A child that never exits is killed. */
    c = fork();
    if (c == 0) for (;;) sched_yield();
    CHECK(kill(c, SIGKILL) == 0);
    CHECK(waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

    /* No children left. */
    CHECK(wait(&st) == -1 && errno == ECHILD);

    printf(bad ? "pipetest FAILED\n" : "pipetest ok\n");
    return bad ? 1 : 7;
}
