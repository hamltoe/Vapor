/* Flat `key = value` config, shared by vapord and vapor-admin so both agree on
 * where the database and content root live. */

#include "vapord.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "vapor/util.h"

void
vapord_config_defaults(vapord_config *c)
{
    memset(c, 0, sizeof(*c));
    c->port = VAPOR_DEFAULT_PORT;
    c->enable_registration = 1;
    c->num_threads = 8;
    snprintf(c->bind_addr, sizeof(c->bind_addr), "0.0.0.0");
    snprintf(c->content_root, sizeof(c->content_root), "/srv/vapor/content");
    c->library_root[0] = '\0';
    snprintf(c->db_path, sizeof(c->db_path), "/var/lib/vapor/vapor.db");
    c->discover_interval = 60;
}

static char *
trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static int
parse_bool(const char *v, int *out)
{
    if (vapor_str_eq_ci(v, "true") || vapor_str_eq_ci(v, "yes")
        || strcmp(v, "1") == 0) {
        *out = 1;
        return 0;
    }
    if (vapor_str_eq_ci(v, "false") || vapor_str_eq_ci(v, "no")
        || strcmp(v, "0") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

int
vapord_config_load(vapord_config *c, const char *path, char *err, size_t errsz)
{
    FILE *f;
    char  line[VAPORD_PATH_MAX + 128];
    int   lineno = 0;

    f = fopen(path, "r");
    if (!f) {
        snprintf(err, errsz, "cannot open config file \"%s\"", path);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *key, *val, *eq;

        lineno++;
        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';') {
            continue;
        }
        eq = strchr(key, '=');
        if (!eq) {
            snprintf(err, errsz, "%s:%d: expected key = value", path, lineno);
            fclose(f);
            return -1;
        }
        *eq = '\0';
        key = trim(key);
        val = trim(eq + 1);

        if (strcmp(key, "listen_port") == 0) {
            c->port = atoi(val);
            if (c->port <= 0 || c->port > 65535) {
                snprintf(err, errsz, "%s:%d: listen_port out of range", path, lineno);
                fclose(f);
                return -1;
            }
        } else if (strcmp(key, "bind_addr") == 0) {
            snprintf(c->bind_addr, sizeof(c->bind_addr), "%s", val);
        } else if (strcmp(key, "content_root") == 0) {
            snprintf(c->content_root, sizeof(c->content_root), "%s", val);
        } else if (strcmp(key, "library_root") == 0) {
            snprintf(c->library_root, sizeof(c->library_root), "%s", val);
        } else if (strcmp(key, "discover_interval") == 0) {
            c->discover_interval = atoi(val);
            if (c->discover_interval < 0 || c->discover_interval > 86400) {
                snprintf(err, errsz, "%s:%d: discover_interval out of range",
                         path, lineno);
                fclose(f);
                return -1;
            }
        } else if (strcmp(key, "db_path") == 0) {
            snprintf(c->db_path, sizeof(c->db_path), "%s", val);
        } else if (strcmp(key, "enable_registration") == 0) {
            if (parse_bool(val, &c->enable_registration) != 0) {
                snprintf(err, errsz, "%s:%d: enable_registration wants a boolean",
                         path, lineno);
                fclose(f);
                return -1;
            }
        } else if (strcmp(key, "num_threads") == 0) {
            c->num_threads = atoi(val);
            if (c->num_threads < 1 || c->num_threads > 256) {
                snprintf(err, errsz, "%s:%d: num_threads out of range", path, lineno);
                fclose(f);
                return -1;
            }
        } else {
            snprintf(err, errsz, "%s:%d: unknown key \"%s\"", path, lineno, key);
            fclose(f);
            return -1;
        }
    }

    fclose(f);

    /* Trailing separators would produce "//" in every derived path. */
    {
        size_t n = strlen(c->content_root);
        while (n > 1 && (c->content_root[n - 1] == '/' || c->content_root[n - 1] == '\\')) {
            c->content_root[--n] = '\0';
        }
        n = strlen(c->library_root);
        while (n > 1 && (c->library_root[n - 1] == '/' || c->library_root[n - 1] == '\\')) {
            c->library_root[--n] = '\0';
        }
    }
    return 0;
}

void
vapord_config_print(const vapord_config *c)
{
    printf("  bind ............. %s:%d\n", c->bind_addr, c->port);
    printf("  content_root ..... %s\n", c->content_root);
    if (c->library_root[0]) {
        printf("  library_root ..... %s\n", c->library_root);
        if (c->discover_interval > 0) {
            printf("  discover ......... every %d s\n", c->discover_interval);
        } else {
            printf("  discover ......... once at startup\n");
        }
    } else {
        printf("  library_root ..... (disabled)\n");
    }
    printf("  db_path .......... %s\n", c->db_path);
    printf("  registration ..... %s\n", c->enable_registration ? "open" : "closed");
    printf("  threads .......... %d\n", c->num_threads);
}

void
vapord_log(const char *level, const char *fmt, ...)
{
    va_list   ap;
    time_t    now = time(NULL);
    struct tm tmv;
    char      stamp[32];

#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(stderr, "%s [%s] ", stamp, level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
