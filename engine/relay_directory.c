
#include "network_protocol.h"

static char storage[256] = "torrents";
static int port = 3490;

// how long (seconds) a peer can go without sending updatetracker before we
// treat it as dead. read from the 3rd value in sconfig, 15 min if it's not there
static long long peer_timeout = 900;

static pthread_mutex_t file_lock =
    PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
    int fd;
    char who[64]; // "ip:port" of the peer that connected, just for printing
} client_job;

static void answer(int fd, const char *text)
{
    send_all(fd, text, strlen(text));
}

static void file_path(
    char out[512], const char *name)
{
    snprintf(out, 512, "%s/%s.track", storage, name);
}

static void create_record(int fd, const char *arg)
{
    char name[160], desc[160], hash[33], ip[64];
    long long size;
    int peer_port;

    if (sscanf(arg, "%159s %lld %159s %32s %63s %d",
               name, &size, desc, hash, ip,
               &peer_port) != 6 ||
        !good_name(name) ||
        size < 0 ||
        strlen(hash) != 32 ||
        peer_port < 1 || peer_port > 65535)
    {
        answer(fd, "<createtracker fail>\n");
        return;
    }

    struct in_addr dummy;
    if (inet_pton(AF_INET, ip, &dummy) != 1)
    {
        answer(fd, "<createtracker fail>\n");
        return;
    }

    char path[512];
    file_path(path, name);

    pthread_mutex_lock(&file_lock);

    int file = open(
        path, O_WRONLY | O_CREAT | O_EXCL, 0644);

    if (file < 0)
    {
        int already_exists = errno == EEXIST;
        pthread_mutex_unlock(&file_lock);

        answer(fd, already_exists ? "<createtracker ferr>\n" : "<createtracker fail>\n");
        return;
    }

    FILE *f = fdopen(file, "w");

    if (!f)
    {
        close(file);
        unlink(path);
        pthread_mutex_unlock(&file_lock);
        answer(fd, "<createtracker fail>\n");
        return;
    }

    fprintf(f,
            "Filename: %s\n"
            "Filesize: %lld\n"
            "Description: %s\n"
            "MD5: %s\n"
            "# peer IP:port:start:end:timestamp\n"
            "%s:%d:0:%lld:%lld\n",
            name, size, desc, hash,
            ip, peer_port, size,
            (long long)time(NULL));

    int ok = fclose(f) == 0;
    pthread_mutex_unlock(&file_lock);

    answer(fd, ok ? "<createtracker succ>\n" : "<createtracker fail>\n");

    printf("[tracker] CREATE %s from %s:%d\n",
           name, ip, peer_port);
}

static void update_record(
    int fd, const char *arg)
{
    char name[160], ip[64], response[240];
    long long start, end;
    int peer_port;

    if (sscanf(arg, "%159s %lld %lld %63s %d",
               name, &start, &end, ip,
               &peer_port) != 5 ||
        !good_name(name) ||
        start < 0 || end < start ||
        peer_port < 1 || peer_port > 65535)
    {
        // spec wants <updatetracker filename fail>, so try to at least grab
        // the filename out of the message even if the rest of it is junk
        char first[160] = "";
        sscanf(arg, "%159s", first);
        snprintf(response, sizeof(response),
                 "<updatetracker %s fail>\n",
                 good_name(first) ? first : "unknown");
        answer(fd, response);
        return;
    }

    char path[512];
    file_path(path, name);

    pthread_mutex_lock(&file_lock);

    size_t bytes = 0;
    char *raw = load_text(path, &bytes);

    if (!raw)
    {
        pthread_mutex_unlock(&file_lock);
        snprintf(response, sizeof(response),
                 "<updatetracker %s ferr>\n", name);
        answer(fd, response);
        return;
    }

    FILE *f = fopen(path, "w");

    if (!f)
    {
        pthread_mutex_unlock(&file_lock);
        free(raw);
        snprintf(response, sizeof(response),
                 "<updatetracker %s fail>\n", name);
        answer(fd, response);
        return;
    }

    char *save = NULL;
    long long now = (long long)time(NULL);
    int removed = 0;

    for (char *line = strtok_r(raw, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save))
    {

        char old_ip[64];
        int old_port;
        long long old_start, old_end, old_time;

        // header lines (Filename:, MD5:, comments) don't match this pattern,
        // only the ip:port:start:end:timestamp peer lines do
        int fields = sscanf(line, "%63[^:]:%d:%lld:%lld:%lld",
                            old_ip, &old_port,
                            &old_start, &old_end, &old_time);

        // same peer as the one updating -> drop the old line, we write a
        // fresh one with the new byte range and timestamp below
        if (fields >= 2 &&
            !strcmp(ip, old_ip) &&
            peer_port == old_port)
            continue;

        // peer hasn't checked in within the timeout, so it's dead. leave it
        // out so nobody tries to download from it anymore
        if (fields == 5 && now - old_time > peer_timeout)
        {
            printf("[tracker] removing dead peer %s:%d from %s.track "
                   "(no update for %lld sec)\n",
                   old_ip, old_port, name, now - old_time);
            removed++;
            continue;
        }

        fprintf(f, "%s\n", line);
    }

    fprintf(f, "%s:%d:%lld:%lld:%lld\n",
            ip, peer_port,
            start, end, now);

    int ok = fclose(f) == 0;
    free(raw);

    pthread_mutex_unlock(&file_lock);

    snprintf(response, sizeof(response),
             "<updatetracker %s %s>\n",
             name, ok ? "succ" : "fail");

    answer(fd, response);

    printf("[tracker] UPDATE %s from %s:%d (bytes %lld-%lld)%s\n",
           name, ip, peer_port, start, end,
           removed ? ", cleaned up dead peers" : "");
}

static void list_records(int fd)
{
    typedef struct
    {
        char name[160], hash[33];
        long long size;
    } item;

    item records[128] = {0};
    int count = 0;

    pthread_mutex_lock(&file_lock);
    DIR *dir = opendir(storage);

    if (dir)
    {
        struct dirent *d;

        while ((d = readdir(dir)) &&
               count < 128)
        {
            size_t n = strlen(d->d_name);

            if (n < 7 ||
                strcmp(d->d_name + n - 6,
                       ".track"))
                continue;

            char path[512];

            snprintf(path, sizeof(path),
                     "%s/%s", storage,
                     d->d_name);

            FILE *f = fopen(path, "r");
            if (!f)
                continue;

            item x = {0};
            char line[LINE_MAX_LEN];

            while (fgets(line, sizeof(line), f))
            {
                if (!strncmp(line, "Filename: ", 10))
                    sscanf(line + 10, "%159s",
                           x.name);

                if (!strncmp(line, "Filesize: ", 10))
                    sscanf(line + 10, "%lld",
                           &x.size);

                if (!strncmp(line, "MD5: ", 5))
                    sscanf(line + 5, "%32s",
                           x.hash);
            }

            fclose(f);

            if (good_name(x.name) &&
                strlen(x.hash) == 32)
                records[count++] = x;
        }

        closedir(dir);
    }

    char line[512];

    snprintf(line, sizeof(line),
             "<REP LIST %d>\n", count);
    answer(fd, line);

    for (int i = 0; i < count; ++i)
    {
        snprintf(line, sizeof(line),
                 "<%d %.150s %lld %.32s>\n",
                 i + 1,
                 records[i].name,
                 records[i].size,
                 records[i].hash);

        answer(fd, line);
    }

    answer(fd, "<REP LIST END>\n");
    pthread_mutex_unlock(&file_lock);

    printf("[tracker] LIST sent %d tracker file(s)\n", count);
}

static void get_record(int fd, const char *arg)
{
    char name[160];

    if (sscanf(arg, "%159s", name) != 1)
    {
        answer(fd, "<REP GET ERROR>\n");
        return;
    }

    size_t n = strlen(name);

    if (n <= 6 ||
        strcmp(name + n - 6, ".track"))
    {
        answer(fd, "<REP GET ERROR>\n");
        return;
    }

    name[n - 6] = 0;

    if (!good_name(name))
    {
        answer(fd, "<REP GET ERROR>\n");
        return;
    }

    char path[512];
    file_path(path, name);

    pthread_mutex_lock(&file_lock);

    size_t bytes = 0;
    char *data = load_text(path, &bytes);

    pthread_mutex_unlock(&file_lock);

    if (!data)
    {
        printf("[tracker] GET %s.track failed, no such tracker file\n", name);
        answer(fd, "<REP GET ERROR>\n");
        return;
    }

    char hash[33], tail[128];

    md5_data(data, bytes, hash);

    answer(fd, "<REP GET BEGIN>\n");
    send_all(fd, data, bytes);

    snprintf(tail, sizeof(tail),
             "<REP GET END %s>\n", hash);

    answer(fd, tail);

    printf("[tracker] GET %s.track\n", name);
    free(data);
}

static void *client_thread(void *arg)
{
    client_job *job = arg;
    int fd = job->fd;
    char who[64];
    snprintf(who, sizeof(who), "%s", job->who);

    free(job);

    char line[LINE_MAX_LEN];

    if (read_line(fd, line, sizeof(line)) > 0)
    {
        // print which peer sent what, and which thread is handling it,
        // so during the demo you can see different threads serving peers
        printf("[tracker] thread %lu got \"%s\" from %s\n",
               (unsigned long)pthread_self(), line, who);

        size_t n = strlen(line);

        if (line[0] == '<' &&
            n >= 2 &&
            line[n - 1] == '>')
        {
            line[n - 1] = 0;

            memmove(line, line + 1,
                    strlen(line));
        }

        if (!strcmp(line, "REQ LIST"))
        {
            list_records(fd);
        }
        else if (!strncmp(line, "GET ", 4))
        {
            get_record(fd, line + 4);
        }
        else if (!strncmp(
                     line, "createtracker ", 14))
        {
            create_record(fd, line + 14);
        }
        else if (!strncmp(
                     line, "updatetracker ", 14))
        {
            update_record(fd, line + 14);
        }
        else
        {
            printf("[tracker] unknown command from %s\n", who);
            answer(fd, "<ERR UNKNOWN COMMAND>\n");
        }
    }

    close(fd);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    FILE *cfg = fopen("sconfig", "r");

    // sconfig is: <port> <tracker folder> [peer timeout in seconds]
    // the timeout is optional, if it's missing we keep the 900 sec default
    if (!cfg ||
        fscanf(cfg, "%d %255s %lld",
               &port, storage, &peer_timeout) < 2)
    {
        fprintf(stderr,
                "Missing/invalid sconfig "
                "(port and directory)\n");

        if (cfg)
            fclose(cfg);

        return 1;
    }

    fclose(cfg);

    if (peer_timeout <= 0)
        peer_timeout = 900;

    if (port < 1 || port > 65535 ||
        (mkdir(storage, 0755) &&
         errno != EEXIST))
    {
        perror("tracker configuration");
        return 1;
    }

    int listener = listen_on(port);

    if (listener < 0)
    {
        perror("tracker listen");
        return 1;
    }

    printf("[tracker] listening on port %d; "
           "directory %s/; dead peer timeout %lld sec\n",
           port, storage, peer_timeout);

    while (1)
    {
        struct sockaddr_in peer_addr;
        socklen_t addr_len = sizeof(peer_addr);
        int fd = accept(listener,
                        (struct sockaddr *)&peer_addr, &addr_len);

        if (fd < 0)
        {
            if (errno == EINTR)
                continue;

            perror("accept");
            continue;
        }

        client_job *job = malloc(sizeof(*job));

        if (!job)
        {
            close(fd);
            continue;
        }

        job->fd = fd;

        // save "ip:port" of whoever connected so the thread can print it
        char ip_text[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer_addr.sin_addr, ip_text, sizeof(ip_text));
        snprintf(job->who, sizeof(job->who), "%s:%d",
                 ip_text, ntohs(peer_addr.sin_port));

        pthread_t thread;

        // every new connection gets its own worker thread, so one slow peer
        // can't hold up the others
        if (pthread_create(
                &thread, NULL,
                client_thread, job) != 0)
        {
            perror("pthread_create");
            close(fd);
            free(job);
            continue;
        }

        pthread_detach(thread);
    }

    close(listener);
    return 0;
}
