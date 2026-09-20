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
#else
#define MKDIR(p) mkdir((p), 0755)
#define FSEEK64(f, o, w) fseeko((f), (o), (w))
#endif

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

static int
seek_lba(FILE *f, uint32_t lba)
{
    return FSEEK64(f, (long long)lba * (long long)ISO_SECTOR, SEEK_SET) == 0
               ? 0
               : -1;
}

static int
read_sector(FILE *f, uint32_t lba, unsigned char out[ISO_SECTOR])
{
    size_t n;

    if (seek_lba(f, lba) != 0) {
        return -1;
    }
    n = fread(out, 1, ISO_SECTOR, f);
    return n == ISO_SECTOR ? 0 : -1;
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
copy_extent(FILE *in, uint32_t lba, uint32_t size, const char *dest,
            char *err, size_t errsz)
{
    FILE          *out;
    unsigned char  buf[64 * 1024];
    uint32_t       left = size;

    if (mkdirs_parent(dest) != 0) {
        set_err(err, errsz, "cannot create directories for ISO file");
        return -1;
    }
    if (seek_lba(in, lba) != 0) {
        set_err(err, errsz, "ISO file extent is unreadable");
        return -1;
    }
    out = fopen(dest, "wb");
    if (!out) {
        set_err(err, errsz, "cannot write extracted ISO file");
        return -1;
    }
    while (left > 0) {
        size_t chunk = left > sizeof(buf) ? sizeof(buf) : left;
        size_t n = fread(buf, 1, chunk, in);
        if (n == 0 || fwrite(buf, 1, n, out) != n) {
            fclose(out);
            set_err(err, errsz, "failed while copying an ISO file");
            return -1;
        }
        left -= (uint32_t)n;
    }
    if (fclose(out) != 0) {
        set_err(err, errsz, "failed to close extracted ISO file");
        return -1;
    }
    return 0;
}

static int
walk_dir(FILE *in, uint32_t lba, uint32_t size, const char *dest_root,
         const char *rel, int joliet, int depth, char *err, size_t errsz)
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
    if (seek_lba(in, lba) != 0 || fread(dir, 1, size, in) != size) {
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
            if (walk_dir(in, flba, flen, dest_root, child_rel, joliet, depth + 1,
                         err, errsz)
                != 0) {
                goto done;
            }
        } else if (flen > 0) {
            if (copy_extent(in, flba, flen, child_abs, err, errsz) != 0) {
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
vapor_iso_extract(const char *iso_path, const char *dest_dir, char *err,
                  size_t errsz)
{
    FILE          *f;
    unsigned char  sec[ISO_SECTOR];
    unsigned char  pvd[ISO_SECTOR];
    unsigned char  svd[ISO_SECTOR];
    int            have_pvd = 0, have_joliet = 0;
    uint32_t       i, root_lba, root_len;
    unsigned char *root;
    uint16_t       block;

    if (!iso_path || !dest_dir) {
        set_err(err, errsz, "ISO extract arguments are missing");
        return -1;
    }
    f = fopen(iso_path, "rb");
    if (!f) {
        set_err(err, errsz, "cannot open ISO image");
        return -1;
    }

    memset(pvd, 0, sizeof(pvd));
    memset(svd, 0, sizeof(svd));
    for (i = 16; i < 32; i++) {
        if (read_sector(f, i, sec) != 0) {
            fclose(f);
            set_err(err, errsz, "cannot read ISO volume descriptors");
            return -1;
        }
        if (memcmp(sec + 1, "CD001", 5) != 0) {
            fclose(f);
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
        fclose(f);
        set_err(err, errsz, "ISO image has no primary volume descriptor");
        return -1;
    }

    root = have_joliet ? svd : pvd;
    block = u16le(root + 128);
    if (block != ISO_SECTOR) {
        fclose(f);
        set_err(err, errsz, "ISO logical block size is not 2048");
        return -1;
    }
    root_lba = u32le(root + 156 + 2);
    root_len = u32le(root + 156 + 10);
    if (mkdirs(dest_dir) != 0) {
        fclose(f);
        set_err(err, errsz, "cannot create ISO extract directory");
        return -1;
    }
    if (walk_dir(f, root_lba, root_len, dest_dir, "", have_joliet, 0, err, errsz)
        != 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
