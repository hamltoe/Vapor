#include "vapord.h"

#include "civetweb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/util.h"

/* Serves the manifest straight out of the database rather than re-reading the
 * file, so the catalog and the bytes on disk cannot drift apart. */
static int
send_manifest(vapord *app, struct mg_connection *c, const char *game_id,
              const char *version)
{
    char *json = NULL;
    char  len[32];
    int   rc;

    rc = vapord_version_manifest(app->db, game_id, version, &json);
    if (rc != 0 || !json) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND,
                                  "no manifest for %s version %s",
                                  game_id, version);
    }

    snprintf(len, sizeof(len), "%llu", (unsigned long long)strlen(json));
    mg_response_header_start(c, 200);
    mg_response_header_add(c, "Content-Type", "application/json; charset=utf-8", -1);
    mg_response_header_add(c, "Content-Length", len, -1);
    mg_response_header_send(c);
    mg_write(c, json, strlen(json));
    free(json);
    return 200;
}

/* Cover art is a separate endpoint rather than bytes inlined in the catalog so
 * that a library view of 200 games is still one small JSON response, and so
 * civetweb's file handler can answer repeat requests with a 304. */
static int
send_cover(vapord *app, struct mg_connection *c, const char *game_id,
           const char *version)
{
    char cover[256];
    char path[VAPORD_PATH_MAX];
    const char *mime;

    if (vapord_version_cover(app->db, game_id, version, cover, sizeof(cover)) != 0) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND,
                                  "%s has no cover art", game_id);
    }
    if (vapord_content_path(&app->cfg, game_id, version, cover, path, sizeof(path))
        != 0) {
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                  "could not resolve cover path");
    }

    if (vapor_str_ends_with_ci(cover, ".png")) {
        mime = "image/png";
    } else if (vapor_str_ends_with_ci(cover, ".jpg")
               || vapor_str_ends_with_ci(cover, ".jpeg")) {
        mime = "image/jpeg";
    } else {
        mime = "application/octet-stream";
    }

    mg_send_mime_file2(c, path, mime, NULL);
    return 200;
}

/* tail is everything after VAPOR_API_PREFIX, i.e. "/games..." */
int
vapord_route_catalog(vapord *app, struct mg_connection *c, const char *method,
                     const char *tail)
{
    int64_t     user_id = 0;
    const char *p;
    char        game_id[VAPOR_ID_MAX + 1];
    char        version[VAPOR_VERSION_MAX + 1];
    size_t      n;

    if (strcmp(method, "GET") != 0) {
        return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST, "use GET");
    }
    if (!vapord_require_user(app, c, &user_id)) {
        return 401;
    }

    /* GET /games -> whole catalog */
    if (strcmp(tail, "/games") == 0 || strcmp(tail, "/games/") == 0) {
        cJSON *out = vapord_catalog_json(app->db);
        if (!out) {
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                      "could not build catalog");
        }
        return vapord_send_json(c, 200, out);
    }

    if (!vapor_str_has_prefix(tail, "/games/")) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
    }
    p = tail + strlen("/games/");

    /* <game_id>[/versions/<version>/manifest] */
    n = strcspn(p, "/");
    if (n == 0 || n > VAPOR_ID_MAX) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "bad game id");
    }
    memcpy(game_id, p, n);
    game_id[n] = '\0';
    if (!vapor_id_is_valid(game_id)) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "bad game id");
    }
    p += n;

    /* GET /games/<id> -> detail */
    if (*p == '\0' || strcmp(p, "/") == 0) {
        cJSON *out = vapord_game_json(app->db, game_id);
        if (!out) {
            return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND,
                                      "no such game \"%s\"", game_id);
        }
        return vapord_send_json(c, 200, out);
    }

    /* GET /games/<id>/versions/<version>/manifest */
    if (!vapor_str_has_prefix(p, "/versions/")) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
    }
    p += strlen("/versions/");

    n = strcspn(p, "/");
    if (n == 0 || n > VAPOR_VERSION_MAX) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "bad version");
    }
    memcpy(version, p, n);
    version[n] = '\0';
    p += n;

    /* "latest" resolves server-side so the client never guesses version order. */
    if (strcmp(version, "latest") == 0) {
        char latest[VAPOR_VERSION_MAX + 1];
        if (vapord_game_latest_version(app->db, game_id, latest, sizeof(latest)) != 0) {
            return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND,
                                      "no published version for \"%s\"", game_id);
        }
        snprintf(version, sizeof(version), "%s", latest);
    } else if (!vapor_version_is_valid(version)) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "bad version");
    }

    if (strcmp(p, "/manifest") == 0) {
        return send_manifest(app, c, game_id, version);
    }
    if (strcmp(p, "/cover") == 0) {
        return send_cover(app, c, game_id, version);
    }
    return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
}
