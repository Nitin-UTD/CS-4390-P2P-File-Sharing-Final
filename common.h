#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Common fixed-size limits used by both the tracker and peer programs. */
#define MAX_LINE 4096
#define MAX_PATH_LEN 512
#define MAX_PEERS 256

/* Project requirement: no peer-to-peer chunk response may exceed 1024 bytes. */
#define CHUNK_SIZE 1024

/* Default dead-peer timeout for tracker entries, in seconds. */
#define DEFAULT_TRACKER_TIMEOUT 1800

/* One peer's advertised byte range inside a tracker file. */
typedef struct {
    char ip[64];
    int port;
    long start;
    long end;
    long timestamp;
} PeerEntry;

/* In-memory representation of the required .track file format. */
typedef struct {
    char filename[256];
    long filesize;
    char description[512];
    char md5[33];
    PeerEntry peers[MAX_PEERS];
    int peer_count;
} TrackerInfo;

/* String cleanup helpers for protocol lines and config values. */
void trim(char *s);
void strip_protocol_marks(char *s);

/* Filesystem helpers used for peer folders, tracker folders, and cache files. */
int ensure_dir(const char *path);
int file_exists(const char *path);
long get_file_size(const char *path);
void path_join(char *out, size_t outsz, const char *a, const char *b);
const char *base_name(const char *path);

/* TCP helper routines that hide partial send/receive behavior. */
int send_all(int sock, const void *buf, size_t len);
int recv_all(int sock, void *buf, size_t len);
int recv_line(int sock, char *buf, size_t maxlen);
int connect_tcp(const char *ip, int port);
int create_listen_socket(int port);
int get_local_ip(char *out, size_t outsz);

/* MD5 helpers for file integrity and tracker GET response validation. */
void md5_buffer(const unsigned char *data, size_t len, char out_hex[33]);
int md5_file(const char *path, char out_hex[33]);

/* Whole-file helpers for small protocol artifacts such as .track files. */
int read_entire_file(const char *path, unsigned char **data, size_t *len);
int write_entire_file(const char *path, const unsigned char *data, size_t len);

/* Parser for the tracker file format described in the project prompt. */
int parse_tracker_text(const char *text, TrackerInfo *info);

#endif
