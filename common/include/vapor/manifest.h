#ifndef VAPOR_MANIFEST_H
#define VAPOR_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "vapor/sha256.h"

#define VAPOR_MANIFEST_SCHEMA 1

typedef struct {
    char *key;
    char *value;
} vapor_kv;

/* One launch configuration. `runtime` is NULL or "native" today; it is the
 * hook for "proton"/"wine" later, where a windows `exec` runs on a linux host
 * without any other field changing meaning. */
typedef struct {
    char     *platform;    /* "windows" | "linux" */
    char     *arch;        /* "x86_64" or "x86"; NULL means any arch */
    char     *runtime;
    char     *exec;        /* relative to install dir */
    char    **args;
    size_t    nargs;
    char     *cwd;         /* relative to install dir; NULL means install dir */
    char    **exec_bits;   /* glob patterns to chmod +x after extraction */
    size_t    nexec_bits;
    vapor_kv *env;
    size_t    nenv;
} vapor_target;

typedef struct {
    char    *file;         /* name within the version directory */
    char    *format;       /* "zip", "iso", or "file" */
    uint64_t size;
    char     sha256[VAPOR_SHA256_HEX_LEN + 1];
    char    *strip_prefix; /* leading path component to drop, or NULL */
} vapor_package;

/* Optional per-game install recipe. Every path is relative to the extracted
 * tree (or, after setup, the product directory for `uninstall`). NULL fields
 * mean the client should keep using filename heuristics. */
typedef struct {
    char *setup;
    char *launch;
    char *uninstall;
} vapor_install_profile;

typedef struct {
    int           schema;
    char         *id;
    char         *name;
    char         *version;
    char         *developer;
    char         *description;
    /* Optional image filename sitting beside the package in the version
     * directory, served separately so the library view can show art without
     * downloading the game. NULL when the game has none. */
    char         *cover;
    vapor_package package;
    vapor_install_profile install;
    /* portable, setup, unpack_disc, or keep_disc. NULL keeps the heuristics. */
    char         *install_mode;
    /* Save roots copied from vapor.json. Absolute, $INSTALL_DIR, %ENV%, or
     * relative to the install directory. NULL/0 means this title syncs nothing. */
    char        **saves;
    size_t        nsaves;
    vapor_target *targets;
    size_t        ntargets;
} vapor_manifest;

void vapor_manifest_init(vapor_manifest *m);
void vapor_manifest_free(vapor_manifest *m);

/* 0 on success; on failure fills `err` with a reason the CLI can print. */
int vapor_manifest_parse(const char *json, size_t len, vapor_manifest *out,
                         char *err, size_t errsz);

/* Pretty-printed JSON; caller frees. NULL on allocation failure. */
char *vapor_manifest_serialize(const vapor_manifest *m);

/* Exact platform+arch match first. A 64-bit Windows host may then run an
 * x86 target (WOW64). A target with no arch is next. A 32-bit Windows host
 * may then try a target labeled x86_64, so manifests published before arch
 * detection still launch. NULL if nothing is runnable for this host. */
const vapor_target *vapor_manifest_pick_target(const vapor_manifest *m,
                                               const char *platform,
                                               const char *arch);

const char *vapor_host_platform(void);
const char *vapor_host_arch(void);

#endif /* VAPOR_MANIFEST_H */
