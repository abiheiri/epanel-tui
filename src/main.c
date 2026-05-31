#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "ui.h"
#include "file_watch.h"
#include "update.h"
#include <ncurses.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include "safari_sync.h"
#endif

static char *detect_target(void) {
    struct utsname u;
    if (uname(&u) != 0) return NULL;
#ifdef __APPLE__
    const char *suffix = "apple-darwin";
#else
    const char *suffix = "unknown-linux-gnu";
#endif
    /* Map kernel arch names to the Rust-style target triple the release
       workflow uploads with: Apple Silicon reports "arm64" but assets are
       published as "aarch64-apple-darwin". */
    const char *machine = u.machine;
    if (strcmp(machine, "arm64") == 0) machine = "aarch64";
    size_t n = strlen(machine) + strlen(suffix) + 2;
    char *t = malloc(n);
    if (t) {
        snprintf(t, n, "%s-%s", machine, suffix);
    }
    return t;
}

static long timespec_diff_ms_now(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec = deadline->tv_sec - now.tv_sec;
    long nsec = deadline->tv_nsec - now.tv_nsec;
    return sec * 1000 + nsec / 1000000;
}

int main(int argc, char **argv) {
    const char *version = VERSION;
    int do_update = 0;
    int do_about = 0;
    int do_version = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("A TUI panel for managing links and notes.\n\n");
            printf("Usage: epanel [OPTIONS]\n\n");
            printf("Options:\n");
            printf("  -v, --version    Print version information\n");
            printf("      --update     Update to the latest release\n");
            printf("      --about      Show author and website info\n");
            printf("  -h, --help       Show this help message\n");
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            do_version = 1;
        } else if (strcmp(argv[i], "--update") == 0) {
            do_update = 1;
        } else if (strcmp(argv[i], "--about") == 0) {
            do_about = 1;
        } else {
            fprintf(stderr, "epanel: unknown option: %s\n", argv[i]);
            fprintf(stderr, "Try 'epanel --help' for more information.\n");
            return 2;
        }
    }

    if (do_version) {
        printf("epanel %s\n", version);
        return 0;
    }
    if (do_about) {
        printf("Author: Al Biheiri <al@forgottheaddress.com>\n");
        printf("Website: http://www.abiheiri.com\n");
        return 0;
    }
    if (do_update) {
        char *target = detect_target();
        int rc = self_update(version, target ? target : "unknown");
        free(target);
        return rc;
    }

    App app;
    app_init(&app);
    if (app_load(&app) != 0) {
        fprintf(stderr, "Warning: failed to load data\n");
    }

    /* Start file watcher — watches both epanel.json and notes.txt */
    char *data_path = data_file_path(&app);
    char *notes_path = notes_file_path(&app);
    int watch_fd = -1;
    if (data_path) {
        watch_fd = file_watch_start(data_path, notes_path);
        if (watch_fd >= 0) {
            app.watch_pipe_write = watch_fd; /* actually read end, but we store for reference */
        }
    }

#ifdef __APPLE__
    /* Safari sync startup */
    int sync_pipe_fds[2] = {-1, -1};
    if (app.safari_sync_enabled) {
        if (pipe(sync_pipe_fds) == 0) {
            app.sync_pipe_write = sync_pipe_fds[1];
            int flags = fcntl(sync_pipe_fds[0], F_GETFL, 0);
            fcntl(sync_pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
            app_sync_safari_on_startup(&app);
            /* Reset timers so startup sync doesn't trigger immediate writeback */
            app.save_after.tv_sec = 0;
            app.save_after.tv_nsec = 0;
            app.safari_writeback_after.tv_sec = 0;
            app.safari_writeback_after.tv_nsec = 0;
            app_start_safari_sync(&app);
        }
    }
#endif

    ui_init();
    app_rebuild_flat_items(&app);
    ui_draw(&app);

    int running = 1;
    while (running) {
        /* Compute timeout: next timer deadline, or 1000ms idle timeout */
        struct timespec deadline = app_next_timer_deadline(&app);
        int timeout_ms = 1000;
        if (deadline.tv_sec != 0 || deadline.tv_nsec != 0) {
            long diff = timespec_diff_ms_now(&deadline);
            if (diff < 0) diff = 0;
            if (diff < timeout_ms) timeout_ms = (int)diff;
        }

        timeout(timeout_ms);
        int ch = getch();
        int changed = 0;

        if (ch != ERR) {
            if (ch == KEY_RESIZE) {
                changed = 1;
            } else if (app_handle_key(&app, ch)) {
                running = 0;
            } else {
                changed = 1;
            }
        }

        /* Check file watcher */
        if (watch_fd >= 0) {
            char sig;
            int has_event = 0;
            while (read(watch_fd, &sig, 1) == 1) {
                has_event = 1;
            }
            if (has_event) {
                /* Ignore events triggered by our own save (within 500 ms), and
                   don't reload while a popup is open — the popup's cached state
                   (e.g. folder choices) would be invalidated under the user. */
                int skip = (app.popup.type != POPUP_NONE);
                if (!skip && (app.last_save_time.tv_sec != 0 || app.last_save_time.tv_nsec != 0)) {
                    long ms = timespec_diff_ms_now(&app.last_save_time);
                    if (ms < 500 && ms >= 0) skip = 1;
                }
                if (!skip) {
                    if (app_reload(&app) == 0) {
                        changed = 1;
                    }
                }
            }
        }

#ifdef __APPLE__
        /* Check Safari sync pipe */
        if (sync_pipe_fds[0] >= 0) {
            char sig;
            while (read(sync_pipe_fds[0], &sig, 1) == 1) {
                if (app.popup.type != POPUP_NONE) {
                    /* Drain while popup is open */
                    continue;
                }
                /* Check modification date gate BEFORE expensive import */
                char *expanded = expand_tilde(app.safari_sync_path);
                struct stat st;
                int should_import = 1;
                if (stat(expanded, &st) == 0) {
                    if (app.last_safari_writeback != 0 && st.st_mtime <= app.last_safari_writeback) {
                        should_import = 0;
                    }
                }
                if (!should_import) {
                    free(expanded);
                    continue;
                }
                Folder *folders = NULL;
                size_t folder_count = 0;
                Folder reading_list = {0};
                char *err = safari_import_plist(app.safari_sync_path, &folders, &folder_count, &reading_list);
                if (!err) {
                    app_apply_safari_sync(&app, folders, folder_count, &reading_list);
                    changed = 1;
                    for (size_t i = 0; i < folder_count; i++) folder_free(&folders[i]);
                    free(folders);
                    folder_free(&reading_list);
                } else {
                    free(err);
                }
                free(expanded);
            }
        }
#endif

        if (app_check_timers(&app)) {
            changed = 1;
        }

        if (changed) {
            if (app.flat_dirty) app_rebuild_flat_items(&app);
            ui_draw(&app);
        }
    }

    ui_shutdown();
    app_save(&app);
    app_free(&app);
    if (watch_fd >= 0) close(watch_fd);
#ifdef __APPLE__
    if (sync_pipe_fds[0] >= 0) close(sync_pipe_fds[0]);
    if (sync_pipe_fds[1] >= 0) close(sync_pipe_fds[1]);
#endif
    free(data_path);
    free(notes_path);
    return 0;
}
