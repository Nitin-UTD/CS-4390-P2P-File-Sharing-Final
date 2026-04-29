#include "common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

void trim(char *s) {
    size_t len;
    char *p;
    if (!s) return;
    while (isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    p = s;
    while (*p) {
        if (*p == '\r' || *p == '\n') *p = '\0';
        p++;
    }
}

static void copy_value(char *dst, size_t dstsz, const char *src) {
    size_t n;
    if (dstsz == 0) return;
    n = strnlen(src, dstsz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void strip_protocol_marks(char *s) {
    size_t len;
    trim(s);
    len = strlen(s);
    if (len >= 2 && s[0] == '<' && s[len - 1] == '>') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
    trim(s);
}

int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(path, 0755);
}

int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

long get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

void path_join(char *out, size_t outsz, const char *a, const char *b) {
    if (!a || !*a) {
        snprintf(out, outsz, "%s", b ? b : "");
        return;
    }
    if (!b || !*b) {
        snprintf(out, outsz, "%s", a);
        return;
    }
    snprintf(out, outsz, "%s%s%s", a, a[strlen(a) - 1] == '/' ? "" : "/", b);
}

const char *base_name(const char *path) {
    const char *p;
    if (!path) return "";
    p = strrchr(path, '/');
    return p ? p + 1 : path;
}

int send_all(int sock, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(sock, p, len, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int recv_all(int sock, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = recv(sock, p, len, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int recv_line(int sock, char *buf, size_t maxlen) {
    size_t used = 0;
    while (used + 1 < maxlen) {
        char c;
        ssize_t n = recv(sock, &c, 1, 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf[used++] = c;
        if (c == '\n') break;
    }
    buf[used] = '\0';
    return (int)used;
}

/* Connects with bounded timeouts so a dead peer cannot stall a download worker. */
int connect_tcp(const char *ip, int port) {
    int sock;
    int flags, err = 0;
    socklen_t err_len = sizeof(err);
    struct sockaddr_in addr;
    struct timeval timeout;
    fd_set wfds;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }
    flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    if (select(sock + 1, NULL, &wfds, NULL, &timeout) <= 0 ||
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0 || err != 0) {
        close(sock);
        return -1;
    }
    if (fcntl(sock, F_SETFL, flags) < 0) {
        close(sock);
        return -1;
    }
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int create_listen_socket(int port) {
    int sock;
    int opt = 1;
    struct sockaddr_in addr;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    if (listen(sock, 64) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int get_local_ip(char *out, size_t outsz) {
    int sock;
    struct sockaddr_in remote;
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        snprintf(out, outsz, "127.0.0.1");
        return -1;
    }
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(80);
    if (inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr) == 1 &&
        connect(sock, (struct sockaddr *)&remote, sizeof(remote)) == 0 &&
        getsockname(sock, (struct sockaddr *)&name, &namelen) == 0) {
        inet_ntop(AF_INET, &name.sin_addr, out, (socklen_t)outsz);
    } else {
        snprintf(out, outsz, "127.0.0.1");
    }
    close(sock);
    return 0;
}

int read_entire_file(const char *path, unsigned char **data, size_t *len) {
    FILE *fp = fopen(path, "rb");
    long size;
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    *data = (unsigned char *)malloc((size_t)size + 1);
    if (!*data) {
        fclose(fp);
        return -1;
    }
    if (fread(*data, 1, (size_t)size, fp) != (size_t)size) {
        free(*data);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    (*data)[size] = '\0';
    *len = (size_t)size;
    return 0;
}

int write_entire_file(const char *path, const unsigned char *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    if (fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

typedef struct {
    uint32_t state[4];
    uint64_t count;
    unsigned char buffer[64];
} MD5_CTX_LOCAL;

/* MD5 follows the RFC 1321 reference algorithm; see README external code notice. */
#define F(x,y,z) (((x) & (y)) | ((~x) & (z)))
#define G(x,y,z) (((x) & (z)) | ((y) & (~z)))
#define H(x,y,z) ((x) ^ (y) ^ (z))
#define I(x,y,z) ((y) ^ ((x) | (~z)))
#define ROTATE_LEFT(x,n) (((x) << (n)) | ((x) >> (32 - (n))))
#define FF(a,b,c,d,x,s,ac) { (a) += F((b),(c),(d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT((a),(s)); (a) += (b); }
#define GG(a,b,c,d,x,s,ac) { (a) += G((b),(c),(d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT((a),(s)); (a) += (b); }
#define HH(a,b,c,d,x,s,ac) { (a) += H((b),(c),(d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT((a),(s)); (a) += (b); }
#define II(a,b,c,d,x,s,ac) { (a) += I((b),(c),(d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT((a),(s)); (a) += (b); }

static void encode(unsigned char *output, const uint32_t *input, size_t len) {
    size_t i, j;
    for (i = 0, j = 0; j < len; i++, j += 4) {
        output[j] = (unsigned char)(input[i] & 0xff);
        output[j + 1] = (unsigned char)((input[i] >> 8) & 0xff);
        output[j + 2] = (unsigned char)((input[i] >> 16) & 0xff);
        output[j + 3] = (unsigned char)((input[i] >> 24) & 0xff);
    }
}

static void decode(uint32_t *output, const unsigned char *input, size_t len) {
    size_t i, j;
    for (i = 0, j = 0; j < len; i++, j += 4) {
        output[i] = ((uint32_t)input[j]) | (((uint32_t)input[j + 1]) << 8) |
                    (((uint32_t)input[j + 2]) << 16) | (((uint32_t)input[j + 3]) << 24);
    }
}

static void md5_transform(uint32_t state[4], const unsigned char block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    decode(x, block, 64);
    FF(a,b,c,d,x[ 0], 7,0xd76aa478); FF(d,a,b,c,x[ 1],12,0xe8c7b756);
    FF(c,d,a,b,x[ 2],17,0x242070db); FF(b,c,d,a,x[ 3],22,0xc1bdceee);
    FF(a,b,c,d,x[ 4], 7,0xf57c0faf); FF(d,a,b,c,x[ 5],12,0x4787c62a);
    FF(c,d,a,b,x[ 6],17,0xa8304613); FF(b,c,d,a,x[ 7],22,0xfd469501);
    FF(a,b,c,d,x[ 8], 7,0x698098d8); FF(d,a,b,c,x[ 9],12,0x8b44f7af);
    FF(c,d,a,b,x[10],17,0xffff5bb1); FF(b,c,d,a,x[11],22,0x895cd7be);
    FF(a,b,c,d,x[12], 7,0x6b901122); FF(d,a,b,c,x[13],12,0xfd987193);
    FF(c,d,a,b,x[14],17,0xa679438e); FF(b,c,d,a,x[15],22,0x49b40821);
    GG(a,b,c,d,x[ 1], 5,0xf61e2562); GG(d,a,b,c,x[ 6], 9,0xc040b340);
    GG(c,d,a,b,x[11],14,0x265e5a51); GG(b,c,d,a,x[ 0],20,0xe9b6c7aa);
    GG(a,b,c,d,x[ 5], 5,0xd62f105d); GG(d,a,b,c,x[10], 9,0x02441453);
    GG(c,d,a,b,x[15],14,0xd8a1e681); GG(b,c,d,a,x[ 4],20,0xe7d3fbc8);
    GG(a,b,c,d,x[ 9], 5,0x21e1cde6); GG(d,a,b,c,x[14], 9,0xc33707d6);
    GG(c,d,a,b,x[ 3],14,0xf4d50d87); GG(b,c,d,a,x[ 8],20,0x455a14ed);
    GG(a,b,c,d,x[13], 5,0xa9e3e905); GG(d,a,b,c,x[ 2], 9,0xfcefa3f8);
    GG(c,d,a,b,x[ 7],14,0x676f02d9); GG(b,c,d,a,x[12],20,0x8d2a4c8a);
    HH(a,b,c,d,x[ 5], 4,0xfffa3942); HH(d,a,b,c,x[ 8],11,0x8771f681);
    HH(c,d,a,b,x[11],16,0x6d9d6122); HH(b,c,d,a,x[14],23,0xfde5380c);
    HH(a,b,c,d,x[ 1], 4,0xa4beea44); HH(d,a,b,c,x[ 4],11,0x4bdecfa9);
    HH(c,d,a,b,x[ 7],16,0xf6bb4b60); HH(b,c,d,a,x[10],23,0xbebfbc70);
    HH(a,b,c,d,x[13], 4,0x289b7ec6); HH(d,a,b,c,x[ 0],11,0xeaa127fa);
    HH(c,d,a,b,x[ 3],16,0xd4ef3085); HH(b,c,d,a,x[ 6],23,0x04881d05);
    HH(a,b,c,d,x[ 9], 4,0xd9d4d039); HH(d,a,b,c,x[12],11,0xe6db99e5);
    HH(c,d,a,b,x[15],16,0x1fa27cf8); HH(b,c,d,a,x[ 2],23,0xc4ac5665);
    II(a,b,c,d,x[ 0], 6,0xf4292244); II(d,a,b,c,x[ 7],10,0x432aff97);
    II(c,d,a,b,x[14],15,0xab9423a7); II(b,c,d,a,x[ 5],21,0xfc93a039);
    II(a,b,c,d,x[12], 6,0x655b59c3); II(d,a,b,c,x[ 3],10,0x8f0ccc92);
    II(c,d,a,b,x[10],15,0xffeff47d); II(b,c,d,a,x[ 1],21,0x85845dd1);
    II(a,b,c,d,x[ 8], 6,0x6fa87e4f); II(d,a,b,c,x[15],10,0xfe2ce6e0);
    II(c,d,a,b,x[ 6],15,0xa3014314); II(b,c,d,a,x[13],21,0x4e0811a1);
    II(a,b,c,d,x[ 4], 6,0xf7537e82); II(d,a,b,c,x[11],10,0xbd3af235);
    II(c,d,a,b,x[ 2],15,0x2ad7d2bb); II(b,c,d,a,x[ 9],21,0xeb86d391);
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void md5_init(MD5_CTX_LOCAL *ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(MD5_CTX_LOCAL *ctx, const unsigned char *input, size_t len) {
    size_t i, index, part_len;
    index = (size_t)((ctx->count >> 3) & 0x3f);
    ctx->count += ((uint64_t)len << 3);
    part_len = 64 - index;
    if (len >= part_len) {
        memcpy(&ctx->buffer[index], input, part_len);
        md5_transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64) md5_transform(ctx->state, &input[i]);
        index = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[index], &input[i], len - i);
}

static void md5_final(unsigned char digest[16], MD5_CTX_LOCAL *ctx) {
    static unsigned char padding[64] = { 0x80 };
    unsigned char bits[8];
    uint32_t count_words[2];
    size_t index, pad_len;
    count_words[0] = (uint32_t)(ctx->count & 0xffffffff);
    count_words[1] = (uint32_t)(ctx->count >> 32);
    encode(bits, count_words, 8);
    index = (size_t)((ctx->count >> 3) & 0x3f);
    pad_len = (index < 56) ? (56 - index) : (120 - index);
    md5_update(ctx, padding, pad_len);
    md5_update(ctx, bits, 8);
    encode(digest, ctx->state, 16);
    memset(ctx, 0, sizeof(*ctx));
}

void md5_buffer(const unsigned char *data, size_t len, char out_hex[33]) {
    static const char hex[] = "0123456789abcdef";
    MD5_CTX_LOCAL ctx;
    unsigned char digest[16];
    int i;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(digest, &ctx);
    for (i = 0; i < 16; i++) {
        out_hex[i * 2] = hex[(digest[i] >> 4) & 0xf];
        out_hex[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out_hex[32] = '\0';
}

int md5_file(const char *path, char out_hex[33]) {
    FILE *fp = fopen(path, "rb");
    MD5_CTX_LOCAL ctx;
    unsigned char buf[8192], digest[16];
    static const char hex[] = "0123456789abcdef";
    size_t n;
    int i;
    if (!fp) return -1;
    md5_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) md5_update(&ctx, buf, n);
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    md5_final(digest, &ctx);
    for (i = 0; i < 16; i++) {
        out_hex[i * 2] = hex[(digest[i] >> 4) & 0xf];
        out_hex[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out_hex[32] = '\0';
    return 0;
}

int parse_tracker_text(const char *text, TrackerInfo *info) {
    char *copy, *line, *saveptr = NULL;
    char value[MAX_LINE];
    if (!text || !info) return -1;
    memset(info, 0, sizeof(*info));
    copy = strdup(text);
    if (!copy) return -1;
    for (line = strtok_r(copy, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        trim(line);
        if (!*line || line[0] == '#') continue;
        if (strncmp(line, "Filename:", 9) == 0) {
            snprintf(value, sizeof(value), "%s", line + 9);
            trim(value);
            copy_value(info->filename, sizeof(info->filename), value);
        } else if (strncmp(line, "Filesize:", 9) == 0) {
            info->filesize = atol(line + 9);
        } else if (strncmp(line, "Description:", 12) == 0) {
            snprintf(value, sizeof(value), "%s", line + 12);
            trim(value);
            copy_value(info->description, sizeof(info->description), value);
        } else if (strncmp(line, "MD5:", 4) == 0) {
            snprintf(value, sizeof(value), "%s", line + 4);
            trim(value);
            copy_value(info->md5, sizeof(info->md5), value);
        } else if (strchr(line, ':') && info->peer_count < MAX_PEERS) {
            PeerEntry *p = &info->peers[info->peer_count];
            if (sscanf(line, "%63[^:]:%d:%ld:%ld:%ld", p->ip, &p->port, &p->start, &p->end, &p->timestamp) == 5) {
                info->peer_count++;
            }
        }
    }
    free(copy);
    return info->filename[0] ? 0 : -1;
}
