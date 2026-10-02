/* Local library state: what is installed, where, and how long it has been
 * played. Entirely client-side; the server never sees any of it. */

#include "vapor/client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/util.h"

int
vapor_db_record_install(vapor_client *vc, const vapor_install *rec)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(vc->db,
                           "INSERT INTO installs (game_id, version, name,"
                           "   install_dir, installed_at, size_on_disk,"
                           "   setup_pending, payload_dir, install_kind,"
                           "   uninstall_exe, uninstall_params, product_dir,"
                           "   launch_exe, setup_state, setup_rel,"
                           "   profile_launch, profile_uninstall)"
                           " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
                           "         ?, ?, ?, ?)"
                           " ON CONFLICT(game_id) DO UPDATE SET"
                           "   version = excluded.version,"
                           "   name = excluded.name,"
                           "   install_dir = excluded.install_dir,"
                           "   installed_at = excluded.installed_at,"
                           "   size_on_disk = excluded.size_on_disk,"
                           "   setup_pending = excluded.setup_pending,"
                           "   payload_dir = excluded.payload_dir,"
                           "   install_kind = excluded.install_kind,"
                           "   uninstall_exe = excluded.uninstall_exe,"
                           "   uninstall_params = excluded.uninstall_params,"
                           "   product_dir = excluded.product_dir,"
                           "   launch_exe = excluded.launch_exe,"
                           "   setup_state = excluded.setup_state,"
                           "   setup_rel = excluded.setup_rel,"
                           "   profile_launch = excluded.profile_launch,"
                           "   profile_uninstall = excluded.profile_uninstall",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, rec->game_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, rec->version, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, rec->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, rec->install_dir, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, rec->installed_at);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)rec->size_on_disk);
    sqlite3_bind_int(st, 7, rec->setup_pending ? 1 : 0);
    sqlite3_bind_text(st, 8,
                      rec->payload_dir[0] ? rec->payload_dir : rec->install_dir,
                      -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 9, rec->install_kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 10, rec->uninstall_exe, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 11, rec->uninstall_params, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 12, rec->product_dir, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 13, rec->launch_exe, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 14, rec->setup_state, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 15, rec->setup_rel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 16, rec->profile_launch, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 17, rec->profile_uninstall, -1, SQLITE_STATIC);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    return 0;
}

int
vapor_db_forget_install(vapor_client *vc, const char *game_id)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(vc->db, "DELETE FROM installs WHERE game_id = ?", -1,
                           &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void
row_to_install(sqlite3_stmt *st, vapor_install *out)
{
    const char *s;

    memset(out, 0, sizeof(*out));
    s = (const char *)sqlite3_column_text(st, 0);
    snprintf(out->game_id, sizeof(out->game_id), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 1);
    snprintf(out->version, sizeof(out->version), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 2);
    snprintf(out->name, sizeof(out->name), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 3);
    snprintf(out->install_dir, sizeof(out->install_dir), "%s", s ? s : "");
    out->installed_at = sqlite3_column_int64(st, 4);
    out->last_played_at = sqlite3_column_int64(st, 5);
    out->play_seconds = sqlite3_column_int64(st, 6);
    out->size_on_disk = (uint64_t)sqlite3_column_int64(st, 7);
    out->setup_pending = sqlite3_column_int(st, 8);
    s = (const char *)sqlite3_column_text(st, 9);
    snprintf(out->payload_dir, sizeof(out->payload_dir), "%s", s ? s : "");
    if (!out->payload_dir[0]) {
        snprintf(out->payload_dir, sizeof(out->payload_dir), "%s",
                 out->install_dir);
    }
    s = (const char *)sqlite3_column_text(st, 10);
    snprintf(out->install_kind, sizeof(out->install_kind), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 11);
    snprintf(out->uninstall_exe, sizeof(out->uninstall_exe), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 12);
    snprintf(out->uninstall_params, sizeof(out->uninstall_params), "%s",
             s ? s : "");
    s = (const char *)sqlite3_column_text(st, 13);
    snprintf(out->product_dir, sizeof(out->product_dir), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 14);
    snprintf(out->launch_exe, sizeof(out->launch_exe), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 15);
    snprintf(out->setup_state, sizeof(out->setup_state), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 16);
    snprintf(out->setup_rel, sizeof(out->setup_rel), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 17);
    snprintf(out->profile_launch, sizeof(out->profile_launch), "%s", s ? s : "");
    s = (const char *)sqlite3_column_text(st, 18);
    snprintf(out->profile_uninstall, sizeof(out->profile_uninstall), "%s",
             s ? s : "");
}

#define INSTALL_COLUMNS                                                        \
    "game_id, version, name, install_dir, installed_at, last_played_at,"       \
    " play_seconds, size_on_disk, setup_pending, payload_dir, install_kind,"   \
    " uninstall_exe, uninstall_params, product_dir, launch_exe,"              \
    " setup_state, setup_rel, profile_launch, profile_uninstall"

int
vapor_db_get_install(vapor_client *vc, const char *game_id, vapor_install *out)
{
    sqlite3_stmt *st = NULL;
    int           found = 0;

    if (sqlite3_prepare_v2(vc->db,
                           "SELECT " INSTALL_COLUMNS
                           " FROM installs WHERE game_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        row_to_install(st, out);
        found = 1;
    }
    sqlite3_finalize(st);
    return found ? 0 : 1;
}

int
vapor_db_list_installs(vapor_client *vc, vapor_install **out, size_t *count)
{
    sqlite3_stmt  *st = NULL;
    vapor_install *arr = NULL;
    size_t         n = 0, cap = 0;

    *out = NULL;
    *count = 0;

    if (sqlite3_prepare_v2(vc->db,
                           "SELECT " INSTALL_COLUMNS
                           " FROM installs ORDER BY name COLLATE NOCASE",
                           -1, &st, NULL)
        != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
        return -1;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            size_t         newcap = cap ? cap * 2 : 8;
            vapor_install *grown =
                (vapor_install *)realloc(arr, newcap * sizeof(*grown));
            if (!grown) {
                free(arr);
                sqlite3_finalize(st);
                vapor_client_set_error(vc, "out of memory");
                return -1;
            }
            arr = grown;
            cap = newcap;
        }
        row_to_install(st, &arr[n++]);
    }
    sqlite3_finalize(st);

    *out = arr;
    *count = n;
    return 0;
}

int
vapor_db_add_playtime(vapor_client *vc, const char *game_id, int64_t started_at,
                      int64_t seconds)
{
    sqlite3_stmt *st = NULL;
    int           rc;

    if (sqlite3_prepare_v2(vc->db,
                           "INSERT INTO sessions (game_id, started_at, seconds)"
                           " VALUES (?, ?, ?)",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, game_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, started_at);
    sqlite3_bind_int64(st, 3, seconds);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return -1;
    }

    st = NULL;
    if (sqlite3_prepare_v2(vc->db,
                           "UPDATE installs SET last_played_at = ?,"
                           "   play_seconds = play_seconds + ?"
                           " WHERE game_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(st, 1, started_at);
    sqlite3_bind_int64(st, 2, seconds);
    sqlite3_bind_text(st, 3, game_id, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
vapor_db_rename_install(vapor_client *vc, const char *from_id, const char *to_id)
{
    sqlite3_stmt *st = NULL;
    char         *emsg = NULL;

    if (!vapor_id_is_valid(from_id) || !vapor_id_is_valid(to_id)
        || strcmp(from_id, to_id) == 0) {
        vapor_client_set_error(vc, "cannot rename that library entry");
        return -1;
    }
    if (sqlite3_exec(vc->db, "BEGIN", NULL, NULL, &emsg) != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", emsg ? emsg : "cannot start a transaction");
        sqlite3_free(emsg);
        return -1;
    }
    if (sqlite3_prepare_v2(vc->db,
                           "UPDATE installs SET game_id = ? WHERE game_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        goto fail;
    }
    sqlite3_bind_text(st, 1, to_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, from_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(vc->db,
                           "UPDATE sessions SET game_id = ? WHERE game_id = ?",
                           -1, &st, NULL)
        != SQLITE_OK) {
        goto fail;
    }
    sqlite3_bind_text(st, 1, to_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, from_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        goto fail;
    }
    sqlite3_finalize(st);
    if (sqlite3_exec(vc->db, "COMMIT", NULL, NULL, &emsg) != SQLITE_OK) {
        vapor_client_set_error(vc, "%s", emsg ? emsg : "cannot commit");
        sqlite3_free(emsg);
        sqlite3_exec(vc->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;

fail:
    vapor_client_set_error(vc, "%s", sqlite3_errmsg(vc->db));
    sqlite3_exec(vc->db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}
