#ifndef VAPOR_ISO9660_H
#define VAPOR_ISO9660_H

#include <stddef.h>

/* Unpack an ISO 9660 / Joliet disc image into `dest_dir`. UDF-only images are
 * not supported. 0 on success; on failure fills `err` if provided. */
int vapor_iso_extract(const char *iso_path, const char *dest_dir, char *err,
                      size_t errsz);

#endif /* VAPOR_ISO9660_H */
