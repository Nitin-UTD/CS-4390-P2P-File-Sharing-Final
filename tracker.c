#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int port;
    int dead_timeout;
    char tracker_dir[MAX_PATH_LEN];
} ServerConfig;

static ServerConfig g_cfg;
static pthread_mutex_t g_file_lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_config_value(char *dst, size_t dstsz, const char *src) {
    size_t n;
    if (dstsz == 0) return;
    n = strnlen(src, dstsz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
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
        printf("tracker config: missing %s\n", field_name);
        return -1;
    }
    trim(line);
    if (!*line) {
        printf("tracker config: empty %s\n", field_name);
        return -1;
    }
    return 0;
}

/* The tracker port, storage directory, and dead-peer timeout must come from sconfig. */
static int read_server_config(const char *path) {
    FILE *fp = fopen(path, "r");
    char line[MAX_LINE];
    if (!fp) {
        printf("tracker: cannot open required config %s\n", path);
        return -1;
    }
    if (read_required_line(fp, line, sizeof(line), "port") != 0 ||
        parse_config_int(line, 1, 65535, &g_cfg.port) != 0) {
        printf("tracker: invalid port in %s\n", path);
        fclose(fp);
        return -1;
    }
    if (read_required_line(fp, line, sizeof(line), "tracker directory") != 0) {
        fclose(fp);
        return -1;
    }
    copy_config_value(g_cfg.tracker_dir, sizeof(g_cfg.tracker_dir), line);
    if (read_required_line(fp, line, sizeof(line), "dead-peer timeout") != 0 ||
        parse_config_int(line, 1, 86400, &g_cfg.dead_timeout) != 0) {
        printf("tracker: invalid dead-peer timeout in %s\n", path);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int has_track_suffix(const char *name) {
    size_t n = strlen(name);
    return n > 6 && strcmp(name + n - 6, ".track") == 0;
}

/* A new tracker run starts with an empty tracker-file directory. */
static int clear_tracker_dir(void) {
    DIR *dir;
    struct dirent *ent;
    char path[MAX_PATH_LEN];
    if (ensure_dir(g_cfg.tracker_dir) != 0) return -1;
    dir = opendir(g_cfg.tracker_dir);
    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        if (has_track_suffix(ent->d_name)) {
            path_join(path, sizeof(path), g_cfg.tracker_dir, ent->d_name);
            unlink(path);
        }
    }
    closedir(dir);
    return 0;
}

static void tracker_path(char *out, size_t outsz, const char *track_name) {
    char clean[256];
    snprintf(clean, sizeof(clean), "%s", base_name(track_name));
    path_join(out, outsz, g_cfg.tracker_dir, clean);
}

static int read_tracker_file(const char *path, TrackerInfo *info, char **text_out) {
    unsigned char *data = NULL;
    size_t len = 0;
    if (read_entire_file(path, &data, &len) != 0) return -1;
    if (parse_tracker_text((char *)data, info) != 0) {
        free(data);
        if (text_out) *text_out = NULL;
        return -1;
    }
    if (text_out) *text_out = (char *)data;
    else free(data);
    return 0;
}

static int write_tracker_info(const char *path, const TrackerInfo *info) {
    FILE *fp = fopen(path, "w");
    int i;
    if (!fp) return -1;
    fprintf(fp, "Filename: %s\n", info->filename);
    fprintf(fp, "Filesize: %ld\n", info->filesize);
    fprintf(fp, "Description: %s\n", info->description[0] ? info->description : "none");
    fprintf(fp, "MD5: %s\n", info->md5);
    fprintf(fp, "# list of peers follows next\n");
    for (i = 0; i < info->peer_count; i++) {
        const PeerEntry *p = &info->peers[i];
        fprintf(fp, "%s:%d:%ld:%ld:%ld\n", p->ip, p->port, p->start, p->end, p->timestamp);
    }
    fclose(fp);
    return 0;
}

static void remove_dead_peers(TrackerInfo *info) {
    long now = (long)time(NULL);
    int out = 0;
    int i;
    for (i = 0; i < info->peer_count; i++) {
        if (now - info->peers[i].timestamp <= g_cfg.dead_timeout) {
            if (out != i) info->peers[out] = info->peers[i];
            out++;
        }
    }
    info->peer_count = out;
}

/* LIST replies with one row per tracker file: index, file name, size, and MD5. */
static void handle_list(int sock) {
    DIR *dir;
    struct dirent *ent;
    char response[MAX_LINE * 4];
    char rows[MAX_LINE * 3] = "";
    int count = 0;
    pthread_mutex_lock(&g_file_lock);
    dir = opendir(g_cfg.tracker_dir);
    if (dir) {
        while ((ent = readdir(dir)) != NULL) {
            char path[MAX_PATH_LEN];
            TrackerInfo info;
            char row[MAX_LINE];
            if (!has_track_suffix(ent->d_name)) continue;
            path_join(path, sizeof(path), g_cfg.tracker_dir, ent->d_name);
            if (read_tracker_file(path, &info, NULL) == 0) {
                count++;
                snprintf(row, sizeof(row), "<%d %s %ld %s>\n", count, info.filename, info.filesize, info.md5);
                strncat(rows, row, sizeof(rows) - strlen(rows) - 1);
            }
        }
        closedir(dir);
    }
    pthread_mutex_unlock(&g_file_lock);
    snprintf(response, sizeof(response), "<REP LIST %d>\n%s<REP LIST END>\n", count, rows);
    send_all(sock, response, strlen(response));
}

/* GET wraps the tracker file text with begin/end markers and an MD5 checksum. */
static void handle_get(int sock, const char *track_name) {
    char path[MAX_PATH_LEN];
    unsigned char *data = NULL;
    size_t len = 0;
    char md5[33];
    char header[128], footer[128];
    tracker_path(path, sizeof(path), track_name);
    pthread_mutex_lock(&g_file_lock);
    if (read_entire_file(path, &data, &len) != 0) {
        pthread_mutex_unlock(&g_file_lock);
        send_all(sock, "<REP GET FAIL>\n", 15);
        return;
    }
    pthread_mutex_unlock(&g_file_lock);
    md5_buffer(data, len, md5);
    snprintf(header, sizeof(header), "<REP GET BEGIN>\n");
    snprintf(footer, sizeof(footer), "<REP GET END %s>\n", md5);
    send_all(sock, header, strlen(header));
    send_all(sock, data, len);
    if (len == 0 || data[len - 1] != '\n') send_all(sock, "\n", 1);
    send_all(sock, footer, strlen(footer));
    free(data);
}

/* createtracker creates the initial tracker entry and first complete peer range. */
static void handle_createtracker(int sock, char *msg) {
    char cmd[64], filename[256], description[512], md5[33], ip[64];
    int port;
    long filesize;
    char path[MAX_PATH_LEN];
    TrackerInfo info;
    if (sscanf(msg, "%63s %255s %ld %511s %32s %63s %d", cmd, filename, &filesize, description, md5, ip, &port) != 7) {
        send_all(sock, "<createtracker fail>\n", 21);
        return;
    }
    if (filesize <= 0 || strlen(md5) != 32 || port <= 0 || port > 65535) {
        send_all(sock, "<createtracker fail>\n", 21);
        return;
    }
    snprintf(path, sizeof(path), "%s.track", filename);
    tracker_path(path, sizeof(path), path);
    pthread_mutex_lock(&g_file_lock);
    if (file_exists(path)) {
        pthread_mutex_unlock(&g_file_lock);
        send_all(sock, "<createtracker ferr>\n", 21);
        return;
    }
    memset(&info, 0, sizeof(info));
    snprintf(info.filename, sizeof(info.filename), "%s", filename);
    info.filesize = filesize;
    snprintf(info.description, sizeof(info.description), "%s", description);
    snprintf(info.md5, sizeof(info.md5), "%s", md5);
    info.peer_count = 1;
    snprintf(info.peers[0].ip, sizeof(info.peers[0].ip), "%s", ip);
    info.peers[0].port = port;
    info.peers[0].start = 0;
    info.peers[0].end = filesize;
    info.peers[0].timestamp = (long)time(NULL);
    if (write_tracker_info(path, &info) == 0) {
        pthread_mutex_unlock(&g_file_lock);
        printf("tracker: created %s\n", path);
        send_all(sock, "<createtracker succ>\n", 21);
    } else {
        pthread_mutex_unlock(&g_file_lock);
        send_all(sock, "<createtracker fail>\n", 21);
    }
}

/* updatetracker refreshes or adds a peer range, dropping stale peers first. */
static void handle_updatetracker(int sock, char *msg) {
    char cmd[64], filename[256], ip[64], path[MAX_PATH_LEN];
    int port;
    long start, end;
    TrackerInfo info;
    int i, found = 0;
    if (sscanf(msg, "%63s %255s %ld %ld %63s %d", cmd, filename, &start, &end, ip, &port) != 6) {
        send_all(sock, "<updatetracker unknown fail>\n", 29);
        return;
    }
    if (start < 0 || end <= start || port <= 0 || port > 65535) {
        send_all(sock, "<updatetracker unknown fail>\n", 29);
        return;
    }
    snprintf(path, sizeof(path), "%s.track", filename);
    tracker_path(path, sizeof(path), path);
    pthread_mutex_lock(&g_file_lock);
    if (read_tracker_file(path, &info, NULL) != 0) {
        pthread_mutex_unlock(&g_file_lock);
        char reply[MAX_LINE];
        snprintf(reply, sizeof(reply), "<updatetracker %s ferr>\n", filename);
        send_all(sock, reply, strlen(reply));
        return;
    }
    remove_dead_peers(&info);
    if (end > info.filesize) {
        char reply[MAX_LINE];
        pthread_mutex_unlock(&g_file_lock);
        snprintf(reply, sizeof(reply), "<updatetracker %s fail>\n", filename);
        send_all(sock, reply, strlen(reply));
        return;
    }
    for (i = 0; i < info.peer_count; i++) {
        PeerEntry *p = &info.peers[i];
        if (strcmp(p->ip, ip) == 0 && p->port == port && p->start == start && p->end == end) {
            p->timestamp = (long)time(NULL);
            found = 1;
            break;
        }
    }
    if (!found && info.peer_count < MAX_PEERS) {
        PeerEntry *p = &info.peers[info.peer_count++];
        snprintf(p->ip, sizeof(p->ip), "%s", ip);
        p->port = port;
        p->start = start;
        p->end = end;
        p->timestamp = (long)time(NULL);
    } else if (!found) {
        char reply[MAX_LINE];
        pthread_mutex_unlock(&g_file_lock);
        snprintf(reply, sizeof(reply), "<updatetracker %s fail>\n", filename);
        send_all(sock, reply, strlen(reply));
        return;
    }
    if (write_tracker_info(path, &info) == 0) {
        char reply[MAX_LINE];
        pthread_mutex_unlock(&g_file_lock);
        snprintf(reply, sizeof(reply), "<updatetracker %s succ>\n", filename);
        send_all(sock, reply, strlen(reply));
    } else {
        char reply[MAX_LINE];
        pthread_mutex_unlock(&g_file_lock);
        snprintf(reply, sizeof(reply), "<updatetracker %s fail>\n", filename);
        send_all(sock, reply, strlen(reply));
    }
}

static void *worker(void *arg) {
    int sock = *(int *)arg;
    char msg[MAX_LINE], cmd[MAX_LINE], filename[256];
    free(arg);
    if (recv_line(sock, msg, sizeof(msg)) <= 0) {
        close(sock);
        return NULL;
    }
    snprintf(cmd, sizeof(cmd), "%s", msg);
    strip_protocol_marks(cmd);
    if (strcasecmp(cmd, "REQ LIST") == 0) {
        printf("tracker: REQ LIST\n");
        handle_list(sock);
    } else if (sscanf(cmd, "GET %255s", filename) == 1) {
        printf("tracker: GET %s\n", filename);
        handle_get(sock, filename);
    } else if (strncasecmp(cmd, "createtracker ", 14) == 0) {
        printf("tracker: %s\n", cmd);
        handle_createtracker(sock, cmd);
    } else if (strncasecmp(cmd, "updatetracker ", 14) == 0) {
        printf("tracker: %s\n", cmd);
        handle_updatetracker(sock, cmd);
    } else {
        const char *reply = "<ERR unknown command>\n";
        send_all(sock, reply, strlen(reply));
    }
    close(sock);
    return NULL;
}

int main(int argc, char **argv) {
    int listen_sock;
    const char *cfg = argc > 1 ? argv[1] : "sconfig";
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (read_server_config(cfg) != 0) return 1;
    if (clear_tracker_dir() != 0) {
        printf("tracker: cannot prepare tracker directory %s\n", g_cfg.tracker_dir);
        return 1;
    }
    listen_sock = create_listen_socket(g_cfg.port);
    if (listen_sock < 0) {
        perror("tracker listen");
        return 1;
    }
    printf("tracker: listening on port %d, directory %s\n", g_cfg.port, g_cfg.tracker_dir);
    while (1) {
        int *client = malloc(sizeof(int));
        pthread_t tid;
        if (!client) continue;
        *client = accept(listen_sock, NULL, NULL);
        if (*client < 0) {
            free(client);
            continue;
        }
        if (pthread_create(&tid, NULL, worker, client) == 0) {
            pthread_detach(tid);
        } else {
            close(*client);
            free(client);
        }
    }
}
