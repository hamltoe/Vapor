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

static int
dll_list_has(const char *list, const char *dll)
{
    size_t n, i, len;

    if (!list || !dll || !dll[0]) {
        return 0;
    }
    n = strlen(dll);
    while (*list) {
        const char *start;
        const char *end;

        while (*list == ' ' || *list == ',') {
            list++;
        }
        if (!*list) {
            break;
        }
        start = list;
        while (*list && *list != ',') {
            list++;
        }
        end = list;
        while (end > start && end[-1] == ' ') {
            end--;
        }
        len = (size_t)(end - start);
        if (len == n) {
            for (i = 0; i < n; i++) {
                unsigned char a = (unsigned char)start[i];
                unsigned char b = (unsigned char)dll[i];

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
            if (i == n) {
                return 1;
            }
        }
    }
    return 0;
}

/* The Windows loader dialog ("binkw32.dll was not found") is too late: Play
 * reports the missing names itself. Companion DLLs have to be the ones that
 * shipped with the game. steam_api.dll is not synthesized. */
static int
refuse_missing_dlls(vapor_client *vc, const char *exec_path)
{
    char        missing[640];
    const char *base;
    const char *slash;
    int         rc;

    rc = vapor_plat_missing_dlls(exec_path, missing, sizeof(missing));
    if (rc <= 0) {
        return 0;
    }
    slash = strrchr(exec_path, '\\');
    if (!slash) {
        slash = strrchr(exec_path, '/');
    }
    base = slash ? slash + 1 : exec_path;
    if (dll_list_has(missing, "steam_api.dll")
        || dll_list_has(missing, "steam_api64.dll")) {
        vapor_client_set_error(vc,
                               "cannot start %s; missing %s. steam_api.dll is "
                               "the Steamworks library and Vapor does not "
                               "emulate it. Any other name in that list is a "
                               "file from the game's own install (Bink video, "
                               "the Sixense Hydra library, and similar) and "
                               "has to sit next to the executable",
                               base, missing);
    } else {
        vapor_client_set_error(vc,
                               "cannot start %s; missing %s. Those files ship "
                               "with the game and have to sit next to the "
                               "executable",
                               base, missing);
    }
    return -1;
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
 * Doom 3 paks; Vapor never runs the SafeDisc wrapper. The XP client cannot
 * fetch it (GitHub requires TLS 1.2). */
#if defined(_WIN32) && !defined(VAPOR_TARGET_XP)
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
#endif /* modern Windows: GitHub dhewm3 download */

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
    if (find_dhewm3(vc, rec, out, outsz) == 0) {
        return 0;
    }
#if !defined(_WIN32) || defined(VAPOR_TARGET_XP)
    /* The XP client cannot fetch the GitHub release: that host requires
     * TLS 1.2, and XP SChannel stops at TLS 1.0. */
    vapor_client_set_error(vc,
                           "this Doom 3 install needs dhewm3 (Windows blocked "
                           "SafeDisc). Install dhewm3 and try Play again");
    return -1;
#else
    char runtime[VAPOR_PATH_MAX];
    char zip_path[VAPOR_PATH_MAX];

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

static const char *
find_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    if (n == 0) {
        return hay;
    }
    for (; *hay; hay++) {
        size_t i;

        for (i = 0; i < n; i++) {
            unsigned char a = (unsigned char)hay[i];
            unsigned char b = (unsigned char)needle[i];

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
        if (i == n) {
            return hay;
        }
    }
    return NULL;
}

/* Next quoted value after `"key"`. Steam appmanifests are this shape. */
static int
acf_quoted(const char *text, const char *key, char *out, size_t outsz)
{
    char        pat[80];
    const char *p = text;
    int         n;

    n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat)) {
        return -1;
    }
    while ((p = strstr(p, pat)) != NULL) {
        size_t i = 0;

        p += (size_t)n;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p != '"') {
            continue;
        }
        p++;
        while (*p && *p != '"' && i + 1 < outsz) {
            out[i++] = *p++;
        }
        if (*p != '"') {
            return -1;
        }
        out[i] = '\0';
        return out[0] ? 0 : -1;
    }
    return -1;
}

static int
all_digits(const char *s)
{
    if (!s || !s[0]) {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    char steamapps[VAPOR_PATH_MAX];
    char installdir[VAPOR_PATH_MAX];
    char appid[32];
    char launcher[VAPOR_PATH_MAX];
} steam_lookup;

static void
unescape_backslashes(char *s)
{
    char *r = s;
    char *w = s;

    while (*r) {
        if (r[0] == '\\' && r[1] == '\\') {
            *w++ = '\\';
            r += 2;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static int
steam_manifest_cb(const char *name, void *ud)
{
    steam_lookup *s = ud;
    char          path[VAPOR_PATH_MAX];
    char          dir[VAPOR_PATH_MAX];
    char          id[32];
    char         *text;
    size_t        nlen;

    nlen = strlen(name);
    if (nlen < strlen("appmanifest_.acf") || !vapor_str_has_prefix(name, "appmanifest_")
        || !vapor_str_ends_with_ci(name, ".acf")) {
        return 0;
    }
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", s->steamapps, name)
        >= sizeof(path)) {
        return 0;
    }
    vapor_plat_native_path(path);
    text = vapor_read_file(path, NULL);
    if (!text) {
        return 0;
    }
    if (acf_quoted(text, "installdir", dir, sizeof(dir)) == 0
        && vapor_str_eq_ci(dir, s->installdir)
        && acf_quoted(text, "appid", id, sizeof(id)) == 0 && all_digits(id)) {
        snprintf(s->appid, sizeof(s->appid), "%s", id);
        if (acf_quoted(text, "LauncherPath", s->launcher, sizeof(s->launcher))
            == 0) {
            unescape_backslashes(s->launcher);
            vapor_plat_native_path(s->launcher);
        }
        free(text);
        return 1;
    }
    free(text);
    return 0;
}

/* 0 if `out_appid` is the Steam id for an exe under steamapps/common/<dir>.
 * `out_launcher` receives steam.exe when the manifest names it. */
static int
steam_appid_for_exe(const char *exe, char *out_appid, size_t outsz,
                    char *out_launcher, size_t launchersz)
{
    static const char MARK[] = "/steamapps/common/";
    steam_lookup      look;
    char              norm[VAPOR_PATH_MAX];
    const char       *mark;
    const char       *start;
    const char       *slash;
    size_t            n;
    int               rc;

    if ((size_t)snprintf(norm, sizeof(norm), "%s", exe) >= sizeof(norm)) {
        return -1;
    }
    for (n = 0; norm[n]; n++) {
        if (norm[n] == '\\') {
            norm[n] = '/';
        }
    }
    mark = find_ci(norm, MARK);
    if (!mark) {
        return -1;
    }
    start = mark + strlen(MARK);
    slash = strchr(start, '/');
    if (!slash || slash == start) {
        return -1;
    }
    memset(&look, 0, sizeof(look));
    n = (size_t)(slash - start);
    if (n >= sizeof(look.installdir)) {
        return -1;
    }
    memcpy(look.installdir, start, n);
    look.installdir[n] = '\0';
    /* mark points at "/steamapps/..."; keep the steamapps directory itself. */
    n = (size_t)(mark - norm) + strlen("/steamapps");
    if (n >= sizeof(look.steamapps)) {
        return -1;
    }
    memcpy(look.steamapps, norm, n);
    look.steamapps[n] = '\0';
    vapor_plat_native_path(look.steamapps);

    rc = vapor_plat_list_dir(look.steamapps, steam_manifest_cb, &look);
    if (rc < 0 || !look.appid[0]) {
        return -1;
    }
    if ((size_t)snprintf(out_appid, outsz, "%s", look.appid) >= outsz) {
        return -1;
    }
    if (out_launcher && launchersz) {
        out_launcher[0] = '\0';
        if (look.launcher[0]
            && (size_t)snprintf(out_launcher, launchersz, "%s", look.launcher)
                   >= launchersz) {
            out_launcher[0] = '\0';
        }
    }
    return 0;
}

/* A 32-bit host cannot start a PE32+ image. Older manifests label every
 * Windows target x86_64, so this is what tells Play apart from a missing file. */
static int
refuse_64bit_exe(vapor_client *vc, const char *path)
{
    const char *pe;

    if (!vapor_str_eq_ci(vapor_host_arch(), "x86")) {
        return 0;
    }
    pe = vapor_pe_arch_file(path);
    if (pe && strcmp(pe, "x86_64") == 0) {
        vapor_client_set_error(vc,
                               "%s is a 64-bit program and cannot run on "
                               "32-bit Windows",
                               path);
        return -1;
    }
    return 0;
}

/* The user pointed at this program. Do not read a manifest, do not rewrite
 * the directory, and do not swap in a source port. Returns 1 when Steam
 * starts the game and this process does not wait for it. */
static int
launch_external(vapor_client *vc, const vapor_install *rec, int *out_exit)
{
    char    exec_path[VAPOR_PATH_MAX];
    char    cwd_path[VAPOR_PATH_MAX];
    char  **argv = NULL;
    int64_t started, elapsed;
    int     exit_code = -1, rc = -1;

    if (!rec->launch_exe[0]) {
        vapor_client_set_error(vc, "%s has no program to launch",
                               rec->name[0] ? rec->name : rec->game_id);
        return -1;
    }
    snprintf(exec_path, sizeof(exec_path), "%s", rec->launch_exe);
    vapor_plat_native_path(exec_path);

    /* Starting the exe ourselves pops Steam's "please launch this from the
     * Steam client" dialog. Ask Steam to start that install instead. */
    {
        char appid[32];
        char launcher[VAPOR_PATH_MAX];
        char params[64];
        char url[64];
        int  handed = 0;

        if (steam_appid_for_exe(exec_path, appid, sizeof(appid), launcher,
                                sizeof(launcher))
            == 0) {
            if (!launcher[0] || !vapor_plat_exists(launcher)) {
                launcher[0] = '\0';
                if (vapor_plat_search_path("steam.exe", launcher, sizeof(launcher))
                        != 0
                    && vapor_plat_search_path("steam", launcher, sizeof(launcher))
                           != 0) {
                    launcher[0] = '\0';
                }
            }
            printf("launching %s through Steam (app %s)\n",
                   rec->name[0] ? rec->name : rec->game_id, appid);
            fflush(stdout);
            /* The raw exe only shows Steam's "launch this from the Steam
             * client" dialog. steam.exe -applaunch is the shortcut Steam
             * itself uses; the steam:// URL is the fallback. */
            if (launcher[0]
                && (size_t)snprintf(params, sizeof(params), "-applaunch %s",
                                    appid)
                       < sizeof(params)
                && vapor_plat_start(launcher, params) == 0) {
                handed = 1;
            }
            if (!handed
                && (size_t)snprintf(url, sizeof(url), "steam://rungameid/%s",
                                    appid)
                       < sizeof(url)
                && vapor_plat_open_url(url) == 0) {
                handed = 1;
            }
            if (!handed) {
                vapor_client_set_error(vc,
                                       "could not ask Steam to start %s. Open "
                                       "the Steam client and try Play again",
                                       rec->name[0] ? rec->name : rec->game_id);
                return -1;
            }
            if (out_exit) {
                *out_exit = 0;
            }
            return 1;
        }
    }

    if (!vapor_plat_exists(exec_path) || vapor_plat_is_dir(exec_path)) {
        vapor_client_set_error(vc,
                               "cannot start %s; %s is not there. Vapor did not "
                               "move it — the shortcut only remembers the path",
                               rec->name[0] ? rec->name : rec->game_id, exec_path);
        return -1;
    }
    if (vapor_exe_is_copy_protected(exec_path)) {
        vapor_client_set_error(vc,
                               "this executable uses SafeDisc (SECDRV), which "
                               "Windows blocked. The administrator dialog is "
                               "that DRM, not a login. Point the shortcut at "
                               "the publisher's patch or a source-port exe");
        return -1;
    }
    if (rec->install_dir[0] && vapor_plat_is_dir(rec->install_dir)) {
        snprintf(cwd_path, sizeof(cwd_path), "%s", rec->install_dir);
    } else {
        parent_dir(exec_path, cwd_path, sizeof(cwd_path));
    }
    vapor_plat_native_path(cwd_path);
    if (refuse_missing_dlls(vc, exec_path) != 0) {
        return -1;
    }

    argv = (char **)calloc(2, sizeof(*argv));
    if (!argv) {
        vapor_client_set_error(vc, "out of memory");
        return -1;
    }
    argv[0] = vapor_strdup(exec_path);
    if (!argv[0]) {
        vapor_client_set_error(vc, "out of memory");
        free(argv);
        return -1;
    }

    printf("launching %s\n", rec->name[0] ? rec->name : rec->game_id);
    fflush(stdout);

    if (refuse_64bit_exe(vc, exec_path) != 0) {
        goto cleanup;
    }
    started = vapor_now_unix();
    if (vapor_plat_run(exec_path, argv, cwd_path, NULL, 0, &exit_code) != 0) {
        vapor_client_set_error(vc, "could not start %s", exec_path);
        goto cleanup;
    }
    elapsed = vapor_now_unix() - started;
    if (elapsed < 0) {
        elapsed = 0;
    }
    vapor_db_add_playtime(vc, rec->game_id, started, elapsed);
    {
        char pretty[64];
        vapor_format_duration(elapsed, pretty, sizeof(pretty));
        if (exit_code == 0) {
            printf("%s exited normally after %s\n",
                   rec->name[0] ? rec->name : rec->game_id, pretty);
        } else {
            printf("%s exited with code %d after %s\n",
                   rec->name[0] ? rec->name : rec->game_id, exit_code, pretty);
        }
    }
    if (out_exit) {
        *out_exit = exit_code;
    }
    rc = 0;

cleanup:
    free(argv[0]);
    free(argv);
    return rc;
}

static int
resolve_dosbox(vapor_client *vc, char *out, size_t outsz)
{
#if !defined(_WIN32)
    (void)out;
    (void)outsz;
    vapor_client_set_error(vc, "DOSBox launch on Linux is not available yet");
    return -1;
#else
    static const char *const known[] = {
        "C:\\Program Files\\DOSBox Staging\\dosbox.exe",
        "C:\\Program Files (x86)\\DOSBox Staging\\dosbox.exe",
        "C:\\Program Files\\DOSBox\\DOSBox.exe",
        NULL
    };
    size_t i;

    if (vc->cfg.dosbox_path[0]) {
        if (!vapor_plat_exists(vc->cfg.dosbox_path)) {
            vapor_client_set_error(vc, "dosbox_path does not exist: %s",
                                   vc->cfg.dosbox_path);
            return -1;
        }
        if ((size_t)snprintf(out, outsz, "%s", vc->cfg.dosbox_path) >= outsz) {
            vapor_client_set_error(vc, "dosbox_path is too long");
            return -1;
        }
        return 0;
    }
    if (vapor_plat_search_path("dosbox.exe", out, outsz) == 0
        || vapor_plat_search_path("dosbox", out, outsz) == 0) {
        return 0;
    }
    for (i = 0; known[i]; i++) {
        if (vapor_plat_exists(known[i])
            && (size_t)snprintf(out, outsz, "%s", known[i]) < outsz) {
            return 0;
        }
    }
    {
        const char *local_app = getenv("LOCALAPPDATA");
        char        local[VAPOR_PATH_MAX];

        if (local_app
            && (size_t)snprintf(local, sizeof(local),
                                "%s\\DOSBox Staging\\dosbox.exe", local_app)
                   < sizeof(local)
            && vapor_plat_exists(local)) {
            snprintf(out, outsz, "%s", local);
            return 0;
        }
    }
    vapor_client_set_error(vc,
                           "DOSBox was not found. Install DOSBox Staging, or "
                           "set dosbox_path in the client config "
                           "(vapor config dosbox PATH)");
    return -1;
#endif
}

static void
to_dos_path(char *s)
{
    for (; *s; s++) {
        if (*s == '/') {
            *s = '\\';
        }
    }
}

static int
write_dosbox_conf(vapor_client *vc, const char *install_dir, const char *exec_rel,
                  const char *cwd, char *conf_out, size_t confsz)
{
    char  dir[VAPOR_PATH_MAX];
    char  mounted[VAPOR_PATH_MAX];
    char  exec_dos[VAPOR_PATH_MAX];
    char  cwd_dos[VAPOR_PATH_MAX];
    FILE *f;
    int   have_cwd = 0;

    if ((size_t)snprintf(dir, sizeof(dir), "%s/.vapor", install_dir) >= sizeof(dir)
        || (size_t)snprintf(conf_out, confsz, "%s/dosbox.conf", dir) >= confsz) {
        vapor_client_set_error(vc, "DOSBox config path is too long");
        return -1;
    }
    if (vapor_plat_mkdirs(dir) != 0) {
        vapor_client_set_error(vc, "cannot create %s", dir);
        return -1;
    }
    if ((size_t)snprintf(mounted, sizeof(mounted), "%s", install_dir) >= sizeof(mounted)
        || (size_t)snprintf(exec_dos, sizeof(exec_dos), "%s", exec_rel ? exec_rel : "")
               >= sizeof(exec_dos)) {
        vapor_client_set_error(vc, "DOSBox launch path is too long");
        return -1;
    }
    vapor_plat_native_path(mounted);
    to_dos_path(exec_dos);
    if (strchr(mounted, '"') || strchr(exec_dos, '"')) {
        vapor_client_set_error(vc, "DOSBox cannot mount a path that contains a quote");
        return -1;
    }
    cwd_dos[0] = '\0';
    if (cwd && cwd[0] && strcmp(cwd, ".") != 0) {
        if ((size_t)snprintf(cwd_dos, sizeof(cwd_dos), "%s", cwd) >= sizeof(cwd_dos)) {
            vapor_client_set_error(vc, "DOSBox working directory is too long");
            return -1;
        }
        to_dos_path(cwd_dos);
        if (strchr(cwd_dos, '"')) {
            vapor_client_set_error(vc, "DOSBox cannot cd to a path that contains a quote");
            return -1;
        }
        have_cwd = 1;
    }
    vapor_plat_native_path(conf_out);
    f = fopen(conf_out, "w");
    if (!f) {
        vapor_client_set_error(vc, "cannot write %s", conf_out);
        return -1;
    }
    fprintf(f, "[autoexec]\n");
    fprintf(f, "mount C \"%s\"\n", mounted);
    fprintf(f, "C:\n");
    if (have_cwd) {
        fprintf(f, "cd %s\n", cwd_dos);
    }
    fprintf(f, "%s\n", exec_dos);
    fprintf(f, "exit\n");
    if (fclose(f) != 0) {
        vapor_client_set_error(vc, "cannot write %s", conf_out);
        return -1;
    }
    return 0;
}

int
vapor_launch_game(vapor_client *vc, const char *game_id, int *out_exit)
{
    vapor_manifest      m;
    vapor_install       rec;
    const vapor_target *t;
    char                install_dir[VAPOR_PATH_MAX];
    char                exec_path[VAPOR_PATH_MAX];
    char                launch_rel[VAPOR_PATH_MAX] = "";
    char                cwd_path[VAPOR_PATH_MAX];
    char              **argv = NULL;
    vapor_kv           *env = NULL;
    size_t              nenv = 0, i, argv_n = 0;
    int64_t             started, elapsed;
    int                 exit_code = -1, rc = -1;
    int                 use_dhewm3 = 0;
    int                 use_dosbox = 0;
    char                dosbox_exe[VAPOR_PATH_MAX];
    char                conf_path[VAPOR_PATH_MAX];

    if (vapor_db_get_install(vc, game_id, &rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed; run \"vapor install %s\"",
                               game_id, game_id);
        return -1;
    }
    if (vapor_install_is_external(&rec)) {
        return launch_external(vc, &rec, out_exit);
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
        if (strcmp(t->runtime, "dosbox") != 0) {
            /* The schema allows later runtimes such as "proton"/"wine".
             * Naming one stores it; Play says which launcher is missing. */
            vapor_client_set_error(vc,
                                   "%s needs the \"%s\" runtime, which this build "
                                   "does not support yet", game_id, t->runtime);
            vapor_manifest_free(&m);
            return -1;
        }
        use_dosbox = 1;
    }
    if (use_dosbox && m.install_mode && strcmp(m.install_mode, "keep_disc") == 0) {
        vapor_client_set_error(vc,
                               "%s keeps its disc images, and CD mount is not "
                               "available yet",
                               rec.name[0] ? rec.name : game_id);
        vapor_manifest_free(&m);
        return -1;
    }
    if (use_dosbox && strcmp(vapor_host_platform(), "windows") != 0) {
        vapor_client_set_error(vc, "DOSBox launch on Linux is not available yet");
        vapor_manifest_free(&m);
        return -1;
    }
    {
        const char *rel = t->exec ? t->exec : "";
        const char *base;
        char        profile_path[VAPOR_PATH_MAX];

        if (rec.profile_launch[0]
            && (size_t)snprintf(profile_path, sizeof(profile_path), "%s/%s",
                                install_dir, rec.profile_launch)
                   < sizeof(profile_path)) {
            vapor_plat_native_path(profile_path);
            if (vapor_plat_exists(profile_path)
                && !vapor_plat_is_dir(profile_path)) {
                rel = rec.profile_launch;
            }
        }
        base = strrchr(rel, '/');

        base = base ? base + 1 : rel;
        snprintf(launch_rel, sizeof(launch_rel), "%s", rel);
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
            snprintf(rec.setup_state, sizeof(rec.setup_state), "%s",
                     VAPOR_SETUP_PENDING);
            if (!rec.install_kind[0]) {
                snprintf(rec.install_kind, sizeof(rec.install_kind), "%s",
                         VAPOR_INSTALL_KIND_OS_PRODUCT);
            }
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
                         launch_rel)
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
                               launch_rel, game_id);
        vapor_manifest_free(&m);
        return -1;
    }

    if (!use_dosbox && vapor_exe_is_copy_protected(exec_path)) {
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
        } else if (looks_doom3_tree(install_dir, game_id, launch_rel)) {
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

    if (!use_dosbox && !use_dhewm3 && refuse_missing_dlls(vc, exec_path) != 0) {
        vapor_manifest_free(&m);
        return -1;
    }

    if (use_dosbox) {
        if (resolve_dosbox(vc, dosbox_exe, sizeof(dosbox_exe)) != 0) {
            goto cleanup;
        }
        if (write_dosbox_conf(vc, install_dir, launch_rel, t->cwd, conf_path,
                              sizeof(conf_path))
            != 0) {
            goto cleanup;
        }
    }
    if (vapor_saves_before_play(vc, &m, install_dir) != 0) {
        goto cleanup;
    }

    argv_n = use_dosbox ? 4 : (use_dhewm3 ? 3 : t->nargs);
    argv = (char **)calloc(argv_n + 2, sizeof(*argv));
    if (!argv) {
        vapor_client_set_error(vc, "out of memory");
        vapor_manifest_free(&m);
        return -1;
    }
    argv[0] = vapor_strdup(use_dosbox ? dosbox_exe : exec_path);
    if (!argv[0]) {
        goto cleanup;
    }
    if (use_dosbox) {
        argv[1] = vapor_strdup("-conf");
        argv[2] = vapor_strdup(conf_path);
        argv[3] = vapor_strdup("-noconsole");
        argv[4] = vapor_strdup("-exit");
        if (!argv[1] || !argv[2] || !argv[3] || !argv[4]) {
            vapor_client_set_error(vc, "out of memory");
            goto cleanup;
        }
    } else if (use_dhewm3) {
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

    if (!use_dosbox && t->nenv > 0) {
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
    if (use_dosbox) {
        printf("using DOSBox %s\n", dosbox_exe);
    }
    /* Flushed before spawning: the child writes straight to the fd, so leaving
     * our own output buffered would interleave it out of order. */
    fflush(stdout);

    if (refuse_64bit_exe(vc, use_dosbox ? dosbox_exe : exec_path) != 0) {
        goto cleanup;
    }
    started = vapor_now_unix();
    if (vapor_plat_run(use_dosbox ? dosbox_exe : exec_path, argv, cwd_path, env,
                       nenv, &exit_code)
        != 0) {
        vapor_client_set_error(vc, "could not start %s",
                               use_dosbox ? dosbox_exe : exec_path);
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
    rc = vapor_saves_after_play(vc, &m, install_dir) == 0 ? 0 : -1;

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
