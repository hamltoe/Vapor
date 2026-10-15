/* Download, apply, and remove community mods.
 *
 * The downloaded zip lives under <library>/.vapor/mods/<game>/<mod>/ so a game
 * update can replace the install tree without deleting it. Inside the install,
 * .vapor/vanilla holds the original bytes of replaced files and
 * .vapor/mod-applied.json lists every path a mod wrote. Enabling, disabling,
 * or restoring always puts those files back first, then reapplies whatever is
 * still enabled. */

#include "vapor/client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "json.h"
#include "miniz.h"
#include "platform.h"
#include "vapor/mods.h"
#include "vapor/sha256.h"

#define APPLIED_NAME "mod-applied.json"

static int
join2(char *out, size_t outsz, const char *a, const char *b)
{
    int n = snprintf(out, outsz, "%s/%s", a, b);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

static int
cache_dir(vapor_client *vc, const char *game_id, const char *mod_id, char *out,
          size_t outsz)
{
    char mid[VAPOR_PATH_MAX];

    if (join2(mid, sizeof(mid), ".vapor/mods", game_id) != 0) {
        return -1;
    }
    if (mod_id && mod_id[0]) {
        char leaf[VAPOR_PATH_MAX];
        if (join2(leaf, sizeof(leaf), mid, mod_id) != 0
            || join2(out, outsz, vc->cfg.library_dir, leaf) != 0) {
            return -1;
        }
    } else if (join2(out, outsz, vc->cfg.library_dir, mid) != 0) {
        return -1;
    }
    return 0;
}

static int
play_dir_of(const vapor_install *rec, char *out, size_t outsz)
{
    const char *dir = rec->install_dir;

    if (strcmp(rec->install_kind, VAPOR_INSTALL_KIND_OS_PRODUCT) == 0
        && rec->product_dir[0]) {
        dir = rec->product_dir;
    }
    if (!dir || !dir[0] || (size_t)snprintf(out, outsz, "%s", dir) >= outsz) {
        return -1;
    }
    return 0;
}

static int
require_playable(vapor_client *vc, const char *game_id, vapor_install *rec,
                 char *play, size_t playsz)
{
    if (vapor_db_get_install(vc, game_id, rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed", game_id);
        return -1;
    }
    if (vapor_install_is_external(rec)) {
        vapor_client_set_error(vc, "mods do not apply to a local shortcut");
        return -1;
    }
    if (rec->setup_pending) {
        vapor_client_set_error(vc, "%s still needs setup before mods can be applied",
                               game_id);
        return -1;
    }
    if (play_dir_of(rec, play, playsz) != 0 || !vapor_plat_is_dir(play)) {
        vapor_client_set_error(vc, "the install directory for %s is missing",
                               game_id);
        return -1;
    }
    return 0;
}

static int
db_exec(vapor_client *vc, sqlite3_stmt *st)
{
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    return 0;
}

static int
state_upsert(vapor_client *vc, const char *game_id, const char *mod_id,
             const char *version, const char *name, int enabled, int sort_order)
{
    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(vc->db,
                           "INSERT INTO mod_state"
                           " (game_id, mod_id, version, name, enabled, sort_order)"
                           " VALUES (?, ?, ?, ?, ?, ?)"
                           " ON CONFLICT(game_id, mod_id) DO UPDATE SET"
                           "   version = excluded.version,"
                           "   name = excluded.name,"
                           "   enabled = excluded.enabled,"
                           "   sort_order = excluded.sort_order",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, mod_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, version, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, name ? name : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 5, enabled ? 1 : 0);
    sqlite3_bind_int(st, 6, sort_order);
    return db_exec(vc, st);
}

static int
next_sort(vapor_client *vc, const char *game_id, const char *mod_id, int *out)
{
    sqlite3_stmt *st = NULL;
    int           existing = 0;

    if (sqlite3_prepare_v2(vc->db,
                           "SELECT sort_order FROM mod_state"
                           " WHERE game_id = ? AND mod_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, mod_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out = sqlite3_column_int(st, 0);
        existing = 1;
    }
    sqlite3_finalize(st);
    if (existing) {
        return 0;
    }
    if (sqlite3_prepare_v2(vc->db,
                           "SELECT COALESCE(MAX(sort_order), 0) FROM mod_state"
                           " WHERE game_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    *out = sqlite3_column_int(st, 0) + 1;
    sqlite3_finalize(st);
    return 0;
}

static int
extract_mod_zip(vapor_client *vc, const char *zip_path, const char *dest)
{
    mz_zip_archive zip;
    mz_uint        i, count;

    memset(&zip, 0, sizeof(zip));
    if (vapor_plat_mkdirs(dest) != 0) {
        vapor_client_set_error(vc, "cannot create %s", dest);
        return -1;
    }
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        vapor_client_set_error(vc, "%s is not a readable zip archive", zip_path);
        return -1;
    }
    count = mz_zip_reader_get_num_files(&zip);
    for (i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        char                     rel[VAPOR_PATH_MAX];
        char                     path[VAPOR_PATH_MAX];

        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            vapor_client_set_error(vc, "cannot read entry %u of %s", (unsigned)i,
                                   zip_path);
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }
        if (vapor_mod_rel_ok(st.m_filename, rel, sizeof(rel)) != 0) {
            vapor_client_set_error(vc,
                                   "archive entry \"%s\" tries to escape the "
                                   "install directory; refusing to extract",
                                   st.m_filename);
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (join2(path, sizeof(path), dest, rel) != 0) {
            vapor_client_set_error(vc, "mod path is too long for \"%s\"", rel);
            mz_zip_reader_end(&zip);
            return -1;
        }
        vapor_plat_native_path(path);
        {
            char parent[VAPOR_PATH_MAX];
            char *slash;

            snprintf(parent, sizeof(parent), "%s", path);
            slash = strrchr(parent, '/');
#if defined(_WIN32)
            {
                char *bs = strrchr(parent, '\\');
                if (bs > slash) {
                    slash = bs;
                }
            }
#endif
            if (slash) {
                *slash = '\0';
                if (vapor_plat_mkdirs(parent) != 0) {
                    vapor_client_set_error(vc, "cannot create %s", parent);
                    mz_zip_reader_end(&zip);
                    return -1;
                }
            }
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, path, 0)) {
            vapor_client_set_error(vc, "cannot write %s", path);
            mz_zip_reader_end(&zip);
            return -1;
        }
    }
    mz_zip_reader_end(&zip);
    return 0;
}

static int
read_applied(const char *play, char **out)
{
    char   path[VAPOR_PATH_MAX];
    size_t len = 0;

    *out = NULL;
    if (join2(path, sizeof(path), play, ".vapor/" APPLIED_NAME) != 0) {
        return -1;
    }
    vapor_plat_native_path(path);
    if (!vapor_plat_exists(path)) {
        return 0;
    }
    *out = vapor_read_file(path, &len);
    return *out ? 0 : -1;
}

static int
write_applied(vapor_client *vc, const char *play, cJSON *files)
{
    char  path[VAPOR_PATH_MAX];
    char  dir[VAPOR_PATH_MAX];
    cJSON *root;
    char  *text;
    FILE  *f;
    int    n;

    if (join2(dir, sizeof(dir), play, ".vapor") != 0
        || join2(path, sizeof(path), dir, APPLIED_NAME) != 0) {
        vapor_client_set_error(vc, "mod state path is too long");
        return -1;
    }
    vapor_plat_native_path(dir);
    vapor_plat_native_path(path);
    if (!files || cJSON_GetArraySize(files) == 0) {
        remove(path);
        cJSON_Delete(files);
        return 0;
    }
    if (vapor_plat_mkdirs(dir) != 0) {
        cJSON_Delete(files);
        vapor_client_set_error(vc, "cannot create %s", dir);
        return -1;
    }
    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(files);
        vapor_client_set_error(vc, "out of memory");
        return -1;
    }
    cJSON_AddItemToObject(root, "files", files);
    text = cJSON_PrintUnformatted(root);
    /* files is owned by root now; the caller must not free it. */
    cJSON_Delete(root);
    if (!text) {
        vapor_client_set_error(vc, "out of memory");
        return -1;
    }
    f = fopen(path, "wb");
    if (!f) {
        free(text);
        vapor_client_set_error(vc, "cannot write %s", path);
        return -1;
    }
    n = (int)fwrite(text, 1, strlen(text), f);
    fclose(f);
    free(text);
    if (n < 0) {
        vapor_client_set_error(vc, "cannot write %s", path);
        return -1;
    }
    return 0;
}

static int
restore_play(vapor_client *vc, const char *play)
{
    char  *applied = NULL;
    char   vanilla[VAPOR_PATH_MAX];
    char   err[256];

    if (read_applied(play, &applied) != 0) {
        vapor_client_set_error(vc, "cannot read the mod apply log");
        return -1;
    }
    if (join2(vanilla, sizeof(vanilla), play, ".vapor/vanilla") != 0) {
        free(applied);
        vapor_client_set_error(vc, "vanilla path is too long");
        return -1;
    }
    if (vapor_mod_revert(play, vanilla, applied, err, sizeof(err)) != 0) {
        free(applied);
        vapor_client_set_error(vc, "%s", err[0] ? err : "cannot restore the game");
        return -1;
    }
    free(applied);
    vapor_plat_native_path(vanilla);
    if (vapor_mod_remove_tree(vanilla) != 0) {
        vapor_client_set_error(vc, "cannot remove vanilla backups");
        return -1;
    }
    {
        char path[VAPOR_PATH_MAX];
        if (join2(path, sizeof(path), play, ".vapor/" APPLIED_NAME) == 0) {
            vapor_plat_native_path(path);
            remove(path);
        }
    }
    return 0;
}

static int
apply_exec_bits(const char *play, const vapor_mod_manifest *m)
{
    size_t i, count = 0;

    for (i = 0; i < m->nexec_bits; i++) {
        char path[VAPOR_PATH_MAX];

        if (!m->exec_bits[i]) {
            continue;
        }
        if (!strchr(m->exec_bits[i], '*') && !strchr(m->exec_bits[i], '?')) {
            if (join2(path, sizeof(path), play, m->exec_bits[i]) != 0) {
                continue;
            }
            vapor_plat_native_path(path);
            (void)vapor_plat_make_executable(path);
            continue;
        }
        if (vapor_plat_chmod_matching(play, m->exec_bits[i], &count) != 0) {
            return -1;
        }
    }
    if (m->exec && m->exec[0]) {
        char path[VAPOR_PATH_MAX];
        if (join2(path, sizeof(path), play, m->exec) == 0) {
            vapor_plat_native_path(path);
            (void)vapor_plat_make_executable(path);
        }
    }
    return 0;
}

static int
load_cached_manifest(vapor_client *vc, const char *game_id, const char *mod_id,
                     vapor_mod_manifest *out)
{
    char   dir[VAPOR_PATH_MAX];
    char   path[VAPOR_PATH_MAX];
    char  *text;
    size_t len = 0;
    char   err[256];

    if (cache_dir(vc, game_id, mod_id, dir, sizeof(dir)) != 0
        || join2(path, sizeof(path), dir, "manifest.json") != 0) {
        vapor_client_set_error(vc, "mod manifest path is too long");
        return -1;
    }
    vapor_plat_native_path(path);
    text = vapor_read_file(path, &len);
    if (!text) {
        vapor_client_set_error(vc, "mod \"%s\" has no local manifest", mod_id);
        return -1;
    }
    if (vapor_mod_manifest_parse(text, len, out, err, sizeof(err)) != 0) {
        free(text);
        vapor_client_set_error(vc, "mod \"%s\" manifest is unusable: %s", mod_id,
                               err);
        return -1;
    }
    free(text);
    return 0;
}

/* The apply log is still in memory. Put those files back and drop vanilla/. */
static void
rollback_apply(const char *play, const char *vanilla, cJSON *files)
{
    char  *text = NULL;
    cJSON *root = cJSON_CreateObject();
    char   err[256];
    char   vdir[VAPOR_PATH_MAX];

    err[0] = '\0';
    if (root) {
        cJSON *copy = cJSON_Duplicate(files, 1);

        if (copy) {
            cJSON_AddItemToObject(root, "files", copy);
            text = cJSON_PrintUnformatted(root);
        }
        cJSON_Delete(root);
    }
    (void)vapor_mod_revert(play, vanilla, text, err, sizeof(err));
    free(text);
    snprintf(vdir, sizeof(vdir), "%s", vanilla);
    vapor_plat_native_path(vdir);
    (void)vapor_mod_remove_tree(vdir);
}

/* Puts the install back, then copies every enabled mod on top. */
static int
rebuild(vapor_client *vc, const char *game_id, const char *play)
{
    sqlite3_stmt       *st = NULL;
    cJSON              *files = NULL;
    char                err[256];
    char                vanilla[VAPOR_PATH_MAX];
    typedef struct {
        char id[VAPOR_ID_MAX + 1];
    } idrow;
    idrow *ids = NULL;
    size_t n = 0, cap = 0, i;

    if (restore_play(vc, play) != 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(vc->db,
                           "SELECT mod_id FROM mod_state"
                           " WHERE game_id = ? AND enabled != 0"
                           " ORDER BY sort_order, mod_id",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(st, 0);
        if (!id) {
            continue;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            idrow *grown = (idrow *)realloc(ids, ncap * sizeof(*grown));
            if (!grown) {
                sqlite3_finalize(st);
                free(ids);
                vapor_client_set_error(vc, "out of memory");
                return -1;
            }
            ids = grown;
            cap = ncap;
        }
        snprintf(ids[n].id, sizeof(ids[n].id), "%s", id);
        n++;
    }
    sqlite3_finalize(st);

    files = cJSON_CreateArray();
    if (!files) {
        free(ids);
        vapor_client_set_error(vc, "out of memory");
        return -1;
    }
    if (join2(vanilla, sizeof(vanilla), play, ".vapor/vanilla") != 0) {
        cJSON_Delete(files);
        free(ids);
        vapor_client_set_error(vc, "vanilla path is too long");
        return -1;
    }
    for (i = 0; i < n; i++) {
        char                dir[VAPOR_PATH_MAX];
        char                zip[VAPOR_PATH_MAX];
        char                stage[VAPOR_PATH_MAX];
        vapor_mod_manifest  mm;

        if (cache_dir(vc, game_id, ids[i].id, dir, sizeof(dir)) != 0
            || join2(zip, sizeof(zip), dir, "package.zip") != 0
            || join2(stage, sizeof(stage), dir, "stage") != 0) {
            cJSON_Delete(files);
            free(ids);
            vapor_client_set_error(vc, "mod path is too long");
            return -1;
        }
        vapor_plat_native_path(zip);
        vapor_plat_native_path(stage);
        (void)vapor_mod_remove_tree(stage);
        if (extract_mod_zip(vc, zip, stage) != 0
            || vapor_mod_apply_tree(play, stage, vanilla, files, err, sizeof(err))
                   != 0) {
            if (err[0]) {
                vapor_client_set_error(vc, "%s", err);
            }
            rollback_apply(play, vanilla, files);
            (void)vapor_mod_remove_tree(stage);
            cJSON_Delete(files);
            free(ids);
            return -1;
        }
        (void)vapor_mod_remove_tree(stage);
        if (load_cached_manifest(vc, game_id, ids[i].id, &mm) == 0) {
            if (apply_exec_bits(play, &mm) != 0) {
                char bad[VAPOR_ID_MAX + 1];

                snprintf(bad, sizeof(bad), "%s", ids[i].id);
                vapor_mod_manifest_free(&mm);
                rollback_apply(play, vanilla, files);
                cJSON_Delete(files);
                free(ids);
                vapor_client_set_error(vc, "cannot apply exec bits for mod \"%s\"",
                                       bad);
                return -1;
            }
            vapor_mod_manifest_free(&mm);
        }
    }
    free(ids);
    {
        cJSON *copy = cJSON_Duplicate(files, 1);

        if (write_applied(vc, play, files) != 0) {
            if (copy) {
                rollback_apply(play, vanilla, copy);
                cJSON_Delete(copy);
            }
            return -1;
        }
        cJSON_Delete(copy);
    }
    return 0;
}

int
vapor_mods_fetch(vapor_client *vc, const char *game_id, vapor_mod_entry **out,
                 size_t *count)
{
    vapor_response   resp;
    cJSON           *root, *arr, *it;
    vapor_mod_entry *rows = NULL;
    size_t           n = 0, cap = 0;
    char             path[256];

    *out = NULL;
    *count = 0;
    if (!game_id || !vapor_id_is_valid(game_id)) {
        vapor_client_set_error(vc, "bad game id");
        return -1;
    }
    snprintf(path, sizeof(path), "/api/v1/games/%s/mods", game_id);
    memset(&resp, 0, sizeof(resp));
    if (vapor_api_get(vc, path, 1, &resp) != 0) {
        return -1;
    }
    root = vapor_json_parse_response(&resp);
    vapor_response_free(&resp);
    if (!root) {
        vapor_client_set_error(vc, "mod list was not JSON");
        return -1;
    }
    arr = cJSON_GetObjectItemCaseSensitive(root, "mods");
    if (arr && cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            vapor_mod_entry *e;
            if (n == cap) {
                size_t ncap = cap ? cap * 2 : 8;
                vapor_mod_entry *grown =
                    (vapor_mod_entry *)realloc(rows, ncap * sizeof(*grown));
                if (!grown) {
                    free(rows);
                    cJSON_Delete(root);
                    vapor_client_set_error(vc, "out of memory");
                    return -1;
                }
                rows = grown;
                cap = ncap;
            }
            e = &rows[n];
            memset(e, 0, sizeof(*e));
            vapor_json_copy(e->id, sizeof(e->id), it, "id", "");
            vapor_json_copy(e->name, sizeof(e->name), it, "name", e->id);
            vapor_json_copy(e->description, sizeof(e->description), it,
                            "description", "");
            vapor_json_copy(e->latest_version, sizeof(e->latest_version), it,
                            "latest_version", "");
            e->size = (uint64_t)vapor_json_num(it, "size", 0);
            e->has_cover = vapor_json_bool(it, "has_cover", 0);
            if (e->id[0]) {
                n++;
            }
        }
    }
    cJSON_Delete(root);

    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(vc->db,
                               "SELECT mod_id, version, name, enabled, sort_order"
                               " FROM mod_state WHERE game_id = ?",
                               -1, &st, NULL)
            == SQLITE_OK) {
            sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *id = (const char *)sqlite3_column_text(st, 0);
                const char *ver = (const char *)sqlite3_column_text(st, 1);
                const char *name = (const char *)sqlite3_column_text(st, 2);
                size_t      i;
                vapor_mod_entry *e = NULL;
                char        dir[VAPOR_PATH_MAX];
                char        zip[VAPOR_PATH_MAX];

                if (!id) {
                    continue;
                }
                for (i = 0; i < n; i++) {
                    if (strcmp(rows[i].id, id) == 0) {
                        e = &rows[i];
                        break;
                    }
                }
                if (!e) {
                    if (n == cap) {
                        size_t ncap = cap ? cap * 2 : 8;
                        vapor_mod_entry *grown = (vapor_mod_entry *)realloc(
                            rows, ncap * sizeof(*grown));
                        if (!grown) {
                            sqlite3_finalize(st);
                            free(rows);
                            vapor_client_set_error(vc, "out of memory");
                            return -1;
                        }
                        rows = grown;
                        cap = ncap;
                    }
                    e = &rows[n++];
                    memset(e, 0, sizeof(*e));
                    snprintf(e->id, sizeof(e->id), "%s", id);
                    snprintf(e->name, sizeof(e->name), "%s",
                             name && name[0] ? name : id);
                }
                if (ver) {
                    snprintf(e->installed_version, sizeof(e->installed_version),
                             "%s", ver);
                }
                e->enabled = sqlite3_column_int(st, 3) ? 1 : 0;
                e->sort_order = sqlite3_column_int(st, 4);
                if (cache_dir(vc, game_id, id, dir, sizeof(dir)) == 0
                    && join2(zip, sizeof(zip), dir, "package.zip") == 0) {
                    vapor_plat_native_path(zip);
                    e->downloaded = vapor_plat_exists(zip) ? 1 : 0;
                }
            }
            sqlite3_finalize(st);
        }
    }
    *out = rows;
    *count = n;
    return 0;
}

static int
download_mod(vapor_client *vc, const char *game_id, const char *mod_id,
             vapor_mod_manifest *out)
{
    char             path[256];
    char             dir[VAPOR_PATH_MAX];
    char             zip[VAPOR_PATH_MAX];
    char             man_path[VAPOR_PATH_MAX];
    vapor_response   resp;
    char            *json;
    char             err[256];
    char             got[VAPOR_SHA256_HEX_LEN + 1];
    FILE            *f;

    snprintf(path, sizeof(path), "/api/v1/games/%s/mods/%s/manifest", game_id,
             mod_id);
    memset(&resp, 0, sizeof(resp));
    if (vapor_api_get(vc, path, 1, &resp) != 0) {
        return -1;
    }
    if (vapor_mod_manifest_parse(resp.body ? resp.body : "", resp.body_len, out,
                                 err, sizeof(err))
        != 0) {
        vapor_response_free(&resp);
        vapor_client_set_error(vc, "mod manifest: %s", err);
        return -1;
    }
    json = resp.body;
    resp.body = NULL;
    vapor_response_free(&resp);

    if (cache_dir(vc, game_id, mod_id, dir, sizeof(dir)) != 0
        || join2(zip, sizeof(zip), dir, "package.zip") != 0
        || join2(man_path, sizeof(man_path), dir, "manifest.json") != 0) {
        free(json);
        vapor_mod_manifest_free(out);
        vapor_client_set_error(vc, "mod path is too long");
        return -1;
    }
    vapor_plat_native_path(dir);
    vapor_plat_native_path(zip);
    vapor_plat_native_path(man_path);
    if (vapor_plat_mkdirs(dir) != 0) {
        free(json);
        vapor_mod_manifest_free(out);
        vapor_client_set_error(vc, "cannot create %s", dir);
        return -1;
    }
    f = fopen(man_path, "wb");
    if (!f) {
        free(json);
        vapor_mod_manifest_free(out);
        vapor_client_set_error(vc, "cannot write %s", man_path);
        return -1;
    }
    fwrite(json, 1, strlen(json), f);
    fclose(f);
    free(json);

    if (vapor_plat_exists(zip) && vapor_sha256_file(zip, got) == 0
        && vapor_str_eq_ci(got, out->package.sha256)) {
        return 0;
    }
    remove(zip);
    snprintf(path, sizeof(path), "/api/v1/download/%s/mods/%s/%s", game_id, mod_id,
             out->version);
    if (vapor_http_download(vc, path, zip, NULL, NULL) != 0) {
        vapor_mod_manifest_free(out);
        return -1;
    }
    if (vapor_sha256_file(zip, got) != 0 || !vapor_str_eq_ci(got, out->package.sha256)) {
        remove(zip);
        vapor_mod_manifest_free(out);
        vapor_client_set_error(vc, "checksum mismatch for mod \"%s\"", mod_id);
        return -1;
    }
    return 0;
}

int
vapor_mod_install(vapor_client *vc, const char *game_id, const char *mod_id)
{
    vapor_install      rec;
    vapor_mod_manifest mm;
    char               play[VAPOR_PATH_MAX];
    int                order = 1;

    if (!vapor_id_is_valid(game_id) || !vapor_id_is_valid(mod_id)) {
        vapor_client_set_error(vc, "bad id");
        return -1;
    }
    if (require_playable(vc, game_id, &rec, play, sizeof(play)) != 0) {
        return -1;
    }
    if (download_mod(vc, game_id, mod_id, &mm) != 0) {
        return -1;
    }
    if (next_sort(vc, game_id, mod_id, &order) != 0
        || state_upsert(vc, game_id, mod_id, mm.version, mm.name, 1, order)
               != 0) {
        vapor_mod_manifest_free(&mm);
        return -1;
    }
    vapor_mod_manifest_free(&mm);
    if (rebuild(vc, game_id, play) != 0) {
        return -1;
    }
    printf("installed mod %s for %s\n", mod_id, game_id);
    return 0;
}

int
vapor_mod_remove(vapor_client *vc, const char *game_id, const char *mod_id)
{
    vapor_install rec;
    sqlite3_stmt *st = NULL;
    char          play[VAPOR_PATH_MAX];
    char          dir[VAPOR_PATH_MAX];

    if (!vapor_id_is_valid(game_id) || !vapor_id_is_valid(mod_id)) {
        vapor_client_set_error(vc, "bad id");
        return -1;
    }
    if (require_playable(vc, game_id, &rec, play, sizeof(play)) != 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(vc->db,
                           "DELETE FROM mod_state WHERE game_id = ? AND mod_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, mod_id, -1, SQLITE_STATIC);
    if (db_exec(vc, st) != 0) {
        return -1;
    }
    if (rebuild(vc, game_id, play) != 0) {
        return -1;
    }
    if (cache_dir(vc, game_id, mod_id, dir, sizeof(dir)) == 0) {
        vapor_plat_native_path(dir);
        (void)vapor_mod_remove_tree(dir);
    }
    printf("removed mod %s from %s\n", mod_id, game_id);
    return 0;
}

int
vapor_mod_set_enabled(vapor_client *vc, const char *game_id, const char *mod_id,
                      int enabled)
{
    vapor_install rec;
    sqlite3_stmt *st = NULL;
    char          play[VAPOR_PATH_MAX];
    char          dir[VAPOR_PATH_MAX];
    char          zip[VAPOR_PATH_MAX];

    if (!vapor_id_is_valid(game_id) || !vapor_id_is_valid(mod_id)) {
        vapor_client_set_error(vc, "bad id");
        return -1;
    }
    if (require_playable(vc, game_id, &rec, play, sizeof(play)) != 0) {
        return -1;
    }
    if (enabled) {
        if (cache_dir(vc, game_id, mod_id, dir, sizeof(dir)) != 0
            || join2(zip, sizeof(zip), dir, "package.zip") != 0) {
            vapor_client_set_error(vc, "mod path is too long");
            return -1;
        }
        vapor_plat_native_path(zip);
        if (!vapor_plat_exists(zip)) {
            vapor_client_set_error(vc, "install mod \"%s\" before enabling it",
                                   mod_id);
            return -1;
        }
    }
    if (sqlite3_prepare_v2(vc->db,
                           "UPDATE mod_state SET enabled = ?"
                           " WHERE game_id = ? AND mod_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_int(st, 1, enabled ? 1 : 0);
    sqlite3_bind_text(st, 2, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, mod_id, -1, SQLITE_STATIC);
    if (db_exec(vc, st) != 0) {
        return -1;
    }
    if (sqlite3_changes(vc->db) == 0) {
        vapor_client_set_error(vc, "mod \"%s\" is not installed", mod_id);
        return -1;
    }
    if (rebuild(vc, game_id, play) != 0) {
        return -1;
    }
    printf("%s mod %s for %s\n", enabled ? "enabled" : "disabled", mod_id,
           game_id);
    return 0;
}

int
vapor_mod_restore(vapor_client *vc, const char *game_id)
{
    vapor_install rec;
    sqlite3_stmt *st = NULL;
    char          play[VAPOR_PATH_MAX];

    if (!vapor_id_is_valid(game_id)) {
        vapor_client_set_error(vc, "bad game id");
        return -1;
    }
    if (require_playable(vc, game_id, &rec, play, sizeof(play)) != 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(vc->db,
                           "UPDATE mod_state SET enabled = 0 WHERE game_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    if (db_exec(vc, st) != 0) {
        return -1;
    }
    if (rebuild(vc, game_id, play) != 0) {
        return -1;
    }
    printf("restored %s to its original files\n", game_id);
    return 0;
}

int
vapor_mods_clear_enabled(vapor_client *vc, const char *game_id)
{
    vapor_install rec;
    sqlite3_stmt *st = NULL;
    char          play[VAPOR_PATH_MAX];

    if (sqlite3_prepare_v2(vc->db,
                           "UPDATE mod_state SET enabled = 0 WHERE game_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    if (db_exec(vc, st) != 0) {
        return -1;
    }
    if (vapor_db_get_install(vc, game_id, &rec) == 0
        && !vapor_install_is_external(&rec)
        && play_dir_of(&rec, play, sizeof(play)) == 0) {
        char path[VAPOR_PATH_MAX];
        char vanilla[VAPOR_PATH_MAX];

        if (join2(path, sizeof(path), play, ".vapor/" APPLIED_NAME) == 0) {
            vapor_plat_native_path(path);
            remove(path);
        }
        if (join2(vanilla, sizeof(vanilla), play, ".vapor/vanilla") == 0) {
            vapor_plat_native_path(vanilla);
            (void)vapor_mod_remove_tree(vanilla);
        }
    }
    return 0;
}

int
vapor_mods_forget_game(vapor_client *vc, const char *game_id)
{
    sqlite3_stmt *st = NULL;
    char          dir[VAPOR_PATH_MAX];

    if (sqlite3_prepare_v2(vc->db, "DELETE FROM mod_state WHERE game_id = ?", -1,
                           &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    if (db_exec(vc, st) != 0) {
        return -1;
    }
    if (cache_dir(vc, game_id, NULL, dir, sizeof(dir)) == 0) {
        vapor_plat_native_path(dir);
        (void)vapor_mod_remove_tree(dir);
    }
    return 0;
}

int
vapor_mod_launch_override(vapor_client *vc, const char *game_id,
                          vapor_mod_manifest *out)
{
    sqlite3_stmt *st = NULL;
    int           found = 0;

    vapor_mod_manifest_init(out);
    if (sqlite3_prepare_v2(vc->db,
                           "SELECT mod_id FROM mod_state"
                           " WHERE game_id = ? AND enabled != 0"
                           " ORDER BY sort_order, mod_id",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char        *id = (const char *)sqlite3_column_text(st, 0);
        vapor_mod_manifest mm;

        if (!id) {
            continue;
        }
        if (load_cached_manifest(vc, game_id, id, &mm) != 0) {
            sqlite3_finalize(st);
            vapor_mod_manifest_free(out);
            return -1;
        }
        if ((mm.exec && mm.exec[0]) || mm.nargs > 0) {
            vapor_mod_manifest_free(out);
            *out = mm;
            found = 1;
        } else {
            vapor_mod_manifest_free(&mm);
        }
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}
