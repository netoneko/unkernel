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
    printf("errno checks: ENOENT=%d\n", ENOENT);

    DIR *d = opendir("/");
    CHECK(d != NULL);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d)); n++) printf("  / %s\n", e->d_name);
    CHECK(n == 5); // ., .., etc, bin, data
    if (d) closedir(d);
    d = opendir("/etc");
    n = 0;
    for (struct dirent *e; d && (e = readdir(d)); n++) {}
    CHECK(n == 5); // ., ., motd, os-release, resolv.conf
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

    /* The tree is writable: mkdir, create, write across pages, read back, append, truncate, rename, symlink. */
    CHECK(mkdir("/tmp", 0755) == 0 && mkdir("/tmp", 0755) == -1 && errno == EEXIST);
    CHECK(mkdir("/tmp/t", 0755) == 0);
    fd = open("/tmp/t/a.bin", O_RDWR | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0);
    static char data[10000], back[10000];
    for (int i = 0; i < 10000; i++) data[i] = (char)(i * 7 + 3);
    CHECK(write(fd, data, sizeof data) == sizeof data);
    CHECK(open("/tmp/t/a.bin", O_CREAT | O_EXCL | O_WRONLY, 0644) == -1 && errno == EEXIST);
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, back, sizeof back) == sizeof back && memcmp(data, back, sizeof data) == 0);
    CHECK(fstat(fd, &st) == 0 && st.st_size == 10000 && S_ISREG(st.st_mode));
    CHECK(pwrite(fd, "XY", 2, 9999) == 2 && fstat(fd, &st) == 0 && st.st_size == 10001);
    CHECK(ftruncate(fd, 5000) == 0 && fstat(fd, &st) == 0 && st.st_size == 5000);
    CHECK(ftruncate(fd, 9000) == 0 && pread(fd, back, 8, 6000) == 8 && back[0] == 0 && back[7] == 0); /* a hole reads as zeros */
    close(fd);
    fd = open("/tmp/t/a.bin", O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "end", 3) == 3);
    close(fd);
    CHECK(stat("/tmp/t/a.bin", &st) == 0 && st.st_size == 9003);
    CHECK(rename("/tmp/t/a.bin", "/tmp/b.bin") == 0 && stat("/tmp/t/a.bin", &st) == -1 && stat("/tmp/b.bin", &st) == 0);
    CHECK(symlink("b.bin", "/tmp/link") == 0);
    char target[64];
    CHECK(readlink("/tmp/link", target, sizeof target) == 5 && memcmp(target, "b.bin", 5) == 0);
    CHECK(stat("/tmp/link", &st) == 0 && S_ISREG(st.st_mode) && lstat("/tmp/link", &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(chdir("/tmp/t") == 0 && getcwd(buf, sizeof buf) && strcmp(buf, "/tmp/t") == 0);
    CHECK(stat("../b.bin", &st) == 0 && st.st_size == 9003);
    CHECK(rmdir("/tmp/t") == 0); /* the working directory itself may go: it is empty */
    CHECK(chdir("/") == 0);
    CHECK(unlink("/tmp/link") == 0 && unlink("/tmp/b.bin") == 0 && unlink("/tmp/b.bin") == -1 && errno == ENOENT);
    CHECK(rmdir("/tmp") == 0 && rmdir("/etc") == -1 && errno == ENOTEMPTY);
    /* a file of the boot archive is copied on first write */
    fd = open("/etc/motd", O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && write(fd, "hi\n", 3) == 3);
    close(fd);
    CHECK(stat("/etc/motd", &st) == 0 && st.st_size == 3);

    printf(bad ? "fstest FAILED\n" : "fstest ok\n");
    return bad ? 1 : 7;
}
