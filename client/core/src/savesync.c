/* Account save sync. Paths come only from the manifest `saves` list. */

#include "vapor/client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "platform.h"
#include "vapor/saves.h"
#include "vapor/sha256.h"

static int
join2(char *out, size_t outsz, const char *dir, const char *leaf)
{
    int n = snprintf(out, outsz, "%s/%s", dir, leaf);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

static int
state_paths(const char *install_dir, char *dir, size_t dirsz, char *state,
            size_t statesz)
{
    if (join2(dir, dirsz, install_dir, ".vapor") != 0
        || join2(state, statesz, dir, "saves-state.json") != 0) {
        return -1;
    }
    return 0;
}

static int
read_state(const char *path, int64_t *revision, char *sha, size_t shasz)
{
    char  *text;
    size_t len = 0;
    cJSON *root;
    const cJSON *rev;
    const cJSON *hash;

    *revision = 0;
    if (sha && shasz) {
        sha[0] = '\0';
    }
    text = vapor_read_file(path, &len);
    if (!text) {
        return 1;
    }
    root = cJSON_ParseWithLength(text, len);
    free(text);
    if (!root) {
        return 1;
    }
    rev = cJSON_GetObjectItemCaseSensitive(root, "revision");
    hash = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    if (cJSON_IsNumber(rev) && rev->valuedouble >= 0) {
        *revision = (int64_t)rev->valuedouble;
    }
    if (sha && shasz && cJSON_IsString(hash) && hash->valuestring) {
        snprintf(sha, shasz, "%s", hash->valuestring);
    }
    cJSON_Delete(root);
    return 0;
}

static int
write_state(const char *dir, const char *path, int64_t revision, const char *sha)
{
    cJSON *root;
    char  *text;
    FILE  *f;

    if (vapor_plat_mkdirs(dir) != 0) {
        return -1;
    }
    root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    cJSON_AddNumberToObject(root, "revision", (double)revision);
    cJSON_AddStringToObject(root, "sha256", sha ? sha : "");
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return -1;
    }
    f = fopen(path, "w");
    if (!f) {
        free(text);
        return -1;
    }
    fputs(text, f);
    fclose(f);
    free(text);
    return 0;
}

static int
parse_meta(const vapor_response *r, int64_t *revision, char *sha, size_t shasz)
{
    cJSON *root;
    const cJSON *rev;
    const cJSON *hash;

    root = cJSON_ParseWithLength(r->body ? r->body : "", r->body_len);
    if (!root) {
        return -1;
    }
    rev = cJSON_GetObjectItemCaseSensitive(root, "revision");
    hash = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    if (!cJSON_IsNumber(rev) || rev->valuedouble < 0 || !cJSON_IsString(hash)
        || !hash->valuestring) {
        cJSON_Delete(root);
        return -1;
    }
    *revision = (int64_t)rev->valuedouble;
    snprintf(sha, shasz, "%s", hash->valuestring);
    cJSON_Delete(root);
    return 0;
}

/* 0 and fills revision/sha, 1 when the account has no save, -1 on error. */
static int
fetch_meta(vapor_client *vc, const char *game_id, int64_t *revision, char *sha,
           size_t shasz)
{
    char            path[256];
    vapor_response  r;
    int             rc;

    snprintf(path, sizeof(path), "/api/v1/me/saves/%s", game_id);
    memset(&r, 0, sizeof(r));
    if (vapor_http_request(vc, "GET", path, NULL, 1, &r) != 0) {
        return -1;
    }
    if (r.status == 404) {
        vapor_response_free(&r);
        *revision = 0;
        sha[0] = '\0';
        return 1;
    }
    if (r.status < 200 || r.status >= 300) {
        if (r.body && r.body_len) {
            cJSON *obj = cJSON_ParseWithLength(r.body, r.body_len);
            const cJSON *msg = obj ? cJSON_GetObjectItemCaseSensitive(obj, "message")
                                   : NULL;
            if (cJSON_IsString(msg) && msg->valuestring) {
                vapor_client_set_error(vc, "%s", msg->valuestring);
            } else {
                vapor_client_set_error(vc, "server returned HTTP %ld", r.status);
            }
            cJSON_Delete(obj);
        } else {
            vapor_client_set_error(vc, "server returned HTTP %ld", r.status);
        }
        vapor_response_free(&r);
        return -1;
    }
    rc = parse_meta(&r, revision, sha, shasz);
    vapor_response_free(&r);
    if (rc != 0) {
        vapor_client_set_error(vc, "save metadata from the server is invalid");
        return -1;
    }
    return 0;
}

static int
read_file_bytes(const char *path, void **out, size_t *outn, char *err, size_t errsz)
{
    FILE  *f;
    long   n;
    char  *buf;

    *out = NULL;
    *outn = 0;
    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }
    n = ftell(f);
    if (n < 0 || (uint64_t)n > VAPOR_SAVE_SYNC_MAX_BYTES) {
        fclose(f);
        snprintf(err, errsz, "save archive is larger than 256 MiB");
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }
    buf = (char *)malloc(n ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }
    fclose(f);
    *out = buf;
    *outn = (size_t)n;
    return 0;
}

static int
put_archive(vapor_client *vc, const char *game_id, int64_t revision,
            const void *body, size_t len, int64_t *out_rev, char *out_sha,
            size_t shasz)
{
    char           path[320];
    vapor_response r;
    int            rc;

    snprintf(path, sizeof(path), "/api/v1/me/saves/%s?revision=%lld", game_id,
             (long long)revision);
    memset(&r, 0, sizeof(r));
    if (vapor_http_put_bytes(vc, path, body, len, 1, &r) != 0) {
        return -1;
    }
    if (r.status == 409) {
        vapor_response_free(&r);
        return 1;
    }
    if (r.status < 200 || r.status >= 300) {
        if (r.body && r.body_len) {
            cJSON *obj = cJSON_ParseWithLength(r.body, r.body_len);
            const cJSON *msg = obj ? cJSON_GetObjectItemCaseSensitive(obj, "message")
                                   : NULL;
            if (cJSON_IsString(msg) && msg->valuestring) {
                vapor_client_set_error(vc, "%s", msg->valuestring);
            } else {
                vapor_client_set_error(vc, "server returned HTTP %ld", r.status);
            }
            cJSON_Delete(obj);
        } else {
            vapor_client_set_error(vc, "server returned HTTP %ld", r.status);
        }
        vapor_response_free(&r);
        return -1;
    }
    rc = parse_meta(&r, out_rev, out_sha, shasz);
    vapor_response_free(&r);
    if (rc != 0) {
        vapor_client_set_error(vc, "save upload response is invalid");
        return -1;
    }
    return 0;
}

static int
sync_wanted(vapor_client *vc, const vapor_manifest *m)
{
    if (!m || m->nsaves == 0 || !m->id) {
        return 0;
    }
    if (!vapor_client_has_token(vc)) {
        return 0;
    }
    return 1;
}

int
vapor_saves_before_play(vapor_client *vc, const vapor_manifest *m,
                        const char *install_dir)
{
    char     dir[VAPOR_PATH_MAX];
    char     state[VAPOR_PATH_MAX];
    char     zip[VAPOR_PATH_MAX];
    char     api[256];
    char     local_sha[VAPOR_SHA256_HEX_LEN + 1];
    char     remote_sha[VAPOR_SHA256_HEX_LEN + 1];
    char     err[512];
    int64_t  local_rev = 0, remote_rev = 0;
    int      skipped = 0;
    int      meta;

    if (!sync_wanted(vc, m)) {
        return 0;
    }
    if (state_paths(install_dir, dir, sizeof(dir), state, sizeof(state)) != 0
        || join2(zip, sizeof(zip), dir, "saves-download.zip") != 0) {
        vapor_client_set_error(vc, "save path is too long");
        return -1;
    }
    read_state(state, &local_rev, local_sha, sizeof(local_sha));
    meta = fetch_meta(vc, m->id, &remote_rev, remote_sha, sizeof(remote_sha));
    if (meta < 0) {
        return -1;
    }
    if (meta == 1 || remote_rev == 0) {
        return 0;
    }
    if (remote_rev == local_rev && strcmp(remote_sha, local_sha) == 0) {
        return 0;
    }

    printf("syncing account saves for %s\n", m->id);
    fflush(stdout);
    remove(zip);
    snprintf(api, sizeof(api), "/api/v1/me/saves/%s/blob", m->id);
    if (vapor_http_download(vc, api, zip, NULL, NULL) != 0) {
        remove(zip);
        return -1;
    }
    err[0] = '\0';
    if (vapor_saves_unpack(zip, (const char *const *)m->saves, m->nsaves,
                           install_dir, &skipped, err, sizeof(err))
        != 0) {
        remove(zip);
        vapor_client_set_error(vc, "%s", err[0] ? err : "cannot restore saves");
        return -1;
    }
    remove(zip);
    if (skipped) {
        printf("account saves: %s\n", err[0] ? err : "a save path is missing");
        fflush(stdout);
        return 0;
    }
    if (write_state(dir, state, remote_rev, remote_sha) != 0) {
        vapor_client_set_error(vc, "cannot record the save revision");
        return -1;
    }
    return 0;
}

int
vapor_saves_after_play(vapor_client *vc, const vapor_manifest *m,
                       const char *install_dir)
{
    char     dir[VAPOR_PATH_MAX];
    char     state[VAPOR_PATH_MAX];
    char     zip[VAPOR_PATH_MAX];
    char     err[512];
    char     sha[VAPOR_SHA256_HEX_LEN + 1];
    char     local_sha[VAPOR_SHA256_HEX_LEN + 1];
    char     remote_sha[VAPOR_SHA256_HEX_LEN + 1];
    int64_t  local_rev = 0, new_rev = 0, remote_rev = 0;
    int      missing = 0;
    int      put;
    void    *body = NULL;
    size_t   body_len = 0;

    if (!sync_wanted(vc, m)) {
        return 0;
    }
    if (state_paths(install_dir, dir, sizeof(dir), state, sizeof(state)) != 0
        || join2(zip, sizeof(zip), dir, "saves-upload.zip") != 0) {
        vapor_client_set_error(vc, "save path is too long");
        return -1;
    }
    if (vapor_plat_mkdirs(dir) != 0) {
        vapor_client_set_error(vc, "cannot create %s", dir);
        return -1;
    }
    err[0] = '\0';
    remove(zip);
    if (vapor_saves_pack((const char *const *)m->saves, m->nsaves, install_dir,
                         zip, &missing, err, sizeof(err))
        != 0) {
        remove(zip);
        vapor_client_set_error(vc, "%s", err[0] ? err : "cannot pack saves");
        return -1;
    }
    if (missing) {
        remove(zip);
        printf("account saves: %s\n", err[0] ? err : "a save path is missing");
        fflush(stdout);
        return 0;
    }
    read_state(state, &local_rev, local_sha, sizeof(local_sha));
    if (vapor_sha256_file(zip, sha) != 0) {
        remove(zip);
        vapor_client_set_error(vc, "cannot hash save archive");
        return -1;
    }
    if (local_rev > 0 && strcmp(sha, local_sha) == 0) {
        remove(zip);
        return 0;
    }
    if (read_file_bytes(zip, &body, &body_len, err, sizeof(err)) != 0) {
        remove(zip);
        vapor_client_set_error(vc, "%s", err);
        return -1;
    }
    printf("uploading account saves for %s\n", m->id);
    fflush(stdout);
    put = put_archive(vc, m->id, local_rev, body, body_len, &new_rev, remote_sha,
                      sizeof(remote_sha));
    if (put == 1) {
        /* The other machine finished later. Take its revision and replace it,
         * which is the exiting player's copy. */
        if (fetch_meta(vc, m->id, &remote_rev, remote_sha, sizeof(remote_sha)) < 0) {
            free(body);
            remove(zip);
            return -1;
        }
        if (remote_rev == 0) {
            remote_rev = 0;
        }
        put = put_archive(vc, m->id, remote_rev, body, body_len, &new_rev,
                          remote_sha, sizeof(remote_sha));
    }
    free(body);
    remove(zip);
    if (put != 0) {
        if (!vc->err[0]) {
            vapor_client_set_error(vc, "could not upload saves");
        }
        return -1;
    }
    if (write_state(dir, state, new_rev, remote_sha) != 0) {
        vapor_client_set_error(vc, "saves uploaded, but the local revision "
                                   "could not be recorded");
        return -1;
    }
    return 0;
}
