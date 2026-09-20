#ifndef VAPOR_WISE_H
#define VAPOR_WISE_H

#include <stddef.h>

/* Unpack a Wise Installation Wizard SETUP.EXE (PE overlay of raw deflate
 * streams) into `dest_dir`. `%MAINDIR%` files are written relative to
 * dest_dir; installer-only and system destinations are skipped.
 *
 * Returns 0 if at least one file was written, 1 if `exe_path` is not a Wise
 * installer, and -1 on a hard failure (fills `err` when provided). */
int vapor_wise_extract(const char *exe_path, const char *dest_dir, char *err,
                       size_t errsz);

/* Drop CD-only leftovers (autorun, tiny HL.DAT stubs) so the tree looks like
 * a SETUP install instead of a copied disc. Safe to call more than once. */
void vapor_disc_finish_install(const char *dir);

#endif /* VAPOR_WISE_H */
