/* Scan library_root and publish each subdirectory as a catalog game.
 *
 * Layout (one folder per title):
 *
 *   library_root/
 *     Hollow Knight/
 *       HollowKnight.zip     (preferred)
 *       cover.png            (optional)
 *       vapor.json           (optional overrides)
 *     Disc Game/
 *       Disc 1.iso          (all images unpacked into one tree, then zipped;
 *       Disc 2.bin           .iso, .img, and BIN/CUE .bin; originals stay)
 *     Portable Game/
 *       Game.exe
 *       data/
 *
 * Zip files already sitting in library_root are served in place. ISO and
 * BIN/CUE images (.iso, .img, .bin) are unpacked (ISO 9660 / Joliet); every
 * image in the folder is merged.
 * A Wise SETUP.EXE on the disc is unpacked too, and the files are zipped into
 * <content_root>/<id>/<version>/package.zip; the original disc images are left
 * in the drop folder. A folder of loose files is zipped the same way.
 * Unchanged folders are fingerprint-skipped so large archives are not
 * re-hashed.
 */

#include "vapord.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miniz.h"
#include "vapor/iso9660.h"
#include "vapor/saves.h"
#include "vapor/sha256.h"
#include "vapor/util.h"
#include "vapor/wise.h"

#define PACKAGED_ZIP "package.zip"

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} strlist;

static int
strlist_push(strlist *l, const char *s)
{
    if (l->count == l->cap) {
        size_t newcap = l->cap ? l->cap * 2 : 8;
        char **grown = (char **)realloc(l->items, newcap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        l->items = grown;
        l->cap = newcap;
    }
    l->items[l->count] = vapor_strdup(s);
    if (!l->items[l->count]) {
        return -1;
    }
    l->count++;
    return 0;
}

static int
strlist_contains(const strlist *l, const char *s)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        if (strcmp(l->items[i], s) == 0) {
            return 1;
        }
    }
    return 0;
}

static void
strlist_free(strlist *l)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        free(l->items[i]);
    }
    free(l->items);
    memset(l, 0, sizeof(*l));
}

static int
cmp_strptr(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;

    return strcmp(*sa, *sb);
}

static void
strlist_sort(strlist *l)
{
    if (l && l->count > 1) {
        qsort(l->items, l->count, sizeof(*l->items), cmp_strptr);
    }
}

static const char *
basename_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int
join2(char *out, size_t outsz, const char *a, const char *b)
{
    int n = snprintf(out, outsz, "%s/%s", a, b);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

static int
file_size(const char *path, uint64_t *out)
{
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    *out = (uint64_t)st.st_size;
    return 0;
}

static int
copy_file(const char *src, const char *dst)
{
    FILE  *in, *out;
    char   buf[64 * 1024];
    size_t n;
    int    rc = 0;

    in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) {
        rc = -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        rc = -1;
    }
    return rc;
}

static int
rm_tree(const char *path)
{
    DIR           *d;
    struct dirent *ent;
    struct stat    st;

    if (lstat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? 0 : -1;
    }
    d = opendir(path);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char child[VAPORD_PATH_MAX];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (join2(child, sizeof(child), path, ent->d_name) != 0
            || rm_tree(child) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -1;
}

static int
is_all_upper(const char *s)
{
    int letters = 0;

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalpha(c)) {
            letters = 1;
            if (islower(c)) {
                return 0;
            }
        }
    }
    return letters;
}

static int
merge_tree_into(const char *src, const char *dst)
{
    DIR           *d;
    struct dirent *ent;

    if (mkdir(dst, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    d = opendir(src);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char        from[VAPORD_PATH_MAX], to[VAPORD_PATH_MAX];
        struct stat st_from, st_to;
        int         have_to;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (join2(from, sizeof(from), src, ent->d_name) != 0
            || join2(to, sizeof(to), dst, ent->d_name) != 0) {
            closedir(d);
            return -1;
        }
        if (lstat(from, &st_from) != 0) {
            continue;
        }
        have_to = lstat(to, &st_to) == 0;
        if (S_ISDIR(st_from.st_mode)) {
            if (have_to && S_ISDIR(st_to.st_mode)) {
                if (merge_tree_into(from, to) != 0 || rmdir(from) != 0) {
                    closedir(d);
                    return -1;
                }
            } else if (!have_to) {
                if (rename(from, to) != 0) {
                    closedir(d);
                    return -1;
                }
            } else if (rm_tree(from) != 0) {
                closedir(d);
                return -1;
            }
        } else if (!have_to) {
            if (rename(from, to) != 0) {
                closedir(d);
                return -1;
            }
        } else if (unlink(from) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

/* ISO 9660 names are often ALLCAPS; Wise scripts use mixed case. On a
 * case-insensitive client those trees collide, so fold them here and let
 * the installer copy win. */
static void
fold_case_collisions(const char *dir)
{
    int changed;

    do {
        strlist        names;
        size_t         i, j;
        DIR           *d;
        struct dirent *ent;

        changed = 0;
        memset(&names, 0, sizeof(names));
        d = opendir(dir);
        if (!d) {
            return;
        }
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0
                || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            if (strlist_push(&names, ent->d_name) != 0) {
                closedir(d);
                strlist_free(&names);
                return;
            }
        }
        closedir(d);

        for (i = 0; i < names.count && !changed; i++) {
            for (j = i + 1; j < names.count; j++) {
                char        path_i[VAPORD_PATH_MAX], path_j[VAPORD_PATH_MAX];
                struct stat st_i, st_j;
                int         i_dir, j_dir;

                if (!vapor_str_eq_ci(names.items[i], names.items[j])) {
                    continue;
                }
                if (join2(path_i, sizeof(path_i), dir, names.items[i]) != 0
                    || join2(path_j, sizeof(path_j), dir, names.items[j]) != 0
                    || lstat(path_i, &st_i) != 0 || lstat(path_j, &st_j) != 0) {
                    continue;
                }
                i_dir = S_ISDIR(st_i.st_mode);
                j_dir = S_ISDIR(st_j.st_mode);
                if (i_dir && j_dir) {
                    if (is_all_upper(names.items[i])
                        && !is_all_upper(names.items[j])) {
                        if (merge_tree_into(path_i, path_j) == 0) {
                            rmdir(path_i);
                        }
                    } else if (merge_tree_into(path_j, path_i) == 0) {
                        rmdir(path_j);
                    }
                    changed = 1;
                    break;
                }
                if (!i_dir && !j_dir) {
                    if (st_i.st_mtime >= st_j.st_mtime) {
                        unlink(path_j);
                    } else {
                        unlink(path_i);
                    }
                    changed = 1;
                    break;
                }
            }
        }
        strlist_free(&names);
    } while (changed);

    {
        DIR           *d;
        struct dirent *ent;

        d = opendir(dir);
        if (!d) {
            return;
        }
        while ((ent = readdir(d)) != NULL) {
            char        child[VAPORD_PATH_MAX];
            struct stat st;

            if (strcmp(ent->d_name, ".") == 0
                || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            if (join2(child, sizeof(child), dir, ent->d_name) != 0) {
                continue;
            }
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                fold_case_collisions(child);
            }
        }
        closedir(d);
    }
}

static int
skip_walk_dir(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0
        || name[0] == '.' || strcmp(name, "__MACOSX") == 0
        || vapor_str_eq_ci(name, "Redist")
        || vapor_str_eq_ci(name, "_CommonRedist")
        || vapor_str_eq_ci(name, "DirectX")
        || vapor_str_eq_ci(name, "node_modules");
}

static int
is_disc_installer_name(const char *base)
{
    return vapor_str_eq_ci(base, "setup.exe")
        || vapor_str_eq_ci(base, "install.exe")
        || vapor_str_eq_ci(base, "installer.exe");
}

static int
unpack_wise_installers(const char *dir)
{
    DIR           *d;
    struct dirent *ent;
    int            n = 0;

    d = opendir(dir);
    if (!d) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        char path[VAPORD_PATH_MAX];
        char err[256];

        if (ent->d_name[0] == '.' || !is_disc_installer_name(ent->d_name)) {
            continue;
        }
        if (join2(path, sizeof(path), dir, ent->d_name) != 0) {
            continue;
        }
        memset(err, 0, sizeof(err));
        VLOG_INFO("discover: unpacking installer \"%s\"", ent->d_name);
        if (vapor_wise_extract(path, dir, err, sizeof(err)) == 0) {
            unlink(path);
            n++;
        } else if (err[0]) {
            VLOG_WARN("discover: installer unpack of %s failed (%s)",
                      ent->d_name, err);
        }
    }
    closedir(d);
    return n;
}

static int
contains_ci(const char *s, const char *needle)
{
    size_t n, i, j;

    if (!s || !needle) {
        return 0;
    }
    n = strlen(needle);
    if (n == 0) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        for (j = 0; j < n; j++) {
            unsigned char a = (unsigned char)s[i + j];
            unsigned char b = (unsigned char)needle[j];

            if (a == 0) {
                return 0;
            }
            if (a >= 'A' && a <= 'Z') {
                a = (unsigned char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (unsigned char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (j == n) {
            return 1;
        }
    }
    return 0;
}

/* Official updater dropped beside the disc, e.g. ukusonly_patch111v9safedisk.exe. */
static int
is_publisher_patch(const char *name)
{
    const char *base = name ? basename_of(name) : "";

    return vapor_str_ends_with_ci(base, ".exe") && contains_ci(base, "patch");
}

static int
is_junk_exec(const char *base)
{
    static const char *const junk[] = {
        "setup.exe", "install.exe", "installer.exe", "setup.com", "install.com",
        "unins000.exe",
        "uninstall.exe", "dxsetup.exe", "autorun.exe", "launch.exe",
        "setup_", "instmsi", "vcredist", "vc_redist",
        "unitycrashhandler", "crashreporter", "easyanticheat",
        "dotnetfx", NULL
    };
    size_t i;

    for (i = 0; junk[i]; i++) {
        if (vapor_str_eq_ci(base, junk[i])
            || vapor_str_has_prefix(base, junk[i])) {
            return 1;
        }
    }
    if (vapor_str_has_prefix(base, "unins") && vapor_str_ends_with_ci(base, ".exe")) {
        return 1;
    }
    if (vapor_str_eq_ci(base, "upd.exe")
        || vapor_str_ends_with_ci(base, "up.exe")
        || vapor_str_ends_with_ci(base, "update.exe")) {
        return 1;
    }
    /* A publisher patch dropped next to a disc (ukusonly_patch111....exe)
     * is installed with the game. It is not the launch target. */
    if (is_publisher_patch(base)) {
        return 1;
    }
    return 0;
}

static int
path_depth(const char *rel)
{
    int n = 0;
    for (; *rel; rel++) {
        if (*rel == '/') {
            n++;
        }
    }
    return n;
}

static int
score_exec(const char *rel, const char *want_slug, int junk_ok)
{
    const char *base = basename_of(rel);
    int         score;
    char        slug[VAPOR_ID_MAX + 1];

    if (!junk_ok && is_junk_exec(base)) {
        return -1;
    }
    score = 200 - path_depth(rel) * 15;
    if (want_slug && *want_slug
        && vapor_id_slug_stem(base, slug, sizeof(slug)) == 0) {
        if (vapor_slug_match(slug, want_slug)) {
            score += 80;
        } else {
            char init[16];
            size_t o = 0, i;
            int    word = 1;
            for (i = 0; want_slug[i] && o + 1 < sizeof(init); i++) {
                if (want_slug[i] == '-') {
                    word = 1;
                } else if (word) {
                    init[o++] = want_slug[i];
                    word = 0;
                }
            }
            init[o] = '\0';
            if (o >= 2 && vapor_str_has_prefix(slug, init)
                && (slug[o] == '\0' || slug[o] == '-')) {
                score += 50;
            }
        }
    }
    return score;
}

static int
looks_windows_exec(const char *name)
{
    const char *base = name ? basename_of(name) : "";

    if (vapor_dos_name_is_junk(base)) {
        return 0;
    }
    return vapor_str_ends_with_ci(base, ".exe")
           || vapor_str_ends_with_ci(base, ".com");
}

static int
looks_linux_exec_name(const char *name)
{
    const char *dot;

    if (vapor_str_ends_with_ci(name, ".sh")
        || vapor_str_ends_with_ci(name, ".x86_64")
        || vapor_str_ends_with_ci(name, ".x86")
        || vapor_str_ends_with_ci(name, ".bin")) {
        return 1;
    }
    if (vapor_str_ends_with_ci(name, ".dll")
        || vapor_str_ends_with_ci(name, ".so")
        || vapor_str_ends_with_ci(name, ".dylib")
        || vapor_str_ends_with_ci(name, ".exe")
        || vapor_str_ends_with_ci(name, ".bat")
        || vapor_str_ends_with_ci(name, ".cmd")
        || vapor_str_ends_with_ci(name, ".png")
        || vapor_str_ends_with_ci(name, ".jpg")
        || vapor_str_ends_with_ci(name, ".json")
        || vapor_str_ends_with_ci(name, ".txt")
        || vapor_str_ends_with_ci(name, ".xml")
        || vapor_str_ends_with_ci(name, ".dll")) {
        return 0;
    }
    dot = strrchr(name, '.');
    return dot == NULL;
}

static int
is_elf_file(const char *path)
{
    FILE          *f;
    unsigned char  mag[4];

    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    if (fread(mag, 1, 4, f) != 4) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return mag[0] == 0x7f && mag[1] == 'E' && mag[2] == 'L' && mag[3] == 'F';
}

static int
is_zip_name(const char *name)
{
    return vapor_str_ends_with_ci(name, ".zip");
}

static int
is_iso_name(const char *name)
{
    return vapor_str_ends_with_ci(name, ".iso")
        || vapor_str_ends_with_ci(name, ".img")
        || vapor_str_ends_with_ci(name, ".bin");
}

/* .bin is also a Linux binary suffix. Only a file with an ISO 9660
 * filesystem (plain or raw BIN/CUE sectors) is a disc image. */
static int
is_disc_image_file(const char *abs, const char *name)
{
    if (vapor_str_ends_with_ci(name, ".iso")
        || vapor_str_ends_with_ci(name, ".img")) {
        return 1;
    }
    return vapor_str_ends_with_ci(name, ".bin") && abs
           && vapor_iso_is_image(abs);
}

static void
drop_non_image_bins(const char *root, strlist *isos)
{
    size_t i = 0;

    while (i < isos->count) {
        char abs[VAPORD_PATH_MAX];

        if (!vapor_str_ends_with_ci(isos->items[i], ".bin")) {
            i++;
            continue;
        }
        if (join2(abs, sizeof(abs), root, isos->items[i]) == 0
            && vapor_iso_is_image(abs)) {
            i++;
            continue;
        }
        free(isos->items[i]);
        if (i + 1 < isos->count) {
            memmove(&isos->items[i], &isos->items[i + 1],
                    (isos->count - i - 1) * sizeof(isos->items[0]));
        }
        isos->count--;
    }
}

/* ---------------------------------------------------------------- fingerprint */

static void
xor_hex_hash(uint8_t acc[VAPOR_SHA256_DIGEST_LEN], const char *s)
{
    uint8_t d[VAPOR_SHA256_DIGEST_LEN];
    size_t  i;

    vapor_sha256_buf(s, strlen(s), d);
    for (i = 0; i < VAPOR_SHA256_DIGEST_LEN; i++) {
        acc[i] ^= d[i];
    }
}

static int
fingerprint_tree(const char *root, const char *rel, uint8_t acc[VAPOR_SHA256_DIGEST_LEN])
{
    char           abs[VAPORD_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (rel[0]) {
        if (join2(abs, sizeof(abs), root, rel) != 0) {
            return -1;
        }
    } else {
        snprintf(abs, sizeof(abs), "%s", root);
    }

    d = opendir(abs);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPORD_PATH_MAX];
        char        child_abs[VAPORD_PATH_MAX];
        struct stat st;
        char        line[VAPORD_PATH_MAX + 64];

        if (skip_walk_dir(ent->d_name) && strcmp(ent->d_name, ".") != 0
            && strcmp(ent->d_name, "..") != 0) {
            continue;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (rel[0]) {
            if (join2(child_rel, sizeof(child_rel), rel, ent->d_name) != 0) {
                rc = -1;
                break;
            }
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (join2(child_abs, sizeof(child_abs), root, child_rel) != 0) {
            rc = -1;
            break;
        }
        if (lstat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (fingerprint_tree(root, child_rel, acc) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        snprintf(line, sizeof(line), "%s|%llu|%lld", child_rel,
                 (unsigned long long)st.st_size, (long long)st.st_mtime);
        xor_hex_hash(acc, line);
    }
    closedir(d);
    return rc;
}

static void
fingerprint_stat(const char *tag, const char *name, uint64_t size, int64_t mtime,
                 char out[VAPOR_SHA256_HEX_LEN + 1])
{
    char line[VAPORD_PATH_MAX + 80];

    snprintf(line, sizeof(line), "%s|%s|%llu|%lld", tag, name,
             (unsigned long long)size, (long long)mtime);
    vapor_sha256_hex_buf(line, strlen(line), out);
}

static void
fingerprint_patches(const char *root, const char *rel, vapor_sha256 *ctx,
                    int64_t *max_mtime)
{
    char           abs[VAPORD_PATH_MAX];
    DIR           *d;
    struct dirent *ent;

    if (rel[0]) {
        if (join2(abs, sizeof(abs), root, rel) != 0) {
            return;
        }
    } else {
        snprintf(abs, sizeof(abs), "%s", root);
    }
    d = opendir(abs);
    if (!d) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPORD_PATH_MAX];
        char        child_abs[VAPORD_PATH_MAX];
        struct stat st;
        char        line[VAPORD_PATH_MAX + 80];
        int         n;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0
            || ent->d_name[0] == '.') {
            continue;
        }
        if (rel[0]) {
            if (join2(child_rel, sizeof(child_rel), rel, ent->d_name) != 0) {
                continue;
            }
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (join2(child_abs, sizeof(child_abs), root, child_rel) != 0
            || lstat(child_abs, &st) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            fingerprint_patches(root, child_rel, ctx, max_mtime);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_publisher_patch(ent->d_name)) {
            continue;
        }
        if (max_mtime && st.st_mtime > *max_mtime) {
            *max_mtime = (int64_t)st.st_mtime;
        }
        n = snprintf(line, sizeof(line), "\npatch|%s|%llu|%lld", child_rel,
                     (unsigned long long)st.st_size, (long long)st.st_mtime);
        if (n > 0) {
            vapor_sha256_update(ctx, line, (size_t)n);
        }
    }
    closedir(d);
}

static int
fingerprint_iso_list(const char *root, const strlist *isos,
                     char out[VAPOR_SHA256_HEX_LEN + 1], int64_t *patch_mtime)
{
    vapor_sha256 ctx;
    uint8_t      digest[VAPOR_SHA256_DIGEST_LEN];
    size_t       i;

    if (patch_mtime) {
        *patch_mtime = 0;
    }
    vapor_sha256_init(&ctx);
    vapor_sha256_update(&ctx, "iso-setup1", 10);
    for (i = 0; i < isos->count; i++) {
        char        abs[VAPORD_PATH_MAX];
        char        line[VAPORD_PATH_MAX + 80];
        struct stat st;
        int         n;

        if (join2(abs, sizeof(abs), root, isos->items[i]) != 0
            || stat(abs, &st) != 0) {
            continue;
        }
        n = snprintf(line, sizeof(line), "\n%s|%llu|%lld", isos->items[i],
                     (unsigned long long)st.st_size, (long long)st.st_mtime);
        if (n > 0) {
            vapor_sha256_update(&ctx, line, (size_t)n);
        }
    }
    fingerprint_patches(root, "", &ctx, patch_mtime);
    vapor_sha256_final(&ctx, digest);
    vapor_sha256_hex(digest, out);
    return 0;
}

/* ---------------------------------------------------------------- zip packaging */

typedef struct {
    mz_zip_archive zip;
    size_t         nfiles;
} packer;

static int
zip_add_dir(packer *p, const char *src_root, const char *rel)
{
    char           abs[VAPORD_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (rel[0]) {
        if (join2(abs, sizeof(abs), src_root, rel) != 0) {
            return -1;
        }
    } else {
        snprintf(abs, sizeof(abs), "%s", src_root);
    }
    d = opendir(abs);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPORD_PATH_MAX];
        char        child_abs[VAPORD_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (rel[0]) {
            if (join2(child_rel, sizeof(child_rel), rel, ent->d_name) != 0) {
                rc = -1;
                break;
            }
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (join2(child_abs, sizeof(child_abs), src_root, child_rel) != 0) {
            rc = -1;
            break;
        }
        if (lstat(child_abs, &st) != 0) {
            rc = -1;
            break;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            char dir_entry[VAPORD_PATH_MAX + 2];
            snprintf(dir_entry, sizeof(dir_entry), "%s/", child_rel);
            if (!mz_zip_writer_add_mem(&p->zip, dir_entry, NULL, 0, 0)) {
                rc = -1;
                break;
            }
            if (zip_add_dir(p, src_root, child_rel) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (!mz_zip_writer_add_file(&p->zip, child_rel, child_abs, NULL, 0,
                                    MZ_DEFAULT_LEVEL)) {
            rc = -1;
            break;
        }
        p->nfiles++;
    }
    closedir(d);
    return rc;
}

/* Copy publisher patch exes from the drop folder into the unpacked tree so
 * they land in the install directory. Disc images stay where they are. */
static int
copy_publisher_patches(const char *src_root, const char *rel,
                       const char *dest_root)
{
    char           abs[VAPORD_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (rel[0]) {
        if (join2(abs, sizeof(abs), src_root, rel) != 0) {
            return -1;
        }
    } else {
        snprintf(abs, sizeof(abs), "%s", src_root);
    }
    d = opendir(abs);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPORD_PATH_MAX];
        char        child_abs[VAPORD_PATH_MAX];
        char        dest[VAPORD_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0
            || ent->d_name[0] == '.') {
            continue;
        }
        if (rel[0]) {
            if (join2(child_rel, sizeof(child_rel), rel, ent->d_name) != 0) {
                rc = -1;
                break;
            }
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (join2(child_abs, sizeof(child_abs), src_root, child_rel) != 0) {
            rc = -1;
            break;
        }
        if (lstat(child_abs, &st) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (copy_publisher_patches(src_root, child_rel, dest_root) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_publisher_patch(ent->d_name)) {
            continue;
        }
        if (join2(dest, sizeof(dest), dest_root, ent->d_name) != 0) {
            rc = -1;
            break;
        }
        VLOG_INFO("discover: including publisher patch \"%s\"", ent->d_name);
        if (copy_file(child_abs, dest) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    return rc;
}

static int
zip_directory(const char *src_dir, const char *out_zip)
{
    packer p;

    memset(&p, 0, sizeof(p));
    if (!mz_zip_writer_init_file(&p.zip, out_zip, 0)) {
        return -1;
    }
    if (zip_add_dir(&p, src_dir, "") != 0 || p.nfiles == 0
        || !mz_zip_writer_finalize_archive(&p.zip)) {
        mz_zip_writer_end(&p.zip);
        remove(out_zip);
        return -1;
    }
    mz_zip_writer_end(&p.zip);
    return 0;
}

/* Wrap a single file (typically a disc image) as a zip entry. ISOs are already
 * compressed, so this stores without deflate, and zip64 covers >4 GiB images. */
static int
zip_file_store(const char *src_file, const char *arcname, const char *out_zip)
{
    mz_zip_archive zip;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file_v2(&zip, out_zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64)) {
        return -1;
    }
    if (!mz_zip_writer_add_file(&zip, arcname, src_file, NULL, 0,
                                MZ_NO_COMPRESSION)
        || !mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        remove(out_zip);
        return -1;
    }
    mz_zip_writer_end(&zip);
    return 0;
}

static int
zip_files_store(const char *root, const strlist *rels, const char *out_zip)
{
    mz_zip_archive zip;
    size_t         i;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file_v2(&zip, out_zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64)) {
        return -1;
    }
    for (i = 0; i < rels->count; i++) {
        char abs[VAPORD_PATH_MAX];

        if (join2(abs, sizeof(abs), root, rels->items[i]) != 0) {
            mz_zip_writer_end(&zip);
            remove(out_zip);
            return -1;
        }
        if (!mz_zip_writer_add_file(&zip, rels->items[i], abs, NULL, 0,
                                    MZ_NO_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            remove(out_zip);
            return -1;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        remove(out_zip);
        return -1;
    }
    mz_zip_writer_end(&zip);
    return 0;
}

/* ---------------------------------------------------------------- inspect */

static int
is_installer_kit_rel(const char *rel)
{
    /* Disc/MSI kits stage the game under Setup/Data; that is not a finished
     * install. DirectX redistributables are the same kind of leftover. */
    return vapor_str_has_prefix(rel, "Setup/")
        || vapor_str_has_prefix(rel, "setup/")
        || vapor_str_has_prefix(rel, "DirectX/")
        || vapor_str_has_prefix(rel, "directx/");
}

static void
consider_exec(const char *rel, const char *slug, int want_win, int want_lin,
              int junk_ok, char *win_best, int *win_score, size_t winsz,
              char *lin_best, int *lin_score, size_t linsz)
{
    int s;

    if (is_installer_kit_rel(rel)) {
        return;
    }
    if (want_win) {
        s = score_exec(rel, slug, junk_ok);
        if (s > *win_score) {
            snprintf(win_best, winsz, "%s", rel);
            *win_score = s;
        }
    }
    if (want_lin) {
        s = score_exec(rel, slug, junk_ok);
        if (s > *lin_score) {
            snprintf(lin_best, linsz, "%s", rel);
            *lin_score = s;
        }
    }
}

static void
zip_common_prefix(mz_zip_archive *zip, char *prefix, size_t prefixsz)
{
    mz_uint i, count = mz_zip_reader_get_num_files(zip);
    char    first[VAPORD_PATH_MAX];
    size_t  first_len = 0;
    int     have = 0;

    prefix[0] = '\0';
    first[0] = '\0';
    for (i = 0; i < count; i++) {
        char        name[VAPORD_PATH_MAX];
        const char *slash;
        size_t      n;

        mz_zip_reader_get_filename(zip, i, name, sizeof(name));
        if (vapor_str_has_prefix(name, "__MACOSX")) {
            continue;
        }
        slash = strchr(name, '/');
        if (!slash) {
            prefix[0] = '\0';
            return;
        }
        n = (size_t)(slash - name) + 1; /* include slash */
        if (!have) {
            if (n >= sizeof(first)) {
                return;
            }
            memcpy(first, name, n);
            first[n] = '\0';
            first_len = n;
            have = 1;
            continue;
        }
        if (strncmp(name, first, first_len) != 0) {
            prefix[0] = '\0';
            return;
        }
    }
    if (have && first_len < prefixsz) {
        memcpy(prefix, first, first_len + 1);
    }
}

static int
inspect_zip(const char *path, const char *slug, char *win_exec, size_t winsz,
            char *lin_exec, size_t linsz, char *strip, size_t stripsz,
            int *has_iso)
{
    mz_zip_archive zip;
    mz_uint        i, count;
    int            win_score = -1, lin_score = -1;
    char           win_best[VAPORD_PATH_MAX] = "", lin_best[VAPORD_PATH_MAX] = "";

    win_exec[0] = lin_exec[0] = strip[0] = '\0';
    if (has_iso) {
        *has_iso = 0;
    }
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        return -1;
    }
    zip_common_prefix(&zip, strip, stripsz);
    count = mz_zip_reader_get_num_files(&zip);
    for (i = 0; i < count; i++) {
        char name[VAPORD_PATH_MAX];
        char rel[VAPORD_PATH_MAX];
        const char *use;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        if (vapor_str_has_prefix(name, "__MACOSX")) {
            continue;
        }
        use = name;
        if (strip[0] && vapor_str_has_prefix(name, strip)) {
            use = name + strlen(strip);
        }
        snprintf(rel, sizeof(rel), "%s", use);
        if (has_iso && is_iso_name(basename_of(rel))) {
            *has_iso = 1;
        }
        consider_exec(rel, slug, looks_windows_exec(rel),
                      looks_linux_exec_name(basename_of(rel)), 0, win_best,
                      &win_score, sizeof(win_best), lin_best, &lin_score,
                      sizeof(lin_best));
    }
    if (win_score < 0 && lin_score < 0) {
        /* Nothing but installers: take those rather than skipping the game. */
        for (i = 0; i < count; i++) {
            char name[VAPORD_PATH_MAX];
            char rel[VAPORD_PATH_MAX];
            const char *use;

            if (mz_zip_reader_is_file_a_directory(&zip, i)) {
                continue;
            }
            mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
            use = name;
            if (strip[0] && vapor_str_has_prefix(name, strip)) {
                use = name + strlen(strip);
            }
            snprintf(rel, sizeof(rel), "%s", use);
            consider_exec(rel, slug, looks_windows_exec(rel),
                          looks_linux_exec_name(basename_of(rel)), 1, win_best,
                          &win_score, sizeof(win_best), lin_best, &lin_score,
                          sizeof(lin_best));
        }
    }
    mz_zip_reader_end(&zip);
    if (win_best[0]) {
        snprintf(win_exec, winsz, "%s", win_best);
    }
    if (lin_best[0]) {
        snprintf(lin_exec, linsz, "%s", lin_best);
    }
    return (win_exec[0] || lin_exec[0]) ? 0 : 1;
}

static int
inspect_tree(const char *root, const char *rel, const char *slug,
             char *win_best, int *win_score, size_t winsz, char *lin_best,
             int *lin_score, size_t linsz, int junk_ok)
{
    char           abs[VAPORD_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (rel[0]) {
        if (join2(abs, sizeof(abs), root, rel) != 0) {
            return -1;
        }
    } else {
        snprintf(abs, sizeof(abs), "%s", root);
    }
    d = opendir(abs);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPORD_PATH_MAX];
        char        child_abs[VAPORD_PATH_MAX];
        struct stat st;

        if (skip_walk_dir(ent->d_name) && strcmp(ent->d_name, ".") != 0
            && strcmp(ent->d_name, "..") != 0) {
            continue;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (rel[0]) {
            if (join2(child_rel, sizeof(child_rel), rel, ent->d_name) != 0) {
                rc = -1;
                break;
            }
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (join2(child_abs, sizeof(child_abs), root, child_rel) != 0) {
            rc = -1;
            break;
        }
        if (lstat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (inspect_tree(root, child_rel, slug, win_best, win_score, winsz,
                             lin_best, lin_score, linsz, junk_ok)
                != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (looks_windows_exec(ent->d_name)) {
            consider_exec(child_rel, slug, 1, 0, junk_ok, win_best, win_score,
                          winsz, lin_best, lin_score, linsz);
        } else if (looks_linux_exec_name(ent->d_name)
                   && (is_elf_file(child_abs) || (st.st_mode & S_IXUSR)
                       || vapor_str_ends_with_ci(ent->d_name, ".sh"))) {
            consider_exec(child_rel, slug, 0, 1, junk_ok, win_best, win_score,
                          winsz, lin_best, lin_score, linsz);
        }
    }
    closedir(d);
    return rc;
}

static int
inspect_dir(const char *root, const char *slug, char *win_exec, size_t winsz,
            char *lin_exec, size_t linsz)
{
    int  win_score = -1, lin_score = -1;
    char win_best[VAPORD_PATH_MAX] = "", lin_best[VAPORD_PATH_MAX] = "";

    win_exec[0] = lin_exec[0] = '\0';
    if (inspect_tree(root, "", slug, win_best, &win_score, sizeof(win_best),
                     lin_best, &lin_score, sizeof(lin_best), 0)
        != 0) {
        return -1;
    }
    if (win_score < 0 && lin_score < 0) {
        inspect_tree(root, "", slug, win_best, &win_score, sizeof(win_best),
                     lin_best, &lin_score, sizeof(lin_best), 1);
    }
    if (win_best[0]) {
        snprintf(win_exec, winsz, "%s", win_best);
    }
    if (lin_best[0]) {
        snprintf(lin_exec, linsz, "%s", lin_best);
    }
    return (win_exec[0] || lin_exec[0]) ? 0 : 1;
}

/* Find the largest matching archive under `root`. `rel_out` is relative to
 * library_root's game folder. */
static int
find_largest(const char *root, const char *rel, int (*match)(const char *),
             char *rel_out, size_t relsz, uint64_t *best_size)
{
    char           abs[VAPORD_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (rel[0]) {
        if (join2(abs, sizeof(abs), root, rel) != 0) {
            return -1;
        }
    } else {
        snprintf(abs, sizeof(abs), "%s", root);
    }
    d = opendir(abs);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPORD_PATH_MAX];
        char        child_abs[VAPORD_PATH_MAX];
        struct stat st;

        if (skip_walk_dir(ent->d_name) && strcmp(ent->d_name, ".") != 0
            && strcmp(ent->d_name, "..") != 0) {
            continue;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (rel[0]) {
            if (join2(child_rel, sizeof(child_rel), rel, ent->d_name) != 0) {
                continue;
            }
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (join2(child_abs, sizeof(child_abs), root, child_rel) != 0) {
            continue;
        }
        if (lstat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (find_largest(root, child_rel, match, rel_out, relsz, best_size)
                != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (!match(ent->d_name)) {
            continue;
        }
        if ((uint64_t)st.st_size > *best_size) {
            *best_size = (uint64_t)st.st_size;
            snprintf(rel_out, relsz, "%s", child_rel);
        }
    }
    closedir(d);
    return rc;
}

static int
collect_matching(const char *root, const char *rel, int (*match)(const char *),
                 strlist *out)
{
    char           abs[VAPORD_PATH_MAX];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (rel[0]) {
        if (join2(abs, sizeof(abs), root, rel) != 0) {
            return -1;
        }
    } else {
        snprintf(abs, sizeof(abs), "%s", root);
    }
    d = opendir(abs);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[VAPORD_PATH_MAX];
        char        child_abs[VAPORD_PATH_MAX];
        struct stat st;

        if (skip_walk_dir(ent->d_name) && strcmp(ent->d_name, ".") != 0
            && strcmp(ent->d_name, "..") != 0) {
            continue;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (rel[0]) {
            if (join2(child_rel, sizeof(child_rel), rel, ent->d_name) != 0) {
                continue;
            }
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        }
        if (join2(child_abs, sizeof(child_abs), root, child_rel) != 0) {
            continue;
        }
        if (lstat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (collect_matching(root, child_rel, match, out) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (!match(ent->d_name)) {
            continue;
        }
        if (strlist_push(out, child_rel) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    return rc;
}

static int
find_cover(const char *dir, char *out, size_t outsz)
{
    static const char *const names[] = {
        "cover.png", "cover.jpg", "cover.jpeg", "folder.png", "folder.jpg", NULL
    };
    size_t i;

    for (i = 0; names[i]; i++) {
        char path[VAPORD_PATH_MAX];
        struct stat st;
        if (join2(path, sizeof(path), dir, names[i]) != 0) {
            continue;
        }
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)
            && (uint64_t)st.st_size <= VAPOR_MAX_COVER_BYTES) {
            snprintf(out, outsz, "%s", path);
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- vapor.json */

typedef struct {
    char id[VAPOR_ID_MAX + 1];
    char name[256];
    char version[VAPOR_VERSION_MAX + 1];
    char developer[128];
    char description[VAPOR_DESC_MAX];
    char windows_exec[VAPORD_PATH_MAX];
    char linux_exec[VAPORD_PATH_MAX];
    char setup_exec[VAPORD_PATH_MAX];
    char uninstall_exec[VAPORD_PATH_MAX];
    char cover[VAPORD_PATH_MAX];
    char package[VAPORD_PATH_MAX];
    char runtime[32];
    char dos_exec[VAPORD_PATH_MAX];
    char install_mode[32];
    char **saves;
    size_t nsaves;
    int  steam_appid;
} sidecar;

static void
sidecar_clear_saves(sidecar *s)
{
    size_t i;

    if (!s->saves) {
        s->nsaves = 0;
        return;
    }
    for (i = 0; i < s->nsaves; i++) {
        free(s->saves[i]);
    }
    free(s->saves);
    s->saves = NULL;
    s->nsaves = 0;
}

static int
runtime_name_ok(const char *s)
{
    size_t i;

    if (!s || !s[0] || strlen(s) > 31) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        char c = s[i];
        int  ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                  || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

static int
dos_exec_ok(const char *s)
{
    if (!s || !s[0] || strlen(s) >= VAPORD_PATH_MAX) {
        return 0;
    }
    if (s[0] == '/' || s[0] == '\\' || strstr(s, "..")) {
        return 0;
    }
    return 1;
}

static const char *
json_str(const cJSON *obj, const char *a, const char *b)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, a);
    if (!cJSON_IsString(v) || !v->valuestring) {
        v = b ? cJSON_GetObjectItemCaseSensitive(obj, b) : NULL;
    }
    if (!cJSON_IsString(v) || !v->valuestring) {
        return NULL;
    }
    return v->valuestring;
}

static int
load_sidecar(const char *dir, sidecar *out)
{
    char   path[VAPORD_PATH_MAX];
    char  *text;
    size_t len = 0;
    cJSON *root;
    const char *s;

    memset(out, 0, sizeof(*out));
    if (join2(path, sizeof(path), dir, "vapor.json") != 0) {
        return 1;
    }
    text = vapor_read_file(path, &len);
    if (!text) {
        return 1;
    }
    (void)len;
    root = cJSON_Parse(text);
    free(text);
    if (!root) {
        VLOG_WARN("discover: %s is not valid JSON", path);
        return -1;
    }
    if ((s = json_str(root, "id", NULL))) {
        snprintf(out->id, sizeof(out->id), "%s", s);
    }
    if ((s = json_str(root, "name", NULL))) {
        snprintf(out->name, sizeof(out->name), "%s", s);
    }
    if ((s = json_str(root, "version", NULL))) {
        snprintf(out->version, sizeof(out->version), "%s", s);
    }
    if ((s = json_str(root, "developer", NULL))) {
        snprintf(out->developer, sizeof(out->developer), "%s", s);
    }
    if ((s = json_str(root, "description", NULL))) {
        snprintf(out->description, sizeof(out->description), "%s", s);
    }
    if ((s = json_str(root, "windows_exec", "windows-exec"))) {
        snprintf(out->windows_exec, sizeof(out->windows_exec), "%s", s);
    }
    if ((s = json_str(root, "linux_exec", "linux-exec"))) {
        snprintf(out->linux_exec, sizeof(out->linux_exec), "%s", s);
    }
    if ((s = json_str(root, "setup_exec", "setup-exec"))) {
        snprintf(out->setup_exec, sizeof(out->setup_exec), "%s", s);
    }
    if ((s = json_str(root, "uninstall_exec", "uninstall-exec"))) {
        snprintf(out->uninstall_exec, sizeof(out->uninstall_exec), "%s", s);
    }
    if ((s = json_str(root, "cover", NULL))) {
        snprintf(out->cover, sizeof(out->cover), "%s", s);
    }
    if ((s = json_str(root, "package", NULL))) {
        snprintf(out->package, sizeof(out->package), "%s", s);
    }
    if ((s = json_str(root, "runtime", NULL))) {
        if (!runtime_name_ok(s)) {
            VLOG_WARN("discover: %s has an invalid runtime", path);
            cJSON_Delete(root);
            sidecar_clear_saves(out);
            return -1;
        }
        snprintf(out->runtime, sizeof(out->runtime), "%s", s);
    }
    if ((s = json_str(root, "dos_exec", "dos-exec"))) {
        if (!dos_exec_ok(s)) {
            VLOG_WARN("discover: %s dos_exec must be a relative path", path);
            cJSON_Delete(root);
            sidecar_clear_saves(out);
            return -1;
        }
        snprintf(out->dos_exec, sizeof(out->dos_exec), "%s", s);
    }
    if ((s = json_str(root, "install_mode", "install-mode"))) {
        if (!vapor_install_mode_is_known(s)) {
            VLOG_WARN("discover: %s has an unknown install_mode", path);
            cJSON_Delete(root);
            sidecar_clear_saves(out);
            return -1;
        }
        snprintf(out->install_mode, sizeof(out->install_mode), "%s", s);
    }
    {
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "saves");
        const cJSON *it;
        size_t       n = 0;

        if (arr && !cJSON_IsNull(arr)) {
            if (!cJSON_IsArray(arr)) {
                VLOG_WARN("discover: %s saves must be an array", path);
                cJSON_Delete(root);
                return -1;
            }
            cJSON_ArrayForEach(it, arr) {
                if (cJSON_IsString(it)) {
                    n++;
                }
            }
            if (n > VAPOR_SAVE_MAX_PATHS) {
                VLOG_WARN("discover: %s has too many save paths", path);
                cJSON_Delete(root);
                return -1;
            }
            if (n > 0) {
                out->saves = (char **)calloc(n, sizeof(*out->saves));
                if (!out->saves) {
                    cJSON_Delete(root);
                    return -1;
                }
                cJSON_ArrayForEach(it, arr) {
                    if (!cJSON_IsString(it) || !it->valuestring) {
                        continue;
                    }
                    if (!vapor_save_path_is_valid(it->valuestring)) {
                        VLOG_WARN("discover: %s save path is not allowed: %s",
                                  path, it->valuestring);
                        cJSON_Delete(root);
                        sidecar_clear_saves(out);
                        return -1;
                    }
                    out->saves[out->nsaves] = vapor_strdup(it->valuestring);
                    if (!out->saves[out->nsaves]) {
                        cJSON_Delete(root);
                        sidecar_clear_saves(out);
                        return -1;
                    }
                    out->nsaves++;
                }
            }
        }
    }
    {
        const cJSON *appid = cJSON_GetObjectItemCaseSensitive(root, "steam_appid");
        if (cJSON_IsNumber(appid) && appid->valuedouble > 0) {
            out->steam_appid = (int)appid->valuedouble;
        }
    }
    cJSON_Delete(root);
    return 0;
}

/* ---------------------------------------------------------------- ids */

static int
alloc_game_id(vapord *app, const char *folder, const char *wanted, char *out,
              size_t outsz)
{
    char other[256];
    int  n = 1;

    snprintf(out, outsz, "%s", wanted);
    for (;;) {
        int discovered = 0;
        int lookup = vapord_game_lookup(app->db, out, &discovered);
        int owned = vapord_discovered_by_id(app->db, out, other, sizeof(other));

        if (lookup < 0 || owned < 0) {
            return -1;
        }
        if (lookup == 1 && owned == 1) {
            return 0; /* unused */
        }
        if (owned == 0 && strcmp(other, folder) == 0) {
            return 0; /* this folder already owns the id */
        }
        if (lookup == 0 && !discovered) {
            VLOG_WARN("discover: skipping \"%s\": id \"%s\" is already used by "
                      "an admin-published game",
                      folder, out);
            return 1;
        }
        n++;
        if (n > 99) {
            return -1;
        }
        {
            char tmp[VAPOR_ID_MAX + 1];
            int  suffix = snprintf(tmp, sizeof(tmp), "-%d", n);
            int  keep = VAPOR_ID_MAX - suffix;
            if (keep < 1) {
                return -1;
            }
            snprintf(tmp, (size_t)keep + 1, "%s", wanted);
            snprintf(out, outsz, "%s-%d", tmp, n);
        }
    }
}

static int
rel_is_dos(const char *dir, const char *zip, const char *rel)
{
    char        full[VAPORD_PATH_MAX];
    struct stat st;

    if (!rel || !rel[0] || vapor_dos_name_is_junk(rel)) {
        return 0;
    }
    if (vapor_str_ends_with_ci(rel, ".com")) {
        return 1;
    }
    if (!vapor_str_ends_with_ci(rel, ".exe")) {
        return 0;
    }
    if (dir && dir[0] && join2(full, sizeof(full), dir, rel) == 0
        && stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
        return vapor_file_is_dos_exe(full);
    }
    if (zip && zip[0]) {
        return vapor_zip_entry_is_dos_exe(zip, rel);
    }
    return 0;
}

static int
add_target(vapor_manifest *m, const char *platform, const char *exec,
           const char *runtime)
{
    vapor_target *grown;
    vapor_target *t;

    grown = (vapor_target *)realloc(m->targets, (m->ntargets + 1) * sizeof(*grown));
    if (!grown) {
        return -1;
    }
    m->targets = grown;
    t = &m->targets[m->ntargets];
    memset(t, 0, sizeof(*t));
    t->platform = vapor_strdup(platform);
    t->arch = vapor_strdup("x86_64");
    t->exec = vapor_strdup(exec);
    if (runtime && runtime[0] && strcmp(runtime, "native") != 0) {
        t->runtime = vapor_strdup(runtime);
        if (!t->runtime) {
            return -1;
        }
    }
    if (!t->platform || !t->arch || !t->exec) {
        return -1;
    }
    if (strcmp(platform, "linux") == 0) {
        t->exec_bits = (char **)calloc(1, sizeof(*t->exec_bits));
        if (!t->exec_bits) {
            return -1;
        }
        t->exec_bits[0] = vapor_strdup(exec);
        if (!t->exec_bits[0]) {
            return -1;
        }
        t->nexec_bits = 1;
    }
    m->ntargets++;
    return 0;
}

static int
package_present(vapord *app, const vapor_manifest *m, const char *source_rel)
{
    char path[VAPORD_PATH_MAX];
    struct stat st;

    if (source_rel && *source_rel) {
        return vapord_library_resolve(&app->cfg, source_rel, path, sizeof(path))
               == 0;
    }
    if (vapord_content_path(&app->cfg, m->id, m->version, m->package.file, path,
                            sizeof(path))
        != 0) {
        return 0;
    }
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* True when the catalog still advertises format "iso". Those entries were
 * published before disc images were wrapped into zip; rediscover them so a
 * zip-only client can install. */
static int
catalog_format_is_iso(vapord *app, const char *id, const char *version)
{
    char           *json = NULL;
    vapor_manifest  m;
    char            err[256];
    int             is_iso = 0;

    if (vapord_version_manifest(app->db, id, version, &json) != 0 || !json) {
        return 0;
    }
    vapor_manifest_init(&m);
    if (vapor_manifest_parse(json, strlen(json), &m, err, sizeof(err)) == 0) {
        if (m.package.format && strcmp(m.package.format, "iso") == 0) {
            is_iso = 1;
        }
        vapor_manifest_free(&m);
    }
    free(json);
    return is_iso;
}

static int
publish_game(vapord *app, const char *folder, const char *abs_dir,
             const sidecar *meta, const char *id, const char *name,
             const char *version, const char *format, const char *package_rel,
             const char *package_abs, const char *package_file,
             const char *win_exec, const char *lin_exec,
             const char *strip_prefix, const char *cover_src,
             const char *fingerprint, int allow_no_target,
             const strlist *iso_rels)
{
    vapor_manifest m;
    char           version_dir[VAPORD_PATH_MAX];
    char           pkg_path[VAPORD_PATH_MAX];
    char           cover_name[64] = "";
    char           cover_dst[VAPORD_PATH_MAX];
    char          *json = NULL;
    char           sha[VAPOR_SHA256_HEX_LEN + 1];
    char           pretty[32];
    uint64_t       size = 0;
    char           source_rel[VAPORD_PATH_MAX] = "";
    int            in_place = (package_rel && package_rel[0]);
    int            wrap_file = !in_place && package_abs && package_abs[0];
    int            rc = -1;
    char           win_found[VAPORD_PATH_MAX] = "";
    char           lin_found[VAPORD_PATH_MAX] = "";
    char           extract_dir[VAPORD_PATH_MAX];
    char           iso_err[256];

    vapor_manifest_init(&m);

    if (vapord_content_path(&app->cfg, id, version, NULL, version_dir,
                            sizeof(version_dir))
        != 0) {
        VLOG_WARN("discover: cannot build content path for %s/%s", id, version);
        goto done;
    }
    if (vapord_content_mkdirs(version_dir) != 0) {
        VLOG_WARN("discover: cannot create %s", version_dir);
        goto done;
    }

    if (in_place) {
        if (join2(source_rel, sizeof(source_rel), folder, package_rel) != 0) {
            VLOG_WARN("discover: source path too long for %s", folder);
            goto done;
        }
        snprintf(pkg_path, sizeof(pkg_path), "%s", package_abs);
    } else if (wrap_file) {
        if (join2(pkg_path, sizeof(pkg_path), version_dir, PACKAGED_ZIP) != 0) {
            goto done;
        }
        if (join2(extract_dir, sizeof(extract_dir), version_dir, ".iso-unpack")
            != 0) {
            goto done;
        }
        rm_tree(extract_dir);
        if (iso_rels && iso_rels->count > 0) {
            size_t i;
            int    unpacked = 0;

            for (i = 0; i < iso_rels->count; i++) {
                char iso_abs[VAPORD_PATH_MAX];

                if (join2(iso_abs, sizeof(iso_abs), abs_dir, iso_rels->items[i])
                    != 0) {
                    continue;
                }
                VLOG_INFO("discover: unpacking disc image \"%s\" (%zu/%zu)",
                          basename_of(iso_rels->items[i]), i + 1,
                          iso_rels->count);
                if (vapor_iso_extract(iso_abs, extract_dir, iso_err,
                                      sizeof(iso_err))
                    != 0) {
                    VLOG_WARN("discover: cannot unpack %s (%s)",
                              basename_of(iso_rels->items[i]), iso_err);
                    continue;
                }
                unpacked++;
            }
            if (unpacked == 0) {
                VLOG_WARN("discover: cannot unpack disc images in \"%s\"; "
                          "wrapping the images as files",
                          folder);
                if (zip_files_store(abs_dir, iso_rels, pkg_path) != 0) {
                    VLOG_WARN("discover: failed to zip disc images for \"%s\"",
                              folder);
                    goto done;
                }
                allow_no_target = 1;
                goto iso_packaged;
            }
        } else {
            VLOG_INFO("discover: unpacking disc image \"%s\"",
                      basename_of(package_abs));
            if (vapor_iso_extract(package_abs, extract_dir, iso_err,
                                  sizeof(iso_err))
                != 0) {
                VLOG_WARN("discover: cannot unpack %s (%s); wrapping the image as a file",
                          basename_of(package_abs), iso_err);
                if (zip_file_store(package_abs, basename_of(package_abs), pkg_path)
                    != 0) {
                    VLOG_WARN("discover: failed to zip \"%s\"", package_abs);
                    goto done;
                }
                allow_no_target = 1;
                goto iso_packaged;
            }
        }
        if (copy_publisher_patches(abs_dir, "", extract_dir) != 0) {
            rm_tree(extract_dir);
            VLOG_WARN("discover: cannot copy publisher patch for \"%s\"",
                      folder);
            goto done;
        }
        unpack_wise_installers(extract_dir);
        vapor_disc_finish_install(extract_dir);
        fold_case_collisions(extract_dir);
        if (!(win_exec && *win_exec) && !(lin_exec && *lin_exec)) {
            inspect_dir(extract_dir, id, win_found, sizeof(win_found),
                        lin_found, sizeof(lin_found));
            if (win_found[0]) {
                win_exec = win_found;
            }
            if (lin_found[0]) {
                lin_exec = lin_found;
            }
        }
        VLOG_INFO("discover: packaging unpacked disc \"%s\"", folder);
        if (zip_directory(extract_dir, pkg_path) != 0) {
            rm_tree(extract_dir);
            VLOG_WARN("discover: failed to zip unpacked ISO for \"%s\"",
                      folder);
            goto done;
        }
        rm_tree(extract_dir);
    iso_packaged:
        package_file = PACKAGED_ZIP;
        format = "zip";
    } else {
        if (join2(pkg_path, sizeof(pkg_path), version_dir, PACKAGED_ZIP) != 0) {
            goto done;
        }
        VLOG_INFO("discover: packaging folder \"%s\"", folder);
        if (zip_directory(abs_dir, pkg_path) != 0) {
            VLOG_WARN("discover: failed to zip \"%s\"", folder);
            goto done;
        }
        package_file = PACKAGED_ZIP;
        format = "zip";
    }

    if (cover_src && *cover_src) {
        if (vapor_str_ends_with_ci(cover_src, ".png")) {
            snprintf(cover_name, sizeof(cover_name), "cover.png");
        } else {
            snprintf(cover_name, sizeof(cover_name), "cover.jpg");
        }
        if (join2(cover_dst, sizeof(cover_dst), version_dir, cover_name) != 0
            || copy_file(cover_src, cover_dst) != 0) {
            VLOG_WARN("discover: could not copy cover for %s", folder);
            cover_name[0] = '\0';
        }
    }

    if (file_size(pkg_path, &size) != 0) {
        VLOG_WARN("discover: cannot size %s", pkg_path);
        goto done;
    }
    vapor_format_bytes(size, pretty, sizeof(pretty));
    VLOG_INFO("discover: hashing %s (%s)", folder, pretty);
    if (vapor_sha256_file(pkg_path, sha) != 0) {
        VLOG_WARN("discover: cannot hash %s", pkg_path);
        goto done;
    }

    m.schema = VAPOR_MANIFEST_SCHEMA;
    m.id = vapor_strdup(id);
    m.name = vapor_strdup(name);
    m.version = vapor_strdup(version);
    m.developer = meta->developer[0] ? vapor_strdup(meta->developer) : NULL;
    m.description = meta->description[0] ? vapor_strdup(meta->description) : NULL;
    m.cover = cover_name[0] ? vapor_strdup(cover_name) : NULL;
    m.package.file = vapor_strdup(package_file);
    m.package.format = vapor_strdup(format);
    m.package.size = size;
    m.package.strip_prefix = (strip_prefix && *strip_prefix)
                                 ? vapor_strdup(strip_prefix)
                                 : NULL;
    memcpy(m.package.sha256, sha, sizeof(sha));
    if (!m.id || !m.name || !m.version || !m.package.file || !m.package.format) {
        goto done;
    }
    if (meta && meta->setup_exec[0]) {
        m.install.setup = vapor_strdup(meta->setup_exec);
        if (!m.install.setup) {
            goto done;
        }
    }
    if (meta && meta->uninstall_exec[0]) {
        m.install.uninstall = vapor_strdup(meta->uninstall_exec);
        if (!m.install.uninstall) {
            goto done;
        }
    }
    if (meta && meta->install_mode[0]) {
        m.install_mode = vapor_strdup(meta->install_mode);
        if (!m.install_mode) {
            goto done;
        }
    }
    if (meta && meta->nsaves) {
        size_t si;

        m.saves = (char **)calloc(meta->nsaves, sizeof(*m.saves));
        if (!m.saves) {
            goto done;
        }
        for (si = 0; si < meta->nsaves; si++) {
            m.saves[si] = vapor_strdup(meta->saves[si]);
            if (!m.saves[si]) {
                goto done;
            }
            m.nsaves++;
        }
    }
    {
        const char *win_runtime = NULL;
        int         explicit_native = meta && meta->runtime[0]
                                      && strcmp(meta->runtime, "native") == 0;

        if (meta && meta->runtime[0] && !explicit_native) {
            win_runtime = meta->runtime;
        } else if (!explicit_native && meta && meta->dos_exec[0]) {
            win_runtime = "dosbox";
        } else if (!explicit_native
                   && rel_is_dos(abs_dir, pkg_path, win_exec)) {
            win_runtime = "dosbox";
        }
        if (win_exec && *win_exec && add_target(&m, "windows", win_exec, win_runtime) != 0) {
            goto done;
        }
    }
    if (lin_exec && *lin_exec && add_target(&m, "linux", lin_exec, NULL) != 0) {
        goto done;
    }
    if (m.ntargets == 0 && !allow_no_target) {
        VLOG_WARN("discover: \"%s\" has no launchable executable; skipping",
                  folder);
        goto done;
    }

    json = vapor_manifest_serialize(&m);
    if (!json) {
        goto done;
    }
    {
        vapor_manifest check;
        char           err[256];
        if (vapor_manifest_parse(json, strlen(json), &check, err, sizeof(err))
            != 0) {
            VLOG_WARN("discover: generated manifest for %s is invalid: %s",
                      folder, err);
            goto done;
        }
        vapor_manifest_free(&check);
    }

    if (vapord_game_upsert(app->db, &m) != 0
        || vapord_version_upsert(app->db, &m, json) != 0
        || vapord_game_mark_discovered(app->db, id, 1) != 0) {
        VLOG_WARN("discover: cannot register %s", id);
        goto done;
    }
    if (in_place) {
        if (vapord_version_set_source(app->db, id, version, source_rel) != 0) {
            VLOG_WARN("discover: cannot store source path for %s", id);
            goto done;
        }
    } else {
        vapord_version_set_source(app->db, id, version, NULL);
    }

    {
        vapord_discovered_row row;
        memset(&row, 0, sizeof(row));
        snprintf(row.folder, sizeof(row.folder), "%s", folder);
        snprintf(row.game_id, sizeof(row.game_id), "%s", id);
        snprintf(row.fingerprint, sizeof(row.fingerprint), "%s", fingerprint);
        snprintf(row.version, sizeof(row.version), "%s", version);
        if (vapord_discovered_put(app->db, &row) != 0) {
            VLOG_WARN("discover: cannot record discovery of %s", folder);
            goto done;
        }
    }

    VLOG_INFO("discover: %s \"%s\" %s (%s, %s)", id, name, version, format,
              pretty);
    rc = 0;
    vapord_meta_enrich(app, id, name, version, meta->steam_appid, 0);

done:
    free(json);
    vapor_manifest_free(&m);
    return rc;
}

static int
process_folder(vapord *app, const char *folder, int *meta_left)
{
    char            abs[VAPORD_PATH_MAX];
    sidecar         meta;
    char            slug[VAPOR_ID_MAX + 1];
    char            id[VAPOR_ID_MAX + 1];
    char            name[256];
    char            version[VAPOR_VERSION_MAX + 1];
    char            zip_rel[VAPORD_PATH_MAX] = "";
    char            pkg_rel[VAPORD_PATH_MAX] = "";
    char            pkg_abs[VAPORD_PATH_MAX];
    char            pkg_file[256] = "";
    char            format[8] = "zip";
    char            win_exec[VAPORD_PATH_MAX] = "";
    char            lin_exec[VAPORD_PATH_MAX] = "";
    char            strip[VAPORD_PATH_MAX] = "";
    char            cover_src[VAPORD_PATH_MAX] = "";
    char            fingerprint[VAPOR_SHA256_HEX_LEN + 1];
    uint64_t        zip_size = 0;
    struct stat     st;
    vapord_discovered_row prev;
    int             kind_dir = 0;
    int             wrap_iso = 0;
    int             zip_has_iso = 0;
    int             have_exec;
    int             rc = 0;
    strlist         isos = { 0 };

    if (join2(abs, sizeof(abs), app->cfg.library_root, folder) != 0) {
        return -1;
    }
    if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return 0;
    }

    if (load_sidecar(abs, &meta) < 0) {
        return 0;
    }
    snprintf(name, sizeof(name), "%s", meta.name[0] ? meta.name : folder);
    if (meta.id[0]) {
        if (!vapor_id_is_valid(meta.id)) {
            VLOG_WARN("discover: vapor.json for \"%s\" has an invalid id", folder);
            rc = 0;
            goto out;
        }
        snprintf(slug, sizeof(slug), "%s", meta.id);
    } else if (vapor_id_slug(name, slug, sizeof(slug)) != 0) {
        VLOG_WARN("discover: cannot derive an id from \"%s\"", folder);
        rc = 0;
        goto out;
    }
    if (alloc_game_id(app, folder, slug, id, sizeof(id)) != 0) {
        rc = 0;
        goto out;
    }

    if (meta.package[0]) {
        if (join2(pkg_abs, sizeof(pkg_abs), abs, meta.package) != 0
            || stat(pkg_abs, &st) != 0 || !S_ISREG(st.st_mode)) {
            VLOG_WARN("discover: vapor.json package \"%s\" missing in %s",
                      meta.package, folder);
            rc = 0;
            goto out;
        }
        snprintf(pkg_rel, sizeof(pkg_rel), "%s", meta.package);
        snprintf(pkg_file, sizeof(pkg_file), "%s", basename_of(meta.package));
        if (is_disc_image_file(pkg_abs, pkg_file)) {
            wrap_iso = 1;
            snprintf(format, sizeof(format), "zip");
            snprintf(pkg_file, sizeof(pkg_file), "%s", PACKAGED_ZIP);
            if (strlist_push(&isos, meta.package) != 0) {
                rc = 0;
                goto out;
            }
        } else {
            snprintf(format, sizeof(format), "zip");
            inspect_zip(pkg_abs, slug, win_exec, sizeof(win_exec), lin_exec,
                        sizeof(lin_exec), strip, sizeof(strip), &zip_has_iso);
        }
    } else {
        find_largest(abs, "", is_zip_name, zip_rel, sizeof(zip_rel), &zip_size);
        if (collect_matching(abs, "", is_iso_name, &isos) != 0) {
            rc = 0;
            goto out;
        }
        drop_non_image_bins(abs, &isos);
        strlist_sort(&isos);
        have_exec = (inspect_dir(abs, slug, win_exec, sizeof(win_exec), lin_exec,
                                 sizeof(lin_exec))
                     == 0);

        if (zip_rel[0]) {
            snprintf(pkg_rel, sizeof(pkg_rel), "%s", zip_rel);
            snprintf(pkg_file, sizeof(pkg_file), "%s", basename_of(zip_rel));
            snprintf(format, sizeof(format), "zip");
            if (join2(pkg_abs, sizeof(pkg_abs), abs, zip_rel) != 0) {
                rc = 0;
                goto out;
            }
            win_exec[0] = lin_exec[0] = '\0';
            if (inspect_zip(pkg_abs, slug, win_exec, sizeof(win_exec), lin_exec,
                            sizeof(lin_exec), strip, sizeof(strip),
                            &zip_has_iso)
                < 0) {
                VLOG_WARN("discover: cannot read zip %s/%s", folder, zip_rel);
                rc = 0;
                goto out;
            }
        } else if (have_exec && !is_publisher_patch(win_exec)
                   && !is_publisher_patch(lin_exec)) {
            kind_dir = 1;
            snprintf(format, sizeof(format), "zip");
            snprintf(pkg_file, sizeof(pkg_file), "%s", PACKAGED_ZIP);
        } else if (isos.count > 0) {
            /* A patch exe beside the disc is not the game. Unpack the disc
             * and carry the patch into the install folder. */
            win_exec[0] = lin_exec[0] = '\0';
            wrap_iso = 1;
            snprintf(pkg_rel, sizeof(pkg_rel), "%s", isos.items[0]);
            snprintf(pkg_file, sizeof(pkg_file), "%s", PACKAGED_ZIP);
            snprintf(format, sizeof(format), "zip");
            if (join2(pkg_abs, sizeof(pkg_abs), abs, isos.items[0]) != 0) {
                rc = 0;
                goto out;
            }
        } else {
            VLOG_WARN("discover: skipping \"%s\": no zip, iso, or executable",
                      folder);
            rc = 0;
            goto out;
        }
    }

    if (meta.windows_exec[0]) {
        snprintf(win_exec, sizeof(win_exec), "%s", meta.windows_exec);
    }
    if (meta.linux_exec[0]) {
        snprintf(lin_exec, sizeof(lin_exec), "%s", meta.linux_exec);
    }
    if (meta.dos_exec[0]) {
        snprintf(win_exec, sizeof(win_exec), "%s", meta.dos_exec);
    }
    if (meta.install_mode[0]
        && (strcmp(meta.install_mode, "portable") == 0
            || strcmp(meta.install_mode, "keep_disc") == 0)
        && wrap_iso) {
        /* Leave disc images in the tree. Unpacking them is unpack_disc. */
        wrap_iso = 0;
        kind_dir = 1;
        pkg_rel[0] = '\0';
        pkg_abs[0] = '\0';
        snprintf(format, sizeof(format), "zip");
        snprintf(pkg_file, sizeof(pkg_file), "%s", PACKAGED_ZIP);
    }

    if (meta.cover[0]) {
        if (join2(cover_src, sizeof(cover_src), abs, meta.cover) != 0
            || stat(cover_src, &st) != 0) {
            cover_src[0] = '\0';
        }
    } else {
        find_cover(abs, cover_src, sizeof(cover_src));
    }

    if (kind_dir) {
        uint8_t acc[VAPOR_SHA256_DIGEST_LEN];
        memset(acc, 0, sizeof(acc));
        if (fingerprint_tree(abs, "", acc) != 0) {
            VLOG_WARN("discover: cannot fingerprint \"%s\"", folder);
            rc = 0;
            goto out;
        }
        vapor_sha256_hex(acc, fingerprint);
        if (stat(abs, &st) == 0) {
            snprintf(version, sizeof(version), "%lld", (long long)st.st_mtime);
        } else {
            snprintf(version, sizeof(version), "1");
        }
    } else if (wrap_iso && isos.count > 0) {
        int64_t max_mtime = 0;
        int64_t patch_mtime = 0;
        size_t  i;

        fingerprint_iso_list(abs, &isos, fingerprint, &patch_mtime);
        if (patch_mtime > max_mtime) {
            max_mtime = patch_mtime;
        }
        for (i = 0; i < isos.count; i++) {
            char iso_abs[VAPORD_PATH_MAX];

            if (join2(iso_abs, sizeof(iso_abs), abs, isos.items[i]) != 0
                || stat(iso_abs, &st) != 0) {
                continue;
            }
            if (st.st_mtime > max_mtime) {
                max_mtime = st.st_mtime;
            }
        }
        if (isos.count > 1) {
            snprintf(version, sizeof(version), "%lld-d%zu", (long long)max_mtime,
                     isos.count);
        } else {
            snprintf(version, sizeof(version), "%lld", (long long)max_mtime);
        }
    } else {
        if (stat(pkg_abs, &st) != 0) {
            rc = 0;
            goto out;
        }
        fingerprint_stat(format, pkg_rel, (uint64_t)st.st_size,
                         (int64_t)st.st_mtime, fingerprint);
        snprintf(version, sizeof(version), "%lld", (long long)st.st_mtime);
    }
    if (meta.version[0]) {
        if (!vapor_version_is_valid(meta.version)) {
            VLOG_WARN("discover: vapor.json for \"%s\" has an invalid version",
                      folder);
            rc = 0;
            goto out;
        }
        snprintf(version, sizeof(version), "%s", meta.version);
    }
    if (!vapor_version_is_valid(version)) {
        snprintf(version, sizeof(version), "1");
    }
    if (meta.runtime[0] || meta.dos_exec[0] || meta.install_mode[0] || meta.nsaves) {
        vapor_sha256 ctx;
        uint8_t      dig[VAPOR_SHA256_DIGEST_LEN];
        size_t       si;

        vapor_sha256_init(&ctx);
        vapor_sha256_update(&ctx, fingerprint, strlen(fingerprint));
        vapor_sha256_update(&ctx, meta.runtime, strlen(meta.runtime));
        vapor_sha256_update(&ctx, meta.dos_exec, strlen(meta.dos_exec));
        vapor_sha256_update(&ctx, meta.install_mode, strlen(meta.install_mode));
        for (si = 0; si < meta.nsaves; si++) {
            vapor_sha256_update(&ctx, meta.saves[si], strlen(meta.saves[si]));
        }
        vapor_sha256_final(&ctx, dig);
        vapor_sha256_hex(dig, fingerprint);
    }

    if (vapord_discovered_get(app->db, folder, &prev) == 0
        && strcmp(prev.fingerprint, fingerprint) == 0
        && strcmp(prev.game_id, id) == 0) {
        vapor_manifest probe;
        char           source_rel[VAPORD_PATH_MAX] = "";
        int            present = 0;

        if (!kind_dir && !wrap_iso && pkg_rel[0]
            && join2(source_rel, sizeof(source_rel), folder, pkg_rel) != 0) {
            source_rel[0] = '\0';
        }
        vapor_manifest_init(&probe);
        probe.id = vapor_strdup(id);
        probe.version = vapor_strdup(prev.version[0] ? prev.version : version);
        probe.package.file = vapor_strdup(pkg_file[0] ? pkg_file : PACKAGED_ZIP);
        present = probe.id && probe.version && probe.package.file
                  && package_present(app, &probe,
                                     source_rel[0] ? source_rel : NULL);
        if (present && probe.version
            && catalog_format_is_iso(app, id, probe.version)) {
            VLOG_INFO("discover: \"%s\" is still catalogued as iso; unpacking disc files",
                      folder);
            present = 0;
        }
        vapor_manifest_free(&probe);
        if (present) {
            if (meta_left && *meta_left > 0
                && vapord_meta_enrich(app, id, name,
                                      prev.version[0] ? prev.version : version,
                                      meta.steam_appid, 0)
                       == 1) {
                (*meta_left)--;
            }
            rc = 0;
            goto out;
        }
    }

    if (kind_dir) {
        pkg_abs[0] = '\0';
        pkg_rel[0] = '\0';
    } else if (wrap_iso) {
        /* Serve the generated zip from content_root; leave the ISO in the
         * drop folder. */
        pkg_rel[0] = '\0';
    }

    rc = publish_game(app, folder, abs, &meta, id, name, version, format,
                      pkg_rel, pkg_abs, pkg_file, win_exec, lin_exec, strip,
                      cover_src, fingerprint, wrap_iso || zip_has_iso,
                      wrap_iso ? &isos : NULL);
out:
    sidecar_clear_saves(&meta);
    strlist_free(&isos);
    return rc;
}

static void
prune_missing(vapord *app, const strlist *seen)
{
    vapord_discovered_row *rows = NULL;
    size_t                 n = 0, i;

    if (vapord_discovered_list(app->db, &rows, &n) != 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        int discovered = 0;

        if (strlist_contains(seen, rows[i].folder)) {
            continue;
        }
        VLOG_INFO("discover: \"%s\" is gone; dropping %s", rows[i].folder,
                  rows[i].game_id);
        if (vapord_game_lookup(app->db, rows[i].game_id, &discovered) == 0
            && discovered) {
            vapord_game_delete(app->db, rows[i].game_id);
        }
        vapord_discovered_delete(app->db, rows[i].folder);
    }
    free(rows);
}

int
vapord_discover(vapord *app)
{
    DIR           *d;
    struct dirent *ent;
    strlist        seen = { 0 };

    if (!app->cfg.library_root[0]) {
        return 0;
    }
    if (vapord_content_mkdirs(app->cfg.library_root) != 0) {
        VLOG_ERROR("discover: cannot create library_root \"%s\"",
                   app->cfg.library_root);
        return -1;
    }
    d = opendir(app->cfg.library_root);
    if (!d) {
        VLOG_ERROR("discover: cannot read library_root \"%s\": %s",
                   app->cfg.library_root, strerror(errno));
        return -1;
    }

    VLOG_INFO("discover: scanning %s", app->cfg.library_root);
    {
        int meta_left = 8;

        while ((ent = readdir(d)) != NULL) {
            char        abs[VAPORD_PATH_MAX];
            struct stat st;

            if (ent->d_name[0] == '.') {
                continue;
            }
            if (strlen(ent->d_name) >= 256) {
                VLOG_WARN("discover: folder name \"%s\" is too long; skipping",
                          ent->d_name);
                continue;
            }
            if (join2(abs, sizeof(abs), app->cfg.library_root, ent->d_name) != 0) {
                continue;
            }
            if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
                continue;
            }
            if (strlist_push(&seen, ent->d_name) != 0) {
                closedir(d);
                strlist_free(&seen);
                return -1;
            }
            process_folder(app, ent->d_name, &meta_left);
        }
    }
    closedir(d);

    prune_missing(app, &seen);
    strlist_free(&seen);
    return 0;
}
