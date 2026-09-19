#ifndef VAPOR_UTIL_H
#define VAPOR_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Game ids land in filesystem paths on both the server and the client, so this
 * is the single chokepoint that keeps traversal and shell-hostile characters
 * out: [a-z0-9] first, then [a-z0-9._-], no "..", 1..64 chars. */
#define VAPOR_ID_MAX 64
int vapor_id_is_valid(const char *id);

/* Account names are not path segments, but they are unique keys. 3-32 chars of
 * letters, digits, dot, dash or underscore. Shared by vapord and libvapor so
 * the client rejects locally what the server would reject. */
int vapor_username_is_valid(const char *username);

/* Version strings are also path segments. Slightly looser than ids (uppercase
 * and '+' are allowed) but still traversal-proof. */
#define VAPOR_VERSION_MAX 64
int vapor_version_is_valid(const char *version);

/* Natural-order version compare: numeric runs compare numerically so that
 * "1.10" > "1.9". Returns <0, 0, >0. */
int vapor_version_cmp(const char *a, const char *b);

char *vapor_strdup(const char *s);
int   vapor_str_eq_ci(const char *a, const char *b);
int   vapor_str_has_prefix(const char *s, const char *prefix);
int   vapor_str_ends_with_ci(const char *s, const char *suffix);

void vapor_hex_encode(const uint8_t *in, size_t n, char *out);
int  vapor_hex_decode(const char *hex, uint8_t *out, size_t outsz);

int64_t vapor_now_unix(void);

/* "1.4 GiB" style, for CLI and GUI progress output. */
void vapor_format_bytes(uint64_t n, char *out, size_t outsz);
void vapor_format_duration(int64_t seconds, char *out, size_t outsz);

/* Supports '*' (any run, including '/') and '?'. Used for manifest exec_bits. */
int vapor_glob_match(const char *pattern, const char *str);

/* Reads a whole file into a malloc'd NUL-terminated buffer. */
char *vapor_read_file(const char *path, size_t *out_len);

/* Overwrites a buffer that held a secret. Written so the compiler cannot treat
 * the store as dead and remove it, which plain memset is allowed to be. */
void vapor_secure_zero(void *p, size_t n);

#endif /* VAPOR_UTIL_H */
