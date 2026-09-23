/* Target selection, variable expansion, process spawn, playtime accounting.
 * The manifest is read from the install directory, so launching works offline. */

#include "vapor/client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "installer.h"
#include "net.h"
#include "platform.h"
#include "vapor/buf.h"
#include "vapor/util.h"
#include "vapor/wise.h"

#include "miniz.h"

/* Expands $INSTALL_DIR and ${INSTALL_DIR}. Deliberately the only variable we
 * substitute: manifests should not be able to read arbitrary host environment
 * into a command line. Returns a malloc'd string, or NULL on allocation
 * failure. */
static char *
expand_install_dir(const char *in, const char *install_dir)
{
    static const char NAME[] = "INSTALL_DIR";
    vapor_buf         out;
    const char       *p = in;
    int               substituted = 0;

    vapor_buf_init(&out);
    if (vapor_buf_append(&out, "", 0) != 0) {
        return NULL;
    }

    while (*p) {
        if (*p != '$') {
            if (vapor_buf_append(&out, p, 1) != 0) {
                goto fail;
            }
            p++;
            continue;
        }

        if (p[1] == '{') {
            size_t n = strlen(NAME);
            if (strncmp(p + 2, NAME, n) == 0 && p[2 + n] == '}') {
                if (vapor_buf_appends(&out, install_dir) != 0) {
                    goto fail;
                }
                substituted = 1;
                p += 2 + n + 1;
                continue;
            }
        } else if (strncmp(p + 1, NAME, strlen(NAME)) == 0) {
            char after = p[1 + strlen(NAME)];
            /* Only a non-identifier character ends the name, so $INSTALL_DIRX
             * is left alone. */
            if (!((after >= 'A' && after <= 'Z') || (after >= 'a' && after <= 'z')
                  || (after >= '0' && after <= '9') || after == '_')) {
                if (vapor_buf_appends(&out, install_dir) != 0) {
                    goto fail;
                }
                substituted = 1;
                p += 1 + strlen(NAME);
                continue;
            }
        }

        if (vapor_buf_append(&out, p, 1) != 0) {
            goto fail;
        }
        p++;
    }
    /* Manifests are written once for every platform and so always spell paths
     * with '/'. A value anchored at $INSTALL_DIR is by definition a path, so
     * fix up its separators; anything else is left byte-for-byte, since a bare
     * '/' elsewhere may well be a URL or a switch rather than a path. */
    if (substituted && out.data) {
        vapor_plat_native_path(out.data);
    }
    return vapor_buf_release(&out);

fail:
    vapor_buf_free(&out);
    return NULL;
}

static int
path_join(char *out, size_t outsz, const char *dir, const char *rel)
{
    int n = snprintf(out, outsz, "%s/%s", dir, rel);

    if (n < 0 || (size_t)n >= outsz) {
        return -1;
    }
    vapor_plat_native_path(out);
    return 0;
}

static int
file_in_dir(const char *dir, const char *name, char *out, size_t outsz)
{
    if (path_join(out, outsz, dir, name) != 0) {
        return 0;
    }
    return vapor_plat_exists(out) && !vapor_plat_is_dir(out);
}

static void
parent_dir(const char *path, char *out, size_t outsz)
{
    const char *s = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    size_t      n;

    if (b > s) {
        s = b;
    }
    if (!s || s == path) {
        snprintf(out, outsz, ".");
        return;
    }
    n = (size_t)(s - path);
    if (n >= outsz) {
        n = outsz - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

/* dhewm3 is a Doom 3 engine, not a general id Tech 4 runner. Retail Doom 3
 * (and Resurrection of Evil) ships base/pak000.pk4 + base/game00.pk4. */
static int
looks_doom3_tree(const char *dir, const char *game_id, const char *exec_rel)
{
    char        pak[VAPOR_PATH_MAX], gamepak[VAPOR_PATH_MAX];
    char        stem[VAPOR_ID_MAX + 1];
    const char *base;

    if (path_join(pak, sizeof(pak), dir, "base/pak000.pk4") != 0
        || !vapor_plat_exists(pak)) {
        return 0;
    }
    if (path_join(gamepak, sizeof(gamepak), dir, "base/game00.pk4") != 0
        || !vapor_plat_exists(gamepak)) {
        return 0;
    }
    if (vapor_slug_match(game_id, "doom3") || vapor_slug_match(game_id, "d3xp")) {
        return 1;
    }
    base = strrchr(exec_rel, '/');
    base = base ? base + 1 : exec_rel;
    if (vapor_id_slug_stem(base, stem, sizeof(stem)) == 0
        && (vapor_slug_match(stem, "doom3") || vapor_slug_match(stem, "d3xp"))) {
        return 1;
    }
    return 0;
}

static int find_dhewm3_in_dir(const char *dir, char *out, size_t outsz);

static int
find_dhewm3(vapor_client *vc, const vapor_install *rec, char *out, size_t outsz)
{
    char runtime[VAPOR_PATH_MAX];

    if (file_in_dir(rec->install_dir, "dhewm3.exe", out, outsz)
        || file_in_dir(rec->install_dir, "dhewm3", out, outsz)) {
        return 0;
    }
    if (rec->payload_dir[0]
        && (file_in_dir(rec->payload_dir, "dhewm3.exe", out, outsz)
            || file_in_dir(rec->payload_dir, "dhewm3", out, outsz))) {
        return 0;
    }
    if (path_join(runtime, sizeof(runtime), vc->data_dir, "runtimes/dhewm3") == 0
        && (file_in_dir(runtime, "dhewm3.exe", out, outsz)
            || file_in_dir(runtime, "dhewm3", out, outsz)
            || find_dhewm3_in_dir(runtime, out, outsz) == 0)) {
        return 0;
    }
    if (vapor_plat_search_path("dhewm3.exe", out, outsz) == 0
        || vapor_plat_search_path("dhewm3", out, outsz) == 0) {
        return 0;
    }
    return 1;
}

/* Official GPL Windows build. dhewm3 is the id Tech 4 runtime for retail
 * Doom 3 paks; Vapor never runs the SafeDisc wrapper. */
#define DHEWM3_WIN_URL \
    "https://github.com/dhewm/dhewm3/releases/download/1.5.5/dhewm3-1.5.5_win32.zip"

static int
zip_entry_rel(const char *name, char *out, size_t outsz)
{
    const char *p = name;
    size_t      n = 0;

    if (!p || !*p) {
        return -1;
    }
    while (*p == '/' || *p == '\\') {
        p++;
    }
    if (!*p || strchr(p, ':')) {
        return -1;
    }
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
        if (seglen > 0 && !(seglen == 1 && seg[0] == '.')) {
            if (n + seglen + 2 > outsz) {
                return -1;
            }
            if (n > 0) {
                out[n++] = '/';
            }
            memcpy(out + n, seg, seglen);
            n += seglen;
        }
        if (*p) {
            p++;
        }
    }
    if (n == 0 || n >= outsz) {
        return -1;
    }
    out[n] = '\0';
    return 0;
}

static int
mkdirs_parent(const char *path)
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
extract_zip_tree(vapor_client *vc, const char *zip_path, const char *dest_dir)
{
    mz_zip_archive zip;
    mz_uint        i, count;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        vapor_client_set_error(vc, "%s is not a readable zip archive", zip_path);
        return -1;
    }
    count = mz_zip_reader_get_num_files(&zip);
    for (i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        char                     rel[VAPOR_PATH_MAX];
        char                     dest[VAPOR_PATH_MAX];

        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            vapor_client_set_error(vc, "cannot read dhewm3 archive entry %u",
                                   (unsigned)i);
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (zip_entry_rel(st.m_filename, rel, sizeof(rel)) != 0) {
            continue;
        }
        if (path_join(dest, sizeof(dest), dest_dir, rel) != 0) {
            vapor_client_set_error(vc, "dhewm3 extract path is too long");
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            if (vapor_plat_mkdirs(dest) != 0) {
                vapor_client_set_error(vc, "cannot create %s", dest);
                mz_zip_reader_end(&zip);
                return -1;
            }
            continue;
        }
        if (mkdirs_parent(dest) != 0 || vapor_plat_mkdirs(dest_dir) != 0) {
            vapor_client_set_error(vc, "cannot create the directory for %s", dest);
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, dest, 0)) {
            vapor_client_set_error(vc, "cannot write %s", dest);
            mz_zip_reader_end(&zip);
            return -1;
        }
    }
    mz_zip_reader_end(&zip);
    return 0;
}

typedef struct {
    char path[VAPOR_PATH_MAX];
} dhewm_hit;

static int
dhewm_walk_cb(const char *rel, const char *abs, void *ud)
{
    dhewm_hit  *h = ud;
    const char *base = strrchr(rel, '/');

    base = base ? base + 1 : rel;
    if (vapor_str_eq_ci(base, "dhewm3.exe") || vapor_str_eq_ci(base, "dhewm3")) {
        snprintf(h->path, sizeof(h->path), "%s", abs);
        return 1;
    }
    return 0;
}

static int
find_dhewm3_in_dir(const char *dir, char *out, size_t outsz)
{
    dhewm_hit h;

    memset(&h, 0, sizeof(h));
    if (!dir || !dir[0] || !vapor_plat_is_dir(dir)) {
        return 1;
    }
    vapor_plat_walk_files(dir, dhewm_walk_cb, &h);
    if (!h.path[0]) {
        return 1;
    }
    if ((size_t)snprintf(out, outsz, "%s", h.path) >= outsz) {
        return 1;
    }
    vapor_plat_native_path(out);
    return 0;
}

static int
ensure_dhewm3(vapor_client *vc, const vapor_install *rec, char *out,
              size_t outsz)
{
    char runtime[VAPOR_PATH_MAX];
    char zip_path[VAPOR_PATH_MAX];

    if (find_dhewm3(vc, rec, out, outsz) == 0) {
        return 0;
    }
#if !defined(_WIN32)
    vapor_client_set_error(vc,
                           "this Doom 3 install needs dhewm3 (Windows blocked "
                           "SafeDisc). Install dhewm3 and try Play again");
    return -1;
#else
    if (path_join(runtime, sizeof(runtime), vc->data_dir, "runtimes/dhewm3") != 0
        || path_join(zip_path, sizeof(zip_path), vc->data_dir,
                     "runtimes/dhewm3.zip")
               != 0) {
        vapor_client_set_error(vc, "runtime path is too long");
        return -1;
    }
    if (vapor_plat_mkdirs(runtime) != 0) {
        vapor_client_set_error(vc, "cannot create %s", runtime);
        return -1;
    }
    printf("downloading dhewm3 (SafeDisc cannot run on this Windows)\n");
    fflush(stdout);
    if (vapor_http_fetch_url(vc, DHEWM3_WIN_URL, zip_path, NULL, NULL) != 0) {
        return -1;
    }
    if (extract_zip_tree(vc, zip_path, runtime) != 0) {
        remove(zip_path);
        return -1;
    }
    remove(zip_path);
    if (find_dhewm3_in_dir(runtime, out, outsz) != 0) {
        vapor_client_set_error(vc,
                               "downloaded dhewm3 but dhewm3.exe was not in the "
                               "archive");
        return -1;
    }
    printf("using source port %s\n", out);
    return 0;
#endif
}

int
vapor_launch_game(vapor_client *vc, const char *game_id, int *out_exit)
{
    vapor_manifest      m;
    vapor_install       rec;
    const vapor_target *t;
    char                install_dir[VAPOR_PATH_MAX];
    char                exec_path[VAPOR_PATH_MAX];
    char                cwd_path[VAPOR_PATH_MAX];
    char              **argv = NULL;
    vapor_kv           *env = NULL;
    size_t              nenv = 0, i, argv_n = 0;
    int64_t             started, elapsed;
    int                 exit_code = -1, rc = -1;
    int                 use_dhewm3 = 0;

    if (vapor_db_get_install(vc, game_id, &rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed; run \"vapor install %s\"",
                               game_id, game_id);
        return -1;
    }
    snprintf(install_dir, sizeof(install_dir), "%s", rec.install_dir);
    vapor_disc_finish_install(install_dir);

    if (rec.setup_pending) {
        vapor_client_set_error(vc,
                               "%s still needs its Windows installer; run "
                               "Setup from the library (or \"vapor setup %s\")",
                               rec.name[0] ? rec.name : game_id, game_id);
        return -1;
    }

    if (vapor_read_local_manifest(vc, game_id, &m) != 0) {
        return -1;
    }

    t = vapor_manifest_pick_target(&m, vapor_host_platform(), vapor_host_arch());
    if (!t) {
        vapor_client_set_error(vc,
                               "%s has no launchable executable for %s/%s "
                               "(disc images cannot be launched yet)",
                               game_id, vapor_host_platform(), vapor_host_arch());
        vapor_manifest_free(&m);
        return -1;
    }
    if (t->runtime && *t->runtime && strcmp(t->runtime, "native") != 0) {
        /* The schema allows "proton"/"wine"; nothing implements them yet, and
         * silently running the binary natively would just fail confusingly. */
        vapor_client_set_error(vc,
                               "%s needs the \"%s\" runtime, which this build "
                               "does not support yet", game_id, t->runtime);
        vapor_manifest_free(&m);
        return -1;
    }
    {
        const char *rel = t->exec ? t->exec : "";
        const char *base = strrchr(rel, '/');

        base = base ? base + 1 : rel;
        if (vapor_str_has_prefix(rel, "Setup/")
            || vapor_str_has_prefix(rel, "setup/")
            || vapor_str_has_prefix(rel, "DirectX/")
            || vapor_str_has_prefix(rel, "directx/")
            || vapor_str_eq_ci(base, "setup.exe")
            || vapor_str_eq_ci(base, "launch.exe")
            || vapor_str_eq_ci(base, "autorun.exe")
            || vapor_str_eq_ci(base, "install.exe")
            || vapor_str_has_prefix(base, "setup_")
            || vapor_str_has_prefix(base, "instmsi")) {
            rec.setup_pending = 1;
            (void)vapor_db_record_install(vc, &rec);
            vapor_client_set_error(vc,
                                   "%s still needs its Windows installer; run "
                                   "Setup from the library (or \"vapor setup %s\")",
                                   rec.name[0] ? rec.name : game_id, game_id);
            vapor_manifest_free(&m);
            return -1;
        }
    }

    if ((size_t)snprintf(exec_path, sizeof(exec_path), "%s/%s", install_dir,
                         t->exec)
        >= sizeof(exec_path)) {
        vapor_client_set_error(vc, "launch path is too long");
        vapor_manifest_free(&m);
        return -1;
    }
    vapor_plat_native_path(exec_path);
    if (!vapor_plat_exists(exec_path)) {
        vapor_client_set_error(vc,
                               "the launch target %s is missing; run "
                               "\"vapor install %s --force\" to repair",
                               t->exec, game_id);
        vapor_manifest_free(&m);
        return -1;
    }

    if (vapor_exe_is_copy_protected(exec_path)) {
        char alt[VAPOR_PATH_MAX];

        /* Running the wrapped exe just pops SafeDisc's fake administrator
         * dialog. Prefer a patched/source-port binary in the same tree. */
        if (vapor_find_unprotected_exe(install_dir, game_id, alt, sizeof(alt))
            == 0) {
            snprintf(exec_path, sizeof(exec_path), "%s", alt);
        } else if (rec.payload_dir[0]
                   && vapor_find_unprotected_exe(rec.payload_dir, game_id, alt,
                                                 sizeof(alt))
                          == 0) {
            snprintf(exec_path, sizeof(exec_path), "%s", alt);
        } else if (looks_doom3_tree(install_dir, game_id, t->exec)) {
            if (ensure_dhewm3(vc, &rec, alt, sizeof(alt)) != 0) {
                vapor_manifest_free(&m);
                return -1;
            }
            snprintf(exec_path, sizeof(exec_path), "%s", alt);
            use_dhewm3 = 1;
        } else {
            vapor_client_set_error(vc,
                                   "this executable uses SafeDisc (SECDRV), "
                                   "which Windows blocked. The administrator "
                                   "dialog is that DRM, not a login. Put the "
                                   "publisher's patch or a source-port exe "
                                   "in the install folder and try Play again");
            vapor_manifest_free(&m);
            return -1;
        }
    }

    if (use_dhewm3) {
        parent_dir(exec_path, cwd_path, sizeof(cwd_path));
    } else if (t->cwd && *t->cwd) {
        if ((size_t)snprintf(cwd_path, sizeof(cwd_path), "%s/%s", install_dir,
                             t->cwd)
            >= sizeof(cwd_path)) {
            vapor_client_set_error(vc, "working directory path is too long");
            vapor_manifest_free(&m);
            return -1;
        }
    } else {
        snprintf(cwd_path, sizeof(cwd_path), "%s", install_dir);
    }
    vapor_plat_native_path(cwd_path);

    argv_n = use_dhewm3 ? 3 : t->nargs;
    argv = (char **)calloc(argv_n + 2, sizeof(*argv));
    if (!argv) {
        vapor_client_set_error(vc, "out of memory");
        vapor_manifest_free(&m);
        return -1;
    }
    argv[0] = vapor_strdup(exec_path);
    if (!argv[0]) {
        goto cleanup;
    }
    if (use_dhewm3) {
        argv[1] = vapor_strdup("+set");
        argv[2] = vapor_strdup("fs_basepath");
        argv[3] = vapor_strdup(install_dir);
        if (!argv[1] || !argv[2] || !argv[3]) {
            vapor_client_set_error(vc, "out of memory");
            goto cleanup;
        }
    } else {
        for (i = 0; i < t->nargs; i++) {
            argv[i + 1] = expand_install_dir(t->args[i], install_dir);
            if (!argv[i + 1]) {
                vapor_client_set_error(vc, "out of memory");
                goto cleanup;
            }
        }
    }
    argv[argv_n + 1] = NULL;

    if (t->nenv > 0) {
        env = (vapor_kv *)calloc(t->nenv, sizeof(*env));
        if (!env) {
            vapor_client_set_error(vc, "out of memory");
            goto cleanup;
        }
        for (i = 0; i < t->nenv; i++) {
            env[i].key = vapor_strdup(t->env[i].key);
            env[i].value = expand_install_dir(t->env[i].value, install_dir);
            if (!env[i].key || !env[i].value) {
                vapor_client_set_error(vc, "out of memory");
                nenv = i + 1;
                goto cleanup;
            }
        }
        nenv = t->nenv;
    }

    printf("launching %s %s\n", rec.name[0] ? rec.name : game_id, rec.version);
    if (use_dhewm3) {
        printf("using source port %s\n", exec_path);
    }
    /* Flushed before spawning: the child writes straight to the fd, so leaving
     * our own output buffered would interleave it out of order. */
    fflush(stdout);

    started = vapor_now_unix();
    if (vapor_plat_run(exec_path, argv, cwd_path, env, nenv, &exit_code) != 0) {
        vapor_client_set_error(vc, "could not start %s", exec_path);
        goto cleanup;
    }
    elapsed = vapor_now_unix() - started;
    if (elapsed < 0) {
        elapsed = 0;
    }

    /* Recorded even on a non-zero exit: the session still happened. */
    vapor_db_add_playtime(vc, game_id, started, elapsed);

    {
        char pretty[64];
        vapor_format_duration(elapsed, pretty, sizeof(pretty));
        if (exit_code == 0) {
            printf("%s exited normally after %s\n",
                   rec.name[0] ? rec.name : game_id, pretty);
        } else {
            printf("%s exited with code %d after %s\n",
                   rec.name[0] ? rec.name : game_id, exit_code, pretty);
        }
    }
    if (out_exit) {
        *out_exit = exit_code;
    }
    rc = 0;

cleanup:
    if (argv) {
        for (i = 0; i < argv_n + 2; i++) {
            free(argv[i]);
        }
        free(argv);
    }
    if (env) {
        for (i = 0; i < nenv; i++) {
            free(env[i].key);
            free(env[i].value);
        }
        free(env);
    }
    vapor_manifest_free(&m);
    return rc;
}
