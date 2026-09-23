#if !defined(_WIN32)

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vapor/util.h"

int
vapor_plat_data_dir(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (xdg && *xdg == '/') {
        snprintf(out, outsz, "%s/vapor", xdg);
        return 0;
    }
    if (home && *home) {
        snprintf(out, outsz, "%s/.local/share/vapor", home);
        return 0;
    }
    return -1;
}

int
vapor_plat_default_library_dir(char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (home && *home) {
        snprintf(out, outsz, "%s/Games/Vapor", home);
        return 0;
    }
    return -1;
}

int
vapor_plat_mkdirs(const char *path)
{
    char   tmp[4096];
    size_t i, n;

    n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, n + 1);

    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            tmp[i] = saved;
        }
    }
    return 0;
}

int
vapor_plat_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int
vapor_plat_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int
vapor_plat_file_size(const char *path, uint64_t *out)
{
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    *out = (uint64_t)st.st_size;
    return 0;
}

int
vapor_plat_remove_tree(const char *path)
{
    DIR           *d;
    struct dirent *ent;
    char           child[4096];
    struct stat    st;

    if (lstat(path, &st) != 0) {
        return 0; /* already gone */
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? 0 : -1;
    }

    d = opendir(path);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if ((size_t)snprintf(child, sizeof(child), "%s/%s", path, ent->d_name)
            >= sizeof(child)) {
            closedir(d);
            return -1;
        }
        if (vapor_plat_remove_tree(child) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -1;
}

int
vapor_plat_dir_size(const char *path, uint64_t *out)
{
    DIR           *d;
    struct dirent *ent;
    char           child[4096];
    struct stat    st;

    if (lstat(path, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        *out += (uint64_t)st.st_size;
        return 0;
    }

    d = opendir(path);
    if (!d) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if ((size_t)snprintf(child, sizeof(child), "%s/%s", path, ent->d_name)
            >= sizeof(child)) {
            continue;
        }
        vapor_plat_dir_size(child, out);
    }
    closedir(d);
    return 0;
}

int
vapor_plat_make_executable(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }
    /* Mirror the read bits into the execute bits: a file the owner can read
     * becomes owner-executable, and likewise for group and other. */
    if (st.st_mode & S_IRUSR) { st.st_mode |= S_IXUSR; }
    if (st.st_mode & S_IRGRP) { st.st_mode |= S_IXGRP; }
    if (st.st_mode & S_IROTH) { st.st_mode |= S_IXOTH; }
    return chmod(path, st.st_mode & 07777) == 0 ? 0 : -1;
}

void
vapor_plat_native_path(char *path)
{
    (void)path; /* '/' is already native */
}

static int
chmod_matching_walk(const char *root, const char *rel, const char *pattern,
                    size_t *count)
{
    char           abs[4096];
    DIR           *d;
    struct dirent *ent;

    if ((size_t)snprintf(abs, sizeof(abs), "%s%s%s", root, *rel ? "/" : "", rel)
        >= sizeof(abs)) {
        return -1;
    }
    d = opendir(abs);
    if (!d) {
        return -1;
    }

    while ((ent = readdir(d)) != NULL) {
        char        child_rel[4096];
        char        child_abs[4096];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if ((size_t)snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel,
                             *rel ? "/" : "", ent->d_name)
            >= sizeof(child_rel)) {
            continue;
        }
        if ((size_t)snprintf(child_abs, sizeof(child_abs), "%s/%s", root,
                             child_rel)
            >= sizeof(child_abs)) {
            continue;
        }
        if (lstat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            chmod_matching_walk(root, child_rel, pattern, count);
            continue;
        }
        if (S_ISREG(st.st_mode) && vapor_glob_match(pattern, child_rel)) {
            if (vapor_plat_make_executable(child_abs) == 0) {
                (*count)++;
            }
        }
    }
    closedir(d);
    return 0;
}

int
vapor_plat_chmod_matching(const char *root, const char *pattern, size_t *count)
{
    return chmod_matching_walk(root, "", pattern, count);
}

static int
skip_walk_name(const char *name)
{
    return name[0] == '.' || strcmp(name, "__MACOSX") == 0;
}

static int
walk_files(const char *root, const char *rel, vapor_plat_walk_fn fn, void *ud)
{
    char           abs[4096];
    DIR           *d;
    struct dirent *ent;
    int            rc = 0;

    if ((size_t)snprintf(abs, sizeof(abs), "%s%s%s", root, *rel ? "/" : "", rel)
        >= sizeof(abs)) {
        return -1;
    }
    d = opendir(abs);
    if (!d) {
        return -1;
    }

    while ((ent = readdir(d)) != NULL) {
        char        child_rel[4096];
        char        child_abs[4096];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (skip_walk_name(ent->d_name)) {
            continue;
        }
        if ((size_t)snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel,
                             *rel ? "/" : "", ent->d_name)
            >= sizeof(child_rel)) {
            continue;
        }
        if ((size_t)snprintf(child_abs, sizeof(child_abs), "%s/%s", root,
                             child_rel)
            >= sizeof(child_abs)) {
            continue;
        }
        if (lstat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            rc = walk_files(root, child_rel, fn, ud);
            if (rc != 0) {
                break;
            }
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            rc = fn(child_rel, child_abs, ud);
            if (rc != 0) {
                break;
            }
        }
    }
    closedir(d);
    return rc;
}

int
vapor_plat_walk_files(const char *root, vapor_plat_walk_fn fn, void *ud)
{
    if (!root || !fn) {
        return -1;
    }
    return walk_files(root, "", fn, ud);
}

int
vapor_plat_run(const char *exec, char *const argv[], const char *cwd,
               const vapor_kv *env, size_t nenv, int *out_exit)
{
    pid_t pid;
    int   status = 0;

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        size_t i;
        /* Child: setenv then exec. Anything that fails here must _exit rather
         * than return, or we would end up with two copies of the caller. */
        for (i = 0; i < nenv; i++) {
            if (env[i].key && env[i].value) {
                setenv(env[i].key, env[i].value, 1);
            }
        }
        if (cwd && *cwd && chdir(cwd) != 0) {
            _exit(127);
        }
        execv(exec, argv);
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (out_exit) {
        *out_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return 0;
}

int
vapor_plat_run_ui(const char *exec, const char *cwd, const char *params,
                  int *out_exit)
{
    char *argv[2];

    argv[0] = (char *)exec;
    argv[1] = NULL;
    (void)params;
    return vapor_plat_run(exec, argv, cwd, NULL, 0, out_exit);
}

int
vapor_plat_find_product_dir(const char *name, const char *id, char *out,
                            size_t outsz)
{
    (void)name;
    (void)id;
    if (out && outsz) {
        out[0] = '\0';
    }
    return 1;
}

int
vapor_plat_guess_product_dir(const char *name, const char *id, char *out,
                             size_t outsz)
{
    return vapor_plat_find_product_dir(name, id, out, outsz);
}

int
vapor_plat_search_path(const char *name, char *out, size_t outsz)
{
    const char *path, *p, *sep;
    char        dir[VAPOR_WIN_PATH];
    char        cand[VAPOR_WIN_PATH];
    size_t      n;

    if (!name || !*name || !out || outsz == 0) {
        return 1;
    }
    out[0] = '\0';
    path = getenv("PATH");
    if (!path) {
        return 1;
    }
    p = path;
    while (*p) {
        sep = strchr(p, ':');
        n = sep ? (size_t)(sep - p) : strlen(p);
        if (n >= sizeof(dir)) {
            n = sizeof(dir) - 1;
        }
        memcpy(dir, p, n);
        dir[n] = '\0';
        if (snprintf(cand, sizeof(cand), "%s/%s", dir[0] ? dir : ".", name)
            < (int)sizeof(cand)
            && vapor_plat_exists(cand) && !vapor_plat_is_dir(cand)) {
            if ((size_t)snprintf(out, outsz, "%s", cand) >= outsz) {
                out[0] = '\0';
                return 1;
            }
            return 0;
        }
        if (!sep) {
            break;
        }
        p = sep + 1;
    }
    return 1;
}

#endif /* !_WIN32 */
