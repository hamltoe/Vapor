/* Shortcuts to programs that already live somewhere else on this computer.
 * The row remembers a path so Play can start it. Nothing is copied, and
 * Remove does not delete or uninstall those files. */

#include "vapor/client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "vapor/util.h"

int
vapor_install_is_external(const vapor_install *rec)
{
    return rec && strcmp(rec->install_kind, VAPOR_INSTALL_KIND_EXTERNAL) == 0;
}

int
vapor_pick_executable(char *out, size_t outsz)
{
    return vapor_plat_pick_file(out, outsz);
}

static void
trim_inplace(char *s)
{
    char  *start = s;
    size_t n;

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'
                     || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

static void
strip_quotes(char *s)
{
    size_t n = strlen(s);

    if (n >= 2
        && ((s[0] == '"' && s[n - 1] == '"')
            || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        memmove(s, s + 1, n - 1);
    }
}

/* Library title taken from the file name, extension removed, case kept. */
static int
display_from_path(const char *path, char *out, size_t outsz)
{
    const char *base = path;
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *dot;
    size_t      n;

    if (bslash > slash) {
        slash = bslash;
    }
    if (slash && slash[1]) {
        base = slash + 1;
    }
    dot = strrchr(base, '.');
    n = (dot && dot != base) ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n >= outsz) {
        return -1;
    }
    memcpy(out, base, n);
    out[n] = '\0';
    trim_inplace(out);
    return out[0] ? 0 : -1;
}

static void
parent_of(const char *path, char *out, size_t outsz)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    size_t      n;

    if (bslash > slash) {
        slash = bslash;
    }
    if (!slash) {
        snprintf(out, outsz, ".");
        return;
    }
    n = (size_t)(slash - path);
    if (n == 0) {
        snprintf(out, outsz, "%c", path[0]);
        return;
    }
    /* "C:\game.exe" -> "C:\". "C:" alone is the current directory on that
     * drive, not the root, and CreateProcess would start in the wrong place. */
    if (n == 2 && path[1] == ':') {
        snprintf(out, outsz, "%c%c%c", path[0], path[1], slash[0]);
        return;
    }
    if (n >= outsz) {
        n = outsz - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static int
same_path(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
#if defined(_WIN32)
    return vapor_str_eq_ci(a, b);
#else
    return strcmp(a, b) == 0;
#endif
}

int
vapor_local_shortcut_id(const char *name, int suffix, char *out, size_t outsz)
{
    char   slug[VAPOR_ID_MAX + 1];
    char   buf[VAPOR_ID_MAX + 1];
    char   tail[16];
    size_t keep;
    int    n;

    if (!name || vapor_id_slug(name, slug, sizeof(slug)) != 0) {
        return -1;
    }
    tail[0] = '\0';
    if (suffix > 1) {
        snprintf(tail, sizeof(tail), "-%d", suffix);
    }
    keep = VAPOR_ID_MAX - strlen("local-") - strlen(tail);
    if (strlen(slug) > keep) {
        slug[keep] = '\0';
    }
    while (slug[0] && slug[strlen(slug) - 1] == '-') {
        slug[strlen(slug) - 1] = '\0';
    }
    if (!slug[0]) {
        return -1;
    }
    n = snprintf(buf, sizeof(buf), "local-%s%s", slug, tail);
    if (n < 0 || (size_t)n >= sizeof(buf) || (size_t)n >= outsz
        || !vapor_id_is_valid(buf)) {
        return -1;
    }
    snprintf(out, outsz, "%s", buf);
    return 0;
}

int
vapor_add_local_game(vapor_client *vc, const char *name, const char *exe_path,
                     char *out_id, size_t idsz)
{
    vapor_install *rows = NULL;
    vapor_install  rec, existing;
    char           title[256];
    char           raw[VAPOR_PATH_MAX];
    char           exe[VAPOR_PATH_MAX];
    char           cwd[VAPOR_PATH_MAX];
    char           id[VAPOR_ID_MAX + 1];
    size_t         n = 0, i;
    int            updating = 0;
    int            suffix;

    if (out_id && idsz) {
        out_id[0] = '\0';
    }
    if (!exe_path || !exe_path[0]) {
        vapor_client_set_error(vc, "choose the program to launch");
        return -1;
    }
    if ((size_t)snprintf(raw, sizeof(raw), "%s", exe_path) >= sizeof(raw)) {
        vapor_client_set_error(vc, "that path is too long");
        return -1;
    }
    trim_inplace(raw);
    strip_quotes(raw);
    if (!raw[0] || strchr(raw, '\n') || strchr(raw, '\r')) {
        vapor_client_set_error(vc, "choose the program to launch");
        return -1;
    }
    if (vapor_str_ends_with_ci(raw, ".lnk")) {
        vapor_client_set_error(vc,
                               "choose the program itself; a shortcut file "
                               "cannot be launched directly");
        return -1;
    }
    if (vapor_plat_absolute(raw, exe, sizeof(exe)) != 0) {
        vapor_client_set_error(vc, "cannot find %s", raw);
        return -1;
    }
    vapor_plat_native_path(exe);
    if (!vapor_plat_exists(exe)) {
        vapor_client_set_error(vc, "cannot find %s", exe);
        return -1;
    }
    if (vapor_plat_is_dir(exe)) {
        vapor_client_set_error(vc,
                               "choose the game executable, not a folder (%s)",
                               exe);
        return -1;
    }

    if (name && name[0]) {
        if ((size_t)snprintf(title, sizeof(title), "%s", name) >= sizeof(title)) {
            vapor_client_set_error(vc, "that name is too long");
            return -1;
        }
        trim_inplace(title);
    } else {
        title[0] = '\0';
    }
    if (!title[0] && display_from_path(exe, title, sizeof(title)) != 0) {
        vapor_client_set_error(vc, "give the game a name");
        return -1;
    }
    if (strchr(title, '\n') || strchr(title, '\r')) {
        vapor_client_set_error(vc, "the name cannot contain a new line");
        return -1;
    }

    parent_of(exe, cwd, sizeof(cwd));
    if (!cwd[0]) {
        vapor_client_set_error(vc, "cannot tell which folder %s is in", exe);
        return -1;
    }

    /* Same program added twice is one shortcut, so playtime stays put when
     * the title is edited. */
    if (vapor_db_list_installs(vc, &rows, &n) != 0) {
        return -1;
    }
    id[0] = '\0';
    for (i = 0; i < n; i++) {
        if (!vapor_install_is_external(&rows[i])) {
            continue;
        }
        if (same_path(rows[i].launch_exe, exe)) {
            snprintf(id, sizeof(id), "%s", rows[i].game_id);
            rec = rows[i];
            updating = 1;
            break;
        }
    }
    free(rows);

    if (!updating) {
        int made = 0;
        int free_id = 0;

        for (suffix = 1; suffix < 100; suffix++) {
            int got;

            if (vapor_local_shortcut_id(title, suffix, id, sizeof(id)) != 0) {
                continue;
            }
            made = 1;
            got = vapor_db_get_install(vc, id, &existing);
            if (got < 0) {
                return -1;
            }
            if (got == 1) {
                free_id = 1;
                break;
            }
            /* A managed install, or another shortcut, already owns this id.
             * The next suffix keeps that row intact. */
        }
        if (!made) {
            vapor_client_set_error(vc,
                                   "that name cannot be used as a library id; "
                                   "use letters or numbers");
            return -1;
        }
        if (!free_id) {
            vapor_client_set_error(vc, "could not find a free library id for %s",
                                   title);
            return -1;
        }
        memset(&rec, 0, sizeof(rec));
        snprintf(rec.game_id, sizeof(rec.game_id), "%s", id);
        rec.installed_at = vapor_now_unix();
    }

    snprintf(rec.version, sizeof(rec.version), "local");
    snprintf(rec.name, sizeof(rec.name), "%s", title);
    snprintf(rec.install_dir, sizeof(rec.install_dir), "%s", cwd);
    snprintf(rec.install_kind, sizeof(rec.install_kind), "%s",
             VAPOR_INSTALL_KIND_EXTERNAL);
    snprintf(rec.launch_exe, sizeof(rec.launch_exe), "%s", exe);
    rec.setup_pending = 0;
    rec.setup_state[0] = '\0';
    rec.setup_rel[0] = '\0';
    rec.profile_launch[0] = '\0';
    rec.profile_uninstall[0] = '\0';
    rec.payload_dir[0] = '\0';
    rec.uninstall_exe[0] = '\0';
    rec.uninstall_params[0] = '\0';
    rec.product_dir[0] = '\0';
    rec.size_on_disk = 0;

    if (vapor_db_record_install(vc, &rec) != 0) {
        return -1;
    }
    if (out_id && idsz) {
        snprintf(out_id, idsz, "%s", rec.game_id);
    }
    printf("%s local shortcut %s (%s)\n  %s\n", updating ? "updated" : "added",
           rec.game_id, rec.name, rec.launch_exe);
    printf("play: vapor launch %s\n", rec.game_id);
    printf("removing this shortcut leaves those files in place\n");
    return 0;
}
