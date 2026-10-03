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

#include "cJSON.h"
#include "installer.h"
#include "json.h"
#include "miniz.h"
#include "net.h"
#include "platform.h"
#include "vapor/buf.h"
#include "vapor/iso9660.h"
#include "vapor/sha256.h"
#include "vapor/saves.h"
#include "vapor/util.h"
#include "vapor/wise.h"

/* Kept inside the install directory so uninstall is a single tree removal and
 * launching works with the server unreachable. */
#define VAPOR_META_DIR          ".vapor"
#define VAPOR_META_FILE         ".vapor/manifest.json"
#define VAPOR_META_INSTALL_FILE ".vapor/install.json"

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
    uint64_t          last_done;
    uint64_t          last_total;
} inst_progress;

typedef struct {
    vapor_progress_fn cb;
    void             *ud;
    vapor_status_fn   on_status;
    void             *on_status_ud;
    int             (*poll_cancel)(void *ud);
    void             *poll_ud;
} setup_hooks;

static int setup_game_impl(vapor_client *vc, const char *game_id,
                           const setup_hooks *hooks);
static int uninstall_game_impl(vapor_client *vc, const char *game_id,
                               vapor_progress_fn cb, void *ud);
static int mkdirs_for_file(const char *path);
static int find_local_uninstaller(const char *dir, char *out, size_t outsz);
static int persist_install(vapor_client *vc, const vapor_install *rec);
static void capture_uninstall_recipe(vapor_install *rec, const char *extra_dir);
static void merge_install_sidecar(vapor_install *rec);

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
    p->last_done = overall;
    p->last_total = INSTALL_PROGRESS_MAX;
    if (p->cb && p->cb(p->ud, overall, INSTALL_PROGRESS_MAX) != 0) {
        p->aborted = 1;
        return -1;
    }
    return 0;
}

static int
progress_poll_cancel(void *ud)
{
    inst_progress *p = (inst_progress *)ud;

    if (!p) {
        return 0;
    }
    if (p->aborted) {
        return 1;
    }
    if (!p->cb) {
        return 0;
    }
    if (p->cb(p->ud, p->last_done, p->last_total ? p->last_total : 1) != 0) {
        p->aborted = 1;
        return 1;
    }
    return 0;
}

static int
hooks_cancel(void *ud)
{
    const setup_hooks *h = (const setup_hooks *)ud;

    if (!h || !h->poll_cancel) {
        return 0;
    }
    return h->poll_cancel(h->poll_ud) != 0;
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

static int
mount_dir_for(vapor_client *vc, const char *game_id, char *out, size_t outsz)
{
    int n = snprintf(out, outsz, "%s/%s/mnt/%s", vc->cfg.library_dir,
                     VAPOR_META_DIR, game_id);

    if (n < 0 || (size_t)n >= outsz) {
        vapor_client_set_error(vc, "library path is too long");
        return -1;
    }
    vapor_plat_native_path(out);
    return 0;
}

static int
rel_file_exists(const char *root, const char *rel)
{
    char path[VAPOR_PATH_MAX];

    if (!root || !root[0] || !rel || !rel[0]) {
        return 0;
    }
    if (join(path, sizeof(path), root, rel) != 0) {
        return 0;
    }
    vapor_plat_native_path(path);
    return vapor_plat_exists(path) && !vapor_plat_is_dir(path);
}

static int
paths_equal(const char *a, const char *b)
{
    char aa[VAPOR_PATH_MAX];
    char bb[VAPOR_PATH_MAX];

    snprintf(aa, sizeof(aa), "%s", a ? a : "");
    snprintf(bb, sizeof(bb), "%s", b ? b : "");
    vapor_plat_native_path(aa);
    vapor_plat_native_path(bb);
    return vapor_str_eq_ci(aa, bb);
}

typedef struct {
    const char *dst_root;
    int         failed;
    size_t      nfiles;
} copy_tree_ctx;

static int
copy_tree_cb(const char *rel, const char *abs, void *ud)
{
    copy_tree_ctx *c = (copy_tree_ctx *)ud;
    char           dest[VAPOR_PATH_MAX];

    if (join(dest, sizeof(dest), c->dst_root, rel) != 0) {
        c->failed = 1;
        return 1;
    }
    vapor_plat_native_path(dest);
    if (mkdirs_for_file(dest) != 0 || copy_file(abs, dest, NULL) != 0) {
        c->failed = 1;
        return 1;
    }
    c->nfiles++;
    return 0;
}

static int
copy_tree(const char *src, const char *dst, size_t *out_files)
{
    copy_tree_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.dst_root = dst;
    if (vapor_plat_walk_files(src, copy_tree_cb, &ctx) != 0 || ctx.failed) {
        return -1;
    }
    if (out_files) {
        *out_files += ctx.nfiles;
    }
    return 0;
}

static int
relocate_file(const char *src, const char *dst)
{
    if (mkdirs_for_file(dst) != 0) {
        return -1;
    }
    if (rename(src, dst) == 0) {
        return 0;
    }
    if (copy_file(src, dst, NULL) != 0) {
        return -1;
    }
    remove(src);
    return 0;
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

/* Retail Doom 3 is launched through the shared dhewm3 runtime, not as a
 * native exe. The tree stays in the library; the port lives under
 * {data_dir}/runtimes and must not be removed with the game. */
static int
looks_dhewm3_payload(const char *dir, const char *id, const char *exec)
{
    char        pak[VAPOR_PATH_MAX], gamepak[VAPOR_PATH_MAX];
    char        stem[VAPOR_ID_MAX + 1];
    const char *base;

    if (!dir || join(pak, sizeof(pak), dir, "base/pak000.pk4") != 0
        || !vapor_plat_exists(pak)) {
        return 0;
    }
    if (join(gamepak, sizeof(gamepak), dir, "base/game00.pk4") != 0
        || !vapor_plat_exists(gamepak)) {
        return 0;
    }
    if (id && (vapor_slug_match(id, "doom3") || vapor_slug_match(id, "d3xp"))) {
        return 1;
    }
    base = exec ? strrchr(exec, '/') : NULL;
    base = base ? base + 1 : (exec ? exec : "");
    if (base[0] && vapor_id_slug_stem(base, stem, sizeof(stem)) == 0
        && (vapor_slug_match(stem, "doom3") || vapor_slug_match(stem, "d3xp"))) {
        return 1;
    }
    return 0;
}

static void
classify_install_kind(vapor_install *rec, int needs_setup, const vapor_target *t,
                      const char *dir)
{
    if (needs_setup) {
        snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
                 VAPOR_INSTALL_KIND_OS_PRODUCT);
    } else if ((t && t->runtime && t->runtime[0]
                && strcmp(t->runtime, "native") != 0)
               || looks_dhewm3_payload(dir, rec->game_id,
                                       t && t->exec ? t->exec : "")) {
        snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
                 VAPOR_INSTALL_KIND_RUNTIME);
    } else {
        snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
                 VAPOR_INSTALL_KIND_PORTABLE);
    }
}

static int
write_install_sidecar(const vapor_install *rec)
{
    const char *root;
    char        meta_dir[VAPOR_PATH_MAX], file[VAPOR_PATH_MAX];
    cJSON      *obj;
    char       *json;
    FILE       *f;

    root = rec->payload_dir[0] ? rec->payload_dir : rec->install_dir;
    if (!root || !root[0]) {
        return 0;
    }
    if (join(meta_dir, sizeof(meta_dir), root, VAPOR_META_DIR) != 0
        || join(file, sizeof(file), root, VAPOR_META_INSTALL_FILE) != 0) {
        return -1;
    }
    if (vapor_plat_mkdirs(meta_dir) != 0) {
        return -1;
    }
    obj = cJSON_CreateObject();
    if (!obj) {
        return -1;
    }
    cJSON_AddStringToObject(obj, "install_kind", rec->install_kind);
    cJSON_AddStringToObject(obj, "uninstall_exe", rec->uninstall_exe);
    cJSON_AddStringToObject(obj, "uninstall_params", rec->uninstall_params);
    cJSON_AddStringToObject(obj, "product_dir", rec->product_dir);
    cJSON_AddStringToObject(obj, "setup_state", rec->setup_state);
    cJSON_AddStringToObject(obj, "setup_rel", rec->setup_rel);
    cJSON_AddStringToObject(obj, "profile_launch", rec->profile_launch);
    cJSON_AddStringToObject(obj, "profile_uninstall", rec->profile_uninstall);
    json = cJSON_Print(obj);
    cJSON_Delete(obj);
    if (!json) {
        return -1;
    }
    f = fopen(file, "w");
    if (!f) {
        free(json);
        return -1;
    }
    fputs(json, f);
    fclose(f);
    free(json);
    return 0;
}

static int
persist_install(vapor_client *vc, const vapor_install *rec)
{
    if (vapor_db_record_install(vc, rec) != 0) {
        return -1;
    }
    if (write_install_sidecar(rec) != 0) {
        printf("  warning: could not write %s/%s\n",
               rec->payload_dir[0] ? rec->payload_dir : rec->install_dir,
               VAPOR_META_INSTALL_FILE);
    }
    return 0;
}

static void
apply_install_sidecar_file(vapor_install *rec, const char *dir)
{
    char   file[VAPOR_PATH_MAX];
    char  *json;
    size_t len = 0;
    cJSON *obj;

    if (!dir || !dir[0] || join(file, sizeof(file), dir, VAPOR_META_INSTALL_FILE) != 0) {
        return;
    }
    vapor_plat_native_path(file);
    json = vapor_read_file(file, &len);
    if (!json) {
        return;
    }
    obj = cJSON_ParseWithLength(json, len);
    free(json);
    if (!obj) {
        return;
    }
    if (!rec->install_kind[0]) {
        vapor_json_copy(rec->install_kind, sizeof(rec->install_kind), obj,
                        "install_kind", "");
    }
    if (!rec->uninstall_exe[0]) {
        vapor_json_copy(rec->uninstall_exe, sizeof(rec->uninstall_exe), obj,
                        "uninstall_exe", "");
    }
    if (!rec->uninstall_params[0]) {
        vapor_json_copy(rec->uninstall_params, sizeof(rec->uninstall_params),
                        obj, "uninstall_params", "");
    }
    if (!rec->product_dir[0]) {
        vapor_json_copy(rec->product_dir, sizeof(rec->product_dir), obj,
                        "product_dir", "");
    }
    if (!rec->setup_state[0]) {
        vapor_json_copy(rec->setup_state, sizeof(rec->setup_state), obj,
                        "setup_state", "");
    }
    if (!rec->setup_rel[0]) {
        vapor_json_copy(rec->setup_rel, sizeof(rec->setup_rel), obj, "setup_rel",
                        "");
    }
    if (!rec->profile_launch[0]) {
        vapor_json_copy(rec->profile_launch, sizeof(rec->profile_launch), obj,
                        "profile_launch", "");
    }
    if (!rec->profile_uninstall[0]) {
        vapor_json_copy(rec->profile_uninstall, sizeof(rec->profile_uninstall),
                        obj, "profile_uninstall", "");
    }
    cJSON_Delete(obj);
}

static void
assign_setup_state(vapor_install *rec, const char *state)
{
    snprintf(rec->setup_state, sizeof(rec->setup_state), "%s", state ? state : "");
    if (!rec->setup_state[0]
        || strcmp(rec->setup_state, VAPOR_SETUP_COMPLETED) == 0) {
        rec->setup_pending = 0;
    } else {
        rec->setup_pending = 1;
    }
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

/* .bin is also a Linux binary suffix. A disc image has an ISO 9660
 * filesystem; without a path, refuse to treat the name as a program. */
static int
looks_disc_image(const char *name, const char *abs)
{
    if (!name) {
        return 0;
    }
    if (vapor_str_ends_with_ci(name, ".iso")
        || vapor_str_ends_with_ci(name, ".img")) {
        return 1;
    }
    if (!vapor_str_ends_with_ci(name, ".bin")) {
        return 0;
    }
    if (!abs) {
        return 1;
    }
    return vapor_iso_is_image(abs);
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
    if (vapor_str_has_prefix(base, "unins")
        && vapor_str_ends_with_ci(base, ".exe")) {
        return 1;
    }
    if (vapor_str_eq_ci(base, "upd.exe")
        || vapor_str_ends_with_ci(base, "up.exe")
        || vapor_str_ends_with_ci(base, "update.exe")) {
        return 1;
    }
    if (vapor_str_ends_with_ci(base, ".exe")) {
        const char *p;

        for (p = base; *p; p++) {
            if ((p[0] == 'p' || p[0] == 'P') && (p[1] == 'a' || p[1] == 'A')
                && (p[2] == 't' || p[2] == 'T') && (p[3] == 'c' || p[3] == 'C')
                && (p[4] == 'h' || p[4] == 'H')) {
                return 1;
            }
        }
    }
    return 0;
}

static int
looks_windows_exec(const char *name)
{
    const char *base = name ? path_basename(name) : "";

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

    if (looks_disc_image(base, abs)) {
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

static const char *
windows_pe_arch(const char *dir, const char *exec)
{
    char        full[VAPOR_PATH_MAX];
    const char *arch;

    if (!dir || !dir[0] || !exec || !exec[0]) {
        return "x86_64";
    }
    if (join(full, sizeof(full), dir, exec) != 0) {
        return "x86_64";
    }
    arch = vapor_pe_arch_file(full);
    return arch ? arch : "x86_64";
}

static int
add_discovered_target(vapor_manifest *m, const char *platform, const char *exec,
                      const char *arch)
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
    t->arch = vapor_strdup(arch && arch[0] ? arch : "x86_64");
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
            && add_discovered_target(m, "windows", scan.win_exec,
                                     windows_pe_arch(install_dir, scan.win_exec))
                   != 0) {
            vapor_client_set_error(vc, "out of memory");
            return -1;
        }
        if (scan.lin_exec[0]
            && add_discovered_target(m, "linux", scan.lin_exec, "x86_64") != 0) {
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

    if (looks_disc_image(path_basename(rel), abs)) {
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
set_windows_exec(vapor_manifest *m, const char *dir, const char *exec)
{
    size_t      i;
    const char *arch = windows_pe_arch(dir, exec);
    char       *arch_copy;

    for (i = 0; i < m->ntargets; i++) {
        if (m->targets[i].platform
            && strcmp(m->targets[i].platform, "windows") == 0) {
            free(m->targets[i].exec);
            m->targets[i].exec = vapor_strdup(exec);
            arch_copy = vapor_strdup(arch);
            if (!m->targets[i].exec || !arch_copy) {
                free(arch_copy);
                return -1;
            }
            free(m->targets[i].arch);
            m->targets[i].arch = arch_copy;
            return 0;
        }
    }
    return add_discovered_target(m, "windows", exec, arch);
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

/* Read leftover disc images into {library}/.vapor/mnt/{id}. A portable tree is
 * copied into the library folder and the mount is removed. An installer kit
 * stays on the mount so setup can read it from there. 0 on success, -1 on
 * error. A failed extract leaves the image in the mount; cancel removes it. */
static int
extract_iso_at(vapor_client *vc, inst_progress *ip, const char *iso_path,
               const char *mount, int index, int count)
{
    char              iso_err[256];
    iso_progress_wrap wrap;
    char              msg[192];
    const char       *base = path_basename(iso_path);

    snprintf(msg, sizeof(msg), "extracting disc image %s (%d/%d)", base, index,
             count);
    progress_status(ip, msg);
    printf("extracting disc image %s (%d/%d)\n", base, index, count);
    wrap.p = ip;
    wrap.span_i = (uint64_t)(index > 0 ? index - 1 : 0);
    wrap.span_n = (uint64_t)count + 1;
    memset(iso_err, 0, sizeof(iso_err));
    if (vapor_iso_extract_progress(iso_path, mount, iso_err, sizeof(iso_err),
                                   on_iso_progress, &wrap)
        != 0) {
        if (progress_cancelled(ip, vc)) {
            return -1;
        }
        vapor_client_set_error(vc, "disc unpack failed (%s)",
                               iso_err[0] ? iso_err : "unreadable image");
        return -1;
    }
    remove(iso_path);
    return 0;
}

static int
keep_failed_iso(const char *iso_path, const char *mount)
{
    char kept[VAPOR_PATH_MAX];

    if (join(kept, sizeof(kept), mount, path_basename(iso_path)) != 0) {
        return -1;
    }
    vapor_plat_native_path(kept);
    if (paths_equal(iso_path, kept)) {
        return 0;
    }
    return relocate_file(iso_path, kept);
}

static int
scan_is_playable(const vapor_manifest *m, const char *root, const install_scan *s)
{
    const vapor_target *t;

    if (looks_playable_exec(s->win_exec) || looks_playable_exec(s->lin_exec)) {
        return 1;
    }
    if (m->install.launch && rel_file_exists(root, m->install.launch)
        && looks_playable_exec(m->install.launch)) {
        return 1;
    }
    t = vapor_manifest_pick_target(m, vapor_host_platform(), vapor_host_arch());
    if (t && t->exec && looks_playable_exec(t->exec) && rel_file_exists(root, t->exec)) {
        return 1;
    }
    return 0;
}

static int
stage_discs(vapor_client *vc, vapor_manifest *m, inst_progress *ip,
            const char *install_dir, const char *downloaded, int legacy_iso,
            install_scan *content, char *payload_out, size_t payload_sz,
            size_t *nfiles, int *unpacked_iso, int *disc_installer)
{
    char         mount[VAPOR_PATH_MAX];
    install_scan disc;
    wise_scan    ws;
    int          explicit_setup;
    int          playable;
    int          i;
    int          niso = content->niso;

    *disc_installer = 0;
    if (mount_dir_for(vc, m->id, mount, sizeof(mount)) != 0) {
        return -1;
    }
    if (vapor_plat_exists(mount) && vapor_plat_remove_tree(mount) != 0) {
        vapor_client_set_error(vc, "cannot clear %s", mount);
        return -1;
    }
    if (vapor_plat_mkdirs(mount) != 0) {
        vapor_client_set_error(vc, "cannot create %s", mount);
        return -1;
    }

    if (legacy_iso) {
        char iso_path[VAPOR_PATH_MAX];

        if (join(iso_path, sizeof(iso_path), mount, m->package.file) != 0) {
            vapor_client_set_error(vc, "install path is too long");
            vapor_plat_remove_tree(mount);
            return -1;
        }
        vapor_plat_native_path(iso_path);
        if (copy_file(downloaded, iso_path, ip) != 0) {
            if (!progress_cancelled(ip, vc)) {
                vapor_client_set_error(vc, "cannot copy %s into the disc folder",
                                       m->package.file);
            }
            vapor_plat_remove_tree(mount);
            return -1;
        }
        if (extract_iso_at(vc, ip, iso_path, mount, 1, 1) != 0) {
            if (progress_cancelled(ip, vc)) {
                vapor_plat_remove_tree(mount);
            }
            return -1;
        }
    }

    for (i = 0; i < niso; i++) {
        char iso_path[VAPOR_PATH_MAX];

        if (join(iso_path, sizeof(iso_path), install_dir, content->iso_rel[i]) != 0) {
            vapor_client_set_error(vc, "install path is too long");
            vapor_plat_remove_tree(mount);
            return -1;
        }
        vapor_plat_native_path(iso_path);
        if (extract_iso_at(vc, ip, iso_path, mount, i + 1, niso) != 0) {
            if (progress_cancelled(ip, vc)) {
                vapor_plat_remove_tree(mount);
                return -1;
            }
            (void)keep_failed_iso(iso_path, mount);
            return -1;
        }
    }

    memset(&ws, 0, sizeof(ws));
    ws.root = mount;
    progress_status(ip, "unpacking bundled installers...");
    vapor_plat_walk_files(mount, wise_install_cb, &ws);
    vapor_disc_finish_install(mount);
    if (discover_install_content(vc, mount, m, &disc) != 0) {
        return -1;
    }

    explicit_setup = m->install.setup && rel_file_exists(mount, m->install.setup);
    playable = scan_is_playable(m, mount, &disc);
    if (explicit_setup || ((disc.has_installer || disc.niso > 0) && !playable)) {
        *disc_installer = 1;
        if ((size_t)snprintf(payload_out, payload_sz, "%s", mount) >= payload_sz) {
            vapor_client_set_error(vc, "install path is too long");
            return -1;
        }
        *content = disc;
        if (explicit_setup) {
            snprintf(content->setup_rel, sizeof(content->setup_rel), "%s",
                     m->install.setup);
            content->has_installer = 1;
        }
        *unpacked_iso = 1;
        return 0;
    }
    if (!playable) {
        vapor_client_set_error(vc,
                               "disc image for %s has no installer and no game "
                               "executable",
                               m->id);
        return -1;
    }
    if (copy_tree(mount, install_dir, nfiles) != 0) {
        vapor_client_set_error(vc, "cannot copy the disc contents into %s",
                               install_dir);
        return -1;
    }
    vapor_plat_remove_tree(mount);
    *unpacked_iso = 1;
    if (discover_install_content(vc, install_dir, m, content) != 0) {
        return -1;
    }
    return 0;
}

/* A windows target with no runtime is native until the file itself is DOS. */
static void
note_dos_runtime(vapor_manifest *m, const char *root)
{
    size_t i;

    if (!m || !root || !root[0]) {
        return;
    }
    for (i = 0; i < m->ntargets; i++) {
        vapor_target *t = &m->targets[i];
        char          path[VAPOR_PATH_MAX];

        if (!t->platform || strcmp(t->platform, "windows") != 0) {
            continue;
        }
        if (t->runtime && t->runtime[0]) {
            continue;
        }
        if (!t->exec || !t->exec[0]) {
            continue;
        }
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, t->exec)
            >= sizeof(path)) {
            continue;
        }
        vapor_plat_native_path(path);
        if (!vapor_file_is_dos_exe(path)) {
            continue;
        }
        t->runtime = vapor_strdup("dosbox");
    }
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
    int                 legacy_iso = 0;
    int                 disc_installer = 0;
    int                 hold_images = 0;
    char                payload_dir[VAPOR_PATH_MAX];
    install_scan        content;
    inst_progress       ip;

    if (!opts) {
        memset(&defaults, 0, sizeof(defaults));
        opts = &defaults;
    }

    memset(&ip, 0, sizeof(ip));
    payload_dir[0] = '\0';
    ip.cb = cb;
    ip.ud = ud;
    ip.on_status = opts->on_status;
    ip.on_status_ud = opts->on_status_ud;
    ip.last_status_pct = -1;

    if (vapor_fetch_manifest(vc, game_id, version, &m) != 0) {
        return -1;
    }

    have_existing = (vapor_db_get_install(vc, m.id, &existing) == 0);
    if (have_existing && vapor_install_is_external(&existing)) {
        vapor_client_set_error(vc,
                               "%s is a local shortcut. Remove it from the "
                               "library before installing a catalog copy",
                               m.id);
        vapor_manifest_free(&m);
        return -1;
    }
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

    /* Only now is the existing install disturbed. A previous attempt may
     * also have left a disc mount; drop it before this install rebuilds it. */
    {
        char stale_mount[VAPOR_PATH_MAX];

        if (mount_dir_for(vc, m.id, stale_mount, sizeof(stale_mount)) == 0
            && vapor_plat_exists(stale_mount)) {
            (void)vapor_plat_remove_tree(stale_mount);
        }
    }
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
    } else if (strcmp(m.package.format, "iso") == 0) {
        /* Staged into the temp mount below, not the library folder.
         * The image is never handed to the shell to mount. */
        legacy_iso = 1;
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
    hold_images = m.install_mode
                  && (strcmp(m.install_mode, "portable") == 0
                      || strcmp(m.install_mode, "keep_disc") == 0);
    if (hold_images && legacy_iso) {
        char dest[VAPOR_PATH_MAX];
        const char *leaf = m.package.file ? m.package.file : "disc.iso";

        if (join(dest, sizeof(dest), install_dir, leaf) != 0
            || copy_file(zip_path, dest, &ip) != 0) {
            vapor_client_set_error(vc, "cannot keep the disc image for %s", m.id);
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
        nfiles = 1;
    } else if (legacy_iso || content.niso > 0) {
        if (stage_discs(vc, &m, &ip, install_dir, zip_path, legacy_iso, &content,
                        payload_dir, sizeof(payload_dir), &nfiles, &unpacked_iso,
                        &disc_installer)
            != 0) {
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
    }
    has_iso = !disc_installer && content.niso > 0;
    if (!hold_images && !disc_installer) {
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
    if (disc_installer) {
        needs_setup = 1;
        target = NULL;
        has_iso = 0;
    } else {
        needs_setup = (content.has_installer || content.niso > 0) && !target
                      && !content.win_exec[0] && !content.lin_exec[0];
    }
    if (m.install_mode && strcmp(m.install_mode, "setup") == 0) {
        needs_setup = 1;
    } else if (hold_images) {
        needs_setup = 0;
    }
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
    if (!disc_installer) {
        snprintf(payload_dir, sizeof(payload_dir), "%s", install_dir);
    }
    if (m.install.launch && rel_file_exists(payload_dir, m.install.launch)
        && looks_playable_exec(m.install.launch)
        && strcmp(vapor_host_platform(), "windows") == 0) {
        if (set_windows_exec(&m, payload_dir, m.install.launch) != 0) {
            vapor_client_set_error(vc, "out of memory");
            vapor_plat_remove_tree(install_dir);
            vapor_manifest_free(&m);
            return -1;
        }
    }
    note_dos_runtime(&m, payload_dir[0] ? payload_dir : install_dir);
    if (write_local_manifest(vc, payload_dir, &m) != 0) {
        vapor_plat_remove_tree(install_dir);
        if (disc_installer) {
            vapor_plat_remove_tree(payload_dir);
        }
        vapor_manifest_free(&m);
        return -1;
    }

    vapor_plat_dir_size(disc_installer ? payload_dir : install_dir, &on_disk);

    memset(&rec, 0, sizeof(rec));
    snprintf(rec.game_id, sizeof(rec.game_id), "%s", m.id);
    snprintf(rec.version, sizeof(rec.version), "%s", m.version);
    snprintf(rec.name, sizeof(rec.name), "%s", m.name ? m.name : m.id);
    snprintf(rec.install_dir, sizeof(rec.install_dir), "%s", install_dir);
    snprintf(rec.payload_dir, sizeof(rec.payload_dir), "%s", payload_dir);
    rec.installed_at = vapor_now_unix();
    rec.size_on_disk = on_disk;
    classify_install_kind(&rec, needs_setup, target, install_dir);
    if (m.install.setup && rel_file_exists(payload_dir, m.install.setup)) {
        snprintf(rec.setup_rel, sizeof(rec.setup_rel), "%s", m.install.setup);
    } else if (content.setup_rel[0]
               && rel_file_exists(payload_dir, content.setup_rel)) {
        snprintf(rec.setup_rel, sizeof(rec.setup_rel), "%s", content.setup_rel);
    }
    if (m.install.launch && rel_file_exists(payload_dir, m.install.launch)) {
        snprintf(rec.profile_launch, sizeof(rec.profile_launch), "%s",
                 m.install.launch);
    } else if (target && target->exec && looks_playable_exec(target->exec)) {
        snprintf(rec.profile_launch, sizeof(rec.profile_launch), "%s", target->exec);
    } else if (looks_playable_exec(content.win_exec)) {
        snprintf(rec.profile_launch, sizeof(rec.profile_launch), "%s",
                 content.win_exec);
    } else if (looks_playable_exec(content.lin_exec)) {
        snprintf(rec.profile_launch, sizeof(rec.profile_launch), "%s",
                 content.lin_exec);
    }
    if (m.install.uninstall && m.install.uninstall[0]) {
        snprintf(rec.profile_uninstall, sizeof(rec.profile_uninstall), "%s",
                 m.install.uninstall);
    }
    assign_setup_state(&rec, needs_setup ? VAPOR_SETUP_PENDING : "");

    if (persist_install(vc, &rec) != 0) {
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
        printf("  location ... %s\n", disc_installer ? payload_dir : install_dir);
        printf("  contents ... %zu file(s), %s\n", nfiles, pretty);
        if (nexec > 0) {
            printf("  exec bits .. %zu file(s) made executable\n", nexec);
        }
        if (has_iso) {
            printf("  disc image . %d image(s) left in place\n", content.niso);
        }
        if (unpacked_iso && disc_installer) {
            printf("  disc image . extracted for the installer\n");
        } else if (unpacked_iso) {
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
            if (disc_installer) {
                vapor_plat_remove_tree(rec.payload_dir);
            }
            return -1;
        }
        {
            setup_hooks setup;

            memset(&setup, 0, sizeof(setup));
            setup.cb = on_setup_progress;
            setup.ud = &ip;
            setup.on_status = on_setup_status;
            setup.on_status_ud = &ip;
            setup.poll_cancel = progress_poll_cancel;
            setup.poll_ud = &ip;
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
run_tracked_ui(vapor_client *vc, vapor_install *rec, const char *setup_path,
               const char *cwd, const char *params, int *exit_code,
               const setup_hooks *hooks)
{
    int src = vapor_plat_run_ui(setup_path, cwd, params, exit_code,
                                hooks ? hooks_cancel : NULL, (void *)hooks);

    if (src < 0) {
        assign_setup_state(rec, VAPOR_SETUP_FAILED);
        (void)persist_install(vc, rec);
        vapor_client_set_error(vc, "could not start %s", setup_path);
        return -1;
    }
    return src > 0 ? 1 : 0;
}

static int
capture_finished_dest(const char *requested, const char *name, const char *id,
                      const char *slug, char *play_dir, size_t play_n,
                      char *exec_rel, size_t exec_n)
{
    char product[VAPOR_PATH_MAX];

    if (dest_is_complete(requested, slug, exec_rel, exec_n)) {
        snprintf(play_dir, play_n, "%s", requested);
        return 1;
    }
    product[0] = '\0';
    if (vapor_plat_find_product_dir(name, id, product, sizeof(product)) == 0
        && dest_is_complete(product, slug, exec_rel, exec_n)) {
        snprintf(play_dir, play_n, "%s", product);
        return 1;
    }
    return 0;
}

static int
note_setup_cancelled(vapor_client *vc, vapor_install *rec, const char *dest,
                     int exit_code)
{
    assign_setup_state(rec, VAPOR_SETUP_CANCELLED);
    snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
             VAPOR_INSTALL_KIND_OS_PRODUCT);
    if (dest && dest[0]) {
        capture_uninstall_recipe(rec, dest);
    }
    (void)persist_install(vc, rec);
    vapor_client_set_error(vc,
                           "the installer for %s was cancelled (exit %d); "
                           "run Setup again to finish",
                           rec->name[0] ? rec->name : rec->game_id, exit_code);
    printf("installer cancelled (exit %d)\n", exit_code);
    return 1;
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
    char           dest[VAPOR_PATH_MAX];
    int            exit_code = 0;
    int            found = 0;

    if (vapor_db_get_install(vc, game_id, &rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed; run \"vapor install %s\"",
                               game_id, game_id);
        return -1;
    }
    merge_install_sidecar(&rec);
    if (vapor_install_is_external(&rec)) {
        vapor_client_set_error(vc, "%s is a local shortcut; Vapor only launches it",
                               rec.name[0] ? rec.name : game_id);
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

    {
        const char *setup_rel = NULL;

        if (rec.setup_rel[0] && rel_file_exists(payload, rec.setup_rel)) {
            setup_rel = rec.setup_rel;
        } else if (m.install.setup && rel_file_exists(payload, m.install.setup)) {
            setup_rel = m.install.setup;
        } else if (scan.setup_rel[0]) {
            setup_rel = scan.setup_rel;
        }
        {
            char        setup_abs[VAPOR_PATH_MAX];
            const char *disc_abs = NULL;

            if (setup_rel
                && join(setup_abs, sizeof(setup_abs), payload, setup_rel) == 0) {
                disc_abs = setup_abs;
            }
            if (!setup_rel
                || looks_disc_image(path_basename(setup_rel), disc_abs)) {
                assign_setup_state(&rec, VAPOR_SETUP_FAILED);
                (void)persist_install(vc, &rec);
                vapor_client_set_error(vc,
                                       "%s has no setup.exe or .msi to run "
                                       "(a disc image is extracted before setup, "
                                       "not opened)",
                                       game_id);
                vapor_manifest_free(&m);
                return -1;
            }
        }
        if (join(setup_path, sizeof(setup_path), payload, setup_rel) != 0) {
            vapor_client_set_error(vc, "installer path is too long");
            vapor_manifest_free(&m);
            return -1;
        }
        snprintf(rec.setup_rel, sizeof(rec.setup_rel), "%s", setup_rel);
    }
    vapor_plat_native_path(setup_path);
    vapor_plat_native_path(payload);
    prefer_dir_bootstrapper(setup_path, sizeof(setup_path));
    path_dirname(setup_path, setup_cwd, sizeof(setup_cwd));
    vapor_plat_native_path(setup_cwd);
    dest[0] = '\0';
    {
        vapor_inst_kind kind;
        char            silent[2048];
        int             silent_rc;
        int             user_cancelled = 0;

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
        assign_setup_state(&rec, VAPOR_SETUP_RUNNING);
        if (persist_install(vc, &rec) != 0) {
            vapor_manifest_free(&m);
            return -1;
        }
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
            int ui;

            printf("unattended install to %s\n", dest);
            ui = run_tracked_ui(vc, &rec, setup_path, setup_cwd, silent,
                                &exit_code, hooks);
            if (ui < 0) {
                vapor_manifest_free(&m);
                return -1;
            }
            if (ui > 0) {
                user_cancelled = 1;
                printf("installer cancelled; checking %s\n", dest);
            } else {
                printf("installer process exited (%d); checking %s\n", exit_code,
                       dest);
                found = wait_install_dest(dest, rec.name, rec.game_id, m.id,
                                          play_dir, sizeof(play_dir), exec_rel,
                                          sizeof(exec_rel), hooks);
                if (found < 0) {
                    note_setup_cancelled(vc, &rec, dest, exit_code);
                    vapor_manifest_free(&m);
                    return 1;
                }
            }
            if (!found && ui == 0) {
                printf("unattended install did not finish; opening the wizard\n");
                ui = run_tracked_ui(vc, &rec, setup_path, setup_cwd, NULL,
                                    &exit_code, hooks);
                if (ui < 0) {
                    vapor_manifest_free(&m);
                    return -1;
                }
                if (ui > 0) {
                    user_cancelled = 1;
                    printf("installer cancelled; checking destination\n");
                } else {
                    printf("installer process exited (%d); checking destination\n",
                           exit_code);
                }
            }
            if (ui > 0 && !found) {
                found = capture_finished_dest(dest, rec.name, rec.game_id, m.id,
                                              play_dir, sizeof(play_dir),
                                              exec_rel, sizeof(exec_rel));
            }
        } else {
            int ui;

            printf("running installer %s\n", setup_path);
            if (kind == VAPOR_INST_ISHIELD) {
                printf("InstallShield needs the Setup window; silent mode "
                       "hangs on this family, so complete the wizard\n");
            }
            ui = run_tracked_ui(vc, &rec, setup_path, setup_cwd, NULL, &exit_code,
                                hooks);
            if (ui < 0) {
                vapor_manifest_free(&m);
                return -1;
            }
            if (ui > 0) {
                user_cancelled = 1;
                printf("installer cancelled; checking destination\n");
                found = capture_finished_dest(dest, rec.name, rec.game_id, m.id,
                                              play_dir, sizeof(play_dir),
                                              exec_rel, sizeof(exec_rel));
            } else {
                printf("installer process exited (%d); checking destination\n",
                       exit_code);
            }
        }
        if (!found && !user_cancelled) {
            found = wait_install_dest(dest, rec.name, rec.game_id, m.id,
                                      play_dir, sizeof(play_dir), exec_rel,
                                      sizeof(exec_rel), hooks);
            if (found < 0) {
                note_setup_cancelled(vc, &rec, dest, exit_code);
                vapor_manifest_free(&m);
                return 1;
            }
        }
        if (!found && dir_is_empty(dest)) {
            (void)vapor_plat_remove_tree(dest);
        }
    }

    if (!found) {
        /* The wizard may still have registered an Add/Remove Programs entry.
         * Either way the tree is not a finished install, so this is cancelled
         * and the disc mount is kept for Setup. */
        note_setup_cancelled(vc, &rec, dest, exit_code);
        vapor_manifest_free(&m);
        return 1;
    }

    if (set_windows_exec(&m, play_dir, exec_rel) != 0) {
        vapor_client_set_error(vc, "out of memory");
        vapor_manifest_free(&m);
        return -1;
    }
    {
        char library_dir[VAPOR_PATH_MAX];
        char mount[VAPOR_PATH_MAX];
        int  was_mount = 0;

        if (mount_dir_for(vc, game_id, mount, sizeof(mount)) == 0) {
            was_mount = paths_equal(payload, mount);
        }
        if (was_mount) {
            if (install_dir_for(vc, game_id, library_dir, sizeof(library_dir)) != 0
                || write_local_manifest(vc, library_dir, &m) != 0) {
                vapor_manifest_free(&m);
                return -1;
            }
            snprintf(rec.payload_dir, sizeof(rec.payload_dir), "%s", library_dir);
        } else if (write_local_manifest(vc, payload, &m) != 0) {
            vapor_manifest_free(&m);
            return -1;
        } else {
            snprintf(rec.payload_dir, sizeof(rec.payload_dir), "%s", payload);
        }
        snprintf(rec.install_dir, sizeof(rec.install_dir), "%s", play_dir);
        snprintf(rec.profile_launch, sizeof(rec.profile_launch), "%s", exec_rel);
        assign_setup_state(&rec, VAPOR_SETUP_COMPLETED);
        snprintf(rec.install_kind, sizeof(rec.install_kind), "%s",
                 VAPOR_INSTALL_KIND_OS_PRODUCT);
        capture_uninstall_recipe(&rec, play_dir);
        if (!rec.product_dir[0]) {
            snprintf(rec.product_dir, sizeof(rec.product_dir), "%s", play_dir);
        }
        vapor_plat_dir_size(play_dir, &rec.size_on_disk);
        rec.installed_at = vapor_now_unix();
        if (persist_install(vc, &rec) != 0) {
            vapor_manifest_free(&m);
            return -1;
        }
        if (was_mount) {
            (void)vapor_plat_remove_tree(mount);
        }
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

static int
progress_cb_cancel(void *ud)
{
    setup_hooks *h = (setup_hooks *)ud;

    if (!h || !h->cb) {
        return 0;
    }
    return h->cb(h->ud, 1, 2) != 0;
}

int
vapor_setup_game_tracked(vapor_client *vc, const char *game_id,
                         vapor_progress_fn cb, void *ud)
{
    setup_hooks hooks;

    memset(&hooks, 0, sizeof(hooks));
    hooks.cb = cb;
    hooks.ud = ud;
    hooks.poll_cancel = cb ? progress_cb_cancel : NULL;
    hooks.poll_ud = &hooks;
    return setup_game_impl(vc, game_id, &hooks);
}

typedef struct {
    char path[VAPOR_PATH_MAX];
    int  score;
} local_uninst;

static int
local_uninst_cb(const char *rel, const char *abs, void *ud)
{
    local_uninst *h = ud;
    const char   *base;
    int           score = 0;

    base = path_basename(rel);
    if (vapor_str_eq_ci(base, "unins000.exe")) {
        score = 300;
    } else if (vapor_str_has_prefix(base, "unins")
               && vapor_str_ends_with_ci(base, ".exe")) {
        score = 200;
    } else if (vapor_str_eq_ci(base, "uninstall.exe")
               || vapor_str_eq_ci(base, "uninstaller.exe")) {
        score = 100;
    }
    if (score == 0) {
        return 0;
    }
    /* Prefer a shallower match so the product-root unins000.exe wins over
     * a helper copy nested under _CommonRedist or similar. */
    score -= path_depth(rel);
    if (score > h->score && (size_t)strlen(abs) < sizeof(h->path)) {
        snprintf(h->path, sizeof(h->path), "%s", abs);
        h->score = score;
    }
    return 0;
}

static int
find_local_uninstaller(const char *dir, char *out, size_t outsz)
{
    local_uninst h;

    if (!dir || !dir[0] || !vapor_plat_is_dir(dir) || !out || outsz == 0) {
        return 1;
    }
    memset(&h, 0, sizeof(h));
    h.score = -1;
    if (vapor_plat_walk_files(dir, local_uninst_cb, &h) != 0 && h.score < 0) {
        return 1;
    }
    if (h.score < 0) {
        return 1;
    }
    if ((size_t)snprintf(out, outsz, "%s", h.path) >= outsz) {
        return 1;
    }
    return 0;
}

static void
copy_recipe_if_better(vapor_install *rec, const char *exe, const char *params,
                      const char *product)
{
    if (!exe || !exe[0] || rec->uninstall_exe[0]) {
        return;
    }
    snprintf(rec->uninstall_exe, sizeof(rec->uninstall_exe), "%s", exe);
    if (params) {
        snprintf(rec->uninstall_params, sizeof(rec->uninstall_params), "%s",
                 params);
    }
    if (product && product[0] && !rec->product_dir[0]) {
        snprintf(rec->product_dir, sizeof(rec->product_dir), "%s", product);
    }
}

static void
capture_uninstall_recipe(vapor_install *rec, const char *extra_dir)
{
    char        exe[VAPOR_PATH_MAX];
    char        params[2048];
    char        product[VAPOR_PATH_MAX];
    const char *dirs[4];
    int         i, n = 0;

    if (rec->install_dir[0]) {
        dirs[n++] = rec->install_dir;
    }
    if (rec->payload_dir[0]) {
        dirs[n++] = rec->payload_dir;
    }
    if (extra_dir && extra_dir[0]) {
        dirs[n++] = extra_dir;
    }

    for (i = 0; i < n && !rec->uninstall_exe[0]; i++) {
        exe[0] = params[0] = product[0] = '\0';
        if (vapor_plat_find_uninstall(rec->name, rec->game_id, dirs[i], exe,
                                      sizeof(exe), params, sizeof(params),
                                      product, sizeof(product))
            == 0) {
            copy_recipe_if_better(rec, exe, params, product);
        }
    }
    if (!rec->uninstall_exe[0]) {
        exe[0] = params[0] = product[0] = '\0';
        if (vapor_plat_find_uninstall(rec->name, rec->game_id, NULL, exe,
                                      sizeof(exe), params, sizeof(params),
                                      product, sizeof(product))
            == 0) {
            copy_recipe_if_better(rec, exe, params, product);
        }
    }
    for (i = 0; i < n && !rec->uninstall_exe[0]; i++) {
        exe[0] = '\0';
        if (find_local_uninstaller(dirs[i], exe, sizeof(exe)) == 0) {
            copy_recipe_if_better(rec, exe, "", NULL);
        }
    }
    if (!rec->product_dir[0]) {
        if (extra_dir && extra_dir[0] && vapor_plat_is_dir(extra_dir)
            && !dir_is_empty(extra_dir)) {
            snprintf(rec->product_dir, sizeof(rec->product_dir), "%s", extra_dir);
        } else if (rec->install_dir[0]) {
            snprintf(rec->product_dir, sizeof(rec->product_dir), "%s",
                     rec->install_dir);
        }
    }
}

static void
trim_trailing_sep(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
        s[--n] = '\0';
    }
}

static int
paths_same(const char *a, const char *b)
{
    char   aa[VAPOR_PATH_MAX];
    char   bb[VAPOR_PATH_MAX];
    size_t i;

    if (!a || !b || !a[0] || !b[0]) {
        return 0;
    }
    snprintf(aa, sizeof(aa), "%s", a);
    snprintf(bb, sizeof(bb), "%s", b);
    vapor_plat_native_path(aa);
    vapor_plat_native_path(bb);
    trim_trailing_sep(aa);
    trim_trailing_sep(bb);
    for (i = 0; aa[i]; i++) {
        if (aa[i] >= 'A' && aa[i] <= 'Z') {
            aa[i] = (char)(aa[i] - 'A' + 'a');
        }
    }
    for (i = 0; bb[i]; i++) {
        if (bb[i] >= 'A' && bb[i] <= 'Z') {
            bb[i] = (char)(bb[i] - 'A' + 'a');
        }
    }
    return strcmp(aa, bb) == 0;
}

static void
merge_install_sidecar(vapor_install *rec)
{
    if (rec->payload_dir[0]) {
        apply_install_sidecar_file(rec, rec->payload_dir);
    }
    if (rec->install_dir[0] && !paths_same(rec->install_dir, rec->payload_dir)) {
        apply_install_sidecar_file(rec, rec->install_dir);
    }
}

static int
kind_is_os_product(const char *kind)
{
    return kind && strcmp(kind, VAPOR_INSTALL_KIND_OS_PRODUCT) == 0;
}

static void
infer_install_kind(vapor_client *vc, vapor_install *rec)
{
    vapor_manifest      m;
    const vapor_target *t;
    char                exe[VAPOR_PATH_MAX];
    char                params[2048];
    char                listed[VAPOR_PATH_MAX];

    if (rec->install_kind[0]) {
        return;
    }
    if (rec->uninstall_exe[0] || rec->setup_pending) {
        snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
                 VAPOR_INSTALL_KIND_OS_PRODUCT);
        return;
    }
    exe[0] = params[0] = listed[0] = '\0';
    if (vapor_plat_find_uninstall(rec->name, rec->game_id, rec->install_dir, exe,
                                  sizeof(exe), params, sizeof(params), listed,
                                  sizeof(listed))
            == 0
        || vapor_plat_find_product_dir(rec->name, rec->game_id, listed,
                                       sizeof(listed))
               == 0) {
        snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
                 VAPOR_INSTALL_KIND_OS_PRODUCT);
        return;
    }
    if (vapor_read_local_manifest(vc, rec->game_id, &m) == 0) {
        t = vapor_manifest_pick_target(&m, vapor_host_platform(),
                                       vapor_host_arch());
        if ((t && t->runtime && t->runtime[0]
             && strcmp(t->runtime, "native") != 0)
            || looks_dhewm3_payload(rec->install_dir, rec->game_id,
                                    t && t->exec ? t->exec : "")) {
            snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
                     VAPOR_INSTALL_KIND_RUNTIME);
        } else {
            snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
                     VAPOR_INSTALL_KIND_PORTABLE);
        }
        vapor_manifest_free(&m);
        return;
    }
    snprintf(rec->install_kind, sizeof(rec->install_kind), "%s",
             VAPOR_INSTALL_KIND_PORTABLE);
}

static int
path_is_under(const char *path, const char *root)
{
    char   p[VAPOR_PATH_MAX], r[VAPOR_PATH_MAX];
    size_t i, n;

    if (!path || !root || !path[0] || !root[0]) {
        return 0;
    }
    snprintf(p, sizeof(p), "%s", path);
    snprintf(r, sizeof(r), "%s", root);
    vapor_plat_native_path(p);
    vapor_plat_native_path(r);
    trim_trailing_sep(p);
    trim_trailing_sep(r);
    for (i = 0; p[i]; i++) {
        if (p[i] >= 'A' && p[i] <= 'Z') {
            p[i] = (char)(p[i] - 'A' + 'a');
        }
    }
    for (i = 0; r[i]; i++) {
        if (r[i] >= 'A' && r[i] <= 'Z') {
            r[i] = (char)(r[i] - 'A' + 'a');
        }
    }
    n = strlen(r);
    if (n == 0 || strncmp(p, r, n) != 0) {
        return 0;
    }
    return p[n] == '\0' || p[n] == '/' || p[n] == '\\';
}

/* Refuse drive roots and the Windows directories themselves. A game folder
 * under Program Files is fine; Program Files is not. Shared emulator
 * runtimes under {data_dir}/runtimes are never deleted with a game. */
static int
removal_is_safe(const char *path, const char *library, const char *data_dir)
{
    char        tmp[VAPOR_PATH_MAX];
    char        runtimes[VAPOR_PATH_MAX];
    const char *base;

    if (!path || !path[0]) {
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s", path);
    vapor_plat_native_path(tmp);
    trim_trailing_sep(tmp);
    if (strlen(tmp) < 4) {
        return 0;
    }
    if (library && library[0] && paths_same(tmp, library)) {
        return 0;
    }
    if (data_dir && data_dir[0]
        && join(runtimes, sizeof(runtimes), data_dir, "runtimes") == 0
        && path_is_under(tmp, runtimes)) {
        return 0;
    }
    base = path_basename(tmp);
    if (vapor_str_eq_ci(base, "Program Files")
        || vapor_str_eq_ci(base, "Program Files (x86)")
        || vapor_str_eq_ci(base, "Windows")
        || vapor_str_eq_ci(base, "Users")
        || vapor_str_eq_ci(base, "ProgramData")) {
        return 0;
    }
    return 1;
}

static int
remove_install_tree(vapor_client *vc, const char *path)
{
    char native[VAPOR_PATH_MAX];

    if (!path || !path[0]) {
        return 0;
    }
    snprintf(native, sizeof(native), "%s", path);
    vapor_plat_native_path(native);
    trim_trailing_sep(native);
    if (!removal_is_safe(native, vc->cfg.library_dir, vc->data_dir)) {
        return 0;
    }
    if (!vapor_plat_exists(native)) {
        return 0;
    }
    printf("removing %s\n", native);
    if (vapor_plat_remove_tree(native) != 0) {
        vapor_client_set_error(vc, "cannot remove %s", native);
        return -1;
    }
    return 0;
}

static int
resolve_rel_uninstall(const char *rel, const char *const *roots, int nroots,
                      char *exe, size_t exesz, char *params, size_t paramsz)
{
    int i;

    if (!rel || !rel[0]) {
        return 1;
    }
    for (i = 0; i < nroots; i++) {
        char            path[VAPOR_PATH_MAX];
        vapor_inst_kind kind;

        if (!rel_file_exists(roots[i], rel)) {
            continue;
        }
        if (join(path, sizeof(path), roots[i], rel) != 0) {
            continue;
        }
        vapor_plat_native_path(path);
        if ((size_t)snprintf(exe, exesz, "%s", path) >= exesz) {
            continue;
        }
        kind = vapor_inst_detect(path, roots[i]);
        if (vapor_inst_uninstall_params(kind, path, params, paramsz) != 0) {
            /* Inno/NSIS setup.exe and InstallShield have no safe uninstall
             * switch here; skip them instead of launching the installer. */
            exe[0] = '\0';
            params[0] = '\0';
            continue;
        }
        return 0;
    }
    return 1;
}

static int
uninstall_game_impl(vapor_client *vc, const char *game_id, vapor_progress_fn cb,
                    void *ud)
{
    vapor_install rec;
    char          library_dir[VAPOR_PATH_MAX];
    char          exe[VAPOR_PATH_MAX];
    char          params[2048];
    char          product_dir[VAPOR_PATH_MAX];
    char          cwd[VAPOR_PATH_MAX];
    int           exit_code = 0;
    int           is_os;

    if (vapor_db_get_install(vc, game_id, &rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed", game_id);
        return -1;
    }
    /* A shortcut only remembers a path. Forgetting it must not walk that
     * directory, run an uninstaller, or delete anything. */
    if (vapor_install_is_external(&rec)) {
        if (vapor_db_forget_install(vc, game_id) != 0) {
            vapor_client_set_error(vc, "could not update the local database");
            return -1;
        }
        printf("removed the library shortcut for %s; left %s in place\n",
               rec.name[0] ? rec.name : game_id,
               rec.launch_exe[0] ? rec.launch_exe : rec.install_dir);
        return 0;
    }
    if (install_dir_for(vc, game_id, library_dir, sizeof(library_dir)) != 0) {
        return -1;
    }
    merge_install_sidecar(&rec);
    infer_install_kind(vc, &rec);
    is_os = kind_is_os_product(rec.install_kind);

    exe[0] = params[0] = product_dir[0] = '\0';
    if (rec.product_dir[0]) {
        snprintf(product_dir, sizeof(product_dir), "%s", rec.product_dir);
    }
    if (is_os && rec.profile_uninstall[0]) {
        const char *roots[4];

        roots[0] = rec.product_dir;
        roots[1] = rec.install_dir;
        roots[2] = rec.payload_dir;
        roots[3] = library_dir;
        if (resolve_rel_uninstall(rec.profile_uninstall, roots, 4, exe,
                                  sizeof(exe), params, sizeof(params))
            == 0) {
            printf("using profile uninstaller %s\n", exe);
        }
    }
    if (!exe[0] && rec.uninstall_exe[0]) {
        snprintf(exe, sizeof(exe), "%s", rec.uninstall_exe);
        snprintf(params, sizeof(params), "%s", rec.uninstall_params);
    }

    /* portable / runtime: never run a vendor uninstaller. os_product (and
     * older installs inferred as such) use the recorded recipe, then
     * rediscover by InstallLocation / DisplayName / local unins*.exe. */
    if (is_os && !exe[0]) {
        capture_uninstall_recipe(&rec, rec.product_dir[0] ? rec.product_dir
                                                          : NULL);
        if (rec.uninstall_exe[0]) {
            snprintf(exe, sizeof(exe), "%s", rec.uninstall_exe);
            snprintf(params, sizeof(params), "%s", rec.uninstall_params);
        }
        if (!product_dir[0] && rec.product_dir[0]) {
            snprintf(product_dir, sizeof(product_dir), "%s", rec.product_dir);
        }
        if (!exe[0]) {
            if (find_local_uninstaller(rec.install_dir, exe, sizeof(exe)) != 0
                && find_local_uninstaller(rec.payload_dir, exe, sizeof(exe))
                       != 0
                && find_local_uninstaller(library_dir, exe, sizeof(exe)) != 0) {
                exe[0] = '\0';
            }
        }
    }
    if (is_os && !exe[0] && rec.setup_rel[0]) {
        const char *roots[3];

        roots[0] = rec.payload_dir;
        roots[1] = library_dir;
        roots[2] = rec.install_dir;
        if (resolve_rel_uninstall(rec.setup_rel, roots, 3, exe, sizeof(exe),
                                  params, sizeof(params))
            == 0) {
            printf("using the game installer to uninstall %s\n", exe);
        } else {
            exe[0] = params[0] = '\0';
        }
    }

    if (is_os && exe[0] && !params[0]) {
        char            dir[VAPOR_PATH_MAX];
        vapor_inst_kind kind;

        path_dirname(exe, dir, sizeof(dir));
        kind = vapor_inst_detect(exe, dir);
        if (vapor_inst_uninstall_params(kind, exe, params, sizeof(params)) != 0) {
            params[0] = '\0';
        }
    }

    /* A Windows installer copies the game into Program Files and adds an
     * Add/Remove Programs entry. Deleting Vapor's folder does not remove
     * that entry. InstallShield also publishes a second key with the same
     * name and an empty uninstall command; the real command is the other
     * key (IDriver.exe /M{GUID}). */
    if (is_os && !exe[0]) {
        char listed[VAPOR_PATH_MAX];

        listed[0] = '\0';
        if (vapor_plat_find_product_dir(rec.name, rec.game_id, listed,
                                        sizeof(listed))
            == 0) {
            vapor_client_set_error(vc,
                                   "Windows still lists %s at %s, and Vapor "
                                   "could not find its uninstaller. The game "
                                   "was left installed",
                                   rec.name[0] ? rec.name : game_id, listed);
            return -1;
        }
    }

    if (is_os && exe[0]) {
        char still_exe[VAPOR_PATH_MAX];
        char still_params[2048];
        char still_dir[VAPOR_PATH_MAX];

        path_dirname(exe, cwd, sizeof(cwd));
        printf("running uninstaller %s%s%s\n", exe, params[0] ? " " : "",
               params);
        fflush(stdout);
        {
            setup_hooks hooks;
            int         src;

            memset(&hooks, 0, sizeof(hooks));
            hooks.cb = cb;
            hooks.ud = ud;
            hooks.poll_cancel = cb ? progress_cb_cancel : NULL;
            hooks.poll_ud = &hooks;
            src = vapor_plat_run_ui(exe, cwd, params[0] ? params : NULL,
                                    &exit_code, cb ? hooks_cancel : NULL,
                                    &hooks);
            if (src > 0) {
                vapor_client_set_error(vc,
                                       "uninstaller for %s was cancelled; the "
                                       "game was left installed",
                                       rec.name[0] ? rec.name : game_id);
                return -1;
            }
            if (src != 0) {
                vapor_client_set_error(vc, "could not start uninstaller %s", exe);
                return -1;
            }
        }
        /* 3010 is Windows' "success, reboot required". */
        if (exit_code != 0 && exit_code != 3010) {
            vapor_client_set_error(vc,
                                   "uninstaller for %s exited (%d); the "
                                   "installed files were left in place",
                                   rec.name[0] ? rec.name : game_id, exit_code);
            return -1;
        }
        /* The maintenance window can return 0 on Cancel or Repair. */
        still_exe[0] = still_params[0] = still_dir[0] = '\0';
        if (vapor_plat_find_uninstall(rec.name, rec.game_id,
                                      rec.install_dir[0] ? rec.install_dir
                                                         : product_dir,
                                      still_exe, sizeof(still_exe), still_params,
                                      sizeof(still_params), still_dir,
                                      sizeof(still_dir))
            == 0) {
            vapor_client_set_error(vc,
                                   "Windows still lists %s. Its uninstaller "
                                   "did not remove the Add/Remove Programs "
                                   "entry, so the game was left installed",
                                   rec.name[0] ? rec.name : game_id);
            return -1;
        }
    }

    {
        char mount[VAPOR_PATH_MAX];

        if (mount_dir_for(vc, game_id, mount, sizeof(mount)) == 0
            && remove_install_tree(vc, mount) != 0) {
            return -1;
        }
    }
    if (remove_install_tree(vc, product_dir) != 0
        || remove_install_tree(vc, rec.install_dir) != 0
        || remove_install_tree(vc, rec.payload_dir) != 0
        || remove_install_tree(vc, rec.product_dir) != 0
        || remove_install_tree(vc, library_dir) != 0) {
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
vapor_uninstall_game(vapor_client *vc, const char *game_id)
{
    return uninstall_game_impl(vc, game_id, NULL, NULL);
}

int
vapor_uninstall_game_cancellable(vapor_client *vc, const char *game_id,
                                 vapor_progress_fn cb, void *ud)
{
    return uninstall_game_impl(vc, game_id, cb, ud);
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
    if (vapor_install_is_external(&rec)) {
        if (rec.launch_exe[0] && vapor_plat_exists(rec.launch_exe)
            && !vapor_plat_is_dir(rec.launch_exe)) {
            printf("  shortcut %s\n", rec.launch_exe);
            return 0;
        }
        printf("  shortcut target is missing: %s\n",
               rec.launch_exe[0] ? rec.launch_exe : "(none)");
        vapor_client_set_error(vc, "the program for %s is missing",
                               rec.name[0] ? rec.name : game_id);
        return 1;
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
