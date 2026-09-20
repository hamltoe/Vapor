# Operations

How to put vapord on a Linux host and point clients at it. For the
from-source walkthrough (bootstrap, build, first game), use the
[root README](../README.md).

## Prerequisites

**Linux (WSL or a real box)**

```
sudo bash scripts/bootstrap-linux.sh
```

That installs a compiler, CMake, libcurl, libsodium, SDL2, and OpenGL
headers. Then vendor the single-file libraries:

```
bash scripts/vendor-deps.sh
bash scripts/build-linux.sh
```

Binaries land in `build-linux/bin/`: `vapord` (host window + HTTP),
`vapor-gui`, `vapor-admin`, `vapor`, `vapor-selftest`.

**Windows**

Visual Studio Build Tools plus CMake. From PowerShell:

```
powershell -File scripts/vendor-deps.ps1
powershell -File scripts/build-windows.ps1
```

The Windows build produces the player client (`vapor-gui`, plus `vapor`
for scripts). vapord needs libsodium and is not built there. SDL2 is fetched
into `third_party/sdl2` by the vendor script.

Pass `-NoGui` to `build-windows.ps1` only if you must skip SDL2.

## First local run

On Linux, in two terminals:

```
# server (opens a host window; add --headless for a terminal-only run)
./build-linux/bin/vapord -H 127.0.0.1 -p 8777 \
    -r ./run/content -L ./run/library -d ./run/vapor.db

# drop a game folder into ./run/library, or ingest by hand:
./build-linux/bin/vapor-admin -r ./run/content -d ./run/vapor.db add ./mygame \
    --id my-game --name "My Game" --version 1.0.0 \
    --linux-exec game --cover ./art/cover.png

# client
./build-linux/bin/vapor config server http://127.0.0.1:8777
./build-linux/bin/vapor register
./build-linux/bin/vapor list
./build-linux/bin/vapor install my-game
./build-linux/bin/vapor launch my-game
```

`VAPOR_PASSWORD` supplies the password without a prompt, which the smoke
tests use. `VAPOR_DATA_DIR` keeps client state out of your real profile.

`scripts/smoke-test.sh` (Linux) and `scripts/smoke-test-windows.ps1`
(Windows client against a Linux server) walk the same loop automatically.

The GUI is the same loop with a window:

```
./build-linux/bin/vapor-gui
# or, to start an install without clicking:
./build-linux/bin/vapor-gui --install my-game
```

## Deploying vapord

On a garage box with a monitor, run `vapord` from a desktop session so the
host window stays up. `deploy/vapord.service` is the unattended path and
passes `--headless`.

Recommended layout on the Linux host:

| Path | What |
| --- | --- |
| `/usr/local/bin/vapord` | Server binary |
| `/usr/local/bin/vapor-admin` | Ingest tool |
| `/etc/vapor/vapord.conf` | Copied from `deploy/vapord.conf` |
| `/var/lib/vapor/vapor.db` | Catalog and accounts |
| `/srv/vapor/library` | Drop folder: one subdirectory per game |
| `/srv/vapor/content` | Packaged archives and cover art |

Create a dedicated user, then install the unit:

```
sudo useradd --system --home /var/lib/vapor --shell /usr/sbin/nologin vapor
sudo mkdir -p /etc/vapor /var/lib/vapor /srv/vapor/content /srv/vapor/library
sudo cp deploy/vapord.conf /etc/vapor/vapord.conf
sudo cp deploy/vapord.service /etc/systemd/system/
sudo chown -R vapor:vapor /var/lib/vapor /srv/vapor/content /srv/vapor/library
sudo systemctl daemon-reload
sudo systemctl enable --now vapord
```

The example unit binds vapord to localhost. Do not expose port 8777 on
the public interface; put TLS in front of it.

## Auto-discovery

Point `library_root` at a directory of games (config `library_root`, or
`vapord -L PATH`). Each immediate subdirectory is one title:

```
/srv/vapor/library/
  Hollow Knight/
    HollowKnight.zip      # preferred: served in place
    cover.png             # optional
  Some RPG/
    disc.iso              # unpacked into content_root as package.zip;
                          # original left in place
  Portable Game/
    Game.exe              # unpacked tree; zipped into content_root
    data/
  Override Me/
    vapor.json            # optional id/name/version/exec/cover
    setup.zip
```

The folder name becomes the catalog title; a slug of that name is the
id (`Hollow Knight` → `hollow-knight`). Discovery prefers a zip, then
an unpacked executable tree, then an ISO. An ISO is unpacked (ISO 9660 /
Joliet). If the disc ships a Wise installer, that is unpacked too and
those files are zipped; the original disc image is left in
the drop folder. After unpack, `autorun.exe` / `autorun.inf` are dropped.
A tiny root `*.DAT` is removed only when a larger file of the same name
exists in a subdirectory (a CD volume stub next to real data). Launch
picking ignores installers plus updaters named `upd.exe`, `*up.exe`, or
`*update.exe`.

After a folder is published, vapord asks Steam for cover art, a short
description, and the public review score when those fields are empty.

Drop a new folder in and wait up to an hour (`discover_interval`, default
3600 seconds), hit **Discover now** in the host window, or run
`vapor-admin -L PATH discover`. Removing a folder drops that game from
the catalog; bytes on disk in `content_root` are left alone.

`vapor-admin add` still works for a fully specified ingest. An
admin-published id is not overwritten by discovery.

## TLS: Caddy (recommended)

vapord speaks plain HTTP. Caddy on 443 terminates TLS with Let's Encrypt
and forwards to `127.0.0.1:8777`.

1. Point a hostname at your static IP (DuckDNS, Cloudflare, or your own
   DNS).
2. Edit `deploy/Caddyfile` and replace `vapor.example.com`.
3. Install and start Caddy:

```
sudo apt install caddy
sudo cp deploy/Caddyfile /etc/caddy/Caddyfile
sudo systemctl enable --now caddy
```

Clients then use HTTPS:

```
vapor config server https://vapor.example.com
```

No pin is required: the system CA store already trusts Let's Encrypt.

## TLS: self-signed pin (no DNS)

If you would rather not involve a public name, generate a long-lived
certificate, keep vapord (or a tiny TLS proxy) on HTTPS, and pin the
public key in the Linux client.

libcurl wants the pin in SPKI form:

```
sha256//BASE64_OF_THE_SUBJECT_PUBLIC_KEY_INFO
```

Set it with:

```
vapor config pin 'sha256//…'
vapor config pin none          # back to normal CA checks
```

The pin is stored in `config.ini` as `pinned_pubkey` and passed to
`CURLOPT_PINNEDPUBLICKEY`. The Windows WinHTTP backend refuses to
connect when a pin is set rather than silently falling back to CA
checks. Use Caddy plus a public certificate for Windows clients.

## Cover art and descriptions

Pass `--cover FILE` to `vapor-admin add`. PNG or JPEG, 4 MiB or smaller.

Discovery also looks up the title on the public Steam store (no API key)
and fills in empty description, developer, cover, and Steam community
score. A local `cover.png` / `cover.jpg` or a `vapor.json` description
wins over Steam. Lookups are cached for a week. Force a refresh with
`vapor-admin enrich` or `vapor-admin enrich GAME`.

The library grid fetches `/games/{id}/versions/{version}/cover` into
`covers/` under the client data directory and decodes it on the UI
thread. Click a tile to open the details page (description, ratings,
Install / Verify / Remove). A game without art still shows a text card.

## Updates

`vapor list` prints `[update available]` when the server's latest
version is newer than the installed one. In the GUI the details page
shows **Update** and runs a forced install of that version.

There is no background updater. Refresh the catalog (or reopen the GUI)
to see new versions after you ingest them.

## Closing registration

Once the household accounts exist:

```
# in vapord.conf
enable_registration = false
```

or start vapord with `--closed`. Existing sessions keep working.
`GET /health` reports `registration_open: false` so the GUI can say so
before anyone types a password.

## What this prototype does not do

- Per-user entitlements (every signed-in user sees every game).
- Wine / Proton launches (`targets[].runtime` is reserved for that).
- Cloud saves, friends, or a storefront.
- Windows-hosted vapord.
- Automatic client self-update.

Those can be added without changing the API prefix or the manifest
schema, which is the point of leaving `runtime`, `package.format`, and
`/api/v1` as extension points.
