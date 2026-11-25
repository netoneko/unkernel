// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
/* Exercises the read-only root file system of a kernel on kwemu: stdio, stat, directories, errors, big reads. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); bad++; } } while (0)

int main(void) {
    char buf[256];
    FILE *f = fopen("/etc/motd", "r");
    CHECK(f && fgets(buf, sizeof buf, f));
    printf("motd: %s", buf);
    CHECK(strncmp(buf, "Welcome to Akuma", 16) == 0);
    CHECK(fseek(f, 0, SEEK_END) == 0 && ftell(f) == 26);
    fclose(f);

    struct stat st;
    CHECK(stat("/etc/motd", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 26);
    CHECK(stat("/etc/../etc/./motd", &st) == 0);
    CHECK(stat("/etc", &st) == 0 && S_ISDIR(st.st_mode));
    printf("/etc isdir=%d\n", S_ISDIR(st.st_mode));
    CHECK(stat("/data/missing", &st) == -1 && errno == ENOENT);
    CHECK(fopen("/etc/motd", "w") == NULL && errno == EROFS);
    printf("errno checks: ENOENT=%d EROFS=%d\n", ENOENT, EROFS);

    DIR *d = opendir("/");
    CHECK(d != NULL);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d)); n++) printf("  / %s\n", e->d_name);
    CHECK(n >= 5); // ., .., etc, bin, data
    if (d) closedir(d);
    d = opendir("/etc");
    n = 0;
    for (struct dirent *e; d && (e = readdir(d)); n++) {}
    CHECK(n == 4); // ., .., motd, os-release
    if (d) closedir(d);

    int fd = open("/data/big.bin", O_RDONLY);
    CHECK(fd >= 0);
    long total = 0;
    char chunk[1000];
    for (ssize_t r; (r = read(fd, chunk, sizeof chunk)) > 0;) {
        for (ssize_t i = 0; i < r; i++) CHECK(chunk[i] == "0123456789abcdef"[(total + i) % 16]);
        total += r;
    }
    printf("read %ld bytes\n", total);
    CHECK(total == 16000);
    CHECK(lseek(fd, 5, SEEK_SET) == 5 && read(fd, chunk, 3) == 3 && memcmp(chunk, "567", 3) == 0);
    close(fd);
    CHECK(read(fd, chunk, 1) == -1 && errno == EBADF);

    printf(bad ? "fstest FAILED\n" : "fstest ok\n");
    return bad ? 1 : 7;
}
