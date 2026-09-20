/* Download, verify, extract. The ordering matters: the archive is hashed before
 * a single file is written into the library, so a truncated or tampered package
 * can never produce a half-installed game. */

#include "vapor/client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "net.h"
#include "platform.h"
#include "vapor/buf.h"
#include "vapor/iso9660.h"
#include "vapor/sha256.h"
#include "vapor/util.h"
#include "vapor/wise.h"

/* Kept inside the install directory so uninstall is a single tree removal and
 * launching works with the server unreachable. */
#define VAPOR_META_DIR  ".vapor"
#define VAPOR_META_FILE ".vapor/manifest.json"

static int
join(char *out, size_t outsz, const char *a, const char *b)
{
    int n = snprintf(out, outsz, "%s/%s", a, b);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

static int
install_dir_for(vapor_client *vc, const char *game_id, char *out, size_t outsz)
{
    if (!vapor_id_is_valid(game_id)) {
        vapor_client_set_error(vc, "\"%s\" is not a valid game id", game_id);
        return -1;
    }
    if (join(out, outsz, vc->cfg.library_dir, game_id) != 0) {
        vapor_client_set_error(vc, "library path is too long");
        return -1;
    }
    /* Games see this path via $INSTALL_DIR, so hand them their platform's
     * separator rather than a mix of the two. */
    vapor_plat_native_path(out);
    return 0;
}

static int
cache_path_for(vapor_client *vc, const char *game_id, const char *version,
               const char *package_file, char *out, size_t outsz)
{
    const char *file = (package_file && *package_file) ? package_file : "package.bin";
    int n = snprintf(out, outsz, "%s/%s/cache/%s-%s-%s", vc->cfg.library_dir,
                     VAPOR_META_DIR, game_id, version, file);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
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

/* --------------------------------------------------------------- manifest */

int
vapor_fetch_manifest(vapor_client *vc, const char *game_id, const char *version,
                     vapor_manifest *out)
{
    vapor_response r;
    char           path[512];
    char           err[256];

    if (!vapor_id_is_valid(game_id)) {
        vapor_client_set_error(vc, "\"%s\" is not a valid game id", game_id);
        return -1;
    }
    if (!version || !*version) {
        version = "latest";
    }
    if (strcmp(version, "latest") != 0 && !vapor_version_is_valid(version)) {
        vapor_client_set_error(vc, "\"%s\" is not a valid version", version);
        return -1;
    }

    snprintf(path, sizeof(path), "%s/%s/versions/%s/manifest", VAPOR_EP_GAMES,
             game_id, version);

    if (vapor_api_get(vc, path, 1, &r) != 0) {
        vapor_response_free(&r);
        return -1;
    }
    if (vapor_manifest_parse(r.body, r.body_len, out, err, sizeof(err)) != 0) {
        vapor_client_set_error(vc, "server sent an unusable manifest: %s", err);
        vapor_response_free(&r);
        return -1;
    }
    vapor_response_free(&r);
    return 0;
}

static int
write_local_manifest(vapor_client *vc, const char *install_dir,
                     const vapor_manifest *m)
{
    char  meta_dir[VAPOR_PATH_MAX], meta_file[VAPOR_PATH_MAX];
    char *json;
    FILE *f;

    if (join(meta_dir, sizeof(meta_dir), install_dir, VAPOR_META_DIR) != 0
        || join(meta_file, sizeof(meta_file), install_dir, VAPOR_META_FILE) != 0) {
        vapor_client_set_error(vc, "install path is too long");
        return -1;
    }
    if (vapor_plat_mkdirs(meta_dir) != 0) {
        vapor_client_set_error(vc, "cannot create %s", meta_dir);
        return -1;
    }

    json = vapor_manifest_serialize(m);
    if (!json) {
        vapor_client_set_error(vc, "out of memory");
        return -1;
    }
    f = fopen(meta_file, "w");
    if (!f) {
        free(json);
        vapor_client_set_error(vc, "cannot write %s", meta_file);
        return -1;
    }
    fputs(json, f);
    fclose(f);
    free(json);
    return 0;
}

int
vapor_read_local_manifest(vapor_client *vc, const char *game_id,
                          vapor_manifest *out)
{
    char  dir[VAPOR_PATH_MAX], file[VAPOR_PATH_MAX];
    char *json;
    char  err[256];
    size_t len = 0;

    if (install_dir_for(vc, game_id, dir, sizeof(dir)) != 0) {
        return -1;
    }
    if (join(file, sizeof(file), dir, VAPOR_META_FILE) != 0) {
        vapor_client_set_error(vc, "install path is too long");
        return -1;
    }
    json = vapor_read_file(file, &len);
    if (!json) {
        vapor_client_set_error(vc, "%s is missing; reinstall %s", file, game_id);
        return -1;
    }
    if (vapor_manifest_parse(json, len, out, err, sizeof(err)) != 0) {
        free(json);
        vapor_client_set_error(vc, "stored manifest for %s is unusable: %s",
                               game_id, err);
        return -1;
    }
    free(json);
    return 0;
}

/* ---------------------------------------------------------------- extract */

/* Rejects zip-slip: an archive entry must stay inside the install directory.
 * Absolute paths, drive letters, and any ".." component are refused outright
 * rather than normalised away. */
static int
safe_archive_path(const char *entry, const char *strip_prefix, char *out,
                  size_t outsz)
{
    const char *p = entry;
    char        clean[VAPOR_PATH_MAX];
    size_t      n = 0;

    if (!entry || !*entry) {
        return -1;
    }
    if (strip_prefix && *strip_prefix && vapor_str_has_prefix(p, strip_prefix)) {
        p += strlen(strip_prefix);
        while (*p == '/' || *p == '\\') {
            p++;
        }
        if (!*p) {
            return 1; /* this entry *was* the stripped directory */
        }
    }

    if (*p == '/' || *p == '\\') {
        return -1;
    }
    /* "C:..." or any colon: a Windows drive-relative path. */
    if (strchr(p, ':')) {
        return -1;
    }

    /* Copy, folding '\' to '/' and validating each component. */
    while (*p) {
        const char *seg = p;
        size_t      seglen;

        while (*p && *p != '/' && *p != '\\') {
            p++;
        }
        seglen = (size_t)(p - seg);

        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            return -1;
        }
        if (seglen == 0 || (seglen == 1 && seg[0] == '.')) {
            /* Collapse "//" and "./" rather than failing. */
            if (*p) {
                p++;
            }
            continue;
        }
        if (n + seglen + 2 > sizeof(clean)) {
            return -1;
        }
        if (n > 0) {
            clean[n++] = '/';
        }
        memcpy(clean + n, seg, seglen);
        n += seglen;

        if (*p) {
            p++;
        }
    }
    clean[n] = '\0';
    if (n == 0) {
        return 1;
    }
    if (n >= outsz) {
        return -1;
    }
    memcpy(out, clean, n + 1);
    return 0;
}

/* Creates the parent directories of a file path. */
static int
mkdirs_for_file(const char *path)
{
    char  tmp[VAPOR_PATH_MAX];
    char *slash;

    snprintf(tmp, sizeof(tmp), "%s", path);
    slash = strrchr(tmp, '/');
#if defined(_WIN32)
    {
        char *bs = strrchr(tmp, '\\');
        if (bs > slash) {
            slash = bs;
        }
    }
#endif
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    return vapor_plat_mkdirs(tmp);
}

static int
extract_archive(vapor_client *vc, const char *zip_path, const char *install_dir,
                const char *strip_prefix, size_t *out_files)
{
    mz_zip_archive zip;
    mz_uint        i, count;
    int            rc = -1;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        vapor_client_set_error(vc, "%s is not a readable zip archive", zip_path);
        return -1;
    }

    count = mz_zip_reader_get_num_files(&zip);
    *out_files = 0;

    for (i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        char                     rel[VAPOR_PATH_MAX];
        char                     dest[VAPOR_PATH_MAX];
        int                      safe;

        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            vapor_client_set_error(vc, "cannot read entry %u of %s",
                                   (unsigned)i, zip_path);
            goto done;
        }

        safe = safe_archive_path(st.m_filename, strip_prefix, rel, sizeof(rel));
        if (safe < 0) {
            /* Refuse the whole install: a package trying to escape its
             * directory is not something to partially apply. */
            vapor_client_set_error(vc,
                                   "archive entry \"%s\" tries to escape the "
                                   "install directory; refusing to extract",
                                   st.m_filename);
            goto done;
        }
        if (safe == 1) {
            continue; /* nothing left after stripping the prefix */
        }
        if (join(dest, sizeof(dest), install_dir, rel) != 0) {
            vapor_client_set_error(vc, "install path is too long for \"%s\"", rel);
            goto done;
        }
        vapor_plat_native_path(dest);

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            if (vapor_plat_mkdirs(dest) != 0) {
                vapor_client_set_error(vc, "cannot create %s", dest);
                goto done;
            }
            continue;
        }

        if (mkdirs_for_file(dest) != 0) {
            vapor_client_set_error(vc, "cannot create the directory for %s", dest);
            goto done;
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, dest, 0)) {
            vapor_client_set_error(vc, "cannot write %s", dest);
            goto done;
        }
        (*out_files)++;
    }
    rc = 0;

done:
    mz_zip_reader_end(&zip);
    return rc;
}

/* Walks the install tree and chmods anything matching the target's exec_bits.
 * Needed because zip does not carry the Unix executable bit. */
static int
apply_exec_bits(vapor_client *vc, const char *install_dir,
                const vapor_target *t, size_t *out_count)
{
    size_t i;

    *out_count = 0;
    if (!t || t->nexec_bits == 0) {
        return 0;
    }

    for (i = 0; i < t->nexec_bits; i++) {
        char        path[VAPOR_PATH_MAX];
        const char *pattern = t->exec_bits[i];

        /* A pattern with no wildcard is a plain path: chmod it directly rather
         * than walking the whole tree looking for it. */
        if (!strchr(pattern, '*') && !strchr(pattern, '?')) {
            if (join(path, sizeof(path), install_dir, pattern) != 0) {
                continue;
            }
            vapor_plat_native_path(path);
            if (vapor_plat_make_executable(path) == 0) {
                (*out_count)++;
            }
            continue;
        }
        if (vapor_plat_chmod_matching(install_dir, pattern, out_count) != 0) {
            vapor_client_set_error(vc, "cannot apply exec bits for \"%s\"",
                                   pattern);
            return -1;
        }
    }

    /* The launch target itself must always be executable, whether or not the
     * manifest remembered to list it. */
    if (t->exec) {
        char path[VAPOR_PATH_MAX];
        if (join(path, sizeof(path), install_dir, t->exec) == 0) {
            vapor_plat_native_path(path);
            vapor_plat_make_executable(path);
        }
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

static const char *
path_basename(const char *path)
{
    const char *s = strrchr(path, '/');
#if defined(_WIN32)
    {
        const char *b = strrchr(path, '\\');
        if (b > s) {
            s = b;
        }
    }
#endif
    return s ? s + 1 : path;
}

static int
looks_iso_name(const char *name)
{
    return vapor_str_ends_with_ci(name, ".iso")
        || vapor_str_ends_with_ci(name, ".img");
}

static int
is_disc_installer_name(const char *base)
{
    return vapor_str_eq_ci(base, "setup.exe")
        || vapor_str_eq_ci(base, "install.exe")
        || vapor_str_eq_ci(base, "installer.exe");
}

typedef struct {
    const char *root;
    int         unpacked;
} wise_scan;

static int
wise_install_cb(const char *rel, const char *abs, void *ud)
{
    wise_scan  *s = ud;
    const char *base = rel;
    const char *slash = strrchr(rel, '/');
    char        err[256];

    if (slash) {
        base = slash + 1;
    }
    if (!is_disc_installer_name(base)) {
        return 0;
    }
    memset(err, 0, sizeof(err));
    printf("extracting installer %s\n", rel);
    if (vapor_wise_extract(abs, s->root, err, sizeof(err)) == 0) {
        remove(abs);
        s->unpacked = 1;
    } else if (err[0]) {
        printf("  installer unpack failed (%s); leaving it in place\n", err);
    }
    return 0;
}

static int
is_junk_exec(const char *base)
{
    static const char *const junk[] = {
        "setup.exe", "install.exe", "installer.exe", "unins000.exe",
        "uninstall.exe", "dxsetup.exe", "autorun.exe", "vcredist", "vc_redist",
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
    if (vapor_str_has_prefix(base, "unins")
        && vapor_str_ends_with_ci(base, ".exe")) {
        return 1;
    }
    if (vapor_str_eq_ci(base, "upd.exe")
        || vapor_str_ends_with_ci(base, "up.exe")
        || vapor_str_ends_with_ci(base, "update.exe")) {
        return 1;
    }
    return 0;
}

static int
looks_windows_exec(const char *name)
{
    return vapor_str_ends_with_ci(name, ".exe");
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
        || vapor_str_ends_with_ci(name, ".xml")) {
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

typedef struct {
    char win_exec[VAPOR_PATH_MAX];
    char lin_exec[VAPOR_PATH_MAX];
    int  win_score;
    int  lin_score;
    int  junk_ok;
    int  has_iso;
    char iso_rel[VAPOR_PATH_MAX];
} install_scan;

static int
scan_install_cb(const char *rel, const char *abs, void *ud)
{
    install_scan *s = ud;
    const char   *base = path_basename(rel);
    int           score;

    if (looks_iso_name(base)) {
        s->has_iso = 1;
        if (!s->iso_rel[0]) {
            snprintf(s->iso_rel, sizeof(s->iso_rel), "%s", rel);
        }
        return 0;
    }

    if (!s->junk_ok && is_junk_exec(base)) {
        return 0;
    }
    score = 200 - path_depth(rel) * 15;
    if (looks_windows_exec(base) && score > s->win_score) {
        snprintf(s->win_exec, sizeof(s->win_exec), "%s", rel);
        s->win_score = score;
    } else if (looks_linux_exec_name(base)
               && (is_elf_file(abs) || vapor_str_ends_with_ci(base, ".sh"))
               && score > s->lin_score) {
        snprintf(s->lin_exec, sizeof(s->lin_exec), "%s", rel);
        s->lin_score = score;
    }
    return 0;
}

static int
add_discovered_target(vapor_manifest *m, const char *platform, const char *exec)
{
    vapor_target *grown;
    vapor_target *t;

    grown = (vapor_target *)realloc(m->targets,
                                    (m->ntargets + 1) * sizeof(*grown));
    if (!grown) {
        return -1;
    }
    m->targets = grown;
    t = &m->targets[m->ntargets];
    memset(t, 0, sizeof(*t));
    t->platform = vapor_strdup(platform);
    t->arch = vapor_strdup("x86_64");
    t->exec = vapor_strdup(exec);
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
discover_install_content(vapor_client *vc, const char *install_dir,
                         vapor_manifest *m, int *out_has_iso, char *iso_rel,
                         size_t isosz)
{
    install_scan scan;

    memset(&scan, 0, sizeof(scan));
    scan.win_score = -1;
    scan.lin_score = -1;
    if (vapor_plat_walk_files(install_dir, scan_install_cb, &scan) != 0) {
        vapor_client_set_error(vc, "cannot inspect extracted files in %s",
                               install_dir);
        return -1;
    }
    if (!scan.win_exec[0] && !scan.lin_exec[0] && !scan.has_iso) {
        scan.junk_ok = 1;
        scan.win_score = scan.lin_score = -1;
        if (vapor_plat_walk_files(install_dir, scan_install_cb, &scan) != 0) {
            vapor_client_set_error(vc, "cannot inspect extracted files in %s",
                                   install_dir);
            return -1;
        }
    }

    *out_has_iso = scan.has_iso;
    if (iso_rel && isosz) {
        snprintf(iso_rel, isosz, "%s", scan.iso_rel);
    }

    if (m->ntargets == 0) {
        if (scan.win_exec[0]
            && add_discovered_target(m, "windows", scan.win_exec) != 0) {
            vapor_client_set_error(vc, "out of memory");
            return -1;
        }
        if (scan.lin_exec[0]
            && add_discovered_target(m, "linux", scan.lin_exec) != 0) {
            vapor_client_set_error(vc, "out of memory");
            return -1;
        }
    }
    return 0;
}

static int
tree_has_iso_cb(const char *rel, const char *abs, void *ud)
{
    int *found = (int *)ud;

    (void)abs;
    if (looks_iso_name(path_basename(rel))) {
        *found = 1;
        return 1;
    }
    return 0;
}

static int
install_tree_has_iso(const char *install_dir)
{
    int found = 0;

    vapor_plat_walk_files(install_dir, tree_has_iso_cb, &found);
    return found;
}

/* ---------------------------------------------------------------- install */

int
vapor_install_game(vapor_client *vc, const char *game_id, const char *version,
                   const vapor_install_opts *opts, vapor_progress_fn cb,
                   void *ud)
{
    vapor_manifest      m;
    vapor_install       rec, existing;
    vapor_install_opts  defaults;
    const vapor_target *target = NULL;
    char                install_dir[VAPOR_PATH_MAX];
    char                cache_dir[VAPOR_PATH_MAX];
    char                zip_path[VAPOR_PATH_MAX];
    char                dl_path[512];
    char                actual_sha[VAPOR_SHA256_HEX_LEN + 1];
    uint64_t            on_disk = 0, size = 0;
    size_t              nfiles = 0, nexec = 0;
    int                 have_existing, rc = -1;
    int                 has_iso = 0;
    int                 unpacked_iso = 0;
    char                iso_rel[VAPOR_PATH_MAX] = "";

    if (!opts) {
        memset(&defaults, 0, sizeof(defaults));
        opts = &defaults;
    }

    if (vapor_fetch_manifest(vc, game_id, version, &m) != 0) {
        return -1;
    }

    have_existing = (vapor_db_get_install(vc, m.id, &existing) == 0);
    if (have_existing && !opts->force
        && strcmp(existing.version, m.version) == 0) {
        vapor_client_set_error(vc, "%s %s is already installed", m.id, m.version);
        vapor_manifest_free(&m);
        return 1;
    }

    if (install_dir_for(vc, m.id, install_dir, sizeof(install_dir)) != 0
        || cache_path_for(vc, m.id, m.version, m.package.file, zip_path,
                         sizeof(zip_path))
               != 0) {
        vapor_manifest_free(&m);
        return -1;
    }
    if ((size_t)snprintf(cache_dir, sizeof(cache_dir), "%s/%s/cache",
                         vc->cfg.library_dir, VAPOR_META_DIR)
        >= sizeof(cache_dir)) {
        vapor_client_set_error(vc, "library path is too long");
        vapor_manifest_free(&m);
        return -1;
    }
    if (vapor_plat_mkdirs(cache_dir) != 0) {
        vapor_client_set_error(vc, "cannot create %s", cache_dir);
        vapor_manifest_free(&m);
        return -1;
    }

    /* A previous run may have left a complete archive; skip straight to
     * verification and let the hash decide whether it is usable. */
    if (vapor_plat_file_size(zip_path, &size) == 0 && size == m.package.size) {
        if (vapor_sha256_file(zip_path, actual_sha) == 0
            && vapor_str_eq_ci(actual_sha, m.package.sha256)) {
            goto verified;
        }
    }

    snprintf(dl_path, sizeof(dl_path), "%s/%s/%s", VAPOR_EP_DOWNLOAD, m.id,
             m.version);
    if (vapor_http_download(vc, dl_path, zip_path, cb, ud) != 0) {
        vapor_manifest_free(&m);
        return -1;
    }

    if (vapor_plat_file_size(zip_path, &size) != 0) {
        vapor_client_set_error(vc, "downloaded archive is missing");
        vapor_manifest_free(&m);
        return -1;
    }
    if (size != m.package.size) {
        /* Removed so the next attempt starts clean instead of trying to resume
         * a file the server disagrees with. */
        remove(zip_path);
        vapor_client_set_error(vc,
                               "downloaded %llu bytes but the manifest says "
                               "%llu; removed the partial file, try again",
                               (unsigned long long)size,
                               (unsigned long long)m.package.size);
        vapor_manifest_free(&m);
        return -1;
    }
    if (vapor_sha256_file(zip_path, actual_sha) != 0) {
        vapor_client_set_error(vc, "cannot hash %s", zip_path);
        vapor_manifest_free(&m);
        return -1;
    }
    if (!vapor_str_eq_ci(actual_sha, m.package.sha256)) {
        remove(zip_path);
        vapor_client_set_error(vc,
                               "checksum mismatch for %s %s\n"
                               "  expected %s\n"
                               "  got      %s\n"
                               "the download was corrupted or tampered with; "
                               "the file has been deleted",
                               m.id, m.version, m.package.sha256, actual_sha);
        vapor_manifest_free(&m);
        return -1;
    }

verified:
    if (opts->verify_only) {
        printf("%s %s: archive verified (%s)\n", m.id, m.version,
               m.package.sha256);
        vapor_manifest_free(&m);
        return 0;
    }

    /* Only now is the existing install disturbed. */
    if (vapor_plat_exists(install_dir) && vapor_plat_remove_tree(install_dir) != 0) {
        vapor_client_set_error(vc, "cannot clear the previous install at %s",
                               install_dir);
        vapor_manifest_free(&m);
        return -1;
    }
    if (vapor_plat_mkdirs(install_dir) != 0) {
        vapor_client_set_error(vc, "cannot create %s", install_dir);
        vapor_manifest_free(&m);
        return -1;
    }

    if (!m.package.format || strcmp(m.package.format, "zip") == 0) {
        if (extract_archive(vc, zip_path, install_dir, m.package.strip_prefix,
                            &nfiles)
            != 0) {
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
    } else {
        char dest[VAPOR_PATH_MAX];
        if (join(dest, sizeof(dest), install_dir, m.package.file) != 0) {
            vapor_client_set_error(vc, "install path is too long");
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
        vapor_plat_native_path(dest);
        if (copy_file(zip_path, dest) != 0) {
            vapor_client_set_error(vc, "cannot copy %s into the install directory",
                                   m.package.file);
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
        nfiles = 1;
    }

    if (discover_install_content(vc, install_dir, &m, &has_iso, iso_rel,
                                 sizeof(iso_rel))
        != 0) {
        vapor_plat_remove_tree(install_dir);
        vapor_manifest_free(&m);
        return -1;
    }
    if (has_iso && iso_rel[0]) {
        char iso_path[VAPOR_PATH_MAX];
        char iso_err[256];

        if (join(iso_path, sizeof(iso_path), install_dir, iso_rel) != 0) {
            vapor_client_set_error(vc, "install path is too long");
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
        vapor_plat_native_path(iso_path);
        printf("extracting disc image %s\n", iso_rel);
        if (vapor_iso_extract(iso_path, install_dir, iso_err, sizeof(iso_err))
            != 0) {
            printf("  disc unpack failed (%s); leaving the image in place\n",
                   iso_err);
        } else {
            remove(iso_path);
            has_iso = 0;
            unpacked_iso = 1;
            iso_rel[0] = '\0';
            if (discover_install_content(vc, install_dir, &m, &has_iso, iso_rel,
                                         sizeof(iso_rel))
                != 0) {
                vapor_plat_remove_tree(install_dir);
                vapor_manifest_free(&m);
                return -1;
            }
        }
    }
    {
        wise_scan ws;

        memset(&ws, 0, sizeof(ws));
        ws.root = install_dir;
        vapor_plat_walk_files(install_dir, wise_install_cb, &ws);
        vapor_disc_finish_install(install_dir);
        if (ws.unpacked) {
            if (discover_install_content(vc, install_dir, &m, &has_iso, iso_rel,
                                         sizeof(iso_rel))
                != 0) {
                vapor_plat_remove_tree(install_dir);
                vapor_manifest_free(&m);
                return -1;
            }
        }
    }
    target = vapor_manifest_pick_target(&m, vapor_host_platform(),
                                        vapor_host_arch());
    if (target) {
        if (apply_exec_bits(vc, install_dir, target, &nexec) != 0) {
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
    } else if (!has_iso) {
        vapor_client_set_error(vc,
                               "%s %s has no %s/%s build and no disc image",
                               m.id, m.version, vapor_host_platform(),
                               vapor_host_arch());
        vapor_plat_remove_tree(install_dir);
        vapor_manifest_free(&m);
        return -1;
    }
    if (write_local_manifest(vc, install_dir, &m) != 0) {
        vapor_plat_remove_tree(install_dir);
        vapor_manifest_free(&m);
        return -1;
    }

    vapor_plat_dir_size(install_dir, &on_disk);

    memset(&rec, 0, sizeof(rec));
    snprintf(rec.game_id, sizeof(rec.game_id), "%s", m.id);
    snprintf(rec.version, sizeof(rec.version), "%s", m.version);
    snprintf(rec.name, sizeof(rec.name), "%s", m.name ? m.name : m.id);
    snprintf(rec.install_dir, sizeof(rec.install_dir), "%s", install_dir);
    rec.installed_at = vapor_now_unix();
    rec.size_on_disk = on_disk;

    if (vapor_db_record_install(vc, &rec) != 0) {
        vapor_manifest_free(&m);
        return -1;
    }

    if (!opts->keep_download) {
        remove(zip_path);
    }

    {
        char pretty[32];
        vapor_format_bytes(on_disk, pretty, sizeof(pretty));
        printf("installed %s %s\n", m.id, m.version);
        printf("  location ... %s\n", install_dir);
        printf("  contents ... %zu file(s), %s\n", nfiles, pretty);
        if (nexec > 0) {
            printf("  exec bits .. %zu file(s) made executable\n", nexec);
        }
        if (has_iso) {
            printf("  disc image . %s\n", iso_rel[0] ? iso_rel : "(found)");
        }
        if (unpacked_iso) {
            printf("  disc image . unpacked into the install directory\n");
        }
        if (!target && has_iso) {
            printf("  launch ..... disc image (no native executable)\n");
        }
        if (have_existing && strcmp(existing.version, m.version) != 0) {
            printf("  upgraded ... from %s\n", existing.version);
        }
    }
    rc = 0;
    vapor_manifest_free(&m);
    return rc;
}

int
vapor_uninstall_game(vapor_client *vc, const char *game_id)
{
    vapor_install rec;
    char          install_dir[VAPOR_PATH_MAX];

    if (vapor_db_get_install(vc, game_id, &rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed", game_id);
        return -1;
    }
    if (install_dir_for(vc, game_id, install_dir, sizeof(install_dir)) != 0) {
        return -1;
    }

    /* Trust the recorded path only if it still sits under the library, so a
     * hand-edited database cannot turn uninstall into an arbitrary delete. */
    if (rec.install_dir[0] && strcmp(rec.install_dir, install_dir) != 0) {
        if (!vapor_str_has_prefix(rec.install_dir, vc->cfg.library_dir)) {
            vapor_client_set_error(vc,
                                   "recorded install path %s is outside the "
                                   "library; not deleting anything",
                                   rec.install_dir);
            return -1;
        }
        snprintf(install_dir, sizeof(install_dir), "%s", rec.install_dir);
    }

    if (vapor_plat_exists(install_dir)
        && vapor_plat_remove_tree(install_dir) != 0) {
        vapor_client_set_error(vc, "cannot remove %s", install_dir);
        return -1;
    }
    if (vapor_db_forget_install(vc, game_id) != 0) {
        vapor_client_set_error(vc, "removed the files but could not update the "
                                  "local database");
        return -1;
    }

    printf("removed %s (%s)\n", rec.name[0] ? rec.name : game_id, rec.version);
    return 0;
}

int
vapor_verify_install(vapor_client *vc, const char *game_id)
{
    vapor_manifest      m;
    vapor_install       rec;
    const vapor_target *t;
    char                install_dir[VAPOR_PATH_MAX];
    char                exec_path[VAPOR_PATH_MAX];
    int                 problems = 0;

    if (vapor_db_get_install(vc, game_id, &rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed", game_id);
        return -1;
    }
    if (install_dir_for(vc, game_id, install_dir, sizeof(install_dir)) != 0) {
        return -1;
    }
    if (vapor_read_local_manifest(vc, game_id, &m) != 0) {
        return -1;
    }

    if (strcmp(m.version, rec.version) != 0) {
        printf("  the stored manifest says %s but the database says %s\n",
               m.version, rec.version);
        problems++;
    }

    t = vapor_manifest_pick_target(&m, vapor_host_platform(), vapor_host_arch());
    if (!t) {
        if (install_tree_has_iso(install_dir)) {
            printf("  disc image (no launch target)\n");
        } else {
            printf("  no %s/%s target in the manifest\n", vapor_host_platform(),
                   vapor_host_arch());
            problems++;
        }
    } else if (join(exec_path, sizeof(exec_path), install_dir, t->exec) != 0) {
        printf("  the launch target path is too long\n");
        problems++;
    } else {
        vapor_plat_native_path(exec_path);
        if (!vapor_plat_exists(exec_path)) {
            printf("  the launch target %s is missing\n", t->exec);
            problems++;
        }
    }

    if (problems == 0) {
        printf("%s %s matches its manifest\n", game_id, rec.version);
    } else {
        printf("%s %s has %d problem(s); reinstall to repair\n", game_id,
               rec.version, problems);
    }
    vapor_manifest_free(&m);
    return problems == 0 ? 0 : 1;
}
