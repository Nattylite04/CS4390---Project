#ifndef NETWORK_PROTOCOL_H
#define NETWORK_PROTOCOL_H
#define _GNU_SOURCE
/* Enable GNU/POSIX extensions*/
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
// Validates a filename to prevent directory traversal and illegal characters
static inline int good_name(const char *s)
{
    // Reject null pointers, empty strings, overly long names, and parent traversal 
    if (!s || !*s || strlen(s) > 150 || strstr(s, ".."))
        return 0;
    // Verify each character against the safe whitelist
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
// Reliably transmits an entire buffer over a stream socket
static inline int send_all(
    int fd, const void *src, size_t len)
{
    const unsigned char *p = src;

    while (len)
    {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        // Interrupted by signal handler; retry without penalty 
        if (n < 0 && errno == EINTR)
            continue;
        // n == 0 or negative error denotes broken pipe / peer disconnection 
        if (n <= 0)
            return -1;

        p += n;
        len -= (size_t)n;
    }
    return 0;
}
// Reads a single newline-delimited line from a socket 
static inline ssize_t read_line(
    int fd, char *out, size_t cap)
{
    size_t n = 0;
    // Reserve the final slot for the null terminator
    while (n + 1 < cap)
    {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        // Interrupted by system signal; re-attempt read 
        if (r < 0 && errno == EINTR)
            continue;
        // Peer closed socket (0) or fatal read error (-1) 
        if (r <= 0)
        {
            out[n] = '\0';
            return n ? (ssize_t)n : r;
        }
        // End-of-line marker detected; break out to finalize 
        if (c == '\n')
            break;
        // Strip carriage return characters from CRLF line endings 
        if (c != '\r')
            out[n++] = c;
    }

    out[n] = '\0';
    return (ssize_t)n;
}
// Creates an outbound IPv4 TCP socket and connects to a target host/port
static inline int connect_to(
    const char *ip, int port)
{
    // Initialize TCP stream socket over IPv4
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    // Convert IP string to network binary format, then initiate handshake 
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr,
                sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}
// Initializes a TCP server listening socket bound to all local interfaces
static inline int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    // Allow fast rebinding of local address during restarts
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // Listen on all network interfaces
    // Bind address to socket and start backlog queue (max 32 pending connections) 
    if (bind(fd, (struct sockaddr *)&addr,
             sizeof(addr)) ||
        listen(fd, 32))
    {
        close(fd);
        return -1;
    }

    return fd;
}
// Reads an entire text/config/tracker file from disk into a heap-allocated buffer
static inline char *load_text(
    const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    // Seek to the end to determine file length and check safety boundaries
    if (fseek(f, 0, SEEK_END) ||
        ftell(f) < 0 ||
        ftell(f) > TRACKER_MAX)
    {
        fclose(f);
        return NULL;
    }

    *bytes = (size_t)ftell(f);
    rewind(f);
    // Allocate memory for file data plus a null terminator
    char *s = malloc(*bytes + 1);
    if (!s)
    {
        fclose(f);
        return NULL;
    }
    // Read file contents into heap buffer
    if (fread(s, 1, *bytes, f) != *bytes)
    {
        free(s);
        fclose(f);
        return NULL;
    }

    s[*bytes] = 0;    // Null-terminate string
    fclose(f);
    return s;
}
// Computes the 128-bit MD5 checksum of an in-memory byte sequence
static inline int md5_data(
    const void *data, size_t bytes, char hex[33])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;

    if (!ctx)
        return -1;
    // Execute the EVP hash lifecycle: Init -> Update -> Final
    int ok =
        EVP_DigestInit_ex(ctx, EVP_md5(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, data, bytes) == 1 &&
        EVP_DigestFinal_ex(ctx, md, &n) == 1;

    EVP_MD_CTX_free(ctx);
    // Verify MD5 digest operation produced exactly 16 bytes (128 bits)
    if (!ok || n != 16)
        return -1;
    // Convert 16 binary bytes into a 32-character hexadecimal string 
    for (unsigned i = 0; i < n; ++i)
        sprintf(hex + 2 * i, "%02x", md[i]);

    hex[32] = '\0';
    return 0;
}
// Computes the 128-bit MD5 checksum of an entire file on disk
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
    // Stream the file in 8 KiB chunks into the running digest
    while (ok &&
           (n = fread(buf, 1, sizeof(buf), f)) > 0)
        ok = EVP_DigestUpdate(ctx, buf, n) == 1;
    // Check for stream read errors
    if (ferror(f))
        ok = 0;
    // Finalize the hash computation 
    if (ok)
        ok = EVP_DigestFinal_ex(
                 ctx, md, &outn) == 1;

    EVP_MD_CTX_free(ctx);
    fclose(f);
    // Confirm 16-byte digest length
    if (!ok || outn != 16)
        return -1;
    // Convert binary digest to a null-terminated 32-character hex string 
    for (unsigned i = 0; i < outn; ++i)
        sprintf(hex + 2 * i, "%02x", md[i]);

    hex[32] = '\0';
    return 0;
}

#endif // NETWORK_PROTOCOL_H 
