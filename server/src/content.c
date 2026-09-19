#include "vapord.h"

#include <errno.h>
#include <stdio.h>
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
