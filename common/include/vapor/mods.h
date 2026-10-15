#ifndef VAPOR_MODS_H
#define VAPOR_MODS_H

#include <stddef.h>

#include "cJSON.h"

/* File overlay for community mods.
 *
 * `overlay_dir` is a tree whose files are copied onto `install_dir` using the
 * same relative paths. A file that already exists is copied to `vanilla_dir`
 * once, from the unmodified bytes, before it is replaced. `files` is a cJSON
 * array of {"path","had_original"} objects; entries already in the array are
 * not backed up again, so a later mod can replace an earlier mod's file and
 * restore still returns the original.
 *
 * Paths under `.vapor` and any `..` component are refused. */

/* 0 when `rel` is a relative path that stays inside the install and is not
 * under `.vapor`. Writes the slash-normalized path to `out` when non-NULL. */
int vapor_mod_rel_ok(const char *rel, char *out, size_t outsz);

int vapor_mod_apply_tree(const char *install_dir, const char *overlay_dir,
                         const char *vanilla_dir, cJSON *files, char *err,
                         size_t errsz);

/* `applied_json` is the text of mod-applied.json, or NULL when nothing was
 * applied. Puts backed-up files back and deletes paths the mods added. */
int vapor_mod_revert(const char *install_dir, const char *vanilla_dir,
                      const char *applied_json, char *err, size_t errsz);

/* Deletes a directory tree. Missing path is success. */
int vapor_mod_remove_tree(const char *path);

#endif /* VAPOR_MODS_H */
