/* URL assembly and API-level error handling. The actual transport lives in
 * net_curl.c / net_winhttp.c behind vapor_net_perform. */

#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "platform.h"
#include "vapor/util.h"

void
vapor_response_free(vapor_response *r)
{
    if (r) {
        free(r->body);
        r->body = NULL;
        r->body_len = 0;
        r->status = 0;
    }
}

static int
build_url(vapor_client *vc, const char *path, char *out, size_t outsz)
{
    if (!vc->cfg.server_url[0]) {
        vapor_client_set_error(vc, "no server configured; run \"vapor config "
                                  "server <url>\" first");
        return -1;
    }
    if ((size_t)snprintf(out, outsz, "%s%s", vc->cfg.server_url, path) >= outsz) {
        vapor_client_set_error(vc, "request URL too long");
        return -1;
    }
    return 0;
}

int
vapor_http_request(vapor_client *vc, const char *method, const char *path,
                   const char *json_body, int auth, vapor_response *out)
{
    vapor_net_req req;
    vapor_net_res res;
    char          url[VAPOR_PATH_MAX + 256];

    memset(out, 0, sizeof(*out));
    if (build_url(vc, path, url, sizeof(url)) != 0) {
        return -1;
    }

    if (auth && !vapor_client_has_token(vc)) {
        vapor_client_set_error(vc, "not signed in; run \"vapor login\"");
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.method = method;
    req.url = url;
    req.body = json_body;
    req.bearer = auth ? vc->cfg.token : NULL;
    req.pinned_pubkey = vc->cfg.pinned_pubkey[0] ? vc->cfg.pinned_pubkey : NULL;

    memset(&res, 0, sizeof(res));
    if (vapor_net_perform(&req, &res) != 0) {
        vapor_client_set_error(vc, "%s", res.err[0] ? res.err : "request failed");
        vapor_net_res_free(&res);
        return -1;
    }

    out->status = res.status;
    out->body = res.body;
    out->body_len = res.body_len;
    res.body = NULL; /* ownership moved to caller */
    vapor_net_res_free(&res);
    return 0;
}

/* Pulls {"error":..,"message":..} out of a failure body so the CLI can print
 * what the server actually objected to. */
static void
absorb_api_error(vapor_client *vc, const vapor_response *r)
{
    cJSON *obj;

    if (r->body && r->body_len) {
        obj = cJSON_ParseWithLength(r->body, r->body_len);
        if (obj) {
            const cJSON *msg = cJSON_GetObjectItemCaseSensitive(obj, "message");
            if (cJSON_IsString(msg) && msg->valuestring && *msg->valuestring) {
                vapor_client_set_error(vc, "%s", msg->valuestring);
                cJSON_Delete(obj);
                return;
            }
            cJSON_Delete(obj);
        }
    }
    vapor_client_set_error(vc, "server returned HTTP %ld", r->status);
}

static int
finish_api_call(vapor_client *vc, vapor_response *out, int rc)
{
    if (rc != 0) {
        return -1;
    }
    if (out->status < 200 || out->status >= 300) {
        absorb_api_error(vc, out);
        return -1;
    }
    return 0;
}

int
vapor_api_get(vapor_client *vc, const char *path, int auth, vapor_response *out)
{
    return finish_api_call(vc, out,
                           vapor_http_request(vc, "GET", path, NULL, auth, out));
}

int
vapor_api_post(vapor_client *vc, const char *path, const char *json_body,
               int auth, vapor_response *out)
{
    return finish_api_call(vc, out,
                           vapor_http_request(vc, "POST", path, json_body, auth,
                                              out));
}

int
vapor_http_download(vapor_client *vc, const char *path, const char *dest_path,
                    vapor_progress_fn cb, void *ud)
{
    vapor_net_req req;
    vapor_net_res res;
    char          url[VAPOR_PATH_MAX + 256];
    uint64_t      have = 0;

    if (build_url(vc, path, url, sizeof(url)) != 0) {
        return -1;
    }
    if (!vapor_client_has_token(vc)) {
        vapor_client_set_error(vc, "not signed in; run \"vapor login\"");
        return -1;
    }

    /* Any bytes already on disk are resumed rather than refetched. */
    if (vapor_plat_file_size(dest_path, &have) != 0) {
        have = 0;
    }

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url;
    req.bearer = vc->cfg.token;
    req.pinned_pubkey = vc->cfg.pinned_pubkey[0] ? vc->cfg.pinned_pubkey : NULL;
    req.dest_path = dest_path;
    req.resume_from = have;
    req.progress = cb;
    req.progress_ud = ud;

    memset(&res, 0, sizeof(res));
    if (vapor_net_perform(&req, &res) != 0) {
        if (res.aborted) {
            vapor_client_set_error(vc, "download cancelled");
        } else {
            vapor_client_set_error(vc, "%s",
                                   res.err[0] ? res.err : "download failed");
        }
        vapor_net_res_free(&res);
        return -1;
    }

    if (res.status != 200 && res.status != 206) {
        vapor_response tmp;
        tmp.status = res.status;
        tmp.body = res.body;
        tmp.body_len = res.body_len;
        absorb_api_error(vc, &tmp);
        vapor_net_res_free(&res);
        return -1;
    }

    vapor_net_res_free(&res);
    return 0;
}
