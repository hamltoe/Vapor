#ifndef VAPOR_PROTOCOL_H
#define VAPOR_PROTOCOL_H

/* Wire contract shared by vapord and libvapor. Bump VAPOR_API_PREFIX rather
 * than changing the shape of an existing endpoint. */

#define VAPOR_VERSION_STRING "0.1.0"
#define VAPOR_API_PREFIX     "/api/v1"
#define VAPOR_DEFAULT_PORT   8777
#define VAPOR_USER_AGENT     "vapor-client/" VAPOR_VERSION_STRING

#define VAPOR_EP_HEALTH   VAPOR_API_PREFIX "/health"
#define VAPOR_EP_REGISTER VAPOR_API_PREFIX "/auth/register"
#define VAPOR_EP_LOGIN    VAPOR_API_PREFIX "/auth/login"
#define VAPOR_EP_LOGOUT   VAPOR_API_PREFIX "/auth/logout"
#define VAPOR_EP_ME       VAPOR_API_PREFIX "/me"
#define VAPOR_EP_GAMES    VAPOR_API_PREFIX "/games"
#define VAPOR_EP_DOWNLOAD VAPOR_API_PREFIX "/download"

/* Largest cover art the client will accept, so a hostile or misconfigured
 * server cannot make it allocate without bound. */
#define VAPOR_MAX_COVER_BYTES (4 * 1024 * 1024)

/* Catalog blurb. Steam short_description fits comfortably; longer about-text
 * is truncated rather than rejected. */
#define VAPOR_DESC_MAX 2048

/* Opaque bearer tokens: 32 random bytes, hex on the wire, stored server-side
 * as a SHA-256 so a database leak does not yield usable sessions. */
#define VAPOR_TOKEN_BYTES       32
#define VAPOR_TOKEN_HEX_LEN     64
#define VAPOR_TOKEN_TTL_SECONDS (30 * 24 * 3600)

#define VAPOR_USERNAME_MIN 3
#define VAPOR_USERNAME_MAX 32
#define VAPOR_PASSWORD_MIN 8
#define VAPOR_PASSWORD_MAX 256

/* Bodies larger than this are rejected before allocation. */
#define VAPOR_MAX_REQUEST_BODY (64 * 1024)

/* Stable machine-readable values for the "error" field of a JSON error body. */
#define VAPOR_ERR_BAD_REQUEST   "bad_request"
#define VAPOR_ERR_UNAUTHORIZED  "unauthorized"
#define VAPOR_ERR_FORBIDDEN     "forbidden"
#define VAPOR_ERR_NOT_FOUND     "not_found"
#define VAPOR_ERR_CONFLICT      "conflict"
#define VAPOR_ERR_INTERNAL      "internal"
#define VAPOR_ERR_DISABLED      "disabled"

#endif /* VAPOR_PROTOCOL_H */
