#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "vapor/iso9660.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ISO_SECTOR 2048u
#define ISO_MAX_DEPTH 16
#define ISO_MAX_DIR (32u * 1024u * 1024u)

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define FSEEK64(f, o, w) _fseeki64((f), (o), (w))
#define FTELL64(f) _ftelli64(f)
#else
#define MKDIR(p) mkdir((p), 0755)
#define FSEEK64(f, o, w) fseeko((f), (o), (w))
#define FTELL64(f) ftello(f)
#endif

typedef struct {
    vapor_iso_progress_fn cb;
    void                 *ud;
    uint64_t              done;
    uint64_t              total;
} iso_progress;

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

static uint64_t
u64le(const unsigned char *p)
{
    return (uint64_t)u32le(p) | ((uint64_t)u32le(p + 4) << 32);
}

/* A raw CD sector is larger than the 2048-byte ISO logical block. `raw` is
 * the stride on disk; `skip` is where the user data starts inside it.
 * `base` is the first byte of that track (0, or an Alcohol .mdf offset). */
typedef struct {
    FILE    *f;
    uint64_t base;
    uint32_t raw;
    uint32_t skip;
} iso_image;

static int
cd001_at(FILE *f, long long off)
{
    unsigned char buf[6];

    if (off < 0 || FSEEK64(f, off, SEEK_SET) != 0) {
        return 0;
    }
    if (fread(buf, 1, 6, f) != 6) {
        return 0;
    }
    return memcmp(buf + 1, "CD001", 5) == 0;
}

static int
try_layout(FILE *f, uint32_t raw, uint32_t skip, uint32_t *out_raw,
           uint32_t *out_skip)
{
    if (!cd001_at(f, 16ll * (long long)raw + (long long)skip)) {
        return -1;
    }
    *out_raw = raw;
    *out_skip = skip;
    return 0;
}

/* Plain ISO first, then BIN/CUE: Mode 1 user data at +16, Mode 2 Form 1 at
 * +24, 2448-byte sectors (2352 + subchannel), and headerless 2336. */
static int
detect_layout(FILE *f, uint32_t *raw, uint32_t *skip)
{
    unsigned char lead[16];
    uint32_t      prefer = 16, alt = 24;
    size_t        i;
    static const uint32_t strides[] = { 2352u, 2448u, 2336u };

    if (try_layout(f, ISO_SECTOR, 0, raw, skip) == 0) {
        return 0;
    }
    if (FSEEK64(f, 0, SEEK_SET) == 0 && fread(lead, 1, 16, f) == 16
        && lead[0] == 0x00 && lead[1] == 0xff && lead[11] == 0x00
        && lead[15] == 2) {
        prefer = 24;
        alt = 16;
    }
    for (i = 0; i < sizeof(strides) / sizeof(strides[0]); i++) {
        if (try_layout(f, strides[i], prefer, raw, skip) == 0
            || try_layout(f, strides[i], alt, raw, skip) == 0) {
            return 0;
        }
    }
    if (try_layout(f, 2336u, 8, raw, skip) == 0) {
        return 0;
    }
    return -1;
}

/* Alcohol 120% .mds (versions 1.3–1.5). The .mdf is the sector dump; the
 * sidecar names each track's byte offset and sector size. */
#define MDS_MAGIC "MEDIA DESCRIPTOR"
#define MDS_HDR 0x58u
#define MDS_SESSION 0x18u
#define MDS_TRACK 0x50u

static int
mds_sector_ok(uint32_t n)
{
    return n == 2048u || n == 2324u || n == 2336u || n == 2352u || n == 2448u;
}

static int
read_exact(FILE *f, long long off, void *dst, size_t n)
{
    if (off < 0 || FSEEK64(f, off, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(dst, 1, n, f) != n) {
        return -1;
    }
    return 0;
}

/* User-data skip is decided by CD001, not the track mode. Alcohol stores
 * Mode 1 as either a 2048-byte payload or a raw 2352-byte sector. */
static int
probe_user_skip(FILE *f, uint64_t base, uint32_t raw, uint32_t *out_skip)
{
    static const uint32_t skips[] = { 0u, 16u, 24u, 8u };
    size_t                i;

    for (i = 0; i < sizeof(skips) / sizeof(skips[0]); i++) {
        long long off;

        if (skips[i] >= raw) {
            continue;
        }
        off = (long long)base + 16ll * (long long)raw + (long long)skips[i];
        if (cd001_at(f, off)) {
            *out_skip = skips[i];
            return 0;
        }
    }
    return -1;
}

/* Sibling of `Game.mdf` is `Game.mds`, matching the extension's letter case,
 * then the other common casings. */
static FILE *
open_sibling_mds(const char *mdf_path)
{
    char        path[1024];
    const char *dot = NULL;
    const char *p;
    size_t      stem;
    char        primary[5];
    static const char *alts[] = { ".mds", ".MDS", ".Mds" };
    size_t      i;
    FILE       *f;

    if (!mdf_path) {
        return NULL;
    }
    for (p = mdf_path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            dot = NULL;
        } else if (*p == '.') {
            dot = p;
        }
    }
    if (!dot || dot[1] == '\0' || dot[2] == '\0' || dot[3] == '\0' || dot[4] != '\0') {
        return NULL;
    }
    if ((dot[1] != 'm' && dot[1] != 'M') || (dot[2] != 'd' && dot[2] != 'D')
        || (dot[3] != 'f' && dot[3] != 'F')) {
        return NULL;
    }
    stem = (size_t)(dot - mdf_path);
    if (stem + 4 >= sizeof(path)) {
        return NULL;
    }
    memcpy(path, mdf_path, stem);
    primary[0] = '.';
    primary[1] = dot[1];
    primary[2] = dot[2];
    primary[3] = (dot[3] == 'F') ? 'S' : 's';
    primary[4] = '\0';
    memcpy(path + stem, primary, 5);
    f = fopen(path, "rb");
    if (f) {
        return f;
    }
    for (i = 0; i < sizeof(alts) / sizeof(alts[0]); i++) {
        if (strcmp(alts[i], primary) == 0) {
            continue;
        }
        memcpy(path + stem, alts[i], 5);
        f = fopen(path, "rb");
        if (f) {
            return f;
        }
    }
    return NULL;
}

/* 0 when a data track contains an ISO 9660 filesystem. -1 means there is
 * no usable sidecar and the caller should probe the file from byte 0. */
static int
layout_from_mds(FILE *mdf, const char *mdf_path, uint64_t *base, uint32_t *raw,
                uint32_t *skip)
{
    FILE         *mds;
    unsigned char hdr[MDS_HDR];
    uint16_t      nsess;
    uint32_t      sess_at;
    unsigned      s;

    mds = open_sibling_mds(mdf_path);
    if (!mds) {
        return -1;
    }
    if (fread(hdr, 1, sizeof(hdr), mds) != sizeof(hdr)
        || memcmp(hdr, MDS_MAGIC, 16) != 0) {
        fclose(mds);
        return -1;
    }
    nsess = u16le(hdr + 0x14);
    sess_at = u32le(hdr + 0x50);
    if (nsess == 0 || nsess > 32) {
        fclose(mds);
        return -1;
    }
    for (s = 0; s < nsess; s++) {
        unsigned char sess[MDS_SESSION];
        uint32_t      data_at;
        unsigned      nblk, b;

        if (read_exact(mds, (long long)sess_at + (long long)s * MDS_SESSION, sess,
                       sizeof(sess))
            != 0) {
            break;
        }
        nblk = sess[0x0A];
        data_at = u32le(sess + 0x14);
        if (nblk > 128) {
            nblk = 128;
        }
        for (b = 0; b < nblk; b++) {
            unsigned char trk[MDS_TRACK];
            unsigned      mode, point;
            uint32_t      sec, user_skip;
            uint64_t      off;

            if (read_exact(mds, (long long)data_at + (long long)b * MDS_TRACK, trk,
                           sizeof(trk))
                != 0) {
                break;
            }
            mode = trk[0];
            point = trk[4];
            /* Point 1..99 is a real track. 0x00 is lead-in; 0xA9 is audio. */
            if (point < 1 || point > 99 || mode == 0x00 || mode == 0xA9) {
                continue;
            }
            sec = u16le(trk + 0x10);
            if (!mds_sector_ok(sec)) {
                continue;
            }
            off = u64le(trk + 0x28);
            if (probe_user_skip(mdf, off, sec, &user_skip) == 0) {
                *base = off;
                *raw = sec;
                *skip = user_skip;
                fclose(mds);
                return 0;
            }
        }
    }
    fclose(mds);
    return -1;
}

static int
detect_image_layout(FILE *f, const char *path, uint64_t *base, uint32_t *raw,
                    uint32_t *skip)
{
    *base = 0;
    if (layout_from_mds(f, path, base, raw, skip) == 0) {
        return 0;
    }
    *base = 0;
    return detect_layout(f, raw, skip);
}

/* Read `n` logical bytes starting `logical_off` into the extent at `lba`.
 * Raw images are not contiguous, so this stops at each sector boundary. */
static int
read_user(iso_image *img, uint32_t lba, uint32_t logical_off, void *dst,
          uint32_t n)
{
    unsigned char *out = (unsigned char *)dst;

    while (n > 0) {
        uint32_t  block = logical_off / ISO_SECTOR;
        uint32_t  into = logical_off % ISO_SECTOR;
        uint32_t  chunk = ISO_SECTOR - into;
        long long off;

        if (chunk > n) {
            chunk = n;
        }
        off = (long long)img->base
              + (long long)(lba + block) * (long long)img->raw
              + (long long)img->skip + (long long)into;
        if (FSEEK64(img->f, off, SEEK_SET) != 0
            || fread(out, 1, chunk, img->f) != chunk) {
            return -1;
        }
        out += chunk;
        logical_off += chunk;
        n -= chunk;
    }
    return 0;
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
join_path(char *out, size_t outsz, const char *dir, const char *rel)
{
    int n;

    if (!rel || !*rel) {
        n = snprintf(out, outsz, "%s", dir);
    } else {
        n = snprintf(out, outsz, "%s/%s", dir, rel);
    }
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

static int
decode_name(const unsigned char *name, unsigned char nlen, int joliet,
            char *out, size_t outsz)
{
    size_t o = 0, i;

    if (nlen == 1 && (name[0] == 0 || name[0] == 1)) {
        return 1; /* . or .. */
    }
    if (joliet) {
        if (nlen < 2 || (nlen & 1)) {
            /* name_len is bytes; Joliet is UCS-2. An odd length includes a pad. */
            if (nlen < 2) {
                return -1;
            }
            nlen = (unsigned char)(nlen - (nlen & 1));
        }
        for (i = 0; i + 1 < nlen && o + 1 < outsz; i += 2) {
            unsigned int cp = ((unsigned)name[i] << 8) | name[i + 1];
            if (cp == 0) {
                break;
            }
            if (cp == ';') {
                break;
            }
            if (cp < 32 || cp == '/' || cp == '\\' || cp == ':' || cp > 0x7f) {
                if (cp > 0x7f && o + 3 < outsz) {
                    /* Basic UTF-8 for BMP. */
                    if (cp < 0x800) {
                        out[o++] = (char)(0xc0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3f));
                        continue;
                    }
                    out[o++] = (char)(0xe0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                    out[o++] = (char)(0x80 | (cp & 0x3f));
                    continue;
                }
                if (cp < 32 || cp == '/' || cp == '\\' || cp == ':') {
                    return -1;
                }
            } else {
                out[o++] = (char)cp;
            }
        }
    } else {
        for (i = 0; i < nlen && o + 1 < outsz; i++) {
            char ch = (char)name[i];
            if (ch == ';' || ch == 0) {
                break;
            }
            if (ch == '/' || ch == '\\' || ch == ':' || (unsigned char)ch < 32) {
                return -1;
            }
            if (ch == '.') {
                /* ISO 9660 often pads as "NAME.;1" */
                if (i + 1 < nlen && name[i + 1] == ';') {
                    break;
                }
            }
            out[o++] = ch;
        }
    }
    if (o == 0) {
        return -1;
    }
    out[o] = '\0';
    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        return 1;
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
copy_extent(iso_image *img, uint32_t lba, uint32_t size, const char *dest,
            char *err, size_t errsz, iso_progress *prog)
{
    FILE          *out;
    unsigned char  buf[64 * 1024];
    uint32_t       off = 0;

    if (mkdirs_parent(dest) != 0) {
        set_err(err, errsz, "cannot create directories for ISO file");
        return -1;
    }
    out = fopen(dest, "wb");
    if (!out) {
        set_err(err, errsz, "cannot write extracted ISO file");
        return -1;
    }
    while (off < size) {
        uint32_t chunk = size - off;
        if (chunk > sizeof(buf)) {
            chunk = (uint32_t)sizeof(buf);
        }
        if (read_user(img, lba, off, buf, chunk) != 0
            || fwrite(buf, 1, chunk, out) != chunk) {
            fclose(out);
            set_err(err, errsz, "failed while copying an ISO file");
            return -1;
        }
        off += chunk;
        if (prog && prog->cb) {
            uint64_t shown;

            prog->done += (uint64_t)chunk;
            shown = prog->done;
            if (prog->total > 0 && shown > prog->total) {
                shown = prog->total;
            }
            if (prog->cb(prog->ud, shown, prog->total) != 0) {
                fclose(out);
                set_err(err, errsz, "ISO extract cancelled");
                return -1;
            }
        }
    }
    if (fclose(out) != 0) {
        set_err(err, errsz, "failed to close extracted ISO file");
        return -1;
    }
    return 0;
}

static int
walk_dir(iso_image *img, uint32_t lba, uint32_t size, const char *dest_root,
         const char *rel, int joliet, int depth, char *err, size_t errsz,
         iso_progress *prog)
{
    unsigned char *dir;
    uint32_t       off = 0;
    int            rc = -1;

    if (depth > ISO_MAX_DEPTH) {
        set_err(err, errsz, "ISO directory tree is too deep");
        return -1;
    }
    if (size == 0 || size > ISO_MAX_DIR) {
        set_err(err, errsz, "ISO directory is empty or too large");
        return -1;
    }
    dir = (unsigned char *)malloc(size);
    if (!dir) {
        set_err(err, errsz, "out of memory");
        return -1;
    }
    if (read_user(img, lba, 0, dir, size) != 0) {
        set_err(err, errsz, "cannot read ISO directory");
        goto done;
    }

    while (off < size) {
        unsigned char rec, nlen, flags;
        uint32_t      flba, flen;
        char          name[256];
        char          child_rel[1024];
        char          child_abs[1024];
        int           nrc;

        rec = dir[off];
        if (rec == 0) {
            off = ((off / ISO_SECTOR) + 1) * ISO_SECTOR;
            continue;
        }
        if (rec < 34 || off + rec > size) {
            break;
        }
        flags = dir[off + 25];
        nlen = dir[off + 32];
        if ((unsigned)33 + nlen > rec) {
            off += rec;
            continue;
        }
        if (flags & 0x04) {
            off += rec;
            continue; /* associated file */
        }
        nrc = decode_name(dir + off + 33, nlen, joliet, name, sizeof(name));
        if (nrc < 0) {
            off += rec;
            continue;
        }
        if (nrc == 1) {
            off += rec;
            continue;
        }
        flba = u32le(dir + off + 2);
        flen = u32le(dir + off + 10);
        if (rel[0]) {
            if (snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, name)
                >= (int)sizeof(child_rel)) {
                set_err(err, errsz, "ISO path is too long");
                goto done;
            }
        } else if (snprintf(child_rel, sizeof(child_rel), "%s", name)
                   >= (int)sizeof(child_rel)) {
            set_err(err, errsz, "ISO path is too long");
            goto done;
        }
        if (join_path(child_abs, sizeof(child_abs), dest_root, child_rel) != 0) {
            set_err(err, errsz, "ISO path is too long");
            goto done;
        }
        if (flags & 0x02) {
            if (mkdirs(child_abs) != 0) {
                set_err(err, errsz, "cannot create directory from ISO");
                goto done;
            }
            if (walk_dir(img, flba, flen, dest_root, child_rel, joliet, depth + 1,
                         err, errsz, prog)
                != 0) {
                goto done;
            }
        } else if (flen > 0) {
            if (copy_extent(img, flba, flen, child_abs, err, errsz, prog) != 0) {
                goto done;
            }
        }
        off += rec;
    }
    rc = 0;

done:
    free(dir);
    return rc;
}

int
vapor_iso_is_image(const char *path)
{
    FILE    *f;
    uint64_t base;
    uint32_t raw, skip;
    int      ok;

    if (!path) {
        return 0;
    }
    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    ok = detect_image_layout(f, path, &base, &raw, &skip) == 0;
    fclose(f);
    return ok;
}

int
vapor_iso_extract_progress(const char *iso_path, const char *dest_dir,
                           char *err, size_t errsz, vapor_iso_progress_fn cb,
                           void *ud)
{
    iso_image      img;
    unsigned char  sec[ISO_SECTOR];
    unsigned char  pvd[ISO_SECTOR];
    unsigned char  svd[ISO_SECTOR];
    int            have_pvd = 0, have_joliet = 0;
    uint32_t       i, root_lba, root_len;
    unsigned char *root;
    uint16_t       block;
    iso_progress   prog;
    long long      nbytes;

    if (!iso_path || !dest_dir) {
        set_err(err, errsz, "ISO extract arguments are missing");
        return -1;
    }
    memset(&img, 0, sizeof(img));
    img.f = fopen(iso_path, "rb");
    if (!img.f) {
        set_err(err, errsz, "cannot open ISO image");
        return -1;
    }
    if (detect_image_layout(img.f, iso_path, &img.base, &img.raw, &img.skip)
        != 0) {
        fclose(img.f);
        set_err(err, errsz, "not an ISO 9660 disc image");
        return -1;
    }

    memset(&prog, 0, sizeof(prog));
    prog.cb = cb;
    prog.ud = ud;
    if (FSEEK64(img.f, 0, SEEK_END) == 0) {
        nbytes = FTELL64(img.f);
        if (nbytes > 0) {
            prog.total = (uint64_t)nbytes;
        }
    }
    if (prog.total == 0) {
        prog.total = 1;
    }

    memset(pvd, 0, sizeof(pvd));
    memset(svd, 0, sizeof(svd));
    for (i = 16; i < 32; i++) {
        if (read_user(&img, i, 0, sec, ISO_SECTOR) != 0) {
            fclose(img.f);
            set_err(err, errsz, "cannot read ISO volume descriptors");
            return -1;
        }
        if (memcmp(sec + 1, "CD001", 5) != 0) {
            fclose(img.f);
            set_err(err, errsz, "not an ISO 9660 disc image");
            return -1;
        }
        if (sec[0] == 1) {
            memcpy(pvd, sec, ISO_SECTOR);
            have_pvd = 1;
        } else if (sec[0] == 2 && sec[88] == '%' && sec[89] == '/'
                   && (sec[90] == '@' || sec[90] == 'C' || sec[90] == 'E')) {
            memcpy(svd, sec, ISO_SECTOR);
            have_joliet = 1;
        } else if (sec[0] == 255) {
            break;
        }
    }
    if (!have_pvd) {
        fclose(img.f);
        set_err(err, errsz, "ISO image has no primary volume descriptor");
        return -1;
    }

    root = have_joliet ? svd : pvd;
    block = u16le(root + 128);
    if (block != ISO_SECTOR) {
        fclose(img.f);
        set_err(err, errsz, "ISO logical block size is not 2048");
        return -1;
    }
    root_lba = u32le(root + 156 + 2);
    root_len = u32le(root + 156 + 10);
    if (mkdirs(dest_dir) != 0) {
        fclose(img.f);
        set_err(err, errsz, "cannot create ISO extract directory");
        return -1;
    }
    if (walk_dir(&img, root_lba, root_len, dest_dir, "", have_joliet, 0, err,
                 errsz, cb ? &prog : NULL)
        != 0) {
        fclose(img.f);
        return -1;
    }
    if (cb) {
        (void)cb(ud, prog.total, prog.total);
    }
    fclose(img.f);
    return 0;
}

int
vapor_iso_extract(const char *iso_path, const char *dest_dir, char *err,
                  size_t errsz)
{
    return vapor_iso_extract_progress(iso_path, dest_dir, err, errsz, NULL,
                                      NULL);
}
