#ifndef VAPOR_PLATFORM_H
#define VAPOR_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "vapor/manifest.h"

/* The only header with two implementations: platform_posix.c and
 * platform_win32.c. Everything else in libvapor is portable C11. */

/* Scratch path buffers. Matches VAPOR_PATH_MAX in client.h; kept separate so
 * the platform layer does not depend on the public client header. */
#define VAPOR_WIN_PATH 1024

/* Per-user state directory: %LOCALAPPDATA%\Vapor or $XDG_DATA_HOME/vapor. */
int vapor_plat_data_dir(char *out, size_t outsz);
/* Default place to install games, which the user can override in config. */
int vapor_plat_default_library_dir(char *out, size_t outsz);

int vapor_plat_mkdirs(const char *path);
int vapor_plat_remove_tree(const char *path);
int vapor_plat_file_size(const char *path, uint64_t *out);
int vapor_plat_exists(const char *path);
int vapor_plat_is_dir(const char *path);
int vapor_plat_make_executable(const char *path);
int vapor_plat_dir_size(const char *path, uint64_t *out);

/* Walks `root` and makes every file whose path relative to `root` matches
 * `pattern` executable, adding to *count. No-op on Windows. */
int vapor_plat_chmod_matching(const char *root, const char *pattern,
                              size_t *count);

/* Walks `root` and calls `fn` for every regular file. `rel` uses '/' even on
 * Windows. Directories named `.vapor` and names starting with '.' are skipped.
 * `fn` returns 0 to continue, or non-zero to stop (that value is returned). */
typedef int (*vapor_plat_walk_fn)(const char *rel, const char *abs, void *ud);
int vapor_plat_walk_files(const char *root, vapor_plat_walk_fn fn, void *ud);

/* Spawns and waits. `argv` is NULL-terminated with argv[0] set to `exec`.
 * `env` entries are added to the inherited environment. */
int vapor_plat_run(const char *exec, char *const argv[], const char *cwd,
                   const vapor_kv *env, size_t nenv, int *out_exit);

/* Path separator normalisation: the manifest always uses '/', Windows APIs
 * mostly accept it, but launching wants native separators. */
void vapor_plat_native_path(char *path);

#endif /* VAPOR_PLATFORM_H */
