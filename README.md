# Vapor

Vapor is a self-hosted game library. A Linux server stores packaged
installs; a Windows or Linux client signs in, downloads, verifies,
installs, and launches them. It is a small Steam- or GOG-style manager
that you run yourself and point at a local disk or a NAS.

This README is the from-source instruction manual: how to build the
server and the client, and how to connect them for a first library.
Contracts and deployment details live in [`Docs/`](Docs/README.md).

## What you get

Four programs, one CMake project, C11.

| Program | Where it runs | Role |
| --- | --- | --- |
| `vapord` | Linux | HTTP server: accounts, catalog, ranged downloads |
| `vapor-admin` | Linux, next to the server | Package a game folder and register it |
| `vapor` | Windows or Linux | CLI: register, list, install, launch |
| `vapor-gui` | Windows or Linux | The same work, in a window |

The client never mounts the server disk. Everything goes over HTTP, so a
NAS versus a local folder is just a `library_root` path on the server.

Typical household setup: build and run **vapord** on a Linux box (or
WSL), then build the **client** on each player PC.

## Prerequisites

Clone the repository, then fetch the vendored libraries (SQLite, cJSON,
miniz, civetweb, Nuklear, stb_image). Those files are not in git.

```
git clone <this-repo> Vapor
cd Vapor
```

### Linux server and Linux client (Debian / Ubuntu / WSL)

You need a C compiler, CMake, libcurl, libsodium, and (for the GUI)
SDL2 plus OpenGL headers.

```
sudo bash scripts/bootstrap-linux.sh
bash scripts/vendor-deps.sh
bash scripts/build-linux.sh
```

Binaries land in `build-linux/bin/`:

```
vapord  vapor-admin  vapor  vapor-gui  vapor-selftest
```

Useful bootstrap flags:

- `--server-only` — skip libcurl and SDL2 (headless host)
- `--no-gui` — skip SDL2 / OpenGL
- `--client-only` — skip libsodium (you will not build vapord)

`scripts/build-linux.sh` turns the GUI on automatically when `pkg-config`
finds SDL2 and OpenGL. Pass `-DVAPOR_BUILD_GUI=OFF` to force it off.

### Windows client

The server is not built on Windows (it needs libsodium). The client uses
WinHTTP from the Windows SDK.

You need Visual Studio 2022 (or Build Tools) with the C++ workload, plus
CMake (the copy bundled with VS is fine).

From PowerShell, in the repo root:

```
powershell -File scripts/vendor-deps.ps1
powershell -File scripts/build-windows.ps1
```

Binaries land in `build-windows\bin\`:

```
vapor.exe  vapor-gui.exe  vapor-selftest.exe
```

Pass `-NoGui` if you only want the CLI. Re-run `vendor-deps.ps1` if the
GUI is skipped because `third_party\sdl2` is missing.

A Windows machine can still host the **server** by building it inside
WSL with the Linux steps above. After Ubuntu is installed:

```
powershell -File scripts/start-wsl-server.ps1
```

That binds vapord to `0.0.0.0:8777` and keeps the database under
`~/vapor` on the Linux filesystem (SQLite on `/mnt/c` is unreliable).
The Windows client then uses `http://127.0.0.1:8777`. Keep the game
drop folder on a Windows path if you want Explorer to see the archives
(`/mnt/d/Games`). Packaged copies still land under `~/vapor/content` on
the Linux filesystem.

## Run the server

vapord listens on port **8777** by default. For a first run from the
build tree, keep everything under `./run` so nothing lands in
`/var/lib` or `/srv`.

On Linux:

```
mkdir -p run/content run/library
./build-linux/bin/vapord -H 0.0.0.0 -p 8777 \
    -r "$(pwd)/run/content" \
    -L "$(pwd)/run/library" \
    -d "$(pwd)/run/vapor.db"
```

- `-H 127.0.0.1` if only this machine will connect.
- `-H 0.0.0.0` if other PCs on the LAN should reach it.
- `--closed` later, once household accounts exist, to reject new
  registrations.

Leave that process running. Check it:

```
curl -s http://127.0.0.1:8777/api/v1/health
```

You should see `"status":"ok"` and `"has_users":false`. The first
account that registers becomes admin.

For a systemd install, TLS with Caddy, and a dedicated `vapor` user, see
[Docs/operations.md](Docs/operations.md). Example files are in
`deploy/`.

## Publish a game

Drop a folder into `library_root`. Each game is one unique directory.
Inside it, vapord accepts a zip (usual case), an ISO, or an unpacked
tree with an executable. ISOs are unpacked and the files are zipped for
download; a Wise SETUP.EXE on the disc is unpacked too. The original
disc image stays in the folder:

```
run/library/
  My Game/
    MyGame.zip          # or Game.iso, or Game.exe plus data/
    cover.png           # optional
```

The folder name is the catalog title (`My Game` → id `my-game`). vapord
scans at startup and every minute. `vapor-admin discover` scans once
immediately. Optional `vapor.json` in the folder overrides id, version,
and launch paths; see [Docs/manifest.md](Docs/manifest.md).

`vapor-admin add` still packages a folder by hand when you want full
control of id, version, and exec paths:

```
./build-linux/bin/vapor-admin \
    -r "$(pwd)/run/content" \
    -d "$(pwd)/run/vapor.db" \
    add /path/to/MyGame \
    --id my-game \
    --name "My Game" \
    --version 1.0.0 \
    --linux-exec MyGame \
    --windows-exec MyGame.exe \
    --cover /path/to/art.png
```

Rules that matter on the first try:

- `--id` is `[a-z0-9._-]`, starts with a letter or digit.
- At least one of `--linux-exec` or `--windows-exec` is required. The
  path is relative to the game folder.
- `--cover` is optional (`.png` or `.jpg`, 4 MiB or smaller).
- On Linux, zip does not keep the executable bit. Add
  `--exec-bit 'MyGame'` (repeatable, globs allowed) so the client
  `chmod +x`s after extract.
- `$INSTALL_DIR` in `--env` and `--arg` is expanded by the client at
  launch.

Confirm it is in the catalog:

```
./build-linux/bin/vapor-admin \
    -r "$(pwd)/run/content" \
    -d "$(pwd)/run/vapor.db" \
    list
```

`vapor-admin add --help` lists every ingest flag.

## Set up the client

The client stores its config and session token in:

| OS | Directory |
| --- | --- |
| Windows | `%LOCALAPPDATA%\Vapor` |
| Linux | `$XDG_DATA_HOME/vapor` (else `~/.local/share/vapor`) |

Set `VAPOR_DATA_DIR` to keep a portable or test profile off that path.
Set `VAPOR_PASSWORD` to skip the password prompt (used by the smoke
tests).

### Linux client, same machine as the server

```
bin=./build-linux/bin
$bin/vapor config server http://127.0.0.1:8777
$bin/vapor ping
$bin/vapor register          # first account is admin
$bin/vapor list
$bin/vapor install my-game
$bin/vapor launch my-game
```

Optional: `$bin/vapor config library /path/to/big/drive/Games` so
installs do not fill the home disk.

### Windows client, talking to a Linux (or WSL) server

Replace `SERVER` with the Linux host's LAN IP (or `127.0.0.1` if vapord
is in WSL with a forwarded port).

```
cd build-windows\bin
.\vapor.exe config server http://SERVER:8777
.\vapor.exe ping
.\vapor.exe register
.\vapor.exe list
.\vapor.exe install my-game
.\vapor.exe launch my-game
```

If `ping` fails from Windows to WSL: bind vapord to `0.0.0.0`, allow
port 8777 in the Windows firewall, and use the WSL address from
`wsl hostname -I` (not `localhost`, unless you have port forwarding).
`scripts/start-wsl-server.ps1` already binds `0.0.0.0` and WSL2 forwards
localhost, so `http://127.0.0.1:8777` is the usual URL.

### GUI

Same settings as the CLI. A stored token is restored only after vapord
confirms it with `GET /me`; if the server is unreachable the login screen
stays up.

```
# Linux
./build-linux/bin/vapor-gui

# Windows
.\build-windows\bin\vapor-gui.exe
```

Sign in, wait for the catalog, then **Install** / **Play**. **Update**
appears when the server has a newer version than the one on disk.

## Everyday commands

```
vapor whoami                 # which account is stored
vapor info my-game           # versions, install path, playtime
vapor installed              # local library
vapor verify my-game         # check an install against its manifest
vapor uninstall my-game
vapor logout
vapor config                 # print config path, server, library dir
```

Re-ingest a higher `--version` of the same `--id` to publish an update.
`vapor list` then shows `[update available]`.

## Check the build

```
# Linux
./build-linux/bin/vapor-selftest
bash scripts/smoke-test.sh

# Windows (client only; needs a running Linux server for the smoke test)
.\build-windows\bin\vapor-selftest.exe
powershell -File scripts/smoke-test-windows.ps1
```

## Going further

- [Docs/architecture.md](Docs/architecture.md) — layout and stack
- [Docs/api.md](Docs/api.md) — HTTP API
- [Docs/manifest.md](Docs/manifest.md) — package format and ingest flags
- [Docs/operations.md](Docs/operations.md) — systemd, Caddy / HTTPS,
  public-key pinning, closing registration

Vapor is licensed under the [GNU GPL v3](LICENSE).
