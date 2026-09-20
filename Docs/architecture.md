# Architecture

Three binaries plus one shared client library. The server is Linux-only.
The client is the same C11 code on Windows and Linux; only the platform
layer and HTTP transport differ.

```
                    Linux host (static IP)
  ┌─────────────────────────────────────────────────┐
  │  Caddy :443  ──►  vapord :8777                  │
  │                     │                           │
  │                     ├── SQLite  users, tokens,  │
  │                     │           games, versions │
  │                     ├── library_root            │
  │                     │     <Game Title>/         │
  │                     │       *.zip | *.iso       │
  │                     │       or unpacked files   │
  │                     └── content_root            │
  │                           <id>/<version>/       │
  │                             package.zip         │
  │                             manifest.json       │
  │                             cover.png           │
  │  vapor-admin ──► same SQLite, library, content  │
  └─────────────────────────────────────────────────┘
                         ▲
            HTTPS JSON + ranged GET
                         │
  ┌─────────────────────────────────────────────────┐
  │  Player PC (Windows or Linux)                   │
  │    vapor / vapor-gui                            │
  │      └── libvapor                               │
  │            ├── local SQLite (installs)          │
  │            └── library directory (extracted)    │
  └─────────────────────────────────────────────────┘
```

## Binaries

| Binary | Links | Role |
| --- | --- | --- |
| `vapord` | civetweb, libsodium, SQLite, cJSON | HTTP API |
| `vapor-admin` | same server core, no HTTP | Ingest a game folder |
| `vapor` | libvapor | CLI frontend |
| `vapor-gui` | libvapor, Nuklear, SDL2, stb_image | GUI frontend |
| `vapor-selftest` | common only | SHA-256, versions, manifests |

`vapor-admin` reuses `vapord_core` (config, database, auth helpers, content
paths) and does not link civetweb. That is why `vapord.h` forward-declares
`struct mg_connection` instead of including `civetweb.h`.

## Stack

Chosen for permissive licenses (MIT / BSD / public domain) so they sit
comfortably next to the repo's GPL-3.0 `LICENSE`. Single-file libraries
are vendored into `third_party/` and are not committed; run
`scripts/vendor-deps.ps1` or `scripts/vendor-deps.sh` after a clone.

| Concern | Library | Notes |
| --- | --- | --- |
| Server HTTP | civetweb | Built with `NO_SSL`. Range / `If-Modified-Since` come from its file handler. |
| Client HTTP (Linux) | libcurl | Resume, progress, `CURLOPT_PINNEDPUBLICKEY`. |
| Client HTTP (Windows) | WinHTTP | Same request shape; public-key pinning is not implemented here. |
| Passwords | libsodium | Argon2id (`crypto_pwhash_str`) and `randombytes_buf`. Server only. |
| Database | SQLite amalgamation | WAL, foreign keys, one connection, serialized. |
| JSON | cJSON | Manifests and API bodies. |
| Integrity | vendored SHA-256 | Same bytes on both ends. |
| Archives | miniz | Zip only for the prototype. |
| GUI | Nuklear + SDL2 + OpenGL 2 | Immediate mode; worker thread owns `libvapor` during jobs. |
| Cover decode | stb_image | PNG and JPEG only. |

TLS is not compiled into vapord. Put Caddy (or another reverse proxy) in
front of it, or pin a self-signed certificate in the Linux client. See
[operations.md](operations.md).

## Shared code

`common/` is the contract between server and client:

- `vapor/protocol.h` — API prefix, token sizes, username/password limits,
  error codes.
- `vapor/manifest.h` — schema-1 parse, serialize, and target selection.
- `vapor/util.h` — id/version validation, natural version compare, hex,
  glob match.
- `vapor/sha256.h` and `vapor/buf.h` — hashing and growable buffers.

`platform_win32.c` and `platform_posix.c` are the only files with OS
conditionals. Paths, directory creation, executable bits, and process
spawn are the entire platform surface.

## Client data

| Platform | State directory |
| --- | --- |
| Windows | `%LOCALAPPDATA%\Vapor` |
| Linux | `$XDG_DATA_HOME/vapor` (else `~/.local/share/vapor`) |

Override with `VAPOR_DATA_DIR` for tests or portable installs.

Inside that directory:

```
config.ini          server URL, session token, library path, optional TLS pin
library.db          installed games and playtime (not accounts)
covers/<id>-<ver>.cover
```

Games themselves live in `library_dir`, defaulting to a sibling `games`
folder. `vapor config library PATH` points it at a larger drive.

## Server data

Defaults in `deploy/vapord.conf`:

```
library_root = /srv/vapor/library
content_root = /srv/vapor/content
db_path      = /var/lib/vapor/vapor.db
```

`library_root` is the drop folder vapord scans. Each immediate
subdirectory is one game. Discovery runs at startup and then every
`discover_interval` seconds (default 60; 0 means once). Unchanged
folders are fingerprint-skipped so large archives are not re-hashed.

Zip files already sitting in `library_root` are served in place.
ISO files are unpacked (ISO 9660 / Joliet). A Wise `SETUP.EXE` on the disc
is unpacked too, and the files are zipped into
`<content_root>/<game_id>/<version>/package.zip`; the original disc image
is left in the drop folder. Folders of loose files are zipped into the
same content path. Every path segment is validated
so a hostile id or version cannot walk out of the root.

Accounts live only in the server SQLite `users` table (username, Argon2id
hash, admin flag, created_at). The client never creates a local user; it
stores a session token after a successful login against vapord.

## Threading

civetweb workers share one SQLite connection opened `FULLMUTEX` with WAL,
so a catalog read does not block a login write.

The GUI runs at most one background job. While the job is active the UI
thread must not call `libvapor`. Refresh builds a pending catalog and the
UI thread swaps it in after the worker is joined, so the draw loop never
sees the array being replaced underneath it.

## Repository layout

```
CMakeLists.txt
common/                 shared by client and server
server/src/             vapord
server/tools/           vapor-admin
client/core/            libvapor
client/cli/             vapor
client/gui/             vapor-gui
deploy/                 systemd unit, Caddyfile, example config
Docs/                   this folder
scripts/                bootstrap, vendor, build, smoke tests
third_party/            fetched, not committed
```
