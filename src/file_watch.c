#define _POSIX_C_SOURCE 200809L
#include "file_watch.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#endif

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#endif

/* ------------------------------------------------------------------ */
/* Helpers: parse a path into (directory_copy, basename)              */
/* Returns a strdup'd copy with the basename part still appended      */
/* so the caller can split it. write_dir and write_name point inside. */
/* ------------------------------------------------------------------ */
static int split_path(const char *path, char **dir_out, const char **name_out) {
    char *copy = strdup(path);
    if (!copy) return -1;
    char *sep = strrchr(copy, '/');
    if (sep) {
        *sep = '\0';
        *dir_out = copy[0] ? copy : strdup("/");
        if (!*dir_out) { free(copy); return -1; }
        if (*dir_out != copy) free(copy);
        *name_out = sep + 1;
    } else {
        *dir_out = strdup(".");
        if (!*dir_out) { free(copy); return -1; }
        *name_out = copy;
    }
    /* *dir_out owns the string; *name_out points inside it */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Watch target — a (directory, basename) pair                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char *full_path;  /* strdup'd original full path (for kqueue open) */
    char *dir;        /* strdup'd directory path, owns storage */
    const char *file; /* points inside dir (after last /)       */
} WatchTarget;

static void target_free(WatchTarget *t) {
    free(t->full_path);
    free(t->dir);
}

/* ------------------------------------------------------------------ */
/* Thread arguments                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    WatchTarget primary;
    WatchTarget notes;
    int has_notes;
    int write_fd;
} WatchArgs;

/* ------------------------------------------------------------------ */
/* Linux inotify backend                                               */
/* ------------------------------------------------------------------ */
#ifdef __linux__

static int setup_inotify_watch(int inotify_fd, const WatchTarget *t) {
    int wd = inotify_add_watch(inotify_fd, t->dir,
                               IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
    return wd;
}

static void *watch_thread(void *arg) {
    WatchArgs *wa = arg;

    int inotify_fd = inotify_init1(IN_CLOEXEC);
    if (inotify_fd < 0) {
        target_free(&wa->primary);
        if (wa->has_notes) target_free(&wa->notes);
        free(wa);
        return NULL;
    }

    int wd_primary = setup_inotify_watch(inotify_fd, &wa->primary);
    if (wd_primary < 0) {
        close(inotify_fd);
        target_free(&wa->primary);
        if (wa->has_notes) target_free(&wa->notes);
        free(wa);
        return NULL;
    }

    int wd_notes = -1;
    if (wa->has_notes) {
        wd_notes = setup_inotify_watch(inotify_fd, &wa->notes);
        /* non-fatal if notes directory doesn't exist yet */
    }

    while (1) {
        char buf[4096];
        ssize_t n = read(inotify_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int fire = 0;
        for (ssize_t off = 0; off < n; ) {
            const struct inotify_event *ev = (struct inotify_event *)(buf + off);
            if (ev->len > 0) {
                if (strcmp(ev->name, wa->primary.file) == 0 ||
                    (wa->has_notes && strcmp(ev->name, wa->notes.file) == 0)) {
                    fire = 1;
                }
            }
            off += (ssize_t)sizeof(struct inotify_event) + ev->len;
        }
        if (fire) {
            char sig = 1;
            if (write(wa->write_fd, &sig, 1) != 1) break;
        }
    }

    if (wd_notes >= 0) inotify_rm_watch(inotify_fd, wd_notes);
    inotify_rm_watch(inotify_fd, wd_primary);
    close(inotify_fd);
    target_free(&wa->primary);
    if (wa->has_notes) target_free(&wa->notes);
    free(wa);
    return NULL;
}

#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* macOS kqueue backend                                                */
/* ------------------------------------------------------------------ */
#ifdef __APPLE__

/* Open a file (or its parent directory fallback) and add a kqueue
   event. Returns the file descriptor (>0) on success, or -1 on
   failure. On success *fd_out is set to the FD to monitor. */
static int setup_kqueue_kevent(int kq, const char *path, int *fd_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* File doesn't exist yet — watch the parent directory instead */
        char *dir_copy = strdup(path);
        if (!dir_copy) return -1;
        char *sep = strrchr(dir_copy, '/');
        const char *watch_dir;
        if (sep) {
            *sep = '\0';
            watch_dir = dir_copy[0] ? dir_copy : "/";
        } else {
            watch_dir = ".";
        }
        fd = open(watch_dir, O_RDONLY);
        free(dir_copy);
        if (fd < 0) return -1;
    }
    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, NULL);
    if (kevent(kq, &change, 1, NULL, 0, NULL) < 0) {
        close(fd);
        return -1;
    }
    *fd_out = fd;
    return 0;
}

/* Re-establish kevent for a file whose FD was invalidated by
   NOTE_DELETE / NOTE_RENAME. */
static int reestablish_kevent(int kq, const char *path, int *fd_out) {
    close(*fd_out);
    usleep(500000);
    return setup_kqueue_kevent(kq, path, fd_out);
}

static void *watch_thread(void *arg) {
    WatchArgs *wa = arg;

    int kq = kqueue();
    if (kq < 0) {
        target_free(&wa->primary);
        if (wa->has_notes) target_free(&wa->notes);
        free(wa);
        return NULL;
    }

    int fd_primary = -1;
    if (setup_kqueue_kevent(kq, wa->primary.full_path, &fd_primary) < 0) {
        close(kq);
        target_free(&wa->primary);
        if (wa->has_notes) target_free(&wa->notes);
        free(wa);
        return NULL;
    }

    int fd_notes = -1;
    int has_notes_kq = 0;
    if (wa->has_notes) {
        has_notes_kq = (setup_kqueue_kevent(kq, wa->notes.full_path, &fd_notes) == 0);
        /* non-fatal if notes file doesn't exist yet */
    }

    /* Two kevents fit in a 2-element array */
    struct kevent change[2];
    size_t nchanges = 1;
    EV_SET(&change[0], fd_primary, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, NULL);
    if (has_notes_kq) {
        nchanges = 2;
        EV_SET(&change[1], fd_notes, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, NULL);
    }
    if (kevent(kq, change, (int)nchanges, NULL, 0, NULL) < 0) {
        close(kq);
        if (fd_primary >= 0) close(fd_primary);
        if (fd_notes >= 0) close(fd_notes);
        target_free(&wa->primary);
        if (wa->has_notes) target_free(&wa->notes);
        free(wa);
        return NULL;
    }

    while (1) {
        struct kevent event;
        int n = kevent(kq, NULL, 0, &event, 1, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char sig = 1;
        if (write(wa->write_fd, &sig, 1) != 1) break;

        if (event.fflags & (NOTE_DELETE | NOTE_RENAME)) {
            if (event.ident == (uintptr_t)fd_primary) {
                reestablish_kevent(kq, wa->primary.full_path, &fd_primary);
            } else if (has_notes_kq && event.ident == (uintptr_t)fd_notes) {
                reestablish_kevent(kq, wa->notes.full_path, &fd_notes);
            }
        }
    }

    close(kq);
    if (fd_primary >= 0) close(fd_primary);
    if (fd_notes >= 0) close(fd_notes);
    target_free(&wa->primary);
    if (wa->has_notes) target_free(&wa->notes);
    free(wa);
    return NULL;
}

#endif /* __APPLE__ */

int file_watch_start(const char *primary_path, const char *notes_path) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return -1;

    WatchArgs *wa = malloc(sizeof(WatchArgs));
    if (!wa) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    wa->primary.full_path = strdup(primary_path);
    if (!wa->primary.full_path) {
        free(wa);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (split_path(primary_path, &wa->primary.dir, &wa->primary.file) != 0) {
        free(wa->primary.full_path);
        free(wa);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    wa->notes.full_path = NULL;
    wa->notes.dir = NULL;
    wa->notes.file = NULL;
    if (notes_path) {
        wa->notes.full_path = strdup(notes_path);
        if (!wa->notes.full_path) {
            target_free(&wa->primary);
            free(wa);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return -1;
        }
        if (split_path(notes_path, &wa->notes.dir, &wa->notes.file) != 0) {
            target_free(&wa->primary);
            free(wa->notes.full_path);
            free(wa);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return -1;
        }
        wa->has_notes = 1;
    } else {
        wa->has_notes = 0;
    }

    wa->write_fd = pipe_fds[1];

    pthread_t tid;
    if (pthread_create(&tid, NULL, watch_thread, wa) != 0) {
        target_free(&wa->primary);
        if (wa->has_notes) target_free(&wa->notes);
        free(wa);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    pthread_detach(tid);

    /* Make read end non-blocking */
    int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);

    return pipe_fds[0];
}


