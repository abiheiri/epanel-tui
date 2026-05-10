#include "ui.h"
#include <locale.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

#define COLOR_PAIR_DEFAULT   1
#define COLOR_PAIR_ACTIVE    2
#define COLOR_PAIR_CURSOR    3
#define COLOR_PAIR_SELECTED  4
#define COLOR_PAIR_POPUP     5
#define COLOR_PAIR_CHROME    6  /* white on terminal bg, for tabs/footer/header */

static int g_scroll_offset = 0;

void ui_init(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    /* ncurses waits ESCDELAY ms after an ESC byte to see if an escape sequence
       follows (arrow keys, F-keys). Default is 1000 ms, which makes standalone
       Esc feel sluggish. 25 ms is plenty of time for a terminal to deliver the
       rest of a multi-byte sequence. */
    ESCDELAY = 25;
    if (curs_set(2) == ERR) curs_set(1);
    if (has_colors()) {
        start_color();
        /* Let -1 mean "use the terminal's ambient color" for fg or bg, so the
           app's default backdrop matches the user's terminal theme instead of
           being hardcoded white-on-black. */
        use_default_colors();
        init_pair(COLOR_PAIR_DEFAULT,  -1,            -1);
        init_pair(COLOR_PAIR_ACTIVE,   COLOR_YELLOW,  -1);
        init_pair(COLOR_PAIR_CURSOR,   COLOR_BLACK,   COLOR_YELLOW);
        init_pair(COLOR_PAIR_SELECTED, COLOR_WHITE,   COLOR_BLUE);
        init_pair(COLOR_PAIR_POPUP,    COLOR_WHITE,   COLOR_BLUE);
        init_pair(COLOR_PAIR_CHROME,   COLOR_WHITE,   -1);
        bkgd(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    }
}

void ui_shutdown(void) {
    endwin();
}

void ui_get_size(int *rows, int *cols) {
    getmaxyx(stdscr, *rows, *cols);
}

static void draw_box_title(int y, int x, int h, int w, const char *title) {
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    for (int i = 1; i < w - 1; i++) {
        mvaddch(y, x + i, ACS_HLINE);
        mvaddch(y + h - 1, x + i, ACS_HLINE);
    }
    for (int i = 1; i < h - 1; i++) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + w - 1, ACS_VLINE);
    }
    if (title) {
        int len = (int)strlen(title);
        int tx = x + 2;
        if (tx + len < x + w - 1) {
            mvaddstr(y, tx, title);
        }
    }
}

static void draw_tabs(const App *app, int y, int x, int h, int w) {
    (void)h;
    const char *labels[] = {" Links ", " Notes ", " Settings "};
    attrset(COLOR_PAIR(COLOR_PAIR_CHROME));
    draw_box_title(y, x, 3, w, NULL);
    int tx = x + 2;
    for (int i = 0; i < 3; i++) {
        if (i == (int)app->current_tab) {
            attrset(COLOR_PAIR(COLOR_PAIR_ACTIVE) | A_BOLD);
        } else {
            attrset(COLOR_PAIR(COLOR_PAIR_CHROME));
        }
        mvaddstr(y + 1, tx, labels[i]);
        tx += (int)strlen(labels[i]) + 1;
    }
    attrset(COLOR_PAIR(COLOR_PAIR_CHROME));
    /* Software name and version, right-aligned */
    char title[64];
    snprintf(title, sizeof(title), " epanel %s -- Tabs (F1/F2/F3) ", VERSION);
    mvaddstr(y, x + w - (int)strlen(title) - 2, title);
    attrset(A_NORMAL);
}

static void draw_links(const App *app, int y, int x, int h, int w) {
    int input_h = 3;
    int list_y = y + input_h;
    int list_h = h - input_h;

    /* Search input */
    int border_color = (app->focus == FOCUS_SEARCH) ? COLOR_PAIR_ACTIVE : COLOR_PAIR_DEFAULT;
    attrset(COLOR_PAIR(border_color));
    draw_box_title(y, x, input_h, w, " Search / Add ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    if (app->search_input) {
        mvaddnstr(y + 1, x + 2, app->search_input, w - 4);
    }

    /* Links list */
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    draw_box_title(list_y, x, list_h, w, " Links ");

    int visible_rows = list_h - 2;
    if (visible_rows < 1) return;

    /* Simple scroll: ensure cursor is visible */
    g_scroll_offset = 0;
    if (app->links_cursor >= 0) {
        if (app->links_cursor < g_scroll_offset) g_scroll_offset = (int)app->links_cursor;
        if (app->links_cursor >= g_scroll_offset + visible_rows) g_scroll_offset = (int)app->links_cursor - visible_rows + 1;
    }
    if (g_scroll_offset < 0) g_scroll_offset = 0;

    for (int row = 0; row < visible_rows; row++) {
        size_t idx = (size_t)(g_scroll_offset + row);
        if (idx >= app->flat_count) break;
        const FlatItem *item = &app->flat_items[idx];
        int cy = list_y + 1 + row;
        int is_cursor = (app->links_cursor == (ssize_t)idx) && (app->focus == FOCUS_LINKS_LIST);
        int is_selected = idset_contains(&app->selected_item_ids, item->id);

        if (is_cursor) {
            attrset(COLOR_PAIR(COLOR_PAIR_CURSOR));
        } else if (is_selected) {
            attrset(COLOR_PAIR(COLOR_PAIR_SELECTED));
        } else {
            attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
        }

        char line[1024];
        char indent[33];
        size_t indent_n = (item->depth < 16 ? item->depth : 16) * 2;
        memset(indent, ' ', indent_n);
        indent[indent_n] = '\0';
        const char *icon = (item->kind == ITEM_FOLDER) ? (item->is_collapsed ? "> " : "v ") : "* ";
        snprintf(line, sizeof(line), "%s%s%s", indent, icon, item->name ? item->name : "");
        mvaddnstr(cy, x + 2, line, w - 4);
        clrtoeol();
    }
    /* Clear remaining rows */
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    for (int row = (int)app->flat_count - g_scroll_offset; row < visible_rows; row++) {
        int cy = list_y + 1 + row;
        if (cy < list_y + list_h - 1) {
            move(cy, x + 2);
            clrtoeol();
        }
    }
}

static void draw_notes(const App *app, int y, int x, int h, int w) {
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    draw_box_title(y, x, h, w, " Notes ");
    if (app->notes_text) {
        int row = 0;
        const char *p = app->notes_text;
        while (*p && row < h - 2) {
            char line[1024];
            int i = 0;
            while (*p && *p != '\n' && i < w - 4 && i < 1023) {
                line[i++] = *p++;
            }
            line[i] = '\0';
            if (*p == '\n') p++;
            mvaddstr(y + 1 + row, x + 2, line);
            row++;
        }
    }
}

static void draw_settings(const App *app, int y, int x, int h, int w) {
    (void)h;
    int row = y;
    int field_h = 3;

    /* Links path */
    int bc1 = (app->focus == FOCUS_SETTINGS_LINKS) ? COLOR_PAIR_ACTIVE : COLOR_PAIR_DEFAULT;
    attrset(COLOR_PAIR(bc1));
    draw_box_title(row, x, field_h, w, " Links Save Path ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    if (app->settings_links_path) {
        mvaddnstr(row + 1, x + 2, app->settings_links_path, w - 4);
    }
    row += field_h;

    /* Notes path */
    int bc2 = (app->focus == FOCUS_SETTINGS_NOTES) ? COLOR_PAIR_ACTIVE : COLOR_PAIR_DEFAULT;
    attrset(COLOR_PAIR(bc2));
    draw_box_title(row, x, field_h, w, " Notes Save Path ");
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    if (app->settings_notes_path) {
        mvaddnstr(row + 1, x + 2, app->settings_notes_path, w - 4);
    }
    row += field_h;

#ifdef __APPLE__
    /* Safari sync */
    int bc3 = (app->focus == FOCUS_SETTINGS_SAFARI) ? COLOR_PAIR_ACTIVE : COLOR_PAIR_DEFAULT;
    attrset(COLOR_PAIR(bc3));
    draw_box_title(row, x, field_h, w, NULL);
    const char *sync_label = app->safari_sync_enabled ? "[X] Safari Sync" : "[ ] Safari Sync";
    if (app->focus == FOCUS_SETTINGS_SAFARI) {
        attrset(COLOR_PAIR(COLOR_PAIR_CURSOR));
    } else {
        attrset(COLOR_PAIR(COLOR_PAIR_SELECTED));
    }
    int label_x = x + (w - (int)strlen(sync_label)) / 2;
    mvaddstr(row + 1, label_x, sync_label);
    row += field_h;
#endif

    /* Save button */
    int bc4 = (app->focus == FOCUS_SETTINGS_SAVE) ? COLOR_PAIR_ACTIVE : COLOR_PAIR_DEFAULT;
    attrset(COLOR_PAIR(bc4));
    draw_box_title(row, x, field_h, w, NULL);
    const char *btn = " Save Settings ";
    if (app->focus == FOCUS_SETTINGS_SAVE) {
        attrset(COLOR_PAIR(COLOR_PAIR_CURSOR));
    } else {
        attrset(COLOR_PAIR(COLOR_PAIR_SELECTED));
    }
    int btn_x = x + (w - (int)strlen(btn)) / 2;
    mvaddstr(row + 1, btn_x, btn);
    row += field_h;

    /* Hint */
    attrset(COLOR_PAIR(COLOR_PAIR_DEFAULT));
    mvaddstr(row, x + 2, "Tab to navigate * Enter to save * Ctrl+C to quit");
}

static void draw_status(const App *app, int y, int x, int w) {
    const char *hints;
    switch (app->current_tab) {
    case TAB_LINKS:
        hints = "Up/Down navigate * Enter open/add * Space select * n new * d delete * m move * r rename * e export * i import * ? help * Esc clear search * F1/F2/F3 tabs * Ctrl+C quit";
        break;
    case TAB_NOTES:
        hints = "Type to edit * ? help * F1/F2/F3 tabs * Ctrl+C quit";
        break;
    case TAB_SETTINGS:
        hints = "Tab navigate * Enter save * ? help * F1/F2/F3 tabs * Ctrl+C quit";
        break;
    default:
        hints = "";
    }
    attrset(COLOR_PAIR(COLOR_PAIR_CHROME));
    char buf[2560];
    int n = 0;
#ifdef __APPLE__
    if (app->safari_sync_enabled) {
        n += snprintf(buf + n, sizeof(buf) - n, "Synced to Safari | ");
    }
#endif
    if (app->message) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s | ", app->message);
    }
    snprintf(buf + n, sizeof(buf) - n, "%s", hints);
    mvaddnstr(y, x, buf, w - 1);
    clrtoeol();
    attrset(A_NORMAL);
}

static void draw_box_stdscr(int y, int x, int h, int w, const char *title) {
    attrset(COLOR_PAIR(COLOR_PAIR_POPUP));
    for (int i = 0; i < h; i++) {
        mvhline(y + i, x, ' ', w);
    }
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    for (int i = 1; i < w - 1; i++) {
        mvaddch(y, x + i, ACS_HLINE);
        mvaddch(y + h - 1, x + i, ACS_HLINE);
    }
    for (int i = 1; i < h - 1; i++) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + w - 1, ACS_VLINE);
    }
    if (title) mvaddnstr(y, x + 2, title, w - 4);
    attrset(A_NORMAL);
}

static void popup_attron(void) {
    attrset(COLOR_PAIR(COLOR_PAIR_POPUP));
}

static void popup_attroff(void) {
    attrset(A_NORMAL);
}

static void draw_help_popup(const App *app, int y, int x, int h, int w) {
    (void)app;
    draw_box_stdscr(y, x, h, w, " Help ");
    popup_attron();
    mvaddstr(y + 1, x + 2, "Help");
    mvaddstr(y + 2, x + 2, "====");
    mvaddstr(y + 3, x + 2, "F1        Links tab");
    mvaddstr(y + 4, x + 2, "F2        Notes tab");
    mvaddstr(y + 5, x + 2, "F3        Settings tab");
    mvaddstr(y + 6, x + 2, "?         Show this help");
    mvaddstr(y + 7, x + 2, "Esc       Clear search / close popup");
    mvaddstr(y + 8, x + 2, "Ctrl+C    Quit");
    mvaddstr(y + 9, x + 2, "");
    mvaddstr(y + 10, x + 2, "Links:");
    mvaddstr(y + 11, x + 2, "Up/Down   Navigate");
    mvaddstr(y + 12, x + 2, "Enter     Open folder or add entry");
    mvaddstr(y + 13, x + 2, "Space     Select item");
    mvaddstr(y + 14, x + 2, "n         New folder");
    mvaddstr(y + 15, x + 2, "d         Delete selected");
    mvaddstr(y + 16, x + 2, "m         Move selected");
    mvaddstr(y + 17, x + 2, "r         Rename folder");
    mvaddstr(y + 18, x + 2, "e         Export JSON");
    mvaddstr(y + 19, x + 2, "i         Import JSON");
    mvaddstr(y + 20, x + 2, "Left/Right Collapse/expand folder");
    mvaddstr(y + 21, x + 2, "Enter/Esc/Space dismiss");
    popup_attroff();
}

static void draw_confirm_delete(const App *app, int y, int x, int h, int w) {
    draw_box_stdscr(y, x, h, w, NULL);
    popup_attron();
    mvaddstr(y + 1, x + 2, "Delete selected items?");
    mvaddstr(y + 2, x + 2, "");
    char buf[64];
    snprintf(buf, sizeof(buf), "  Folders:       %zu", app->popup.folder_count);
    mvaddstr(y + 3, x + 2, buf);
    snprintf(buf, sizeof(buf), "  Entries:       %zu", app->popup.entry_count);
    mvaddstr(y + 4, x + 2, buf);
    if (app->popup.folder_count > 0) {
        mvaddstr(y + 5, x + 2, "This will delete all contained items.");
    }
    mvaddstr(y + h - 2, x + 2, "y/Enter confirm * n/Esc cancel");
    popup_attroff();
}

static int count_lines(const char *s) {
    int n = 1;
    while (*s) {
        if (*s == '\n') n++;
        s++;
    }
    return n;
}

static void draw_simple_popup(const char *title, const char *body, const char *hint, int y, int x, int h, int w) {
    draw_box_stdscr(y, x, h, w, title);
    popup_attron();
    int row = y + 1;
    const char *p = body;
    while (*p && row < y + h - 2) {
        char line[1024];
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof(line) - 1) {
            line[i++] = *p++;
        }
        line[i] = '\0';
        if (*p == '\n') p++;
        mvaddnstr(row, x + 2, line, w - 4);
        row++;
    }
    if (hint) mvaddstr(y + h - 2, x + 2, hint);
    popup_attroff();
}

static void draw_popup(const App *app, int rows, int cols) {
    int pw = cols * 60 / 100;
    int ph = rows * 50 / 100;
    if (pw < 40) pw = 40;
    if (ph < 10) ph = 10;

    /* Compact popups for single-line text input */
    if (app->popup.type == POPUP_EXPORT_JSON || app->popup.type == POPUP_IMPORT_JSON ||
        app->popup.type == POPUP_RENAME_FOLDER) {
        ph = 5;
        if (ph > rows - 2) ph = rows - 2;
        if (ph < 5) ph = 5;
    } else if (app->popup.type == POPUP_ALERT) {
        int lines = app->popup.text ? count_lines(app->popup.text) : 1;
        ph = lines + 4;  /* top border + body + hint + bottom border + padding */
        if (ph > rows - 2) ph = rows - 2;
        if (ph < 5) ph = 5;
    }

    int px = (cols - pw) / 2;
    int py = (rows - ph) / 2;

    switch (app->popup.type) {
    case POPUP_HELP:
        draw_help_popup(app, py, px, ph, pw);
        break;
    case POPUP_ALERT:
        draw_simple_popup(" Alert ", app->popup.text ? app->popup.text : "",
                         "Enter/Esc/Space dismiss", py, px, ph, pw);
        break;
    case POPUP_CONFIRM_DELETE:
        draw_confirm_delete(app, py, px, ph, pw);
        break;
    case POPUP_ADD_ENTRY:
    case POPUP_NEW_FOLDER:
    case POPUP_RENAME_FOLDER:
    case POPUP_MOVE_ITEM:
    case POPUP_EXPORT_JSON:
    case POPUP_IMPORT_JSON:
    case POPUP_CONFIRM_IMPORT_JSON: {
        const char *title = "";
        const char *hint = "Enter confirm * Esc cancel";
        switch (app->popup.type) {
        case POPUP_ADD_ENTRY: title = " Add Entry "; hint = "Up/Down select * Enter confirm * Esc cancel"; break;
        case POPUP_NEW_FOLDER: title = " New Folder "; hint = "Up/Down select * Enter confirm * Esc cancel"; break;
        case POPUP_RENAME_FOLDER: title = " Rename Folder "; break;
        case POPUP_MOVE_ITEM: title = " Move To "; hint = "Up/Down select * Enter confirm * Esc cancel"; break;
        case POPUP_EXPORT_JSON: title = " Export JSON Path "; break;
        case POPUP_IMPORT_JSON: title = " Import JSON Path "; break;
        case POPUP_CONFIRM_IMPORT_JSON:
            title = " Confirm Import ";
            hint = "y/Enter confirm * n/Esc cancel";
            break;
        default: break;
        }
        draw_box_stdscr(py, px, ph, pw, title);
        popup_attron();
        if (app->popup.type == POPUP_CONFIRM_IMPORT_JSON) {
            mvaddstr(py + 1, px + 2, "Replace All Data?");
            mvaddstr(py + 2, px + 2, "");
            mvaddnstr(py + 3, px + 2, app->popup.path ? app->popup.path : "", pw - 4);
            mvaddstr(py + 4, px + 2, "");
            mvaddstr(py + 5, px + 2, "This will replace all existing entries, folders, and notes.");
            mvaddstr(py + 6, px + 2, "This cannot be undone.");
        } else if (app->popup.type == POPUP_MOVE_ITEM || app->popup.type == POPUP_ADD_ENTRY || app->popup.type == POPUP_NEW_FOLDER) {
            /* Show list of folders */
            if (app->popup.type != POPUP_ADD_ENTRY) {
                mvaddnstr(py + 1, px + 2, app->popup.text ? app->popup.text : "", pw - 4);
            }
            const char *sel_id = NULL;
            if (app->popup.type == POPUP_ADD_ENTRY) sel_id = app->popup.selected_folder;
            else if (app->popup.type == POPUP_NEW_FOLDER) sel_id = app->popup.selected_parent;
            else sel_id = app->popup.selected_folder;
            const FolderChoice *choices = app->popup.choices;
            size_t fcount = app->popup.choice_count;
            int list_start = (app->popup.type == POPUP_ADD_ENTRY) ? 1 : 2;
            int list_h = ph - list_start - 2;
            if (list_h < 1) list_h = 1;
            for (int row = 0; row < list_h && (size_t)row < fcount; row++) {
                char line[1024];
                char indent[33];
                size_t indent_n = (choices[row].depth < 16 ? choices[row].depth : 16) * 2;
                memset(indent, ' ', indent_n);
                indent[indent_n] = '\0';
                snprintf(line, sizeof(line), "%s%s", indent, choices[row].name ? choices[row].name : "");
                if (sel_id && strcmp(choices[row].id, sel_id) == 0) {
                    attrset(COLOR_PAIR(COLOR_PAIR_CURSOR));
                    mvaddnstr(py + list_start + row, px + 2, line, pw - 4);
                    attrset(COLOR_PAIR(COLOR_PAIR_POPUP));
                } else {
                    mvaddnstr(py + list_start + row, px + 2, line, pw - 4);
                }
            }
        } else {
            const char *txt = app->popup.text ? app->popup.text :
                              (app->popup.path ? app->popup.path : "");
            mvaddnstr(py + 1, px + 2, txt, pw - 4);
        }
        mvaddstr(py + ph - 2, px + 2, hint);
        popup_attroff();
        break;
    }
    default:
        break;
    }
}

void ui_draw(const App *app) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < 10 || cols < 30) return;

    erase();

    int tab_h = 3;
    int status_h = 1;
    int content_y = 1 + tab_h;
    int content_h = rows - content_y - status_h - 1;
    int content_w = cols - 2;

    draw_tabs(app, 1, 1, tab_h, cols - 2);

    switch (app->current_tab) {
    case TAB_LINKS:
        draw_links(app, content_y, 1, content_h, content_w);
        break;
    case TAB_NOTES:
        draw_notes(app, content_y, 1, content_h, content_w);
        break;
    case TAB_SETTINGS:
        draw_settings(app, content_y, 1, content_h, content_w);
        break;
    }

    draw_status(app, rows - 1, 1, cols - 2);

    if (app->popup.type != POPUP_NONE) {
        draw_popup(app, rows, cols);
    }

    /* Place cursor in the focused input field */
    if (app->popup.type != POPUP_NONE) {
        int pw = cols * 60 / 100;
        int ph = rows * 50 / 100;
        if (pw < 40) pw = 40;
        if (ph < 10) ph = 10;
        if (app->popup.type == POPUP_EXPORT_JSON || app->popup.type == POPUP_IMPORT_JSON ||
            app->popup.type == POPUP_RENAME_FOLDER) {
            ph = 5;
            if (ph > rows - 2) ph = rows - 2;
            if (ph < 5) ph = 5;
        } else if (app->popup.type == POPUP_ALERT) {
            int lines = app->popup.text ? count_lines(app->popup.text) : 1;
            ph = lines + 4;
            if (ph > rows - 2) ph = rows - 2;
            if (ph < 5) ph = 5;
        }
        int px = (cols - pw) / 2;
        int py = (rows - ph) / 2;
        switch (app->popup.type) {
        case POPUP_NEW_FOLDER:
        case POPUP_RENAME_FOLDER:
            if (app->popup.text) {
                int len = (int)app->popup.cursor_pos;
                if (len > pw - 4) len = pw - 4;
                move(py + 1, px + 2 + len);
            }
            break;
        case POPUP_EXPORT_JSON:
        case POPUP_IMPORT_JSON:
            if (app->popup.path) {
                int len = (int)app->popup.cursor_pos;
                if (len > pw - 4) len = pw - 4;
                move(py + 1, px + 2 + len);
            }
            break;
        default:
            break;
        }
    } else {
        switch (app->focus) {
        case FOCUS_SEARCH:
            if (app->search_input) {
                move(content_y + 1, 3 + (int)strlen(app->search_input));
            }
            break;
        case FOCUS_LINKS_LIST:
            if (app->links_cursor >= 0) {
                move(content_y + 4 + (app->links_cursor - g_scroll_offset), 3);
            }
            break;
        case FOCUS_NOTES_TEXT:
            move(content_y + 1 + (int)app->notes_cursor_y, 3 + (int)app->notes_cursor_x);
            break;
        case FOCUS_SETTINGS_LINKS:
            if (app->settings_links_path) {
                move(content_y + 1, 3 + (int)strlen(app->settings_links_path));
            }
            break;
        case FOCUS_SETTINGS_NOTES:
            if (app->settings_notes_path) {
                move(content_y + 4, 3 + (int)strlen(app->settings_notes_path));
            }
            break;
        default:
            break;
        }
    }

    refresh();
}
