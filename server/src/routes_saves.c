#include "vapord.h"

#include "civetweb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/saves.h"
#include "vapor/sha256.h"

static int
query_revision(const struct mg_request_info *ri, int64_t *out)
{
    const char *q;
    const char *p;
    char       *end = NULL;

    q = ri ? ri->query_string : NULL;
    if (!q) {
        return -1;
    }
    p = strstr(q, "revision=");
    if (!p || (p != q && p[-1] != '&')) {
        return -1;
    }
    *out = strtoll(p + 9, &end, 10);
    if (end == p + 9 || *out < 0) {
        return -1;
    }
    return 0;
}

static int
copy_game_id(const char *rest, char *id, size_t idsz, const char **suffix)
{
    size_t n = 0;

    *suffix = NULL;
    while (rest[n] && rest[n] != '/') {
        n++;
    }
    if (n == 0 || n >= idsz) {
        return -1;
    }
    memcpy(id, rest, n);
    id[n] = '\0';
    if (!vapor_id_is_valid(id)) {
        return -1;
    }
    if (rest[n] == '/') {
        *suffix = rest + n;
    }
    return 0;
}

static cJSON *
save_json(int64_t revision, const char *sha, int64_t size, int64_t updated)
{
    cJSON *out = cJSON_CreateObject();

    if (!out) {
        return NULL;
    }
    cJSON_AddNumberToObject(out, "revision", (double)revision);
    cJSON_AddStringToObject(out, "sha256", sha ? sha : "");
    cJSON_AddNumberToObject(out, "size", (double)size);
    cJSON_AddNumberToObject(out, "updated_at", (double)updated);
    return out;
}

int
vapord_route_saves(vapord *app, struct mg_connection *c, const char *method,
                   const char *tail)
{
    int64_t     user_id = 0;
    char        game_id[VAPOR_ID_MAX + 1];
    const char *suffix = NULL;
    char        dir[VAPORD_PATH_MAX];
    char        path[VAPORD_PATH_MAX];
    int64_t     revision = 0, size = 0, updated = 0;
    char        sha[VAPOR_SHA256_HEX_LEN + 1];
    int         found;

    if (!vapor_str_has_prefix(tail, "/me/saves/")) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
    }
    if (!vapord_require_user(app, c, &user_id)) {
        return 401;
    }
    if (copy_game_id(tail + strlen("/me/saves/"), game_id, sizeof(game_id),
                     &suffix)
        != 0) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "invalid game id");
    }
    if (vapord_saves_path(&app->cfg, user_id, game_id, NULL, dir, sizeof(dir)) != 0
        || vapord_saves_path(&app->cfg, user_id, game_id, "save.zip", path,
                             sizeof(path))
               != 0) {
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "save path is invalid");
    }

    if (suffix && strcmp(suffix, "/blob") == 0) {
        if (strcmp(method, "GET") != 0) {
            return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST, "use GET");
        }
        found = vapord_save_get(app->db, user_id, game_id, &revision, sha,
                                sizeof(sha), &size, &updated);
        if (found != 0) {
            return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND,
                                      "no saves stored for this game");
        }
        mg_send_mime_file2(c, path, "application/octet-stream", NULL);
        return 200;
    }
    if (suffix) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
    }

    if (strcmp(method, "GET") == 0) {
        found = vapord_save_get(app->db, user_id, game_id, &revision, sha,
                                sizeof(sha), &size, &updated);
        if (found < 0) {
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "cannot read saves");
        }
        if (found == 1) {
            return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND,
                                      "no saves stored for this game");
        }
        return vapord_send_json(c, 200, save_json(revision, sha, size, updated));
    }

    if (strcmp(method, "PUT") == 0) {
        const struct mg_request_info *ri = mg_get_request_info(c);
        char                          tmp[VAPORD_PATH_MAX];
        char                          err[256];
        int64_t                       expect = 0;
        int64_t                       current = 0;
        uint64_t                      nbytes = 0;
        int                           n;

        if (query_revision(ri, &expect) != 0) {
            return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST,
                                      "revision query is required");
        }
        found = vapord_save_get(app->db, user_id, game_id, &current, sha,
                                sizeof(sha), &size, &updated);
        if (found < 0) {
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "cannot read saves");
        }
        if (found == 1) {
            current = 0;
        }
        if (expect != current) {
            return vapord_send_errorf(c, 409, VAPOR_ERR_CONFLICT,
                                      "save revision is %lld", (long long)current);
        }
        if (vapord_content_mkdirs(dir) != 0) {
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                      "cannot create save directory");
        }
        n = snprintf(tmp, sizeof(tmp), "%s.incoming", path);
        if (n < 0 || (size_t)n >= sizeof(tmp)) {
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "save path is too long");
        }
        if (vapord_read_body_file(c, tmp, VAPOR_SAVE_SYNC_MAX_BYTES, &nbytes, err,
                                  sizeof(err))
            != 0) {
            return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "%s", err);
        }
        if (vapor_sha256_file(tmp, sha) != 0) {
            remove(tmp);
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                      "cannot hash save archive");
        }
        /* Re-check after the upload so a concurrent put is still a 409. */
        found = vapord_save_get(app->db, user_id, game_id, &current, NULL, 0,
                                NULL, NULL);
        if (found < 0) {
            remove(tmp);
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "cannot read saves");
        }
        if (found == 1) {
            current = 0;
        }
        if (expect != current) {
            remove(tmp);
            return vapord_send_errorf(c, 409, VAPOR_ERR_CONFLICT,
                                      "save revision is %lld", (long long)current);
        }
        remove(path);
        if (rename(tmp, path) != 0) {
            remove(tmp);
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                      "cannot store save archive");
        }
        if (vapord_save_store(app->db, user_id, game_id, current + 1, sha,
                              (int64_t)nbytes)
            != 0) {
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                      "cannot record save revision");
        }
        VLOG_INFO("user %lld stored saves for %s revision %lld",
                  (long long)user_id, game_id, (long long)(current + 1));
        return vapord_send_json(c, 200,
                                save_json(current + 1, sha, (int64_t)nbytes,
                                          vapor_now_unix()));
    }

    return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST, "use GET or PUT");
}
