#include "vapord.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "vapor/util.h"

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

int
vapord_content_path(const vapord_config *cfg, const char *game_id,
                    const char *version, const char *file,
                    char *out, size_t outsz)
{
    int n;

    /* Every caller funnels through here, so validating the segments once is
     * what keeps request-supplied ids out of the rest of the filesystem. */
    if (!vapor_id_is_valid(game_id)) {
        return -1;
    }
    if (version && !vapor_version_is_valid(version)) {
        return -1;
    }
    if (file) {
        if (!*file || strchr(file, '/') || strchr(file, '\\')
            || strcmp(file, ".") == 0 || strstr(file, "..")) {
            return -1;
        }
    }

    if (version && file) {
        n = snprintf(out, outsz, "%s/%s/%s/%s",
                     cfg->content_root, game_id, version, file);
    } else if (version) {
        n = snprintf(out, outsz, "%s/%s/%s", cfg->content_root, game_id, version);
    } else {
        n = snprintf(out, outsz, "%s/%s", cfg->content_root, game_id);
    }

    if (n < 0 || (size_t)n >= outsz) {
        return -1;
    }
    return 0;
}

int
vapord_content_mkdirs(const char *path)
{
    char   tmp[VAPORD_PATH_MAX];
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
            if (MKDIR(tmp) != 0 && errno != EEXIST) {
                return -1;
            }
            tmp[i] = saved;
        }
    }
    return 0;
}

static int
rel_path_ok(const char *rel)
{
    const char *p;

    if (!rel || !*rel) {
        return 0;
    }
    if (rel[0] == '/' || rel[0] == '\\') {
        return 0;
    }
    if (strchr(rel, '\\')) {
        return 0;
    }
    p = rel;
    while (*p) {
        const char *seg = p;
        size_t      n;

        while (*p && *p != '/') {
            p++;
        }
        n = (size_t)(p - seg);
        if (n == 0 || (n == 1 && seg[0] == '.')
            || (n == 2 && seg[0] == '.' && seg[1] == '.')) {
            return 0;
        }
        if (*p == '/') {
            p++;
        }
    }
    return 1;
}

int
vapord_library_path(const vapord_config *cfg, const char *rel, char *out,
                    size_t outsz)
{
    int n;

    if (!cfg->library_root[0] || !rel_path_ok(rel)) {
        return -1;
    }
    n = snprintf(out, outsz, "%s/%s", cfg->library_root, rel);
    if (n < 0 || (size_t)n >= outsz) {
        return -1;
    }
    return 0;
}

int
vapord_library_resolve(const vapord_config *cfg, const char *rel, char *out,
                       size_t outsz)
{
    char  joined[VAPORD_PATH_MAX];
    char *root_real = NULL;
    char *file_real = NULL;
    size_t n;
    int    rc = -1;

    if (vapord_library_path(cfg, rel, joined, sizeof(joined)) != 0) {
        return -1;
    }
    root_real = realpath(cfg->library_root, NULL);
    file_real = realpath(joined, NULL);
    if (!root_real || !file_real) {
        goto done;
    }
    n = strlen(root_real);
    if (n == 0 || strncmp(file_real, root_real, n) != 0) {
        goto done;
    }
    if (file_real[n] != '\0' && file_real[n] != '/') {
        goto done;
    }
    if (strlen(file_real) >= outsz) {
        goto done;
    }
    memcpy(out, file_real, strlen(file_real) + 1);
    rc = 0;

done:
    free(root_real);
    free(file_real);
    return rc;
}
