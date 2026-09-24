#ifndef VAPOR_CLIENT_H
#define VAPOR_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "sqlite3.h"
#include "vapor/manifest.h"
#include "vapor/protocol.h"
#include "vapor/util.h"

#define VAPOR_PATH_MAX 1024

typedef struct {
    char    server_url[512];   /* scheme://host[:port], no trailing slash */
    char    username[VAPOR_USERNAME_MAX + 1];
    char    token[VAPOR_TOKEN_HEX_LEN + 1];
    int64_t token_expires_at;
    char    library_dir[VAPOR_PATH_MAX];
    /* Base64 "sha256//..." public-key pin, for a server using a self-signed
     * certificate instead of a CA-issued one. Empty means normal CA checks. */
    char    pinned_pubkey[256];
} vapor_client_config;

typedef struct {
    vapor_client_config cfg;
    char     data_dir[VAPOR_PATH_MAX];
    char     config_path[VAPOR_PATH_MAX];
    char     db_path[VAPOR_PATH_MAX];
    sqlite3 *db;
    char     err[1024];   /* last human-readable error */
} vapor_client;

/* ------------------------------------------------------------------ config */
/* Resolves the per-user data dir, loads config if present, opens the local db.
 * Returns 0 on success; on failure vc->err explains why. */
int  vapor_client_open(vapor_client *vc);
void vapor_client_close(vapor_client *vc);
int  vapor_client_save_config(vapor_client *vc);
void vapor_client_set_error(vapor_client *vc, const char *fmt, ...);
int  vapor_client_has_token(const vapor_client *vc);
/* Validates and applies `url` in memory. Does not persist; call
 * vapor_client_save_config after. A change of host clears the stored token
 * so a leftover session cannot be sent to the wrong server. */
int  vapor_client_set_server_url(vapor_client *vc, const char *url);

/* --------------------------------------------------------------------- net */
typedef struct {
    long   status;
    char  *body;
    size_t body_len;
} vapor_response;

void vapor_response_free(vapor_response *r);

/* `path` is an absolute API path such as "/api/v1/games". `json_body` may be
 * NULL. When `auth` is non-zero the stored bearer token is attached. Returns 0
 * if a response was received at all (check r->status), -1 on transport error. */
int vapor_http_request(vapor_client *vc, const char *method, const char *path,
                       const char *json_body, int auth, vapor_response *out);

/* Convenience wrappers that also surface a server-side JSON "message" into
 * vc->err when the status is not 2xx. Return 0 only on 2xx. */
int vapor_api_get(vapor_client *vc, const char *path, int auth,
                  vapor_response *out);
int vapor_api_post(vapor_client *vc, const char *path, const char *json_body,
                   int auth, vapor_response *out);
int vapor_api_put(vapor_client *vc, const char *path, const char *json_body,
                  int auth, vapor_response *out);

/* Return non-zero to abort. `done`/`total` are an overall work fraction for
 * install (download through setup), not download bytes alone. */
typedef int (*vapor_progress_fn)(void *ud, uint64_t done, uint64_t total);
/* Optional phase label while install runs (download, verify, extract, setup). */
typedef void (*vapor_status_fn)(void *ud, const char *status);

/* Resumable GET straight to `dest_path`. If the file already exists it is
 * continued with a Range request rather than restarted. */
int vapor_http_download(vapor_client *vc, const char *path,
                        const char *dest_path, vapor_progress_fn cb, void *ud);

/* GET an absolute https URL to `dest_path` (no vapord session). Used for
 * public source-port runtimes. */
int vapor_http_fetch_url(vapor_client *vc, const char *url,
                         const char *dest_path, vapor_progress_fn cb, void *ud);

/* -------------------------------------------------------------------- auth */
typedef struct {
    char username[VAPOR_USERNAME_MAX + 1];
    int  is_admin;
} vapor_account;

/* What GET /health reports about the other end. */
typedef struct {
    char service[64];
    char version[32];
    int  registration_open;
    int  has_users;   /* 0 means the next account registered becomes admin */
} vapor_server_info;

int vapor_server_ping(vapor_client *vc, vapor_server_info *out);
/* GET /health and require a vapord JSON body. Register and login call this
 * first so an account cannot be created or signed in against a dead URL. */
int vapor_require_server(vapor_client *vc, vapor_server_info *out);

int vapor_auth_register(vapor_client *vc, const char *username,
                        const char *password, vapor_account *out);
/* On success the token is stored in the config file, so a later run of any
 * frontend is already signed in. The server must be reachable. */
int vapor_auth_login(vapor_client *vc, const char *username,
                     const char *password);
int vapor_auth_logout(vapor_client *vc);
int vapor_auth_whoami(vapor_client *vc, vapor_account *out);

/* ----------------------------------------------------------------- catalog */
typedef struct {
    char     id[VAPOR_ID_MAX + 1];
    char     name[256];
    char     latest_version[VAPOR_VERSION_MAX + 1];
    char     developer[128];
    char     description[VAPOR_DESC_MAX];
    uint64_t size;             /* of the latest version's package */
    /* Merged in from the local database so one call answers both "what does
     * the server have" and "what do I have". */
    int      installed;
    char     installed_version[VAPOR_VERSION_MAX + 1];
    int      setup_pending;
    int      update_available;
    int64_t  play_seconds;
    int      has_cover;
    double   rating_avg;       /* local community average, 0 if no votes */
    int      rating_votes;
    int      my_rating;        /* 1-5, or 0 if this account has not rated */
    int      steam_rating_pct; /* 0 if unknown */
    int      steam_rating_count;
    char     steam_rating_label[48];
} vapor_catalog_entry;

/* Caller frees *out. */
int vapor_catalog_fetch(vapor_client *vc, vapor_catalog_entry **out,
                        size_t *count);

/* Downloads the cover for a game version into the local cache and writes its
 * path to `out_path`. A cached file is reused without touching the network.
 * Returns 0 on success, 1 if there is no cover, -1 on error. */
int vapor_fetch_cover(vapor_client *vc, const char *game_id,
                      const char *version, char *out_path, size_t outsz);

typedef struct {
    vapor_catalog_entry game;
    char              **versions;   /* newest first, as the server orders them */
    size_t              nversions;
} vapor_game_detail;

int  vapor_game_fetch(vapor_client *vc, const char *game_id,
                      vapor_game_detail *out);
void vapor_game_detail_free(vapor_game_detail *d);

typedef struct {
    int    my_rating;
    double rating_avg;
    int    rating_votes;
} vapor_rating;

/* `score` is 1-5. On success fills `out` when non-NULL. */
int vapor_game_rate(vapor_client *vc, const char *game_id, int score,
                    vapor_rating *out);

/* ---------------------------------------------------------------- local db */
typedef struct {
    char    game_id[VAPOR_ID_MAX + 1];
    char    version[VAPOR_VERSION_MAX + 1];
    char    name[256];
    char    install_dir[VAPOR_PATH_MAX];
    int64_t installed_at;
    int64_t last_played_at;
    int64_t play_seconds;
    uint64_t size_on_disk;
    int      setup_pending; /* 1 until a Windows installer has been run and tracked */
    char     payload_dir[VAPOR_PATH_MAX]; /* downloaded kit; empty means install_dir */
} vapor_install;

int vapor_db_record_install(vapor_client *vc, const vapor_install *rec);
int vapor_db_forget_install(vapor_client *vc, const char *game_id);
int vapor_db_get_install(vapor_client *vc, const char *game_id,
                         vapor_install *out);
/* Caller frees *out. */
int vapor_db_list_installs(vapor_client *vc, vapor_install **out, size_t *count);
int vapor_db_add_playtime(vapor_client *vc, const char *game_id,
                          int64_t started_at, int64_t seconds);

/* ----------------------------------------------------------------- install */
typedef struct {
    int   verify_only;
    int   force;          /* reinstall even if the version already matches */
    int   keep_download;  /* leave the archive in the cache after extracting */
    vapor_status_fn on_status;
    void           *on_status_ud;
} vapor_install_opts;

/* Fetches the manifest for `game_id` at `version` ("latest" is resolved by the
 * server), downloads, verifies, extracts, and records the install. `cb`
 * reports overall install progress (not only the HTTP transfer). */
int vapor_install_game(vapor_client *vc, const char *game_id,
                       const char *version, const vapor_install_opts *opts,
                       vapor_progress_fn cb, void *ud);
/* Removes a game the way it was installed. A Windows product match runs
 * that entry's uninstaller (or unins000.exe / uninstall.exe in the install
 * folder) before the recorded folders are deleted. */
int vapor_uninstall_game(vapor_client *vc, const char *game_id);

/* Run a Windows installer that was downloaded into the payload directory,
 * then record where it placed the game. 0 if the title is playable, 1 if the
 * installer did not finish (setup still pending; vc->err explains), -1 on
 * error. Completion is the destination tree (requested silent folder, or the
 * Uninstall / Program Files path the installer created), not the process
 * exit code alone. */
int vapor_setup_game(vapor_client *vc, const char *game_id);

/* Fetch and parse a manifest without installing. Caller frees via
 * vapor_manifest_free. */
int vapor_fetch_manifest(vapor_client *vc, const char *game_id,
                         const char *version, vapor_manifest *out);

/* Reads the manifest stored inside the install directory, so launching and
 * verifying work with the server unreachable. */
int vapor_read_local_manifest(vapor_client *vc, const char *game_id,
                              vapor_manifest *out);

/* Structural check of an install: manifest parses, version agrees with the
 * database, launch target present. 0 if clean, 1 if problems, -1 on error. */
int vapor_verify_install(vapor_client *vc, const char *game_id);

/* ------------------------------------------------------------------ launch */
/* Runs the game and blocks until it exits. *out_exit gets the process exit
 * code. Playtime is recorded automatically. Copy-protected disc wrappers
 * (SafeDisc/SECDRV) are not spawned; a patched or source-port exe is used
 * when one is present. */
int vapor_launch_game(vapor_client *vc, const char *game_id, int *out_exit);

#endif /* VAPOR_CLIENT_H */
