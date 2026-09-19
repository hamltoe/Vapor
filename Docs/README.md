# Vapor

Vapor is a self-hosted game library: a Linux server that stores packaged
installs, and a Windows or Linux client that signs in, downloads, verifies,
installs, and launches them. Think of a small Steam or GOG that you run on
your own machine and point at a disk or NAS.

**To build from source and get a first library running, start at the
[root README](../README.md).** This folder is the project plan as
implemented: contracts and deployment detail. The source of truth for
behaviour is the C code; these pages describe the contracts that code
honours.

| Document | What it covers |
| --- | --- |
| [architecture.md](architecture.md) | Binaries, stack, data flow, repository layout |
| [api.md](api.md) | HTTP API, auth tokens, error bodies |
| [manifest.md](manifest.md) | Game package schema, ingest, install and launch |
| [operations.md](operations.md) | Build, run, TLS, systemd, cover art, updates |

## What you get

Four programs, one shared client library, all C11, one CMake project.

- **vapord** — Linux HTTP server. Catalog, accounts, ranged downloads.
- **vapor-admin** — Server-side ingest. Zips a folder, writes a manifest,
  registers the game.
- **vapor** — CLI on Windows and Linux. Register, list, install, launch.
- **vapor-gui** — The same `libvapor` calls behind a Nuklear + SDL2 window.

The client never mounts the server disk. Everything goes over one HTTP API,
so a NAS versus a local folder is just `content_root` on the server.

## Milestone status

| Milestone | Status |
| --- | --- |
| M0 Infrastructure — CMake, vendored deps, `/health` | Done |
| M1 Auth — Argon2id, bearer tokens, register/login | Done |
| M2 Catalog — manifests, `vapor-admin`, list/info | Done |
| M3 Install and launch — resume, SHA-256, extract, play | Done |
| M4 GUI — login, library grid, progress, play | Done |
| M5 Hardening — TLS proxy, systemd, updates, cover art | Done |
