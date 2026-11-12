#include "network_protocol.h"

// Global peer configuration settings
static char tracker_ip[64];
static int tracker_port;
static int own_port;
static int refresh_seconds;
static char shared[256];

// Metadata for a single peer hosting a file range
typedef struct
{
    char ip[64];
    int port;
    long long first, last, stamp;
} seed;

// Parsed tracker metadata for a shared file
typedef struct
{
    char name[160], hash[33];
    long long size;
    seed seeds[MAX_SEEDS];
    int count;
} file_info;

// Thread-safe work queue for parallel chunk downloads
typedef struct
{
    file_info *info;
    int output;
    size_t next_chunk, total_chunks;
    int failures;
    pthread_mutex_t queue_lock;
} chunk_queue;

// Arguments passed to each download worker thread
typedef struct
{
    chunk_queue *queue;
    int worker_no;
} worker_arg;

// Socket descriptor wrapper for incoming peer connections
typedef struct
{
    int fd;
} service_job;

// Find the local IP used to reach the tracker.
static void local_ip(char out[64])
{
    const char *lan_ip = getenv("PEER_ADVERTISE_IP");
    if (lan_ip && *lan_ip)
    {
        snprintf(out, 64, "%s", lan_ip);
        return;
    }
    strcpy(out, "127.0.0.1");
    int sock = connect_to(tracker_ip, tracker_port);
    if (sock < 0)
        return;

    // Query the local socket address assigned by the OS
    struct sockaddr_in local;
    socklen_t n = sizeof(local);

    if (getsockname(
            sock, (struct sockaddr *)&local,
            &n) == 0)
    {
        inet_ntop(AF_INET, &local.sin_addr,
                  out, 64);
    }
    close(sock);
}

// Send a command to the tracker and collect its reply.
static int tracker_request(
    const char *request, char **reply)
{
    int fd = connect_to(tracker_ip, tracker_port);
    if (fd < 0)
    {
        perror("connect to tracker");
        return -1;
    }

    // Transmit full request and signal EOF on the write half
    if (send_all(fd, request, strlen(request)))
    {
        close(fd);
        return -1;
    }
    shutdown(fd, SHUT_WR);
    
    size_t used = 0, cap = 8192;
    char *data = malloc(cap);

    if (!data)
    {
        close(fd);
        return -1;
    }

    // Read response dynamically, doubling buffer size as needed
    while (1)
    {
        if (used + 2048 + 1 > cap)
        {
            if (cap >= TRACKER_MAX * 2)
            {
                free(data);
                close(fd);
                return -1;
            }

            cap *= 2;
            char *larger = realloc(data, cap);

            if (!larger)
            {
                free(data);
                close(fd);
                return -1;
            }
            data = larger;
        }
        ssize_t n = recv(
            fd, data + used, cap - used - 1, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n < 0)
        {
            free(data);
            close(fd);
            return -1;
        }
        if (!n)
            break;
        used += (size_t)n;
    }

    data[used] = 0;
    close(fd);

    *reply = data;
    return 0;
}

// Register one complete shared file.
static int command_share(const char *name)
{
    if (!good_name(name))
        return 1;

    char path[512], hash[33];
    char ip[64], req[LINE_MAX_LEN];

    snprintf(path, sizeof(path),
             "%s/%s", shared, name);

    // Verify file exists, is a regular file, and compute its MD5
    struct stat st;

    if (stat(path, &st) ||
        !S_ISREG(st.st_mode) ||
        md5_file(path, hash))
    {

        fprintf(stderr,
                "Cannot share file: %s\n", path);
        return 1;
    }

    local_ip(ip);

    // Format and send the tracker creation message
    snprintf(req, sizeof(req),
             "<createtracker %s %lld class_demo "
             "%s %s %d>\n",
             name, (long long)st.st_size,
             hash, ip, own_port);

    char *reply = NULL;

    if (tracker_request(req, &reply))
        return 1;

    printf("[peer %d] share %s: %s",
           own_port, name, reply);

    int ok =
        strstr(reply, "createtracker succ") != NULL;

    free(reply);
    return ok ? 0 : 1;
}

// Update the byte range available at this peer.
static int command_update(
    const char *name, long long first, long long last)
{
    if (!good_name(name) ||
        first < 0 || last < first)
        return 1;

    char ip[64], req[LINE_MAX_LEN];
    char *reply = NULL;

    local_ip(ip);

    snprintf(req, sizeof(req),
             "<updatetracker %s %lld %lld %s %d>\n",
             name, first, last, ip, own_port);

    if (tracker_request(req, &reply))
        return 1;

    printf("[peer %d] %s", own_port, reply);

    int ok = strstr(reply, " succ>") != NULL;

    free(reply);
    return ok ? 0 : 1;
}

// Ask the tracker for the list of available files.
static int command_list(void)
{
    char *reply = NULL;

    if (tracker_request("<REQ LIST>\n", &reply))
        return 1;

    printf("[peer %d] available tracker files:\n%s",
           own_port, reply);

    free(reply);
    return 0;
}

// Download, verify and parse a tracker file.
static int get_tracker(
    const char *filename, file_info *info)
{
    char name[160];

    if (strlen(filename) >= sizeof(name))
        return 1;

    snprintf(name, sizeof(name), "%s", filename);

    // Strip trailing ".track" extension if provided
    size_t n = strlen(name);

    if (n > 6 &&
        !strcmp(name + n - 6, ".track"))
        name[n - 6] = 0;

    if (!good_name(name))
        return 1;

    char req[256];
    char *response = NULL;

    snprintf(req, sizeof(req),
             "<GET %s.track>\n", name);

    if (tracker_request(req, &response))
        return 1;

    // Locate payload boundaries in the tracker response
    char *begin = strstr(
        response, "<REP GET BEGIN>\n");

    char *end = strstr(
        response, "<REP GET END ");

    if (!begin || !end || end < begin)
    {
        fprintf(stderr,
                "Tracker GET failed: %s\n", response);

        free(response);
        return 1;
    }

    begin += strlen("<REP GET BEGIN>\n");

    size_t bytes = (size_t)(end - begin);

    // Verify integrity of the tracker payload via MD5
    char expected[33], actual[33];

    if (sscanf(end,
               "<REP GET END %32[0-9a-f]>",
               expected) != 1 ||
        md5_data(begin, bytes, actual) ||
        strcmp(expected, actual))
    {

        fprintf(stderr,
                "Tracker checksum failed\n");
        free(response);
        return 1;
    }

    memset(info, 0, sizeof(*info));

    char *contents = strndup(begin, bytes);

    if (!contents)
    {
        free(response);
        return 1;
    }

    // Parse file metadata headers and peer seed entries line by line
    char *save = NULL;

    for (char *line =
             strtok_r(contents, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save))
    {

        if (!strncmp(line, "Filename: ", 10))
        {
            sscanf(line + 10, "%159s",
                   info->name);
        }
        else if (!strncmp(
                     line, "Filesize: ", 10))
        {

            sscanf(line + 10, "%lld",
                   &info->size);
        }
        else if (!strncmp(line, "MD5: ", 5))
        {

            sscanf(line + 5, "%32s",
                   info->hash);
        }
        else if (line[0] != '#' &&
                 strchr(line, ':') &&
                 info->count < MAX_SEEDS)
        {

            seed s = {0};

            if (sscanf(line,
                       "%63[^:]:%d:%lld:%lld:%lld",
                       s.ip, &s.port, &s.first,
                       &s.last, &s.stamp) == 5 &&
                s.port > 0 &&
                s.port <= 65535)
            {

                info->seeds[info->count++] = s;
            }
        }
    }

    free(contents);

    // Ensure required metadata fields are valid
    if (strcmp(info->name, name) ||
        strlen(info->hash) != 32 ||
        info->size < 0)
    {

        free(response);
        return 1;
    }

    // Try seeds with newer timestamps first.
    for (int i = 0; i < info->count; ++i)
    {
        for (int j = i + 1;
             j < info->count; ++j)
        {

            if (info->seeds[j].stamp >
                info->seeds[i].stamp)
            {

                seed tmp = info->seeds[i];
                info->seeds[i] = info->seeds[j];
                info->seeds[j] = tmp;
            }
        }
    }

    // Save a local copy of the verified .track file in cache/
    mkdir("cache", 0755);

    char cache_path[256];

    snprintf(cache_path, sizeof(cache_path),
             "cache/%s.track", name);

    FILE *f = fopen(cache_path, "wb");

    if (f)
    {
        fwrite(begin, 1, bytes, f);
        fclose(f);
    }

    printf("[peer %d] GET %s.track verified; "
           "%d seed(s), %lld bytes\n",
           own_port, name,
           info->count, info->size);

    free(response);
    return 0;
}

// Handle one incoming file-chunk request.
static void *handle_file_client(void *arg)
{
    service_job *job = arg;
    int fd = job->fd;

    free(job);

    char request[LINE_MAX_LEN];

    if (read_line(fd, request,
                  sizeof(request)) <= 0)
    {
        close(fd);
        return NULL;
    }

    // Parse and validate the requested filename, byte offset, and length
    char name[160];
    long long offset;
    size_t length;

    int ok = sscanf(
                 request, "<GET %159s %lld %zu>",
                 name, &offset, &length) == 3;

    if (!ok ||
        !good_name(name) ||
        offset < 0 ||
        length == 0 ||
        length > CHUNK_MAX)
    {

        send_all(fd, "<GET invalid>\n",
                 strlen("<GET invalid>\n"));

        close(fd);
        return NULL;
    }

    char path[512];

    snprintf(path, sizeof(path),
             "%s/%s", shared, name);

    int file = open(path, O_RDONLY);

    if (file < 0)
    {
        send_all(fd, "<GET missing>\n",
                 strlen("<GET missing>\n"));

        close(fd);
        return NULL;
    }

    // Read requested slice from disk without altering file offset
    unsigned char bytes[CHUNK_MAX];

    ssize_t n = pread(
        file, bytes, length, (off_t)offset);

    close(file);

    // Send data header followed by raw chunk bytes
    if (n != (ssize_t)length)
    {
        send_all(fd, "<GET missing>\n",
                 strlen("<GET missing>\n"));
    }
    else
    {
        char header[64];

        snprintf(header, sizeof(header),
                 "DATA %zu\n", length);

        if (!send_all(
                fd, header, strlen(header)))
        {

            send_all(fd, bytes, length);
        }
    }

    close(fd);
    return NULL;
}

// Periodically update the tracker with shared files.
static void *heartbeat(void *unused)
{
    (void)unused;

    while (1)
    {
        sleep((unsigned)refresh_seconds);

        DIR *d = opendir(shared);

        if (!d)
            continue;

        // Scan shared directory and refresh tracker status for each file
        struct dirent *entry;

        while ((entry = readdir(d)))
        {
            if (!good_name(entry->d_name))
                continue;

            char path[512];

            snprintf(path, sizeof(path),
                     "%s/%s", shared,
                     entry->d_name);

            struct stat st;

            if (!stat(path, &st) &&
                S_ISREG(st.st_mode))
            {

                command_update(
                    entry->d_name,
                    0, (long long)st.st_size);
            }
        }

        closedir(d);
    }

    return NULL;
}

// Run the peer's multithreaded file server.
static int command_serve(void)
{
    int listener = listen_on(own_port);

    if (listener < 0)
    {
        perror("peer listen");
        return 1;
    }

    // Start background thread for periodic tracker heartbeats
    pthread_t ticker;

    if (!pthread_create(
            &ticker, NULL, heartbeat, NULL))
    {

        pthread_detach(ticker);
    }

    printf("[peer %d] serving folder %s/; "
           "threaded socket server ready\n",
           own_port, shared);

    // Accept incoming peer connections and spawn a detached handler thread
    while (1)
    {
        int fd = accept(listener, NULL, NULL);

        if (fd < 0)
        {
            if (errno == EINTR)
                continue;

            perror("accept");
            continue;
        }

        service_job *job =
            malloc(sizeof(*job));

        if (!job)
        {
            close(fd);
            continue;
        }

        job->fd = fd;

        pthread_t thread;

        if (pthread_create(
                &thread, NULL,
                handle_file_client, job))
        {

            close(fd);
            free(job);
            continue;
        }

        pthread_detach(thread);
    }

    close(listener);
    return 0;
}

// Download one chunk from a selected seed.
static int receive_chunk(
    const seed *s,
    const file_info *info,
    long long offset,
    size_t length,
    int output)
{
    int fd = connect_to(s->ip, s->port);

    if (fd < 0)
        return -1;

    // Request chunk and verify response header matches expected size
    char req[256], head[64];

    snprintf(req, sizeof(req),
             "<GET %s %lld %zu>\n",
             info->name, offset, length);

    if (send_all(fd, req, strlen(req)) ||
        read_line(fd, head, sizeof(head)) <= 0)
    {

        close(fd);
        return -1;
    }

    size_t announced;

    if (sscanf(head, "DATA %zu",
               &announced) != 1 ||
        announced != length)
    {

        close(fd);
        return -1;
    }

    // Receive the exact byte count into memory
    unsigned char buffer[CHUNK_MAX];
    size_t got = 0;

    while (got < length)
    {
        ssize_t n = recv(
            fd, buffer + got,
            length - got, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
        {
            close(fd);
            return -1;
        }

        got += (size_t)n;
    }

    close(fd);

    // Write downloaded chunk directly to its target offset on disk
    size_t written = 0;

    while (written < length)
    {
        ssize_t n = pwrite(
            output,
            buffer + written,
            length - written,
            (off_t)(offset +
                    (long long)written));

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        written += (size_t)n;
    }

    return 0;
}

// Worker threads take the next available chunk.
static void *download_worker(void *arg)
{
    worker_arg *a = arg;
    chunk_queue *q = a->queue;

    int completed = 0, errors = 0;

    while (1)
    {
        // Atomically claim the next chunk index from the shared queue
        pthread_mutex_lock(&q->queue_lock);

        size_t chunk = q->next_chunk++;

        pthread_mutex_unlock(&q->queue_lock);

        if (chunk >= q->total_chunks)
            break;

        long long offset =
            (long long)chunk * CHUNK_MAX;

        size_t length =
            q->info->size - offset >= CHUNK_MAX ? CHUNK_MAX : (size_t)(q->info->size - offset);

        int success = 0;

        // Iterate through seeds until one serves the needed byte range
        for (int i = 0;
             i < q->info->count; ++i)
        {

            const seed *s =
                &q->info->seeds[i];

            if (s->first > offset ||
                s->last <
                    offset + (long long)length)
            {

                continue;
            }

            if (!receive_chunk(
                    s, q->info,
                    offset, length, q->output))
            {

                success = 1;
                break;
            }
        }

        if (success)
            completed++;
        else
            errors++;
    }

    // Record total chunk failures before exiting
    pthread_mutex_lock(&q->queue_lock);
    q->failures += errors;
    pthread_mutex_unlock(&q->queue_lock);

    printf("[peer %d] worker %d finished: "
           "%d chunk(s), %d error(s)\n",
           own_port, a->worker_no,
           completed, errors);

    return NULL;
}

// Automatically download a file using 4 workers.
static int command_get(const char *name)
{
    file_info info;

    if (get_tracker(name, &info))
        return 1;

    if (!info.count)
    {
        fprintf(stderr,
                "No seed is available\n");
        return 1;
    }

    // Create and pre-allocate the destination file in the shared folder
    char path[512];

    snprintf(path, sizeof(path),
             "%s/%s", shared, info.name);

    int output = open(
        path, O_CREAT | O_TRUNC | O_RDWR, 0644);

    if (output < 0 ||
        ftruncate(output, (off_t)info.size))
    {

        perror("download output");

        if (output >= 0)
            close(output);

        return 1;
    }

    // Initialize shared chunk queue and spawn 4 worker threads
    chunk_queue q = {
        .info = &info,
        .output = output,
        .next_chunk = 0,
        .total_chunks = (size_t)((info.size + CHUNK_MAX - 1) /
                                 CHUNK_MAX),
        .failures = 0,
        .queue_lock = PTHREAD_MUTEX_INITIALIZER};

    pthread_t ids[4];
    worker_arg args[4];

    int started = 0;

    printf("[peer %d] starting 4 workers; "
           "shared queue, max chunk %d bytes\n",
           own_port, CHUNK_MAX);

    for (int i = 0; i < 4; ++i)
    {
        args[i] = (worker_arg){
            .queue = &q,
            .worker_no = i};

        if (pthread_create(
                &ids[i], NULL,
                download_worker, &args[i]))
        {

            q.failures++;
            break;
        }

        started++;
    }

    // Wait for all worker threads to finish
    for (int i = 0; i < started; ++i)
        pthread_join(ids[i], NULL);

    pthread_mutex_destroy(&q.queue_lock);
    close(output);

    // Validate completed download against expected MD5 checksum
    char actual[33];

    if (q.failures ||
        md5_file(path, actual) ||
        strcmp(actual, info.hash))
    {

        fprintf(stderr,
                "[peer %d] download failed "
                "(chunks=%d / MD5 mismatch)\n",
                own_port, q.failures);

        return 1;
    }

    printf("[peer %d] download COMPLETE: "
           "%s; MD5 MATCH %s\n",
           own_port, info.name, actual);

    // Clean up cached tracker file and register as a new seed
    char cache_path[256];

    snprintf(cache_path, sizeof(cache_path),
             "cache/%s.track", info.name);

    unlink(cache_path);

    return command_update(
        info.name, 0, info.size);
}

// Read configuration files for this peer.
static int config(void)
{
    // Load tracker connection and heartbeat interval settings
    FILE *f =
        fopen("clientThreadConfig.cfg", "r");

    if (!f ||
        fscanf(f, "%d %63s %d",
               &tracker_port,
               tracker_ip,
               &refresh_seconds) != 3)
    {

        fprintf(stderr,
                "Missing clientThreadConfig.cfg\n");

        if (f)
            fclose(f);

        return 1;
    }

    fclose(f);

    // Load local listening port and shared directory path
    f = fopen("serverThreadConfig.cfg", "r");

    if (!f ||
        fscanf(f, "%d %255s",
               &own_port, shared) != 2)
    {

        fprintf(stderr,
                "Missing serverThreadConfig.cfg\n");

        if (f)
            fclose(f);

        return 1;
    }

    fclose(f);

    // Validate parsed port numbers and refresh interval
    if (tracker_port < 1 ||
        tracker_port > 65535 ||
        own_port < 1 ||
        own_port > 65535 ||
        refresh_seconds < 1)
    {

        return 1;
    }

    // Ensure required working directories exist
    mkdir(shared, 0755);
    mkdir("cache", 0755);

    return 0;
}

// Peer command-line interface.
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (config())
        return 1;

    // Dispatch subcommands
    if (argc == 2 &&
        !strcmp(argv[1], "serve"))
    {

        return command_serve();
    }

    if (argc == 2 &&
        !strcmp(argv[1], "list"))
    {

        return command_list();
    }

    if (argc == 3 &&
        !strcmp(argv[1], "share"))
    {

        return command_share(argv[2]);
    }

    if (argc == 3 &&
        !strcmp(argv[1], "info"))
    {

        file_info info;
        return get_tracker(argv[2], &info);
    }

    if (argc == 3 &&
        !strcmp(argv[1], "get"))
    {

        return command_get(argv[2]);
    }

    if (argc == 5 &&
        !strcmp(argv[1], "update"))
    {

        return command_update(
            argv[2],
            atoll(argv[3]),
            atoll(argv[4]));
    }

    fprintf(stderr,
            "Usage: ./peer serve | share FILE | "
            "list | info FILE.track | "
            "get FILE | update FILE START END\n");

    return 1;
}
