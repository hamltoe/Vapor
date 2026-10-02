#ifndef VAPOR_SAVES_H
#define VAPOR_SAVES_H

#include <stddef.h>
#include <stdint.h>

/* Account save archives. Paths are admin-authored in vapor.json and copied
 * onto the manifest. The client never invents roots by scanning the disk. */

#define VAPOR_SAVE_PATH_MAX       1024
#define VAPOR_SAVE_MAX_PATHS      32
#define VAPOR_SAVE_SYNC_MAX_FILES 10000
#define VAPOR_SAVE_SYNC_MAX_BYTES (256ull * 1024 * 1024)

/* 1 when `mode` is portable, setup, unpack_disc, or keep_disc. */
int vapor_install_mode_is_known(const char *mode);

/* 1 when `path` may be stored as a save root. Rejects an empty path, a ".."
 * segment, and a bare drive or filesystem root. A folder on any drive, an
 * environment token (%USERPROFILE%, $HOME, $INSTALL_DIR), or a path relative
 * to the install directory is accepted. */
int vapor_save_path_is_valid(const char *path);

/* Expand $INSTALL_DIR, $HOME, and %NAME% and, when the result is relative,
 * join it onto `install_dir`. 0 on success, 1 when a named environment
 * variable is unset (the caller skips that root), -1 when the path is illegal. */
int vapor_save_resolve(const char *spec, const char *install_dir, char *out,
                       size_t outsz, char *err, size_t errsz);

/* 1 when `base` is setup.com or install.com. */
int vapor_dos_name_is_junk(const char *path);

/* 1 when `data` is an MZ image with no PE signature at e_lfanew, 0 when it is
 * not a DOS executable, -1 when the buffer is too short to see e_lfanew. */
int vapor_image_is_dos_mz(const uint8_t *data, size_t len);

/* 1 when `path` is a DOS .COM (and not junk) or an MZ executable without a PE
 * signature. 0 otherwise, including when the file cannot be read. */
int vapor_file_is_dos_exe(const char *path);

/* Same check for one zip entry. `entry` is the name stored in the archive. */
int vapor_zip_entry_is_dos_exe(const char *zip_path, const char *entry);

/* Zip the resolved roots into `zip_path`. Entry names are "<index>/<rel>" for
 * a directory root and "<index>" for a single file, so unpack can only write
 * back under those roots. 0 on success. *out_missing is 1 when a root does not
 * exist; the zip is not written in that case so a partial tree cannot replace
 * the account copy. -1 on a hard error (`err` explains it). */
int vapor_saves_pack(const char *const *specs, size_t nspecs,
                     const char *install_dir, const char *zip_path,
                     int *out_missing, char *err, size_t errsz);

/* Write a save zip back under the declared roots. *out_skipped is the number
 * of roots that could not be created. 0 on success, -1 on a hard error. */
int vapor_saves_unpack(const char *zip_path, const char *const *specs,
                       size_t nspecs, const char *install_dir, int *out_skipped,
                       char *err, size_t errsz);

#endif /* VAPOR_SAVES_H */
