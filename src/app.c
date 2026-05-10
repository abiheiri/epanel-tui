#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "uuid.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ncurses.h>

#ifdef __APPLE__
#include "safari_sync.h"
#endif

/* -------------------------------------------------------------------------- */
/* String helpers                                                             */
/* -------------------------------------------------------------------------- */

static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *str_dup_len(const char *s, size_t len) {
    char *d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

static void str_append_char(char **s, char c) {
    size_t len = *s ? strlen(*s) : 0;
    char *n = realloc(*s, len + 2);
    if (n) {
        n[len] = c;
        n[len + 1] = '\0';
        *s = n;
    }
}

static void str_pop_char(char **s) {
    if (!*s) return;
    size_t len = strlen(*s);
    if (len > 0) (*s)[len - 1] = '\0';
}

static void str_insert_char(char **s, size_t pos, char c) {
    size_t len = *s ? strlen(*s) : 0;
    if (pos > len) pos = len;
    char *n = realloc(*s, len + 2);
    if (n) {
        memmove(n + pos + 1, n + pos, len - pos + 1);
        n[pos] = c;
        *s = n;
    }
}

static void str_delete_before(char **s, size_t pos) {
    if (!*s || pos == 0) return;
    size_t len = strlen(*s);
    if (pos > len) pos = len;
    memmove(*s + pos - 1, *s + pos, len - pos + 1);
}

#ifdef __APPLE__
void app_move_existing_to_original(App *app);
#endif

static int str_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/* -------------------------------------------------------------------------- */
/* Expand tilde                                                               */
/* -------------------------------------------------------------------------- */

char *expand_tilde(const char *path) {
    if (!path) return NULL;
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            size_t hlen = strlen(home);
            size_t plen = strlen(path + 1);
            char *out = malloc(hlen + plen + 1);
            if (out) {
                memcpy(out, home, hlen);
                memcpy(out + hlen, path + 1, plen + 1);
                return out;
            }
        }
    }
    return str_dup(path);
}

char *config_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) {
        size_t n = strlen(xdg);
        char *p = malloc(n + 9);
        if (p) {
            memcpy(p, xdg, n);
            memcpy(p + n, "/epanel", 8);
            return p;
        }
    }
    const char *home = getenv("HOME");
    if (home) {
        size_t n = strlen(home);
        char *p = malloc(n + 17);
        if (p) {
            memcpy(p, home, n);
            memcpy(p + n, "/.config/epanel", 16);
            return p;
        }
    }
    return str_dup(".");
}

char *current_iso_datetime(void) {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char *buf = malloc(32);
    if (buf) {
        strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", tm);
    }
    return buf;
}

/* -------------------------------------------------------------------------- */
/* Folder / entry helpers                                                     */
/* -------------------------------------------------------------------------- */

static void entry_free(Entry *e) {
    free(e->text);
    free(e->date);
}

void folder_free(Folder *f) {
    free(f->name);
    for (size_t i = 0; i < f->entry_count; i++) entry_free(&f->entries[i]);
    free(f->entries);
    for (size_t i = 0; i < f->subfolder_count; i++) folder_free(&f->subfolders[i]);
    free(f->subfolders);
}

Folder *find_folder(Folder *root, const char *id) {
    if (str_eq(root->id, id)) return root;
    for (size_t i = 0; i < root->subfolder_count; i++) {
        Folder *found = find_folder(&root->subfolders[i], id);
        if (found) return found;
    }
    return NULL;
}

Entry *find_entry(Folder *root, const char *id) {
    for (size_t i = 0; i < root->entry_count; i++) {
        if (str_eq(root->entries[i].id, id)) return &root->entries[i];
    }
    for (size_t i = 0; i < root->subfolder_count; i++) {
        Entry *found = find_entry(&root->subfolders[i], id);
        if (found) return found;
    }
    return NULL;
}

static Entry *find_entry_with_parent(Folder *root, const char *id, Folder **out_parent) {
    for (size_t i = 0; i < root->entry_count; i++) {
        if (str_eq(root->entries[i].id, id)) {
            *out_parent = root;
            return &root->entries[i];
        }
    }
    for (size_t i = 0; i < root->subfolder_count; i++) {
        Entry *found = find_entry_with_parent(&root->subfolders[i], id, out_parent);
        if (found) return found;
    }
    return NULL;
}

void folder_add_entry(Folder *f, const char *text) {
    if (f->entry_count >= f->entry_cap) {
        f->entry_cap = f->entry_cap ? f->entry_cap * 2 : 4;
        f->entries = realloc(f->entries, f->entry_cap * sizeof(Entry));
    }
    Entry *e = &f->entries[f->entry_count++];
    memset(e, 0, sizeof(*e));
    uuid_gen(e->id);
    e->text = str_dup(text);
    e->date = current_iso_datetime();
}

void folder_add_subfolder(Folder *f, const char *name) {
    if (f->subfolder_count >= f->subfolder_cap) {
        f->subfolder_cap = f->subfolder_cap ? f->subfolder_cap * 2 : 4;
        f->subfolders = realloc(f->subfolders, f->subfolder_cap * sizeof(Folder));
    }
    Folder *sub = &f->subfolders[f->subfolder_count++];
    memset(sub, 0, sizeof(*sub));
    uuid_gen(sub->id);
    sub->name = str_dup(name);
}

static void folder_remove_entry(Folder *f, const char *id) {
    for (size_t i = 0; i < f->entry_count; i++) {
        if (str_eq(f->entries[i].id, id)) {
            entry_free(&f->entries[i]);
            memmove(&f->entries[i], &f->entries[i + 1],
                    (f->entry_count - i - 1) * sizeof(Entry));
            f->entry_count--;
            return;
        }
    }
}

static void folder_remove_subfolder(Folder *f, const char *id) {
    for (size_t i = 0; i < f->subfolder_count; i++) {
        if (str_eq(f->subfolders[i].id, id)) {
            folder_free(&f->subfolders[i]);
            memmove(&f->subfolders[i], &f->subfolders[i + 1],
                    (f->subfolder_count - i - 1) * sizeof(Folder));
            f->subfolder_count--;
            return;
        }
    }
}

void delete_folder_recursive(Folder *root, const char *id) {
    folder_remove_subfolder(root, id);
    for (size_t i = 0; i < root->subfolder_count; i++) {
        delete_folder_recursive(&root->subfolders[i], id);
    }
}

void delete_entry_recursive(Folder *root, const char *id) {
    folder_remove_entry(root, id);
    for (size_t i = 0; i < root->subfolder_count; i++) {
        delete_entry_recursive(&root->subfolders[i], id);
    }
}

void move_folder(Folder *root, const char *item_id, const char *target_id) {
    Folder *src_parent = NULL;
    size_t src_idx = 0;
    for (size_t i = 0; i < root->subfolder_count; i++) {
        if (str_eq(root->subfolders[i].id, item_id)) {
            src_parent = root;
            src_idx = i;
            break;
        }
    }
    if (!src_parent) return;
    Folder *target = find_folder(root, target_id);
    if (!target) return;
    if (target->subfolder_count >= target->subfolder_cap) {
        target->subfolder_cap = target->subfolder_cap ? target->subfolder_cap * 2 : 4;
        target->subfolders = realloc(target->subfolders, target->subfolder_cap * sizeof(Folder));
    }
    target->subfolders[target->subfolder_count++] = src_parent->subfolders[src_idx];
    memmove(&src_parent->subfolders[src_idx], &src_parent->subfolders[src_idx + 1],
            (src_parent->subfolder_count - src_idx - 1) * sizeof(Folder));
    src_parent->subfolder_count--;
}

void move_entry(Folder *root, const char *item_id, const char *target_id) {
    Folder *src = NULL;
    Entry *e = find_entry_with_parent(root, item_id, &src);
    if (!e || !src) return;
    Folder *target = find_folder(root, target_id);
    if (!target || src == target) return;
    /* Shallow-copy into target (shares text/date pointers with the original slot),
       then drop the original slot from src without freeing its contents — target
       now owns them. `src != target`, so growing target->entries cannot disturb
       src->entries, and `e` remains a valid index into src->entries. */
    size_t idx = (size_t)(e - src->entries);
    Entry tmp = *e;
    if (target->entry_count >= target->entry_cap) {
        target->entry_cap = target->entry_cap ? target->entry_cap * 2 : 4;
        target->entries = realloc(target->entries, target->entry_cap * sizeof(Entry));
    }
    target->entries[target->entry_count++] = tmp;
    memmove(&src->entries[idx], &src->entries[idx + 1],
            (src->entry_count - idx - 1) * sizeof(Entry));
    src->entry_count--;
}

/* -------------------------------------------------------------------------- */
/* IdSet                                                                      */
/* -------------------------------------------------------------------------- */

int idset_contains(const IdSet *set, const char *id) {
    for (size_t i = 0; i < set->count; i++) {
        if (str_eq(set->ids[i], id)) return 1;
    }
    return 0;
}

void idset_add(IdSet *set, const char *id) {
    if (idset_contains(set, id)) return;
    if (set->count >= set->cap) {
        set->cap = set->cap ? set->cap * 2 : 4;
        set->ids = realloc(set->ids, set->cap * sizeof(char *));
    }
    set->ids[set->count++] = str_dup(id);
}

void idset_remove(IdSet *set, const char *id) {
    for (size_t i = 0; i < set->count; i++) {
        if (str_eq(set->ids[i], id)) {
            free(set->ids[i]);
            memmove(&set->ids[i], &set->ids[i + 1],
                    (set->count - i - 1) * sizeof(char *));
            set->count--;
            return;
        }
    }
}

void idset_clear(IdSet *set) {
    for (size_t i = 0; i < set->count; i++) free(set->ids[i]);
    free(set->ids);
    set->ids = NULL;
    set->count = 0;
    set->cap = 0;
}

/* -------------------------------------------------------------------------- */
/* Popup                                                                      */
/* -------------------------------------------------------------------------- */

void popup_clear(Popup *p) {
    free(p->text);
    free(p->selected_folder);
    free(p->selected_parent);
    free(p->folder_id);
    free(p->item_id);
    free(p->path);
    for (size_t i = 0; i < p->choice_count; i++) free(p->choices[i].name);
    free(p->choices);
    memset(p, 0, sizeof(*p));
    p->type = POPUP_NONE;
}

void popup_build_choices(Popup *p, const App *app, const char *exclude_id) {
    for (size_t i = 0; i < p->choice_count; i++) free(p->choices[i].name);
    free(p->choices);
    p->choices = app_get_folder_choices(app, exclude_id, &p->choice_count);
}

/* Advance the `sel` UUID up or down through p->choices. `sel` is a pointer to
   a Popup field holding the currently-selected UUID; it's freed and replaced
   with a new str_dup of the neighboring choice's id. No-op if the cache is
   empty or the current id isn't in the cache. */
static void popup_navigate_choices(Popup *p, int ch, char **sel) {
    if (!p->choices || p->choice_count == 0 || !*sel) return;
    ssize_t idx = -1;
    for (size_t i = 0; i < p->choice_count; i++) {
        if (str_eq(p->choices[i].id, *sel)) { idx = (ssize_t)i; break; }
    }
    if (idx < 0) return;
    if (ch == KEY_UP) idx = idx > 0 ? idx - 1 : 0;
    else idx = (size_t)(idx + 1) < p->choice_count ? idx + 1 : (ssize_t)p->choice_count - 1;
    free(*sel);
    *sel = str_dup(p->choices[idx].id);
}

void popup_set_alert(Popup *p, const char *msg) {
    popup_clear(p);
    p->type = POPUP_ALERT;
    p->text = str_dup(msg);
    p->alert_action = ALERT_ACTION_NONE;
}

/* -------------------------------------------------------------------------- */
/* Flat items                                                                 */
/* -------------------------------------------------------------------------- */

static int entry_matches_search(const char *text, const char *search) {
    const char *p = text;
    const char *q = search;
    while (*p) {
        if (tolower((unsigned char)*p) == tolower((unsigned char)*q)) {
            if (!*++q) return 1;
        } else {
            q = search;
        }
        p++;
    }
    return 0;
}

static int folder_has_search_match(const Folder *f, const char *search) {
    if (!search || search[0] == '\0') return 1;
    if (f->name && entry_matches_search(f->name, search)) return 1;
    for (size_t i = 0; i < f->entry_count; i++) {
        if (f->entries[i].text && entry_matches_search(f->entries[i].text, search))
            return 1;
    }
    for (size_t i = 0; i < f->subfolder_count; i++) {
        if (folder_has_search_match(&f->subfolders[i], search)) return 1;
    }
    return 0;
}

static void rebuild_flat(Folder *f, size_t depth, FlatItem **out, size_t *count, size_t *cap, const IdSet *expanded, const char *search) {
    int is_searching = search && search[0] != '\0';
    for (size_t i = 0; i < f->subfolder_count; i++) {
        Folder *sub = &f->subfolders[i];
        if (is_searching && !folder_has_search_match(sub, search)) continue;
        if (*count >= *cap) {
            size_t old_cap = *cap;
            *cap = *cap ? *cap * 2 : 16;
            FlatItem *new_out = realloc(*out, *cap * sizeof(FlatItem));
            if (!new_out) return;
            memset(new_out + old_cap, 0, (*cap - old_cap) * sizeof(FlatItem));
            *out = new_out;
        }
        FlatItem *item = &(*out)[(*count)++];
        memcpy(item->id, sub->id, UUID_STR_LEN + 1);
        item->kind = ITEM_FOLDER;
        item->depth = depth;
        free(item->name);
        item->name = str_dup(sub->name);
        item->is_collapsed = is_searching ? 0 : sub->is_collapsed;

        if (is_searching || !sub->is_collapsed) {
            rebuild_flat(sub, depth + 1, out, count, cap, expanded, search);
        }
    }
    for (size_t i = 0; i < f->entry_count; i++) {
        Entry *e = &f->entries[i];
        int match = 1;
        if (is_searching) {
            match = e->text && entry_matches_search(e->text, search);
        }
        if (!match) continue;
        if (*count >= *cap) {
            size_t old_cap = *cap;
            *cap = *cap ? *cap * 2 : 16;
            FlatItem *new_out = realloc(*out, *cap * sizeof(FlatItem));
            if (!new_out) return;
            memset(new_out + old_cap, 0, (*cap - old_cap) * sizeof(FlatItem));
            *out = new_out;
        }
        FlatItem *item = &(*out)[(*count)++];
        memcpy(item->id, e->id, UUID_STR_LEN + 1);
        item->kind = ITEM_ENTRY;
        item->depth = depth;
        free(item->name);
        item->name = str_dup(e->text);
        item->is_collapsed = 0;
    }
}

void app_rebuild_flat_items(App *app) {
    for (size_t i = 0; i < app->flat_count; i++) {
        free(app->flat_items[i].name);
        app->flat_items[i].name = NULL;
    }
    app->flat_count = 0;
    rebuild_flat(&app->data.root_folder, 0,
                 &app->flat_items, &app->flat_count, &app->flat_cap,
                 &app->search_expanded_folders, app->search_input);
    app->flat_dirty = 0;
}

/* -------------------------------------------------------------------------- */
/* JSON serialization                                                         */
/* -------------------------------------------------------------------------- */

static cJSON *entry_to_json(const Entry *e) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", e->id);
    cJSON_AddStringToObject(o, "text", e->text ? e->text : "");
    cJSON_AddStringToObject(o, "date", e->date ? e->date : "");
    return o;
}

static cJSON *folder_to_json(const Folder *f) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", f->id);
    cJSON_AddStringToObject(o, "name", f->name ? f->name : "");
    cJSON *entries = cJSON_CreateArray();
    for (size_t i = 0; i < f->entry_count; i++) {
        cJSON_AddItemToArray(entries, entry_to_json(&f->entries[i]));
    }
    cJSON_AddItemToObject(o, "entries", entries);
    cJSON *subfolders = cJSON_CreateArray();
    for (size_t i = 0; i < f->subfolder_count; i++) {
        cJSON_AddItemToArray(subfolders, folder_to_json(&f->subfolders[i]));
    }
    cJSON_AddItemToObject(o, "subfolders", subfolders);
    cJSON_AddBoolToObject(o, "isCollapsed", f->is_collapsed);
    return o;
}

static char *epanel_to_json(const EPanelData *data) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "rootFolder", folder_to_json(&data->root_folder));
    cJSON_AddStringToObject(root, "notes", data->notes ? data->notes : "");
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    return s;
}

static int entry_from_json(cJSON *o, Entry *e) {
    memset(e, 0, sizeof(*e));
    cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
    if (cJSON_IsString(id)) strncpy(e->id, id->valuestring, UUID_STR_LEN);
    if (e->id[0] == '\0') uuid_gen(e->id);
    cJSON *text = cJSON_GetObjectItemCaseSensitive(o, "text");
    if (cJSON_IsString(text)) e->text = str_dup(text->valuestring);
    cJSON *date = cJSON_GetObjectItemCaseSensitive(o, "date");
    if (cJSON_IsString(date)) e->date = str_dup(date->valuestring);
    return 1;
}

static int folder_from_json(cJSON *o, Folder *f);

static int folder_from_json(cJSON *o, Folder *f) {
    memset(f, 0, sizeof(*f));
    cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
    if (cJSON_IsString(id)) strncpy(f->id, id->valuestring, UUID_STR_LEN);
    if (f->id[0] == '\0') uuid_gen(f->id);
    cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
    if (cJSON_IsString(name)) f->name = str_dup(name->valuestring);
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(o, "entries");
    if (cJSON_IsArray(entries)) {
        size_t n = cJSON_GetArraySize(entries);
        for (size_t i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(entries, i);
            if (f->entry_count >= f->entry_cap) {
                f->entry_cap = f->entry_cap ? f->entry_cap * 2 : 4;
                f->entries = realloc(f->entries, f->entry_cap * sizeof(Entry));
            }
            if (entry_from_json(e, &f->entries[f->entry_count])) f->entry_count++;
        }
    }
    cJSON *subfolders = cJSON_GetObjectItemCaseSensitive(o, "subfolders");
    if (cJSON_IsArray(subfolders)) {
        size_t n = cJSON_GetArraySize(subfolders);
        for (size_t i = 0; i < n; i++) {
            cJSON *s = cJSON_GetArrayItem(subfolders, i);
            if (f->subfolder_count >= f->subfolder_cap) {
                f->subfolder_cap = f->subfolder_cap ? f->subfolder_cap * 2 : 4;
                f->subfolders = realloc(f->subfolders, f->subfolder_cap * sizeof(Folder));
            }
            if (folder_from_json(s, &f->subfolders[f->subfolder_count])) f->subfolder_count++;
        }
    }
    cJSON *collapsed = cJSON_GetObjectItemCaseSensitive(o, "isCollapsed");
    if (cJSON_IsBool(collapsed)) f->is_collapsed = cJSON_IsTrue(collapsed);
    return 1;
}

static int epanel_from_json(const char *json, EPanelData *data) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;
    cJSON *rf = cJSON_GetObjectItemCaseSensitive(root, "rootFolder");
    if (rf) folder_from_json(rf, &data->root_folder);
    cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "notes");
    if (cJSON_IsString(notes)) data->notes = str_dup(notes->valuestring);
    cJSON_Delete(root);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Settings / paths                                                           */
/* -------------------------------------------------------------------------- */

static char *data_file_path(const App *app) {
    char *expanded = expand_tilde(app->settings_links_path);
    size_t n = strlen(expanded);
    char *p = malloc(n + 13);
    if (p) {
        memcpy(p, expanded, n);
        memcpy(p + n, "/epanel.json", 13);
    }
    free(expanded);
    return p;
}

static char *notes_file_path(const App *app) {
    char *expanded = expand_tilde(app->settings_notes_path);
    size_t n = strlen(expanded);
    char *p = malloc(n + 11);
    if (p) {
        memcpy(p, expanded, n);
        memcpy(p + n, "/notes.txt", 11);
    }
    free(expanded);
    return p;
}

static char *settings_file_path(const App *app) {
    size_t n = strlen(app->config_dir);
    char *p = malloc(n + 15);
    if (p) {
        memcpy(p, app->config_dir, n);
        memcpy(p + n, "/settings.txt", 14);
    }
    return p;
}

static void ensure_dir(const char *path) {
    char *p = expand_tilde(path);
    struct stat st;
    if (stat(p, &st) != 0) {
        /* naive mkdir -p */
        char *tmp = str_dup(p);
        for (char *c = tmp + 1; *c; c++) {
            if (*c == '/') {
                *c = '\0';
                mkdir(tmp, 0755);
                *c = '/';
            }
        }
        mkdir(tmp, 0755);
        free(tmp);
    }
    free(p);
}

/* -------------------------------------------------------------------------- */
/* App init / free                                                            */
/* -------------------------------------------------------------------------- */

void app_init(App *app) {
    memset(app, 0, sizeof(*app));
    app->save_after.tv_sec = -1;
#ifdef __APPLE__
    app->safari_writeback_after.tv_sec = -1;
#endif
    app->current_tab = TAB_LINKS;
    app->focus = FOCUS_SEARCH;
    app->links_cursor = -1;

    uuid_gen(app->data.root_folder.id);
    app->data.root_folder.name = str_dup("/");
    app->data.root_folder.is_collapsed = 0;

    app->config_dir = config_path();
    app->settings_links_path = str_dup(app->config_dir);
    app->settings_notes_path = str_dup(app->config_dir);
    app->search_input = str_dup("");
    app->notes_text = str_dup("");

#ifdef __APPLE__
    app->safari_sync_enabled = 0;
    char *home = getenv("HOME");
    if (home) {
        size_t n = strlen(home);
        char *p = malloc(n + 34);
        if (p) {
            memcpy(p, home, n);
            memcpy(p + n, "/Library/Safari/Bookmarks.plist", 32);
            p[n + 32] = '\0';
            app->safari_sync_path = p;
        }
    }
    app->sync_pipe_write = -1;
#endif
    app->watch_pipe_write = -1;
}

void app_free(App *app) {
    folder_free(&app->data.root_folder);
    free(app->data.notes);
    free(app->search_input);
    idset_clear(&app->search_expanded_folders);
    idset_clear(&app->selected_item_ids);
    for (size_t i = 0; i < app->flat_count; i++) free(app->flat_items[i].name);
    free(app->flat_items);
    free(app->notes_text);
    free(app->settings_links_path);
    free(app->settings_notes_path);
    free(app->config_dir);
    popup_clear(&app->popup);
    free(app->message);
#ifdef __APPLE__
    free(app->safari_sync_path);
    free(app->last_sync_date);
    if (app->sync_pipe_write >= 0) close(app->sync_pipe_write);
    if (app->safari) safari_state_free(app->safari);
#endif
    if (app->watch_pipe_write >= 0) close(app->watch_pipe_write);
}

/* -------------------------------------------------------------------------- */
/* Load / save                                                                */
/* -------------------------------------------------------------------------- */

int app_load(App *app) {
    char *settings_path = settings_file_path(app);
    FILE *fp = fopen(settings_path, "r");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;
            while (*val == ' ' || *val == '\t') val++;
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r')) val[--vlen] = '\0';
            if (strcmp(key, "links_path") == 0) {
                free(app->settings_links_path);
                app->settings_links_path = str_dup(val);
            } else if (strcmp(key, "notes_path") == 0) {
                free(app->settings_notes_path);
                app->settings_notes_path = str_dup(val);
            }
#ifdef __APPLE__
            else if (strcmp(key, "safari_sync") == 0) {
                app->safari_sync_enabled = strcmp(val, "true") == 0;
            } else if (strcmp(key, "safari_sync_path") == 0) {
                free(app->safari_sync_path);
                app->safari_sync_path = str_dup(val);
            }
#endif
        }
        fclose(fp);
    }
    free(settings_path);

    char *expanded_links = expand_tilde(app->settings_links_path);
    if (stat(expanded_links, &(struct stat){0}) != 0) {
        free(app->settings_links_path);
        app->settings_links_path = str_dup(app->config_dir);
    }
    free(expanded_links);

    char *expanded_notes = expand_tilde(app->settings_notes_path);
    if (stat(expanded_notes, &(struct stat){0}) != 0) {
        free(app->settings_notes_path);
        app->settings_notes_path = str_dup(app->config_dir);
    }
    free(expanded_notes);

    char *data_path = data_file_path(app);
    fp = fopen(data_path, "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *buf = malloc(len + 1);
        if (buf && fread(buf, 1, len, fp) == (size_t)len) {
            buf[len] = '\0';
            EPanelData new_data = {0};
            if (epanel_from_json(buf, &new_data)) {
                folder_free(&app->data.root_folder);
                free(app->data.notes);
                app->data = new_data;
                if (strcmp(app->data.root_folder.id, "") == 0) {
                    uuid_gen(app->data.root_folder.id);
                }
                free(app->data.root_folder.name);
                app->data.root_folder.name = str_dup("/");
                app->data.root_folder.is_collapsed = 0;
                free(app->notes_text);
                app->notes_text = str_dup(app->data.notes ? app->data.notes : "");
            } else {
                folder_free(&new_data.root_folder);
                free(new_data.notes);
            }
            free(buf);
        }
        fclose(fp);
    }
    free(data_path);

    return 0;
}

int app_save(App *app) {
    ensure_dir(app->settings_links_path);
    ensure_dir(app->settings_notes_path);
    ensure_dir(app->config_dir);

    free(app->data.notes);
    app->data.notes = str_dup(app->notes_text ? app->notes_text : "");

    char *json = epanel_to_json(&app->data);
    if (!json) return -1;
    char *data_path = data_file_path(app);
    char *tmp_path = malloc(strlen(data_path) + 5);
    if (tmp_path) {
        strcpy(tmp_path, data_path);
        strcat(tmp_path, ".tmp");
        FILE *fp = fopen(tmp_path, "w");
        if (fp) {
            fputs(json, fp);
            fclose(fp);
            rename(tmp_path, data_path);
        }
        free(tmp_path);
    }
    free(data_path);
    free(json);

    /* Plain text notes mirror */
    char *notes_path = notes_file_path(app);
    FILE *fp = fopen(notes_path, "w");
    if (fp) {
        fputs(app->notes_text ? app->notes_text : "", fp);
        fclose(fp);
    }
    free(notes_path);

    /* Settings file */
    char *settings_path = settings_file_path(app);
    fp = fopen(settings_path, "w");
    if (fp) {
        fprintf(fp, "links_path=%s\n", app->settings_links_path);
        fprintf(fp, "notes_path=%s\n", app->settings_notes_path);
#ifdef __APPLE__
        fprintf(fp, "safari_sync=%s\n", app->safari_sync_enabled ? "true" : "false");
        fprintf(fp, "safari_sync_path=%s\n", app->safari_sync_path ? app->safari_sync_path : "");
        if (app->safari_sync_enabled) {
            char *expanded = expand_tilde(app->safari_sync_path);
            if (stat(expanded, &(struct stat){0}) == 0) {
                if (app_writeback_safari(app) == 0) {
                    app->safari_permission_warned = 0;
                    struct stat st;
                    if (stat(expanded, &st) == 0) {
                        app->last_safari_writeback = st.st_mtime;
                    }
                    app->safari_writeback_after.tv_sec = 0;
                    app->safari_writeback_after.tv_nsec = 0;
                }
            }
            free(expanded);
        }
#endif
        fclose(fp);
    }
    free(settings_path);

    clock_gettime(CLOCK_MONOTONIC, &app->last_save_time);
    return 0;
}

int app_reload(App *app) {
    char *data_path = data_file_path(app);
    struct stat st;
    if (stat(data_path, &st) != 0) {
        free(app->message);
        app->message = str_dup("Data file missing");
        free(data_path);
        return 0;
    }

    FILE *fp = fopen(data_path, "r");
    if (!fp) {
        free(data_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(fp);
        free(data_path);
        return -1;
    }
    if (fread(buf, 1, len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        free(data_path);
        return -1;
    }
    buf[len] = '\0';
    fclose(fp);

    EPanelData new_data = {0};
    if (!epanel_from_json(buf, &new_data)) {
        free(buf);
        popup_set_alert(&app->popup, "Reload failed: invalid JSON");
        free(data_path);
        return 0;
    }
    free(buf);

    app->save_after.tv_sec = 0;
    app->save_after.tv_nsec = 0;

    char *old_cursor_id = NULL;
    if (app->links_cursor >= 0 && (size_t)app->links_cursor < app->flat_count) {
        old_cursor_id = str_dup(app->flat_items[app->links_cursor].id);
    }

    IdSet old_selected = {0};
    for (size_t i = 0; i < app->selected_item_ids.count; i++) {
        idset_add(&old_selected, app->selected_item_ids.ids[i]);
    }

    folder_free(&app->data.root_folder);
    free(app->data.notes);
    app->data = new_data;
    if (strcmp(app->data.root_folder.id, "") == 0) {
        uuid_gen(app->data.root_folder.id);
    }
    free(app->data.root_folder.name);
    app->data.root_folder.name = str_dup("/");
    app->data.root_folder.is_collapsed = 0;
    free(app->notes_text);
    app->notes_text = str_dup(app->data.notes ? app->data.notes : "");

    app_rebuild_flat_items(app);

    if (old_cursor_id) {
        for (size_t i = 0; i < app->flat_count; i++) {
            if (str_eq(app->flat_items[i].id, old_cursor_id)) {
                app->links_cursor = (ssize_t)i;
                break;
            }
        }
        free(old_cursor_id);
    }

    idset_clear(&app->selected_item_ids);
    for (size_t i = 0; i < old_selected.count; i++) {
        int found = 0;
        for (size_t j = 0; j < app->flat_count; j++) {
            if (str_eq(app->flat_items[j].id, old_selected.ids[i])) {
                found = 1; break;
            }
        }
        if (found) idset_add(&app->selected_item_ids, old_selected.ids[i]);
    }
    idset_clear(&old_selected);

    free(data_path);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Timers                                                                     */
/* -------------------------------------------------------------------------- */

static int timespec_is_set(const struct timespec *ts) {
    return ts->tv_sec != 0 || ts->tv_nsec != 0;
}

static int timespec_passed(const struct timespec *ts) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > ts->tv_sec) return 1;
    if (now.tv_sec == ts->tv_sec && now.tv_nsec >= ts->tv_nsec) return 1;
    return 0;
}

#ifdef __APPLE__
static long timespec_diff_ms(const struct timespec *a, const struct timespec *b) {
    long sec = a->tv_sec - b->tv_sec;
    long nsec = a->tv_nsec - b->tv_nsec;
    return sec * 1000 + nsec / 1000000;
}
#endif

void app_data_changed(App *app) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    app->save_after.tv_sec = now.tv_sec + 1;
    app->save_after.tv_nsec = now.tv_nsec;
#ifdef __APPLE__
    app->safari_writeback_after.tv_sec = now.tv_sec + 2;
    app->safari_writeback_after.tv_nsec = now.tv_nsec;
#endif
    app->flat_dirty = 1;
}

int app_check_timers(App *app) {
    int changed = 0;
    if (timespec_is_set(&app->save_after) && timespec_passed(&app->save_after)) {
        app->save_after.tv_sec = 0;
        app->save_after.tv_nsec = 0;
        app_save(app);
    }
#ifdef __APPLE__
    if (timespec_is_set(&app->safari_writeback_after) && timespec_passed(&app->safari_writeback_after)) {
        app->safari_writeback_after.tv_sec = 0;
        app->safari_writeback_after.tv_nsec = 0;
        if (app->safari_sync_enabled) {
            char *expanded = expand_tilde(app->safari_sync_path);
            if (stat(expanded, &(struct stat){0}) == 0) {
                if (app_writeback_safari(app) != 0) {
                    if (!app->safari_permission_warned) {
                        app->safari_permission_warned = 1;
                        popup_set_alert(&app->popup,
                            "Safari sync requires Full Disk Access.\n\n"
                            "1. Press Enter to open System Settings\n"
                            "2. Add your terminal app to Full Disk Access\n"
                            "3. Restart epanel and re-enable Safari Sync.");
                        app->popup.alert_action = ALERT_ACTION_OPEN_SETTINGS;
                        changed = 1;
                    }
                } else {
                    app->safari_permission_warned = 0;
                    free(app->message);
                    app->message = str_dup("Synced to Safari");
                    struct stat st;
                    if (stat(expanded, &st) == 0) {
                        app->last_safari_writeback = st.st_mtime;
                    }
                    changed = 1;
                }
            }
            free(expanded);
        }
    }
#endif
    return changed;
}

struct timespec app_next_timer_deadline(const App *app) {
    struct timespec deadline = {0, 0};
    if (timespec_is_set(&app->save_after)) {
        deadline = app->save_after;
    }
#ifdef __APPLE__
    if (timespec_is_set(&app->safari_writeback_after)) {
        if (!timespec_is_set(&deadline) ||
            timespec_diff_ms(&app->safari_writeback_after, &deadline) < 0) {
            deadline = app->safari_writeback_after;
        }
    }
#endif
    return deadline;
}

/* -------------------------------------------------------------------------- */
/* Keyboard handling                                                          */
/* -------------------------------------------------------------------------- */

/* ncurses key constants we use */
#ifndef KEY_RESIZE
#define KEY_RESIZE 410
#endif

static void ensure_root_id(App *app) {
    if (strcmp(app->data.root_folder.id, "") == 0) {
        uuid_gen(app->data.root_folder.id);
        free(app->data.root_folder.name);
        app->data.root_folder.name = str_dup("/");
        app->data.root_folder.is_collapsed = 0;
    }
}

static void app_toggle_folder(App *app, const char *id) {
    Folder *f = find_folder(&app->data.root_folder, id);
    if (!f) return;
    f->is_collapsed = !f->is_collapsed;
    if (f->is_collapsed) {
        idset_remove(&app->search_expanded_folders, id);
    } else {
        idset_add(&app->search_expanded_folders, id);
    }
    app->flat_dirty = 1;
}

static void app_expand_folder(App *app, const char *id) {
    Folder *f = find_folder(&app->data.root_folder, id);
    if (!f) return;
    f->is_collapsed = 0;
    idset_add(&app->search_expanded_folders, id);
    app->flat_dirty = 1;
}

static void app_collapse_folder(App *app, const char *id) {
    Folder *f = find_folder(&app->data.root_folder, id);
    if (!f) return;
    f->is_collapsed = 1;
    idset_remove(&app->search_expanded_folders, id);
    app->flat_dirty = 1;
}

static void app_move_cursor(App *app, int delta) {
    if (app->flat_count == 0) {
        app->links_cursor = -1;
        return;
    }
    ssize_t new_cursor = app->links_cursor + delta;
    if (new_cursor < 0) new_cursor = 0;
    if ((size_t)new_cursor >= app->flat_count) new_cursor = (ssize_t)app->flat_count - 1;
    app->links_cursor = new_cursor;
}

static void count_folder_descendants(const Folder *f, size_t *folders, size_t *entries) {
    for (size_t i = 0; i < f->subfolder_count; i++) {
        (*folders)++;
        count_folder_descendants(&f->subfolders[i], folders, entries);
    }
    *entries += f->entry_count;
}

static void app_delete_selected(App *app) {
    /* Auto-select cursor item if nothing explicitly selected */
    if (app->selected_item_ids.count == 0) {
        if (app->links_cursor >= 0 && (size_t)app->links_cursor < app->flat_count) {
            idset_add(&app->selected_item_ids, app->flat_items[app->links_cursor].id);
        }
    }
    size_t entry_count = 0, folder_count = 0;
    for (size_t i = 0; i < app->selected_item_ids.count; i++) {
        const char *id = app->selected_item_ids.ids[i];
        Entry *e = find_entry(&app->data.root_folder, id);
        if (e) {
            entry_count++;
            continue;
        }
        Folder *f = find_folder(&app->data.root_folder, id);
        if (f) {
            folder_count++;  /* the selected folder itself */
            count_folder_descendants(f, &folder_count, &entry_count);
        }
    }
    if (app->selected_item_ids.count == 0) return;
    popup_clear(&app->popup);
    app->popup.type = POPUP_CONFIRM_DELETE;
    app->popup.entry_count = entry_count;
    app->popup.folder_count = folder_count;
    app->popup.subfolder_count = 0;
}

static void app_do_delete_selected(App *app) {
    for (size_t i = 0; i < app->selected_item_ids.count; i++) {
        const char *id = app->selected_item_ids.ids[i];
        delete_entry_recursive(&app->data.root_folder, id);
        delete_folder_recursive(&app->data.root_folder, id);
    }
    idset_clear(&app->selected_item_ids);
    app->links_cursor = -1;
    app_data_changed(app);
}

static void app_init_move_selected(App *app) {
    if (app->links_cursor < 0 || (size_t)app->links_cursor >= app->flat_count) return;
    const char *id = app->flat_items[app->links_cursor].id;
    int is_folder = app->flat_items[app->links_cursor].kind == ITEM_FOLDER;
    popup_clear(&app->popup);
    app->popup.type = POPUP_MOVE_ITEM;
    app->popup.item_id = str_dup(id);
    app->popup.is_folder = is_folder;
    app->popup.selected_folder = str_dup(app->data.root_folder.id);
    popup_build_choices(&app->popup, app, is_folder ? id : NULL);
}

static void app_init_rename_selected(App *app) {
    if (app->links_cursor < 0 || (size_t)app->links_cursor >= app->flat_count) return;
    const char *id = app->flat_items[app->links_cursor].id;
    if (app->flat_items[app->links_cursor].kind != ITEM_FOLDER) return;
    Folder *f = find_folder(&app->data.root_folder, id);
    if (!f) return;
    popup_clear(&app->popup);
    app->popup.type = POPUP_RENAME_FOLDER;
    app->popup.folder_id = str_dup(id);
    app->popup.text = str_dup(f->name);
    app->popup.cursor_pos = strlen(f->name);
}

static int app_open_entry(const char *text) {
    /* Launch via fork+exec, not system(), so `text` is a single argv element and
       can't be interpreted by a shell. Double-fork so the grandchild is orphaned
       to init and we don't leave zombies. */
#ifdef __APPLE__
    const char *cmd = "open";
#else
    const char *cmd = "xdg-open";
#endif
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            execlp(cmd, cmd, text, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    return 0;
}

static void app_flattened_folder_choices(const Folder *root, const char *exclude_id,
                                         char ***out_ids, char ***out_names, size_t **out_depths,
                                         size_t *count, size_t *cap, size_t depth) {
    if (exclude_id && str_eq(root->id, exclude_id)) return;
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *out_ids = realloc(*out_ids, *cap * sizeof(char *));
        *out_names = realloc(*out_names, *cap * sizeof(char *));
        *out_depths = realloc(*out_depths, *cap * sizeof(size_t));
    }
    (*out_ids)[*count] = str_dup(root->id);
    (*out_names)[*count] = str_dup(root->name);
    (*out_depths)[(*count)++] = depth;
    for (size_t i = 0; i < root->subfolder_count; i++) {
        app_flattened_folder_choices(&root->subfolders[i], exclude_id, out_ids, out_names, out_depths, count, cap, depth + 1);
    }
}

FolderChoice *app_get_folder_choices(const App *app, const char *exclude_id, size_t *count) {
    char **ids = NULL, **names = NULL;
    size_t *depths = NULL;
    size_t n = 0, cap = 0;
    app_flattened_folder_choices(&app->data.root_folder, exclude_id, &ids, &names, &depths, &n, &cap, 0);
    FolderChoice *choices = calloc(n, sizeof(FolderChoice));
    for (size_t i = 0; i < n; i++) {
        strncpy(choices[i].id, ids[i], UUID_STR_LEN);
        choices[i].name = names[i];
        choices[i].depth = depths[i];
        free(ids[i]);
    }
    free(ids);
    free(names);
    free(depths);
    *count = n;
    return choices;
}

void app_free_folder_choices(FolderChoice *choices, size_t count) {
    for (size_t i = 0; i < count; i++) free(choices[i].name);
    free(choices);
}

static void app_add_entry_to_folder(App *app, const char *text, const char *folder_id) {
    Folder *f = find_folder(&app->data.root_folder, folder_id);
    if (!f) f = &app->data.root_folder;
    folder_add_entry(f, text);
    app_data_changed(app);
}

static void app_create_folder(App *app, const char *name, const char *parent_id) {
    Folder *f = find_folder(&app->data.root_folder, parent_id);
    if (!f) f = &app->data.root_folder;
    folder_add_subfolder(f, name);
    app_data_changed(app);
}

static void app_modify_folder(App *app, const char *id, const char *new_name) {
    Folder *f = find_folder(&app->data.root_folder, id);
    if (!f) return;
    free(f->name);
    f->name = str_dup(new_name);
    app_data_changed(app);
}

static void app_handle_popup(App *app, int ch, int *changed) {
    Popup *p = &app->popup;
    switch (p->type) {
    case POPUP_ADD_ENTRY: {
        if (ch == KEY_UP || ch == KEY_DOWN) {
            popup_navigate_choices(p, ch, &p->selected_folder);
        } else if (ch == '\n' || ch == KEY_ENTER) {
            app_add_entry_to_folder(app, p->text ? p->text : "", p->selected_folder ? p->selected_folder : app->data.root_folder.id);
            free(app->search_input); app->search_input = str_dup("");
            idset_clear(&app->search_expanded_folders);
            idset_clear(&app->selected_item_ids);
            app_rebuild_flat_items(app);
            if (app->flat_count > 0) {
                idset_add(&app->selected_item_ids, app->flat_items[app->flat_count - 1].id);
            }
            popup_clear(p);
            *changed = 1;
            return;
        } else if (ch == 27) {
            popup_clear(p);
            return;
        } else if (ch >= 32 && ch < 127) {
            str_append_char(&p->text, (char)ch);
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            str_pop_char(&p->text);
        }
        break;
    }
    case POPUP_NEW_FOLDER: {
        if (ch == KEY_UP || ch == KEY_DOWN) {
            popup_navigate_choices(p, ch, &p->selected_parent);
        } else if (ch == '\n' || ch == KEY_ENTER) {
            char *trimmed = p->text ? p->text : "";
            while (isspace((unsigned char)*trimmed)) trimmed++;
            size_t len = strlen(trimmed);
            while (len > 0 && isspace((unsigned char)trimmed[len - 1])) len--;
            if (len > 0) {
                char *tmp = str_dup_len(trimmed, len);
                app_create_folder(app, tmp, p->selected_parent ? p->selected_parent : app->data.root_folder.id);
                free(tmp);
                popup_clear(p);
                *changed = 1;
            }
            return;
        } else if (ch == 27) {
            popup_clear(p);
            return;
        } else if (ch == KEY_LEFT) {
            if (p->cursor_pos > 0) p->cursor_pos--;
        } else if (ch == KEY_RIGHT) {
            if (p->text && p->cursor_pos < strlen(p->text)) p->cursor_pos++;
        } else if (ch >= 32 && ch < 127) {
            str_insert_char(&p->text, p->cursor_pos, (char)ch);
            p->cursor_pos++;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            str_delete_before(&p->text, p->cursor_pos);
            if (p->cursor_pos > 0) p->cursor_pos--;
        }
        break;
    }
    case POPUP_RENAME_FOLDER: {
        if (ch == '\n' || ch == KEY_ENTER) {
            char *trimmed = p->text ? p->text : "";
            while (isspace((unsigned char)*trimmed)) trimmed++;
            size_t len = strlen(trimmed);
            while (len > 0 && isspace((unsigned char)trimmed[len - 1])) len--;
            if (len > 0) {
                char *tmp = str_dup_len(trimmed, len);
                app_modify_folder(app, p->folder_id, tmp);
                free(tmp);
                popup_clear(p);
                *changed = 1;
            }
            return;
        } else if (ch == 27) {
            popup_clear(p);
            return;
        } else if (ch == KEY_LEFT) {
            if (p->cursor_pos > 0) p->cursor_pos--;
        } else if (ch == KEY_RIGHT) {
            if (p->text && p->cursor_pos < strlen(p->text)) p->cursor_pos++;
        } else if (ch >= 32 && ch < 127) {
            str_insert_char(&p->text, p->cursor_pos, (char)ch);
            p->cursor_pos++;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            str_delete_before(&p->text, p->cursor_pos);
            if (p->cursor_pos > 0) p->cursor_pos--;
        }
        break;
    }
    case POPUP_MOVE_ITEM: {
        if (ch == KEY_UP || ch == KEY_DOWN) {
            popup_navigate_choices(p, ch, &p->selected_folder);
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (p->is_folder) {
                move_folder(&app->data.root_folder, p->item_id, p->selected_folder ? p->selected_folder : app->data.root_folder.id);
            } else {
                IdSet to_move = {0};
                if (app->selected_item_ids.count > 1) {
                    for (size_t i = 0; i < app->selected_item_ids.count; i++) {
                        if (find_entry(&app->data.root_folder, app->selected_item_ids.ids[i])) {
                            idset_add(&to_move, app->selected_item_ids.ids[i]);
                        }
                    }
                } else {
                    idset_add(&to_move, p->item_id);
                }
                for (size_t i = 0; i < to_move.count; i++) {
                    move_entry(&app->data.root_folder, to_move.ids[i], p->selected_folder ? p->selected_folder : app->data.root_folder.id);
                }
                idset_clear(&to_move);
            }
            idset_clear(&app->selected_item_ids);
            popup_clear(p);
            app_data_changed(app);
            *changed = 1;
            return;
        } else if (ch == 27) {
            popup_clear(p);
            return;
        }
        break;
    }
    case POPUP_CONFIRM_DELETE:
        if (ch == 'y' || ch == 'Y' || ch == '\n' || ch == KEY_ENTER) {
            app_do_delete_selected(app);
            popup_clear(p);
            *changed = 1;
        } else if (ch == 'n' || ch == 'N' || ch == 27) {
            popup_clear(p);
        }
        break;
    case POPUP_ALERT:
        if (ch == '\n' || ch == KEY_ENTER) {
            if (p->alert_action == ALERT_ACTION_OPEN_SETTINGS) {
                int rc = system("open 'x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles'");
                (void)rc;
            }
            popup_clear(p);
            *changed = 1;
        } else if (ch == 27 || ch == ' ') {
            popup_clear(p);
            *changed = 1;
        }
        break;
    case POPUP_EXPORT_JSON: {
        if (ch == '\n' || ch == KEY_ENTER) {
            char *json = epanel_to_json(&app->data);
            char *expanded = expand_tilde(p->path ? p->path : "");
            FILE *fp = fopen(expanded, "w");
            if (fp) {
                fputs(json, fp);
                fclose(fp);
                free(app->message);
                app->message = str_dup("Exported successfully");
            } else {
                free(expanded); free(json);
                popup_set_alert(p, "Export failed: could not write file");
                return;
            }
            free(expanded);
            free(json);
            popup_clear(p);
            *changed = 1;
            return;
        } else if (ch == 27) {
            popup_clear(p);
            return;
        } else if (ch == KEY_LEFT) {
            if (p->cursor_pos > 0) p->cursor_pos--;
        } else if (ch == KEY_RIGHT) {
            if (p->path && p->cursor_pos < strlen(p->path)) p->cursor_pos++;
        } else if (ch >= 32 && ch < 127) {
            str_insert_char(&p->path, p->cursor_pos, (char)ch);
            p->cursor_pos++;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            str_delete_before(&p->path, p->cursor_pos);
            if (p->cursor_pos > 0) p->cursor_pos--;
        }
        break;
    }
    case POPUP_IMPORT_JSON: {
        if (ch == '\n' || ch == KEY_ENTER) {
            p->type = POPUP_CONFIRM_IMPORT_JSON;
            *changed = 1;
            return;
        } else if (ch == 27) {
            popup_clear(p);
            return;
        } else if (ch == KEY_LEFT) {
            if (p->cursor_pos > 0) p->cursor_pos--;
        } else if (ch == KEY_RIGHT) {
            if (p->path && p->cursor_pos < strlen(p->path)) p->cursor_pos++;
        } else if (ch >= 32 && ch < 127) {
            str_insert_char(&p->path, p->cursor_pos, (char)ch);
            p->cursor_pos++;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            str_delete_before(&p->path, p->cursor_pos);
            if (p->cursor_pos > 0) p->cursor_pos--;
        }
        break;
    }
    case POPUP_CONFIRM_IMPORT_JSON:
        if (ch == 'y' || ch == 'Y' || ch == '\n' || ch == KEY_ENTER) {
            char *expanded = expand_tilde(p->path ? p->path : "");
            FILE *fp = fopen(expanded, "r");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long len = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                char *buf = malloc(len + 1);
                if (buf && fread(buf, 1, len, fp) == (size_t)len) {
                    buf[len] = '\0';
                    EPanelData new_data = {0};
                    if (epanel_from_json(buf, &new_data)) {
                        folder_free(&app->data.root_folder);
                        free(app->data.notes);
                        app->data = new_data;
                        ensure_root_id(app);
                        free(app->notes_text);
                        app->notes_text = str_dup(app->data.notes ? app->data.notes : "");
                        app_data_changed(app);
                        free(app->message);
                        app->message = str_dup("Imported successfully");
                        *changed = 1;
                    } else {
                        folder_free(&new_data.root_folder);
                        free(new_data.notes);
                        popup_set_alert(p, "Import failed: invalid JSON");
                    }
                    free(buf);
                }
                fclose(fp);
            } else {
                popup_set_alert(p, "Import failed: could not read file");
            }
            free(expanded);
            if (p->type != POPUP_ALERT) popup_clear(p);
        } else if (ch == 'n' || ch == 'N' || ch == 27) {
            popup_clear(p);
        }
        break;
    case POPUP_HELP:
        if (ch == '\n' || ch == KEY_ENTER || ch == 27 || ch == ' ' || ch == 'q' || ch == 'Q') {
            popup_clear(p);
        }
        break;
    default:
        break;
    }
}

static void app_handle_links(App *app, int ch, int *changed) {
    switch (app->focus) {
    case FOCUS_SEARCH:
        if (ch >= 32 && ch < 127) {
            str_append_char(&app->search_input, (char)ch);
            idset_clear(&app->search_expanded_folders);
            app->flat_dirty = 1;
            *changed = 1;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            str_pop_char(&app->search_input);
            idset_clear(&app->search_expanded_folders);
            app->flat_dirty = 1;
            *changed = 1;
        } else if (ch == '\t' || ch == KEY_DOWN) {
            app->focus = FOCUS_LINKS_LIST;
            if (app->flat_count > 0 && app->links_cursor < 0) app->links_cursor = 0;
            *changed = 1;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            char *trimmed = app->search_input;
            while (isspace((unsigned char)*trimmed)) trimmed++;
            size_t len = strlen(trimmed);
            while (len > 0 && isspace((unsigned char)trimmed[len - 1])) len--;
            if (len > 0) {
                popup_clear(&app->popup);
                app->popup.type = POPUP_ADD_ENTRY;
                app->popup.text = str_dup_len(trimmed, len);
                app->popup.selected_folder = str_dup(app->data.root_folder.id);
                popup_build_choices(&app->popup, app, NULL);
                *changed = 1;
            }
        }
        break;
    case FOCUS_LINKS_LIST:
        if (ch == '\t') {
            app->focus = FOCUS_SEARCH;
            *changed = 1;
        } else if (ch == KEY_UP) {
            app_move_cursor(app, -1);
            *changed = 1;
        } else if (ch == KEY_DOWN) {
            app_move_cursor(app, 1);
            *changed = 1;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (app->links_cursor >= 0 && (size_t)app->links_cursor < app->flat_count) {
                const char *id = app->flat_items[app->links_cursor].id;
                if (app->flat_items[app->links_cursor].kind == ITEM_FOLDER) {
                    app_toggle_folder(app, id);
                    *changed = 1;
                } else {
                    if (app_open_entry(app->flat_items[app->links_cursor].name) != 0) {
                        popup_set_alert(&app->popup, "Could not open entry");
                        *changed = 1;
                    }
                }
            }
        } else if (ch == ' ') {
            if (app->links_cursor >= 0 && (size_t)app->links_cursor < app->flat_count) {
                const char *id = app->flat_items[app->links_cursor].id;
                if (idset_contains(&app->selected_item_ids, id))
                    idset_remove(&app->selected_item_ids, id);
                else
                    idset_add(&app->selected_item_ids, id);
                *changed = 1;
            }
        } else if (ch == 'n' || ch == 'N') {
            popup_clear(&app->popup);
            app->popup.type = POPUP_NEW_FOLDER;
            app->popup.text = str_dup("New Folder");
            app->popup.cursor_pos = strlen("New Folder");
            app->popup.selected_parent = str_dup(app->data.root_folder.id);
            popup_build_choices(&app->popup, app, NULL);
            *changed = 1;
        } else if (ch == 'd' || ch == 'D' || ch == KEY_DC) {
            app_delete_selected(app);
            *changed = 1;
        } else if (ch == 'm' || ch == 'M') {
            app_init_move_selected(app);
            *changed = 1;
        } else if (ch == 'r' || ch == 'R') {
            app_init_rename_selected(app);
            *changed = 1;
        } else if (ch == 'e' || ch == 'E') {
            popup_clear(&app->popup);
            app->popup.type = POPUP_EXPORT_JSON;
            const char *home = getenv("HOME");
            if (home) {
                char buf[4096];
                snprintf(buf, sizeof(buf), "%s/epanel-export.json", home);
                app->popup.path = str_dup(buf);
            } else {
                app->popup.path = str_dup("epanel-export.json");
            }
            app->popup.cursor_pos = app->popup.path ? strlen(app->popup.path) : 0;
            *changed = 1;
        } else if (ch == 'i' || ch == 'I') {
            popup_clear(&app->popup);
            app->popup.type = POPUP_IMPORT_JSON;
            app->popup.path = str_dup("");
            app->popup.cursor_pos = 0;
            *changed = 1;
        } else if (ch == KEY_RIGHT) {
            if (app->links_cursor >= 0 && (size_t)app->links_cursor < app->flat_count) {
                const char *id = app->flat_items[app->links_cursor].id;
                if (app->flat_items[app->links_cursor].kind == ITEM_FOLDER) {
                    app_expand_folder(app, id);
                    *changed = 1;
                }
            }
        } else if (ch == KEY_LEFT) {
            if (app->links_cursor >= 0 && (size_t)app->links_cursor < app->flat_count) {
                const char *id = app->flat_items[app->links_cursor].id;
                if (app->flat_items[app->links_cursor].kind == ITEM_FOLDER) {
                    app_collapse_folder(app, id);
                    *changed = 1;
                }
            }
        }
        break;
    default:
        break;
    }
}

static void app_handle_notes(App *app, int ch, int *changed) {
    size_t old_len = app->notes_text ? strlen(app->notes_text) : 0;
    if (ch >= 32 && ch < 127) {
        str_append_char(&app->notes_text, (char)ch);
    } else if (ch == '\n' || ch == KEY_ENTER) {
        str_append_char(&app->notes_text, '\n');
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        str_pop_char(&app->notes_text);
    }
    size_t new_len = app->notes_text ? strlen(app->notes_text) : 0;
    if (new_len != old_len) {
        app_data_changed(app);
        *changed = 1;
    }
    /* Update cursor */
    size_t lines = 1;
    size_t last_line_len = 0;
    for (size_t i = 0; app->notes_text && app->notes_text[i]; i++) {
        if (app->notes_text[i] == '\n') {
            lines++;
            last_line_len = 0;
        } else {
            last_line_len++;
        }
    }
    if (new_len > 0 && app->notes_text[new_len - 1] == '\n') {
        app->notes_cursor_y = lines;
        app->notes_cursor_x = 0;
    } else {
        app->notes_cursor_y = lines > 0 ? lines - 1 : 0;
        app->notes_cursor_x = last_line_len;
    }
}

static void app_handle_settings(App *app, int ch, int *changed) {
    switch (app->focus) {
    case FOCUS_SETTINGS_LINKS:
        if (ch >= 32 && ch < 127) {
            str_append_char(&app->settings_links_path, (char)ch);
            *changed = 1;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            str_pop_char(&app->settings_links_path);
            *changed = 1;
        } else if (ch == '\t') {
            app->focus = FOCUS_SETTINGS_NOTES;
            *changed = 1;
        }
        break;
    case FOCUS_SETTINGS_NOTES:
        if (ch >= 32 && ch < 127) {
            str_append_char(&app->settings_notes_path, (char)ch);
            *changed = 1;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            str_pop_char(&app->settings_notes_path);
            *changed = 1;
        } else if (ch == KEY_BTAB) {
            app->focus = FOCUS_SETTINGS_LINKS;
            *changed = 1;
        } else if (ch == '\t') {
#ifdef __APPLE__
            app->focus = FOCUS_SETTINGS_SAFARI;
#else
            app->focus = FOCUS_SETTINGS_SAVE;
#endif
            *changed = 1;
        }
        break;
#ifdef __APPLE__
    case FOCUS_SETTINGS_SAFARI:
        if (ch == KEY_BTAB) {
            app->focus = FOCUS_SETTINGS_NOTES;
            *changed = 1;
        } else if (ch == '\t') {
            app->focus = FOCUS_SETTINGS_SAVE;
            *changed = 1;
        } else if (ch == '\n' || ch == KEY_ENTER || ch == ' ') {
            app->safari_sync_enabled = !app->safari_sync_enabled;
            if (app->safari_sync_enabled) {
                app->safari_permission_warned = 0;
                Folder *folders = NULL;
                size_t folder_count = 0;
                Folder reading_list = {0};
                char *err = safari_import_plist(app->safari_sync_path, &folders, &folder_count, &reading_list);
                if (err) {
                    app->safari_sync_enabled = 0;
                    if (strstr(err, "Full Disk Access")) {
                        char *msg = malloc(strlen(err) + 64);
                        if (msg) {
                            strcpy(msg, err);
                            strcat(msg, "\n\nPress Enter to open System Settings.");
                            popup_set_alert(&app->popup, msg);
                            app->popup.alert_action = ALERT_ACTION_OPEN_SETTINGS;
                            free(msg);
                        } else {
                            popup_set_alert(&app->popup, err);
                            app->popup.alert_action = ALERT_ACTION_OPEN_SETTINGS;
                        }
                    } else {
                        popup_set_alert(&app->popup, err);
                    }
                    free(err);
                    *changed = 1;
                } else {
                    app_move_existing_to_original(app);
                    app_apply_safari_sync(app, folders, folder_count, &reading_list);
                    for (size_t i = 0; i < folder_count; i++) folder_free(&folders[i]);
                    free(folders);
                    folder_free(&reading_list);
                    struct stat st;
                    char *expanded_path = expand_tilde(app->safari_sync_path);
                    if (stat(expanded_path, &st) == 0) {
                        app->last_safari_writeback = st.st_mtime;
                    }
                    free(expanded_path);
                    app_start_safari_sync(app);
                    *changed = 1;
                }
            } else {
                if (app->safari) {
                    safari_state_free(app->safari);
                    app->safari = NULL;
                }
                *changed = 1;
            }
        }
        break;
#endif
    case FOCUS_SETTINGS_SAVE:
        if (ch == KEY_BTAB) {
#ifdef __APPLE__
            app->focus = FOCUS_SETTINGS_SAFARI;
#else
            app->focus = FOCUS_SETTINGS_NOTES;
#endif
            *changed = 1;
        } else if (ch == '\t') {
            app->focus = FOCUS_SETTINGS_LINKS;
            *changed = 1;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (app_save(app) != 0) {
                popup_set_alert(&app->popup, "Save failed");
            } else {
                free(app->message);
                app->message = str_dup("Saved successfully");
            }
            *changed = 1;
        }
        break;
    default:
        break;
    }
}

int app_handle_key(App *app, int ch) {
    /* Ctrl+C */
    if (ch == 3) return 1;

    int changed = 0;

    if (ch == KEY_F(1)) {
        app->current_tab = TAB_LINKS;
        app->focus = FOCUS_SEARCH;
        changed = 1;
    } else if (ch == KEY_F(2)) {
        app->current_tab = TAB_NOTES;
        app->focus = FOCUS_NOTES_TEXT;
        changed = 1;
    } else if (ch == KEY_F(3)) {
        app->current_tab = TAB_SETTINGS;
        app->focus = FOCUS_SETTINGS_LINKS;
        changed = 1;
    } else if (ch == KEY_RESIZE) {
        changed = 1;
    } else if (ch == 27) { /* Esc */
        if (app->search_input && app->search_input[0] != '\0') {
            free(app->search_input);
            app->search_input = str_dup("");
            idset_clear(&app->search_expanded_folders);
            app->flat_dirty = 1;
            changed = 1;
        }
    } else if (ch == '?' || ch == KEY_F(15)) { /* Shift+/ is ? */
        popup_clear(&app->popup);
        app->popup.type = POPUP_HELP;
        changed = 1;
    }

    if (app->popup.type != POPUP_NONE) {
        app_handle_popup(app, ch, &changed);
        return 0;
    }

    switch (app->current_tab) {
    case TAB_LINKS: app_handle_links(app, ch, &changed); break;
    case TAB_NOTES: app_handle_notes(app, ch, &changed); break;
    case TAB_SETTINGS: app_handle_settings(app, ch, &changed); break;
    }

    if (changed) {
        /* If anything changed, mark for potential redraw in main loop */
    }
    return 0;
}

#ifdef __APPLE__
/* -------------------------------------------------------------------------- */
/* Safari sync helpers (called from app.c to keep struct knowledge here)      */
/* -------------------------------------------------------------------------- */

void app_move_existing_to_original(App *app) {
    Folder *root = &app->data.root_folder;
    if (root->entry_count == 0 && root->subfolder_count == 0) return;
    for (size_t i = 0; i < root->subfolder_count; i++) {
        if (str_eq(root->subfolders[i].name, "my_original_epanel")) return;
    }
    /* Build `orig` from root's current contents, then replace root's children
       with a fresh single-slot array containing only `orig`. The old code put
       `orig` at the end of root->subfolders and then nulled root->subfolders,
       which left `orig` (and all its data) unreachable. */
    Folder orig;
    memset(&orig, 0, sizeof(orig));
    uuid_gen(orig.id);
    orig.name = str_dup("my_original_epanel");
    orig.entries = root->entries;
    orig.entry_count = root->entry_count;
    orig.entry_cap = root->entry_cap;
    orig.subfolders = root->subfolders;
    orig.subfolder_count = root->subfolder_count;
    orig.subfolder_cap = root->subfolder_cap;
    root->entries = NULL;
    root->entry_count = 0;
    root->entry_cap = 0;
    root->subfolders = malloc(sizeof(Folder));
    root->subfolder_count = 1;
    root->subfolder_cap = 1;
    root->subfolders[0] = orig;
    app_data_changed(app);
}

void app_apply_safari_sync(App *app, Folder *bookmark_folders, size_t bm_count, Folder *reading_list) {
    app_data_changed(app);
    /* Capture collapsed state */
    /* (simplified: we skip complex state preservation for brevity in C port) */
    Folder *root = &app->data.root_folder;
    for (size_t i = 0; i < bm_count; i++) {
        Folder *sf = &bookmark_folders[i];
        int found = 0;
        for (size_t j = 0; j < root->subfolder_count; j++) {
            if (str_eq(root->subfolders[j].name, sf->name)) {
                folder_free(&root->subfolders[j]);
                root->subfolders[j] = *sf;
                memset(sf, 0, sizeof(*sf));
                found = 1;
                break;
            }
        }
        if (!found && (sf->entry_count > 0 || sf->subfolder_count > 0)) {
            if (root->subfolder_count >= root->subfolder_cap) {
                root->subfolder_cap = root->subfolder_cap ? root->subfolder_cap * 2 : 4;
                root->subfolders = realloc(root->subfolders, root->subfolder_cap * sizeof(Folder));
            }
            root->subfolders[root->subfolder_count++] = *sf;
            memset(sf, 0, sizeof(*sf));
        }
    }
    /* Reading List */
    Folder rl = {0};
    memcpy(&rl, reading_list, sizeof(Folder));
    rl.name = str_dup("Reading List");
    int found = 0;
    for (size_t j = 0; j < root->subfolder_count; j++) {
        if (str_eq(root->subfolders[j].name, "Reading List")) {
            folder_free(&root->subfolders[j]);
            root->subfolders[j] = rl;
            found = 1;
            break;
        }
    }
    if (!found && rl.entry_count > 0) {
        if (root->subfolder_count >= root->subfolder_cap) {
            root->subfolder_cap = root->subfolder_cap ? root->subfolder_cap * 2 : 4;
            root->subfolders = realloc(root->subfolders, root->subfolder_cap * sizeof(Folder));
        }
        /* Shift to insert at front */
        memmove(&root->subfolders[1], &root->subfolders[0], root->subfolder_count * sizeof(Folder));
        root->subfolders[0] = rl;
        root->subfolder_count++;
    }
    memset(reading_list, 0, sizeof(*reading_list));
    free(app->last_sync_date);
    app->last_sync_date = current_iso_datetime();
}

int app_writeback_safari(App *app) {
    return safari_writeback_plist(app->safari_sync_path, &app->data.root_folder);
}

void app_sync_safari_on_startup(App *app) {
    if (!app->safari_sync_enabled) return;
    Folder *folders = NULL;
    size_t folder_count = 0;
    Folder reading_list = {0};
    char *err = safari_import_plist(app->safari_sync_path, &folders, &folder_count, &reading_list);
    if (err) {
        free(app->message);
        app->message = malloc(strlen(err) + 32);
        if (app->message) sprintf(app->message, "Safari sync failed: %s", err);
        free(err);
    } else {
        app_apply_safari_sync(app, folders, folder_count, &reading_list);
        for (size_t i = 0; i < folder_count; i++) folder_free(&folders[i]);
        free(folders);
        folder_free(&reading_list);
        struct stat st;
        char *expanded = expand_tilde(app->safari_sync_path);
        if (stat(expanded, &st) == 0) {
            app->last_safari_writeback = st.st_mtime;
        }
        free(expanded);
    }
}

void app_start_safari_sync(App *app) {
    if (app->sync_pipe_write < 0) return;
    char *expanded = expand_tilde(app->safari_sync_path);
    app->safari = safari_state_new(expanded, app->sync_pipe_write);
    free(expanded);
}
#endif
