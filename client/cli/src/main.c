/* The vapor CLI. Every command is a thin shell over libvapor so the GUI can do
 * exactly the same work without duplicating logic. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/client.h"
#include "vapor/util.h"

#if defined(_WIN32)
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

static void
usage(void)
{
    printf("vapor %s - self-hosted game library\n\n", VAPOR_VERSION_STRING);
    printf("usage: vapor <command> [arguments]\n\n");
    printf("  Account\n");
    printf("    register [USERNAME]     create an account on the server\n");
    printf("    login [USERNAME]        sign in and store a session token\n");
    printf("    logout                  discard the session token\n");
    printf("    whoami                  show the signed-in account\n\n");
    printf("  Library\n");
    printf("    list                    show the server catalog\n");
    printf("    info GAME               show details for one game\n");
    printf("    installed               show what is installed locally\n\n");
    printf("  Games\n");
    printf("    install GAME [options]  download, verify and install\n");
    printf("      --version VERSION       install a specific version\n");
    printf("      --force                 reinstall even if up to date\n");
    printf("      --verify-only           download and check, do not extract\n");
    printf("      --keep-download         keep the archive in the cache\n");
    printf("    setup GAME              run a downloaded Windows installer\n");
    printf("    launch GAME             run an installed game\n");
    printf("    verify GAME             check an install against its manifest\n");
    printf("    uninstall GAME          delete an installed game\n\n");
    printf("  Configuration\n");
    printf("    config                  print the current settings\n");
    printf("    config server URL       point the client at a server\n");
    printf("    config library PATH     choose where games are installed\n");
    printf("    config pin PIN          pin a self-signed TLS public key\n");
    printf("                            (sha256//BASE64, or \"none\")\n");
    printf("    ping                    check the server is reachable\n\n");
}

/* ------------------------------------------------------------------ helpers */

static void
read_line(const char *prompt, char *out, size_t outsz)
{
    size_t n;

    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(out, (int)outsz, stdin)) {
        out[0] = '\0';
        return;
    }
    n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        out[--n] = '\0';
    }
}

/* Reads a password without echoing it. VAPOR_PASSWORD short-circuits the
 * prompt so scripts and CI can drive register/login non-interactively. */
static void
read_password(const char *prompt, char *out, size_t outsz)
{
    const char *env = getenv("VAPOR_PASSWORD");

    if (env && *env) {
        snprintf(out, outsz, "%s", env);
        return;
    }

    fputs(prompt, stdout);
    fflush(stdout);

#if defined(_WIN32)
    {
        size_t n = 0;
        int    ch;
        while ((ch = _getch()) != '\r' && ch != '\n') {
            if (ch == 3) { /* Ctrl-C */
                out[0] = '\0';
                printf("\n");
                exit(130);
            }
            if (ch == '\b' || ch == 127) {
                if (n > 0) {
                    n--;
                    fputs("\b \b", stdout);
                }
                continue;
            }
            if (ch == 0 || ch == 0xE0) {
                _getch(); /* swallow the second byte of a function key */
                continue;
            }
            if (n + 1 < outsz) {
                out[n++] = (char)ch;
                fputc('*', stdout);
            }
        }
        out[n] = '\0';
        printf("\n");
    }
#else
    {
        struct termios old, quiet;
        int            have_tty = (tcgetattr(STDIN_FILENO, &old) == 0);

        if (have_tty) {
            quiet = old;
            quiet.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
        }
        if (!fgets(out, (int)outsz, stdin)) {
            out[0] = '\0';
        } else {
            size_t n = strlen(out);
            while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
                out[--n] = '\0';
            }
        }
        if (have_tty) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        }
        printf("\n");
    }
#endif
}

/* ----------------------------------------------------------------- commands */

static int
cmd_ping(vapor_client *vc)
{
    vapor_server_info info;

    if (vapor_server_ping(vc, &info) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }

    printf("%s is up\n", vc->cfg.server_url);
    if (info.service[0]) {
        printf("  service ........ %s %s\n", info.service, info.version);
    }
    printf("  registration ... %s\n", info.registration_open ? "open" : "closed");
    if (!info.has_users) {
        printf("  note ........... no accounts yet; the first one you register "
               "becomes admin\n");
    }
    return 0;
}

static int
cmd_register(vapor_client *vc, const char *arg_user)
{
    char              username[VAPOR_USERNAME_MAX + 1];
    char              password[VAPOR_PASSWORD_MAX + 1];
    char              confirm[VAPOR_PASSWORD_MAX + 1];
    vapor_account     acct;
    vapor_server_info info;
    int               rc;

    if (vapor_require_server(vc, &info) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    if (!info.registration_open) {
        fprintf(stderr, "vapor: registration is closed on this server\n");
        return 1;
    }
    if (!info.has_users) {
        printf("note: no accounts yet; this one becomes admin\n");
    }

    if (arg_user) {
        snprintf(username, sizeof(username), "%s", arg_user);
    } else {
        read_line("username: ", username, sizeof(username));
    }

    read_password("password: ", password, sizeof(password));
    read_password("confirm password: ", confirm, sizeof(confirm));
    if (strcmp(password, confirm) != 0) {
        fprintf(stderr, "vapor: passwords do not match\n");
        vapor_secure_zero(password, sizeof(password));
        vapor_secure_zero(confirm, sizeof(confirm));
        return 1;
    }
    vapor_secure_zero(confirm, sizeof(confirm));

    rc = vapor_auth_register(vc, username, password, &acct);
    vapor_secure_zero(password, sizeof(password));
    if (rc != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }

    printf("created account \"%s\"%s on %s\n", acct.username,
           acct.is_admin ? " (admin)" : "", vc->cfg.server_url);
    printf("run \"vapor login\" to sign in.\n");
    return 0;
}

static int
cmd_login(vapor_client *vc, const char *arg_user)
{
    char username[VAPOR_USERNAME_MAX + 1];
    char password[VAPOR_PASSWORD_MAX + 1];
    int  rc;

    if (vapor_require_server(vc, NULL) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }

    if (arg_user) {
        snprintf(username, sizeof(username), "%s", arg_user);
    } else if (vc->cfg.username[0]) {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "username [%s]: ", vc->cfg.username);
        read_line(prompt, username, sizeof(username));
        if (!username[0]) {
            snprintf(username, sizeof(username), "%s", vc->cfg.username);
        }
    } else {
        read_line("username: ", username, sizeof(username));
    }
    read_password("password: ", password, sizeof(password));

    rc = vapor_auth_login(vc, username, password);
    vapor_secure_zero(password, sizeof(password));
    if (rc != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }

    printf("signed in as %s\n", vc->cfg.username);
    if (vc->cfg.token_expires_at > 0) {
        char    when[64];
        int64_t left = vc->cfg.token_expires_at - vapor_now_unix();
        vapor_format_duration(left, when, sizeof(when));
        printf("session valid for %s\n", when);
    }
    return 0;
}

static int
cmd_logout(vapor_client *vc)
{
    int rc;

    if (!vapor_client_has_token(vc)) {
        printf("not signed in\n");
        return 0;
    }
    rc = vapor_auth_logout(vc);
    if (rc < 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    if (rc > 0) {
        /* The token is gone locally; the server copy will lapse at its expiry. */
        fprintf(stderr, "vapor: warning: could not reach the server to revoke "
                        "the session\n");
    }
    printf("signed out\n");
    return 0;
}

static int
cmd_whoami(vapor_client *vc)
{
    vapor_account acct;

    if (!vapor_client_has_token(vc)) {
        printf("not signed in\n");
        return 1;
    }
    if (vapor_auth_whoami(vc, &acct) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    printf("%s%s\n", acct.username, acct.is_admin ? " (admin)" : "");
    printf("  server ... %s\n", vc->cfg.server_url);
    return 0;
}

static int
cmd_list(vapor_client *vc)
{
    vapor_catalog_entry *rows = NULL;
    size_t               n = 0, i;

    if (vapor_catalog_fetch(vc, &rows, &n) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    if (n == 0) {
        printf("the server catalog is empty\n");
        free(rows);
        return 0;
    }

    for (i = 0; i < n; i++) {
        char size[32];
        vapor_format_bytes(rows[i].size, size, sizeof(size));
        printf("%-22s %-10s %10s  %s%s\n", rows[i].id,
               rows[i].latest_version[0] ? rows[i].latest_version : "-", size,
               rows[i].name,
               rows[i].update_available  ? " [update available]"
               : rows[i].setup_pending   ? " [needs setup]"
               : rows[i].installed       ? " [installed]"
                                         : "");
    }
    free(rows);
    return 0;
}

static int
cmd_info(vapor_client *vc, const char *game_id)
{
    vapor_game_detail d;
    vapor_install     rec;
    size_t            i;

    if (vapor_game_fetch(vc, game_id, &d) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }

    printf("%s\n", d.game.name);
    printf("  id ............. %s\n", d.game.id);
    if (d.game.developer[0]) {
        printf("  developer ...... %s\n", d.game.developer);
    }
    printf("  latest ......... %s\n",
           d.game.latest_version[0] ? d.game.latest_version : "-");
    if (d.game.description[0]) {
        printf("  description .... %s\n", d.game.description);
    }
    if (d.game.has_cover) {
        printf("  cover .......... yes\n");
    }
    if (d.game.steam_rating_label[0] || d.game.steam_rating_pct > 0) {
        printf("  steam .......... %s",
               d.game.steam_rating_label[0] ? d.game.steam_rating_label
                                            : "rated");
        if (d.game.steam_rating_pct > 0) {
            printf(" (%d%% of %d)", d.game.steam_rating_pct,
                   d.game.steam_rating_count);
        }
        printf("\n");
    }
    if (d.game.rating_votes > 0) {
        printf("  community ...... %.1f / 5  (%d rating%s)\n",
               d.game.rating_avg, d.game.rating_votes,
               d.game.rating_votes == 1 ? "" : "s");
    }
    if (d.game.my_rating > 0) {
        printf("  your rating .... %d / 5\n", d.game.my_rating);
    }
    if (d.game.update_available) {
        printf("  update ......... %s -> %s\n", d.game.installed_version,
               d.game.latest_version);
    }

    printf("  versions ....... ");
    for (i = 0; i < d.nversions; i++) {
        printf("%s%s", i ? ", " : "", d.versions[i]);
    }
    printf("%s\n", d.nversions ? "" : "(none published)");

    if (vapor_db_get_install(vc, game_id, &rec) == 0) {
        printf("  installed ...... %s at %s%s\n", rec.version, rec.install_dir,
               rec.setup_pending ? " (setup pending)" : "");
        if (rec.play_seconds > 0) {
            char when[64];
            vapor_format_duration(rec.play_seconds, when, sizeof(when));
            printf("  playtime ....... %s\n", when);
        }
    } else {
        printf("  installed ...... no\n");
    }

    vapor_game_detail_free(&d);
    return 0;
}

static int
cmd_installed(vapor_client *vc)
{
    vapor_install *rows = NULL;
    size_t         n = 0, i;

    if (vapor_db_list_installs(vc, &rows, &n) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    if (n == 0) {
        printf("nothing installed yet; try \"vapor list\" then \"vapor install "
               "<game>\"\n");
        free(rows);
        return 0;
    }

    for (i = 0; i < n; i++) {
        char size[32], played[64];
        vapor_format_bytes(rows[i].size_on_disk, size, sizeof(size));
        vapor_format_duration(rows[i].play_seconds, played, sizeof(played));
        printf("%-22s %-10s %10s  %-28s %s\n", rows[i].game_id, rows[i].version,
               size, rows[i].name,
               rows[i].setup_pending ? "setup pending"
               : rows[i].play_seconds > 0 ? played
                                          : "");
    }
    free(rows);
    return 0;
}

/* ------------------------------------------------------- progress reporting */

typedef struct {
    int      last_percent;
    int      is_tty;
    uint64_t last_done;
} progress_state;

static int
on_progress(void *ud, uint64_t done, uint64_t total)
{
    progress_state *ps = (progress_state *)ud;

    if (total == 0) {
        /* Unknown length: fall back to reporting bytes every megabyte. */
        char a[32];

        if (done - ps->last_done < 1024 * 1024) {
            return 0;
        }
        ps->last_done = done;
        vapor_format_bytes(done, a, sizeof(a));
        printf("\r  downloaded %s", a);
        fflush(stdout);
        return 0;
    }

    {
        int pct = (int)((done * 100) / total);
        if (pct == ps->last_percent) {
            return 0;
        }
        ps->last_percent = pct;

        if (ps->is_tty) {
            /* A 30-cell bar, redrawn in place. Overall install, not download. */
            int filled = pct * 30 / 100, i;
            printf("\r  [");
            for (i = 0; i < 30; i++) {
                putchar(i < filled ? '#' : ' ');
            }
            printf("] %3d%%", pct);
        } else if (pct % 10 == 0) {
            printf("  %3d%%\n", pct);
        }
        fflush(stdout);
    }
    return 0;
}

static void
progress_done(progress_state *ps)
{
    if (ps->is_tty && ps->last_percent >= 0) {
        printf("\n");
        fflush(stdout);
    }
}

static int
stdout_is_tty(void)
{
#if defined(_WIN32)
    return _isatty(_fileno(stdout));
#else
    return isatty(fileno(stdout));
#endif
}

static int
cmd_install(vapor_client *vc, int argc, char **argv)
{
    vapor_install_opts opts;
    progress_state     ps;
    const char        *game_id = NULL, *version = NULL;
    int                i, rc;

    memset(&opts, 0, sizeof(opts));
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) {
            version = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0) {
            opts.force = 1;
        } else if (strcmp(argv[i], "--verify-only") == 0) {
            opts.verify_only = 1;
        } else if (strcmp(argv[i], "--keep-download") == 0) {
            opts.keep_download = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "vapor: unknown option \"%s\"\n", argv[i]);
            return 1;
        } else if (!game_id) {
            game_id = argv[i];
        } else {
            fprintf(stderr, "vapor: install takes one game id\n");
            return 1;
        }
    }
    if (!game_id) {
        fprintf(stderr, "vapor: install needs a game id\n");
        return 1;
    }

    memset(&ps, 0, sizeof(ps));
    ps.last_percent = -1;
    ps.is_tty = stdout_is_tty();

    rc = vapor_install_game(vc, game_id, version, &opts, on_progress, &ps);
    progress_done(&ps);

    if (rc == 1) {
        /* Already installed: worth saying, not worth failing over. */
        printf("%s\n", vc->err);
        return 0;
    }
    if (rc != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    return 0;
}

static int
cmd_setup(vapor_client *vc, const char *game_id)
{
    int rc = vapor_setup_game(vc, game_id);

    if (rc < 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    if (rc > 0) {
        if (vc->err[0]) {
            fprintf(stderr, "vapor: %s\n", vc->err);
        } else {
            fprintf(stderr,
                    "vapor: installer finished, but the game was not found\n");
        }
        return 1;
    }
    return 0;
}

static int
cmd_uninstall(vapor_client *vc, const char *game_id)
{
    if (vapor_uninstall_game(vc, game_id) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    return 0;
}

static int
cmd_verify(vapor_client *vc, const char *game_id)
{
    int rc = vapor_verify_install(vc, game_id);

    if (rc < 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    return rc;
}

static int
cmd_launch(vapor_client *vc, const char *game_id)
{
    int exit_code = 0;

    if (vapor_launch_game(vc, game_id, &exit_code) != 0) {
        fprintf(stderr, "vapor: %s\n", vc->err);
        return 1;
    }
    return 0;
}

static int
cmd_config(vapor_client *vc, int argc, char **argv)
{
    if (argc == 0) {
        printf("config file .... %s\n", vc->config_path);
        printf("server_url ..... %s\n", vc->cfg.server_url);
        printf("library_dir .... %s\n", vc->cfg.library_dir);
        printf("username ....... %s\n",
               vc->cfg.username[0] ? vc->cfg.username : "(none)");
        printf("session ........ %s\n",
               vapor_client_has_token(vc) ? "active" : "none");
        if (vc->cfg.pinned_pubkey[0]) {
            printf("pinned_pubkey .. %s\n", vc->cfg.pinned_pubkey);
        }
        printf("local database . %s\n", vc->db_path);
        return 0;
    }

    if (strcmp(argv[0], "server") == 0 && argc == 2) {
        int had_token = vc->cfg.token[0] != '\0';

        if (vapor_client_set_server_url(vc, argv[1]) != 0) {
            fprintf(stderr, "vapor: %s\n", vc->err);
            return 1;
        }
        if (had_token && !vc->cfg.token[0]) {
            printf("note: cleared the stored session because the server changed\n");
        }
        if (vapor_client_save_config(vc) != 0) {
            fprintf(stderr, "vapor: %s\n", vc->err);
            return 1;
        }
        printf("server is now %s\n", vc->cfg.server_url);
        return 0;
    }

    if (strcmp(argv[0], "library") == 0 && argc == 2) {
        snprintf(vc->cfg.library_dir, sizeof(vc->cfg.library_dir), "%s", argv[1]);
        if (vapor_client_save_config(vc) != 0) {
            fprintf(stderr, "vapor: %s\n", vc->err);
            return 1;
        }
        printf("library directory is now %s\n", vc->cfg.library_dir);
        return 0;
    }

    if (strcmp(argv[0], "pin") == 0 && argc == 2) {
        if (strcmp(argv[1], "none") == 0 || strcmp(argv[1], "off") == 0) {
            vc->cfg.pinned_pubkey[0] = '\0';
        } else if (!vapor_str_has_prefix(argv[1], "sha256//")) {
            fprintf(stderr, "vapor: pin must be sha256//BASE64 or \"none\"\n");
            return 1;
        } else if ((size_t)snprintf(vc->cfg.pinned_pubkey,
                                    sizeof(vc->cfg.pinned_pubkey), "%s",
                                    argv[1])
                   >= sizeof(vc->cfg.pinned_pubkey)) {
            fprintf(stderr, "vapor: pin is too long\n");
            return 1;
        }
        if (vapor_client_save_config(vc) != 0) {
            fprintf(stderr, "vapor: %s\n", vc->err);
            return 1;
        }
        if (vc->cfg.pinned_pubkey[0]) {
            printf("pinned public key is now %s\n", vc->cfg.pinned_pubkey);
        } else {
            printf("public-key pin cleared; normal CA checks apply\n");
        }
        return 0;
    }

    fprintf(stderr,
            "vapor: usage: vapor config [server URL | library PATH | pin PIN]\n");
    return 1;
}

int
main(int argc, char **argv)
{
    vapor_client vc;
    const char  *cmd;
    int          rc = 0;

    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0
        || strcmp(argv[1], "help") == 0) {
        usage();
        return argc < 2 ? 1 : 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0) {
        printf("vapor %s (%s/%s)\n", VAPOR_VERSION_STRING, vapor_host_platform(),
               vapor_host_arch());
        return 0;
    }

    if (vapor_client_open(&vc) != 0) {
        fprintf(stderr, "vapor: %s\n", vc.err);
        return 1;
    }

    cmd = argv[1];
    if (strcmp(cmd, "ping") == 0) {
        rc = cmd_ping(&vc);
    } else if (strcmp(cmd, "register") == 0) {
        rc = cmd_register(&vc, argc > 2 ? argv[2] : NULL);
    } else if (strcmp(cmd, "login") == 0) {
        rc = cmd_login(&vc, argc > 2 ? argv[2] : NULL);
    } else if (strcmp(cmd, "logout") == 0) {
        rc = cmd_logout(&vc);
    } else if (strcmp(cmd, "whoami") == 0) {
        rc = cmd_whoami(&vc);
    } else if (strcmp(cmd, "list") == 0) {
        rc = cmd_list(&vc);
    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "vapor: info needs a game id\n");
            rc = 1;
        } else {
            rc = cmd_info(&vc, argv[2]);
        }
    } else if (strcmp(cmd, "installed") == 0) {
        rc = cmd_installed(&vc);
    } else if (strcmp(cmd, "install") == 0) {
        rc = cmd_install(&vc, argc - 2, argv + 2);
    } else if (strcmp(cmd, "setup") == 0) {
        if (argc < 3) {
            fprintf(stderr, "vapor: setup needs a game id\n");
            rc = 1;
        } else {
            rc = cmd_setup(&vc, argv[2]);
        }
    } else if (strcmp(cmd, "uninstall") == 0) {
        if (argc < 3) {
            fprintf(stderr, "vapor: uninstall needs a game id\n");
            rc = 1;
        } else {
            rc = cmd_uninstall(&vc, argv[2]);
        }
    } else if (strcmp(cmd, "verify") == 0) {
        if (argc < 3) {
            fprintf(stderr, "vapor: verify needs a game id\n");
            rc = 1;
        } else {
            rc = cmd_verify(&vc, argv[2]);
        }
    } else if (strcmp(cmd, "launch") == 0 || strcmp(cmd, "play") == 0) {
        if (argc < 3) {
            fprintf(stderr, "vapor: %s needs a game id\n", cmd);
            rc = 1;
        } else {
            rc = cmd_launch(&vc, argv[2]);
        }
    } else if (strcmp(cmd, "config") == 0) {
        rc = cmd_config(&vc, argc - 2, argv + 2);
    } else {
        fprintf(stderr, "vapor: unknown command \"%s\"\n\n", cmd);
        usage();
        rc = 1;
    }

    vapor_client_close(&vc);
    return rc;
}
