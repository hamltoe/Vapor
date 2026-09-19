#include "vapord.h"

#include <stdio.h>
#include <string.h>

#include <sodium.h>

#include "vapor/util.h"

int
vapord_auth_init(char *err, size_t errsz)
{
    if (sodium_init() < 0) {
        snprintf(err, errsz, "libsodium failed to initialise");
        return -1;
    }
    /* Password hashes are 128-byte NUL-terminated strings that carry their own
     * salt and parameters, so a future parameter bump verifies old hashes. */
    if (crypto_pwhash_STRBYTES > 256) {
        snprintf(err, errsz, "unexpected crypto_pwhash_STRBYTES");
        return -1;
    }
    return 0;
}

int
vapord_password_hash(const char *password, char *out, size_t outsz)
{
    if (outsz < crypto_pwhash_STRBYTES) {
        return -1;
    }
    if (crypto_pwhash_str(out, password, strlen(password),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE)
        != 0) {
        return -1; /* out of memory */
    }
    return 0;
}

int
vapord_password_verify(const char *stored, const char *password)
{
    return crypto_pwhash_str_verify(stored, password, strlen(password)) == 0
               ? 0
               : -1;
}

void
vapord_token_mint(char out[VAPOR_TOKEN_HEX_LEN + 1])
{
    unsigned char raw[VAPOR_TOKEN_BYTES];

    randombytes_buf(raw, sizeof(raw));
    vapor_hex_encode(raw, sizeof(raw), out);
    sodium_memzero(raw, sizeof(raw));
}

/* Only the digest is persisted, so a stolen database yields no live sessions.
 * A plain SHA-256 is right here: the token is 256 bits of CSPRNG output, so
 * there is nothing to brute-force and no need for a slow KDF. */
void
vapord_token_fingerprint(const char *token_hex,
                         char out[VAPOR_SHA256_HEX_LEN + 1])
{
    vapor_sha256_hex_buf(token_hex, strlen(token_hex), out);
}
