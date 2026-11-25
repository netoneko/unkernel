// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
/* Lives in the root file system as /bin/echoargs: prints its arguments on stdout and exits 3. */
#include <stdio.h>

int main(int argc, char **argv) {
    printf("echoargs:");
    for (int i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");
    return 3;
}
