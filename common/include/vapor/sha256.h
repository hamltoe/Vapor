#ifndef VAPOR_SHA256_H
#define VAPOR_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define VAPOR_SHA256_DIGEST_LEN 32
#define VAPOR_SHA256_HEX_LEN    64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  block[64];
    size_t   blocklen;
} vapor_sha256;

void vapor_sha256_init(vapor_sha256 *c);
void vapor_sha256_update(vapor_sha256 *c, const void *data, size_t len);
void vapor_sha256_final(vapor_sha256 *c, uint8_t out[VAPOR_SHA256_DIGEST_LEN]);

/* Convenience: one-shot digest, and lowercase hex of a digest. */
void vapor_sha256_buf(const void *data, size_t len,
                      uint8_t out[VAPOR_SHA256_DIGEST_LEN]);
void vapor_sha256_hex(const uint8_t digest[VAPOR_SHA256_DIGEST_LEN],
                      char out[VAPOR_SHA256_HEX_LEN + 1]);
void vapor_sha256_hex_buf(const void *data, size_t len,
                          char out[VAPOR_SHA256_HEX_LEN + 1]);

/* Streams `path` in chunks. Returns 0 on success, -1 if it could not be read. */
int vapor_sha256_file(const char *path, char out[VAPOR_SHA256_HEX_LEN + 1]);

#endif /* VAPOR_SHA256_H */
