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

/* Per-user state directory: %LOCALAPPDATA%\Vapor, the XP
 * "Local Settings\Application Data" folder, or $XDG_DATA_HOME/vapor. */
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

/* Non-zero means the caller wants the running installer stopped. */
typedef int (*vapor_plat_cancel_fn)(void *ud);

/* Show a windowed installer and wait until it and any helper processes it
 * started (msiexec, a second setup.exe, InstallShield engines) have exited.
 * `params` is the argument string (silent flags, msiexec switches); NULL
 * means none. On Windows this uses ShellExecuteEx so a required-administrator
 * manifest can prompt for elevation. `cancel` is polled while waiting; when
 * it returns non-zero the launched process and new helper processes are
 * terminated. Returns 0 if the process exited (*out_exit set), 1 if cancel
 * fired, -1 if it could not be started. `cancel` may be NULL. */
int vapor_plat_run_ui(const char *exec, const char *cwd, const char *params,
                      int *out_exit, vapor_plat_cancel_fn cancel, void *ud);

/* Look up a Windows Uninstall entry / Program Files folder matching `name`
 * or `id`. 0 if `out` was filled, 1 if nothing matched. The folder must
 * already contain files (an empty InstallShield destination is ignored). */
int vapor_plat_find_product_dir(const char *name, const char *id, char *out,
                                size_t outsz);

/* Same lookup, but an empty matching folder is returned so the caller can
 * wait for the installer to fill it. */
int vapor_plat_guess_product_dir(const char *name, const char *id, char *out,
                                 size_t outsz);

/* Resolve `name` on PATH. 0 if `out` is filled. */
int vapor_plat_search_path(const char *name, char *out, size_t outsz);

/* Windows uninstall entry. Prefers a key whose InstallLocation matches
 * `install_dir`, then a DisplayName slug match on `name` / `id`. Prefers
 * QuietUninstallString. 0 if `out_exe` was filled. `out_dir` receives
 * InstallLocation when the key has one. Steam (steam.exe / steam://)
 * entries are ignored. */
int vapor_plat_find_uninstall(const char *name, const char *id,
                              const char *install_dir, char *out_exe,
                              size_t exesz, char *out_params, size_t paramsz,
                              char *out_dir, size_t dirsz);

/* Implicit DLL imports of a Windows executable that the loader would not
 * find beside the exe or in a system directory. 0 if nothing is missing,
 * 1 if `out` lists the missing names, -1 if `exe` is not a readable PE
 * (the caller should launch anyway). */
int vapor_plat_missing_dlls(const char *exe, char *out, size_t outsz);

/* Path separator normalisation: the manifest always uses '/', Windows APIs
 * mostly accept it, but launching wants native separators. */
void vapor_plat_native_path(char *path);

/* Absolute form of `path`. 0 if `out` was filled. The path should already
 * exist; this is for remembering a program the user pointed at. */
int vapor_plat_absolute(const char *path, char *out, size_t outsz);

/* Modal "open file" dialog. 0 if `out` was filled, 1 if cancelled, -1 if no
 * dialog is available on this system. */
int vapor_plat_pick_file(char *out, size_t outsz);

/* Non-recursive directory listing. `fn` is called with each entry name.
 * Return 0 from `fn` to continue, or non-zero to stop (that value is returned). */
typedef int (*vapor_plat_dir_fn)(const char *name, void *ud);
int vapor_plat_list_dir(const char *dir, vapor_plat_dir_fn fn, void *ud);

/* Hand `url` to the system (a steam:// link, for example). Does not wait for
 * the target program. 0 if the handler accepted it. */
int vapor_plat_open_url(const char *url);

/* Start `exe` with `params` (may be NULL) and do not wait. 0 if the shell
 * accepted the request. */
int vapor_plat_start(const char *exe, const char *params);

#endif /* VAPOR_PLATFORM_H */
