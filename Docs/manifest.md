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
| `targets[].exec` | Relative to the install dir. No `..`. Optional when the payload is a disc image |
| `targets[].runtime` | `null` or `"native"` today; reserved for `"proton"` later |

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

vapord unpacks each `.iso` / `.img` it finds in a game folder (ISO 9660
and Joliet) into one tree and zips those files for the client. Several
disc images in the same folder are merged, later discs overlaying
same-named files. If a disc includes a
Wise `SETUP.EXE` (Half-Life GOTY and similar 1999–2003 Sierra installers),
the packed game files such as `WONAuth.dll` are unpacked into the same
tree and the installer executable is dropped from the zip. The original
disc images are left in place. After the client extracts the zip, it
inspects the install tree. A portable game binary (`HL.EXE` on a
Half-Life disc) is launched as-is. A Windows installer kit (`setup.exe`,
`.msi`, leftover `.iso`) is not treated as the game: the client runs
that installer locally, then tracks where it placed the files
(Uninstall registry / Program Files, or an in-place copy) and only then
shows Play. Files staged under `Setup/` are ignored when picking a
launch target. Installers, CD autorun stubs (`setup.exe`, `launch.exe`,
`autorun.exe`), and updaters (`upd.exe`, `*up.exe`, `*update.exe`) are
ignored when a real game binary is present. A leftover `SETUP.EXE` in an older zip
is unpacked the same way on install. If the image cannot be unpacked
(UDF-only DVDs), the ISO is wrapped as a file and the client tries the same
unpack on install. After a SETUP unpack, autorun files are dropped, and a
tiny root `*.DAT` is removed only when a larger file of the same name
exists in a subdirectory.

`iso` and `file` still skip extraction for older manifests: the client
copies the downloaded payload into the install directory.

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
  "cover": "art.png",
  "package": "HollowKnight.zip",
  "steam_appid": 367520
}
```

`vapor.json` may also set `steam_appid` so discovery does not have to
guess the Steam store listing when filling in art, description, and the
public review score.

## Auto-discovery

vapord scans `library_root` and publishes each subdirectory. See
[operations.md](operations.md#auto-discovery).

## Install

`vapor install <id>` (or the GUI Install button):

1. Fetch the manifest for `latest` or a given version.
2. Stream the archive to `<library>/.vapor/downloads/` with Range resume.
3. Verify SHA-256 against the manifest.
4. Extract a zip with miniz, then inspect the tree for a launchable
   executable or a disc image. Copy an `iso`/`file` payload as-is.
5. Apply `exec_bits` on Linux when a native target was found.
6. Record the install in the local SQLite. If the tree still has a
   Windows installer (`setup.exe`, `.msi`, leftover `.iso`) and no
   playable game binary, the client classifies the wrapper (Inno/GOG,
   NSIS, MSI, InstallShield, or a generic exe), silent-installs into
   `<library>/<id>/installed` when that family supports it (Inno, NSIS,
   MSI). InstallShield disc kits skip silent mode: `/s /sms /qn` hangs
   waiting on msiexec with no window, so the Setup wizard is shown
   instead. The client waits for helper processes (`msiexec`, IDriver,
   a second `setup.exe`, …) to exit. Setup.exe's exit code is not enough: many bootstrappers
   return 0 while the real copy is still running. Vapor then tracks the
   destination the installer actually created (the silent folder, the
   Uninstall `InstallLocation`, or Program Files) and treats the install
   as complete only when that tree has a game executable plus data and
   has stopped growing. Play is offered only after that. If unattended
   mode produces no complete tree, the wizard is shown once as a
   fallback. `vapor setup <id>` retries the same flow.

Verify-before-extract means a truncated download cannot produce a
half-installed game. `--verify-only` stops after the hash.
`--force` reinstalls even when the version already matches. The GUI and
CLI progress bar tracks this whole pipeline (download, hash, extract,
disc unpack, Windows setup), not only the HTTP transfer. Cancel is
honoured between those steps.

## Launch

`vapor launch <id>` picks the first target whose `platform` and `arch`
match the host, then any target for that platform. `$INSTALL_DIR` is
expanded in `args` and `env`, then converted to native separators so
Windows does not mix `/` and `\`.

The process is spawned with `CreateProcessW` or `fork` + `execvp`. Start
and exit time are recorded as playtime.

If the launch target is a SafeDisc (SECDRV) wrapper, Vapor does not run
it: that DRM cannot load on Windows 10+, and the wrapper's
"administrator privileges" dialog is not a real login. Play uses an
unprotected exe in the install folder when one exists. For a Doom 3
tree (`base/pak000.pk4` + `base/game00.pk4`) it launches dhewm3 against
those paks, downloading the official Windows build into
`%LOCALAPPDATA%\Vapor\runtimes\dhewm3` if needed. Other SafeDisc titles
need the publisher's patch or a source-port exe in the install folder.
SECDRV.SYS is never enabled. The GUI install job reports extract, then
the Windows installer; it does not stay on Verify after the download
finishes.

Launch and verify read the *local* copy of the manifest, so an already
installed game still runs if the server is unreachable.

## Updates

The catalog marks `update_available` when the server's latest version
compares greater than the installed version. The CLI prints
`[update available]`; the GUI turns Verify into Update. Install of the
new version is a normal `--force` install over the same directory.
