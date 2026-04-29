#include "common.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_DOWNLOAD_THREADS 8
#define SEGMENT_SIZE 65536

typedef struct {
    char id[64];
    char root[MAX_PATH_LEN];
    char shared_dir[MAX_PATH_LEN];
    char cache_dir[MAX_PATH_LEN];
    char downloads_dir[MAX_PATH_LEN];
    char tracker_ip[64];
    int tracker_port;
    int update_interval;
    int listen_port;
    char local_ip[64];
} PeerConfig;

typedef struct {
    PeerConfig *cfg;
    TrackerInfo *tracker;
    PeerEntry source;
    long start;
    long end;
    int ok;
} DownloadTask;

static PeerConfig g_cfg;
static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_file_lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_config_value(char *dst, size_t dstsz, const char *src) {
    size_t n;
    if (dstsz == 0) return;
    n = strnlen(src, dstsz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void stop_running(int sig) {
    (void)sig;
    g_running = 0;
}

static int parse_config_int(const char *text, int min, int max, int *out) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (text == end || *end != '\0' || value < min || value > max) return -1;
    *out = (int)value;
    return 0;
}

static int read_required_line(FILE *fp, char *line, size_t line_sz, const char *field_name) {
    if (!fgets(line, line_sz, fp)) {
        printf("peer config: missing %s\n", field_name);
        return -1;
    }
    trim(line);
    if (!*line) {
        printf("peer config: empty %s\n", field_name);
        return -1;
    }
    return 0;
}

/* Each peer must get tracker and upload-server settings from its own config files. */
static int read_client_config(PeerConfig *cfg) {
    char path[MAX_PATH_LEN], line[MAX_LINE];
    FILE *fp;
    path_join(path, sizeof(path), cfg->root, "clientThreadConfig.cfg");
    fp = fopen(path, "r");
    if (!fp) {
        printf("%s: cannot open required config %s\n", cfg->id, path);
        return -1;
    }
    if (read_required_line(fp, line, sizeof(line), "tracker port") != 0 ||
        parse_config_int(line, 1, 65535, &cfg->tracker_port) != 0) {
        printf("%s: invalid tracker port in %s\n", cfg->id, path);
        fclose(fp);
        return -1;
    }
    if (read_required_line(fp, line, sizeof(line), "tracker IP") != 0) {
        fclose(fp);
        return -1;
    }
    if (strcmp(line, "AUTO") != 0) {
        struct in_addr tmp;
        if (inet_pton(AF_INET, line, &tmp) <= 0) {
            printf("%s: invalid tracker IP in %s\n", cfg->id, path);
            fclose(fp);
            return -1;
        }
    }
    copy_config_value(cfg->tracker_ip, sizeof(cfg->tracker_ip), line);
    if (read_required_line(fp, line, sizeof(line), "update interval") != 0 ||
        parse_config_int(line, 1, 86400, &cfg->update_interval) != 0) {
        printf("%s: invalid update interval in %s\n", cfg->id, path);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int read_server_config(PeerConfig *cfg) {
    char path[MAX_PATH_LEN], line[MAX_LINE], shared[MAX_PATH_LEN];
    FILE *fp;
    path_join(path, sizeof(path), cfg->root, "serverThreadConfig.cfg");
    fp = fopen(path, "r");
    if (!fp) {
        printf("%s: cannot open required config %s\n", cfg->id, path);
        return -1;
    }
    if (read_required_line(fp, line, sizeof(line), "peer listen port") != 0 ||
        parse_config_int(line, 1, 65535, &cfg->listen_port) != 0) {
        printf("%s: invalid listen port in %s\n", cfg->id, path);
        fclose(fp);
        return -1;
    }
    if (read_required_line(fp, shared, sizeof(shared), "shared directory") != 0) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    if (shared[0] == '/') snprintf(cfg->shared_dir, sizeof(cfg->shared_dir), "%s", shared);
    else path_join(cfg->shared_dir, sizeof(cfg->shared_dir), cfg->root, shared);
    return 0;
}

static int init_config(PeerConfig *cfg, const char *id, const char *root) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->id, sizeof(cfg->id), "%s", id ? id : "Peer");
    snprintf(cfg->root, sizeof(cfg->root), "%s", root ? root : ".");
    path_join(cfg->cache_dir, sizeof(cfg->cache_dir), cfg->root, "cache");
    path_join(cfg->downloads_dir, sizeof(cfg->downloads_dir), cfg->root, "downloads");
    if (read_client_config(cfg) != 0 || read_server_config(cfg) != 0) return -1;
    if (ensure_dir(cfg->root) != 0 || ensure_dir(cfg->shared_dir) != 0 ||
        ensure_dir(cfg->cache_dir) != 0 || ensure_dir(cfg->downloads_dir) != 0) {
        printf("%s: could not create required peer directories\n", cfg->id);
        return -1;
    }
    get_local_ip(cfg->local_ip, sizeof(cfg->local_ip));
    if (strcmp(cfg->tracker_ip, "AUTO") == 0) snprintf(cfg->tracker_ip, sizeof(cfg->tracker_ip), "%s", cfg->local_ip);
    return 0;
}

static int tracker_request(const PeerConfig *cfg, const char *msg, char **reply_out, size_t *reply_len) {
    int sock = connect_tcp(cfg->tracker_ip, cfg->tracker_port);
    char buf[4096];
    size_t cap = 8192, len = 0;
    char *reply;
    ssize_t n;
    if (sock < 0) return -1;
    if (send_all(sock, msg, strlen(msg)) != 0) {
        close(sock);
        return -1;
    }
    reply = malloc(cap);
    if (!reply) {
        close(sock);
        return -1;
    }
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        if (len + (size_t)n + 1 > cap) {
            char *tmp;
            cap *= 2;
            tmp = realloc(reply, cap);
            if (!tmp) {
                free(reply);
                close(sock);
                return -1;
            }
            reply = tmp;
        }
        memcpy(reply + len, buf, (size_t)n);
        len += (size_t)n;
    }
    if (n < 0) {
        free(reply);
        close(sock);
        return -1;
    }
    close(sock);
    reply[len] = '\0';
    *reply_out = reply;
    if (reply_len) *reply_len = len;
    return 0;
}

static int send_createtracker(const PeerConfig *cfg, const char *file_name, const char *description) {
    char path[MAX_PATH_LEN], md5[33], msg[MAX_LINE], *reply = NULL;
    long size;
    path_join(path, sizeof(path), cfg->shared_dir, file_name);
    size = get_file_size(path);
    if (size < 0 || md5_file(path, md5) != 0) {
        printf("%s: cannot create tracker for %s\n", cfg->id, file_name);
        return -1;
    }
    snprintf(msg, sizeof(msg), "<createtracker %s %ld %s %s %s %d>\n",
             file_name, size, description && *description ? description : "shared_file", md5, cfg->local_ip, cfg->listen_port);
    printf("%s: %s", cfg->id, msg);
    if (tracker_request(cfg, msg, &reply, NULL) == 0) {
        printf("%s: %s", cfg->id, reply);
        free(reply);
        return 0;
    }
    printf("%s: tracker connection failed\n", cfg->id);
    return -1;
}

static int send_updatetracker(const PeerConfig *cfg, const char *file_name, long start, long end) {
    char msg[MAX_LINE], *reply = NULL;
    snprintf(msg, sizeof(msg), "<updatetracker %s %ld %ld %s %d>\n",
             file_name, start, end, cfg->local_ip, cfg->listen_port);
    if (tracker_request(cfg, msg, &reply, NULL) == 0) {
        printf("%s: %s", cfg->id, reply);
        free(reply);
        return 0;
    }
    printf("%s: updatetracker failed for %s\n", cfg->id, file_name);
    return -1;
}

static int send_raw_tracker_command(const PeerConfig *cfg, const char *line) {
    char msg[MAX_LINE], *reply = NULL;
    snprintf(msg, sizeof(msg), "<%s>\n", line);
    printf("%s: %s", cfg->id, msg);
    if (tracker_request(cfg, msg, &reply, NULL) == 0) {
        printf("%s: %s", cfg->id, reply);
        free(reply);
        return 0;
    }
    printf("%s: tracker command failed\n", cfg->id);
    return -1;
}

static int request_list(const PeerConfig *cfg) {
    char *reply = NULL;
    printf("%s: REQ LIST\n", cfg->id);
    if (tracker_request(cfg, "<REQ LIST>\n", &reply, NULL) != 0) {
        printf("%s: LIST failed\n", cfg->id);
        return -1;
    }
    printf("%s: tracker reply\n%s", cfg->id, reply);
    free(reply);
    return 0;
}

static int extract_tracker_body(const char *reply, char **body_out, char md5[33]) {
    const char *begin = strstr(reply, "<REP GET BEGIN>");
    const char *body;
    const char *end = strstr(reply, "<REP GET END ");
    size_t len;
    if (!begin || !end || end <= begin) return -1;
    body = strchr(begin, '\n');
    if (!body) return -1;
    body++;
    len = (size_t)(end - body);
    *body_out = malloc(len + 1);
    if (!*body_out) return -1;
    memcpy(*body_out, body, len);
    (*body_out)[len] = '\0';
    if (sscanf(end, "<REP GET END %32[^>]>", md5) != 1) {
        free(*body_out);
        return -1;
    }
    return 0;
}

/* GET returns a tracker file plus an MD5 of the tracker text; verify before parsing. */
static int get_tracker_file(const PeerConfig *cfg, const char *track_name, TrackerInfo *info) {
    char msg[MAX_LINE], *reply = NULL, *body = NULL, md5_expected[33], md5_actual[33], cache_path[MAX_PATH_LEN];
    snprintf(msg, sizeof(msg), "<GET %s>\n", track_name);
    printf("%s: GET %s\n", cfg->id, track_name);
    if (tracker_request(cfg, msg, &reply, NULL) != 0) {
        printf("%s: GET failed\n", cfg->id);
        return -1;
    }
    if (extract_tracker_body(reply, &body, md5_expected) != 0) {
        printf("%s: invalid tracker GET response\n%s", cfg->id, reply);
        free(reply);
        return -1;
    }
    md5_buffer((unsigned char *)body, strlen(body), md5_actual);
    if (strcmp(md5_expected, md5_actual) != 0) {
        printf("%s: tracker MD5 mismatch expected %s got %s\n", cfg->id, md5_expected, md5_actual);
        free(reply);
        free(body);
        return -1;
    }
    path_join(cache_path, sizeof(cache_path), cfg->cache_dir, track_name);
    if (write_entire_file(cache_path, (unsigned char *)body, strlen(body)) != 0) {
        printf("%s: cannot cache tracker file %s\n", cfg->id, track_name);
        free(reply);
        free(body);
        return -1;
    }
    if (parse_tracker_text(body, info) != 0) {
        printf("%s: cannot parse tracker file\n", cfg->id);
        free(reply);
        free(body);
        return -1;
    }
    free(reply);
    free(body);
    return 0;
}

static int choose_source_skip(const TrackerInfo *tracker, long start, long end,
                              const char *skip_ip, int skip_port, PeerEntry *out) {
    int i, found = 0;
    long best_ts = -1;
    for (i = 0; i < tracker->peer_count; i++) {
        const PeerEntry *p = &tracker->peers[i];
        if (skip_ip && strcmp(p->ip, skip_ip) == 0 && p->port == skip_port) continue;
        if (p->start <= start && p->end >= end && p->timestamp > best_ts) {
            *out = *p;
            best_ts = p->timestamp;
            found = 1;
        }
    }
    return found ? 0 : -1;
}

static int choose_source(const TrackerInfo *tracker, long start, long end, PeerEntry *out) {
    return choose_source_skip(tracker, start, end, NULL, 0, out);
}

static void part_file_path(const PeerConfig *cfg, const char *filename, char *out, size_t outsz) {
    char name[320];
    snprintf(name, sizeof(name), "%s.parts", filename);
    path_join(out, outsz, cfg->cache_dir, name);
}

static int is_part_done(const PeerConfig *cfg, const char *filename, long start, long end) {
    char path[MAX_PATH_LEN];
    FILE *fp;
    long a, b;
    part_file_path(cfg, filename, path, sizeof(path));
    fp = fopen(path, "r");
    if (!fp) return 0;
    while (fscanf(fp, "%ld %ld", &a, &b) == 2) {
        if (a == start && b == end) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static void mark_part_done(const PeerConfig *cfg, const char *filename, long start, long end) {
    char path[MAX_PATH_LEN];
    FILE *fp;
    if (is_part_done(cfg, filename, start, end)) return;
    part_file_path(cfg, filename, path, sizeof(path));
    fp = fopen(path, "a");
    if (!fp) {
        printf("%s: cannot update part cache for %s\n", cfg->id, filename);
        return;
    }
    fprintf(fp, "%ld %ld\n", start, end);
    fclose(fp);
}

static int request_chunk(const PeerConfig *cfg, const PeerEntry *src, const char *filename, long start, long end, unsigned char **data, size_t *len) {
    int sock = connect_tcp(src->ip, src->port);
    char req[MAX_LINE], header[MAX_LINE];
    long size;
    if (sock < 0) return -1;
    snprintf(req, sizeof(req), "<GETCHUNK %s %ld %ld>\n", filename, start, end);
    if (send_all(sock, req, strlen(req)) != 0 || recv_line(sock, header, sizeof(header)) <= 0) {
        close(sock);
        return -1;
    }
    strip_protocol_marks(header);
    if (strncmp(header, "GET invalid", 11) == 0) {
        close(sock);
        return -1;
    }
    if (sscanf(header, "REP CHUNK %ld", &size) != 1 || size != end - start || size > CHUNK_SIZE) {
        close(sock);
        return -1;
    }
    *data = malloc((size_t)size);
    if (!*data) {
        close(sock);
        return -1;
    }
    if (recv_all(sock, *data, (size_t)size) != 0) {
        free(*data);
        close(sock);
        return -1;
    }
    *len = (size_t)size;
    close(sock);
    printf("%s: downloading %ld to %ld bytes of %s from %s %d\n",
           cfg->id, start, end, filename, src->ip, src->port);
    return 0;
}

/* A segment is downloaded in 1024-byte chunks and retried from fresh tracker data. */
static void *download_worker(void *arg) {
    DownloadTask *task = (DownloadTask *)arg;
    long pos;
    char path[MAX_PATH_LEN];
    PeerEntry source = task->source;
    task->ok = 0;
    for (pos = task->start; pos < task->end; pos += CHUNK_SIZE) {
        long chunk_end = pos + CHUNK_SIZE;
        unsigned char *data = NULL;
        size_t len = 0;
        FILE *fp;
        int attempt, got_chunk = 0;
        if (chunk_end > task->end) chunk_end = task->end;
        for (attempt = 0; attempt < 120; attempt++) {
            PeerEntry failed_source = source;
            TrackerInfo fresh;
            char track_name[300];
            if (request_chunk(task->cfg, &source, task->tracker->filename, pos, chunk_end, &data, &len) == 0) {
                got_chunk = 1;
                break;
            }
            snprintf(track_name, sizeof(track_name), "%s.track", task->tracker->filename);
            usleep(100000);
            if (get_tracker_file(task->cfg, track_name, &fresh) == 0 &&
                choose_source_skip(&fresh, pos, chunk_end, failed_source.ip, failed_source.port, &source) == 0) {
                continue;
            }
            if (choose_source_skip(task->tracker, pos, chunk_end, failed_source.ip, failed_source.port, &source) != 0) {
                source = failed_source;
            }
        }
        if (!got_chunk) {
            return NULL;
        }
        path_join(path, sizeof(path), task->cfg->shared_dir, task->tracker->filename);
        pthread_mutex_lock(&g_file_lock);
        fp = fopen(path, "r+b");
        if (!fp) fp = fopen(path, "w+b");
        if (!fp || fseek(fp, pos, SEEK_SET) != 0 || fwrite(data, 1, len, fp) != len) {
            if (fp) fclose(fp);
            pthread_mutex_unlock(&g_file_lock);
            free(data);
            return NULL;
        }
        fclose(fp);
        pthread_mutex_unlock(&g_file_lock);
        free(data);
    }
    task->ok = 1;
    if (task->ok) {
        mark_part_done(task->cfg, task->tracker->filename, task->start, task->end);
        send_updatetracker(task->cfg, task->tracker->filename, task->start, task->end);
    }
    return NULL;
}

static int download_from_tracker(const PeerConfig *cfg, TrackerInfo *tracker) {
    long size = tracker->filesize;
    long total_segments, next_segment = 0;
    int i;
    DownloadTask tasks[MAX_DOWNLOAD_THREADS];
    pthread_t threads[MAX_DOWNLOAD_THREADS];
    char final_path[MAX_PATH_LEN], md5[33], cache_track[MAX_PATH_LEN], track_name[300];
    FILE *fp;
    if (size <= 0) return -1;
    total_segments = (size + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
    path_join(final_path, sizeof(final_path), cfg->shared_dir, tracker->filename);
    fp = fopen(final_path, "ab");
    if (fp) fclose(fp);
    snprintf(track_name, sizeof(track_name), "%s.track", tracker->filename);
    while (next_segment < total_segments) {
        int tasks_count = 0;
        memset(tasks, 0, sizeof(tasks));
        while (tasks_count < MAX_DOWNLOAD_THREADS && next_segment < total_segments) {
            long segment = next_segment;
            long pos = segment * SEGMENT_SIZE;
            long end = pos + SEGMENT_SIZE;
            if (end > size) end = size;
            next_segment++;
            if (is_part_done(cfg, tracker->filename, pos, end)) {
                send_updatetracker(cfg, tracker->filename, pos, end);
                continue;
            }
            if (choose_source(tracker, pos, end, &tasks[tasks_count].source) != 0) {
                if (get_tracker_file(cfg, track_name, tracker) != 0 ||
                    choose_source(tracker, pos, end, &tasks[tasks_count].source) != 0) {
                    printf("%s: no source currently has bytes %ld to %ld of %s\n", cfg->id, pos, end, tracker->filename);
                    return -1;
                }
            }
            tasks[tasks_count].cfg = (PeerConfig *)cfg;
            tasks[tasks_count].tracker = tracker;
            tasks[tasks_count].start = pos;
            tasks[tasks_count].end = end;
            if (pthread_create(&threads[tasks_count], NULL, download_worker, &tasks[tasks_count]) != 0) {
                printf("%s: could not create download thread\n", cfg->id);
                return -1;
            }
            tasks_count++;
        }
        if (tasks_count == 0) continue;
        for (i = 0; i < tasks_count; i++) pthread_join(threads[i], NULL);
        for (i = 0; i < tasks_count; i++) {
            if (!tasks[i].ok) {
                printf("%s: failed to download bytes %ld to %ld of %s\n",
                       cfg->id, tasks[i].start, tasks[i].end, tracker->filename);
                return -1;
            }
        }
    }
    if (get_file_size(final_path) == size && md5_file(final_path, md5) == 0 && strcmp(md5, tracker->md5) == 0) {
        printf("%s: File %s download complete\n", cfg->id, tracker->filename);
        path_join(cache_track, sizeof(cache_track), cfg->cache_dir, track_name);
        unlink(cache_track);
        part_file_path(cfg, tracker->filename, cache_track, sizeof(cache_track));
        unlink(cache_track);
        return 0;
    }
    printf("%s: MD5 check failed for %s\n", cfg->id, tracker->filename);
    return -1;
}

static int get_and_download(const PeerConfig *cfg, const char *track_name) {
    TrackerInfo tracker;
    if (get_tracker_file(cfg, track_name, &tracker) != 0) return -1;
    return download_from_tracker(cfg, &tracker);
}

static void serve_chunk_request(int sock, char *request) {
    char cmd[64], filename[256], path[MAX_PATH_LEN], header[MAX_LINE];
    long start, end, size;
    FILE *fp;
    unsigned char buf[CHUNK_SIZE];
    strip_protocol_marks(request);
    if (sscanf(request, "%63s %255s %ld %ld", cmd, filename, &start, &end) != 4 ||
        strcmp(cmd, "GETCHUNK") != 0 || start < 0 || end <= start || end - start > CHUNK_SIZE) {
        send_all(sock, "<GET invalid>\n", 14);
        return;
    }
    path_join(path, sizeof(path), g_cfg.shared_dir, filename);
    fp = fopen(path, "rb");
    if (!fp) {
        send_all(sock, "<GET invalid>\n", 14);
        return;
    }
    if (fseek(fp, start, SEEK_SET) != 0) {
        fclose(fp);
        send_all(sock, "<GET invalid>\n", 14);
        return;
    }
    size = (long)fread(buf, 1, (size_t)(end - start), fp);
    fclose(fp);
    snprintf(header, sizeof(header), "<REP CHUNK %ld>\n", size);
    send_all(sock, header, strlen(header));
    if (size > 0) send_all(sock, buf, (size_t)size);
}

static void *peer_upload_worker(void *arg) {
    int sock = *(int *)arg;
    char request[MAX_LINE];
    free(arg);
    if (recv_line(sock, request, sizeof(request)) > 0) serve_chunk_request(sock, request);
    close(sock);
    return NULL;
}

static void *peer_server(void *arg) {
    PeerConfig *cfg = (PeerConfig *)arg;
    int listen_sock = create_listen_socket(cfg->listen_port);
    if (listen_sock < 0) {
        perror("peer listen");
        g_running = 0;
        return NULL;
    }
    printf("%s: listening for peer chunks on port %d\n", cfg->id, cfg->listen_port);
    while (g_running) {
        int *client = malloc(sizeof(int));
        pthread_t tid;
        if (!client) continue;
        *client = accept(listen_sock, NULL, NULL);
        if (*client < 0) {
            free(client);
            continue;
        }
        if (pthread_create(&tid, NULL, peer_upload_worker, client) == 0) pthread_detach(tid);
        else {
            close(*client);
            free(client);
        }
    }
    close(listen_sock);
    return NULL;
}

static void seed_all_shared_files(const PeerConfig *cfg) {
    DIR *dir = opendir(cfg->shared_dir);
    struct dirent *ent;
    if (!dir) return;
    while ((ent = readdir(dir)) != NULL) {
        char path[MAX_PATH_LEN];
        struct stat st;
        if (ent->d_name[0] == '.') continue;
        path_join(path, sizeof(path), cfg->shared_dir, ent->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (send_createtracker(cfg, ent->d_name, "shared_file") != 0) {
                printf("%s: failed to seed %s\n", cfg->id, ent->d_name);
            }
        }
    }
    closedir(dir);
}

/* Advertise only complete segments for partial downloads; full files advertise whole range. */
static void periodic_update_shared(const PeerConfig *cfg) {
    DIR *dir = opendir(cfg->shared_dir);
    struct dirent *ent;
    if (!dir) return;
    while ((ent = readdir(dir)) != NULL) {
        char path[MAX_PATH_LEN], parts_path[MAX_PATH_LEN];
        long size;
        FILE *parts;
        int sent_parts = 0;
        if (ent->d_name[0] == '.') continue;
        path_join(path, sizeof(path), cfg->shared_dir, ent->d_name);
        size = get_file_size(path);
        if (size <= 0) continue;
        part_file_path(cfg, ent->d_name, parts_path, sizeof(parts_path));
        parts = fopen(parts_path, "r");
        if (parts) {
            long start, end;
            while (fscanf(parts, "%ld %ld", &start, &end) == 2) {
                if (start >= 0 && end > start && end <= size) {
                    send_updatetracker(cfg, ent->d_name, start, end);
                    sent_parts = 1;
                }
            }
            fclose(parts);
        }
        if (!sent_parts) send_updatetracker(cfg, ent->d_name, 0, size);
    }
    closedir(dir);
}

static void print_usage(void) {
    printf("Usage:\n");
    printf("  ./peer Peer1 peer1\n");
    printf("  ./peer Peer1 peer1 --seed\n");
    printf("  ./peer Peer3 peer3 --download file1.track file2.track [--stay]\n");
}

static void interactive_loop(PeerConfig *cfg) {
    char line[MAX_LINE];
    printf("%s: enter commands: LIST, GET file.track, createtracker file desc, updatetracker file start end, quit\n", cfg->id);
    while (g_running && fgets(line, sizeof(line), stdin)) {
        char a[256] = "", b[512] = "";
        char file[256] = "", desc[512] = "", md5[64] = "", ip[64] = "";
        long filesize, start, end;
        int port;
        trim(line);
        if (!*line) continue;
        if (strcasecmp(line, "quit") == 0 || strcasecmp(line, "exit") == 0) break;
        if (strcasecmp(line, "LIST") == 0 || strcasecmp(line, "REQ LIST") == 0) {
            request_list(cfg);
        } else if (sscanf(line, "GET %255s", a) == 1) {
            get_and_download(cfg, a);
        } else if (sscanf(line, "createtracker %255s %ld %511s %63s %63s %d",
                          file, &filesize, desc, md5, ip, &port) == 6) {
            send_raw_tracker_command(cfg, line);
        } else if (sscanf(line, "updatetracker %255s %ld %ld %63s %d",
                          file, &start, &end, ip, &port) == 5) {
            send_raw_tracker_command(cfg, line);
        } else if (sscanf(line, "createtracker %255s %511s", a, b) >= 1) {
            send_createtracker(cfg, a, b[0] ? b : "shared_file");
        } else {
            if (sscanf(line, "updatetracker %255s %ld %ld", a, &start, &end) == 3) send_updatetracker(cfg, a, start, end);
            else printf("%s: unknown command\n", cfg->id);
        }
    }
}

int main(int argc, char **argv) {
    pthread_t server_tid;
    int i;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);
    if (argc < 3) {
        print_usage();
        return 1;
    }
    if (init_config(&g_cfg, argv[1], argv[2]) != 0) return 1;
    if (pthread_create(&server_tid, NULL, peer_server, &g_cfg) != 0) {
        perror("peer server thread");
        return 1;
    }
    pthread_detach(server_tid);
    sleep(1);
    if (argc >= 4 && strcmp(argv[3], "--seed") == 0) {
        seed_all_shared_files(&g_cfg);
        while (g_running) {
            sleep(g_cfg.update_interval);
            if (!g_running) break;
            periodic_update_shared(&g_cfg);
        }
    } else if (argc >= 5 && strcmp(argv[3], "--download") == 0) {
        int download_failed = 0, stay_online = 0, track_count = 0;
        request_list(&g_cfg);
        for (i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--stay") == 0) {
                stay_online = 1;
                continue;
            }
            if (argv[i][0] == '-') {
                printf("%s: unknown download option %s\n", g_cfg.id, argv[i]);
                download_failed = 1;
                continue;
            }
            track_count++;
            if (get_and_download(&g_cfg, argv[i]) != 0) download_failed = 1;
        }
        if (track_count == 0) {
            printf("%s: no tracker files requested\n", g_cfg.id);
            download_failed = 1;
        }
        if (stay_online) {
            if (download_failed) {
                printf("%s: staying online to serve available files\n", g_cfg.id);
            } else {
                printf("%s: staying online to serve downloaded files\n", g_cfg.id);
            }
            while (g_running) {
                sleep(g_cfg.update_interval);
                if (!g_running) break;
                periodic_update_shared(&g_cfg);
            }
        } else {
            sleep(2);
        }
        printf("%s terminated\n", g_cfg.id);
        return download_failed ? 1 : 0;
    } else {
        interactive_loop(&g_cfg);
    }
    printf("%s terminated\n", g_cfg.id);
    return 0;
}
