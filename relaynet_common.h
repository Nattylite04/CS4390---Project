
#ifndef RELAYNET_COMMON_H
#define RELAYNET_COMMON_H

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RN_LINE 4096
#define RN_MAX_TRACKER (256 * 1024)
#define RN_CHUNK 1024
#define RN_MAX_PEERS 64

static inline int rn_valid_name(const char *s) {
    if (!s || !*s || strlen(s) > 160 || strstr(s, ".."))
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.'))
            return 0;
    return 1;
}

static inline int rn_sendall(int fd, const void *data, size_t n) {
    const char *p = data;
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static inline ssize_t rn_readline(int fd, char *buf, size_t max) {
    size_t n = 0;
    while (n + 1 < max) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return n ? (ssize_t)n : r;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return (ssize_t)n;
}

static inline int rn_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static inline int rn_md5_bytes(
    const unsigned char *data, size_t size, char out[33]
) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    int ok = EVP_DigestInit_ex(ctx, EVP_md5(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, data, size) == 1 &&
             EVP_DigestFinal_ex(ctx, md, &mdlen) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok || mdlen != 16) return -1;
    for (int i = 0; i < 16; i++)
        sprintf(out + 2*i, "%02x", md[i]);
    out[32] = 0;
    return 0;
}

static inline int rn_md5_file(const char *path, char out[33]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fclose(f);
        return -1;
    }
    unsigned char buf[8192], digest[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    int ok = EVP_DigestInit_ex(ctx, EVP_md5(), NULL) == 1;
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), f)) != 0)
        ok = EVP_DigestUpdate(ctx, buf, n) == 1;
    if (ferror(f)) ok = 0;
    if (ok) ok = EVP_DigestFinal_ex(ctx, digest, &mdlen) == 1;
    EVP_MD_CTX_free(ctx);
    fclose(f);
    if (!ok || mdlen != 16) return -1;
    for (int i = 0; i < 16; i++)
        sprintf(out + 2*i, "%02x", digest[i]);
    out[32] = 0;
    return 0;
}

static inline int rn_writefile(
    const char *path, const void *data, size_t size
) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size;
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}

static inline char *rn_readfile(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) ||
        (long)(*len = (size_t)ftell(f)) < 0 ||
        *len > RN_MAX_TRACKER ||
        fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    char *p = malloc(*len + 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    if (fread(p, 1, *len, f) != *len) {
        free(p);
        fclose(f);
        return NULL;
    }
    p[*len] = 0;
    fclose(f);
    return p;
}

static inline void rn_trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\r' || s[n-1] == '\n' ||
           isspace((unsigned char)s[n-1])))
        s[--n] = 0;
}

#endif
