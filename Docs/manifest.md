# Manifest format

One `manifest.json` per game version. Schema version is `1`
(`VAPOR_MANIFEST_SCHEMA`). The client and `vapor-admin` share the same
parser in `common/src/manifest.c`.

```json
{
  "schema": 1,
  "id": "hollow-vale",
  "name": "Hollow Vale",
  "version": "1.0.3",
  "developer": "Someone",
  "description": "Short blurb for the library view.",
  "cover": "cover.png",
  "package": {
    "file": "package.zip",
    "format": "zip",
    "size": 524288000,
    "sha256": "9f2c…64 hex characters…",
    "strip_prefix": "HollowVale/"
  },
  "targets": [
    {
      "platform": "windows",
      "arch": "x86_64",
      "exec": "bin/HollowVale.exe",
      "args": [],
      "cwd": "bin"
    },
    {
      "platform": "linux",
      "arch": "x86_64",
      "exec": "bin/hollowvale",
      "args": [],
      "cwd": "bin",
      "exec_bits": ["bin/hollowvale", "lib/*.so*"],
      "env": { "LD_LIBRARY_PATH": "$INSTALL_DIR/lib" }
    }
  ]
}
```

## Field rules

| Field | Rule |
| --- | --- |
| `id` | 1–32 chars of `[a-z0-9._-]`, starts alphanumeric, no `..` |
| `version` | Compared naturally: `1.10` > `1.9` |
| `cover` | Optional. Bare filename only (`cover.png` or `cover.jpg`) |
| `package.file` | Bare filename. No `/` or `\` |
| `package.format` | `zip` (extract, then inspect), `iso` or `file` (copy as-is) |
| `package.sha256` | 64 lowercase hex characters |
| `targets[].arch` | `x86_64` or `x86`. Discovery reads the Windows PE machine field (`0x14c` is `x86`, `0x8664` is `x86_64`). Linux targets stay `x86_64` |
| `targets[].exec` | Relative to the install dir. No `..`. Optional when the payload is a disc image |
| `targets[].runtime` | `null` or `"native"`; `"dosbox"` launches DOSBox Staging. Any other string is stored and Play names it until that runtime exists |
| `install_mode` | Optional. `portable`, `setup`, `unpack_disc`, or `keep_disc`. Omitted means the usual heuristics |
| `saves` | Optional array of save roots. Each entry is a folder or file: absolute (any drive), `%USERPROFILE%` / `%APPDATA%` / `%LOCALAPPDATA%` / `$HOME`, `$INSTALL_DIR\...`, or relative to the install directory. A bare drive root and `..` are rejected |

A hostile id, version, cover, or package name cannot escape the content
root: `vapord_content_path` rejects any segment that is not a valid id,
version, or bare filename.

## Why zip

Zip does not reliably carry the Unix executable bit. Linux targets
declare `exec_bits` glob patterns; the installer `chmod +x`s matches
after extraction. That keeps the prototype at zero extra archive
dependencies.

If a game later needs symlinks or fine-grained permissions, swap miniz
for libarchive and package `.tar.zst`. `package.format` already exists
so zip, iso, and other payloads can coexist.

vapord unpacks each `.iso`, `.img`, and BIN/CUE `.bin` it finds in a game
folder (ISO 9660 and Joliet, including raw 2352-byte sectors) into one
tree and zips those files for the client. Several disc images in the same
folder are merged, later discs overlaying same-named files. A `.bin` that
is not a disc image (a Linux binary, or other data) is left as a normal
file. If a disc includes a
Wise `SETUP.EXE` (Half-Life GOTY and similar 1999–2003 Sierra installers),
the packed game files such as `WONAuth.dll` are unpacked into the same
tree and the installer executable is dropped from the zip. The original
disc images are left in place. After the client extracts the zip, it
inspects the install tree. A portable game binary (`HL.EXE` on a
Half-Life disc) is launched as-is. A Windows installer kit (`setup.exe`,
`.msi`) is not treated as the game: the client runs
that installer locally, then tracks where it placed the files
(Uninstall registry / Program Files, or an in-place copy) and only then
shows Play. Files staged under `Setup/` are ignored when picking a
launch target. Installers, CD autorun stubs (`setup.exe`, `launch.exe`,
`autorun.exe`), and updaters (`upd.exe`, `*up.exe`, `*update.exe`) are
ignored when a real game binary is present. A leftover `SETUP.EXE` in an older zip
is unpacked the same way on install. If the image cannot be unpacked
(UDF-only DVDs), the ISO is wrapped as a file and the client extracts it
into `<library>/.vapor/mnt/<id>` on install. After a SETUP unpack, autorun files are dropped, and a
tiny root `*.DAT` is removed only when a larger file of the same name
exists in a subdirectory.

`file` still skips extraction for older manifests: the client copies
the downloaded payload into the install directory. A legacy `iso`
package is extracted into `<library>/.vapor/mnt/<id>` rather than copied
in as the installed game.

`strip_prefix` drops a leading folder inside the zip so a publisher can
ship `HollowVale/bin/game` and still install as `<library>/hollow-vale/bin/game`.

Extraction refuses zip-slip: any entry that would land outside the
install directory is rejected.

## Ingest

On the server:

```
vapor-admin add ./HollowVale \
    --id hollow-vale \
    --name "Hollow Vale" \
    --version 1.0.3 \
    --developer Someone \
    --description "Short blurb." \
    --cover ./art/cover.png \
    --linux-exec bin/hollowvale \
    --windows-exec bin/HollowVale.exe \
    --exec-bit 'bin/hollowvale' \
    --exec-bit 'lib/*.so*' \
    --env 'LD_LIBRARY_PATH=$INSTALL_DIR/lib' \
    --cwd bin
```

`--cover` accepts `.png`, `.jpg`, or `.jpeg`, at most 4 MiB. The file is
copied next to the package as `cover.png` or `cover.jpg`; the original
path does not appear in the manifest.

`--developer` and `--description` are sticky: publishing a new version
without repeating them leaves the existing catalog text alone.

The generated manifest is parsed before it is written. If the client
would reject it, ingest fails instead of shipping something
uninstallable.

An optional `vapor.json` in a discovered game folder overrides detection:

```json
{
  "id": "hollow-knight",
  "name": "Hollow Knight",
  "version": "1.5.78",
  "developer": "Team Cherry",
  "windows_exec": "hollow_knight.exe",
  "linux_exec": "hollow_knight.x86_64",
  "setup_exec": "setup.exe",
  "uninstall_exec": "unins000.exe",
  "cover": "art.png",
  "package": "HollowKnight.zip",
  "steam_appid": 367520,
  "runtime": "native",
  "install_mode": "portable",
  "saves": ["%USERPROFILE%\\Saved Games\\Hollow Knight"]
}
```

`vapor.json` may also set `steam_appid` so discovery does not have to
guess the Steam store listing when filling in art, description, and the
public review score.

`runtime` is `native` (the default) or `dosbox`. Any other token is stored so a later emulator can use the same manifest; Play reports that this build does not run it yet. `dos_exec` is the DOS binary, relative to the install directory, and forces `runtime` `dosbox` unless `runtime` is explicitly `native`.

`install_mode` tells discovery and the client to stop guessing:

- `portable` — ship the files as they are and do not run a Windows setup, even if `setup.exe` is present
- `setup` — run `setup_exec`
- `unpack_disc` — today's ISO 9660 / Wise unpack
- `keep_disc` — leave disc images in the tree. Play of a DOS title then fails with an explicit message until CD mount exists, instead of unpacking a disc the game cannot use

`saves` is the list of files that sync to the signed-in account. See [api.md](api.md). The client also syncs any `save`, `saves`, or `savegames` directory under the install, which is where Half-Life GOTY writes `valve\save`. A path that is not on this machine stays in the account archive, so Windows XP and another PC can share one list. One sync is capped at 256 MiB and 10,000 files.

When `runtime`, `dos_exec`, and `install_mode` are all omitted, discovery keeps the previous heuristics. A DOS `.COM`, or an `.EXE` that is MZ without a PE header, is published as `runtime: "dosbox"` on its own. `setup.com` and `install.com` are ignored, same as `setup.exe`.

`setup_exec` and `uninstall_exec` are optional relative paths. Discovery
copies them into the manifest `install` object (`setup`, `uninstall`).
`windows_exec` / `linux_exec` stay the launch targets. The client uses
`install.setup` and `install.launch` when those files exist, and otherwise
picks the installer and the game executable by filename. `install.uninstall`
is the uninstaller inside the product tree, used by Remove when that file
is present.

```json
"install": {
  "setup": "Setup/setup.exe",
  "launch": "game.exe",
  "uninstall": "unins000.exe"
}
```

## Auto-discovery

vapord scans `library_root` and publishes each subdirectory. See
[operations.md](operations.md#auto-discovery).

## Install

`vapor install <id>` (or the GUI Install button):

1. Fetch the manifest for `latest` or a given version.
2. Stream the archive to `<library>/.vapor/downloads/` with Range resume.
3. Verify SHA-256 against the manifest.
4. Extract a zip with miniz, then inspect the tree. A leftover `.iso`,
   `.img`, or BIN/CUE `.bin`, or a legacy `package.format` of `iso`, is
   extracted into
   `<library>/.vapor/mnt/<id>` for reading. It is not opened with the
   shell. A portable tree is then copied into `<library>/<id>` and the
   mount is removed. An installer kit stays on the mount until setup
   finishes. A failed extract leaves the image in that mount and stops.
5. Apply `exec_bits` on Linux when a native target was found.
6. Record the install in the local SQLite, including `setup_state`
   (`pending`, `running`, `completed`, `cancelled`, or `failed`). The
   chosen setup, launch, and uninstall paths are stored in
   `<payload>/.vapor/install.json`. If the tree still has a Windows
   installer (`setup.exe`, `.msi`) and no playable game binary — or the
   manifest names `install.setup` — the client classifies the wrapper
   (Inno/GOG, NSIS, MSI, InstallShield, or a generic exe), silent-installs
   into `<payload>/installed` when that family supports it (Inno, NSIS,
   MSI). InstallShield disc kits skip silent mode: `/s /sms /qn` hangs
   waiting on msiexec with no window, so the Setup wizard is shown
   instead. The client waits for helper processes (`msiexec`, IDriver,
   a second `setup.exe`, …) to exit, and Cancel during that wait stops
   the process tree. Setup.exe's exit code is not enough: many bootstrappers
   return 0 while the real copy is still running. Vapor then tracks the
   destination the installer actually created (the silent folder, the
   Uninstall `InstallLocation`, or Program Files) and treats the install
   as complete only when that tree has a game executable plus data and
   has stopped growing. A finished tree wins over the exit code. Play is
   offered only after `setup_state` is `completed`. If unattended mode
   produces no complete tree, the wizard is shown once as a fallback.
   Closing the wizard or cancelling leaves `setup_state` as `cancelled`
   and keeps the mount so `vapor setup <id>` can retry. A successful
   setup deletes the mount.

Verify-before-extract means a truncated download cannot produce a
half-installed game. `--verify-only` stops after the hash.
`--force` reinstalls even when the version already matches. The GUI and
CLI progress bar tracks this whole pipeline (download, hash, extract,
disc unpack, Windows setup), not only the HTTP transfer. Cancel is
honoured between those steps and while the installer process is running.

The local database records an **install kind** so Remove is the inverse
of Install:

- `portable` — extracted into `<library>/<id>`. Remove deletes that tree.
- `os_product` — a Windows installer ran. The uninstall command and
  product folder are stored in SQLite and `<payload>/.vapor/install.json`
  at setup time. Remove prefers `install.uninstall` (or the recorded
  profile path) when that file exists, then the recorded command
  (QuietUninstallString / UninstallString), then rediscovery by
  InstallLocation. If the only remaining installer is an MSI, Remove
  runs `msiexec /x`. Inno and NSIS are removed through `unins*.exe` /
  `uninstall.exe`, not by launching `setup.exe` again. InstallShield
  stays on the Add/Remove Programs command. Cancelling the uninstaller
  leaves the game installed. After the product is gone, Remove deletes
  the recorded folders and `<library>/.vapor/mnt/<id>`.
- `runtime` — same on-disk layout as portable, but
  `targets[].runtime` is not `native` (reserved for DOSBox, Wine /
  Proton). Remove deletes the game tree only; shared
  `{data_dir}/runtimes/*` (dhewm3 today) stays.
- `external` — a program that was already on this computer (another
  store, a manual copy). The client stores the executable path and a
  working directory. Play starts that program. A copy that lives in a
  Steam library is started through the Steam client, because launching
  the executable directly is rejected by Steam. Remove only forgets the
  shortcut; it does not delete files or run an uninstaller. Add one
  from the library (**Add game**) or with `vapor add PATH [--name TITLE]`.

ARP rediscovery by DisplayName slug is a fallback for installs that
predate the recipe, not the primary path.

## Launch

`vapor launch <id>` picks a target in this order: exact `platform` and
`arch`; on 64-bit Windows, an `x86` target (it runs under WOW64); a target
for that platform with no `arch`; on 32-bit Windows, a target still labeled
`x86_64` from a scan that predates PE detection. `$INSTALL_DIR` is
expanded in `args` and `env`, then converted to native separators so
Windows does not mix `/` and `\`. A file that is actually 64-bit does not
start on 32-bit Windows.

The process is spawned with `CreateProcessW` or `fork` + `execvp`. Start
and exit time are recorded as playtime.

If the launch target is a SafeDisc (SECDRV) wrapper, Vapor does not run
it: that DRM cannot load on Windows 10+, and the wrapper's
"administrator privileges" dialog is not a real login. Play uses an
unprotected exe in the install folder when one exists. For a Doom 3
tree (`base/pak000.pk4` + `base/game00.pk4`) it launches dhewm3 against
those paks, downloading the official Windows build into
`%LOCALAPPDATA%\Vapor\runtimes\dhewm3` if needed. The Windows XP client
cannot download that archive; put `dhewm3.exe` in the install folder or
in that runtimes directory. Other SafeDisc titles
need the publisher's patch or a source-port exe in the install folder.
SECDRV.SYS is never enabled. The GUI install job reports extract, then
the Windows installer; it does not stay on Verify after the download
finishes.

`targets[].runtime` is `native` or omitted today. A non-native value
(`dosbox`, later `wine` / `proton`) is stored as install kind `runtime`
and rejected at Play until that wrapper is implemented. Emulator titles
still install as a zip extract; they are not Windows installer kits.

Launch and verify read the *local* copy of the manifest, so an already
installed game still runs if the server is unreachable.

## Updates

The catalog marks `update_available` when the server's latest version
compares greater than the installed version. The CLI prints
`[update available]`; the GUI turns Verify into Update. Install of the
new version is a normal `--force` install over the same directory.
