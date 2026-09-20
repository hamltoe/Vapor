# MS-DOS games via DOSBox

Future work. Vapor already installs folder/zip dumps and has a reserved
`targets[].runtime` field that launch currently rejects. DOS support is
mostly **detect the game** and **wrap DOSBox** — not a new package format.

64-bit Windows cannot run 16-bit DOS `.EXE`/`.COM` natively, so an
emulator is required, not optional.

## Recommendation

- **Runtime:** [DOSBox Staging](https://www.dosbox-staging.org/) as the
  preferred binary. Better defaults for games than upstream DOSBox 0.74;
  still understands classic `-conf` / `[autoexec]`. Accept any compatible
  `dosbox.exe` via config.
- **Do not bundle** the emulator in v1. Resolve `dosbox_path` from client
  config, then `PATH`, then a couple of common Windows install locations.
  Tell the user to install Staging if none is found.
- **Manifest model (no schema bump):** host platform + runtime, same
  shape as the planned Wine/Proton hook:

```json
{
  "platform": "windows",
  "arch": "x86_64",
  "runtime": "dosbox",
  "exec": "DOOM.EXE",
  "cwd": "."
}
```

Keep `exec` as the **DOS binary** (relative to the install dir). Do not
invent `platform: "dos"` yet — `vapor_manifest_pick_target()` in
[`common/src/manifest.c`](../common/src/manifest.c) and install
validation already key off the host OS, so a windows target launches on
Windows today.

Linux later is a second target with `"platform": "linux"` and the same
`runtime` / `exec`.

## v1 scope

**In:** unzipped DOS folders and zips (the common GOG/archive dump).
Auto-discover when possible; override with `vapor.json` / `vapor-admin`.
Play from the existing Install / Play buttons.

**Out for v1:** bundling DOSBox, Linux launch, CD/floppy `imgmount`,
Windows 3.1/9x (that is DOSBox-X), and a per-game settings UI.

## What to build

```
Discover or vapor-admin
        │
        ▼
manifest runtime: dosbox
        │
        ▼
existing zip install
        │
        ▼
launch.c writes .vapor/dosbox.conf
        │
        ▼
DOSBox Staging ──► GAME.EXE
```

### 1. Tell DOS games from Windows games

Add a small MZ/PE check (shared, e.g. in common): `MZ` header and **no**
`PE\0\0` at the `e_lfanew` offset → DOS. `.COM` is always DOS. `.BAT`
only if the sidecar/admin names it (too many `INSTALL.BAT`s).

Reuse the existing junk heuristics (`setup.exe`, `upd.exe`, `*up.exe`,
`*update.exe`, …) and extend them with `install.com` / `setup.com`.

Without this, discovery will keep treating DOS `.EXE` as native Windows
and Play will fail on 64-bit.

### 2. Catalog / ingest

- [`server/src/discover.c`](../server/src/discover.c) and the mirrored
  walk in [`client/core/src/install.c`](../client/core/src/install.c): if
  the best candidate is DOS, emit a windows target with
  `runtime: "dosbox"` instead of a native `.exe`.
- Sidecar `vapor.json` (see [manifest.md](manifest.md)): add `dos_exec`.
- [`vapor-admin add`](../server/tools/vapor_admin.c): add `--dos-exec`.

The zip / SHA-256 / catalog API stay as they are.

### 3. Launch on Windows

Replace the hard reject in
[`client/core/src/launch.c`](../client/core/src/launch.c) (the
`"does not support yet"` branch) for `runtime == "dosbox"`:

1. Resolve the DOSBox binary (`dosbox_path`, `PATH`, well-known folders).
2. Write `<install>/.vapor/dosbox.conf` with a generated `[autoexec]`:

```
mount C "<install_dir>"
C:
cd <cwd>
GAME.EXE
exit
```

3. Spawn `dosbox -conf ... -noconsole -exit` via the existing
   `vapor_plat_run()`.

A generated conf is more portable across DOSBox / Staging / DOSBox-X
than `-c` chains or Staging-only `--working-dir`.

Add `dosbox_path` to `vapor_client_config` in
[`client/core/include/vapor/client.h`](../client/core/include/vapor/client.h)
and the key=value config in
[`client/core/src/config.c`](../client/core/src/config.c). No GUI panel
required; a clear “DOSBox not found” error is enough.

### 4. Docs and a small test

- Document `runtime: "dosbox"`, `dos_exec`, and `--dos-exec` in
  [manifest.md](manifest.md).
- Selftest the MZ-vs-PE helper and conf generation (no need to spawn
  DOSBox in CI).

## How you use it

Drop a DOS game folder in `library_root` (or zip it). If auto-detect
misses the right binary, add:

```json
{ "id": "doom", "name": "Doom", "dos_exec": "DOOM.EXE" }
```

Install Staging once on the Windows client. Play works offline from the
local manifest, same as native games.

## Later (not v1)

- **Linux client:** same wrapper; look up `dosbox` on `PATH`. Discover
  emits a linux+dosbox target. Conf generation already uses
  `$INSTALL_DIR`.
- **CD / floppy:** keep the ISO in the tree and `imgmount` it; today’s
  ISO unpack is wrong for games that expect MSCDEX.
- **Ship Staging** with the Windows client if you want zero extra
  install (GPL packaging work).
- **GOG-style trees** that already include a `dosbox*.conf`: prefer that
  file over a generated one.
- **Per-game cycles / machine / Sound Blaster:** optional extra section
  in `vapor.json`, or a future Configure UI.
- **Wine/Proton** can reuse the same launch dispatch you add for
  `dosbox`.
