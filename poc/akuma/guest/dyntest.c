// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
/* A dynamically linked program: PT_INTERP (/lib/ld-musl-aarch64.so.1 from the root file system), DT_NEEDED libfoo.so
 * mapped from files, relocations through the GOT, then dlopen of libbar.so and dlsym. Exercises the interpreter
 * loader, AT_BASE, and file-backed mmap. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int foo(int);
extern const char *foo_name;

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(int argc, char **argv) {
    CHECK(foo(1) == 43);
    CHECK(strcmp(foo_name, "libfoo") == 0);
    void *h = dlopen("libbar.so", RTLD_NOW);
    CHECK(h != NULL);
    if (h) {
        int (*bar)(int) = (int (*)(int))dlsym(h, "bar");
        CHECK(bar && bar(2) == 20);
        dlclose(h);
    } else {
        printf("dlopen: %s\n", dlerror());
    }
    printf("argc=%d argv0=%s\n", argc, argv[0]);
    printf(bad ? "dyntest FAILED\n" : "dyntest ok\n");
    return bad ? 1 : 7;
}
