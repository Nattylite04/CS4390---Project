
/* RelayNet tracker: multithreaded TCP server. */
#include "relaynet_common.h"
#include <dirent.h>
#include <strings.h>

static char tracker_dir[256] = "torrents";
static int tracker_port = 3490;
static pthread_mutex_t tracker_mutex =
    PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int fd;
    struct sockaddr_in remote;
} connection_t;

static void reply(int fd, const char *s) {
    rn_sendall(fd, s, strlen(s));
}

static void tracker_path(
    char *dst, size_t n, const char *name
) {
    snprintf(dst, n, "%s/%s.track", tracker_dir, name);
}

static void on_create(int fd, char *args) {
    char name[180], desc[256], md5[33], ip[64];
    long long bytes;
    int port;

    if (sscanf(args, "%179s %lld %255s %32s %63s %d",
               name, &bytes, desc, md5, ip, &port) != 6 ||
        !rn_valid_name(name) || bytes < 0 ||
        port < 1 || port > 65535 || strlen(md5) != 32) {
        reply(fd, "<createtracker fail>\n");
        return;
    }

    char path[512];
    tracker_path(path, sizeof(path), name);
    pthread_mutex_lock(&tracker_mutex);

    if (access(path, F_OK) == 0) {
        reply(fd, "<createtracker ferr>\n");
        pthread_mutex_unlock(&tracker_mutex);
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        reply(fd, "<createtracker fail>\n");
        pthread_mutex_unlock(&tracker_mutex);
        return;
    }

    fprintf(f,
        "Filename: %s\n"
        "Filesize: %lld\n"
        "Description: %s\n"
        "MD5: %s\n"
        "# list of peers follows\n"
        "%s:%d:0:%lld:%lld\n",
        name, bytes, desc, md5,
        ip, port, bytes, (long long)time(NULL));

    fclose(f);
    pthread_mutex_unlock(&tracker_mutex);

    printf("[tracker] created %s.track, seed %s:%d\n",
           name, ip, port);
    reply(fd, "<createtracker succ>\n");
}

static void on_update(int fd, char *args) {
    char name[180], ip[64];
    long long start, end;
    int port;

    if (sscanf(args, "%179s %lld %lld %63s %d",
               name, &start, &end, ip, &port) != 5 ||
        !rn_valid_name(name) || start < 0 ||
        end < start || port < 1 || port > 65535) {
        reply(fd, "<updatetracker invalid fail>\n");
        return;
    }

    char path[512];
    tracker_path(path, sizeof(path), name);
    pthread_mutex_lock(&tracker_mutex);

    size_t size = 0;
    char *raw = rn_readfile(path, &size);

    if (!raw) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "<updatetracker %s ferr>\n", name);
        reply(fd, msg);
        pthread_mutex_unlock(&tracker_mutex);
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        free(raw);
        pthread_mutex_unlock(&tracker_mutex);
        reply(fd, "<updatetracker invalid fail>\n");
        return;
    }

    char *save = NULL;
    char *line = strtok_r(raw, "\n", &save);

    while (line) {
        char oldip[64];
        int oldport = 0;
        if (sscanf(line, "%63[^:]:%d:",
                   oldip, &oldport) != 2 ||
            strcmp(oldip, ip) || oldport != port)
            fprintf(f, "%s\n", line);

        line = strtok_r(NULL, "\n", &save);
    }

    fprintf(f, "%s:%d:%lld:%lld:%lld\n",
            ip, port, start, end,
            (long long)time(NULL));

    fclose(f);
    free(raw);
    pthread_mutex_unlock(&tracker_mutex);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "<updatetracker %s succ>\n", name);
    reply(fd, msg);
}

static void on_list(int fd) {
    pthread_mutex_lock(&tracker_mutex);
    DIR *d = opendir(tracker_dir);

    if (!d) {
        reply(fd, "<REP LIST 0>\n<REP LIST END>\n");
        pthread_mutex_unlock(&tracker_mutex);
        return;
    }

    typedef struct {
        char name[180], md5[33];
        long long size;
    } entry_t;

    entry_t items[512];
    int count = 0;
    struct dirent *e;

    while ((e = readdir(d)) && count < 512) {
        size_t n = strlen(e->d_name);
        if (n <= 6 ||
            strcmp(e->d_name + n - 6, ".track"))
            continue;

        char path[512];
        snprintf(path, sizeof(path),
                 "%s/%s", tracker_dir, e->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[RN_LINE];
        entry_t entry = {0};

        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "Filename: ", 10))
                sscanf(line + 10, "%179s", entry.name);
            if (!strncmp(line, "Filesize: ", 10))
                sscanf(line + 10, "%lld", &entry.size);
            if (!strncmp(line, "MD5: ", 5))
                sscanf(line + 5, "%32s", entry.md5);
        }

        fclose(f);
        if (rn_valid_name(entry.name) &&
            strlen(entry.md5) == 32)
            items[count++] = entry;
    }

    closedir(d);

    char out[RN_LINE];
    snprintf(out, sizeof(out),
             "<REP LIST %d>\n", count);
    reply(fd, out);

    for (int i = 0; i < count; ++i) {
        snprintf(out, sizeof(out),
            "<%d %.160s %lld %.32s>\n",
            i + 1, items[i].name,
            items[i].size, items[i].md5);
        reply(fd, out);
    }

    reply(fd, "<REP LIST END>\n");
    pthread_mutex_unlock(&tracker_mutex);
}

static void on_get(int fd, char *args) {
    char name[180];
    if (sscanf(args, "%179s", name) != 1 ||
        strlen(name) <= 6 ||
        strcmp(name + strlen(name) - 6, ".track")) {
        reply(fd, "<REP GET ERROR>\n");
        return;
    }

    name[strlen(name) - 6] = 0;
    if (!rn_valid_name(name)) {
        reply(fd, "<REP GET ERROR>\n");
        return;
    }

    char path[512];
    tracker_path(path, sizeof(path), name);

    pthread_mutex_lock(&tracker_mutex);
    size_t size = 0;
    char *raw = rn_readfile(path, &size);
    pthread_mutex_unlock(&tracker_mutex);

    if (!raw) {
        reply(fd, "<REP GET ERROR>\n");
        return;
    }

    char md5[33], end[128];
    rn_md5_bytes((const unsigned char *)raw, size, md5);

    reply(fd, "<REP GET BEGIN>\n");
    rn_sendall(fd, raw, size);
    snprintf(end, sizeof(end),
             "<REP GET END %s>\n", md5);
    reply(fd, end);
    free(raw);
}

static void *handle_client(void *arg) {
    connection_t *c = arg;
    int fd = c->fd;
    char peerip[INET_ADDRSTRLEN] = "?";

    inet_ntop(AF_INET, &c->remote.sin_addr,
              peerip, sizeof(peerip));
    free(c);

    char line[RN_LINE];
    ssize_t n = rn_readline(fd, line, sizeof(line));

    if (n > 0) {
        rn_trim(line);

        if (line[0] == '<') {
            size_t k = strlen(line);
            if (k && line[k-1] == '>')
                line[k-1] = 0;
            memmove(line, line + 1, strlen(line));
        }

        printf("[tracker] request from %s: %.130s\n",
               peerip, line);

        if (!strcasecmp(line, "REQ LIST"))
            on_list(fd);
        else if (!strncasecmp(line, "GET ", 4))
            on_get(fd, line + 4);
        else if (!strncasecmp(line, "createtracker ", 14))
            on_create(fd, line + 14);
        else if (!strncasecmp(line, "updatetracker ", 14))
            on_update(fd, line + 14);
        else
            reply(fd, "<ERR UNKNOWN>\n");
    }

    close(fd);
    return NULL;
}

static void read_sconfig(void) {
    FILE *f = fopen("sconfig", "r");
    if (!f) {
        fprintf(stderr,
                "Missing sconfig in current directory\n");
        exit(1);
    }

    if (fscanf(f, "%d %255s",
               &tracker_port, tracker_dir) != 2 ||
        tracker_port < 1 || tracker_port > 65535) {
        fprintf(stderr,
                "Invalid sconfig; expected port and folder\n");
        exit(1);
    }

    fclose(f);
    if (mkdir(tracker_dir, 0755) &&
        errno != EEXIST) {
        perror("mkdir");
        exit(1);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    read_sconfig();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET,
               SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)tracker_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd,
             (struct sockaddr *)&addr,
             sizeof(addr)) ||
        listen(listen_fd, 32)) {
        perror("bind/listen");
        close(listen_fd);
        return 1;
    }

    printf("[tracker] multithreaded server listening "
           "at 0.0.0.0:%d; files in %s/\n",
           tracker_port, tracker_dir);

    for (;;) {
        connection_t *c = malloc(sizeof(*c));
        if (!c) break;

        socklen_t sz = sizeof(c->remote);
        c->fd = accept(listen_fd,
                       (struct sockaddr *)&c->remote,
                       &sz);

        if (c->fd < 0) {
            free(c);
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL,
                           handle_client, c)) {
            close(c->fd);
            free(c);
            continue;
        }

        pthread_detach(tid);
    }

    close(listen_fd);
    return 0;
}
