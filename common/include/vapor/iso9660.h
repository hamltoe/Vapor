#ifndef VAPOR_ISO9660_H
#define VAPOR_ISO9660_H

#include <stddef.h>
#include <stdint.h>

/* Return non-zero to abort. `done`/`total` are payload bytes copied against
 * the image size (a close enough stand-in; unused ISO space is skipped). */
typedef int (*vapor_iso_progress_fn)(void *ud, uint64_t done, uint64_t total);

/* 1 when `path` is an ISO 9660 image: a 2048-byte .iso/.img, or a raw
 * BIN/CUE sector dump (.bin at 2352, 2336, or 2448 bytes). */
int vapor_iso_is_image(const char *path);

/* Unpack an ISO 9660 / Joliet disc image into `dest_dir`. Plain 2048-byte
 * images and raw BIN/CUE sectors are accepted. UDF-only images are not
 * supported. 0 on success; on failure fills `err` if provided. */
int vapor_iso_extract(const char *iso_path, const char *dest_dir, char *err,
                      size_t errsz);

/* Same as vapor_iso_extract; `cb` may be NULL. */
int vapor_iso_extract_progress(const char *iso_path, const char *dest_dir,
                               char *err, size_t errsz,
                               vapor_iso_progress_fn cb, void *ud);

#endif /* VAPOR_ISO9660_H */
