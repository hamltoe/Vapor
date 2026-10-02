# MS-DOS games via DOSBox

Windows Play wraps DOSBox Staging when a target's `runtime` is
`dosbox`. That comes from `vapor.json` (`runtime`, `dos_exec`) or from
discovery noticing an MZ executable with no PE header, or a `.COM` that
is not `setup.com` / `install.com`. Linux launch and CD `imgmount` are
still later. The per-game file is the same sidecar as install mode and
account saves; see [manifest.md](manifest.md).

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
Auto-discover when possible; override with `vapor.json`. Play from the
existing Install / Play buttons on Windows. The client looks up
`dosbox_path`, then `PATH`, then common Staging install folders.
`vapor config dosbox PATH` sets the binary. A generated
`<install>/.vapor/dosbox.conf` mounts the install directory and runs
the DOS executable.

**Out for v1:** bundling DOSBox, Linux launch, CD/floppy `imgmount`
(`install_mode: "keep_disc"` leaves the images and Play says CD mount
is not available yet), Windows 3.1/9x (that is DOSBox-X), and a
per-game settings UI. Save paths are the manifest `saves` list, not a
separate DOSBox capture directory.

## How Windows launch works

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

A shared MZ/PE check in common treats an `MZ` header with **no**
`PE\0\0` at `e_lfanew` as DOS. `.COM` is always DOS. `.BAT` is DOS only
when `vapor.json` names it (too many `INSTALL.BAT`s).

Reuse the existing junk heuristics (`setup.exe`, `upd.exe`, `*up.exe`,
`*update.exe`, …) and extend them with `install.com` / `setup.com`.

Without this, discovery will keep treating DOS `.EXE` as native Windows
and Play will fail on 64-bit.

### 2. Catalog / ingest

- [`server/src/discover.c`](../server/src/discover.c) and the mirrored
  walk in [`client/core/src/install.c`](../client/core/src/install.c): if
  the best candidate is DOS, emit a windows target with
  `runtime: "dosbox"` instead of a native `.exe`.
- Sidecar `vapor.json` (see [manifest.md](manifest.md)): `runtime` and
  `dos_exec`. An explicit `runtime` other than `native` is stored as-is.
- `vapor-admin add` still has no `--dos-exec`. Put `dos_exec` in
  `vapor.json` instead.

The zip / SHA-256 / catalog API stay as they are.

### 3. Launch on Windows

[`client/core/src/launch.c`](../client/core/src/launch.c) launches
`runtime == "dosbox"` on Windows. Any other runtime name is still
rejected with that name in the error.

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

`dosbox_path` lives on `vapor_client_config` and in the key=value
config. There is no GUI panel; Play says to install DOSBox Staging when
none of the lookups find a binary.

### 4. Test

Selftest covers the MZ-vs-PE helper and save-path rules. It does not
spawn DOSBox.

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
