# HTTP API

Base path: `/api/v1`. Bump the prefix rather than changing the shape of an
existing endpoint. All bodies are JSON unless noted. Dates and sizes are
numbers (Unix seconds, bytes).

Every route except `/health`, `/auth/register`, and `/auth/login` requires:

```
Authorization: Bearer <64-hex-character token>
```

## Tokens

Tokens are not JWTs. The server mints 32 random bytes, hex-encodes them
for the wire, and stores only the SHA-256 of that hex. Revocation is a
`DELETE`. There is no signing key to rotate.

TTL is 30 days (`VAPOR_TOKEN_TTL_SECONDS`). The client treats an expired
stored token as absent and asks the user to sign in again.

Username: 3–32 characters. Password: 8–256 characters. The first account
created on an empty database is an admin.

## Errors

Failure bodies look like:

```json
{
  "error": "not_found",
  "message": "no such game \"hollow-vale\""
}
```

Stable `error` values: `bad_request`, `unauthorized`, `forbidden`,
`not_found`, `conflict`, `internal`, `disabled`.

## Endpoints

### `GET /api/v1/health`

Unauthenticated liveness probe.

```json
{
  "status": "ok",
  "service": "vapord",
  "version": "0.1.0",
  "api": "/api/v1",
  "registration_open": true,
  "has_users": true
}
```

`has_users` is false on a fresh server: the next `register` becomes admin.

### `POST /api/v1/auth/register`

```json
{ "username": "ada", "password": "a long enough secret" }
```

Returns the stored account. Does not start a session; call `login` next.
Fails with `disabled` when `enable_registration` is off, and `conflict`
if the username is taken. The client refuses to send this request until
`GET /health` proves a vapord is reachable.

```json
{
  "username": "ada",
  "user_id": 1,
  "is_admin": true,
  "created_at": 1760000000
}
```

`user_id` and `created_at` come from the SQLite `users` row that was just
written. The first account on an empty database is admin.

### `POST /api/v1/auth/login`

Same body. The client also requires a live vapord before posting. Returns:

```json
{
  "token": "64 hex characters",
  "expires_at": 1770000000,
  "username": "ada",
  "user_id": 1,
  "is_admin": false
}
```

The CLI and GUI store `token` and `expires_at` in `config.ini`.

### `POST /api/v1/auth/logout`

Authenticated. Deletes the token hash. The client also wipes its local
copy. A 401 is treated as already signed out.

### `GET /api/v1/me`

```json
{ "user_id": 1, "username": "ada", "is_admin": false, "created_at": 1760000000 }
```

### `GET /api/v1/games`

Catalog, one row per game that has at least one published version.

```json
{
  "games": [
    {
      "id": "hollow-vale",
      "name": "Hollow Vale",
      "developer": "Someone",
      "description": "Short blurb.",
      "latest_version": "1.0.3",
      "size": 524288000,
      "sha256": "9f2c…",
      "published_at": 1760000000,
      "has_cover": true
    }
  ]
}
```

`latest_version` is chosen with natural version order (`1.10` after
`1.9`), not SQLite text sort. `has_cover` is true when that latest
version has art on disk.

The client merges this with local install records and sets
`installed`, `installed_version`, `update_available`, and `play_seconds`
before any frontend sees the row.

### `GET /api/v1/games/{id}`

Detail plus every published version. `has_cover` is per version.

### `GET /api/v1/games/{id}/versions/{version}/manifest`

The stored schema-1 JSON. `{version}` may be `latest`, which the server
resolves. See [manifest.md](manifest.md).

### `GET /api/v1/games/{id}/versions/{version}/cover`

The cover image (`image/png` or `image/jpeg`). 404 if the version has
none. Served through civetweb's file handler so repeat requests can 304.

The filename never comes from the URL. The server looks up the stored
cover name and builds a path inside the content root.

### `GET /api/v1/download/{id}/{version}`

The package bytes, `application/octet-stream`. Honours `Range` and
`If-Modified-Since`. `{version}` may be `latest`.

The package filename comes from the stored manifest, never from the URL.

`HEAD` is accepted on the download route so a client can probe size.

## What is not in v1

Every authenticated user sees the whole catalog. A per-user
`entitlements` table can be added later without changing the client:
the catalog route would filter, and the same JSON shape would remain.
