#include "vapord.h"

#include "civetweb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/util.h"

/* GET /download/<game_id>/<version>
 *
 * The auth check happens here and then mg_send_file takes over, which is what
 * gives us Range/resume and If-Modified-Since without writing any of it: see
 * handle_static_file_request in civetweb.c. */
int
vapord_route_download(vapord *app, struct mg_connection *c, const char *method,
                      const char *tail)
{
    int64_t user_id = 0;
    const char *p;
    char    game_id[VAPOR_ID_MAX + 1];
    char    version[VAPOR_VERSION_MAX + 1];
    char    path[VAPORD_PATH_MAX];
    char   *manifest_json = NULL;
    vapor_manifest m;
    char    err[256];
    size_t  n;

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST, "use GET");
    }
    if (!vapord_require_user(app, c, &user_id)) {
        return 401;
    }

    if (!vapor_str_has_prefix(tail, "/download/")) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
    }
    p = tail + strlen("/download/");

    n = strcspn(p, "/");
    if (n == 0 || n > VAPOR_ID_MAX) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "bad game id");
    }
    memcpy(game_id, p, n);
    game_id[n] = '\0';
    p += n;
    if (*p != '/') {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST,
                                  "expected /download/<game>/<version>");
    }
    p++;

    n = strlen(p);
    if (n == 0 || n > VAPOR_VERSION_MAX || strchr(p, '/')) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "bad version");
    }
    memcpy(version, p, n);
    version[n] = '\0';

    if (strcmp(version, "latest") == 0) {
        char latest[VAPOR_VERSION_MAX + 1];
        if (vapord_game_latest_version(app->db, game_id, latest, sizeof(latest)) != 0) {
            return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND,
                                      "no published version for \"%s\"", game_id);
        }
        snprintf(version, sizeof(version), "%s", latest);
    }

    /* The package filename comes from the stored manifest, never from the URL,
     * so the request cannot name an arbitrary file inside the content root. */
    if (vapord_version_manifest(app->db, game_id, version, &manifest_json) != 0
        || !manifest_json) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND,
                                  "no such game version");
    }
    if (vapor_manifest_parse(manifest_json, strlen(manifest_json), &m,
                             err, sizeof(err))
        != 0) {
        free(manifest_json);
        VLOG_ERROR("stored manifest for %s/%s is invalid: %s", game_id, version, err);
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                  "stored manifest is invalid");
    }
    free(manifest_json);

    {
        char source[VAPORD_PATH_MAX];
        int  have_source = vapord_version_source(app->db, game_id, version,
                                                source, sizeof(source));

        if (have_source == 0) {
            vapor_manifest_free(&m);
            if (vapord_library_resolve(&app->cfg, source, path, sizeof(path))
                != 0) {
                return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                          "could not resolve library file");
            }
        } else if (vapord_content_path(&app->cfg, game_id, version, m.package.file,
                                       path, sizeof(path))
                   != 0) {
            vapor_manifest_free(&m);
            return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                      "could not resolve package path");
        } else {
            vapor_manifest_free(&m);
        }
    }

    VLOG_INFO("user %lld downloading %s/%s", (long long)user_id, game_id, version);

    /* application/octet-stream keeps proxies from trying to transform it. */
    mg_send_mime_file2(c, path, "application/octet-stream", NULL);
    return 200;
}
