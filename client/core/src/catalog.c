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

    if (e->id[0] && vapor_db_get_install(vc, e->id, &rec) == 0) {
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
                    if (vapor_db_record_install(vc, &rec) == 0) {
                        e->setup_pending = 1;
                    }
                }
                vapor_manifest_free(&m);
            }
        }
    }
}

int
vapor_catalog_fetch(vapor_client *vc, vapor_catalog_entry **out, size_t *count)
{
    vapor_response       r;
    cJSON               *body, *games, *g;
    vapor_catalog_entry *rows = NULL;
    size_t               n = 0, cap = 0;

    *out = NULL;
    *count = 0;

    if (vapor_api_get(vc, VAPOR_EP_GAMES, 1, &r) != 0) {
        vapor_response_free(&r);
        return -1;
    }
    body = vapor_json_parse_response(&r);
    vapor_response_free(&r);
    if (!body) {
        vapor_client_set_error(vc, "could not read the catalog");
        return -1;
    }

    games = cJSON_GetObjectItemCaseSensitive(body, "games");
    if (!cJSON_IsArray(games)) {
        vapor_client_set_error(vc, "the catalog response has no games array");
        cJSON_Delete(body);
        return -1;
    }

    cJSON_ArrayForEach(g, games) {
        if (n == cap) {
            size_t               ncap = cap ? cap * 2 : 16;
            vapor_catalog_entry *grown;

            grown = (vapor_catalog_entry *)realloc(rows, ncap * sizeof(*grown));
            if (!grown) {
                vapor_client_set_error(vc, "out of memory");
                free(rows);
                cJSON_Delete(body);
                return -1;
            }
            rows = grown;
            cap = ncap;
        }
        entry_from_json(vc, g, &rows[n]);
        n++;
    }

    cJSON_Delete(body);
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
