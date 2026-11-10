
#ifndef NETWORK_PROTOCOL_H
#define NETWORK_PROTOCOL_H
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_LEN 4096
#define TRACKER_MAX 131072
#define CHUNK_MAX 1024
#define MAX_SEEDS 32

static inline int good_name(const char *s)
{
    if (!s || !*s || strlen(s) > 150 || strstr(s, ".."))
        return 0;

    for (; *s; ++s)
    {
        if (!((*s >= 'a' && *s <= 'z') ||
              (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') ||
              *s == '.' || *s == '_' || *s == '-'))
            return 0;
    }
    return 1;
}

static inline int send_all(
    int fd, const void *src, size_t len)
{
    const unsigned char *p = src;

    while (len)
    {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static inline ssize_t read_line(
    int fd, char *out, size_t cap)
{
    size_t n = 0;

    while (n + 1 < cap)
    {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);

        if (r < 0 && errno == EINTR)
            continue;

        if (r <= 0)
        {
            out[n] = '\0';
            return n ? (ssize_t)n : r;
        }

        if (c == '\n')
            break;

        if (c != '\r')
            out[n++] = c;
    }

    out[n] = '\0';
    return (ssize_t)n;
}

static inline int connect_to(
    const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr,
                sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

static inline int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr,
             sizeof(addr)) ||
        listen(fd, 32))
    {
        close(fd);
        return -1;
    }

    return fd;
}

static inline char *load_text(
    const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) ||
        ftell(f) < 0 ||
        ftell(f) > TRACKER_MAX)
    {
        fclose(f);
        return NULL;
    }

    *bytes = (size_t)ftell(f);
    rewind(f);

    char *s = malloc(*bytes + 1);
    if (!s)
    {
        fclose(f);
        return NULL;
    }

    if (fread(s, 1, *bytes, f) != *bytes)
    {
        free(s);
        fclose(f);
        return NULL;
    }

    s[*bytes] = 0;
    fclose(f);
    return s;
}

static inline int md5_data(
    const void *data, size_t bytes, char hex[33])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;

    if (!ctx)
        return -1;

    int ok =
        EVP_DigestInit_ex(ctx, EVP_md5(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, data, bytes) == 1 &&
        EVP_DigestFinal_ex(ctx, md, &n) == 1;

    EVP_MD_CTX_free(ctx);

    if (!ok || n != 16)
        return -1;

    for (unsigned i = 0; i < n; ++i)
        sprintf(hex + 2 * i, "%02x", md[i]);

    hex[32] = '\0';
    return 0;
}

static inline int md5_file(
    const char *path, char hex[33])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        fclose(f);
        return -1;
    }

    unsigned char buf[8192];
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int outn = 0;

    int ok =
        EVP_DigestInit_ex(ctx, EVP_md5(), NULL) == 1;

    size_t n;

    while (ok &&
           (n = fread(buf, 1, sizeof(buf), f)) > 0)
        ok = EVP_DigestUpdate(ctx, buf, n) == 1;

    if (ferror(f))
        ok = 0;

    if (ok)
        ok = EVP_DigestFinal_ex(
                 ctx, md, &outn) == 1;

    EVP_MD_CTX_free(ctx);
    fclose(f);

    if (!ok || outn != 16)
        return -1;

    for (unsigned i = 0; i < outn; ++i)
        sprintf(hex + 2 * i, "%02x", md[i]);

    hex[32] = '\0';
    return 0;
}

#endif
