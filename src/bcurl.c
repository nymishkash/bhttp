/* bcurl: BHTTP/1 client. See README.md for usage. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HDR_LEN       8
#define T_REQUEST     0x01
#define T_RESPONSE    0x02
#define T_DATA        0x03
#define F_END_STREAM  0x01
#define M_GET         0x01
#define M_HEAD        0x02
#define MAX_EXTRA     32

static const char *NAMES[] = { NULL, "host", "user-agent", "accept",
    "content-type", "content-length", "last-modified", "etag", "server",
    "date", "cache-control" };
#define N_NAMES 10

static int verbose;

typedef struct {
    char host[256], port[8], authority[300], path[4096];
    size_t path_len;
} url_t;

/* ---------------------------------------------------------------- I/O */

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

/* ----------------------------------------------------------- hexdump */

static const char *type_name(uint8_t t)
{
    switch (t) {
    case T_REQUEST:  return "REQUEST";
    case T_RESPONSE: return "RESPONSE";
    case T_DATA:     return "DATA";
    }
    return "UNKNOWN";
}

static void dump_frame(const char *dir, const uint8_t *f, size_t len)
{
    uint8_t type = f[3], flags = f[4];
    unsigned stream = ((unsigned)f[6] << 8) | f[7];
    int known = type >= T_REQUEST && type <= T_DATA;
    fprintf(stderr, "%s %s(0x%02x) stream=%u flags=0x%02x%s length=%zu\n",
            dir, type_name(type), type, stream, flags,
            known && (flags & F_END_STREAM) ? " END_STREAM" : "", len);

    size_t n = HDR_LEN + len;
    for (size_t off = 0; off < n; off += 16) {
        fprintf(stderr, "%s %04zx  ", dir, off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < n) fprintf(stderr, "%02x ", f[off + i]);
            else fputs("   ", stderr);
            if (i == 7) fputc(' ', stderr);
        }
        fputs(" |", stderr);
        for (size_t i = 0; i < 16 && off + i < n; i++) {
            uint8_t c = f[off + i];
            fputc(c >= 0x20 && c < 0x7f ? c : '.', stderr);
        }
        fputs("|\n", stderr);
    }
}

/* --------------------------------------------------------------- URL */

static int hexval(int c)
{
    return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10;
}

static int parse_url(const char *s, url_t *u)
{
    const char *hs, *he;
    if (!strncmp(s, "bhttp://", 8)) s += 8;

    if (*s == '[') {
        hs = s + 1;
        he = strchr(s, ']');
        if (!he) return -1;
        s = he + 1;
    } else {
        hs = s;
        he = s + strcspn(s, ":/");
        s = he;
    }
    size_t hl = (size_t)(he - hs);
    if (hl == 0 || hl >= sizeof u->host) return -1;
    memcpy(u->host, hs, hl);
    u->host[hl] = '\0';

    if (*s == ':') {
        s++;
        size_t pl = strcspn(s, "/");
        if (pl == 0 || pl >= sizeof u->port) return -1;
        memcpy(u->port, s, pl);
        u->port[pl] = '\0';
        s += pl;
    } else {
        strcpy(u->port, "9000");
    }
    snprintf(u->authority, sizeof u->authority,
             strchr(u->host, ':') ? "[%s]:%s" : "%s:%s", u->host, u->port);

    /* paths are sent raw, so decode %XX */
    const char *raw = *s ? s : "/";
    size_t o = 0;
    for (size_t i = 0; raw[i] && raw[i] != '#'; i++) {
        if (o + 1 >= sizeof u->path) return -1;
        if (raw[i] == '%' && isxdigit((unsigned char)raw[i + 1]) &&
            isxdigit((unsigned char)raw[i + 2])) {
            int c = hexval(raw[i + 1]) * 16 + hexval(raw[i + 2]);
            if (c == 0) return -1;
            u->path[o++] = (char)c;
            i += 2;
        } else {
            u->path[o++] = raw[i];
        }
    }
    u->path[o] = '\0';
    u->path_len = o;
    return 0;
}

/* ------------------------------------------------------ header blocks */

typedef struct { uint8_t b[65536]; size_t n; int overflow; } block;

static void put(block *k, const void *p, size_t n)
{
    if (k->n + n > sizeof k->b) { k->overflow = 1; return; }
    memcpy(k->b + k->n, p, n);
    k->n += n;
}

static int name_index(const char *name)
{
    for (int i = 1; i <= N_NAMES; i++)
        if (!strcmp(NAMES[i], name)) return i;
    return 0;
}

static void put_field(block *k, const char *name, const char *value, size_t vl)
{
    int idx = name_index(name);
    if (idx) {
        uint8_t b = (uint8_t)(0x80 | idx);
        put(k, &b, 1);
    } else {
        size_t nl = strlen(name);
        uint8_t b[2] = { 0x00, (uint8_t)nl };
        put(k, b, 2);
        put(k, name, nl);
    }
    uint8_t l[2] = { (uint8_t)(vl >> 8), (uint8_t)vl };
    put(k, l, 2);
    put(k, value, vl);
}

typedef struct { char name[256]; const char *value; } extra_t;

static int send_request(int fd, uint16_t stream, int method, const url_t *u,
                        const extra_t *ex, int nex)
{
    static block k;
    k.n = HDR_LEN;
    k.overflow = 0;

    uint8_t pre[3] = { (uint8_t)method, (uint8_t)(u->path_len >> 8),
                       (uint8_t)u->path_len };
    put(&k, pre, 3);
    put(&k, u->path, u->path_len);

    static const char *dflt[][2] = { { "host", NULL },
        { "user-agent", "bcurl/1.0" }, { "accept", "*/*" } };
    for (int d = 0; d < 3; d++) {
        int overridden = 0;
        for (int i = 0; i < nex; i++)
            if (!strcmp(ex[i].name, dflt[d][0])) overridden = 1;
        if (overridden) continue;
        const char *v = d == 0 ? u->authority : dflt[d][1];
        put_field(&k, dflt[d][0], v, strlen(v));
    }
    for (int i = 0; i < nex; i++)
        put_field(&k, ex[i].name, ex[i].value, strlen(ex[i].value));

    size_t len = k.n - HDR_LEN;
    if (k.overflow || len > 0xFFFFFF) {
        fprintf(stderr, "bcurl: request too large\n");
        return -1;
    }
    k.b[0] = (uint8_t)(len >> 16);
    k.b[1] = (uint8_t)(len >> 8);
    k.b[2] = (uint8_t)len;
    k.b[3] = T_REQUEST;
    k.b[4] = F_END_STREAM;
    k.b[5] = 0;
    k.b[6] = (uint8_t)(stream >> 8);
    k.b[7] = (uint8_t)stream;

    if (verbose) {
        dump_frame(">", k.b, len);
        fprintf(stderr, "> %s %s\n", method == M_HEAD ? "HEAD" : "GET", u->path);
    }
    return write_full(fd, k.b, k.n);
}

static int parse_response(const uint8_t *p, size_t n, int head)
{
    if (n < 2) return -1;
    int status = (p[0] << 8) | p[1];
    if (verbose) fprintf(stderr, "< status: %d\n", status);
    if (head) printf("status: %d\n", status);

    size_t off = 2;
    while (off < n) {
        char name[300];
        uint8_t b = p[off++];
        if (b == 0x00) {
            if (off >= n) return -1;
            size_t nl = p[off++];
            if (nl == 0 || off + nl > n) return -1;
            snprintf(name, sizeof name, "%.*s", (int)nl, (const char *)p + off);
            off += nl;
        } else if ((b & 0x80) && b != 0x80) {
            int idx = b & 0x7f;
            if (idx <= N_NAMES) snprintf(name, sizeof name, "%s", NAMES[idx]);
            else snprintf(name, sizeof name, "#%d", idx);
        } else {
            return -1;
        }
        if (off + 2 > n) return -1;
        size_t vl = ((size_t)p[off] << 8) | p[off + 1];
        off += 2;
        if (off + vl > n) return -1;
        if (verbose)
            fprintf(stderr, "< %s: %.*s\n", name, (int)vl, (const char *)p + off);
        if (head) printf("%s: %.*s\n", name, (int)vl, (const char *)p + off);
        off += vl;
    }
    return status;
}

/* returns status, or -1 */
static int await_response(int fd, uint16_t want, int head)
{
    int status = -1;
    for (;;) {
        uint8_t h[HDR_LEN];
        if (read_full(fd, h, HDR_LEN) != 1) {
            fprintf(stderr, "bcurl: connection closed before stream %u ended\n", want);
            return -1;
        }
        size_t len = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
        uint8_t type = h[3], flags = h[4];
        uint16_t stream = (uint16_t)((h[6] << 8) | h[7]);

        uint8_t *f = malloc(HDR_LEN + len);
        if (!f) return -1;
        memcpy(f, h, HDR_LEN);
        if (len && read_full(fd, f + HDR_LEN, len) != 1) {
            fprintf(stderr, "bcurl: short frame\n");
            free(f);
            return -1;
        }
        if (verbose) dump_frame("<", f, len);
        const uint8_t *p = f + HDR_LEN;
        int done = 0;

        if (type == T_RESPONSE || type == T_DATA) {
            if (stream != want) {
                fprintf(stderr, "bcurl: ignoring frame for stream %u\n", stream);
            } else if (type == T_RESPONSE) {
                if (status != -1 || (status = parse_response(p, len, head)) < 0) {
                    fprintf(stderr, "bcurl: malformed or duplicate RESPONSE\n");
                    free(f);
                    return -1;
                }
                done = flags & F_END_STREAM;
            } else {
                if (status == -1) {
                    fprintf(stderr, "bcurl: DATA before RESPONSE\n");
                    free(f);
                    return -1;
                }
                if (!head) fwrite(p, 1, len, stdout);
                done = flags & F_END_STREAM;
            }
        } else if (verbose) {
            fprintf(stderr, "* skipped frame type 0x%02x (%zu bytes)\n", type, len);
        }
        free(f);
        if (done) { fflush(stdout); return status; }
    }
}

static int dial(const char *host, const char *port)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host, port, &hints, &res);
    if (e) {
        fprintf(stderr, "bcurl: %s: %s\n", host, gai_strerror(e));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "bcurl: cannot connect to %s:%s\n", host, port);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (verbose) fprintf(stderr, "* connected to %s port %s\n", host, port);
    return fd;
}

static void usage(void)
{
    fprintf(stderr, "usage: bcurl [-v] [-I] [-H 'name: value']... URL [URL...]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int head = 0, nex = 0, opt;
    extra_t ex[MAX_EXTRA];

    while ((opt = getopt(argc, argv, "vIH:")) != -1) {
        switch (opt) {
        case 'v': verbose = 1; break;
        case 'I': head = 1; break;
        case 'H': {
            const char *colon = strchr(optarg, ':');
            size_t nl = colon ? (size_t)(colon - optarg) : 0;
            if (!colon || nl == 0 || nl > 255 || nex == MAX_EXTRA) usage();
            for (size_t i = 0; i < nl; i++)
                ex[nex].name[i] = (char)tolower((unsigned char)optarg[i]);
            ex[nex].name[nl] = '\0';
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            ex[nex++].value = v;
            break;
        }
        default: usage();
        }
    }
    int nurl = argc - optind;
    if (nurl < 1) usage();
    if (nurl > 65535) { fprintf(stderr, "bcurl: too many URLs\n"); return 2; }

    url_t *urls = calloc((size_t)nurl, sizeof *urls);
    if (!urls) return 3;
    for (int i = 0; i < nurl; i++) {
        if (parse_url(argv[optind + i], &urls[i]) < 0 || urls[i].path[0] != '/') {
            fprintf(stderr, "bcurl: bad URL %s\n", argv[optind + i]);
            return 2;
        }
        if (strcmp(urls[i].host, urls[0].host) || strcmp(urls[i].port, urls[0].port)) {
            fprintf(stderr, "bcurl: all URLs must have the same host:port\n");
            return 2;
        }
    }

    int fd = dial(urls[0].host, urls[0].port);
    if (fd < 0) return 3;

    int rc = 0;
    for (int i = 0; i < nurl; i++) {
        uint16_t stream = (uint16_t)(i + 1);
        if (send_request(fd, stream, head ? M_HEAD : M_GET, &urls[i], ex, nex) < 0) {
            fprintf(stderr, "bcurl: write failed\n");
            return 3;
        }
        int status = await_response(fd, stream, head);
        if (status < 0) return 3;
        if (status >= 400) rc = 1;
    }
    close(fd);
    return rc;
}
