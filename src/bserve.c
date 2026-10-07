/* bserve: BHTTP/1 static file server. usage: bserve <root-dir> <port> */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HDR_LEN        8
#define T_REQUEST      0x01
#define T_RESPONSE     0x02
#define T_DATA         0x03
#define F_END_STREAM   0x01
#define M_GET          0x01
#define M_HEAD         0x02
#define MAX_REQ_BLOCK  65536
#define DATA_CHUNK     16384
#define IDLE_SECONDS   60

enum { H_HOST = 1, H_USER_AGENT, H_ACCEPT, H_CONTENT_TYPE, H_CONTENT_LENGTH,
       H_LAST_MODIFIED, H_ETAG, H_SERVER, H_DATE, H_CACHE_CONTROL };

static char root_dir[PATH_MAX];

/* ---------------------------------------------------------------- I/O */

/* 1 = ok, 0 = EOF, -1 = error or short read */
static int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r == 0) return got == 0 ? 0 : -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)w;
    }
    return 0;
}

static int discard(int fd, size_t n)
{
    char tmp[4096];
    while (n) {
        size_t k = n < sizeof tmp ? n : sizeof tmp;
        if (read_full(fd, tmp, k) != 1) return -1;
        n -= k;
    }
    return 0;
}

static int send_frame(int fd, uint8_t type, uint8_t flags, uint16_t stream,
                      const void *payload, size_t len)
{
    static uint8_t out[HDR_LEN + DATA_CHUNK];
    if (len > DATA_CHUNK) return -1;
    out[0] = (uint8_t)(len >> 16);
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)len;
    out[3] = type;
    out[4] = flags;
    out[5] = 0;
    out[6] = (uint8_t)(stream >> 8);
    out[7] = (uint8_t)stream;
    if (len) memcpy(out + HDR_LEN, payload, len);
    return write_full(fd, out, HDR_LEN + len);
}

/* ------------------------------------------------------ header blocks */

typedef struct { uint8_t b[4096]; size_t n; } block;

static void put(block *k, const void *p, size_t n)
{
    if (k->n + n > sizeof k->b) return;
    memcpy(k->b + k->n, p, n);
    k->n += n;
}

static void put_field(block *k, int idx, const char *value)
{
    uint8_t first = (uint8_t)(0x80 | idx);
    size_t vl = strlen(value);
    uint8_t l[2] = { (uint8_t)(vl >> 8), (uint8_t)vl };
    put(k, &first, 1);
    put(k, l, 2);
    put(k, value, vl);
}

static int parse_request(const uint8_t *p, size_t n, int *method,
                         char *path, size_t cap)
{
    if (n < 3) return -1;
    *method = p[0];
    size_t pl = ((size_t)p[1] << 8) | p[2];
    if (pl == 0 || pl >= cap || 3 + pl > n) return -1;
    if (memchr(p + 3, 0, pl)) return -1;
    memcpy(path, p + 3, pl);
    path[pl] = '\0';

    size_t off = 3 + pl;
    while (off < n) {
        uint8_t b = p[off++];
        if (b == 0x00) {
            if (off >= n) return -1;
            size_t nl = p[off++];
            if (nl == 0 || off + nl > n) return -1;
            off += nl;
        } else if (!(b & 0x80) || b == 0x80) {
            return -1;
        }
        if (off + 2 > n) return -1;
        size_t vl = ((size_t)p[off] << 8) | p[off + 1];
        off += 2;
        if (off + vl > n) return -1;
        off += vl;
    }
    return 0;
}

/* ------------------------------------------------------------ helpers */

static void http_date(time_t t, char *out, size_t cap)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static const char *mime_for(const char *path)
{
    static const struct { const char *ext, *type; } t[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".md",   "text/markdown; charset=utf-8" },
        { ".css",  "text/css" },
        { ".js",   "text/javascript" },
        { ".json", "application/json" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".svg",  "image/svg+xml" },
        { ".ico",  "image/x-icon" },
        { ".pdf",  "application/pdf" },
    };
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash))
        for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
            if (!strcasecmp(dot, t[i].ext)) return t[i].type;
    return "application/octet-stream";
}

static const char *reason(int s)
{
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    }
    return "";
}

static void log_req(uint16_t stream, const char *m, const char *path, int status)
{
    fprintf(stderr, "bserve conn=%d stream=%u %s %s -> %d\n",
            (int)getpid(), stream, m, path, status);
}

/* ------------------------------------------------------------ replies */

static int send_head(int fd, uint16_t stream, int status, const char *ctype,
                     long long clen, const struct stat *st, int end)
{
    block k; char tmp[64];
    k.n = 0;
    uint8_t s[2] = { (uint8_t)(status >> 8), (uint8_t)status };
    put(&k, s, 2);
    put_field(&k, H_CONTENT_TYPE, ctype);
    snprintf(tmp, sizeof tmp, "%lld", clen);
    put_field(&k, H_CONTENT_LENGTH, tmp);
    if (st) {
        http_date(st->st_mtime, tmp, sizeof tmp);
        put_field(&k, H_LAST_MODIFIED, tmp);
        snprintf(tmp, sizeof tmp, "\"%llx-%llx\"",
                 (unsigned long long)st->st_mtime,
                 (unsigned long long)st->st_size);
        put_field(&k, H_ETAG, tmp);
        put_field(&k, H_CACHE_CONTROL, "max-age=60");
    }
    put_field(&k, H_SERVER, "bserve/1.0");
    http_date(time(NULL), tmp, sizeof tmp);
    put_field(&k, H_DATE, tmp);
    return send_frame(fd, T_RESPONSE, end ? F_END_STREAM : 0, stream, k.b, k.n);
}

static int send_error(int fd, uint16_t stream, int status, int head_only)
{
    char body[64];
    int bl = snprintf(body, sizeof body, "%d %s\n", status, reason(status));
    if (send_head(fd, stream, status, "text/plain; charset=utf-8", bl, NULL,
                  head_only) < 0)
        return -1;
    if (head_only) return 0;
    return send_frame(fd, T_DATA, F_END_STREAM, stream, body, (size_t)bl);
}

/* Map a request path to a regular file inside root. 0 = ok, else status. */
static int resolve(const char *path, char *out, struct stat *st)
{
    char want[PATH_MAX * 2], real[PATH_MAX];
    size_t rl = strlen(root_dir);
    int trailing = path[strlen(path) - 1] == '/';

    if (snprintf(want, sizeof want, "%s%s%s", root_dir, path,
                 trailing ? "index.html" : "") >= (int)sizeof want)
        return 404;

    for (int pass = 0; pass < 2; pass++) {
        if (!realpath(want, real)) return errno == EACCES ? 403 : 404;
        if (rl > 1 && (strncmp(real, root_dir, rl) != 0 ||
                       (real[rl] != '/' && real[rl] != '\0')))
            return 403;
        if (stat(real, st) < 0) return 404;
        if (S_ISREG(st->st_mode)) { strcpy(out, real); return 0; }
        if (!S_ISDIR(st->st_mode) || pass) return 404;
        snprintf(want, sizeof want, "%s/index.html", real);
    }
    return 404;
}

static int handle_request(int fd, uint16_t stream, const uint8_t *p, size_t n)
{
    int method;
    char path[4096], file[PATH_MAX];
    struct stat st;

    if (parse_request(p, n, &method, path, sizeof path) < 0 || path[0] != '/') {
        log_req(stream, "?", "(malformed)", 400);
        return send_error(fd, stream, 400, 0);
    }
    const char *m = method == M_GET ? "GET" : method == M_HEAD ? "HEAD" : "?";
    if (method != M_GET && method != M_HEAD) {
        log_req(stream, m, path, 501);
        return send_error(fd, stream, 501, 0);
    }
    int head = method == M_HEAD;

    int s = resolve(path, file, &st);
    if (s) { log_req(stream, m, path, s); return send_error(fd, stream, s, head); }

    int f = open(file, O_RDONLY);
    if (f < 0) {
        s = errno == EACCES ? 403 : 404;
        log_req(stream, m, path, s);
        return send_error(fd, stream, s, head);
    }
    if (fstat(f, &st) < 0) {
        close(f);
        log_req(stream, m, path, 500);
        return send_error(fd, stream, 500, head);
    }

    long long size = st.st_size;
    int rc = send_head(fd, stream, 200, mime_for(file), size, &st,
                       head || size == 0);
    if (rc == 0 && !head && size > 0) {
        static uint8_t chunk[DATA_CHUNK];
        long long left = size;
        while (left > 0) {
            ssize_t r = read(f, chunk, left < DATA_CHUNK ? (size_t)left : DATA_CHUNK);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {            /* file shrank */
                rc = send_frame(fd, T_DATA, F_END_STREAM, stream, NULL, 0);
                break;
            }
            left -= r;
            rc = send_frame(fd, T_DATA, left == 0 ? F_END_STREAM : 0, stream,
                            chunk, (size_t)r);
            if (rc < 0) break;
        }
    }
    close(f);
    log_req(stream, m, path, 200);
    return rc;
}

/* --------------------------------------------------------- connection */

static void serve_conn(int fd)
{
    static uint8_t payload[MAX_REQ_BLOCK];
    uint8_t h[HDR_LEN];

    for (;;) {
        if (read_full(fd, h, HDR_LEN) != 1) break;
        size_t len = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
        uint8_t type = h[3];
        uint16_t stream = (uint16_t)((h[6] << 8) | h[7]);

        if (type != T_REQUEST) {
            /* skip unknown types, and RESPONSE/DATA from a client */
            if (discard(fd, len) < 0) break;
            continue;
        }
        if (len > MAX_REQ_BLOCK) {
            if (discard(fd, len) < 0) break;
            log_req(stream, "?", "(header block too large)", 400);
            if (send_error(fd, stream, 400, 0) < 0) break;
            continue;
        }
        if (len && read_full(fd, payload, len) != 1) break;
        if (handle_request(fd, stream, payload, len) < 0) break;
    }
    close(fd);
}

static int listen_on(uint16_t port)
{
    int one = 1, zero = 0;
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_in6 a;
        memset(&a, 0, sizeof a);
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(port);
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 64) == 0)
            return s;
        close(s);
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); exit(1); }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
    if (listen(s, 64) < 0) { perror("listen"); exit(1); }
    return s;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <root-dir> <port>\n", argv[0]);
        return 2;
    }
    struct stat st;
    if (!realpath(argv[1], root_dir) || stat(root_dir, &st) < 0 ||
        !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "bserve: %s is not a directory\n", argv[1]);
        return 2;
    }
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (*end || port < 1 || port > 65535) {
        fprintf(stderr, "bserve: bad port %s\n", argv[2]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    int ls = listen_on((uint16_t)port);
    fprintf(stderr, "bserve: serving %s on port %ld\n", root_dir, port);

    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { if (errno != EINTR) perror("accept"); continue; }
        pid_t pid = fork();
        if (pid == 0) {
            int one = 1;
            struct timeval tv = { IDLE_SECONDS, 0 };
            close(ls);
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            serve_conn(c);
            _exit(0);
        }
        if (pid < 0) perror("fork");
        close(c);
    }
}
