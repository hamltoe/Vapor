#include "vapor/client.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "platform.h"
#include "vapor/util.h"

void
vapor_client_set_error(vapor_client *vc, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(vc->err, sizeof(vc->err), fmt, ap);
    va_end(ap);
}

int
vapor_client_has_token(const vapor_client *vc)
{
    if (strlen(vc->cfg.token) != VAPOR_TOKEN_HEX_LEN) {
        return 0;
    }
    /* Treat an expired token as absent so the CLI says "log in" instead of
     * bouncing off a 401. */
    if (vc->cfg.token_expires_at > 0
        && vc->cfg.token_expires_at <= vapor_now_unix()) {
        return 0;
    }
    return 1;
}

static char *
trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static void
strip_trailing_slash(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
        s[--n] = '\0';
    }
}

static int
load_config_file(vapor_client *vc)
{
    FILE *f;
    char  line[VAPOR_PATH_MAX + 128];

    f = fopen(vc->config_path, "r");
    if (!f) {
        return 1; /* absent is fine; defaults apply */
    }

    while (fgets(line, sizeof(line), f)) {
        char *key, *val, *eq;

        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';') {
            continue;
        }
        eq = strchr(key, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        key = trim(key);
        val = trim(eq + 1);

        if (strcmp(key, "server_url") == 0) {
            snprintf(vc->cfg.server_url, sizeof(vc->cfg.server_url), "%s", val);
            strip_trailing_slash(vc->cfg.server_url);
        } else if (strcmp(key, "username") == 0) {
            snprintf(vc->cfg.username, sizeof(vc->cfg.username), "%s", val);
        } else if (strcmp(key, "token") == 0) {
            snprintf(vc->cfg.token, sizeof(vc->cfg.token), "%s", val);
        } else if (strcmp(key, "token_expires_at") == 0) {
            vc->cfg.token_expires_at = strtoll(val, NULL, 10);
        } else if (strcmp(key, "library_dir") == 0) {
            snprintf(vc->cfg.library_dir, sizeof(vc->cfg.library_dir), "%s", val);
            strip_trailing_slash(vc->cfg.library_dir);
        } else if (strcmp(key, "pinned_pubkey") == 0) {
            snprintf(vc->cfg.pinned_pubkey, sizeof(vc->cfg.pinned_pubkey), "%s", val);
        }
    }
    fclose(f);
    return 0;
}

int
vapor_client_save_config(vapor_client *vc)
{
    char  tmp[VAPOR_PATH_MAX];
    FILE *f;

    if (vapor_plat_mkdirs(vc->data_dir) != 0) {
        vapor_client_set_error(vc, "cannot create %s", vc->data_dir);
        return -1;
    }

    /* Write-then-rename so an interrupted save cannot leave a truncated config
     * that loses the session token. */
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", vc->config_path)
        >= sizeof(tmp)) {
        vapor_client_set_error(vc, "config path too long");
        return -1;
    }

    f = fopen(tmp, "w");
    if (!f) {
        vapor_client_set_error(vc, "cannot write %s", tmp);
        return -1;
    }
    fprintf(f, "# Vapor client configuration\n");
    fprintf(f, "server_url = %s\n", vc->cfg.server_url);
    fprintf(f, "library_dir = %s\n", vc->cfg.library_dir);
    if (vc->cfg.username[0]) {
        fprintf(f, "username = %s\n", vc->cfg.username);
    }
    if (vc->cfg.token[0]) {
        fprintf(f, "token = %s\n", vc->cfg.token);
        fprintf(f, "token_expires_at = %lld\n",
                (long long)vc->cfg.token_expires_at);
    }
    if (vc->cfg.pinned_pubkey[0]) {
        fprintf(f, "pinned_pubkey = %s\n", vc->cfg.pinned_pubkey);
    }
    fclose(f);

    remove(vc->config_path);
    if (rename(tmp, vc->config_path) != 0) {
        vapor_client_set_error(vc, "cannot replace %s", vc->config_path);
        return -1;
    }
    return 0;
}

/* Path joins are checked rather than silently truncated: a clipped library
 * path would install a game somewhere unexpected. */
static int
join_path(char *out, size_t outsz, const char *dir, const char *leaf)
{
    int n = snprintf(out, outsz, "%s%s", dir, leaf);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

static const char LOCAL_SCHEMA[] =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS installs ("
    "  game_id        TEXT PRIMARY KEY,"
    "  version        TEXT NOT NULL,"
    "  name           TEXT NOT NULL,"
    "  install_dir    TEXT NOT NULL,"
    "  installed_at   INTEGER NOT NULL,"
    "  last_played_at INTEGER NOT NULL DEFAULT 0,"
    "  play_seconds   INTEGER NOT NULL DEFAULT 0,"
    "  size_on_disk   INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  game_id    TEXT NOT NULL,"
    "  started_at INTEGER NOT NULL,"
    "  seconds    INTEGER NOT NULL"
    ");";

int
vapor_client_open(vapor_client *vc)
{
    char *emsg = NULL;
    const char *override;

    memset(vc, 0, sizeof(*vc));

    if (vapor_net_global_init() != 0) {
        vapor_client_set_error(vc, "could not initialise HTTP transport");
        return -1;
    }

    /* An explicit override keeps portable installs and tests off the real
     * per-user directory. */
    override = getenv("VAPOR_DATA_DIR");
    if (override && *override) {
        if ((size_t)snprintf(vc->data_dir, sizeof(vc->data_dir), "%s", override)
            >= sizeof(vc->data_dir)) {
            vapor_client_set_error(vc, "VAPOR_DATA_DIR is too long");
            return -1;
        }
    } else if (vapor_plat_data_dir(vc->data_dir, sizeof(vc->data_dir)) != 0) {
        vapor_client_set_error(vc, "cannot determine a per-user data directory");
        return -1;
    }
    if (join_path(vc->config_path, sizeof(vc->config_path), vc->data_dir,
                  "/config.ini")
            != 0
        || join_path(vc->db_path, sizeof(vc->db_path), vc->data_dir,
                     "/library.db")
               != 0) {
        vapor_client_set_error(vc, "data directory path is too long: %s",
                               vc->data_dir);
        return -1;
    }
    vapor_plat_native_path(vc->config_path);
    vapor_plat_native_path(vc->db_path);

    /* Defaults before the file, so a partial config still yields a usable client. */
    snprintf(vc->cfg.server_url, sizeof(vc->cfg.server_url),
             "http://127.0.0.1:%d", VAPOR_DEFAULT_PORT);
    if (vapor_plat_default_library_dir(vc->cfg.library_dir,
                                       sizeof(vc->cfg.library_dir))
        != 0) {
        if (join_path(vc->cfg.library_dir, sizeof(vc->cfg.library_dir),
                      vc->data_dir, "/games")
            != 0) {
            vapor_client_set_error(vc, "data directory path is too long: %s",
                                   vc->data_dir);
            return -1;
        }
    }

    if (vapor_plat_mkdirs(vc->data_dir) != 0) {
        vapor_client_set_error(vc, "cannot create %s", vc->data_dir);
        return -1;
    }
    load_config_file(vc);

    if (sqlite3_open_v2(vc->db_path, &vc->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                            | SQLITE_OPEN_FULLMUTEX,
                        NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "cannot open %s: %s", vc->db_path,
                               vc->db ? sqlite3_errmsg(vc->db) : "?");
        return -1;
    }
    if (sqlite3_exec(vc->db, LOCAL_SCHEMA, NULL, NULL, &emsg) != SQLITE_OK) {
        vapor_client_set_error(vc, "local schema init failed: %s",
                               emsg ? emsg : "?");
        sqlite3_free(emsg);
        return -1;
    }
    return 0;
}

void
vapor_client_close(vapor_client *vc)
{
    if (vc->db) {
        sqlite3_close(vc->db);
        vc->db = NULL;
    }
    vapor_net_global_cleanup();
}
