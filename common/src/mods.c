#include "vapor/mods.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MOD_PATH 1024

static void
set_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz) {
        snprintf(err, errsz, "%s", msg);
    }
}

static void
set_errf(char *err, size_t errsz, const char *fmt, const char *a)
{
    if (err && errsz) {
        snprintf(err, errsz, fmt, a);
    }
}

static void
native_seps(char *s)
{
#if defined(_WIN32)
    for (; s && *s; s++) {
        if (*s == '/') {
            *s = '\\';
        }
    }
#else
    (void)s;
#endif
}

static int
join_rel(char *out, size_t outsz, const char *root, const char *rel)
{
    int n;

    if (rel && rel[0]) {
        n = snprintf(out, outsz, "%s/%s", root, rel);
    } else {
        n = snprintf(out, outsz, "%s", root);
    }
    if (n < 0 || (size_t)n >= outsz) {
        return -1;
    }
    native_seps(out);
    return 0;
}

static int
path_exists(const char *path, int *is_dir)
{
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);

    if (attr == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (is_dir) {
        *is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return 1;
#else
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    if (is_dir) {
        *is_dir = S_ISDIR(st.st_mode);
    }
    return 1;
#endif
}

static int
mkdir_one(const char *path)
{
#if defined(_WIN32)
    if (_mkdir(path) == 0 || errno == EEXIST) {
        return 0;
    }
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
#endif
    return path_exists(path, NULL) ? 0 : -1;
}

static int
mkdirs(const char *path)
{
    char   tmp[MOD_PATH];
    size_t i, n;

    n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == '\0') {
            char saved = tmp[i];
            int  is_dir = 0;

            /* Skip the drive colon on Windows ("C:"). */
            if (i == 2 && tmp[1] == ':') {
                tmp[i] = saved;
                continue;
            }
            tmp[i] = '\0';
            if (tmp[0] && !path_exists(tmp, &is_dir)) {
                if (mkdir_one(tmp) != 0) {
                    return -1;
                }
            }
            tmp[i] = saved;
        }
    }
    return 0;
}

static int
mkdirs_parent(const char *file)
{
    char  tmp[MOD_PATH];
    char *slash;

    if (snprintf(tmp, sizeof(tmp), "%s", file) >= (int)sizeof(tmp)) {
        return -1;
    }
    slash = strrchr(tmp, '/');
#if defined(_WIN32)
    {
        char *bs = strrchr(tmp, '\\');
        if (bs > slash) {
            slash = bs;
        }
    }
#endif
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    return mkdirs(tmp);
}

static int
copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char  buf[64 * 1024];
    size_t n;

    if (mkdirs_parent(dst) != 0) {
        return -1;
    }
    in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            remove(dst);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        remove(dst);
        return -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        remove(dst);
        return -1;
    }
    return 0;
}

int
vapor_mod_rel_ok(const char *rel, char *out, size_t outsz)
{
    char   clean[MOD_PATH];
    size_t n = 0;
    const char *p;

    if (!rel || !*rel) {
        return -1;
    }
    if (rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':')) {
        return -1;
    }
    p = rel;
    while (*p) {
        const char *seg = p;
        size_t      seglen;

        while (*p && *p != '/' && *p != '\\') {
            p++;
        }
        seglen = (size_t)(p - seg);
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            return -1;
        }
        if (seglen == 0 || (seglen == 1 && seg[0] == '.')) {
            if (*p) {
                p++;
            }
            continue;
        }
        if (n + seglen + 2 >= sizeof(clean)) {
            return -1;
        }
        if (n > 0) {
            clean[n++] = '/';
        }
        memcpy(clean + n, seg, seglen);
        n += seglen;
        clean[n] = '\0';
        if (*p) {
            p++;
        }
    }
    if (n == 0) {
        return -1;
    }
    if (strcmp(clean, ".vapor") == 0 || strncmp(clean, ".vapor/", 7) == 0) {
        return -1;
    }
    if (out) {
        if (n + 1 > outsz) {
            return -1;
        }
        memcpy(out, clean, n + 1);
    }
    return 0;
}

static cJSON *
applied_find(cJSON *files, const char *path)
{
    cJSON *it;

    if (!files) {
        return NULL;
    }
    cJSON_ArrayForEach(it, files) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "path");

        if (cJSON_IsString(p) && p->valuestring && strcmp(p->valuestring, path) == 0) {
            return it;
        }
    }
    return NULL;
}

static int
applied_add(cJSON *files, const char *path, int had_original)
{
    cJSON *obj, *s;

    if (!files) {
        return -1;
    }
    obj = cJSON_CreateObject();
    s = cJSON_CreateString(path);
    if (!obj || !s) {
        cJSON_Delete(obj);
        cJSON_Delete(s);
        return -1;
    }
    cJSON_AddItemToObject(obj, "path", s);
    if (!cJSON_AddNumberToObject(obj, "had_original", had_original ? 1 : 0)) {
        cJSON_Delete(obj);
        return -1;
    }
    cJSON_AddItemToArray(files, obj);
    return 0;
}

static int
apply_one(const char *install_dir, const char *overlay_abs, const char *rel,
          const char *vanilla_dir, cJSON *files, char *err, size_t errsz)
{
    char   dest[MOD_PATH];
    char   backup[MOD_PATH];
    cJSON *prev;
    int    dest_dir = 0;
    int    had;

    if (join_rel(dest, sizeof(dest), install_dir, rel) != 0) {
        set_errf(err, errsz, "install path is too long for %s", rel);
        return -1;
    }
    prev = applied_find(files, rel);
    if (prev) {
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(prev, "had_original");
        had = cJSON_IsNumber(h) && h->valuedouble != 0;
    } else if (path_exists(dest, &dest_dir) && !dest_dir) {
        if (join_rel(backup, sizeof(backup), vanilla_dir, rel) != 0) {
            set_err(err, errsz, "vanilla path is too long");
            return -1;
        }
        if (!path_exists(backup, NULL) && copy_file(dest, backup) != 0) {
            set_errf(err, errsz, "cannot back up %s", rel);
            return -1;
        }
        had = 1;
    } else if (dest_dir) {
        set_errf(err, errsz, "%s is a directory; a mod cannot replace it with a file",
                 rel);
        return -1;
    } else {
        had = 0;
    }
    if (copy_file(overlay_abs, dest) != 0) {
        set_errf(err, errsz, "cannot write %s", rel);
        return -1;
    }
    if (!prev && applied_add(files, rel, had) != 0) {
        set_err(err, errsz, "out of memory");
        return -1;
    }
    return 0;
}

#if defined(_WIN32)
static int apply_walk(const char *install_dir, const char *overlay_root,
                      const char *rel, const char *vanilla_dir, cJSON *files,
                      char *err, size_t errsz);

static int
apply_walk(const char *install_dir, const char *overlay_root, const char *rel,
           const char *vanilla_dir, cJSON *files, char *err, size_t errsz)
{
    char             abs[MOD_PATH];
    char             pattern[MOD_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              rc = 0;

    if (join_rel(abs, sizeof(abs), overlay_root, rel) != 0) {
        set_err(err, errsz, "mod path is too long");
        return -1;
    }
    if (snprintf(pattern, sizeof(pattern), "%s\\*", abs) >= (int)sizeof(pattern)) {
        set_err(err, errsz, "mod path is too long");
        return -1;
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        set_errf(err, errsz, "cannot read %s", abs);
        return -1;
    }
    do {
        char child_rel[MOD_PATH];
        char child_abs[MOD_PATH];

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        if (rel[0]) {
            if (snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, fd.cFileName)
                >= (int)sizeof(child_rel)) {
                set_err(err, errsz, "mod path is too long");
                rc = -1;
                break;
            }
        } else if (snprintf(child_rel, sizeof(child_rel), "%s", fd.cFileName)
                   >= (int)sizeof(child_rel)) {
            set_err(err, errsz, "mod path is too long");
            rc = -1;
            break;
        }
        if (vapor_mod_rel_ok(child_rel, child_rel, sizeof(child_rel)) != 0) {
            set_errf(err, errsz, "mod path \"%s\" is not allowed", fd.cFileName);
            rc = -1;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (apply_walk(install_dir, overlay_root, child_rel, vanilla_dir,
                           files, err, errsz)
                != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (join_rel(child_abs, sizeof(child_abs), overlay_root, child_rel) != 0) {
            set_err(err, errsz, "mod path is too long");
            rc = -1;
            break;
        }
        if (apply_one(install_dir, child_abs, child_rel, vanilla_dir, files, err,
                      errsz)
            != 0) {
            rc = -1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return rc;
}
#else
static int
apply_walk(const char *install_dir, const char *overlay_root, const char *rel,
           const char *vanilla_dir, cJSON *files, char *err, size_t errsz)
{
    char           abs[MOD_PATH];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if (join_rel(abs, sizeof(abs), overlay_root, rel) != 0) {
        set_err(err, errsz, "mod path is too long");
        return -1;
    }
    d = opendir(abs);
    if (!d) {
        set_errf(err, errsz, "cannot read %s", abs);
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child_rel[MOD_PATH];
        char        child_abs[MOD_PATH];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (rel[0]) {
            if (snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name)
                >= (int)sizeof(child_rel)) {
                set_err(err, errsz, "mod path is too long");
                rc = -1;
                break;
            }
        } else if (snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name)
                   >= (int)sizeof(child_rel)) {
            set_err(err, errsz, "mod path is too long");
            rc = -1;
            break;
        }
        if (vapor_mod_rel_ok(child_rel, child_rel, sizeof(child_rel)) != 0) {
            set_errf(err, errsz, "mod path \"%s\" is not allowed", ent->d_name);
            rc = -1;
            break;
        }
        if (join_rel(child_abs, sizeof(child_abs), overlay_root, child_rel) != 0
            || lstat(child_abs, &st) != 0 || S_ISLNK(st.st_mode)) {
            set_errf(err, errsz, "cannot read %s", child_rel);
            rc = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (apply_walk(install_dir, overlay_root, child_rel, vanilla_dir,
                           files, err, errsz)
                != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (apply_one(install_dir, child_abs, child_rel, vanilla_dir, files, err,
                      errsz)
            != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    return rc;
}
#endif

int
vapor_mod_apply_tree(const char *install_dir, const char *overlay_dir,
                     const char *vanilla_dir, cJSON *files, char *err,
                     size_t errsz)
{
    int is_dir = 0;

    if (!install_dir || !overlay_dir || !vanilla_dir || !files
        || !cJSON_IsArray(files)) {
        set_err(err, errsz, "mod apply was given an empty path");
        return -1;
    }
    if (!path_exists(overlay_dir, &is_dir) || !is_dir) {
        set_errf(err, errsz, "mod tree is missing: %s", overlay_dir);
        return -1;
    }
    if (mkdirs(vanilla_dir) != 0) {
        set_errf(err, errsz, "cannot create %s", vanilla_dir);
        return -1;
    }
    return apply_walk(install_dir, overlay_dir, "", vanilla_dir, files, err,
                      errsz);
}

static void
remove_empty_parents(const char *install_dir, const char *rel)
{
    char   tmp[MOD_PATH];
    char  *slash;
    size_t n;

    if (snprintf(tmp, sizeof(tmp), "%s", rel) >= (int)sizeof(tmp)) {
        return;
    }
    while ((slash = strrchr(tmp, '/')) != NULL) {
        char dir[MOD_PATH];

        *slash = '\0';
        if (!tmp[0]) {
            break;
        }
        if (join_rel(dir, sizeof(dir), install_dir, tmp) != 0) {
            break;
        }
        n = strlen(install_dir);
        if (strlen(dir) <= n) {
            break;
        }
#if defined(_WIN32)
        if (_rmdir(dir) != 0) {
            break;
        }
#else
        if (rmdir(dir) != 0) {
            break;
        }
#endif
    }
}

int
vapor_mod_revert(const char *install_dir, const char *vanilla_dir,
                  const char *applied_json, char *err, size_t errsz)
{
    cJSON *root = NULL;
    cJSON *files, *it;

    if (!install_dir || !vanilla_dir) {
        set_err(err, errsz, "mod restore was given an empty path");
        return -1;
    }
    if (!applied_json || !applied_json[0]) {
        return 0;
    }
    root = cJSON_Parse(applied_json);
    if (!root) {
        set_err(err, errsz, "mod-applied.json is not valid JSON");
        return -1;
    }
    files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (!files) {
        cJSON_Delete(root);
        return 0;
    }
    if (!cJSON_IsArray(files)) {
        cJSON_Delete(root);
        set_err(err, errsz, "mod-applied.json has no file list");
        return -1;
    }
    cJSON_ArrayForEach(it, files) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "path");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(it, "had_original");
        char         rel[MOD_PATH];
        char         dest[MOD_PATH];
        char         backup[MOD_PATH];
        int          had;

        if (!cJSON_IsString(p) || !p->valuestring) {
            continue;
        }
        if (vapor_mod_rel_ok(p->valuestring, rel, sizeof(rel)) != 0) {
            cJSON_Delete(root);
            set_errf(err, errsz, "applied path \"%s\" is not allowed",
                     p->valuestring);
            return -1;
        }
        if (join_rel(dest, sizeof(dest), install_dir, rel) != 0) {
            cJSON_Delete(root);
            set_err(err, errsz, "install path is too long");
            return -1;
        }
        had = cJSON_IsNumber(h) && h->valuedouble != 0;
        if (had) {
            if (join_rel(backup, sizeof(backup), vanilla_dir, rel) != 0
                || copy_file(backup, dest) != 0) {
                cJSON_Delete(root);
                set_errf(err, errsz, "cannot restore %s", rel);
                return -1;
            }
        } else {
            remove(dest);
            remove_empty_parents(install_dir, rel);
        }
    }
    cJSON_Delete(root);
    return 0;
}

#if defined(_WIN32)
static int
remove_walk(const char *path)
{
    char             pattern[MOD_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;

    if (snprintf(pattern, sizeof(pattern), "%s\\*", path) >= (int)sizeof(pattern)) {
        return -1;
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        char child[MOD_PATH];

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName)
            >= (int)sizeof(child)) {
            FindClose(h);
            return -1;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (remove_walk(child) != 0) {
                FindClose(h);
                return -1;
            }
        } else if (remove(child) != 0) {
            FindClose(h);
            return -1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return _rmdir(path) == 0 ? 0 : -1;
}
#else
static int
remove_walk(const char *path)
{
    DIR           *d;
    struct dirent *ent;

    d = opendir(path);
    if (!d) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        char        child[MOD_PATH];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name)
            >= (int)sizeof(child)) {
            closedir(d);
            return -1;
        }
        if (lstat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (remove_walk(child) != 0) {
                closedir(d);
                return -1;
            }
        } else if (unlink(child) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -1;
}
#endif

int
vapor_mod_remove_tree(const char *path)
{
    int is_dir = 0;

    if (!path || !path[0]) {
        return -1;
    }
    if (!path_exists(path, &is_dir)) {
        return 0;
    }
    if (!is_dir) {
        return remove(path) == 0 ? 0 : -1;
    }
    return remove_walk(path);
}
