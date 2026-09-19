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
| `package.sha256` | 64 lowercase hex characters |
| `targets[].exec` | Relative to the install dir. No `..` |
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
so both can coexist.

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

## Install

`vapor install <id>` (or the GUI Install button):

1. Fetch the manifest for `latest` or a given version.
2. Stream the archive to `<library>/.vapor/downloads/` with Range resume.
3. Verify SHA-256 against the manifest.
4. Extract with miniz into `<library>/<id>/`.
5. Apply `exec_bits` on Linux.
6. Record the install in the local SQLite.

Verify-before-extract means a truncated download cannot produce a
half-installed game. `--verify-only` stops after the hash.
`--force` reinstalls even when the version already matches.

## Launch

`vapor launch <id>` picks the first target whose `platform` and `arch`
match the host, then any target for that platform. `$INSTALL_DIR` is
expanded in `args` and `env`, then converted to native separators so
Windows does not mix `/` and `\`.

The process is spawned with `CreateProcessW` or `fork` + `execvp`. Start
and exit time are recorded as playtime.

Launch and verify read the *local* copy of the manifest, so an already
installed game still runs if the server is unreachable.

## Updates

The catalog marks `update_available` when the server's latest version
compares greater than the installed version. The CLI prints
`[update available]`; the GUI turns Verify into Update. Install of the
new version is a normal `--force` install over the same directory.
