#include "vapor/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "miniz.h"
#include "vapor/protocol.h"

int
vapor_id_is_valid(const char *id)
{
    size_t i, n;

    if (!id) {
        return 0;
    }
    n = strlen(id);
    if (n == 0 || n > VAPOR_ID_MAX) {
        return 0;
    }
    /* Must start alphanumeric so an id can never look like a flag or a dotfile. */
    if (!((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9'))) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char ch = id[i];
        int  ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
               || ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            return 0;
        }
    }
    if (strstr(id, "..") != NULL) {
        return 0;
    }
    return 1;
}

int
vapor_id_slug(const char *name, char *out, size_t outsz)
{
    size_t i, o = 0;
    int    pending_dash = 0;

    if (!name || !out || outsz < 2) {
        return -1;
    }

    for (i = 0; name[i] != '\0' && o < VAPOR_ID_MAX && o + 1 < outsz; i++) {
        unsigned char ch = (unsigned char)name[i];
        char          lower = (char)tolower(ch);

        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
            if (pending_dash && o > 0 && o < VAPOR_ID_MAX && o + 1 < outsz) {
                out[o++] = '-';
            }
            pending_dash = 0;
            if (o < VAPOR_ID_MAX && o + 1 < outsz) {
                out[o++] = lower;
            }
        } else if (o > 0) {
            pending_dash = 1;
        }
    }
    out[o] = '\0';

    if (o == 0) {
        if (outsz < 5) {
            return -1;
        }
        memcpy(out, "game", 5);
    }
    return vapor_id_is_valid(out) ? 0 : -1;
}

int
vapor_id_slug_stem(const char *name, char *out, size_t outsz)
{
    char        tmp[256];
    const char *base;
    const char *dot;
    size_t      n;

    if (!name) {
        return -1;
    }
    base = strrchr(name, '/');
    base = base ? base + 1 : name;
#if defined(_WIN32)
    {
        const char *b = strrchr(base, '\\');
        if (b) {
            base = b + 1;
        }
    }
#endif
    dot = strrchr(base, '.');
    if (dot && dot != base) {
        n = (size_t)(dot - base);
        if (n >= sizeof(tmp)) {
            n = sizeof(tmp) - 1;
        }
        memcpy(tmp, base, n);
        tmp[n] = '\0';
        return vapor_id_slug(tmp, out, outsz);
    }
    return vapor_id_slug(base, out, outsz);
}

int
vapor_slug_match(const char *a, const char *b)
{
    char   ca[VAPOR_ID_MAX + 1], cb[VAPOR_ID_MAX + 1];
    size_t i, oa = 0, ob = 0;

    if (!a || !b) {
        return 0;
    }
    if (strcmp(a, b) == 0) {
        return 1;
    }
    for (i = 0; a[i] && oa + 1 < sizeof(ca); i++) {
        if (a[i] != '-') {
            ca[oa++] = a[i];
        }
    }
    ca[oa] = '\0';
    for (i = 0; b[i] && ob + 1 < sizeof(cb); i++) {
        if (b[i] != '-') {
            cb[ob++] = b[i];
        }
    }
    cb[ob] = '\0';
    return ca[0] && strcmp(ca, cb) == 0;
}

int
vapor_username_is_valid(const char *username)
{
    size_t i, n;

    if (!username) {
        return 0;
    }
    n = strlen(username);
    if (n < VAPOR_USERNAME_MIN || n > VAPOR_USERNAME_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char ch = username[i];
        int  ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
               || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

int
vapor_version_is_valid(const char *version)
{
    size_t i, n;

    if (!version) {
        return 0;
    }
    n = strlen(version);
    if (n == 0 || n > VAPOR_VERSION_MAX) {
        return 0;
    }
    /* Must start alphanumeric: keeps out dotfiles and anything flag-like. */
    if (!isalnum((unsigned char)version[0])) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char ch = version[i];
        if (!isalnum((unsigned char)ch)
            && ch != '.' && ch != '_' && ch != '-' && ch != '+') {
            return 0;
        }
    }
    if (strstr(version, "..") != NULL) {
        return 0;
    }
    return 1;
}

int
vapor_version_cmp(const char *a, const char *b)
{
    if (!a) { a = ""; }
    if (!b) { b = ""; }

    while (*a || *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long va, vb;
            char         *ea, *eb;

            va = strtoul(a, &ea, 10);
            vb = strtoul(b, &eb, 10);
            if (va != vb) {
                return va < vb ? -1 : 1;
            }
            a = ea;
            b = eb;
            continue;
        }
        if (*a != *b) {
            /* A shorter version sorts first: "1.2" < "1.2.1". */
            if (!*a) { return -1; }
            if (!*b) { return 1; }
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        }
        a++;
        b++;
    }
    return 0;
}

char *
vapor_strdup(const char *s)
{
    size_t n;
    char  *p;

    if (!s) {
        return NULL;
    }
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

int
vapor_str_eq_ci(const char *a, const char *b)
{
    if (!a || !b) {
        return a == b;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

int
vapor_str_has_prefix(const char *s, const char *prefix)
{
    if (!s || !prefix) {
        return 0;
    }
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int
vapor_str_ends_with_ci(const char *s, const char *suffix)
{
    size_t ls, lx;

    if (!s || !suffix) {
        return 0;
    }
    ls = strlen(s);
    lx = strlen(suffix);
    return ls >= lx && vapor_str_eq_ci(s + ls - lx, suffix);
}

void
vapor_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char hexdig[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[i * 2]     = hexdig[in[i] >> 4];
        out[i * 2 + 1] = hexdig[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

int
vapor_hex_decode(const char *hex, uint8_t *out, size_t outsz)
{
    size_t n, i;

    if (!hex) {
        return -1;
    }
    n = strlen(hex);
    if (n % 2 != 0 || n / 2 > outsz) {
        return -1;
    }
    for (i = 0; i < n; i += 2) {
        int hi = 0, lo = 0, k;
        for (k = 0; k < 2; k++) {
            char ch = hex[i + k];
            int  v;
            if (ch >= '0' && ch <= '9')      { v = ch - '0'; }
            else if (ch >= 'a' && ch <= 'f') { v = ch - 'a' + 10; }
            else if (ch >= 'A' && ch <= 'F') { v = ch - 'A' + 10; }
            else                             { return -1; }
            if (k == 0) { hi = v; } else { lo = v; }
        }
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

int64_t
vapor_now_unix(void)
{
    return (int64_t)time(NULL);
}

void
vapor_format_bytes(uint64_t n, char *out, size_t outsz)
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int    u = 0;

    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(out, outsz, "%llu B", (unsigned long long)n);
    } else {
        snprintf(out, outsz, "%.1f %s", v, unit[u]);
    }
}

void
vapor_format_duration(int64_t seconds, char *out, size_t outsz)
{
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds < 60) {
        snprintf(out, outsz, "%llds", (long long)seconds);
    } else if (seconds < 3600) {
        snprintf(out, outsz, "%lldm %llds",
                 (long long)(seconds / 60), (long long)(seconds % 60));
    } else {
        snprintf(out, outsz, "%lldh %lldm",
                 (long long)(seconds / 3600), (long long)((seconds % 3600) / 60));
    }
}

int
vapor_glob_match(const char *pattern, const char *str)
{
    /* Iterative backtracking, so a deeply starred pattern cannot blow the
     * stack on a long path. */
    const char *p = pattern, *s = str;
    const char *star = NULL, *star_s = NULL;

    if (!pattern || !str) {
        return 0;
    }

    while (*s) {
        if (*p == '?' || (*p && *p == *s)) {
            p++;
            s++;
        } else if (*p == '*') {
            star = ++p;
            star_s = s;
        } else if (star) {
            p = star;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

char *
vapor_read_file(const char *path, size_t *out_len)
{
    FILE  *f;
    char  *buf;
    long   size;
    size_t got;

    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    buf[got] = '\0';
    if (out_len) {
        *out_len = got;
    }
    return buf;
}

static uint16_t
u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t
u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

const char *
vapor_pe_arch_mem(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t       lfanew;
    uint16_t       machine;

    if (!p || len < 0x40 || p[0] != 'M' || p[1] != 'Z') {
        return NULL;
    }
    lfanew = u32le(p + 0x3c);
    if (lfanew < 0x40 || (size_t)lfanew + 6 > len) {
        return NULL;
    }
    if (p[lfanew] != 'P' || p[lfanew + 1] != 'E' || p[lfanew + 2] != 0
        || p[lfanew + 3] != 0) {
        return NULL;
    }
    machine = u16le(p + lfanew + 4);
    if (machine == 0x14c) {
        return "x86";
    }
    if (machine == 0x8664) {
        return "x86_64";
    }
    return NULL;
}

const char *
vapor_pe_arch_file(const char *path)
{
    FILE    *f;
    uint8_t  buf[65536];
    size_t   n;
    const char *arch;

    if (!path || !path[0]) {
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    arch = vapor_pe_arch_mem(buf, n);
    return arch;
}

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   n;
} pe_grab;

static size_t
pe_grab_write(void *opaque, mz_uint64 file_ofs, const void *buf, size_t n)
{
    pe_grab *g = (pe_grab *)opaque;

    if (file_ofs >= g->cap) {
        return 0;
    }
    if (n > g->cap - (size_t)file_ofs) {
        n = g->cap - (size_t)file_ofs;
    }
    memcpy(g->buf + (size_t)file_ofs, buf, n);
    if ((size_t)file_ofs + n > g->n) {
        g->n = (size_t)file_ofs + n;
    }
    return g->n >= g->cap ? 0 : n;
}

const char *
vapor_pe_arch_zip(const char *zip_path, const char *entry)
{
    mz_zip_archive zip;
    char           norm[1024];
    uint8_t        buf[65536];
    pe_grab        grab;
    int            index;
    size_t         i, n;
    const char    *arch;

    if (!zip_path || !entry || !entry[0]) {
        return NULL;
    }
    n = strlen(entry);
    if (n >= sizeof(norm)) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        norm[i] = entry[i] == '\\' ? '/' : entry[i];
    }
    norm[n] = '\0';

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        return NULL;
    }
    index = mz_zip_reader_locate_file(&zip, norm, NULL, 0);
    if (index < 0) {
        mz_zip_reader_end(&zip);
        return NULL;
    }
    memset(buf, 0, sizeof(buf));
    grab.buf = buf;
    grab.cap = sizeof(buf);
    grab.n = 0;
    (void)mz_zip_reader_extract_to_callback(&zip, (mz_uint)index, pe_grab_write,
                                            &grab, 0);
    mz_zip_reader_end(&zip);
    arch = vapor_pe_arch_mem(buf, grab.n);
    return arch;
}

void
vapor_secure_zero(void *p, size_t n)
{
    /* The volatile pointer is what stops the write being optimised away as a
     * dead store to memory nothing reads again. */
    volatile unsigned char *q = (volatile unsigned char *)p;

    if (!p) {
        return;
    }
    while (n--) {
        *q++ = 0;
    }
}
