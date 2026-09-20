/* The two screens, plus the theme.
 *
 * Nuklear is immediate-mode: this code runs every frame and describes the whole
 * window from current state, so there is no widget tree to keep in sync. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "vapor/util.h"

#define ROW_H   28.0f
/* Cover, title, rating, state, optional Play. Install/verify live on the
 * details page so the grid stays a browse view. */
#define COVER_H 150.0f
#define CARD_H  248.0f
/* The narrowest a card may get before the grid drops to fewer columns. Cards
 * then stretch to share the row, so this is a minimum and not a fixed size. */
#define CARD_W  260.0f
#define PAD_X   18.0f
#define PAD_Y   16.0f
#define GAP     10.0f

#define COL_TEXT_V    nk_rgb(222, 227, 234)
#define COL_MUTED_V   nk_rgb(141, 152, 168)
#define COL_GOOD_V    nk_rgb(104, 190, 124)
#define COL_WARN_V    nk_rgb(226, 170, 80)
#define COL_BAD_V     nk_rgb(222, 106, 106)

void
vapor_gui_style(struct nk_context *ctx)
{
    struct nk_color t[NK_COLOR_COUNT];
    struct nk_color bg = nk_rgb(22, 26, 32);
    struct nk_color panel = nk_rgb(32, 37, 45);
    struct nk_color card = nk_rgb(41, 47, 57);
    struct nk_color accent = nk_rgb(66, 150, 210);
    struct nk_color accent_h = nk_rgb(86, 172, 232);
    struct nk_color field = nk_rgb(26, 30, 37);

    /* Every slot is set explicitly rather than patching the defaults, so a
     * widget we have not used yet still looks like it belongs. */
    t[NK_COLOR_TEXT] = COL_TEXT_V;
    t[NK_COLOR_WINDOW] = bg;
    t[NK_COLOR_HEADER] = panel;
    t[NK_COLOR_BORDER] = nk_rgb(58, 66, 78);
    t[NK_COLOR_BUTTON] = accent;
    t[NK_COLOR_BUTTON_HOVER] = accent_h;
    t[NK_COLOR_BUTTON_ACTIVE] = nk_rgb(46, 124, 180);
    t[NK_COLOR_TOGGLE] = card;
    t[NK_COLOR_TOGGLE_HOVER] = nk_rgb(56, 64, 76);
    t[NK_COLOR_TOGGLE_CURSOR] = accent;
    t[NK_COLOR_SELECT] = card;
    t[NK_COLOR_SELECT_ACTIVE] = accent;
    t[NK_COLOR_SLIDER] = field;
    t[NK_COLOR_SLIDER_CURSOR] = accent;
    t[NK_COLOR_SLIDER_CURSOR_HOVER] = accent_h;
    t[NK_COLOR_SLIDER_CURSOR_ACTIVE] = accent_h;
    t[NK_COLOR_PROPERTY] = card;
    t[NK_COLOR_EDIT] = field;
    t[NK_COLOR_EDIT_CURSOR] = COL_TEXT_V;
    t[NK_COLOR_COMBO] = card;
    t[NK_COLOR_CHART] = card;
    t[NK_COLOR_CHART_COLOR] = accent;
    t[NK_COLOR_CHART_COLOR_HIGHLIGHT] = COL_BAD_V;
    t[NK_COLOR_SCROLLBAR] = field;
    t[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgb(70, 80, 94);
    t[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgb(90, 102, 118);
    t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = accent;
    t[NK_COLOR_TAB_HEADER] = panel;
    t[NK_COLOR_KNOB] = field;
    t[NK_COLOR_KNOB_CURSOR] = accent;
    t[NK_COLOR_KNOB_CURSOR_HOVER] = accent_h;
    t[NK_COLOR_KNOB_CURSOR_ACTIVE] = accent_h;
    nk_style_from_table(ctx, t);

    ctx->style.window.padding = nk_vec2(PAD_X, PAD_Y);
    ctx->style.window.spacing = nk_vec2(GAP, GAP);
    ctx->style.window.group_padding = nk_vec2(12, 10);
    ctx->style.window.border = 0.0f;
    ctx->style.button.rounding = 3.0f;
    ctx->style.window.fixed_background = nk_style_item_color(bg);
    /* Cards are groups, so the group background is what gives them a surface
     * distinct from the page. */
    ctx->style.window.group_border = 0.0f;
}

static void
draw_notice(struct nk_context *ctx, vapor_app *app)
{
    if (!app->notice[0]) {
        return;
    }
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label_colored(ctx, app->notice, NK_TEXT_LEFT,
                     app->notice_is_error ? COL_BAD_V : COL_GOOD_V);
}

/* ------------------------------------------------------------ login screen */

void
vapor_gui_login_screen(struct nk_context *ctx, vapor_app *app, int w, int h)
{
    const float panel_w = 430.0f;
    const float panel_h = 470.0f;
    struct nk_rect area;
    int            busy = vapor_gui_job_busy(app);

    area = nk_rect(((float)w - panel_w) / 2.0f, ((float)h - panel_h) / 2.0f,
                   panel_w, panel_h);

    if (nk_begin(ctx, "sign in", area, NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 34, 1);
        nk_label_colored(ctx, "Vapor", NK_TEXT_CENTERED, COL_TEXT_V);
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, "your self-hosted game library", NK_TEXT_CENTERED,
                         COL_MUTED_V);
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx,
                         "a live server is required to sign in or create an account",
                         NK_TEXT_CENTERED, COL_MUTED_V);

        nk_layout_row_dynamic(ctx, 8, 1);
        nk_spacing(ctx, 1);

        nk_layout_row_dynamic(ctx, 16, 1);
        nk_label_colored(ctx, "Server", NK_TEXT_LEFT, COL_MUTED_V);
        nk_layout_row_dynamic(ctx, ROW_H, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, app->server_url, &app->server_url_len,
                       (int)sizeof(app->server_url) - 1, nk_filter_default);

        nk_layout_row_dynamic(ctx, 16, 1);
        nk_label_colored(ctx, "Username", NK_TEXT_LEFT, COL_MUTED_V);
        nk_layout_row_dynamic(ctx, ROW_H, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, app->username, &app->username_len,
                       (int)sizeof(app->username) - 1, nk_filter_default);

        nk_layout_row_dynamic(ctx, 16, 1);
        nk_label_colored(ctx, "Password", NK_TEXT_LEFT, COL_MUTED_V);
        nk_layout_row_dynamic(ctx, ROW_H, 1);
        /* Nuklear has no password flag. The upstream idiom is to show a buffer
         * of stars and copy back whatever the edit appended past the old
         * length, which keeps the real characters out of the drawn text. */
        {
            char shown[VAPOR_PASSWORD_MAX + 1];
            int  old_len = app->password_len;
            int  len = app->password_len;
            int  i;

            for (i = 0; i < len; i++) {
                shown[i] = '*';
            }
            nk_edit_string(ctx, NK_EDIT_FIELD, shown, &len,
                           (int)sizeof(shown) - 1, nk_filter_default);
            if (len > old_len) {
                memcpy(app->password + old_len, shown + old_len,
                       (size_t)(len - old_len));
            }
            app->password_len = len;
            app->password[len] = '\0';
        }

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        nk_layout_row_dynamic(ctx, 32, 2);
        if (busy) {
            char           status[256];
            uint64_t       d, tot;
            vapor_job_kind k;

            vapor_gui_job_progress(app, status, sizeof(status), &d, &tot, &k);
            nk_label_colored(ctx, status[0] ? status : "working...", NK_TEXT_LEFT,
                             COL_MUTED_V);
            nk_spacing(ctx, 1);
        } else {
            if (nk_button_label(ctx, "Sign in")) {
                vapor_gui_start_login(app, 0);
            }
            if (nk_button_label(ctx, "Create account")) {
                vapor_gui_start_login(app, 1);
            }
        }

        draw_notice(ctx, app);

        nk_layout_row_dynamic(ctx, 16, 1);
        nk_label_colored(ctx, app->server_info, NK_TEXT_LEFT, COL_MUTED_V);
    }
    nk_end(ctx);
}

/* ---------------------------------------------------------- library screen */

/* The header is its own top-level window so the grid below can be a plain
 * scrolling region, instead of the grid having to work out how much vertical
 * space the header left behind. */
static float
header_height(const vapor_app *app, int busy)
{
    float h = PAD_Y * 2 + 30;   /* padding plus the toolbar row */

    if (app->show_settings) {
        h += 3 * (ROW_H + GAP);
    }
    if (app->notice[0]) {
        h += 22 + GAP;
    }
    if (busy) {
        h += 20 + GAP + 20 + GAP;   /* progress label plus the bar row */
    }
    return h;
}

static void
draw_toolbar(struct nk_context *ctx, vapor_app *app, int busy)
{
    /* A row template rather than fixed widths: the search box absorbs whatever
     * is left, so the buttons on the right never fall off a narrow window. */
    nk_layout_row_template_begin(ctx, 30);
    nk_layout_row_template_push_static(ctx, 70);
    nk_layout_row_template_push_static(ctx, 170);
    if (app->selected_id[0]) {
        nk_layout_row_template_push_static(ctx, 80);
        nk_layout_row_template_push_dynamic(ctx);
    } else {
        nk_layout_row_template_push_dynamic(ctx);
    }
    nk_layout_row_template_push_static(ctx, 90);
    nk_layout_row_template_push_static(ctx, 90);
    nk_layout_row_template_end(ctx);

    nk_label_colored(ctx, "Vapor", NK_TEXT_LEFT, COL_TEXT_V);

    {
        char who[200];
        snprintf(who, sizeof(who), "%s%s", app->account.username,
                 app->account.is_admin ? "  (admin)" : "");
        nk_label_colored(ctx, who, NK_TEXT_LEFT, COL_MUTED_V);
    }

    if (app->selected_id[0]) {
        if (nk_button_label(ctx, "Back")) {
            app->selected_id[0] = '\0';
        }
        nk_spacing(ctx, 1);
    } else {
        nk_edit_string(ctx, NK_EDIT_FIELD, app->search, &app->search_len,
                       (int)sizeof(app->search) - 1, nk_filter_default);
    }

    if (busy) {
        nk_label_colored(ctx, "working", NK_TEXT_CENTERED, COL_MUTED_V);
    } else if (nk_button_label(ctx, "Refresh")) {
        vapor_gui_start_refresh(app);
    }

    if (nk_button_label(ctx, app->show_settings ? "Close" : "Settings")) {
        app->show_settings = !app->show_settings;
    }
}

static void
draw_settings(struct nk_context *ctx, vapor_app *app, int busy)
{
    nk_layout_row_template_begin(ctx, ROW_H);
    nk_layout_row_template_push_static(ctx, 70);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    nk_label_colored(ctx, "Server", NK_TEXT_LEFT, COL_MUTED_V);
    nk_edit_string(ctx, NK_EDIT_FIELD, app->server_url, &app->server_url_len,
                   (int)sizeof(app->server_url) - 1, nk_filter_default);

    nk_layout_row_template_begin(ctx, ROW_H);
    nk_layout_row_template_push_static(ctx, 70);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    nk_label_colored(ctx, "Library", NK_TEXT_LEFT, COL_MUTED_V);
    nk_edit_string(ctx, NK_EDIT_FIELD, app->library_dir, &app->library_dir_len,
                   (int)sizeof(app->library_dir) - 1, nk_filter_default);

    nk_layout_row_begin(ctx, NK_STATIC, ROW_H, 3);
    nk_layout_row_push(ctx, 70);
    nk_spacing(ctx, 1);
    nk_layout_row_push(ctx, 150);
    if (!busy && nk_button_label(ctx, "Save settings")) {
        app->server_url[app->server_url_len] = '\0';
        app->library_dir[app->library_dir_len] = '\0';
        snprintf(app->vc->cfg.server_url, sizeof(app->vc->cfg.server_url), "%s",
                 app->server_url);
        snprintf(app->vc->cfg.library_dir, sizeof(app->vc->cfg.library_dir), "%s",
                 app->library_dir);
        if (vapor_client_save_config(app->vc) != 0) {
            vapor_gui_notice(app, 1, "%s", app->vc->err);
        } else {
            vapor_gui_notice(app, 0, "settings saved");
            vapor_gui_start_refresh(app);
        }
    }
    nk_layout_row_push(ctx, 120);
    if (!busy && nk_button_label(ctx, "Sign out")) {
        vapor_auth_logout(app->vc);
        app->screen = SCREEN_LOGIN;
        app->show_settings = 0;
        vapor_gui_covers_release(app);
        free(app->games);
        app->games = NULL;
        app->ngames = 0;
        app->catalog_loaded = 0;
        app->selected_id[0] = '\0';
        vapor_gui_notice(app, 0, "signed out");
    }
    nk_layout_row_end(ctx);
}

static void
draw_progress(struct nk_context *ctx, vapor_app *app)
{
    char           status[256];
    uint64_t       done = 0, total = 0;
    vapor_job_kind kind;
    nk_size        value, max;

    vapor_gui_job_progress(app, status, sizeof(status), &done, &total, &kind);

    nk_layout_row_dynamic(ctx, 20, 1);
    if (total > 0) {
        char a[32], b[32], line[380];
        vapor_format_bytes(done, a, sizeof(a));
        vapor_format_bytes(total, b, sizeof(b));
        snprintf(line, sizeof(line), "%s   %s of %s  (%d%%)", status, a, b,
                 (int)((done * 100) / total));
        nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_TEXT_V);
    } else {
        nk_label_colored(ctx, status[0] ? status : "working...", NK_TEXT_LEFT,
                         COL_TEXT_V);
    }

    nk_layout_row_template_begin(ctx, 20);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, 90);
    nk_layout_row_template_end(ctx);

    /* With no known total the bar sits full rather than jumping around. */
    max = total > 0 ? (nk_size)total : 1;
    value = total > 0 ? (nk_size)done : 1;
    nk_progress(ctx, &value, max, NK_FIXED);

    if (kind == JOB_INSTALL) {
        if (nk_button_label(ctx, "Cancel")) {
            SDL_LockMutex(app->job.lock);
            app->job.cancel = 1;
            SDL_UnlockMutex(app->job.lock);
        }
    } else {
        nk_spacing(ctx, 1);
    }
}

static void
lowercase(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s + 32);
        }
    }
}

static int
matches_search(const vapor_app *app, const vapor_catalog_entry *e)
{
    char needle[64];
    char hay[sizeof(e->name) + sizeof(e->id) + 2];

    if (app->search_len <= 0) {
        return 1;
    }
    snprintf(needle, sizeof(needle), "%.*s", app->search_len, app->search);
    lowercase(needle);
    snprintf(hay, sizeof(hay), "%s %s", e->name, e->id);
    lowercase(hay);
    return strstr(hay, needle) != NULL;
}

static int
find_game(const vapor_app *app, const char *id)
{
    size_t i;

    if (!id || !id[0]) {
        return -1;
    }
    for (i = 0; i < app->ngames; i++) {
        if (strcmp(app->games[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void
open_details(vapor_app *app, const char *id)
{
    snprintf(app->selected_id, sizeof(app->selected_id), "%s", id);
}

static void
rating_text(const vapor_catalog_entry *e, char *out, size_t outsz)
{
    if (e->steam_rating_label[0]) {
        if (e->steam_rating_pct > 0) {
            snprintf(out, outsz, "%s  %d%%", e->steam_rating_label,
                     e->steam_rating_pct);
        } else {
            snprintf(out, outsz, "%s", e->steam_rating_label);
        }
    } else if (e->rating_votes > 0) {
        snprintf(out, outsz, "%.1f / 5  (%d)", e->rating_avg, e->rating_votes);
    } else {
        out[0] = '\0';
    }
}

static void
draw_card(struct nk_context *ctx, vapor_app *app, size_t index, int busy)
{
    const vapor_catalog_entry *e = &app->games[index];
    char            size[32], line[320], rate[160];
    const char     *state;
    struct nk_color state_col;
    struct nk_image cover;
    int             open;

    /* A group paints style.window.fixed_background like any other panel, so
     * pushing a different one for the duration is what gives cards a surface
     * distinct from the page behind them. */
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                             nk_style_item_color(nk_rgb(41, 47, 57)));
    nk_style_push_float(ctx, &ctx->style.window.rounding, 4.0f);

    open = nk_group_begin(ctx, e->id, NK_WINDOW_NO_SCROLLBAR);
    if (!open) {
        nk_style_pop_float(ctx);
        nk_style_pop_style_item(ctx);
        return;
    }

    nk_layout_row_dynamic(ctx, COVER_H, 1);
    if (vapor_gui_cover_image(app, index, &cover)) {
        nk_image(ctx, cover);
    } else {
        nk_label_colored(ctx, e->name, NK_TEXT_CENTERED, COL_MUTED_V);
    }
    if (nk_widget_is_mouse_clicked(ctx, NK_BUTTON_LEFT)) {
        open_details(app, e->id);
    }

    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label_colored(ctx, e->name, NK_TEXT_LEFT, COL_TEXT_V);
    if (nk_widget_is_mouse_clicked(ctx, NK_BUTTON_LEFT)) {
        open_details(app, e->id);
    }

    rating_text(e, rate, sizeof(rate));
    nk_layout_row_dynamic(ctx, 16, 1);
    if (rate[0]) {
        nk_label_colored(ctx, rate, NK_TEXT_LEFT, COL_WARN_V);
    } else {
        vapor_format_bytes(e->size, size, sizeof(size));
        snprintf(line, sizeof(line), "%s   %s",
                 e->latest_version[0] ? e->latest_version : "-", size);
        nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_MUTED_V);
    }

    nk_layout_row_dynamic(ctx, 16, 1);
    if (e->update_available) {
        snprintf(line, sizeof(line), "update from %s", e->installed_version);
        state = line;
        state_col = COL_WARN_V;
    } else if (e->installed) {
        if (e->play_seconds > 0) {
            char played[64];
            vapor_format_duration(e->play_seconds, played, sizeof(played));
            snprintf(line, sizeof(line), "installed   %s played", played);
        } else {
            snprintf(line, sizeof(line), "installed");
        }
        state = line;
        state_col = COL_GOOD_V;
    } else {
        state = e->developer[0] ? e->developer : "not installed";
        state_col = COL_MUTED_V;
    }
    nk_label_colored(ctx, state, NK_TEXT_LEFT, state_col);

    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);

    nk_layout_row_dynamic(ctx, ROW_H, 1);
    if (!busy && e->installed) {
        if (nk_button_label(ctx, "Play")) {
            vapor_gui_start_launch(app, e->id);
        }
    } else {
        nk_spacing(ctx, 1);
    }

    nk_group_end(ctx);
    nk_style_pop_float(ctx);
    nk_style_pop_style_item(ctx);
}

static float
desc_height(struct nk_context *ctx, const char *text)
{
    float width = nk_window_get_content_region_size(ctx).x;
    float line = ctx->style.font->height + 4.0f;
    int   cols, n, lines;

    if (!text || !text[0]) {
        return line * 2.0f;
    }
    cols = (int)(width / 7.5f);
    if (cols < 24) {
        cols = 24;
    }
    n = (int)strlen(text);
    lines = (n + cols - 1) / cols;
    if (lines < 3) {
        lines = 3;
    }
    if (lines > 14) {
        lines = 14;
    }
    return line * (float)lines;
}

static void
draw_detail(struct nk_context *ctx, vapor_app *app, size_t index, int busy)
{
    const vapor_catalog_entry *e = &app->games[index];
    char            size[32], line[320], rate[160];
    struct nk_image cover;
    int             i;

    nk_layout_row_template_begin(ctx, 280);
    nk_layout_row_template_push_static(ctx, 200);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);

    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                             nk_style_item_color(nk_rgb(41, 47, 57)));
    if (nk_group_begin(ctx, "detail-art", NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 260, 1);
        if (vapor_gui_cover_image(app, index, &cover)) {
            nk_image(ctx, cover);
        } else {
            nk_label_colored(ctx, "no cover", NK_TEXT_CENTERED, COL_MUTED_V);
        }
        nk_group_end(ctx);
    }
    nk_style_pop_style_item(ctx);

    if (nk_group_begin(ctx, "detail-meta", 0)) {
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_label_colored(ctx, e->name, NK_TEXT_LEFT, COL_TEXT_V);

        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, e->developer[0] ? e->developer : e->id,
                         NK_TEXT_LEFT, COL_MUTED_V);

        rating_text(e, rate, sizeof(rate));
        nk_layout_row_dynamic(ctx, 18, 1);
        if (rate[0]) {
            nk_label_colored(ctx, rate, NK_TEXT_LEFT, COL_WARN_V);
        } else {
            nk_label_colored(ctx, "no Steam rating yet", NK_TEXT_LEFT, COL_MUTED_V);
        }

        if (e->rating_votes > 0) {
            snprintf(line, sizeof(line), "Vapor community  %.1f / 5  (%d)",
                     e->rating_avg, e->rating_votes);
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_GOOD_V);
        }

        vapor_format_bytes(e->size, size, sizeof(size));
        snprintf(line, sizeof(line), "%s   %s",
                 e->latest_version[0] ? e->latest_version : "-", size);
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_MUTED_V);

        nk_layout_row_dynamic(ctx, 18, 1);
        if (e->update_available) {
            snprintf(line, sizeof(line), "update available from %s",
                     e->installed_version);
            nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_WARN_V);
        } else if (e->installed) {
            if (e->play_seconds > 0) {
                char played[64];
                vapor_format_duration(e->play_seconds, played, sizeof(played));
                snprintf(line, sizeof(line), "installed   %s played", played);
                nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_GOOD_V);
            } else {
                nk_label_colored(ctx, "installed", NK_TEXT_LEFT, COL_GOOD_V);
            }
        } else {
            nk_label_colored(ctx, "not installed", NK_TEXT_LEFT, COL_MUTED_V);
        }

        nk_layout_row_dynamic(ctx, 10, 1);
        nk_spacing(ctx, 1);

        nk_layout_row_begin(ctx, NK_STATIC, ROW_H, 6);
        nk_layout_row_push(ctx, 88);
        nk_label_colored(ctx, "Your rating", NK_TEXT_LEFT, COL_MUTED_V);
        for (i = 1; i <= 5; i++) {
            char lab[4];
            int  mine = (e->my_rating == i);

            snprintf(lab, sizeof(lab), "%d", i);
            nk_layout_row_push(ctx, 36);
            if (mine) {
                nk_style_push_style_item(ctx, &ctx->style.button.normal,
                                         nk_style_item_color(COL_WARN_V));
                nk_style_push_style_item(ctx, &ctx->style.button.hover,
                                         nk_style_item_color(COL_WARN_V));
                nk_style_push_style_item(ctx, &ctx->style.button.active,
                                         nk_style_item_color(COL_WARN_V));
            }
            if (busy) {
                nk_label_colored(ctx, lab, NK_TEXT_CENTERED, COL_MUTED_V);
            } else if (nk_button_label(ctx, lab)) {
                vapor_gui_start_rate(app, e->id, i);
            }
            if (mine) {
                nk_style_pop_style_item(ctx);
                nk_style_pop_style_item(ctx);
                nk_style_pop_style_item(ctx);
            }
        }
        nk_layout_row_end(ctx);

        nk_group_end(ctx);
    }

    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label_colored(ctx, "Description", NK_TEXT_LEFT, COL_MUTED_V);
    nk_layout_row_dynamic(ctx, desc_height(ctx, e->description[0]
                                               ? e->description
                                               : "No description yet."), 1);
    if (e->description[0]) {
        nk_label_wrap(ctx, e->description);
    } else {
        nk_label_wrap(ctx,
                      "No description yet. vapord fills this in from Steam when "
                      "it can match the title.");
    }

    if (busy) {
        nk_layout_row_dynamic(ctx, ROW_H, 1);
        nk_spacing(ctx, 1);
        return;
    }

    if (e->installed) {
        nk_layout_row_dynamic(ctx, ROW_H, e->update_available ? 4 : 3);
        if (nk_button_label(ctx, "Play")) {
            vapor_gui_start_launch(app, e->id);
        }
        if (e->update_available && nk_button_label(ctx, "Update")) {
            vapor_gui_start_install(app, e->id, e->latest_version);
        }
        if (nk_button_label(ctx, "Verify")) {
            vapor_gui_start_verify(app, e->id);
        }
        if (nk_button_label(ctx, "Remove")) {
            vapor_gui_start_uninstall(app, e->id);
        }
    } else {
        nk_layout_row_dynamic(ctx, ROW_H, 1);
        if (nk_button_label(ctx, "Install")) {
            vapor_gui_start_install(app, e->id, e->latest_version);
        }
    }
}

void
vapor_gui_library_screen(struct nk_context *ctx, vapor_app *app, int w, int h)
{
    int   busy = vapor_gui_job_busy(app);
    float hdr = header_height(app, busy);
    size_t i;
    int    shown = 0, per_row;

    if (nk_begin(ctx, "header", nk_rect(0, 0, (float)w, hdr),
                 NK_WINDOW_NO_SCROLLBAR)) {
        draw_toolbar(ctx, app, busy);
        if (app->show_settings) {
            draw_settings(ctx, app, busy);
        }
        draw_notice(ctx, app);
        if (busy) {
            draw_progress(ctx, app);
        }
    }
    nk_end(ctx);

    if (nk_begin(ctx, "grid", nk_rect(0, hdr, (float)w, (float)h - hdr), 0)) {
        int sel = find_game(app, app->selected_id);

        if (app->selected_id[0] && sel < 0 && app->catalog_loaded) {
            app->selected_id[0] = '\0';
        }

        if (sel >= 0) {
            draw_detail(ctx, app, (size_t)sel, busy);
        } else {
            /* Measured from the panel rather than the window, so the padding and
             * the scrollbar are already accounted for. */
            float avail = nk_window_get_content_region_size(ctx).x;

            per_row = (int)(avail / CARD_W);
            if (per_row < 1) {
                per_row = 1;
            }

            for (i = 0; i < app->ngames; i++) {
                if (!matches_search(app, &app->games[i])) {
                    continue;
                }
                if (shown % per_row == 0) {
                    /* Cards share the row evenly instead of leaving a ragged gap
                     * on the right. */
                    int item_w =
                        (int)((avail - GAP * (float)(per_row - 1)) / (float)per_row);
                    nk_layout_row_static(ctx, CARD_H, item_w, per_row);
                }
                draw_card(ctx, app, i, busy);
                shown++;
            }

            if (shown == 0) {
                nk_layout_row_dynamic(ctx, 24, 1);
                nk_label_colored(ctx,
                                 !app->catalog_loaded ? "Loading the catalog..."
                                 : app->ngames == 0
                                     ? "The catalog is empty. Publish a game with "
                                       "vapor-admin on the server."
                                     : "Nothing matches that search.",
                                 NK_TEXT_LEFT, COL_MUTED_V);
            }
        }
    }
    nk_end(ctx);
}
