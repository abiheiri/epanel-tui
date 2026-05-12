#define _POSIX_C_SOURCE 200809L
#include "safari_sync.h"
#include "uuid.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>

/* -------------------------------------------------------------------------- */
/* CoreFoundation plist parsing helpers                                       */
/* -------------------------------------------------------------------------- */

static char *cfstring_to_cstr(CFStringRef s) {
    if (!s) return NULL;
    CFIndex len = CFStringGetLength(s);
    CFIndex maxLen = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    char *buf = malloc((size_t)maxLen);
    if (!buf) return NULL;
    if (!CFStringGetCString(s, buf, maxLen, kCFStringEncodingUTF8)) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void parse_cf_children(CFArrayRef arr, Folder *out);

static void parse_cf_item(CFDictionaryRef dict, Folder *out) {
    memset(out, 0, sizeof(*out));
    uuid_gen(out->id);

    CFStringRef title = CFDictionaryGetValue(dict, CFSTR("Title"));
    if (title && CFGetTypeID(title) == CFStringGetTypeID()) {
        out->name = cfstring_to_cstr(title);
    }
    if (!out->name) {
        out->name = strdup("");
    }

    CFArrayRef children = CFDictionaryGetValue(dict, CFSTR("Children"));
    if (children && CFGetTypeID(children) == CFArrayGetTypeID()) {
        parse_cf_children(children, out);
    }
}

static void parse_cf_children(CFArrayRef arr, Folder *out) {
    CFIndex n = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < n; i++) {
        CFTypeRef item = CFArrayGetValueAtIndex(arr, i);
        if (CFGetTypeID(item) != CFDictionaryGetTypeID()) continue;
        CFDictionaryRef dict = (CFDictionaryRef)item;

        CFStringRef type = CFDictionaryGetValue(dict, CFSTR("WebBookmarkType"));
        if (!type || CFGetTypeID(type) != CFStringGetTypeID()) continue;

        char *type_str = cfstring_to_cstr(type);
        if (!type_str) continue;

        if (strcmp(type_str, "WebBookmarkTypeLeaf") == 0) {
            CFStringRef url = CFDictionaryGetValue(dict, CFSTR("URLString"));
            if (!url || CFGetTypeID(url) != CFStringGetTypeID()) {
                url = CFDictionaryGetValue(dict, CFSTR("URL"));
            }
            if (url && CFGetTypeID(url) == CFStringGetTypeID()) {
                char *url_str = cfstring_to_cstr(url);
                if (url_str && strlen(url_str) > 0) {
                    if (out->entry_count >= out->entry_cap) {
                        out->entry_cap = out->entry_cap ? out->entry_cap * 2 : 4;
                        out->entries = realloc(out->entries, out->entry_cap * sizeof(Entry));
                    }
                    Entry *e = &out->entries[out->entry_count++];
                    memset(e, 0, sizeof(*e));
                    uuid_gen(e->id);
                    e->text = url_str;
                    e->date = strdup("1970-01-01T00:00:00Z");
                } else {
                    free(url_str);
                }
            }
        } else if (strcmp(type_str, "WebBookmarkTypeList") == 0) {
            if (out->subfolder_count >= out->subfolder_cap) {
                out->subfolder_cap = out->subfolder_cap ? out->subfolder_cap * 2 : 4;
                out->subfolders = realloc(out->subfolders, out->subfolder_cap * sizeof(Folder));
            }
            Folder *sub = &out->subfolders[out->subfolder_count];
            parse_cf_item(dict, sub);
            out->subfolder_count++;
        }

        free(type_str);
    }
}
#endif

/* -------------------------------------------------------------------------- */
/* Import Safari Bookmarks.plist                                              */
/* -------------------------------------------------------------------------- */

char *safari_import_plist(const char *path, Folder **folders, size_t *count, Folder *reading_list) {
    memset(reading_list, 0, sizeof(*reading_list));
    *folders = NULL;
    *count = 0;
    char *err = NULL;
    char *to_free = NULL;

    struct stat st;
    const char *effective_path = path;
    int found = 0;

    if (stat(path, &st) == 0) {
        found = 1;
    } else {
        const char *home = getenv("HOME");
        if (home) {
            const char *fallback = "/Library/Containers/com.apple.Safari/Data/Library/Safari/Bookmarks.plist";
            size_t len = strlen(home) + strlen(fallback) + 1;
            to_free = malloc(len);
            if (to_free) {
                snprintf(to_free, len, "%s%s", home, fallback);
                if (stat(to_free, &st) == 0) {
                    effective_path = to_free;
                    found = 1;
                }
            }
        }
    }

    if (!found) {
        err = strdup("Safari bookmarks file not found.\n\nOn newer macOS versions the file may be in a different location (e.g. ~/Library/Containers/com.apple.Safari/Data/Library/Safari/Bookmarks.plist).");
        goto done;
    }

#ifndef __APPLE__
    err = strdup("Safari sync is only supported on macOS.");
    goto done;
#else
    FILE *fp = fopen(effective_path, "rb");
    if (!fp) {
        err = strdup("Cannot open Safari bookmarks file.");
        goto done;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(fp);
        err = strdup("Safari bookmarks file is empty.");
        goto done;
    }

    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) {
        fclose(fp);
        err = strdup("Out of memory reading Safari bookmarks.");
        goto done;
    }

    if (fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        free(buf);
        fclose(fp);
        err = strdup("Failed to read Safari bookmarks file.");
        goto done;
    }
    fclose(fp);

    CFDataRef data = CFDataCreate(kCFAllocatorDefault, buf, (CFIndex)fsize);
    free(buf);
    if (!data) {
        err = strdup("Failed to create CFData from bookmarks file.");
        goto done;
    }

    CFPropertyListRef plist = CFPropertyListCreateWithData(
        kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);

    if (!plist) {
        err = strdup("Failed to parse Safari bookmarks plist.");
        goto done;
    }

    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        CFRelease(plist);
        err = strdup("Safari bookmarks plist root is not a dictionary.");
        goto done;
    }

    CFDictionaryRef root = (CFDictionaryRef)plist;
    CFArrayRef children = CFDictionaryGetValue(root, CFSTR("Children"));

    if (!children || CFGetTypeID(children) != CFArrayGetTypeID()) {
        CFRelease(plist);
        err = strdup("Missing 'Children' key in Safari bookmarks root dictionary.");
        goto done;
    }

    size_t bm_cap = 4;
    *folders = calloc(bm_cap, sizeof(Folder));

    CFIndex nchildren = CFArrayGetCount(children);
    for (CFIndex i = 0; i < nchildren; i++) {
        CFTypeRef child = CFArrayGetValueAtIndex(children, i);
        if (CFGetTypeID(child) != CFDictionaryGetTypeID()) continue;
        CFDictionaryRef dict = (CFDictionaryRef)child;

        CFStringRef title = CFDictionaryGetValue(dict, CFSTR("Title"));
        CFStringRef bmtype = CFDictionaryGetValue(dict, CFSTR("WebBookmarkType"));

        if (!bmtype || CFGetTypeID(bmtype) != CFStringGetTypeID()) continue;

        char *type_str = cfstring_to_cstr(bmtype);
        if (!type_str || strcmp(type_str, "WebBookmarkTypeList") != 0) {
            free(type_str);
            continue;
        }
        free(type_str);

        CFArrayRef child_children = CFDictionaryGetValue(dict, CFSTR("Children"));
        if (!child_children || CFGetTypeID(child_children) != CFArrayGetTypeID()) continue;

        char *title_str = cfstring_to_cstr(title);
        const char *t = title_str ? title_str : "";

        if (strcmp(t, "com.apple.ReadingList") == 0) {
            parse_cf_children(child_children, reading_list);
            uuid_gen(reading_list->id);
            reading_list->name = strdup("Reading List");
        } else {
            if (*count >= bm_cap) {
                bm_cap *= 2;
                *folders = realloc(*folders, bm_cap * sizeof(Folder));
            }
            Folder *f = &(*folders)[*count];
            parse_cf_children(child_children, f);
            uuid_gen(f->id);
            if (strcmp(t, "BookmarksBar") == 0) {
                free(f->name);
                f->name = strdup("Favorites");
            } else if (strcmp(t, "BookmarksMenu") == 0) {
                free(f->name);
                f->name = strdup("Bookmarks Menu");
            } else if (strlen(t) == 0) {
                free(f->name);
                f->name = strdup("Untitled Folder");
            } else {
                free(f->name);
                f->name = strdup(t);
            }
            (*count)++;
        }

        free(title_str);
    }

    CFRelease(plist);
#endif

done:
    free(to_free);
    return err;
}

/* -------------------------------------------------------------------------- */
/* Writeback (simplified - constructs JSON and uses plutil)                   */
/* -------------------------------------------------------------------------- */

static cJSON *build_safari_leaf(const Entry *e) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "WebBookmarkType", "WebBookmarkTypeLeaf");
    char uuid[37];
    uuid_gen(uuid);
    cJSON_AddStringToObject(o, "WebBookmarkUUID", uuid);
    cJSON_AddStringToObject(o, "URLString", e->text);
    cJSON *uri = cJSON_CreateObject();
    cJSON_AddStringToObject(uri, "title", e->text);
    cJSON_AddItemToObject(o, "URIDictionary", uri);
    return o;
}

static cJSON *build_safari_folder_json(const Folder *f, const char *title) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "WebBookmarkType", "WebBookmarkTypeList");
    cJSON_AddStringToObject(o, "Title", title);
    char uuid[37];
    uuid_gen(uuid);
    cJSON_AddStringToObject(o, "WebBookmarkUUID", uuid);
    cJSON *children = cJSON_CreateArray();
    for (size_t i = 0; i < f->subfolder_count; i++) {
        cJSON_AddItemToArray(children, build_safari_folder_json(&f->subfolders[i], f->subfolders[i].name));
    }
    for (size_t i = 0; i < f->entry_count; i++) {
        cJSON_AddItemToArray(children, build_safari_leaf(&f->entries[i]));
    }
    if (cJSON_GetArraySize(children) > 0) {
        cJSON_AddItemToObject(o, "Children", children);
    } else {
        cJSON_Delete(children);
    }
    return o;
}

int safari_writeback_plist(const char *path, Folder *root) {
    /* Build a simplified plist JSON */
    cJSON *plist = cJSON_CreateObject();
    cJSON_AddStringToObject(plist, "WebBookmarkType", "WebBookmarkTypeList");
    cJSON_AddStringToObject(plist, "Title", "");
    char uuid[37];
    uuid_gen(uuid);
    cJSON_AddStringToObject(plist, "WebBookmarkUUID", uuid);

    cJSON *children = cJSON_CreateArray();

    Folder *favorites = NULL;
    Folder *bookmarks_menu = NULL;
    Folder *reading_list = NULL;

    for (size_t i = 0; i < root->subfolder_count; i++) {
        if (strcmp(root->subfolders[i].name, "Favorites") == 0) {
            favorites = &root->subfolders[i];
        } else if (strcmp(root->subfolders[i].name, "Bookmarks Menu") == 0) {
            bookmarks_menu = &root->subfolders[i];
        } else if (strcmp(root->subfolders[i].name, "Reading List") == 0) {
            reading_list = &root->subfolders[i];
        }
    }

    if (favorites) {
        cJSON_AddItemToArray(children, build_safari_folder_json(favorites, "BookmarksBar"));
    }

    /* BookmarksMenu merges Bookmarks Menu + other folders + root entries */
    cJSON *bm = cJSON_CreateObject();
    cJSON_AddStringToObject(bm, "WebBookmarkType", "WebBookmarkTypeList");
    cJSON_AddStringToObject(bm, "Title", "BookmarksMenu");
    uuid_gen(uuid);
    cJSON_AddStringToObject(bm, "WebBookmarkUUID", uuid);
    cJSON *bm_children = cJSON_CreateArray();
    if (bookmarks_menu) {
        for (size_t i = 0; i < bookmarks_menu->entry_count; i++) {
            cJSON_AddItemToArray(bm_children, build_safari_leaf(&bookmarks_menu->entries[i]));
        }
        for (size_t i = 0; i < bookmarks_menu->subfolder_count; i++) {
            cJSON_AddItemToArray(bm_children, build_safari_folder_json(&bookmarks_menu->subfolders[i], bookmarks_menu->subfolders[i].name));
        }
    }
    for (size_t i = 0; i < root->entry_count; i++) {
        cJSON_AddItemToArray(bm_children, build_safari_leaf(&root->entries[i]));
    }
    for (size_t i = 0; i < root->subfolder_count; i++) {
        const char *name = root->subfolders[i].name;
        if (strcmp(name, "Favorites") == 0 || strcmp(name, "Bookmarks Menu") == 0 ||
            strcmp(name, "Reading List") == 0 || strcmp(name, "my_original_epanel") == 0) {
            continue;
        }
        cJSON_AddItemToArray(bm_children, build_safari_folder_json(&root->subfolders[i], name));
    }
    if (cJSON_GetArraySize(bm_children) > 0) {
        cJSON_AddItemToObject(bm, "Children", bm_children);
    } else {
        cJSON_Delete(bm_children);
    }
    cJSON_AddItemToArray(children, bm);

    if (reading_list) {
        cJSON *rl = build_safari_folder_json(reading_list, "com.apple.ReadingList");
        cJSON_AddItemToArray(children, rl);
    }

    cJSON_AddItemToObject(plist, "Children", children);

    char *json = cJSON_Print(plist);
    cJSON_Delete(plist);
    if (!json) return -1;

    /* Write temp JSON and convert to binary plist */
    char tmp_json[1024];
    snprintf(tmp_json, sizeof(tmp_json), "%s.json.tmp", path);
    FILE *fp = fopen(tmp_json, "w");
    if (!fp) {
        free(json);
        return -1;
    }
    fputs(json, fp);
    fclose(fp);
    free(json);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "plutil -convert binary1 \"%s\" -o \"%s\" 2>/dev/null", tmp_json, path);
    int rc = system(cmd);
    unlink(tmp_json);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Watcher thread (kqueue)                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *path;
    int signal_pipe;
    int exit_pipe;
} WatchCtx;

static void *safari_watch_thread(void *arg) {
    WatchCtx *ctx = arg;
    int signal_pipe = ctx->signal_pipe;
    int exit_pipe = ctx->exit_pipe;

    while (1) {
        int fd = open(ctx->path, O_RDONLY);
        if (fd < 0) {
            sleep(30);
            continue;
        }
        int kq = kqueue();
        if (kq < 0) {
            close(fd);
            sleep(30);
            continue;
        }
        struct kevent changes[2];
        EV_SET(&changes[0], fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND, 0, NULL);
        EV_SET(&changes[1], exit_pipe, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(kq, changes, 2, NULL, 0, NULL) < 0) {
            close(fd);
            close(kq);
            sleep(30);
            continue;
        }

        while (1) {
            struct kevent events[2];
            int nev = kevent(kq, NULL, 0, events, 2, NULL);
            if (nev < 0) {
                if (errno == EINTR) continue;
                break;
            }
            int file_changed = 0;
            int should_exit = 0;
            int reestablish = 0;
            for (int i = 0; i < nev; i++) {
                if ((int)events[i].ident == exit_pipe) {
                    should_exit = 1;
                } else if ((int)events[i].ident == fd) {
                    file_changed = 1;
                    if (events[i].fflags & (NOTE_DELETE | NOTE_RENAME)) {
                        reestablish = 1;
                    }
                }
            }
            if (should_exit) {
                close(fd);
                close(kq);
                free(ctx->path);
                free(ctx);
                return NULL;
            }
            if (file_changed) {
                char sig = 1;
                write(signal_pipe, &sig, 1);
            }
            close(fd);
            close(kq);
            if (reestablish) {
                usleep(500000);
            }
            break;
        }
    }
}

SafariState *safari_state_new(const char *path, int signal_pipe_write) {
    int exit_pipes[2];
    if (pipe(exit_pipes) != 0) return NULL;

    WatchCtx *ctx = malloc(sizeof(WatchCtx));
    if (!ctx) {
        close(exit_pipes[0]);
        close(exit_pipes[1]);
        return NULL;
    }
    ctx->path = strdup(path);
    ctx->signal_pipe = signal_pipe_write;
    ctx->exit_pipe = exit_pipes[0];

    SafariState *s = calloc(1, sizeof(SafariState));
    if (!s) {
        free(ctx->path);
        free(ctx);
        close(exit_pipes[0]);
        close(exit_pipes[1]);
        return NULL;
    }
    s->exit_pipe_write = exit_pipes[1];

    if (pthread_create(&s->thread, NULL, safari_watch_thread, ctx) != 0) {
        free(ctx->path);
        free(ctx);
        close(exit_pipes[0]);
        close(exit_pipes[1]);
        free(s);
        return NULL;
    }
    return s;
}

void safari_state_free(SafariState *s) {
    if (!s) return;
    char z = 0;
    write(s->exit_pipe_write, &z, 1);
    close(s->exit_pipe_write);
    pthread_join(s->thread, NULL);
    free(s);
}
