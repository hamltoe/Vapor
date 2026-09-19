#include "vapord.h"

#include "civetweb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "vapor/util.h"

static int
username_is_valid(const char *u)
{
    size_t i, n;

    if (!u) {
        return 0;
    }
    n = strlen(u);
    if (n < VAPOR_USERNAME_MIN || n > VAPOR_USERNAME_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char ch = u[i];
        int  ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
               || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

static int
handle_register(vapord *app, struct mg_connection *c)
{
    cJSON      *body;
    char        err[256];
    char        username[VAPOR_USERNAME_MAX + 1];
    char        password[VAPOR_PASSWORD_MAX + 1];
    char        pwhash[512];
    int64_t     user_id = 0, existing = 0;
    int         is_first_user, rc, status, got_user, got_pass;

    cJSON      *out;

    if (!app->cfg.enable_registration) {
        return vapord_send_errorf(c, 403, VAPOR_ERR_DISABLED,
                                  "registration is closed on this server");
    }

    body = vapord_read_json(c, err, sizeof(err));
    if (!body) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "%s", err);
    }

    /* Copied out of the JSON tree before it is freed: everything below this
     * point outlives `body`. */
    got_user = vapord_json_copy_string(body, "username", username,
                                       sizeof(username));
    got_pass = vapord_json_copy_string(body, "password", password,
                                       sizeof(password));
    cJSON_Delete(body);

    if (got_user != 0 || !username_is_valid(username)) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST,
                                  "username must be %d-%d chars of letters, "
                                  "digits, dot, dash or underscore",
                                  VAPOR_USERNAME_MIN, VAPOR_USERNAME_MAX);
    }
    if (got_pass != 0 || strlen(password) < VAPOR_PASSWORD_MIN) {
        sodium_memzero(password, sizeof(password));
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST,
                                  "password must be %d-%d characters",
                                  VAPOR_PASSWORD_MIN, VAPOR_PASSWORD_MAX);
    }

    /* The very first account becomes the admin, which avoids a chicken-and-egg
     * bootstrap step on a fresh server. */
    if (vapord_user_count(app->db, &existing) != 0) {
        sodium_memzero(password, sizeof(password));
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "database error");
    }
    is_first_user = (existing == 0);

    rc = vapord_password_hash(password, pwhash, sizeof(pwhash));
    sodium_memzero(password, sizeof(password));
    if (rc != 0) {
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                  "could not hash password");
    }

    rc = vapord_user_create(app->db, username, pwhash, is_first_user, &user_id);
    sodium_memzero(pwhash, sizeof(pwhash));

    if (rc == 1) {
        return vapord_send_errorf(c, 409, VAPOR_ERR_CONFLICT,
                                  "username is already taken");
    }
    if (rc != 0) {
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "database error");
    }

    VLOG_INFO("registered user \"%s\" (id %lld%s)", username, (long long)user_id,
              is_first_user ? ", admin" : "");

    out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "username", username);
    cJSON_AddNumberToObject(out, "user_id", (double)user_id);
    cJSON_AddBoolToObject(out, "is_admin", is_first_user);
    status = vapord_send_json(c, 201, out);
    return status;
}

static int
handle_login(vapord *app, struct mg_connection *c)
{
    cJSON      *body, *out;
    char        err[256];
    char        username[VAPOR_USERNAME_MAX + 1];
    char        password[VAPOR_PASSWORD_MAX + 1];
    char        stored[512];
    char        token[VAPOR_TOKEN_HEX_LEN + 1];
    char        fingerprint[VAPOR_SHA256_HEX_LEN + 1];
    int64_t     user_id = 0, now, expires;
    int         is_admin = 0, rc, got_user, got_pass;

    body = vapord_read_json(c, err, sizeof(err));
    if (!body) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "%s", err);
    }
    got_user = vapord_json_copy_string(body, "username", username,
                                       sizeof(username));
    got_pass = vapord_json_copy_string(body, "password", password,
                                       sizeof(password));
    cJSON_Delete(body);

    if (got_user != 0 || got_pass != 0) {
        sodium_memzero(password, sizeof(password));
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST,
                                  "username and password are required");
    }

    rc = vapord_user_lookup(app->db, username, &user_id, stored, sizeof(stored),
                            &is_admin);
    if (rc < 0) {
        sodium_memzero(password, sizeof(password));
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "database error");
    }
    if (rc == 1) {
        /* Same response and roughly the same cost as a bad password, so the
         * endpoint does not become a username oracle. */
        char dummy[512];
        vapord_password_hash(password, dummy, sizeof(dummy));
        sodium_memzero(dummy, sizeof(dummy));
        sodium_memzero(password, sizeof(password));
        VLOG_WARN("failed login for unknown user \"%s\"", username);
        return vapord_send_errorf(c, 401, VAPOR_ERR_UNAUTHORIZED,
                                  "invalid username or password");
    }

    rc = vapord_password_verify(stored, password);
    sodium_memzero(password, sizeof(password));
    sodium_memzero(stored, sizeof(stored));
    if (rc != 0) {
        VLOG_WARN("failed login for \"%s\"", username);
        return vapord_send_errorf(c, 401, VAPOR_ERR_UNAUTHORIZED,
                                  "invalid username or password");
    }

    now = vapor_now_unix();
    expires = now + VAPOR_TOKEN_TTL_SECONDS;
    vapord_token_mint(token);
    vapord_token_fingerprint(token, fingerprint);

    if (vapord_token_store(app->db, fingerprint, user_id, now, expires) != 0) {
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL,
                                  "could not create session");
    }
    vapord_token_prune(app->db, now);

    VLOG_INFO("login for \"%s\" (id %lld)", username, (long long)user_id);

    out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "token", token);
    cJSON_AddNumberToObject(out, "expires_at", (double)expires);
    cJSON_AddStringToObject(out, "username", username);
    cJSON_AddNumberToObject(out, "user_id", (double)user_id);
    cJSON_AddBoolToObject(out, "is_admin", is_admin);
    return vapord_send_json(c, 200, out);
}

static int
handle_logout(vapord *app, struct mg_connection *c)
{
    const char *hdr = mg_get_header(c, "Authorization");
    char        fingerprint[VAPOR_SHA256_HEX_LEN + 1];
    const char *token;

    if (!hdr || !vapor_str_has_prefix(hdr, "Bearer ")) {
        return vapord_send_errorf(c, 401, VAPOR_ERR_UNAUTHORIZED,
                                  "missing Bearer token");
    }
    token = hdr + 7;
    while (*token == ' ') {
        token++;
    }
    if (strlen(token) != VAPOR_TOKEN_HEX_LEN) {
        return vapord_send_errorf(c, 401, VAPOR_ERR_UNAUTHORIZED,
                                  "malformed token");
    }

    vapord_token_fingerprint(token, fingerprint);
    vapord_token_delete(app->db, fingerprint);
    /* Idempotent: logging out twice is not an error worth surfacing. */
    return vapord_send_empty(c, 204);
}

static int
handle_me(vapord *app, struct mg_connection *c)
{
    int64_t user_id = 0, created_at = 0;
    char    username[VAPOR_USERNAME_MAX + 1];
    int     is_admin = 0;
    cJSON  *out;

    if (!vapord_require_user(app, c, &user_id)) {
        return 401;
    }
    if (vapord_user_name(app->db, user_id, username, sizeof(username),
                         &is_admin, &created_at)
        != 0) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such user");
    }

    out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "user_id", (double)user_id);
    cJSON_AddStringToObject(out, "username", username);
    cJSON_AddBoolToObject(out, "is_admin", is_admin);
    cJSON_AddNumberToObject(out, "created_at", (double)created_at);
    return vapord_send_json(c, 200, out);
}

int
vapord_route_auth(vapord *app, struct mg_connection *c, const char *method,
                  const char *tail)
{
    int is_post = strcmp(method, "POST") == 0;
    int is_get  = strcmp(method, "GET") == 0;

    if (strcmp(tail, "/auth/register") == 0) {
        if (!is_post) {
            return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST,
                                      "use POST");
        }
        return handle_register(app, c);
    }
    if (strcmp(tail, "/auth/login") == 0) {
        if (!is_post) {
            return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST, "use POST");
        }
        return handle_login(app, c);
    }
    if (strcmp(tail, "/auth/logout") == 0) {
        if (!is_post) {
            return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST, "use POST");
        }
        return handle_logout(app, c);
    }
    if (strcmp(tail, "/me") == 0) {
        if (!is_get) {
            return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST, "use GET");
        }
        return handle_me(app, c);
    }
    return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
}
