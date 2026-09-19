/* Account operations. These live in the core rather than in a frontend so the
 * CLI and the GUI share one definition of "signed in", including where the
 * token is persisted. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "json.h"
#include "vapor/client.h"
#include "vapor/util.h"

static char *
credentials_json(const char *username, const char *password)
{
    cJSON *obj = cJSON_CreateObject();
    char  *text;

    if (!obj) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "username", username);
    cJSON_AddStringToObject(obj, "password", password);
    text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return text;
}

/* Rejects locally what the server would reject anyway, so both frontends give
 * the same message without a round trip. */
static int
check_credentials(vapor_client *vc, const char *username, const char *password)
{
    if (!vapor_username_is_valid(username)) {
        vapor_client_set_error(vc,
                               "username must be %d-%d characters of letters, "
                               "digits, dot, dash or underscore",
                               VAPOR_USERNAME_MIN, VAPOR_USERNAME_MAX);
        return -1;
    }
    if (!password || strlen(password) < VAPOR_PASSWORD_MIN) {
        vapor_client_set_error(vc, "the password must be at least %d characters",
                               VAPOR_PASSWORD_MIN);
        return -1;
    }
    if (strlen(password) > VAPOR_PASSWORD_MAX) {
        vapor_client_set_error(vc, "the password may be at most %d characters",
                               VAPOR_PASSWORD_MAX);
        return -1;
    }
    return 0;
}

int
vapor_server_ping(vapor_client *vc, vapor_server_info *out)
{
    vapor_response r;
    cJSON         *body;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (vapor_api_get(vc, VAPOR_EP_HEALTH, 0, &r) != 0) {
        vapor_response_free(&r);
        return -1;
    }
    body = vapor_json_parse_response(&r);
    vapor_response_free(&r);
    if (!body) {
        /* A 2xx with an unreadable body still proves the server is up. */
        return 0;
    }
    if (out) {
        vapor_json_copy(out->service, sizeof(out->service), body, "service", "");
        vapor_json_copy(out->version, sizeof(out->version), body, "version", "");
        out->registration_open = vapor_json_bool(body, "registration_open", 1);
        out->has_users = vapor_json_bool(body, "has_users", 1);
    }
    cJSON_Delete(body);
    return 0;
}

int
vapor_require_server(vapor_client *vc, vapor_server_info *out)
{
    vapor_server_info  info;
    vapor_server_info *dst = out ? out : &info;

    memset(dst, 0, sizeof(*dst));
    if (!vc->cfg.server_url[0]) {
        vapor_client_set_error(vc, "no server configured; set a server URL first");
        return -1;
    }
    if (vapor_server_ping(vc, dst) != 0) {
        if (!vc->err[0]) {
            vapor_client_set_error(vc, "cannot reach the server at %s",
                                   vc->cfg.server_url);
        }
        return -1;
    }
    /* Ping treats any 2xx as success. Register and login need a real vapord,
     * not an empty 200 from whatever is bound to that port. */
    if (strcmp(dst->service, "vapord") != 0) {
        vapor_client_set_error(vc,
                               "no Vapor server at %s; check the URL and that "
                               "vapord is running",
                               vc->cfg.server_url);
        return -1;
    }
    return 0;
}

int
vapor_auth_register(vapor_client *vc, const char *username,
                    const char *password, vapor_account *out)
{
    vapor_response    r;
    vapor_server_info info;
    cJSON            *body;
    char             *payload;
    const char       *stored_name;
    int               rc;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (vapor_require_server(vc, &info) != 0) {
        return -1;
    }
    if (!info.registration_open) {
        vapor_client_set_error(vc, "registration is closed on this server");
        return -1;
    }
    if (check_credentials(vc, username, password) != 0) {
        return -1;
    }
    payload = credentials_json(username, password);
    if (!payload) {
        vapor_client_set_error(vc, "out of memory");
        return -1;
    }
    rc = vapor_api_post(vc, VAPOR_EP_REGISTER, payload, 0, &r);
    vapor_secure_zero(payload, strlen(payload));
    free(payload);
    if (rc != 0) {
        vapor_response_free(&r);
        return -1;
    }

    body = vapor_json_parse_response(&r);
    vapor_response_free(&r);
    stored_name = body ? vapor_json_str(body, "username", NULL) : NULL;
    if (!stored_name || !*stored_name) {
        vapor_client_set_error(vc, "the server did not confirm the new account");
        cJSON_Delete(body);
        return -1;
    }
    if (out) {
        vapor_json_copy(out->username, sizeof(out->username), body, "username",
                        stored_name);
        out->is_admin = vapor_json_bool(body, "is_admin", 0);
    }
    cJSON_Delete(body);
    return 0;
}

int
vapor_auth_login(vapor_client *vc, const char *username, const char *password)
{
    vapor_response r;
    cJSON         *body;
    char          *payload;
    const char    *token;
    int            rc;

    if (vapor_require_server(vc, NULL) != 0) {
        return -1;
    }
    if (check_credentials(vc, username, password) != 0) {
        return -1;
    }
    payload = credentials_json(username, password);
    if (!payload) {
        vapor_client_set_error(vc, "out of memory");
        return -1;
    }
    rc = vapor_api_post(vc, VAPOR_EP_LOGIN, payload, 0, &r);
    vapor_secure_zero(payload, strlen(payload));
    free(payload);
    if (rc != 0) {
        vapor_response_free(&r);
        return -1;
    }

    body = vapor_json_parse_response(&r);
    vapor_response_free(&r);
    token = body ? vapor_json_str(body, "token", NULL) : NULL;
    if (!token || strlen(token) != VAPOR_TOKEN_HEX_LEN) {
        vapor_client_set_error(vc, "the server did not return a usable token");
        cJSON_Delete(body);
        return -1;
    }

    snprintf(vc->cfg.token, sizeof(vc->cfg.token), "%s", token);
    vapor_json_copy(vc->cfg.username, sizeof(vc->cfg.username), body, "username",
                    username);
    vc->cfg.token_expires_at = (int64_t)vapor_json_num(body, "expires_at", 0);
    cJSON_Delete(body);

    if (vapor_client_save_config(vc) != 0) {
        /* Keep the in-memory token: this process can still work, the user just
         * has to sign in again next time. */
        vapor_client_set_error(vc, "signed in, but the token could not be "
                                   "saved: %s", vc->err);
        return -1;
    }
    return 0;
}

int
vapor_auth_logout(vapor_client *vc)
{
    vapor_response r;
    int            warned = 0;

    if (!vapor_client_has_token(vc)) {
        return 0;
    }
    /* Best-effort revocation. The local token is dropped either way, so an
     * unreachable server cannot leave the client stuck signed in. */
    if (vapor_api_post(vc, VAPOR_EP_LOGOUT, NULL, 1, &r) != 0) {
        warned = 1;
    }
    vapor_response_free(&r);

    vapor_secure_zero(vc->cfg.token, sizeof(vc->cfg.token));
    vc->cfg.token_expires_at = 0;
    if (vapor_client_save_config(vc) != 0) {
        return -1;
    }
    return warned ? 1 : 0;
}

int
vapor_auth_whoami(vapor_client *vc, vapor_account *out)
{
    vapor_response r;
    cJSON         *body;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!vapor_client_has_token(vc)) {
        vapor_client_set_error(vc, "not signed in");
        return -1;
    }
    if (vapor_api_get(vc, VAPOR_EP_ME, 1, &r) != 0) {
        vapor_response_free(&r);
        return -1;
    }
    body = vapor_json_parse_response(&r);
    vapor_response_free(&r);
    if (!body) {
        vapor_client_set_error(vc, "could not read the account response");
        return -1;
    }
    if (out) {
        vapor_json_copy(out->username, sizeof(out->username), body, "username",
                        vc->cfg.username);
        out->is_admin = vapor_json_bool(body, "is_admin", 0);
    }
    cJSON_Delete(body);
    return 0;
}
