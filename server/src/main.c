#include "vapord.h"

#include "civetweb.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vapor/util.h"

static volatile sig_atomic_t g_stop;

static void
on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int
handle_health(vapord *app, struct mg_connection *c)
{
    cJSON  *out = cJSON_CreateObject();
    int64_t users = 0;

    if (!out) {
        return vapord_send_errorf(c, 500, VAPOR_ERR_INTERNAL, "out of memory");
    }
    cJSON_AddStringToObject(out, "status", "ok");
    cJSON_AddStringToObject(out, "service", "vapord");
    cJSON_AddStringToObject(out, "version", VAPOR_VERSION_STRING);
    cJSON_AddStringToObject(out, "api", VAPOR_API_PREFIX);
    /* Lets a fresh client tell "server has no accounts yet" from "registration
     * is closed" before it prompts for a password. */
    cJSON_AddBoolToObject(out, "registration_open", app->cfg.enable_registration);
    if (vapord_user_count(app->db, &users) == 0) {
        cJSON_AddBoolToObject(out, "has_users", users > 0);
    }
    return vapord_send_json(c, 200, out);
}

/* Single entry point for everything under /api/v1. Doing the dispatch here
 * rather than with civetweb URI patterns keeps the whole routing table visible
 * in one place and avoids depending on civetweb's pattern precedence. */
static int
dispatch(struct mg_connection *c, void *cbdata)
{
    vapord                       *app = (vapord *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(c);
    const char                   *uri = ri->local_uri;
    const char                   *method = ri->request_method;
    const char                   *tail;

    if (!uri || !method) {
        return vapord_send_errorf(c, 400, VAPOR_ERR_BAD_REQUEST, "bad request");
    }
    if (!vapor_str_has_prefix(uri, VAPOR_API_PREFIX)) {
        return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
    }
    tail = uri + strlen(VAPOR_API_PREFIX);

    if (strcmp(tail, "/health") == 0) {
        if (strcmp(method, "GET") != 0) {
            return vapord_send_errorf(c, 405, VAPOR_ERR_BAD_REQUEST, "use GET");
        }
        return handle_health(app, c);
    }
    if (vapor_str_has_prefix(tail, "/auth/") || strcmp(tail, "/me") == 0) {
        return vapord_route_auth(app, c, method, tail);
    }
    if (vapor_str_has_prefix(tail, "/games")) {
        return vapord_route_catalog(app, c, method, tail);
    }
    if (vapor_str_has_prefix(tail, "/download/")) {
        return vapord_route_download(app, c, method, tail);
    }
    return vapord_send_errorf(c, 404, VAPOR_ERR_NOT_FOUND, "no such endpoint");
}

static void
usage(const char *argv0)
{
    printf("vapord %s - Vapor game content server\n\n", VAPOR_VERSION_STRING);
    printf("usage: %s [-c CONFIG] [-H ADDR] [-p PORT] [-r CONTENT_ROOT] "
           "[-L LIBRARY_ROOT] [-d DB_PATH]\n\n",
           argv0);
    printf("  -c CONFIG        read settings from CONFIG (key = value)\n");
    printf("  -H ADDR          bind address (default 0.0.0.0)\n");
    printf("  -p PORT          listen port (default %d)\n", VAPOR_DEFAULT_PORT);
    printf("  -r CONTENT_ROOT  packaged <game>/<version>/ files\n");
    printf("  -L LIBRARY_ROOT  drop folder of game directories to auto-discover\n");
    printf("  -d DB_PATH       SQLite database path\n");
    printf("  --closed         reject new registrations\n");
    printf("  -h, --help       this message\n");
}

int
main(int argc, char **argv)
{
    vapord            app;
    char              err[512];
    const char       *cfg_path = NULL;
    struct mg_context *ctx;
    struct mg_callbacks callbacks;
    char              port_str[sizeof(app.cfg.bind_addr) + 16];
    char              threads_str[32];
    const char       *options[16];
    int               i, nopt = 0;

    memset(&app, 0, sizeof(app));
    vapord_config_defaults(&app.cfg);

    /* Config file first so flags can override it. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }
    if (cfg_path && vapord_config_load(&app.cfg, cfg_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "vapord: %s\n", err);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            i++;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            app.cfg.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            snprintf(app.cfg.content_root, sizeof(app.cfg.content_root), "%s",
                     argv[++i]);
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            snprintf(app.cfg.library_root, sizeof(app.cfg.library_root), "%s",
                     argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            snprintf(app.cfg.db_path, sizeof(app.cfg.db_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            snprintf(app.cfg.bind_addr, sizeof(app.cfg.bind_addr), "%s",
                     argv[++i]);
        } else if (strcmp(argv[i], "--closed") == 0) {
            app.cfg.enable_registration = 0;
        } else {
            fprintf(stderr, "vapord: unexpected argument \"%s\"\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    if (app.cfg.port <= 0 || app.cfg.port > 65535) {
        fprintf(stderr, "vapord: port out of range\n");
        return 1;
    }

    if (vapord_auth_init(err, sizeof(err)) != 0) {
        fprintf(stderr, "vapord: %s\n", err);
        return 1;
    }

    /* Create the content root and the database's directory up front so a fresh
     * install does not fail on the first request instead of at startup. */
    if (vapord_content_mkdirs(app.cfg.content_root) != 0) {
        fprintf(stderr, "vapord: cannot create content_root \"%s\"\n",
                app.cfg.content_root);
        return 1;
    }
    if (app.cfg.library_root[0]
        && vapord_content_mkdirs(app.cfg.library_root) != 0) {
        fprintf(stderr, "vapord: cannot create library_root \"%s\"\n",
                app.cfg.library_root);
        return 1;
    }
    {
        char  dbdir[VAPORD_PATH_MAX];
        char *slash;
        snprintf(dbdir, sizeof(dbdir), "%s", app.cfg.db_path);
        slash = strrchr(dbdir, '/');
        if (slash && slash != dbdir) {
            *slash = '\0';
            if (vapord_content_mkdirs(dbdir) != 0) {
                fprintf(stderr, "vapord: cannot create db directory \"%s\"\n", dbdir);
                return 1;
            }
        }
    }

    if (vapord_db_open(&app, err, sizeof(err)) != 0) {
        fprintf(stderr, "vapord: %s\n", err);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#if defined(SIGPIPE)
    /* A client that walks away mid-download must not take the server with it. */
    signal(SIGPIPE, SIG_IGN);
#endif

    snprintf(port_str, sizeof(port_str), "%s:%d", app.cfg.bind_addr, app.cfg.port);
    snprintf(threads_str, sizeof(threads_str), "%d", app.cfg.num_threads);

    options[nopt++] = "listening_ports";
    options[nopt++] = port_str;
    options[nopt++] = "num_threads";
    options[nopt++] = threads_str;
    /* No document_root: every byte served goes through an authenticated route. */
    options[nopt++] = "enable_directory_listing";
    options[nopt++] = "no";
    options[nopt++] = "enable_keep_alive";
    options[nopt++] = "yes";
    options[nopt++] = "request_timeout_ms";
    options[nopt++] = "30000";
    options[nopt] = NULL;

    memset(&callbacks, 0, sizeof(callbacks));

    mg_init_library(0);
    ctx = mg_start(&callbacks, &app, options);
    if (!ctx) {
        fprintf(stderr, "vapord: could not bind %s\n", port_str);
        vapord_db_close(&app);
        return 1;
    }

    mg_set_request_handler(ctx, VAPOR_API_PREFIX "/**", dispatch, &app);

    printf("vapord %s listening on %s\n", VAPOR_VERSION_STRING, port_str);
    vapord_config_print(&app.cfg);
    {
        int64_t users = 0;
        if (vapord_user_count(app.db, &users) == 0) {
            printf("  accounts ......... %lld%s\n", (long long)users,
                   users == 0 ? " (first registration becomes admin)" : "");
        }
    }
    fflush(stdout);

    vapord_token_prune(app.db, vapor_now_unix());
    if (app.cfg.library_root[0]) {
        vapord_discover(&app);
    }

    {
        int64_t last_discover = vapor_now_unix();
        int64_t last_prune = last_discover;

        while (!g_stop) {
#if defined(_WIN32)
            Sleep(200);
#else
            struct timespec ts = { 0, 200 * 1000 * 1000 };
            nanosleep(&ts, NULL);
#endif
            {
                int64_t now = vapor_now_unix();
                if (app.cfg.library_root[0] && app.cfg.discover_interval > 0
                    && now - last_discover >= app.cfg.discover_interval) {
                    vapord_discover(&app);
                    last_discover = now;
                }
                if (now - last_prune >= 3600) {
                    vapord_token_prune(app.db, now);
                    last_prune = now;
                }
            }
        }
    }

    VLOG_INFO("shutting down");
    mg_stop(ctx);
    mg_exit_library();
    vapord_db_close(&app);
    return 0;
}
