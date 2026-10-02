/* Catalog reads. The server's view and the local install database are merged
 * here so a frontend can render a library row without consulting two sources
 * and re-deriving "is there an update" itself. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "json.h"
#include "platform.h"
#include "vapor/client.h"
#include "vapor/util.h"

static void
entry_from_json(vapor_client *vc, const cJSON *g, vapor_catalog_entry *e)
{
    vapor_install rec;

    memset(e, 0, sizeof(*e));
    vapor_json_copy(e->id, sizeof(e->id), g, "id", "");
    vapor_json_copy(e->name, sizeof(e->name), g, "name", e->id);
    vapor_json_copy(e->latest_version, sizeof(e->latest_version), g,
                    "latest_version", "");
    vapor_json_copy(e->developer, sizeof(e->developer), g, "developer", "");
    vapor_json_copy(e->description, sizeof(e->description), g, "description", "");
    e->size = (uint64_t)vapor_json_num(g, "size", 0);
    e->has_cover = vapor_json_bool(g, "has_cover", 0);
    e->rating_avg = vapor_json_num(g, "rating_avg", 0);
    e->rating_votes = (int)vapor_json_num(g, "rating_votes", 0);
    e->my_rating = (int)vapor_json_num(g, "my_rating", 0);
    e->steam_rating_pct = (int)vapor_json_num(g, "steam_rating_pct", 0);
    e->steam_rating_count = (int)vapor_json_num(g, "steam_rating_count", 0);
    vapor_json_copy(e->steam_rating_label, sizeof(e->steam_rating_label), g,
                    "steam_rating_label", "");

    /* An external shortcut can share an id shape with a catalog row only after
     * a collision rename failed. Do not treat the server game as that shortcut. */
    if (e->id[0] && vapor_db_get_install(vc, e->id, &rec) == 0
        && !vapor_install_is_external(&rec)) {
        e->installed = 1;
        snprintf(e->installed_version, sizeof(e->installed_version), "%s",
                 rec.version);
        e->setup_pending = rec.setup_pending;
        e->play_seconds = rec.play_seconds;
        /* Natural-order compare, so 1.9 -> 1.10 counts as an update. */
        e->update_available = (e->latest_version[0]
                               && vapor_version_cmp(e->latest_version,
                                                    rec.version)
                                      > 0);
        if (!e->setup_pending) {
            vapor_manifest m;

            if (vapor_read_local_manifest(vc, e->id, &m) == 0) {
                const vapor_target *t = vapor_manifest_pick_target(
                    &m, vapor_host_platform(), vapor_host_arch());
                const char         *rel = (t && t->exec) ? t->exec : "";
                const char         *base = strrchr(rel, '/');

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
                    snprintf(rec.setup_state, sizeof(rec.setup_state), "%s",
                             VAPOR_SETUP_PENDING);
                    if (!rec.install_kind[0]) {
                        snprintf(rec.install_kind, sizeof(rec.install_kind),
                                 "%s", VAPOR_INSTALL_KIND_OS_PRODUCT);
                    }
                    if (vapor_db_record_install(vc, &rec) == 0) {
                        e->setup_pending = 1;
                    }
                }
                vapor_manifest_free(&m);
            }
        }
    }
}

static int
catalog_grow(vapor_catalog_entry **rows, size_t *cap, size_t n)
{
    size_t               ncap;
    vapor_catalog_entry *grown;

    if (n < *cap) {
        return 0;
    }
    ncap = *cap ? *cap * 2 : 16;
    grown = (vapor_catalog_entry *)realloc(*rows, ncap * sizeof(*grown));
    if (!grown) {
        return -1;
    }
    *rows = grown;
    *cap = ncap;
    return 0;
}

static int
catalog_has_id(const vapor_catalog_entry *rows, size_t n, const char *id)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void
entry_from_external(const vapor_install *rec, vapor_catalog_entry *e)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->id, sizeof(e->id), "%s", rec->game_id);
    snprintf(e->name, sizeof(e->name), "%s",
             rec->name[0] ? rec->name : rec->game_id);
    snprintf(e->developer, sizeof(e->developer), "On this computer");
    snprintf(e->description, sizeof(e->description), "%s",
             rec->launch_exe[0] ? rec->launch_exe : rec->install_dir);
    e->installed = 1;
    e->external = 1;
    e->play_seconds = rec->play_seconds;
}

/* Local shortcuts are not in the server catalog. If a server id collides with
 * one, rename the shortcut so both can sit in the library. */
static int
append_external_shortcuts(vapor_client *vc, vapor_catalog_entry **rows,
                          size_t *n, size_t *cap)
{
    vapor_install *all = NULL;
    size_t         ni = 0, i;

    if (vapor_db_list_installs(vc, &all, &ni) != 0) {
        return -1;
    }
    for (i = 0; i < ni; i++) {
        vapor_install rec = all[i];

        if (!vapor_install_is_external(&rec)) {
            continue;
        }
        if (catalog_has_id(*rows, *n, rec.game_id)) {
            char alt[VAPOR_ID_MAX + 1];
            int  s, renamed = 0;

            for (s = 2; s < 100; s++) {
                vapor_install tmp;
                int           got;

                if (vapor_local_shortcut_id(rec.name[0] ? rec.name : "game", s,
                                            alt, sizeof(alt))
                    != 0) {
                    continue;
                }
                if (catalog_has_id(*rows, *n, alt)) {
                    continue;
                }
                got = vapor_db_get_install(vc, alt, &tmp);
                if (got < 0) {
                    free(all);
                    return -1;
                }
                if (got == 0) {
                    continue;
                }
                if (vapor_db_rename_install(vc, rec.game_id, alt) != 0) {
                    break;
                }
                snprintf(rec.game_id, sizeof(rec.game_id), "%s", alt);
                renamed = 1;
                break;
            }
            if (!renamed) {
                continue;
            }
        }
        if (catalog_grow(rows, cap, *n) != 0) {
            vapor_client_set_error(vc, "out of memory");
            free(all);
            return -1;
        }
        entry_from_external(&rec, &(*rows)[*n]);
        (*n)++;
    }
    free(all);
    return 0;
}

int
vapor_catalog_fetch(vapor_client *vc, vapor_catalog_entry **out, size_t *count)
{
    vapor_response       r;
    cJSON               *body = NULL, *games, *g;
    vapor_catalog_entry *rows = NULL;
    size_t               n = 0, cap = 0;
    int                  server_ok = 0;
    char                 server_err[1024];

    *out = NULL;
    *count = 0;
    vc->err[0] = '\0';
    server_err[0] = '\0';

    if (vapor_api_get(vc, VAPOR_EP_GAMES, 1, &r) != 0) {
        snprintf(server_err, sizeof(server_err), "%s",
                 vc->err[0] ? vc->err : "cannot reach the server");
        vapor_response_free(&r);
    } else {
        body = vapor_json_parse_response(&r);
        vapor_response_free(&r);
        if (!body) {
            snprintf(server_err, sizeof(server_err), "could not read the catalog");
        } else {
            games = cJSON_GetObjectItemCaseSensitive(body, "games");
            if (!cJSON_IsArray(games)) {
                snprintf(server_err, sizeof(server_err),
                         "the catalog response has no games array");
            } else {
                server_ok = 1;
                cJSON_ArrayForEach(g, games) {
                    if (catalog_grow(&rows, &cap, n) != 0) {
                        vapor_client_set_error(vc, "out of memory");
                        free(rows);
                        cJSON_Delete(body);
                        return -1;
                    }
                    entry_from_json(vc, g, &rows[n]);
                    n++;
                }
            }
            cJSON_Delete(body);
            body = NULL;
        }
    }

    if (append_external_shortcuts(vc, &rows, &n, &cap) != 0) {
        free(rows);
        return -1;
    }
    if (!server_ok && n == 0) {
        vapor_client_set_error(vc, "%s",
                               server_err[0] ? server_err : "cannot reach the server");
        free(rows);
        return -1;
    }
    if (!server_ok) {
        vapor_client_set_error(
            vc, "server catalog is unavailable; showing games on this computer");
    } else {
        vc->err[0] = '\0';
    }
    *out = rows;
    *count = n;
    return 0;
}

/* Covers are cached under a synthetic name with no extension: the decoders
 * sniff the format from the leading bytes, so the client never has to trust or
 * even learn the filename the publisher used. */
int
vapor_fetch_cover(vapor_client *vc, const char *game_id, const char *version,
                  char *out_path, size_t outsz)
{
    vapor_response r;
    char           dir[VAPOR_PATH_MAX];
    char           path[VAPOR_PATH_MAX];
    char           api[512];
    uint64_t       have = 0;
    FILE          *f;
    size_t         wrote, want;

    if (out_path && outsz) {
        out_path[0] = '\0';
    }
    if (!vapor_id_is_valid(game_id) || !version || !*version) {
        vapor_client_set_error(vc, "bad game id or version");
        return -1;
    }

    if ((size_t)snprintf(dir, sizeof(dir), "%s/covers", vc->data_dir) >= sizeof(dir)
        || (size_t)snprintf(path, sizeof(path), "%s/%s-%s.cover", dir, game_id,
                            version)
               >= sizeof(path)) {
        vapor_client_set_error(vc, "the cover cache path is too long");
        return -1;
    }

    if (vapor_plat_file_size(path, &have) == 0 && have > 0) {
        goto found;
    }

    if (vapor_plat_mkdirs(dir) != 0) {
        vapor_client_set_error(vc, "cannot create %s", dir);
        return -1;
    }

    snprintf(api, sizeof(api), "%s/%s/versions/%s/cover", VAPOR_EP_GAMES,
             game_id, version);
    if (vapor_http_request(vc, "GET", api, NULL, 1, &r) != 0) {
        vapor_response_free(&r);
        vapor_client_set_error(vc, "cannot reach the server for cover art");
        return -1;
    }
    if (r.status == 404) {
        vapor_response_free(&r);
        return 1;
    }
    if (r.status < 200 || r.status > 299 || !r.body || r.body_len == 0) {
        vapor_response_free(&r);
        vapor_client_set_error(vc, "the server refused the cover for %s", game_id);
        return -1;
    }
    if (r.body_len > VAPOR_MAX_COVER_BYTES) {
        vapor_response_free(&r);
        vapor_client_set_error(vc, "the cover for %s is implausibly large",
                               game_id);
        return -1;
    }

    f = fopen(path, "wb");
    if (!f) {
        vapor_response_free(&r);
        vapor_client_set_error(vc, "cannot write %s", path);
        return -1;
    }
    want = r.body_len;
    wrote = fwrite(r.body, 1, want, f);
    vapor_response_free(&r);
    if (wrote != want || fclose(f) != 0) {
        remove(path);
        vapor_client_set_error(vc, "cannot write %s", path);
        return -1;
    }

found:
    if (out_path && outsz) {
        snprintf(out_path, outsz, "%s", path);
    }
    return 0;
}

int
vapor_game_fetch(vapor_client *vc, const char *game_id, vapor_game_detail *out)
{
    vapor_response r;
    cJSON         *body, *versions, *v;
    char           path[256];
    size_t         n = 0, i;

    memset(out, 0, sizeof(*out));

    if (!vapor_id_is_valid(game_id)) {
        vapor_client_set_error(vc, "\"%s\" is not a valid game id", game_id);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", VAPOR_EP_GAMES, game_id);

    if (vapor_api_get(vc, path, 1, &r) != 0) {
        vapor_response_free(&r);
        return -1;
    }
    body = vapor_json_parse_response(&r);
    vapor_response_free(&r);
    if (!body) {
        vapor_client_set_error(vc, "could not read the response for %s", game_id);
        return -1;
    }

    entry_from_json(vc, body, &out->game);

    versions = cJSON_GetObjectItemCaseSensitive(body, "versions");
    cJSON_ArrayForEach(v, versions) {
        n++;
    }
    if (n > 0) {
        out->versions = (char **)calloc(n, sizeof(*out->versions));
        if (!out->versions) {
            vapor_client_set_error(vc, "out of memory");
            cJSON_Delete(body);
            return -1;
        }
        i = 0;
        cJSON_ArrayForEach(v, versions) {
            const char *s = vapor_json_str(v, "version", NULL);
            if (!s) {
                continue;
            }
            out->versions[i] = vapor_strdup(s);
            if (!out->versions[i]) {
                vapor_client_set_error(vc, "out of memory");
                out->nversions = i;
                vapor_game_detail_free(out);
                cJSON_Delete(body);
                return -1;
            }
            i++;
        }
        out->nversions = i;
    }

    cJSON_Delete(body);
    return 0;
}

void
vapor_game_detail_free(vapor_game_detail *d)
{
    size_t i;

    if (!d) {
        return;
    }
    for (i = 0; i < d->nversions; i++) {
        free(d->versions[i]);
    }
    free(d->versions);
    d->versions = NULL;
    d->nversions = 0;
}

int
vapor_game_rate(vapor_client *vc, const char *game_id, int score,
                vapor_rating *out)
{
    vapor_response r;
    cJSON         *body;
    char           path[256];
    char           payload[64];

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!vapor_id_is_valid(game_id)) {
        vapor_client_set_error(vc, "\"%s\" is not a valid game id", game_id);
        return -1;
    }
    if (score < 1 || score > 5) {
        vapor_client_set_error(vc, "rating must be 1 to 5");
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s/rating", VAPOR_EP_GAMES, game_id);
    snprintf(payload, sizeof(payload), "{\"score\":%d}", score);

    if (vapor_api_put(vc, path, payload, 1, &r) != 0) {
        vapor_response_free(&r);
        return -1;
    }
    body = vapor_json_parse_response(&r);
    vapor_response_free(&r);
    if (!body) {
        vapor_client_set_error(vc, "could not read the rating response");
        return -1;
    }
    if (out) {
        out->my_rating = (int)vapor_json_num(body, "score", (double)score);
        out->rating_avg = vapor_json_num(body, "rating_avg", 0);
        out->rating_votes = (int)vapor_json_num(body, "rating_votes", 0);
    }
    cJSON_Delete(body);
    return 0;
}
