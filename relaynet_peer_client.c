
/* RelayNet peer: multithreaded P2P file transfer. */
#include "relaynet_common.h"
#include <dirent.h>
#include <strings.h>

static char tracker_ip[64] = "127.0.0.1";
static int tracker_port = 3490;
static int peer_port = 4101;
static int refresh = 900;
static char shared_dir[256] = "shared";
static char cache_dir[256] = "cache";

typedef struct {
    char ip[64];
    int port;
    long long first, last, stamp;
} remote_peer_t;

typedef struct {
    char name[180], md5[33];
    long long bytes;
    remote_peer_t peers[RN_MAX_PEERS];
    int peer_count;
} tracker_t;

typedef struct {
    int sock;
} client_t;

typedef struct {
    int index, count, dst, errors;
    tracker_t *tracker;
} downloader_t;

static int command_tracker(
    const char *request, char **out, size_t *size
) {
    int fd = rn_connect(tracker_ip, tracker_port);
    if (fd < 0) {
        perror("connect to tracker");
        return -1;
    }

    if (rn_sendall(fd, request, strlen(request))) {
        perror("send tracker");
        close(fd);
        return -1;
    }

    shutdown(fd, SHUT_WR);

    size_t cap = 8192, used = 0;
    char *buffer = malloc(cap);
    if (!buffer) {
        close(fd);
        return -1;
    }

    for (;;) {
        if (used + 4096 + 1 > cap) {
            cap *= 2;
            if (cap > RN_MAX_TRACKER * 2) {
                free(buffer);
                close(fd);
                return -1;
            }
            char *newbuf = realloc(buffer, cap);
            if (!newbuf) {
                free(buffer);
                close(fd);
                return -1;
            }
            buffer = newbuf;
        }

        ssize_t n = recv(fd, buffer + used,
                         cap - used - 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            free(buffer);
            close(fd);
            return -1;
        }
        if (n == 0) break;
        used += (size_t)n;
    }

    close(fd);
    buffer[used] = 0;
    *out = buffer;
    *size = used;
    return 0;
}

static void derive_ip(char out[64]) {
    snprintf(out, 64, "127.0.0.1");

    int fd = rn_connect(tracker_ip, tracker_port);
    if (fd < 0) return;

    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &n) == 0)
        inet_ntop(AF_INET, &a.sin_addr, out, 64);
    close(fd);
}

static void send_update(
    const char *file, long long start, long long end
) {
    char ip[64], req[RN_LINE];
    derive_ip(ip);
    snprintf(req, sizeof(req),
        "<updatetracker %s %lld %lld %s %d>\n",
        file, start, end, ip, peer_port);

    char *out = NULL;
    size_t n = 0;
    if (!command_tracker(req, &out, &n)) {
        printf("[peer %d] %s", peer_port, out);
        free(out);
    }
}

static int cmd_register(
    const char *name, const char *description
) {
    if (!rn_valid_name(name)) {
        fprintf(stderr,
            "Only safe simple filenames are allowed\n");
        return 1;
    }

    char path[512], md5[33], ip[64], req[RN_LINE];
    snprintf(path, sizeof(path),
             "%s/%s", shared_dir, name);

    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) ||
        rn_md5_file(path, md5)) {
        fprintf(stderr,
            "File not found in %s/ or MD5 error: %s\n",
            shared_dir, name);
        return 1;
    }

    derive_ip(ip);
    snprintf(req, sizeof(req),
        "<createtracker %s %lld %s %s %s %d>\n",
        name, (long long)st.st_size,
        description, md5, ip, peer_port);

    char *out = NULL;
    size_t n = 0;
    if (command_tracker(req, &out, &n)) return 1;

    printf("[peer %d] %s", peer_port, out);
    int ok = strstr(out, "succ") != NULL;
    free(out);
    return ok ? 0 : 1;
}

static int cmd_list(void) {
    char *out = NULL;
    size_t n = 0;
    if (command_tracker("<REQ LIST>\n", &out, &n))
        return 1;

    printf("[peer %d] tracker LIST response:\n%s",
           peer_port, out);
    free(out);
    return 0;
}

static int fetch_tracker(const char *name, tracker_t *t) {
    if (!rn_valid_name(name)) return 1;

    char request[256];
    snprintf(request, sizeof(request),
             "<GET %s.track>\n", name);

    char *full = NULL;
    size_t n = 0;
    if (command_tracker(request, &full, &n))
        return 1;

    const char *start_tag = "<REP GET BEGIN>\n";
    const char *end_tag = "<REP GET END ";
    char *begin = strstr(full, start_tag);
    char *end = strstr(full, end_tag);

    if (!begin || !end) {
        printf("[peer %d] tracker GET failed: %s",
               peer_port, full);
        free(full);
        return 1;
    }

    begin += strlen(start_tag);
    size_t bytes = (size_t)(end - begin);
    char received_md5[33], computed_md5[33];

    if (sscanf(end + strlen(end_tag),
               "%32[0-9a-f]", received_md5) != 1 ||
        rn_md5_bytes((unsigned char *)begin,
                     bytes, computed_md5) ||
        strcmp(received_md5, computed_md5)) {
        fprintf(stderr,
                "Tracker content checksum mismatch\n");
        free(full);
        return 1;
    }

    memset(t, 0, sizeof(*t));
    char *raw = strndup(begin, bytes);
    if (!raw) {
        free(full);
        return 1;
    }

    char *save = NULL;
    char *line = strtok_r(raw, "\n", &save);

    while (line) {
        if (!strncmp(line, "Filename: ", 10))
            sscanf(line + 10, "%179s", t->name);
        else if (!strncmp(line, "Filesize: ", 10))
            sscanf(line + 10, "%lld", &t->bytes);
        else if (!strncmp(line, "MD5: ", 5))
            sscanf(line + 5, "%32s", t->md5);
        else if (line[0] != '#' &&
                 strchr(line, ':') &&
                 t->peer_count < RN_MAX_PEERS) {
            remote_peer_t p = {0};
            if (sscanf(line,
                "%63[^:]:%d:%lld:%lld:%lld",
                p.ip, &p.port, &p.first,
                &p.last, &p.stamp) == 5 &&
                p.port >= 1 && p.port <= 65535)
                t->peers[t->peer_count++] = p;
        }
        line = strtok_r(NULL, "\n", &save);
    }

    free(raw);

    if (strcmp(t->name, name) ||
        strlen(t->md5) != 32 || t->bytes < 0) {
        free(full);
        return 1;
    }

    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path),
             "%s/%s.track", cache_dir, name);

    mkdir(cache_dir, 0755);
    rn_writefile(cache_path, begin, bytes);

    printf("[peer %d] received verified tracker "
           "for %s (%lld bytes, %d seeds)\n",
           peer_port, t->name, t->bytes,
           t->peer_count);

    free(full);
    return 0;
}

static void *serve_client(void *arg) {
    client_t *c = arg;
    int fd = c->sock;
    free(c);

    char line[RN_LINE], name[180];
    long long offset;
    size_t bytes;

    if (rn_readline(fd, line, sizeof(line)) <= 0) {
        close(fd);
        return NULL;
    }

    if (sscanf(line, "GETCHUNK %179s %lld %zu",
               name, &offset, &bytes) != 3 ||
        !rn_valid_name(name) ||
        offset < 0 || bytes == 0 ||
        bytes > RN_CHUNK) {
        rn_sendall(fd, "<GET invalid>\n", 14);
        close(fd);
        return NULL;
    }

    char path[512];
    snprintf(path, sizeof(path),
             "%s/%s", shared_dir, name);

    FILE *f = fopen(path, "rb");
    if (!f || fseeko(f, (off_t)offset, SEEK_SET)) {
        rn_sendall(fd, "<GET missing>\n", 14);
        if (f) fclose(f);
        close(fd);
        return NULL;
    }

    unsigned char buf[RN_CHUNK];
    size_t got = fread(buf, 1, bytes, f);
    fclose(f);

    if (got != bytes) {
        rn_sendall(fd, "<GET missing>\n", 14);
        close(fd);
        return NULL;
    }

    char header[64];
    snprintf(header, sizeof(header), "DATA %zu\n", got);

    if (!rn_sendall(fd, header, strlen(header)))
        rn_sendall(fd, buf, got);

    close(fd);
    return NULL;
}

static void *periodic_update(void *ignored) {
    (void)ignored;

    for (;;) {
        sleep((unsigned int)refresh);
        DIR *d = opendir(shared_dir);
        if (!d) continue;

        struct dirent *entry;
        while ((entry = readdir(d))) {
            if (!rn_valid_name(entry->d_name))
                continue;

            char path[512];
            snprintf(path, sizeof(path),
                     "%s/%s", shared_dir,
                     entry->d_name);

            struct stat st;
            if (!stat(path, &st) && S_ISREG(st.st_mode))
                send_update(entry->d_name, 0,
                            (long long)st.st_size);
        }

        closedir(d);
    }
    return NULL;
}

static int cmd_serve(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               &yes, sizeof(yes));

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)peer_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&a,
             sizeof(a)) ||
        listen(fd, 32)) {
        perror("peer bind/listen");
        close(fd);
        return 1;
    }

    pthread_t updater;
    if (!pthread_create(&updater, NULL,
                        periodic_update, NULL))
        pthread_detach(updater);

    printf("[peer %d] multithreaded peer file server "
           "listening at 0.0.0.0:%d, folder %s/\n",
           peer_port, peer_port, shared_dir);

    for (;;) {
        client_t *c = malloc(sizeof(*c));
        if (!c) break;

        c->sock = accept(fd, NULL, NULL);
        if (c->sock < 0) {
            free(c);
            if (errno == EINTR) continue;
            perror("peer accept");
            continue;
        }

        pthread_t worker;
        if (pthread_create(&worker, NULL,
                           serve_client, c)) {
            close(c->sock);
            free(c);
            continue;
        }

        pthread_detach(worker);
    }

    close(fd);
    return 0;
}

static void *download_worker(void *arg) {
    downloader_t *w = arg;
    tracker_t *t = w->tracker;

    printf("[peer %d] worker %d started (thread %lu)\n",
           peer_port, w->index,
           (unsigned long)pthread_self());

    int completed = 0;

    for (long long chunk = w->index;
         chunk * RN_CHUNK < t->bytes;
         chunk += w->count) {

        long long offset = chunk * RN_CHUNK;
        size_t bytes = (size_t)(
            (t->bytes - offset) > RN_CHUNK ?
            RN_CHUNK : (t->bytes - offset));

        int done = 0;

        for (int attempt = 0;
             attempt < t->peer_count && !done;
             ++attempt) {

            remote_peer_t *p =
                &t->peers[(chunk + attempt)
                % t->peer_count];

            if (p->first > offset ||
                p->last < offset + (long long)bytes)
                continue;

            int s = rn_connect(p->ip, p->port);
            if (s < 0) continue;

            char req[512], header[64];
            snprintf(req, sizeof(req),
                     "GETCHUNK %s %lld %zu\n",
                     t->name, offset, bytes);

            if (!rn_sendall(s, req, strlen(req)) &&
                rn_readline(s, header,
                            sizeof(header)) > 0) {

                size_t expected;
                if (sscanf(header, "DATA %zu",
                           &expected) == 1 &&
                    expected == bytes) {

                    unsigned char buf[RN_CHUNK];
                    size_t received = 0;

                    while (received < bytes) {
                        ssize_t n = recv(
                            s, buf + received,
                            bytes - received, 0);

                        if (n < 0 && errno == EINTR)
                            continue;
                        if (n <= 0) break;

                        received += (size_t)n;
                    }

                    if (received == bytes &&
                        pwrite(w->dst, buf, bytes,
                               (off_t)offset) ==
                        (ssize_t)bytes)
                        done = 1;
                }
            }
            close(s);
        }

        if (!done) {
            fprintf(stderr,
                "[worker %d] chunk %lld transfer failed\n",
                w->index, chunk);
            w->errors++;
        } else {
            completed++;
        }
    }

    printf("[peer %d] worker %d finished; "
           "chunks received: %d, errors: %d\n",
           peer_port, w->index,
           completed, w->errors);

    return NULL;
}

static int cmd_download(const char *name) {
    tracker_t t;
    if (fetch_tracker(name, &t))
        return 1;

    if (!t.peer_count) {
        fprintf(stderr, "Tracker has no seeds\n");
        return 1;
    }

    char path[512];
    snprintf(path, sizeof(path),
             "%s/%s", shared_dir, name);

    int fd = open(path,
                  O_CREAT | O_RDWR | O_TRUNC, 0644);

    if (fd < 0 || ftruncate(fd, (off_t)t.bytes)) {
        perror("file output");
        if (fd >= 0) close(fd);
        return 1;
    }

    enum { THREADS = 4 };
    pthread_t ids[THREADS];
    downloader_t workers[THREADS];

    printf("[peer %d] starting %d concurrent "
           "download workers, max chunk size %d bytes\n",
           peer_port, THREADS, RN_CHUNK);

    int errors = 0, started = 0;

    for (int i = 0; i < THREADS; ++i) {
        workers[i] = (downloader_t){
            .index = i,
            .count = THREADS,
            .dst = fd,
            .errors = 0,
            .tracker = &t
        };

        if (pthread_create(
                &ids[i], NULL,
                download_worker, &workers[i])) {
            errors++;
            break;
        }
        started++;
    }

    for (int i = 0; i < started; ++i) {
        pthread_join(ids[i], NULL);
        errors += workers[i].errors;
    }

    close(fd);

    char actual_md5[33];
    if (errors ||
        rn_md5_file(path, actual_md5) ||
        strcmp(t.md5, actual_md5)) {
        fprintf(stderr,
            "[peer %d] download incomplete or "
            "MD5 mismatch; %d failed chunks\n",
            peer_port, errors);
        return 1;
    }

    printf("[peer %d] download complete: "
           "%s, MD5 verified %s\n",
           peer_port, name, actual_md5);

    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path),
             "%s/%s.track", cache_dir, name);

    unlink(cache_path);
    send_update(name, 0, t.bytes);
    return 0;
}

static int load_config(void) {
    FILE *f = fopen("clientThreadConfig.cfg", "r");

    if (!f || fscanf(f, "%d %63s %d",
                     &tracker_port,
                     tracker_ip, &refresh) != 3) {
        fprintf(stderr,
            "Missing or invalid clientThreadConfig.cfg\n");
        if (f) fclose(f);
        return 1;
    }
    fclose(f);

    f = fopen("serverThreadConfig.cfg", "r");

    if (!f || fscanf(f, "%d %255s",
                     &peer_port, shared_dir) != 2) {
        fprintf(stderr,
            "Missing or invalid serverThreadConfig.cfg\n");
        if (f) fclose(f);
        return 1;
    }
    fclose(f);

    if (tracker_port < 1 || tracker_port > 65535 ||
        peer_port < 1 || peer_port > 65535 ||
        refresh < 1)
        return 1;

    mkdir(shared_dir, 0755);
    mkdir(cache_dir, 0755);
    return 0;
}

static void usage(void) {
    puts("Usage: ./peer serve | list | "
         "register <filename> [description] | "
         "update <filename> <start> <end> | "
         "get <filename.track> | "
         "download <filename>");

    puts("Run from the peer1/, peer2/, "
         "or peer3/ folder.");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (load_config()) return 1;
    if (argc < 2) {
        usage();
        return 1;
    }

    if (!strcmp(argv[1], "serve"))
        return cmd_serve();

    if (!strcmp(argv[1], "list"))
        return cmd_list();

    if (!strcmp(argv[1], "register") && argc >= 3)
        return cmd_register(
            argv[2],
            argc >= 4 ? argv[3] : "shared_file");

    if (!strcmp(argv[1], "update") && argc == 5) {
        send_update(
            argv[2], atoll(argv[3]), atoll(argv[4]));
        return 0;
    }

    if (!strcmp(argv[1], "get") && argc == 3) {
        char name[180];
        snprintf(name, sizeof(name), "%s", argv[2]);

        if (strlen(name) > 6 &&
            !strcmp(name + strlen(name)-6, ".track"))
            name[strlen(name)-6] = 0;

        tracker_t t;
        return fetch_tracker(name, &t);
    }

    if (!strcmp(argv[1], "download") && argc == 3)
        return cmd_download(argv[2]);

    usage();
    return 1;
}
