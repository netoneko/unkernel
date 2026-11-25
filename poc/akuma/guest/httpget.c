// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
/* httpget HOST [PATH [PORT]]: resolves HOST (getaddrinfo over UDP, answered by the page's DNS-over-HTTPS), connects,
 * sends a plain HTTP/1.0 GET and prints the reply. The smallest real-network guest: ?elf=httpget.elf&arg=example.com&net=ws */
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: httpget HOST [PATH [PORT]]\n"); return 2; }
    const char *host = argv[1], *path = argc > 2 ? argv[2] : "/", *port = argc > 3 ? argv[3] : "80";
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    int r = getaddrinfo(host, port, &hints, &res);
    if (r) { fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(r)); return 1; }
    char ip[32];
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);
    fprintf(stderr, "%s = %s\n", host, ip);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen)) { perror("connect"); return 1; }
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: akuma-on-kwemu\r\nConnection: close\r\n\r\n", path, host);
    if (write(fd, req, n) != n) { perror("write"); return 1; }
    char buf[4096];
    long total = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) { fwrite(buf, 1, n, stdout); total += n; }
    fprintf(stderr, "\n[%ld bytes]\n", total);
    return n < 0;
}
