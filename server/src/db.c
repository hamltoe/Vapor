#include "vapord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/util.h"

/* SQLite is built SQLITE_THREADSAFE=1 (serialized), so one connection shared
 * across civetweb's worker threads is safe. WAL keeps a slow catalog read from
 * blocking a login write. */
static const char SCHEMA[] =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA foreign_keys = ON;"
    "PRAGMA busy_timeout = 5000;"
    "CREATE TABLE IF NOT EXISTS users ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  username   TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    "  pwhash     TEXT NOT NULL,"
    "  is_admin   INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS tokens ("
    "  token_hash TEXT PRIMARY KEY,"
    "  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
    "  created_at INTEGER NOT NULL,"
    "  expires_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_tokens_user ON tokens(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_tokens_expiry ON tokens(expires_at);"
    "CREATE TABLE IF NOT EXISTS games ("
    "  id          TEXT PRIMARY KEY,"
    "  name        TEXT NOT NULL,"
    "  developer   TEXT,"
    "  description TEXT,"
    "  updated_at  INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS versions ("
    "  game_id        TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,"
    "  version        TEXT NOT NULL,"
    "  package_file   TEXT NOT NULL,"
    "  package_size   INTEGER NOT NULL,"
    "  package_sha256 TEXT NOT NULL,"
    "  manifest_json  TEXT NOT NULL,"
    "  created_at     INTEGER NOT NULL,"
    "  PRIMARY KEY (game_id, version)"
    ");";

/* Columns added after the first release. SQLite has no ADD COLUMN IF NOT
 * EXISTS, and a duplicate-column error is exactly what an up-to-date database
 * reports, so failures here are expected and ignored. */
static const char *const MIGRATIONS[] = {
    "ALTER TABLE versions ADD COLUMN cover TEXT",
    "ALTER TABLE versions ADD COLUMN source_path TEXT",
    "ALTER TABLE games ADD COLUMN discovered INTEGER NOT NULL DEFAULT 0",
    "CREATE TABLE IF NOT EXISTS discovered ("
    "  folder      TEXT PRIMARY KEY,"
    "  game_id     TEXT NOT NULL,"
    "  fingerprint TEXT NOT NULL,"
    "  version     TEXT NOT NULL,"
    "  updated_at  INTEGER NOT NULL"
    ")",
};

int
vapord_db_open(vapord *app, char *err, size_t errsz)
{
    char *emsg = NULL;
    int   rc;

    rc = sqlite3_open_v2(app->cfg.db_path, &app->db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                             | SQLITE_OPEN_FULLMUTEX,
                         NULL);
    if (rc != SQLITE_OK) {
        snprintf(err, errsz, "cannot open database \"%s\": %s",
                 app->cfg.db_path,
                 app->db ? sqlite3_errmsg(app->db) : sqlite3_errstr(rc));
        return -1;
    }

    if (sqlite3_exec(app->db, SCHEMA, NULL, NULL, &emsg) != SQLITE_OK) {
        snprintf(err, errsz, "schema init failed: %s", emsg ? emsg : "?");
        sqlite3_free(emsg);
        return -1;
    }

    for (size_t i = 0; i < sizeof(MIGRATIONS) / sizeof(MIGRATIONS[0]); i++) {
        sqlite3_exec(app->db, MIGRATIONS[i], NULL, NULL, NULL);
    }
    return 0;
}

void
vapord_db_close(vapord *app)
{
    if (app->db) {
        sqlite3_close(app->db);
        app->db = NULL;
    }
}

/* ------------------------------------------------------------------- users */

int
vapord_user_create(sqlite3 *db, const char *username, const char *pwhash,
                   int is_admin, int64_t *out_id)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO users (username, pwhash, is_admin, created_at)"
                            " VALUES (?, ?, ?, ?)",
                            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, pwhash, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, is_admin);
    sqlite3_bind_int64(st, 4, vapor_now_unix());

    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc == SQLITE_CONSTRAINT) {
        return 1; /* username taken */
    }
    if (rc != SQLITE_DONE) {
        return -1;
    }
    if (out_id) {
        *out_id = sqlite3_last_insert_rowid(db);
    }
    return 0;
}

int
vapord_user_lookup(sqlite3 *db, const char *username, int64_t *out_id,
                   char *pwhash, size_t pwhash_sz, int *is_admin)
{
    sqlite3_stmt *st = NULL;
    int           rc, found = 0;

    if (sqlite3_prepare_v2(db,
                           "SELECT id, pwhash, is_admin FROM users WHERE username = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, username, -1, SQLITE_STATIC);

    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *h = (const char *)sqlite3_column_text(st, 1);
        if (out_id) {
            *out_id = sqlite3_column_int64(st, 0);
        }
        if (pwhash && pwhash_sz) {
            snprintf(pwhash, pwhash_sz, "%s", h ? h : "");
        }
        if (is_admin) {
            *is_admin = sqlite3_column_int(st, 2);
        }
        found = 1;
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

int
vapord_user_name(sqlite3 *db, int64_t user_id, char *out, size_t outsz,
                 int *is_admin, int64_t *created_at)
{
    sqlite3_stmt *st = NULL;
    int           rc, found = 0;

    if (sqlite3_prepare_v2(db,
                           "SELECT username, is_admin, created_at FROM users WHERE id = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, user_id);

    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *u = (const char *)sqlite3_column_text(st, 0);
        snprintf(out, outsz, "%s", u ? u : "");
        if (is_admin) {
            *is_admin = sqlite3_column_int(st, 1);
        }
        if (created_at) {
            *created_at = sqlite3_column_int64(st, 2);
        }
        found = 1;
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

int
vapord_user_count(sqlite3 *db, int64_t *out)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    *out = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users", -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

/* ------------------------------------------------------------------ tokens */

int
vapord_token_store(sqlite3 *db, const char *token_hash, int64_t user_id,
                   int64_t created_at, int64_t expires_at)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO tokens (token_hash, user_id, created_at, expires_at)"
                           " VALUES (?, ?, ?, ?)",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, token_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, user_id);
    sqlite3_bind_int64(st, 3, created_at);
    sqlite3_bind_int64(st, 4, expires_at);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapord_token_lookup(sqlite3 *db, const char *token_hash, int64_t now,
                    int64_t *out_user_id)
{
    sqlite3_stmt *st = NULL;
    int           rc, found = 0;

    if (sqlite3_prepare_v2(db,
                           "SELECT user_id FROM tokens"
                           " WHERE token_hash = ? AND expires_at > ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, token_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now);

    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out_user_id = sqlite3_column_int64(st, 0);
        found = 1;
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

int
vapord_token_delete(sqlite3 *db, const char *token_hash)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db, "DELETE FROM tokens WHERE token_hash = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, token_hash, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapord_token_prune(sqlite3 *db, int64_t now)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db, "DELETE FROM tokens WHERE expires_at <= ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, now);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ------------------------------------------------------------------- games */

int
vapord_game_upsert(sqlite3 *db, const vapor_manifest *m)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO games (id, name, developer, description, updated_at)"
                           " VALUES (?, ?, ?, ?, ?)"
                           /* COALESCE so that publishing a new version without
                            * repeating --developer/--description does not wipe
                            * the metadata already on record. */
                           " ON CONFLICT(id) DO UPDATE SET"
                           "   name = excluded.name,"
                           "   developer = COALESCE(excluded.developer, developer),"
                           "   description = COALESCE(excluded.description, description),"
                           "   updated_at = excluded.updated_at",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, m->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, m->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, m->developer, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, m->description, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, vapor_now_unix());

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapord_version_upsert(sqlite3 *db, const vapor_manifest *m,
                      const char *manifest_json)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO versions (game_id, version, package_file,"
                           "   package_size, package_sha256, manifest_json, cover,"
                           "   created_at)"
                           " VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
                           " ON CONFLICT(game_id, version) DO UPDATE SET"
                           "   package_file = excluded.package_file,"
                           "   package_size = excluded.package_size,"
                           "   package_sha256 = excluded.package_sha256,"
                           "   manifest_json = excluded.manifest_json,"
                           "   cover = excluded.cover,"
                           "   created_at = excluded.created_at",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, m->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, m->version, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, m->package.file, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)m->package.size);
    sqlite3_bind_text(st, 5, m->package.sha256, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 6, manifest_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 7, m->cover, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 8, vapor_now_unix());

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapord_game_delete(sqlite3 *db, const char *game_id)
{
    sqlite3_stmt *st = NULL;
    int           rc, changes;

    if (sqlite3_prepare_v2(db, "DELETE FROM games WHERE id = ?", -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return -1;
    }
    changes = sqlite3_changes(db);
    return changes > 0 ? 0 : 1;
}

int
vapord_game_mark_discovered(sqlite3 *db, const char *game_id, int discovered)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db, "UPDATE games SET discovered = ? WHERE id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int(st, 1, discovered ? 1 : 0);
    sqlite3_bind_text(st, 2, game_id, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapord_game_lookup(sqlite3 *db, const char *game_id, int *discovered)
{
    sqlite3_stmt *st = NULL;
    int           found = 0;

    if (discovered) {
        *discovered = 0;
    }
    if (sqlite3_prepare_v2(db, "SELECT discovered FROM games WHERE id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (discovered) {
            *discovered = sqlite3_column_int(st, 0);
        }
        found = 1;
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

int
vapord_version_set_source(sqlite3 *db, const char *game_id, const char *version,
                          const char *source_rel)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db,
                           "UPDATE versions SET source_path = ? WHERE game_id = ?"
                           " AND version = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    if (source_rel && *source_rel) {
        sqlite3_bind_text(st, 1, source_rel, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(st, 1);
    }
    sqlite3_bind_text(st, 2, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, version, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapord_version_source(sqlite3 *db, const char *game_id, const char *version,
                      char *out, size_t outsz)
{
    sqlite3_stmt *st = NULL;
    int           found = 0;

    out[0] = '\0';
    if (sqlite3_prepare_v2(db,
                           "SELECT source_path FROM versions"
                           " WHERE game_id = ? AND version = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, version, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(st, 0);
        if (p && *p) {
            snprintf(out, outsz, "%s", p);
            found = 1;
        }
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

int
vapord_discovered_get(sqlite3 *db, const char *folder, vapord_discovered_row *out)
{
    sqlite3_stmt *st = NULL;
    int           found = 0;

    memset(out, 0, sizeof(*out));
    if (sqlite3_prepare_v2(db,
                           "SELECT folder, game_id, fingerprint, version"
                           " FROM discovered WHERE folder = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, folder, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *f = (const char *)sqlite3_column_text(st, 0);
        const char *id = (const char *)sqlite3_column_text(st, 1);
        const char *fp = (const char *)sqlite3_column_text(st, 2);
        const char *ver = (const char *)sqlite3_column_text(st, 3);
        snprintf(out->folder, sizeof(out->folder), "%s", f ? f : "");
        snprintf(out->game_id, sizeof(out->game_id), "%s", id ? id : "");
        snprintf(out->fingerprint, sizeof(out->fingerprint), "%s", fp ? fp : "");
        snprintf(out->version, sizeof(out->version), "%s", ver ? ver : "");
        found = 1;
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

int
vapord_discovered_put(sqlite3 *db, const vapord_discovered_row *row)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO discovered (folder, game_id, fingerprint,"
                           "   version, updated_at)"
                           " VALUES (?, ?, ?, ?, ?)"
                           " ON CONFLICT(folder) DO UPDATE SET"
                           "   game_id = excluded.game_id,"
                           "   fingerprint = excluded.fingerprint,"
                           "   version = excluded.version,"
                           "   updated_at = excluded.updated_at",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, row->folder, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, row->game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, row->fingerprint, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, row->version, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, vapor_now_unix());
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapord_discovered_delete(sqlite3 *db, const char *folder)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(db, "DELETE FROM discovered WHERE folder = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, folder, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapord_discovered_list(sqlite3 *db, vapord_discovered_row **out, size_t *count)
{
    sqlite3_stmt         *st = NULL;
    vapord_discovered_row *rows = NULL;
    size_t                n = 0, cap = 0;

    *out = NULL;
    *count = 0;
    if (sqlite3_prepare_v2(db,
                           "SELECT folder, game_id, fingerprint, version FROM discovered",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *f = (const char *)sqlite3_column_text(st, 0);
        const char *id = (const char *)sqlite3_column_text(st, 1);
        const char *fp = (const char *)sqlite3_column_text(st, 2);
        const char *ver = (const char *)sqlite3_column_text(st, 3);

        if (n == cap) {
            size_t newcap = cap ? cap * 2 : 8;
            vapord_discovered_row *grown =
                (vapord_discovered_row *)realloc(rows, newcap * sizeof(*grown));
            if (!grown) {
                free(rows);
                sqlite3_finalize(st);
                return -1;
            }
            rows = grown;
            cap = newcap;
        }
        memset(&rows[n], 0, sizeof(rows[n]));
        snprintf(rows[n].folder, sizeof(rows[n].folder), "%s", f ? f : "");
        snprintf(rows[n].game_id, sizeof(rows[n].game_id), "%s", id ? id : "");
        snprintf(rows[n].fingerprint, sizeof(rows[n].fingerprint), "%s",
                 fp ? fp : "");
        snprintf(rows[n].version, sizeof(rows[n].version), "%s", ver ? ver : "");
        n++;
    }
    sqlite3_finalize(st);
    *out = rows;
    *count = n;
    return 0;
}

int
vapord_discovered_by_id(sqlite3 *db, const char *game_id, char *folder,
                        size_t foldersz)
{
    sqlite3_stmt *st = NULL;
    int           found = 0;

    folder[0] = '\0';
    if (sqlite3_prepare_v2(db, "SELECT folder FROM discovered WHERE game_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *f = (const char *)sqlite3_column_text(st, 0);
        snprintf(folder, foldersz, "%s", f ? f : "");
        found = 1;
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

/* SQLite cannot order "1.10" after "1.9", so versions are compared in C. */
int
vapord_game_latest_version(sqlite3 *db, const char *game_id,
                           char *out, size_t outsz)
{
    sqlite3_stmt *st = NULL;
    int           any = 0, exists = 0;
    char          best[VAPOR_VERSION_MAX + 1];

    best[0] = '\0';

    if (sqlite3_prepare_v2(db, "SELECT 1 FROM games WHERE id = ?", -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    exists = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (!exists) {
        return -1;
    }

    st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT version FROM versions WHERE game_id = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (!v) {
            continue;
        }
        if (!any || vapor_version_cmp(v, best) > 0) {
            snprintf(best, sizeof(best), "%s", v);
            any = 1;
        }
    }
    sqlite3_finalize(st);

    if (!any) {
        return 1;
    }
    snprintf(out, outsz, "%s", best);
    return 0;
}

int
vapord_version_manifest(sqlite3 *db, const char *game_id, const char *version,
                        char **out_json)
{
    sqlite3_stmt *st = NULL;
    int           found = 0;

    *out_json = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT manifest_json FROM versions"
                           " WHERE game_id = ? AND version = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, version, -1, SQLITE_STATIC);

    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *j = (const char *)sqlite3_column_text(st, 0);
        if (j) {
            *out_json = vapor_strdup(j);
            found = (*out_json != NULL);
        }
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

/* ----------------------------------------------------------------- catalog */

int
vapord_version_cover(sqlite3 *db, const char *game_id, const char *version,
                     char *out, size_t outsz)
{
    sqlite3_stmt *st = NULL;
    int           found = 0;

    out[0] = '\0';
    if (sqlite3_prepare_v2(db,
                           "SELECT cover FROM versions"
                           " WHERE game_id = ? AND version = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, version, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *c = (const char *)sqlite3_column_text(st, 0);
        if (c && *c) {
            snprintf(out, outsz, "%s", c);
            found = 1;
        }
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

/* Adds size/sha/created_at/cover of one version onto `dst`. */
static void
add_version_details(sqlite3 *db, const char *game_id, const char *version,
                    cJSON *dst)
{
    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(db,
                           "SELECT package_size, package_sha256, created_at, cover"
                           " FROM versions WHERE game_id = ? AND version = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, version, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *sha   = (const char *)sqlite3_column_text(st, 1);
        const char *cover = (const char *)sqlite3_column_text(st, 3);
        cJSON_AddNumberToObject(dst, "size",
                                (double)sqlite3_column_int64(st, 0));
        cJSON_AddStringToObject(dst, "sha256", sha ? sha : "");
        cJSON_AddNumberToObject(dst, "published_at",
                                (double)sqlite3_column_int64(st, 2));
        cJSON_AddBoolToObject(dst, "has_cover", cover && *cover);
    }
    sqlite3_finalize(st);
}

cJSON *
vapord_catalog_json(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    cJSON        *root, *games;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    games = cJSON_AddArrayToObject(root, "games");
    if (!games) {
        cJSON_Delete(root);
        return NULL;
    }

    if (sqlite3_prepare_v2(db,
                           "SELECT id, name, developer, description FROM games"
                           " ORDER BY name COLLATE NOCASE",
                           -1, &st, NULL) != SQLITE_OK) {
        cJSON_Delete(root);
        return NULL;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *id   = (const char *)sqlite3_column_text(st, 0);
        const char *name = (const char *)sqlite3_column_text(st, 1);
        const char *dev  = (const char *)sqlite3_column_text(st, 2);
        const char *desc = (const char *)sqlite3_column_text(st, 3);
        char        latest[VAPOR_VERSION_MAX + 1];
        cJSON      *g;

        if (!id) {
            continue;
        }
        /* A game with no published version cannot be installed, so hide it. */
        if (vapord_game_latest_version(db, id, latest, sizeof(latest)) != 0) {
            continue;
        }

        g = cJSON_CreateObject();
        if (!g) {
            continue;
        }
        cJSON_AddItemToArray(games, g);
        cJSON_AddStringToObject(g, "id", id);
        cJSON_AddStringToObject(g, "name", name ? name : id);
        cJSON_AddStringToObject(g, "developer", dev ? dev : "");
        cJSON_AddStringToObject(g, "description", desc ? desc : "");
        cJSON_AddStringToObject(g, "latest_version", latest);
        add_version_details(db, id, latest, g);
    }
    sqlite3_finalize(st);
    return root;
}

cJSON *
vapord_game_json(sqlite3 *db, const char *game_id)
{
    sqlite3_stmt *st = NULL;
    cJSON        *root = NULL, *versions;
    char          latest[VAPOR_VERSION_MAX + 1];
    int           found = 0;

    if (sqlite3_prepare_v2(db,
                           "SELECT name, developer, description FROM games WHERE id = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *dev  = (const char *)sqlite3_column_text(st, 1);
        const char *desc = (const char *)sqlite3_column_text(st, 2);

        root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "id", game_id);
            cJSON_AddStringToObject(root, "name", name ? name : game_id);
            cJSON_AddStringToObject(root, "developer", dev ? dev : "");
            cJSON_AddStringToObject(root, "description", desc ? desc : "");
            found = 1;
        }
    }
    sqlite3_finalize(st);
    if (!found) {
        return NULL;
    }

    if (vapord_game_latest_version(db, game_id, latest, sizeof(latest)) == 0) {
        cJSON_AddStringToObject(root, "latest_version", latest);
    }

    versions = cJSON_AddArrayToObject(root, "versions");
    st = NULL;
    if (versions
        && sqlite3_prepare_v2(db,
                              "SELECT version, package_file, package_size,"
                              "   package_sha256, created_at, cover"
                              " FROM versions WHERE game_id = ?",
                              -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *ver   = (const char *)sqlite3_column_text(st, 0);
            const char *file  = (const char *)sqlite3_column_text(st, 1);
            const char *sha   = (const char *)sqlite3_column_text(st, 3);
            const char *cover = (const char *)sqlite3_column_text(st, 5);
            cJSON      *v = cJSON_CreateObject();

            if (!v) {
                continue;
            }
            cJSON_AddItemToArray(versions, v);
            cJSON_AddStringToObject(v, "version", ver ? ver : "");
            cJSON_AddStringToObject(v, "file", file ? file : "");
            cJSON_AddNumberToObject(v, "size", (double)sqlite3_column_int64(st, 2));
            cJSON_AddStringToObject(v, "sha256", sha ? sha : "");
            cJSON_AddNumberToObject(v, "published_at",
                                    (double)sqlite3_column_int64(st, 4));
            cJSON_AddBoolToObject(v, "has_cover", cover && *cover);
        }
        sqlite3_finalize(st);
    }
    return root;
}
