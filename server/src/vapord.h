#ifndef VAPORD_H
#define VAPORD_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "sqlite3.h"

#include "vapor/manifest.h"
#include "vapor/protocol.h"
#include "vapor/sha256.h"
#include "vapor/util.h"

/* Forward-declared rather than including civetweb.h: vapor-admin reuses the
 * config/db/auth code below and has no business linking a web server. */
struct mg_connection;

#define VAPORD_PATH_MAX 1024

typedef struct {
    int  port;
    char bind_addr[64];
    char content_root[VAPORD_PATH_MAX];
    /* Drop folder of game directories. Empty means auto-discovery is off. */
    char library_root[VAPORD_PATH_MAX];
    char db_path[VAPORD_PATH_MAX];
    int  enable_registration;
    int  num_threads;
    /* Seconds between library scans. 0 = only once at startup / on demand. */
    int  discover_interval;
} vapord_config;

typedef struct {
    vapord_config cfg;
    sqlite3      *db;
} vapord;

/* ------------------------------------------------------------------ config */
void vapord_config_defaults(vapord_config *c);
int  vapord_config_load(vapord_config *c, const char *path,
                        char *err, size_t errsz);
void vapord_config_print(const vapord_config *c);

/* Extra log sink for the host window. stderr is still written. */
void vapord_set_log_sink(void (*fn)(const char *line, void *ud), void *ud);

/* ---------------------------------------------------------------------- db */
int  vapord_db_open(vapord *app, char *err, size_t errsz);
void vapord_db_close(vapord *app);

int vapord_user_create(sqlite3 *db, const char *username, const char *pwhash,
                       int is_admin, int64_t *out_id);
int vapord_user_lookup(sqlite3 *db, const char *username, int64_t *out_id,
                       char *pwhash, size_t pwhash_sz, int *is_admin);
int vapord_user_name(sqlite3 *db, int64_t user_id, char *out, size_t outsz,
                     int *is_admin, int64_t *created_at);
int vapord_user_count(sqlite3 *db, int64_t *out);
int vapord_game_count(sqlite3 *db, int64_t *out);
/* Copies up to `cap` catalog titles (NUL-terminated, each `namesz` bytes). */
int vapord_game_titles(sqlite3 *db, char *names, size_t namesz, size_t cap,
                       size_t *out_n);

int vapord_token_store(sqlite3 *db, const char *token_hash, int64_t user_id,
                       int64_t created_at, int64_t expires_at);
int vapord_token_lookup(sqlite3 *db, const char *token_hash, int64_t now,
                        int64_t *out_user_id);
int vapord_token_delete(sqlite3 *db, const char *token_hash);
int vapord_token_prune(sqlite3 *db, int64_t now);

int vapord_game_upsert(sqlite3 *db, const vapor_manifest *m);
int vapord_version_upsert(sqlite3 *db, const vapor_manifest *m,
                          const char *manifest_json);
int vapord_game_delete(sqlite3 *db, const char *game_id);
int vapord_game_mark_discovered(sqlite3 *db, const char *game_id, int discovered);
/* 0 if the game exists (*discovered is 0/1), 1 if it does not. */
int vapord_game_lookup(sqlite3 *db, const char *game_id, int *discovered);
int vapord_version_set_source(sqlite3 *db, const char *game_id,
                              const char *version, const char *source_rel);
/* 0 if this version is served from library_root; 1 if it lives in content_root. */
int vapord_version_source(sqlite3 *db, const char *game_id, const char *version,
                          char *out, size_t outsz);

typedef struct {
    char folder[256];
    char game_id[VAPOR_ID_MAX + 1];
    char fingerprint[VAPOR_SHA256_HEX_LEN + 1];
    char version[VAPOR_VERSION_MAX + 1];
} vapord_discovered_row;

int vapord_discovered_get(sqlite3 *db, const char *folder,
                          vapord_discovered_row *out);
int vapord_discovered_put(sqlite3 *db, const vapord_discovered_row *row);
int vapord_discovered_delete(sqlite3 *db, const char *folder);
/* Caller frees *out. */
int vapord_discovered_list(sqlite3 *db, vapord_discovered_row **out, size_t *count);
int vapord_discovered_by_id(sqlite3 *db, const char *game_id, char *folder,
                            size_t foldersz);

/* Newest version of `game_id` by natural version order. 0 on success,
 * 1 if the game exists with no versions, -1 if it does not exist. */
int vapord_game_latest_version(sqlite3 *db, const char *game_id,
                               char *out, size_t outsz);
int vapord_version_manifest(sqlite3 *db, const char *game_id,
                            const char *version, char **out_json);

/* Cover image filename for a version. 0 on success, 1 if it has none. */
int vapord_version_cover(sqlite3 *db, const char *game_id, const char *version,
                         char *out, size_t outsz);

int vapord_game_meta_row(sqlite3 *db, const char *game_id,
                         char *developer, size_t devsz,
                         char *description, size_t descsz,
                         int *steam_appid, int64_t *fetched_at);
int vapord_game_set_meta(sqlite3 *db, const char *game_id,
                         const char *developer, const char *description,
                         int steam_appid, int rating_pct, int rating_count,
                         const char *rating_label);
/* Records that a Steam lookup was attempted, even when it found nothing. */
int vapord_game_touch_meta(sqlite3 *db, const char *game_id);
int vapord_version_set_cover(sqlite3 *db, const char *game_id,
                             const char *version, const char *cover);

int vapord_rating_set(sqlite3 *db, int64_t user_id, const char *game_id,
                      int score);
int vapord_rating_summary(sqlite3 *db, const char *game_id, int64_t user_id,
                          double *avg, int *votes, int *mine);

/* Catalog rendering. Both return a new cJSON the caller owns, or NULL.
 * `user_id` of 0 omits `my_rating`. */
cJSON *vapord_catalog_json(sqlite3 *db, int64_t user_id);
cJSON *vapord_game_json(sqlite3 *db, const char *game_id, int64_t user_id);

/* Steam store lookup: cover, description, developer, community score.
 * `hint_appid` comes from vapor.json (0 means search by name). `force`
 * ignores the fetch cache. Returns 1 if the network was used, 0 if skipped. */
int vapord_meta_enrich(vapord *app, const char *game_id, const char *name,
                       const char *version, int hint_appid, int force);
int vapord_meta_enrich_all(vapord *app, int force);

/* -------------------------------------------------------------------- auth */
int  vapord_auth_init(char *err, size_t errsz);
int  vapord_password_hash(const char *password, char *out, size_t outsz);
int  vapord_password_verify(const char *stored, const char *password);
void vapord_token_mint(char out[VAPOR_TOKEN_HEX_LEN + 1]);
void vapord_token_fingerprint(const char *token_hex,
                              char out[VAPOR_SHA256_HEX_LEN + 1]);

/* -------------------------------------------------------------------- http */
int   vapord_send_json(struct mg_connection *c, int status, cJSON *body);
int   vapord_send_errorf(struct mg_connection *c, int status, const char *code,
                         const char *fmt, ...);
int   vapord_send_empty(struct mg_connection *c, int status);
char *vapord_read_body(struct mg_connection *c, size_t *out_len);
cJSON *vapord_read_json(struct mg_connection *c, char *err, size_t errsz);

/* Borrowed pointer into the cJSON tree: only valid until the tree is freed. */
const char *vapord_json_string(const cJSON *obj, const char *key);

/* Copies a string field into a caller-owned buffer so it can outlive the JSON
 * tree. Returns 0 on success, -1 if the field is missing or not a string, and
 * 1 if the value does not fit (the caller should reject, not truncate). */
int vapord_json_copy_string(const cJSON *obj, const char *key, char *out,
                            size_t outsz);

/* Resolves the bearer token. Returns 1 and sets *user_id when authenticated;
 * returns 0 after having already written a 401 to the connection. */
int vapord_require_user(vapord *app, struct mg_connection *c, int64_t *user_id);

/* ------------------------------------------------------------------ routes */
/* Each takes the path remainder after its prefix. Return an HTTP status for
 * civetweb (the handlers write their own bodies). */
int vapord_route_auth(vapord *app, struct mg_connection *c,
                      const char *method, const char *tail);
int vapord_route_catalog(vapord *app, struct mg_connection *c,
                         const char *method, const char *tail);
int vapord_route_download(vapord *app, struct mg_connection *c,
                          const char *method, const char *tail);

/* ----------------------------------------------------------------- content */
/* Build <content_root>/<game>/<version>[/<file>]. Validates every segment, so
 * a hostile id or version can never escape the content root. */
int vapord_content_path(const vapord_config *cfg, const char *game_id,
                        const char *version, const char *file,
                        char *out, size_t outsz);
int vapord_content_mkdirs(const char *path);

/* Join library_root + a relative path (may contain subdirectories). Rejects
 * absolute paths, backslashes, and ".." so a stored source_path cannot walk
 * out of the drop folder. */
int vapord_library_path(const vapord_config *cfg, const char *rel,
                        char *out, size_t outsz);
/* realpath(library_root/rel) and confirm it still sits under library_root. */
int vapord_library_resolve(const vapord_config *cfg, const char *rel,
                           char *out, size_t outsz);

/* Scan library_root, register new or changed game folders, drop vanished ones.
 * Safe to call repeatedly; unchanged folders are skipped after a cheap
 * fingerprint compare so multi-gigabyte archives are not re-hashed.
 * Zip files in the drop folder are served in place. ISO files are unpacked
 * (and a bundled Wise SETUP.EXE is unpacked too) and the files are zipped
 * under content_root; the original disc image is left alone. CD leftovers
 * (autorun, and a tiny root *.DAT that has a larger namesake below) are
 * stripped so the tree matches a SETUP install. */
int vapord_discover(vapord *app);

/* Host window (SDL + Nuklear). Closing it stops the server. Returns 0 after
 * a clean quit, or -1 if a display could not be opened. */
int vapord_gui_run(vapord *app, volatile int *stop);

/* --------------------------------------------------------------------- log */
void vapord_log(const char *level, const char *fmt, ...);
#define VLOG_INFO(...)  vapord_log("info", __VA_ARGS__)
#define VLOG_WARN(...)  vapord_log("warn", __VA_ARGS__)
#define VLOG_ERROR(...) vapord_log("error", __VA_ARGS__)

#endif /* VAPORD_H */
