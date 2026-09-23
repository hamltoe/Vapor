/* Download, verify, extract, and (on Windows) run leftover installers.
 * Progress is reported across that whole lifetime so the GUI bar does not
 * freeze at 100% after the archive has landed. The archive is hashed before
 * a single file is written into the library, so a truncated or tampered
 * package can never produce a half-installed game. */

#include "vapor/client.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "miniz.h"
#include "installer.h"
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

/* Overall install progress is a 0..INSTALL_PROGRESS_MAX tick scale so the
 * GUI bar covers download, hash, extract, disc unpack, and Windows setup
 * instead of slamming to 100% when the HTTP transfer finishes. Phase
 * ranges are fixed so the bar never jumps backwards when we later learn
 * there is an ISO or a setup.exe. */
#define INSTALL_PROGRESS_MAX 10000ull

typedef enum {
    INST_PH_DOWNLOAD = 0,
    INST_PH_VERIFY,
    INST_PH_EXTRACT,
    INST_PH_POST,
    INST_PH_SETUP,
    INST_PH_DONE
} inst_phase;

static const uint64_t inst_phase_end[] = {
    5000,  /* download:  0-50%  */
    6000,  /* verify:   50-60%  */
    8500,  /* extract:  60-85%  */
    9200,  /* post:     85-92%  */
    9900,  /* setup:    92-99%  */
    10000  /* done:      100%   */
};

typedef struct {
    vapor_progress_fn cb;
    void             *ud;
    vapor_status_fn   on_status;
    void             *on_status_ud;
    inst_phase        phase;
    int               aborted;
    int               last_status_pct;
} inst_progress;

typedef struct {
    vapor_progress_fn cb;
    void             *ud;
    vapor_status_fn   on_status;
    void             *on_status_ud;
} setup_hooks;

static int setup_game_impl(vapor_client *vc, const char *game_id,
                           const setup_hooks *hooks);

static uint64_t
inst_phase_begin(inst_phase p)
{
    return p == INST_PH_DOWNLOAD ? 0 : inst_phase_end[(int)p - 1];
}

static uint64_t
scale_u64(uint64_t n, uint64_t num, uint64_t den)
{
    if (den == 0 || num == 0) {
        return 0;
    }
    if (n > UINT64_MAX / num) {
        return (n / den) * num + ((n % den) * num) / den;
    }
    return (n * num) / den;
}

static int
progress_emit(inst_progress *p, uint64_t overall)
{
    if (!p) {
        return 0;
    }
    if (overall > INSTALL_PROGRESS_MAX) {
        overall = INSTALL_PROGRESS_MAX;
    }
    if (p->cb && p->cb(p->ud, overall, INSTALL_PROGRESS_MAX) != 0) {
        p->aborted = 1;
        return -1;
    }
    return 0;
}

static int
progress_report_span(inst_progress *p, uint64_t span_i, uint64_t span_n,
                     uint64_t local_done, uint64_t local_total)
{
    uint64_t start, end, span, slice_start, slice_end, slice, overall;

    if (!p) {
        return 0;
    }
    if (p->aborted) {
        return -1;
    }
    start = inst_phase_begin(p->phase);
    end = inst_phase_end[p->phase];
    span = end - start;
    if (span_n == 0) {
        span_n = 1;
    }
    if (span_i >= span_n) {
        span_i = span_n - 1;
    }
    slice_start = start + scale_u64(span_i, span, span_n);
    slice_end = start + scale_u64(span_i + 1, span, span_n);
    slice = slice_end - slice_start;
    if (local_total == 0) {
        overall = slice_start;
    } else if (local_done >= local_total) {
        overall = slice_end;
    } else {
        overall = slice_start + scale_u64(local_done, slice, local_total);
    }
    return progress_emit(p, overall);
}

static int
progress_report(inst_progress *p, uint64_t local_done, uint64_t local_total)
{
    return progress_report_span(p, 0, 1, local_done, local_total);
}

static void
progress_status(inst_progress *p, const char *status)
{
    if (p && p->on_status && status && *status) {
        p->on_status(p->on_status_ud, status);
    }
}

static int
progress_enter(inst_progress *p, inst_phase phase, const char *status)
{
    if (!p) {
        return 0;
    }
    p->phase = phase;
    p->last_status_pct = -1;
    progress_status(p, status);
    return progress_report(p, 0, 1);
}

static int
progress_finish_phase(inst_progress *p)
{
    return progress_report(p, 1, 1);
}

static int
progress_complete(inst_progress *p)
{
    if (!p) {
        return 0;
    }
    p->phase = INST_PH_DONE;
    return progress_emit(p, INSTALL_PROGRESS_MAX);
}

static int
progress_cancelled(inst_progress *p, vapor_client *vc)
{
    if (!p || !p->aborted) {
        return 0;
    }
    vapor_client_set_error(vc, "install cancelled");
    return 1;
}

static int
on_dl_progress(void *ud, uint64_t done, uint64_t total)
{
    inst_progress *p = (inst_progress *)ud;
    char           a[32], b[32], msg[160];
    int            pct;

    if (total > 0) {
        pct = (int)scale_u64(done, 100, total);
        if (pct != p->last_status_pct) {
            p->last_status_pct = pct;
            vapor_format_bytes(done, a, sizeof(a));
            vapor_format_bytes(total, b, sizeof(b));
            snprintf(msg, sizeof(msg), "downloading... %s of %s", a, b);
            progress_status(p, msg);
        }
    }
    return progress_report(p, done, total > 0 ? total : 1) != 0 ? 1 : 0;
}

typedef struct {
    inst_progress *p;
    uint64_t       span_i;
    uint64_t       span_n;
} iso_progress_wrap;

static int
on_iso_progress(void *ud, uint64_t done, uint64_t total)
{
    iso_progress_wrap *w = (iso_progress_wrap *)ud;

    return progress_report_span(w->p, w->span_i, w->span_n, done,
                                total > 0 ? total : 1)
                   != 0
               ? 1
               : 0;
}

static int
on_setup_progress(void *ud, uint64_t done, uint64_t total)
{
    inst_progress *p = (inst_progress *)ud;

    return progress_report(p, done, total > 0 ? total : 1) != 0 ? 1 : 0;
}

static void
on_setup_status(void *ud, const char *status)
{
    progress_status((inst_progress *)ud, status);
}

static int
hash_file(vapor_client *vc, const char *path, char out_hex[VAPOR_SHA256_HEX_LEN + 1],
          inst_progress *prog)
{
    uint8_t      digest[VAPOR_SHA256_DIGEST_LEN];
    vapor_sha256 c;
    FILE        *f;
    uint8_t     *tmp;
    size_t       n;
    uint64_t     total = 0, done = 0;
    static const size_t chunk = 65536;

    if (vapor_plat_file_size(path, &total) != 0) {
        vapor_client_set_error(vc, "cannot hash %s", path);
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        vapor_client_set_error(vc, "cannot hash %s", path);
        return -1;
    }
    tmp = (uint8_t *)malloc(chunk);
    if (!tmp) {
        fclose(f);
        vapor_client_set_error(vc, "out of memory");
        return -1;
    }

    vapor_sha256_init(&c);
    if (progress_report(prog, 0, total > 0 ? total : 1) != 0) {
        free(tmp);
        fclose(f);
        vapor_client_set_error(vc, "install cancelled");
        return -1;
    }
    while ((n = fread(tmp, 1, chunk, f)) > 0) {
        vapor_sha256_update(&c, tmp, n);
        done += (uint64_t)n;
        if (progress_report(prog, done, total > 0 ? total : done) != 0) {
            free(tmp);
            fclose(f);
            vapor_client_set_error(vc, "install cancelled");
            return -1;
        }
    }
    if (ferror(f)) {
        free(tmp);
        fclose(f);
        vapor_client_set_error(vc, "cannot hash %s", path);
        return -1;
    }
    free(tmp);
    fclose(f);
    vapor_sha256_final(&c, digest);
    vapor_sha256_hex(digest, out_hex);
    return 0;
}

static int
copy_file(const char *src, const char *dst, inst_progress *prog)
{
    FILE    *in, *out;
    char     buf[64 * 1024];
    size_t   n;
    int      rc = 0;
    uint64_t total = 0, done = 0;

    if (vapor_plat_file_size(src, &total) != 0) {
        total = 0;
    }
    in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    if (progress_report(prog, 0, total > 0 ? total : 1) != 0) {
        fclose(out);
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
        done += (uint64_t)n;
        if (progress_report(prog, done, total > 0 ? total : done) != 0) {
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
    vapor_install rec;
    char  dir[VAPOR_PATH_MAX], file[VAPOR_PATH_MAX];
    char *json;
    char  err[256];
    size_t len = 0;

    /* Prefer the downloaded kit: after a Windows installer, rec.install_dir
     * may be Program Files, but the local .vapor copy stays with the payload. */
    if (vapor_db_get_install(vc, game_id, &rec) == 0 && rec.payload_dir[0]) {
        snprintf(dir, sizeof(dir), "%s", rec.payload_dir);
        vapor_plat_native_path(dir);
    } else if (install_dir_for(vc, game_id, dir, sizeof(dir)) != 0) {
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
                const char *strip_prefix, size_t *out_files, inst_progress *prog)
{
    mz_zip_archive zip;
    mz_uint        i, count;
    int            rc = -1;
    uint64_t       unzip_total = 0, unzip_done = 0;

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
        int                      safe;

        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            continue;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }
        safe = safe_archive_path(st.m_filename, strip_prefix, rel, sizeof(rel));
        if (safe != 0) {
            continue;
        }
        unzip_total += (uint64_t)st.m_uncomp_size;
    }
    if (unzip_total == 0) {
        unzip_total = count > 0 ? (uint64_t)count : 1;
    }
    if (progress_report(prog, 0, unzip_total) != 0) {
        vapor_client_set_error(vc, "install cancelled");
        goto done;
    }

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
        unzip_done += (uint64_t)st.m_uncomp_size;
        if (unzip_done == 0) {
            unzip_done = (uint64_t)(*out_files);
        }
        if (prog && (i % 4 == 0 || i + 1 == count)) {
            char msg[160];

            snprintf(msg, sizeof(msg), "extracting... %u / %u files",
                     (unsigned)(i + 1), (unsigned)count);
            progress_status(prog, msg);
        }
        if (progress_report(prog, unzip_done, unzip_total) != 0) {
            vapor_client_set_error(vc, "install cancelled");
            goto done;
        }
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

static void
path_dirname(const char *path, char *out, size_t outsz)
{
    const char *base = path_basename(path);
    size_t      n = (size_t)(base - path);

    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\')) {
        n--;
    }
    if (n == 0 || n >= outsz) {
        snprintf(out, outsz, ".");
        return;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

/* If `path` is an MSI sitting next to setup.exe/install.exe, run that wrapper
 * instead: Windows installers often reject msiexec /i on a wrapped package. */
static void
prefer_dir_bootstrapper(char *path, size_t pathsz)
{
    char               dir[VAPOR_PATH_MAX];
    char               cand[VAPOR_PATH_MAX];
    static const char *const names[] = {
        "setup.exe", "install.exe", "installer.exe", NULL
    };
    int i;

    if (!vapor_str_ends_with_ci(path, ".msi")) {
        return;
    }
    path_dirname(path, dir, sizeof(dir));
    for (i = 0; names[i]; i++) {
        if (join(cand, sizeof(cand), dir, names[i]) != 0) {
            continue;
        }
        vapor_plat_native_path(cand);
        if (vapor_plat_exists(cand)) {
            snprintf(path, pathsz, "%s", cand);
            return;
        }
    }
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

#define VAPOR_MAX_DISCS 32

static int
is_installer_kit_rel(const char *rel)
{
    return vapor_str_has_prefix(rel, "Setup/")
        || vapor_str_has_prefix(rel, "setup/")
        || vapor_str_has_prefix(rel, "DirectX/")
        || vapor_str_has_prefix(rel, "directx/");
}

static int
is_helper_msi(const char *base)
{
    return vapor_str_has_prefix(base, "ISScript")
        || vapor_str_has_prefix(base, "isscript");
}

static int
is_gog_setup_name(const char *base)
{
    return vapor_str_has_prefix(base, "setup_")
        && vapor_str_ends_with_ci(base, ".exe");
}

static int
is_setup_payload_name(const char *rel)
{
    const char *base = path_basename(rel);

    if (is_disc_installer_name(base) || is_gog_setup_name(base)) {
        return 1;
    }
    if (vapor_str_ends_with_ci(base, ".msi") && !is_helper_msi(base)
        && !is_installer_kit_rel(rel)) {
        return 1;
    }
    return 0;
}

typedef struct {
    char win_exec[VAPOR_PATH_MAX];
    char lin_exec[VAPOR_PATH_MAX];
    int  win_score;
    int  lin_score;
    int  junk_ok;
    int  niso;
    char iso_rel[VAPOR_MAX_DISCS][VAPOR_PATH_MAX];
    char want_slug[VAPOR_ID_MAX + 1];
    int  has_installer;
    char setup_rel[VAPOR_PATH_MAX];
    int  setup_score;
    int  allow_kit;
} install_scan;

static int
scan_install_cb(const char *rel, const char *abs, void *ud)
{
    install_scan *s = ud;
    const char   *base = path_basename(rel);
    int           score;

    if (looks_iso_name(base)) {
        if (s->niso < VAPOR_MAX_DISCS) {
            snprintf(s->iso_rel[s->niso], sizeof(s->iso_rel[s->niso]), "%s",
                     rel);
            s->niso++;
        }
        return 0;
    }
    if (is_setup_payload_name(rel)) {
        int setup_score = 200 - path_depth(rel) * 15;

        s->has_installer = 1;
        /* Bootstrappers (setup.exe) must be launched, not an inner .msi they wrap:
         * many Windows installers refuse a raw MSI with "run Setup.exe instead".
         * vapor_plat_run_ui then waits for helper processes the bootstrapper starts. */
        if (is_disc_installer_name(base) || is_gog_setup_name(base)) {
            setup_score += 80;
        } else if (vapor_str_ends_with_ci(base, ".exe")) {
            setup_score += 50;
        } else if (vapor_str_ends_with_ci(base, ".msi") && !is_helper_msi(base)) {
            setup_score += 20;
        }
        if (setup_score > s->setup_score) {
            snprintf(s->setup_rel, sizeof(s->setup_rel), "%s", rel);
            s->setup_score = setup_score;
        }
    }
    if (!s->allow_kit && is_installer_kit_rel(rel)) {
        return 0;
    }

    if (!s->junk_ok && is_junk_exec(base)) {
        return 0;
    }
    score = 200 - path_depth(rel) * 15;
    if (s->want_slug[0]) {
        char stem[VAPOR_ID_MAX + 1];

        if (vapor_id_slug_stem(base, stem, sizeof(stem)) == 0
            && vapor_slug_match(stem, s->want_slug)) {
            score += 80;
        }
    }
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
cmp_iso_rel(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int
discover_install_content(vapor_client *vc, const char *install_dir,
                         vapor_manifest *m, install_scan *out)
{
    install_scan scan;

    memset(&scan, 0, sizeof(scan));
    scan.win_score = -1;
    scan.lin_score = -1;
    scan.setup_score = -1;
    if (m->id) {
        snprintf(scan.want_slug, sizeof(scan.want_slug), "%s", m->id);
    }
    if (vapor_plat_walk_files(install_dir, scan_install_cb, &scan) != 0) {
        vapor_client_set_error(vc, "cannot inspect extracted files in %s",
                               install_dir);
        return -1;
    }
    if (!scan.win_exec[0] && !scan.lin_exec[0] && scan.niso == 0
        && !scan.has_installer) {
        scan.junk_ok = 1;
        scan.win_score = scan.lin_score = -1;
        scan.niso = 0;
        if (vapor_plat_walk_files(install_dir, scan_install_cb, &scan) != 0) {
            vapor_client_set_error(vc, "cannot inspect extracted files in %s",
                                   install_dir);
            return -1;
        }
    }
    if (scan.niso > 1) {
        qsort(scan.iso_rel, (size_t)scan.niso, sizeof(scan.iso_rel[0]),
              cmp_iso_rel);
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
    if (out) {
        *out = scan;
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

static int
set_windows_exec(vapor_manifest *m, const char *exec)
{
    size_t i;

    for (i = 0; i < m->ntargets; i++) {
        if (m->targets[i].platform
            && strcmp(m->targets[i].platform, "windows") == 0) {
            free(m->targets[i].exec);
            m->targets[i].exec = vapor_strdup(exec);
            return m->targets[i].exec ? 0 : -1;
        }
    }
    return add_discovered_target(m, "windows", exec);
}

static int
scan_tree_execs(const char *dir, const char *want_slug, install_scan *out)
{
    memset(out, 0, sizeof(*out));
    out->win_score = -1;
    out->lin_score = -1;
    out->setup_score = -1;
    if (want_slug && *want_slug) {
        snprintf(out->want_slug, sizeof(out->want_slug), "%s", want_slug);
    }
    return vapor_plat_walk_files(dir, scan_install_cb, out);
}

static void
pause_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    (void)ms;
#endif
}

static int
looks_playable_exec(const char *rel)
{
    return rel && rel[0] && !is_installer_kit_rel(rel)
        && !is_junk_exec(path_basename(rel));
}

static int
count_files_cb(const char *rel, const char *abs, void *ud)
{
    size_t *n = ud;

    (void)rel;
    (void)abs;
    (*n)++;
    return 0;
}

static int
dir_file_count(const char *dir, size_t *out)
{
    size_t n = 0;

    if (!dir || !dir[0] || !vapor_plat_is_dir(dir)) {
        if (out) {
            *out = 0;
        }
        return 0;
    }
    if (vapor_plat_walk_files(dir, count_files_cb, &n) != 0) {
        return -1;
    }
    if (out) {
        *out = n;
    }
    return 0;
}

static int
dir_is_empty(const char *dir)
{
    size_t n = 0;

    if (!dir || !dir[0] || !vapor_plat_is_dir(dir)) {
        return 1;
    }
    if (dir_file_count(dir, &n) != 0) {
        return 0;
    }
    return n == 0;
}

/* A finished game tree, not the installer kit and not a stub the wizard
 * dropped before copying data. Exit codes from setup.exe are often 0 while
 * msiexec is still copying, so this is the real completion check. */
static int
dest_is_complete(const char *dir, const char *slug, char *exec_rel,
                 size_t exec_n)
{
    install_scan scan;
    uint64_t     bytes = 0;
    size_t       nfiles = 0;

    if (!dir || !dir[0] || !vapor_plat_is_dir(dir)) {
        return 0;
    }
    if (scan_tree_execs(dir, slug, &scan) != 0
        || !looks_playable_exec(scan.win_exec)) {
        return 0;
    }
    if (dir_file_count(dir, &nfiles) != 0) {
        return 0;
    }
    vapor_plat_dir_size(dir, &bytes);
    if (nfiles < 3) {
        return 0;
    }
    if (bytes < (1024ull * 1024ull) && nfiles < 8) {
        return 0;
    }
    if (exec_rel && exec_n) {
        snprintf(exec_rel, exec_n, "%s", scan.win_exec);
    }
    return 1;
}

static int
wait_install_dest(const char *requested, const char *name, const char *id,
                  const char *slug, char *play_dir, size_t play_n,
                  char *exec_rel, size_t exec_n, const setup_hooks *hooks)
{
    uint64_t last_bytes = (uint64_t)-1;
    int      tries, stable = 0;
    char     product[VAPOR_PATH_MAX];
    char     watch[VAPOR_PATH_MAX];
    char     exec_tmp[VAPOR_PATH_MAX];

    for (tries = 0; tries < 90; tries++) {
        uint64_t bytes = 0;
        size_t   nfiles = 0;
        int      complete = 0;

        if (tries > 0) {
            pause_ms(2000);
        }
        watch[0] = '\0';
        exec_tmp[0] = '\0';
        product[0] = '\0';

        if (dest_is_complete(requested, slug, exec_tmp, sizeof(exec_tmp))) {
            snprintf(watch, sizeof(watch), "%s", requested);
            complete = 1;
        } else if (vapor_plat_find_product_dir(name, id, product, sizeof(product))
                       == 0
                   && dest_is_complete(product, slug, exec_tmp,
                                       sizeof(exec_tmp))) {
            snprintf(watch, sizeof(watch), "%s", product);
            complete = 1;
        } else if (requested && requested[0] && vapor_plat_is_dir(requested)
                   && !dir_is_empty(requested)) {
            snprintf(watch, sizeof(watch), "%s", requested);
        } else if (vapor_plat_guess_product_dir(name, id, product,
                                                sizeof(product))
                   == 0) {
            snprintf(watch, sizeof(watch), "%s", product);
        }

        if (!watch[0]) {
            if (tries >= 8) {
                return 0;
            }
            if (hooks && hooks->cb
                && hooks->cb(hooks->ud, (uint64_t)tries + 1, 90) != 0) {
                return -1;
            }
            continue;
        }

        vapor_plat_dir_size(watch, &bytes);
        dir_file_count(watch, &nfiles);
        if (hooks && hooks->on_status) {
            char msg[256];

            snprintf(msg, sizeof(msg), "waiting on installer (%zu file(s))...",
                     nfiles);
            hooks->on_status(hooks->on_status_ud, msg);
        }
        if (hooks && hooks->cb
            && hooks->cb(hooks->ud, (uint64_t)tries + 1, 90) != 0) {
            return -1;
        }
        if (!complete) {
            printf("  waiting on %s (%zu file(s))\n", watch, nfiles);
            last_bytes = bytes;
            stable = 0;
            continue;
        }
        if (bytes == last_bytes) {
            stable++;
        } else {
            if (tries > 0 && last_bytes != (uint64_t)-1) {
                printf("  %s still copying (%zu file(s))\n", watch, nfiles);
            }
            last_bytes = bytes;
            stable = 0;
        }
        /* Two quiet samples (~4s of no growth) after the tree looks finished. */
        if (stable >= 2) {
            snprintf(play_dir, play_n, "%s", watch);
            vapor_plat_native_path(play_dir);
            if (exec_rel && exec_n) {
                snprintf(exec_rel, exec_n, "%s", exec_tmp);
            }
            return 1;
        }
    }
    return 0;
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
    int                 needs_setup = 0;
    int                 cached = 0;
    install_scan        content;
    inst_progress       ip;

    if (!opts) {
        memset(&defaults, 0, sizeof(defaults));
        opts = &defaults;
    }

    memset(&ip, 0, sizeof(ip));
    ip.cb = cb;
    ip.ud = ud;
    ip.on_status = opts->on_status;
    ip.on_status_ud = opts->on_status_ud;
    ip.last_status_pct = -1;

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
        if (progress_enter(&ip, INST_PH_DOWNLOAD, "using cached download...")
            != 0) {
            vapor_manifest_free(&m);
            return -1;
        }
        if (progress_finish_phase(&ip) != 0) {
            vapor_manifest_free(&m);
            return -1;
        }
        if (progress_enter(&ip, INST_PH_VERIFY, "verifying...") != 0) {
            vapor_manifest_free(&m);
            return -1;
        }
        if (hash_file(vc, zip_path, actual_sha, &ip) == 0
            && vapor_str_eq_ci(actual_sha, m.package.sha256)) {
            cached = 1;
            goto verified;
        }
        if (progress_cancelled(&ip, vc)) {
            vapor_manifest_free(&m);
            return -1;
        }
    }

    if (progress_enter(&ip, INST_PH_DOWNLOAD, "downloading...") != 0) {
        vapor_manifest_free(&m);
        return -1;
    }
    snprintf(dl_path, sizeof(dl_path), "%s/%s/%s", VAPOR_EP_DOWNLOAD, m.id,
             m.version);
    if (vapor_http_download(vc, dl_path, zip_path, on_dl_progress, &ip) != 0) {
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
    if (progress_enter(&ip, INST_PH_VERIFY, "verifying...") != 0) {
        vapor_manifest_free(&m);
        return -1;
    }
    if (hash_file(vc, zip_path, actual_sha, &ip) != 0) {
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
    if (!cached && progress_finish_phase(&ip) != 0) {
        vapor_manifest_free(&m);
        return -1;
    }
    if (opts->verify_only) {
        printf("%s %s: archive verified (%s)\n", m.id, m.version,
               m.package.sha256);
        (void)progress_complete(&ip);
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

    if (progress_enter(&ip, INST_PH_EXTRACT, "extracting...") != 0) {
        vapor_plat_remove_tree(install_dir);
        vapor_manifest_free(&m);
        return -1;
    }
    if (!m.package.format || strcmp(m.package.format, "zip") == 0) {
        if (extract_archive(vc, zip_path, install_dir, m.package.strip_prefix,
                            &nfiles, &ip)
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
        if (copy_file(zip_path, dest, &ip) != 0) {
            if (progress_cancelled(&ip, vc)) {
                vapor_plat_remove_tree(install_dir);
                vapor_manifest_free(&m);
                return -1;
            }
            vapor_client_set_error(vc, "cannot copy %s into the install directory",
                                   m.package.file);
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
        nfiles = 1;
    }

    memset(&content, 0, sizeof(content));
    if (discover_install_content(vc, install_dir, &m, &content) != 0) {
        vapor_plat_remove_tree(install_dir);
        vapor_manifest_free(&m);
        return -1;
    }
    if (progress_enter(&ip, INST_PH_POST, "unpacking extra payloads...") != 0) {
        vapor_plat_remove_tree(install_dir);
        vapor_manifest_free(&m);
        return -1;
    }
    if (content.niso > 0) {
        int i;
        int any_unpacked = 0;
        uint64_t post_steps = (uint64_t)content.niso + 1;

        for (i = 0; i < content.niso; i++) {
            char              iso_path[VAPOR_PATH_MAX];
            char              iso_err[256];
            iso_progress_wrap wrap;
            char              msg[192];

            if (join(iso_path, sizeof(iso_path), install_dir, content.iso_rel[i])
                != 0) {
                vapor_client_set_error(vc, "install path is too long");
                vapor_plat_remove_tree(install_dir);
                vapor_manifest_free(&m);
                return -1;
            }
            vapor_plat_native_path(iso_path);
            snprintf(msg, sizeof(msg), "extracting disc image %s (%d/%d)",
                     content.iso_rel[i], i + 1, content.niso);
            progress_status(&ip, msg);
            printf("extracting disc image %s (%d/%d)\n", content.iso_rel[i],
                   i + 1, content.niso);
            wrap.p = &ip;
            wrap.span_i = (uint64_t)i;
            wrap.span_n = post_steps;
            if (vapor_iso_extract_progress(iso_path, install_dir, iso_err,
                                           sizeof(iso_err), on_iso_progress,
                                           &wrap)
                != 0) {
                if (progress_cancelled(&ip, vc)) {
                    vapor_plat_remove_tree(install_dir);
                    vapor_manifest_free(&m);
                    return -1;
                }
                printf("  disc unpack failed (%s); leaving the image in place\n",
                       iso_err);
            } else {
                remove(iso_path);
                any_unpacked = 1;
            }
        }
        if (any_unpacked) {
            unpacked_iso = 1;
            if (discover_install_content(vc, install_dir, &m, &content) != 0) {
                vapor_plat_remove_tree(install_dir);
                vapor_manifest_free(&m);
                return -1;
            }
        }
        if (progress_report_span(&ip, post_steps - 1, post_steps, 0, 1) != 0) {
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
    }
    has_iso = content.niso > 0;
    {
        wise_scan ws;

        memset(&ws, 0, sizeof(ws));
        ws.root = install_dir;
        progress_status(&ip, "unpacking bundled installers...");
        vapor_plat_walk_files(install_dir, wise_install_cb, &ws);
        vapor_disc_finish_install(install_dir);
        if (ws.unpacked) {
            if (discover_install_content(vc, install_dir, &m, &content) != 0) {
                vapor_plat_remove_tree(install_dir);
                vapor_manifest_free(&m);
                return -1;
            }
            has_iso = content.niso > 0;
        }
    }
    if (progress_finish_phase(&ip) != 0) {
        vapor_plat_remove_tree(install_dir);
        vapor_manifest_free(&m);
        return -1;
    }
    target = vapor_manifest_pick_target(&m, vapor_host_platform(),
                                        vapor_host_arch());
    if (target
        && (is_installer_kit_rel(target->exec)
            || is_junk_exec(path_basename(target->exec)))) {
        target = NULL;
    }
    needs_setup = (content.has_installer || content.niso > 0) && !target
                  && !content.win_exec[0] && !content.lin_exec[0];
    if (strcmp(vapor_host_platform(), "windows") != 0 && needs_setup) {
        /* Linux can still store the kit; it cannot run the installer. */
        needs_setup = 1;
    }
    if (target) {
        if (apply_exec_bits(vc, install_dir, target, &nexec) != 0) {
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
    } else if (!has_iso && !needs_setup && !content.has_installer) {
        vapor_client_set_error(vc,
                               "%s %s has no %s/%s build and no disc image",
                               m.id, m.version, vapor_host_platform(),
                               vapor_host_arch());
        vapor_plat_remove_tree(install_dir);
        vapor_manifest_free(&m);
        return -1;
    }
    if (needs_setup) {
        size_t i;

        for (i = 0; i < m.ntargets; i++) {
            if (m.targets[i].exec && is_installer_kit_rel(m.targets[i].exec)) {
                free(m.targets[i].exec);
                m.targets[i].exec = vapor_strdup("");
                if (!m.targets[i].exec) {
                    vapor_client_set_error(vc, "out of memory");
                    vapor_plat_remove_tree(install_dir);
                    vapor_manifest_free(&m);
                    return -1;
                }
            }
        }
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
    snprintf(rec.payload_dir, sizeof(rec.payload_dir), "%s", install_dir);
    rec.installed_at = vapor_now_unix();
    rec.size_on_disk = on_disk;
    rec.setup_pending = needs_setup ? 1 : 0;

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
            printf("  disc image . %d image(s) left in place\n", content.niso);
        }
        if (unpacked_iso) {
            printf("  disc image . unpacked into the install directory\n");
        }
        if (needs_setup) {
            printf("  setup ...... completing the Windows installer\n");
        } else if (!target && has_iso) {
            printf("  launch ..... disc image (no native executable)\n");
        }
        if (have_existing && strcmp(existing.version, m.version) != 0) {
            printf("  upgraded ... from %s\n", existing.version);
        }
    }
    vapor_manifest_free(&m);
    if (needs_setup) {
        int src;

        printf("starting the installer for %s\n", rec.game_id);
        if (progress_enter(&ip, INST_PH_SETUP,
                           "running the Windows installer...")
            != 0) {
            return -1;
        }
        {
            setup_hooks setup;

            memset(&setup, 0, sizeof(setup));
            setup.cb = on_setup_progress;
            setup.ud = &ip;
            setup.on_status = on_setup_status;
            setup.on_status_ud = &ip;
            src = setup_game_impl(vc, rec.game_id, &setup);
        }
        if (src < 0) {
            return -1;
        }
        if (src > 0) {
            printf("  setup did not finish; use Setup in the library when you "
                   "are ready\n");
            if (strcmp(vapor_host_platform(), "windows") == 0) {
                (void)progress_complete(&ip);
                return 1;
            }
        }
    } else if (progress_enter(&ip, INST_PH_SETUP, "finishing...") != 0
               || progress_finish_phase(&ip) != 0) {
        return -1;
    }
    (void)progress_complete(&ip);
    rc = 0;
    return rc;
}

static int
setup_game_impl(vapor_client *vc, const char *game_id, const setup_hooks *hooks)
{
    vapor_install  rec;
    vapor_manifest m;
    install_scan   scan;
    char           payload[VAPOR_PATH_MAX];
    char           setup_path[VAPOR_PATH_MAX];
    char           setup_cwd[VAPOR_PATH_MAX];
    char           play_dir[VAPOR_PATH_MAX];
    char           exec_rel[VAPOR_PATH_MAX];
    int            exit_code = 0;
    int            found = 0;

    if (vapor_db_get_install(vc, game_id, &rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed; run \"vapor install %s\"",
                               game_id, game_id);
        return -1;
    }
    if (!rec.setup_pending) {
        return 0;
    }
    snprintf(payload, sizeof(payload), "%s",
             rec.payload_dir[0] ? rec.payload_dir : rec.install_dir);
    if (!payload[0] && install_dir_for(vc, game_id, payload, sizeof(payload)) != 0) {
        return -1;
    }

    if (strcmp(vapor_host_platform(), "windows") != 0) {
        vapor_client_set_error(vc,
                               "%s is a Windows installer kit; finish setup on "
                               "a Windows client",
                               rec.name[0] ? rec.name : game_id);
        return 1;
    }

    if (vapor_read_local_manifest(vc, game_id, &m) != 0) {
        return -1;
    }
    if (scan_tree_execs(payload, m.id, &scan) != 0) {
        vapor_client_set_error(vc, "cannot inspect installer files in %s",
                               payload);
        vapor_manifest_free(&m);
        return -1;
    }

    if (scan.setup_rel[0]) {
        if (join(setup_path, sizeof(setup_path), payload, scan.setup_rel) != 0) {
            vapor_client_set_error(vc, "installer path is too long");
            vapor_manifest_free(&m);
            return -1;
        }
    } else if (scan.niso > 0) {
        if (join(setup_path, sizeof(setup_path), payload, scan.iso_rel[0]) != 0) {
            vapor_client_set_error(vc, "installer path is too long");
            vapor_manifest_free(&m);
            return -1;
        }
    } else {
        vapor_client_set_error(vc, "%s has no setup.exe, .msi, or disc image",
                               game_id);
        vapor_manifest_free(&m);
        return -1;
    }
    vapor_plat_native_path(setup_path);
    vapor_plat_native_path(payload);
    prefer_dir_bootstrapper(setup_path, sizeof(setup_path));
    path_dirname(setup_path, setup_cwd, sizeof(setup_cwd));
    vapor_plat_native_path(setup_cwd);
    {
        vapor_inst_kind kind;
        char            dest[VAPOR_PATH_MAX];
        char            silent[2048];
        int             silent_rc;

        kind = vapor_inst_detect(setup_path, setup_cwd);
        if (join(dest, sizeof(dest), payload, "installed") != 0) {
            vapor_client_set_error(vc, "install path is too long");
            vapor_manifest_free(&m);
            return -1;
        }
        vapor_plat_native_path(dest);
        if (vapor_plat_mkdirs(dest) != 0) {
            vapor_client_set_error(vc, "cannot create %s", dest);
            vapor_manifest_free(&m);
            return -1;
        }

        silent_rc = vapor_inst_silent_params(kind, setup_path, dest, silent,
                                             sizeof(silent));
        printf("installer %s (%s)\n", setup_path, vapor_inst_kind_name(kind));
        printf("tracking destination %s (and the Uninstall / Program Files "
               "folder the installer creates)\n",
               dest);
        if (silent_rc < 0) {
            vapor_client_set_error(vc, "installer arguments are too long");
            vapor_manifest_free(&m);
            return -1;
        }
        if (silent_rc == 0) {
            printf("unattended install to %s\n", dest);
            if (vapor_plat_run_ui(setup_path, setup_cwd, silent, &exit_code)
                != 0) {
                vapor_client_set_error(vc, "could not start %s", setup_path);
                vapor_manifest_free(&m);
                return -1;
            }
            printf("installer process exited (%d); checking %s\n", exit_code,
                   dest);
            found = wait_install_dest(dest, rec.name, rec.game_id, m.id,
                                      play_dir, sizeof(play_dir), exec_rel,
                                      sizeof(exec_rel), hooks);
            if (found < 0) {
                vapor_client_set_error(vc, "install cancelled");
                vapor_manifest_free(&m);
                return -1;
            }
            if (!found) {
                printf("unattended install did not finish; opening the wizard\n");
                if (vapor_plat_run_ui(setup_path, setup_cwd, NULL, &exit_code)
                    != 0) {
                    vapor_client_set_error(vc, "could not start %s", setup_path);
                    vapor_manifest_free(&m);
                    return -1;
                }
                printf("installer process exited (%d); checking destination\n",
                       exit_code);
            }
        } else {
            printf("running installer %s\n", setup_path);
            if (kind == VAPOR_INST_ISHIELD) {
                printf("InstallShield needs the Setup window; silent mode "
                       "hangs on this family, so complete the wizard\n");
            }
            if (vapor_plat_run_ui(setup_path, setup_cwd, NULL, &exit_code)
                != 0) {
                vapor_client_set_error(vc, "could not start %s", setup_path);
                vapor_manifest_free(&m);
                return -1;
            }
            printf("installer process exited (%d); checking destination\n",
                   exit_code);
        }
        if (!found) {
            found = wait_install_dest(dest, rec.name, rec.game_id, m.id,
                                      play_dir, sizeof(play_dir), exec_rel,
                                      sizeof(exec_rel), hooks);
            if (found < 0) {
                vapor_client_set_error(vc, "install cancelled");
                vapor_manifest_free(&m);
                return -1;
            }
        }
        if (!found && dir_is_empty(dest)) {
            (void)vapor_plat_remove_tree(dest);
        }
    }

    if (!found) {
        vapor_client_set_error(vc,
                               "the installer for %s exited (%d) but did not "
                               "finish copying the game; run Setup again after "
                               "the destination folder is complete",
                               rec.name[0] ? rec.name : game_id, exit_code);
        printf("installer did not complete (exit %d)\n", exit_code);
        vapor_manifest_free(&m);
        return 1;
    }

    if (set_windows_exec(&m, exec_rel) != 0) {
        vapor_client_set_error(vc, "out of memory");
        vapor_manifest_free(&m);
        return -1;
    }
    if (write_local_manifest(vc, payload, &m) != 0) {
        vapor_manifest_free(&m);
        return -1;
    }
    snprintf(rec.install_dir, sizeof(rec.install_dir), "%s", play_dir);
    snprintf(rec.payload_dir, sizeof(rec.payload_dir), "%s", payload);
    rec.setup_pending = 0;
    vapor_plat_dir_size(play_dir, &rec.size_on_disk);
    rec.installed_at = vapor_now_unix();
    if (vapor_db_record_install(vc, &rec) != 0) {
        vapor_manifest_free(&m);
        return -1;
    }
    printf("tracked %s at %s (%s)\n", rec.name[0] ? rec.name : game_id,
           rec.install_dir, exec_rel);
    vapor_manifest_free(&m);
    if (hooks && hooks->cb) {
        (void)hooks->cb(hooks->ud, 90, 90);
    }
    return 0;
}

int
vapor_setup_game(vapor_client *vc, const char *game_id)
{
    return setup_game_impl(vc, game_id, NULL);
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

    /* Always drop Vapor's downloaded kit. A Windows installer may have copied
     * the game into Program Files; that tree is left for the OS uninstaller. */
    if (vapor_plat_exists(install_dir)
        && vapor_plat_remove_tree(install_dir) != 0) {
        vapor_client_set_error(vc, "cannot remove %s", install_dir);
        return -1;
    }
    if (rec.payload_dir[0] && strcmp(rec.payload_dir, install_dir) != 0
        && vapor_str_has_prefix(rec.payload_dir, vc->cfg.library_dir)
        && vapor_plat_exists(rec.payload_dir)
        && vapor_plat_remove_tree(rec.payload_dir) != 0) {
        vapor_client_set_error(vc, "cannot remove %s", rec.payload_dir);
        return -1;
    }
    if (rec.install_dir[0] && strcmp(rec.install_dir, install_dir) != 0
        && vapor_str_has_prefix(rec.install_dir, vc->cfg.library_dir)
        && vapor_plat_exists(rec.install_dir)
        && vapor_plat_remove_tree(rec.install_dir) != 0) {
        vapor_client_set_error(vc, "cannot remove %s", rec.install_dir);
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
    if (rec.setup_pending) {
        printf("  setup pending (Windows installer has not been tracked yet)\n");
    } else if (!t) {
        if (install_tree_has_iso(install_dir)) {
            printf("  disc image (no launch target)\n");
        } else {
            printf("  no %s/%s target in the manifest\n", vapor_host_platform(),
                   vapor_host_arch());
            problems++;
        }
    } else if (join(exec_path, sizeof(exec_path), rec.install_dir, t->exec)
               != 0) {
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
