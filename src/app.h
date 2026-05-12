#ifndef APP_H
#define APP_H

#include <sys/types.h>
#include <stddef.h>
#include <time.h>

#define UUID_STR_LEN 36

/* Fallback so editors that don't parse the Makefile's -DVERSION=... still see
   this identifier defined. Release builds override it via the Makefile. */
#ifndef VERSION
#define VERSION "dev"
#endif

typedef struct {
    char id[UUID_STR_LEN + 1];
    char *text;
    char *date; /* ISO-8601 UTC */
} Entry;

typedef struct Folder {
    char id[UUID_STR_LEN + 1];
    char *name;
    Entry *entries;
    size_t entry_count;
    size_t entry_cap;
    struct Folder *subfolders;
    size_t subfolder_count;
    size_t subfolder_cap;
    int is_collapsed;
} Folder;

typedef struct {
    Folder root_folder;
    char *notes;
} EPanelData;

typedef enum {
    TAB_LINKS,
    TAB_NOTES,
    TAB_SETTINGS
} Tab;

typedef enum {
    FOCUS_SEARCH,
    FOCUS_LINKS_LIST,
    FOCUS_NOTES_TEXT,
    FOCUS_SETTINGS_LINKS,
    FOCUS_SETTINGS_NOTES,
#ifdef __APPLE__
    FOCUS_SETTINGS_SAFARI,
#endif
    FOCUS_SETTINGS_SAVE
} Focus;

typedef enum {
    POPUP_NONE,
    POPUP_ADD_ENTRY,
    POPUP_NEW_FOLDER,
    POPUP_RENAME_FOLDER,
    POPUP_MOVE_ITEM,
    POPUP_CONFIRM_DELETE,
    POPUP_ALERT,
    POPUP_HELP,
    POPUP_EXPORT_JSON,
    POPUP_IMPORT_JSON,
    POPUP_CONFIRM_IMPORT_JSON
} PopupType;

typedef enum {
    ALERT_ACTION_NONE,
    ALERT_ACTION_OPEN_SETTINGS
} AlertAction;

typedef struct {
    char id[UUID_STR_LEN + 1];
    char *name;
    size_t depth;
} FolderChoice;

typedef struct {
    PopupType type;
    char *text;
    char *selected_folder; /* UUID string */
    char *selected_parent; /* UUID string */
    char *folder_id;       /* UUID string */
    char *item_id;         /* UUID string */
    int is_folder;
    size_t entry_count;
    size_t folder_count;
    size_t subfolder_count;
    char *path;
    size_t cursor_pos;
    AlertAction alert_action;
    /* Flattened folder list built once when a folder-picker popup (ADD_ENTRY,
       NEW_FOLDER, MOVE_ITEM) opens, reused by both the key handler and the
       renderer. Freed in popup_clear. */
    FolderChoice *choices;
    size_t choice_count;
} Popup;

typedef enum {
    ITEM_FOLDER,
    ITEM_ENTRY
} FlatItemKind;

typedef struct {
    char id[UUID_STR_LEN + 1];
    FlatItemKind kind;
    size_t depth;
    char *name;
    int is_collapsed;
} FlatItem;

typedef struct {
    char **ids;
    size_t count;
    size_t cap;
} IdSet;

typedef struct {
    char **ids;
    size_t count;
    size_t cap;
} IdList;

#ifdef __APPLE__
typedef struct SafariState SafariState;
#endif

typedef struct {
    Tab current_tab;
    Focus focus;
    EPanelData data;
    char *search_input;
    IdSet search_expanded_folders;
    IdSet selected_item_ids;
    ssize_t links_cursor; /* -1 = none */
    FlatItem *flat_items;
    size_t flat_count;
    size_t flat_cap;
    int flat_dirty; /* 1 when tree/search/expand state has changed since last rebuild */
    char *notes_text;
    size_t notes_cursor_x;
    size_t notes_cursor_y;
    char *settings_links_path;
    char *settings_notes_path;
#ifdef __APPLE__
    int safari_sync_enabled;
    char *safari_sync_path;
    char *last_sync_date;
    int sync_pipe_write;
    SafariState *safari;
    time_t last_safari_writeback;
    int safari_permission_warned;
    struct timespec safari_writeback_after;
#endif
    Popup popup;
    char *message;
    char *config_dir;
    struct timespec save_after;
    struct timespec last_save_time;

    /* file watcher state */
    int watch_pipe_write;
    int need_reload;
} App;

void app_init(App *app);
void app_free(App *app);
int app_load(App *app);
int app_save(App *app);
int app_reload(App *app);

/* Timers: return 1 if something changed and needs redraw */
int app_check_timers(App *app);
struct timespec app_next_timer_deadline(const App *app);

/* Keyboard handling: return 1 if should quit */
int app_handle_key(App *app, int ch);

/* Flat item rebuilding */
void app_rebuild_flat_items(App *app);

/* Helpers */
char *expand_tilde(const char *path);

/* Folder/entry manipulation */
void folder_free(Folder *f);

/* IdSet helpers */
int idset_contains(const IdSet *set, const char *id);

#ifdef __APPLE__
void app_sync_safari_on_startup(App *app);
void app_start_safari_sync(App *app);
void app_apply_safari_sync(App *app, Folder *bookmark_folders, size_t bm_count, Folder *reading_list);
#endif

#endif
