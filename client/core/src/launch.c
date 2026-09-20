/* Target selection, variable expansion, process spawn, playtime accounting.
 * The manifest is read from the install directory, so launching works offline. */

#include "vapor/client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "vapor/buf.h"
#include "vapor/util.h"

/* Expands $INSTALL_DIR and ${INSTALL_DIR}. Deliberately the only variable we
 * substitute: manifests should not be able to read arbitrary host environment
 * into a command line. Returns a malloc'd string, or NULL on allocation
 * failure. */
static char *
expand_install_dir(const char *in, const char *install_dir)
{
    static const char NAME[] = "INSTALL_DIR";
    vapor_buf         out;
    const char       *p = in;
    int               substituted = 0;

    vapor_buf_init(&out);
    if (vapor_buf_append(&out, "", 0) != 0) {
        return NULL;
    }

    while (*p) {
        if (*p != '$') {
            if (vapor_buf_append(&out, p, 1) != 0) {
                goto fail;
            }
            p++;
            continue;
        }

        if (p[1] == '{') {
            size_t n = strlen(NAME);
            if (strncmp(p + 2, NAME, n) == 0 && p[2 + n] == '}') {
                if (vapor_buf_appends(&out, install_dir) != 0) {
                    goto fail;
                }
                substituted = 1;
                p += 2 + n + 1;
                continue;
            }
        } else if (strncmp(p + 1, NAME, strlen(NAME)) == 0) {
            char after = p[1 + strlen(NAME)];
            /* Only a non-identifier character ends the name, so $INSTALL_DIRX
             * is left alone. */
            if (!((after >= 'A' && after <= 'Z') || (after >= 'a' && after <= 'z')
                  || (after >= '0' && after <= '9') || after == '_')) {
                if (vapor_buf_appends(&out, install_dir) != 0) {
                    goto fail;
                }
                substituted = 1;
                p += 1 + strlen(NAME);
                continue;
            }
        }

        if (vapor_buf_append(&out, p, 1) != 0) {
            goto fail;
        }
        p++;
    }
    /* Manifests are written once for every platform and so always spell paths
     * with '/'. A value anchored at $INSTALL_DIR is by definition a path, so
     * fix up its separators; anything else is left byte-for-byte, since a bare
     * '/' elsewhere may well be a URL or a switch rather than a path. */
    if (substituted && out.data) {
        vapor_plat_native_path(out.data);
    }
    return vapor_buf_release(&out);

fail:
    vapor_buf_free(&out);
    return NULL;
}

int
vapor_launch_game(vapor_client *vc, const char *game_id, int *out_exit)
{
    vapor_manifest      m;
    vapor_install       rec;
    const vapor_target *t;
    char                install_dir[VAPOR_PATH_MAX];
    char                exec_path[VAPOR_PATH_MAX];
    char                cwd_path[VAPOR_PATH_MAX];
    char              **argv = NULL;
    vapor_kv           *env = NULL;
    size_t              nenv = 0, i;
    int64_t             started, elapsed;
    int                 exit_code = -1, rc = -1;

    if (vapor_db_get_install(vc, game_id, &rec) != 0) {
        vapor_client_set_error(vc, "%s is not installed; run \"vapor install %s\"",
                               game_id, game_id);
        return -1;
    }
    snprintf(install_dir, sizeof(install_dir), "%s", rec.install_dir);

    if (vapor_read_local_manifest(vc, game_id, &m) != 0) {
        return -1;
    }

    t = vapor_manifest_pick_target(&m, vapor_host_platform(), vapor_host_arch());
    if (!t) {
        vapor_client_set_error(vc,
                               "%s has no launchable executable for %s/%s "
                               "(disc images cannot be launched yet)",
                               game_id, vapor_host_platform(), vapor_host_arch());
        vapor_manifest_free(&m);
        return -1;
    }
    if (t->runtime && *t->runtime && strcmp(t->runtime, "native") != 0) {
        /* The schema allows "proton"/"wine"; nothing implements them yet, and
         * silently running the binary natively would just fail confusingly. */
        vapor_client_set_error(vc,
                               "%s needs the \"%s\" runtime, which this build "
                               "does not support yet", game_id, t->runtime);
        vapor_manifest_free(&m);
        return -1;
    }

    if ((size_t)snprintf(exec_path, sizeof(exec_path), "%s/%s", install_dir,
                         t->exec)
        >= sizeof(exec_path)) {
        vapor_client_set_error(vc, "launch path is too long");
        vapor_manifest_free(&m);
        return -1;
    }
    vapor_plat_native_path(exec_path);
    if (!vapor_plat_exists(exec_path)) {
        vapor_client_set_error(vc,
                               "the launch target %s is missing; run "
                               "\"vapor install %s --force\" to repair",
                               t->exec, game_id);
        vapor_manifest_free(&m);
        return -1;
    }

    if (t->cwd && *t->cwd) {
        if ((size_t)snprintf(cwd_path, sizeof(cwd_path), "%s/%s", install_dir,
                             t->cwd)
            >= sizeof(cwd_path)) {
            vapor_client_set_error(vc, "working directory path is too long");
            vapor_manifest_free(&m);
            return -1;
        }
    } else {
        snprintf(cwd_path, sizeof(cwd_path), "%s", install_dir);
    }
    vapor_plat_native_path(cwd_path);

    /* argv[0] is the executable path; the rest come from the manifest. */
    argv = (char **)calloc(t->nargs + 2, sizeof(*argv));
    if (!argv) {
        vapor_client_set_error(vc, "out of memory");
        vapor_manifest_free(&m);
        return -1;
    }
    argv[0] = vapor_strdup(exec_path);
    if (!argv[0]) {
        goto cleanup;
    }
    for (i = 0; i < t->nargs; i++) {
        argv[i + 1] = expand_install_dir(t->args[i], install_dir);
        if (!argv[i + 1]) {
            vapor_client_set_error(vc, "out of memory");
            goto cleanup;
        }
    }
    argv[t->nargs + 1] = NULL;

    if (t->nenv > 0) {
        env = (vapor_kv *)calloc(t->nenv, sizeof(*env));
        if (!env) {
            vapor_client_set_error(vc, "out of memory");
            goto cleanup;
        }
        for (i = 0; i < t->nenv; i++) {
            env[i].key = vapor_strdup(t->env[i].key);
            env[i].value = expand_install_dir(t->env[i].value, install_dir);
            if (!env[i].key || !env[i].value) {
                vapor_client_set_error(vc, "out of memory");
                nenv = i + 1;
                goto cleanup;
            }
        }
        nenv = t->nenv;
    }

    printf("launching %s %s\n", rec.name[0] ? rec.name : game_id, rec.version);
    /* Flushed before spawning: the child writes straight to the fd, so leaving
     * our own output buffered would interleave it out of order. */
    fflush(stdout);

    started = vapor_now_unix();
    if (vapor_plat_run(exec_path, argv, cwd_path, env, nenv, &exit_code) != 0) {
        vapor_client_set_error(vc, "could not start %s", exec_path);
        goto cleanup;
    }
    elapsed = vapor_now_unix() - started;
    if (elapsed < 0) {
        elapsed = 0;
    }

    /* Recorded even on a non-zero exit: the session still happened. */
    vapor_db_add_playtime(vc, game_id, started, elapsed);

    {
        char pretty[64];
        vapor_format_duration(elapsed, pretty, sizeof(pretty));
        if (exit_code == 0) {
            printf("%s exited normally after %s\n",
                   rec.name[0] ? rec.name : game_id, pretty);
        } else {
            printf("%s exited with code %d after %s\n",
                   rec.name[0] ? rec.name : game_id, exit_code, pretty);
        }
    }
    if (out_exit) {
        *out_exit = exit_code;
    }
    rc = 0;

cleanup:
    if (argv) {
        for (i = 0; i < t->nargs + 2; i++) {
            free(argv[i]);
        }
        free(argv);
    }
    if (env) {
        for (i = 0; i < nenv; i++) {
            free(env[i].key);
            free(env[i].value);
        }
        free(env);
    }
    vapor_manifest_free(&m);
    return rc;
}
