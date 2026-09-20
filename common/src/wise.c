#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "vapor/wise.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "miniz.h"
#include "vapor/util.h"

#define WISE_MAX_SCRIPT (8u * 1024u * 1024u)
#define WISE_MAX_FILE (512u * 1024u * 1024u)
#define WISE_MAX_FILES 4096
#define WISE_MAX_SKIP 64
#define WISE_HDR 43

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t size;
    uint32_t crc;
    char     dest[260];
} wise_file;

static void
set_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz) {
        snprintf(err, errsz, "%s", msg);
    }
}

static uint16_t
u16le(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t
u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static const unsigned char *
find_mem(const unsigned char *h, size_t hlen, const char *n)
{
    size_t nlen, i;

    nlen = strlen(n);
    if (nlen == 0 || nlen > hlen) {
        return NULL;
    }
    for (i = 0; i + nlen <= hlen; i++) {
        if (memcmp(h + i, n, nlen) == 0) {
            return h + i;
        }
    }
    return NULL;
}

static int
mkdirs(const char *path)
{
    char   tmp[1024];
    size_t i, n;

    n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] && MKDIR(tmp) != 0 && errno != EEXIST) {
#if defined(_WIN32)
                if (strlen(tmp) != 2 || tmp[1] != ':') {
                    return -1;
                }
#else
                return -1;
#endif
            }
            tmp[i] = saved;
        }
    }
    return 0;
}

static int
mkdirs_parent(const char *path)
{
    char  tmp[1024];
    char *slash;

    snprintf(tmp, sizeof(tmp), "%s", path);
    slash = strrchr(tmp, '/');
#if defined(_WIN32)
    {
        char *b = strrchr(tmp, '\\');
        if (b > slash) {
            slash = b;
        }
    }
#endif
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    return *tmp ? mkdirs(tmp) : 0;
}

static int
pe_overlay_off(const unsigned char *data, size_t len, size_t *out)
{
    uint32_t e_lfanew, optsz, sec, i, nsec, end = 0;

    if (len < 64 || data[0] != 'M' || data[1] != 'Z') {
        return -1;
    }
    e_lfanew = u32le(data + 0x3c);
    if (e_lfanew < 64 || (size_t)e_lfanew + 24 > len) {
        return -1;
    }
    if (memcmp(data + e_lfanew, "PE\0\0", 4) != 0) {
        return -1;
    }
    nsec = u16le(data + e_lfanew + 6);
    optsz = u16le(data + e_lfanew + 20);
    sec = e_lfanew + 24 + optsz;
    if (nsec == 0 || nsec > 96 || (size_t)sec + nsec * 40 > len) {
        return -1;
    }
    for (i = 0; i < nsec; i++) {
        uint32_t rs = u32le(data + sec + i * 40 + 16);
        uint32_t rp = u32le(data + sec + i * 40 + 20);
        if (rp + rs > end) {
            end = rp + rs;
        }
    }
    if (end == 0 || (size_t)end >= len) {
        return -1;
    }
    *out = end;
    return 0;
}

static int
inflate_raw(const unsigned char *src, size_t src_len, size_t max_out,
            unsigned char **out, size_t *out_len, size_t *consumed)
{
    mz_stream      s;
    unsigned char *buf;
    size_t         cap = 64 * 1024;
    int            z;

    if (!src || src_len == 0) {
        return -1;
    }
    memset(&s, 0, sizeof(s));
    if (mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
        return -1;
    }
    buf = (unsigned char *)malloc(cap);
    if (!buf) {
        mz_inflateEnd(&s);
        return -1;
    }
    s.next_in = src;
    s.avail_in = src_len > 0xffffffffu ? 0xffffffffu : (unsigned)src_len;
    for (;;) {
        if (s.total_out >= max_out) {
            free(buf);
            mz_inflateEnd(&s);
            return -1;
        }
        if (cap - (size_t)s.total_out < 4096) {
            unsigned char *nb;
            size_t         ncap = cap * 2;
            if (ncap > max_out) {
                ncap = max_out;
            }
            if (ncap <= cap) {
                free(buf);
                mz_inflateEnd(&s);
                return -1;
            }
            nb = (unsigned char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                mz_inflateEnd(&s);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        s.next_out = buf + s.total_out;
        s.avail_out = (unsigned)(cap - (size_t)s.total_out);
        z = mz_inflate(&s, MZ_NO_FLUSH);
        if (z == MZ_STREAM_END) {
            break;
        }
        if (z != MZ_OK) {
            free(buf);
            mz_inflateEnd(&s);
            return -1;
        }
        if (s.avail_in == 0 && s.avail_out > 0) {
            free(buf);
            mz_inflateEnd(&s);
            return -1;
        }
    }
    mz_inflateEnd(&s);
    *out = buf;
    *out_len = (size_t)s.total_out;
    *consumed = (size_t)s.total_in;
    return 0;
}

static int
inflate_known(const unsigned char *src, size_t src_len, size_t expect,
              unsigned char *out, size_t *consumed)
{
    mz_stream s;
    int       z;

    memset(&s, 0, sizeof(s));
    if (mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
        return -1;
    }
    s.next_in = src;
    s.avail_in = src_len > 0xffffffffu ? 0xffffffffu : (unsigned)src_len;
    s.next_out = out;
    s.avail_out = expect > 0xffffffffu ? 0xffffffffu : (unsigned)expect;
    z = mz_inflate(&s, MZ_FINISH);
    *consumed = (size_t)s.total_in;
    mz_inflateEnd(&s);
    return (z == MZ_STREAM_END && (size_t)s.total_out == expect) ? 0 : -1;
}

static int
valid_dest(const char *dest)
{
    const char *p;

    if (!vapor_str_has_prefix(dest, "%") || !strchr(dest, '\\')) {
        return 0;
    }
    p = dest;
    if (strstr(dest, "..")) {
        return 0;
    }
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c < 32 || c == '/' || c == ':') {
            return 0;
        }
    }
    return 1;
}

static int
is_maindir(const char *dest)
{
    return vapor_str_has_prefix(dest, "%MAINDIR%\\") && dest[10] != '\0';
}

static int
skip_maindir(const char *dest)
{
    const char *base = dest + 10;
    const char *slash = strrchr(base, '\\');
    if (slash) {
        base = slash + 1;
    }
    return vapor_str_eq_ci(base, "INSTALL.LOG")
        || vapor_str_eq_ci(base, "UNWISE.EXE")
        || vapor_str_eq_ci(base, "UNWISE.INI");
}

static int
parse_script(const unsigned char *script, size_t len, wise_file *files,
             size_t *nfiles)
{
    size_t n = 0, i = 0;

    while (i + 11 < len) {
        const unsigned char *hit;
        size_t               p, h;
        uint32_t             start, end, size, crc, comp;
        char                 dest[260];
        size_t               dlen;

        hit = find_mem(script + i, len - i, "%");
        if (!hit) {
            break;
        }
        p = (size_t)(hit - script);
        i = p + 1;
        if (p < WISE_HDR || script[p - WISE_HDR] != 0x00
            || script[p - WISE_HDR + 2] != 0) {
            continue;
        }
        h = p - WISE_HDR;
        start = u32le(script + h + 3);
        end = u32le(script + h + 7);
        size = u32le(script + h + 15);
        crc = u32le(script + h + 39);
        dlen = 0;
        while (p + dlen < len && script[p + dlen] != 0 && dlen + 1 < sizeof(dest)) {
            dest[dlen] = (char)script[p + dlen];
            dlen++;
        }
        dest[dlen] = '\0';
        if (p + dlen >= len || script[p + dlen] != 0) {
            continue;
        }
        if (!valid_dest(dest) || end <= start || size == 0 || size > WISE_MAX_FILE) {
            continue;
        }
        comp = end - start;
        if (comp < 5 || (comp - 4 > size + 4096 && size > 64)) {
            continue;
        }
        if (n >= WISE_MAX_FILES) {
            break;
        }
        files[n].start = start;
        files[n].end = end;
        files[n].size = size;
        files[n].crc = crc;
        snprintf(files[n].dest, sizeof(files[n].dest), "%s", dest);
        n++;
        i = p + dlen + 1;
    }
    *nfiles = n;
    return n > 0 ? 0 : -1;
}

static int
write_file(const char *path, const unsigned char *data, size_t n)
{
    FILE *f;

    if (mkdirs_parent(path) != 0) {
        return -1;
    }
    f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(data, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int
rel_from_maindir(const char *dest, char *out, size_t outsz)
{
    const char *p = dest + 10;
    size_t      o = 0;

    while (*p && o + 1 < outsz) {
        char ch = *p++;
        if (ch == '\\') {
            ch = '/';
        }
        out[o++] = ch;
    }
    out[o] = '\0';
    return o > 0 ? 0 : -1;
}

int
vapor_wise_extract(const char *exe_path, const char *dest_dir, char *err,
                   size_t errsz)
{
    FILE                *f;
    unsigned char       *data = NULL, *script = NULL, *out = NULL;
    size_t               file_len = 0, ov_off = 0, ov_len, first_off, cons;
    size_t               s0_len, script_len, nfiles = 0, i, written = 0;
    long long            flen;
    const unsigned char *ov, *mark;
    wise_file           *files = NULL;
    wise_file           *anchor = NULL;
    size_t               base = (size_t)-1, walk;
    int                  rc = 1;

    if (!exe_path || !dest_dir) {
        set_err(err, errsz, "Wise extract arguments are missing");
        return -1;
    }
    f = fopen(exe_path, "rb");
    if (!f) {
        set_err(err, errsz, "cannot open installer");
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err, errsz, "cannot size installer");
        return -1;
    }
    flen = ftell(f);
    if (flen < 256 || flen > (long long)(2u * 1024u * 1024u * 1024u)) {
        fclose(f);
        return 1;
    }
    file_len = (size_t)flen;
    rewind(f);
    data = (unsigned char *)malloc(file_len);
    if (!data) {
        fclose(f);
        set_err(err, errsz, "out of memory");
        return -1;
    }
    if (fread(data, 1, file_len, f) != file_len) {
        fclose(f);
        free(data);
        set_err(err, errsz, "cannot read installer");
        return -1;
    }
    fclose(f);

    if (pe_overlay_off(data, file_len, &ov_off) != 0) {
        free(data);
        return 1;
    }
    ov = data + ov_off;
    ov_len = file_len - ov_off;
    mark = find_mem(ov, ov_len < 4096 ? ov_len : 4096,
                    "Initializing Wise Installation Wizard");
    if (!mark) {
        mark = find_mem(ov, ov_len < 65536 ? ov_len : 65536, "WiseMain");
        if (!mark) {
            free(data);
            return 1;
        }
        first_off = 0;
        for (i = 0; i < 512 && i < ov_len; i++) {
            unsigned char *tmp = NULL;
            size_t         n = 0, c = 0;
            if (inflate_raw(ov + i, ov_len - i, 2 * 1024 * 1024, &tmp, &n, &c)
                == 0 && n > 16 && c > 8) {
                first_off = i;
                free(tmp);
                break;
            }
            free(tmp);
        }
        if (first_off == 0 && i >= 512) {
            free(data);
            return 1;
        }
    } else {
        first_off = (size_t)(mark - ov) + strlen("Initializing Wise Installation Wizard");
        while (first_off < ov_len && ov[first_off] != 0) {
            first_off++;
        }
        if (first_off < ov_len && ov[first_off] == 0) {
            first_off++;
        }
    }

    if (inflate_raw(ov + first_off, ov_len - first_off, 2 * 1024 * 1024, &out,
                    &s0_len, &cons)
        != 0) {
        free(data);
        return 1;
    }
    (void)s0_len;
    free(out);
    out = NULL;
    walk = first_off + cons + 4;
    if (walk >= ov_len
        || inflate_raw(ov + walk, ov_len - walk, WISE_MAX_SCRIPT, &script,
                       &script_len, &cons)
               != 0) {
        free(data);
        set_err(err, errsz, "cannot inflate Wise script");
        return -1;
    }
    walk += cons + 4;

    files = (wise_file *)calloc(WISE_MAX_FILES, sizeof(*files));
    if (!files || parse_script(script, script_len, files, &nfiles) != 0) {
        free(files);
        free(script);
        free(data);
        set_err(err, errsz, "Wise script has no packed files");
        return -1;
    }
    free(script);
    script = NULL;

    for (i = 0; i < nfiles; i++) {
        if (!anchor || files[i].start < anchor->start) {
            anchor = &files[i];
        }
    }
    for (i = 0; i < WISE_MAX_SKIP && walk + 8 < ov_len; i++) {
        size_t n = 0, c = 0;
        uint32_t crc;
        if (inflate_raw(ov + walk, ov_len - walk, WISE_MAX_FILE, &out, &n, &c)
            != 0) {
            break;
        }
        crc = 0;
        if (walk + c + 4 <= ov_len) {
            crc = u32le(ov + walk + c);
        }
        if (n == anchor->size
            && (anchor->crc == 0 || crc == anchor->crc
                || mz_crc32(MZ_CRC32_INIT, out, n) == anchor->crc)) {
            if (walk >= anchor->start) {
                base = walk - anchor->start;
            }
            free(out);
            out = NULL;
            break;
        }
        free(out);
        out = NULL;
        walk += c + 4;
    }
    if (base == (size_t)-1) {
        free(files);
        free(data);
        set_err(err, errsz, "cannot locate Wise packed data");
        return -1;
    }

    if (mkdirs(dest_dir) != 0) {
        free(files);
        free(data);
        set_err(err, errsz, "cannot create Wise extract directory");
        return -1;
    }

    rc = -1;
    for (i = 0; i < nfiles; i++) {
        char          rel[260];
        char          abs[1024];
        unsigned char *payload;
        size_t         src_off, c = 0;
        uint32_t       crc;

        if (!is_maindir(files[i].dest) || skip_maindir(files[i].dest)) {
            continue;
        }
        if (rel_from_maindir(files[i].dest, rel, sizeof(rel)) != 0) {
            continue;
        }
        src_off = base + files[i].start;
        if (src_off >= ov_len) {
            continue;
        }
        payload = (unsigned char *)malloc(files[i].size);
        if (!payload) {
            set_err(err, errsz, "out of memory");
            goto done;
        }
        if (inflate_known(ov + src_off, ov_len - src_off, files[i].size, payload,
                          &c)
            != 0) {
            free(payload);
            continue;
        }
        crc = mz_crc32(MZ_CRC32_INIT, payload, files[i].size);
        if (files[i].crc != 0 && crc != files[i].crc) {
            free(payload);
            continue;
        }
        if ((size_t)snprintf(abs, sizeof(abs), "%s/%s", dest_dir, rel)
            >= sizeof(abs)) {
            free(payload);
            continue;
        }
        if (write_file(abs, payload, files[i].size) != 0) {
            free(payload);
            set_err(err, errsz, "cannot write extracted installer file");
            goto done;
        }
        free(payload);
        written++;
    }
    if (written == 0) {
        set_err(err, errsz, "Wise installer contained no installable files");
        goto done;
    }
    rc = 0;

done:
    free(files);
    free(data);
    return rc;
}

static void
unlink_if_exists(const char *path)
{
    remove(path);
}

void
vapor_disc_finish_install(const char *dir)
{
    static const char *const drop[] = {
        "AUTORUN.EXE", "AUTORUN.INF", "autorun.exe", "autorun.inf",
        "SIERRA.URL", "sierra.url", NULL
    };
    static const char *const stub_dat[] = {
        "HL.DAT", "hl.dat", "halflife.dat", "HLDS.DAT", "hlds.dat", NULL
    };
    size_t i;

    if (!dir || !*dir) {
        return;
    }
    for (i = 0; drop[i]; i++) {
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/%s", dir, drop[i]) < (int)sizeof(path)) {
            unlink_if_exists(path);
        }
    }
    for (i = 0; stub_dat[i]; i++) {
        char path[1024];
        FILE *f;
        long  n;

        if (snprintf(path, sizeof(path), "%s/%s", dir, stub_dat[i])
            >= (int)sizeof(path)) {
            continue;
        }
        f = fopen(path, "rb");
        if (!f) {
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            continue;
        }
        n = ftell(f);
        fclose(f);
        if (n >= 0 && n < 64) {
            unlink_if_exists(path);
        }
    }
}
