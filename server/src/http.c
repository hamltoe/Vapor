#include "vapord.h"

#include "civetweb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/buf.h"
#include "vapor/util.h"

/* Takes ownership of `body`. */
int
vapord_send_json(struct mg_connection *c, int status, cJSON *body)
{
    char *text;
    char  len[32];
    size_t n;

    text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (!text) {
        mg_send_http_error(c, 500, "%s", "out of memory");
        return 500;
    }

    n = strlen(text);
    snprintf(len, sizeof(len), "%llu", (unsigned long long)n);

    mg_response_header_start(c, status);
    mg_response_header_add(c, "Content-Type", "application/json; charset=utf-8", -1);
    mg_response_header_add(c, "Content-Length", len, -1);
    mg_response_header_add(c, "Cache-Control", "no-store", -1);
    mg_response_header_send(c);
    mg_write(c, text, n);

    free(text);
    return status;
}

int
vapord_send_errorf(struct mg_connection *c, int status, const char *code,
                   const char *fmt, ...)
{
    cJSON  *obj;
    char    msg[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    obj = cJSON_CreateObject();
    if (!obj) {
        mg_send_http_error(c, status, "%s", msg);
        return status;
    }
    cJSON_AddStringToObject(obj, "error", code);
    cJSON_AddStringToObject(obj, "message", msg);
    return vapord_send_json(c, status, obj);
}

int
vapord_send_empty(struct mg_connection *c, int status)
{
    mg_response_header_start(c, status);
    mg_response_header_add(c, "Content-Length", "0", -1);
    mg_response_header_send(c);
    return status;
}

char *
vapord_read_body(struct mg_connection *c, size_t *out_len)
{
    const struct mg_request_info *ri = mg_get_request_info(c);
    vapor_buf                     b;
    char                          tmp[4096];
    int                           n;

    if (out_len) {
        *out_len = 0;
    }
    if (ri->content_length > (long long)VAPOR_MAX_REQUEST_BODY) {
        return NULL;
    }

    vapor_buf_init(&b);
    /* Materialise the allocation so a zero-length body still returns non-NULL
     * and callers can distinguish "empty" from "failed". */
    if (vapor_buf_append(&b, "", 0) != 0) {
        return NULL;
    }

    while ((n = mg_read(c, tmp, sizeof(tmp))) > 0) {
        if (b.len + (size_t)n > VAPOR_MAX_REQUEST_BODY) {
            vapor_buf_free(&b);
            return NULL;
        }
        if (vapor_buf_append(&b, tmp, (size_t)n) != 0) {
            vapor_buf_free(&b);
            return NULL;
        }
    }

    if (out_len) {
        *out_len = b.len;
    }
    return vapor_buf_release(&b);
}

cJSON *
vapord_read_json(struct mg_connection *c, char *err, size_t errsz)
{
    char  *body;
    size_t len = 0;
    cJSON *obj;

    body = vapord_read_body(c, &len);
    if (!body) {
        snprintf(err, errsz, "request body missing or larger than %d bytes",
                 VAPOR_MAX_REQUEST_BODY);
        return NULL;
    }
    if (len == 0) {
        free(body);
        snprintf(err, errsz, "request body is empty");
        return NULL;
    }

    obj = cJSON_ParseWithLength(body, len);
    free(body);
    if (!obj) {
        snprintf(err, errsz, "request body is not valid JSON");
        return NULL;
    }
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        snprintf(err, errsz, "request body must be a JSON object");
        return NULL;
    }
    return obj;
}

const char *
vapord_json_string(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        return it->valuestring;
    }
    return NULL;
}

int
vapord_json_copy_string(const cJSON *obj, const char *key, char *out,
                        size_t outsz)
{
    const char *v = vapord_json_string(obj, key);
    size_t      n;

    if (!v) {
        return -1;
    }
    n = strlen(v);
    if (n >= outsz) {
        return 1;
    }
    memcpy(out, v, n + 1);
    return 0;
}

int
vapord_require_user(vapord *app, struct mg_connection *c, int64_t *user_id)
{
    const char *hdr = mg_get_header(c, "Authorization");
    char        fingerprint[VAPOR_SHA256_HEX_LEN + 1];
    const char *token;
    size_t      n;

    if (!hdr || !vapor_str_has_prefix(hdr, "Bearer ")) {
        vapord_send_errorf(c, 401, VAPOR_ERR_UNAUTHORIZED,
                           "missing Bearer token");
        return 0;
    }
    token = hdr + 7;
    while (*token == ' ') {
        token++;
    }

    /* Reject anything that is not exactly a hex token before touching the db. */
    n = strlen(token);
    if (n != VAPOR_TOKEN_HEX_LEN) {
        vapord_send_errorf(c, 401, VAPOR_ERR_UNAUTHORIZED, "malformed token");
        return 0;
    }

    vapord_token_fingerprint(token, fingerprint);
    if (vapord_token_lookup(app->db, fingerprint, vapor_now_unix(), user_id) != 0) {
        vapord_send_errorf(c, 401, VAPOR_ERR_UNAUTHORIZED,
                           "token is invalid or expired");
        return 0;
    }
    return 1;
}
