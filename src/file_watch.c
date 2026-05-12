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

typedef struct {
    char *path;
    int write_fd;
} WatchArgs;

static void *watch_thread(void *arg) {
    WatchArgs *wa = arg;

#ifdef __linux__
    int inotify_fd = inotify_init1(IN_CLOEXEC);
    if (inotify_fd < 0) {
        free(wa->path);
        free(wa);
        return NULL;
    }
    /* Watch the parent directory, not the file itself. inotify binds to the
       inode, so a file-level watch goes deaf after atomic rename-over (as done
       by Syncthing and editors like vim with `backupcopy=no`). Watching the
       directory and filtering by basename survives those replace-in-place
       patterns and also handles the file not existing yet. */
    char *dir_copy = strdup(wa->path);
    if (!dir_copy) {
        close(inotify_fd);
        free(wa->path);
        free(wa);
        return NULL;
    }
    char *sep = strrchr(dir_copy, '/');
    const char *watch_dir;
    const char *watch_name;
    if (sep) {
        *sep = '\0';
        watch_dir = dir_copy[0] ? dir_copy : "/";
        watch_name = sep + 1;
    } else {
        watch_dir = ".";
        watch_name = dir_copy;
    }
    int wd = inotify_add_watch(inotify_fd, watch_dir,
                               IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
        close(inotify_fd);
        free(dir_copy);
        free(wa->path);
        free(wa);
        return NULL;
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
            struct inotify_event *ev = (struct inotify_event *)(buf + off);
            if (ev->len > 0 && strcmp(ev->name, watch_name) == 0) {
                fire = 1;
            }
            off += (ssize_t)sizeof(struct inotify_event) + ev->len;
        }
        if (fire) {
            char sig = 1;
            if (write(wa->write_fd, &sig, 1) != 1) break;
        }
    }

    inotify_rm_watch(inotify_fd, wd);
    close(inotify_fd);
    free(dir_copy);
#endif

#ifdef __APPLE__
    /* On macOS, watch the specific file with kqueue */
    int fd = open(wa->path, O_RDONLY);
    if (fd < 0) {
        free(wa->path);
        free(wa);
        return NULL;
    }
    int kq = kqueue();
    if (kq < 0) {
        close(fd);
        free(wa->path);
        free(wa);
        return NULL;
    }
    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, NULL);
    if (kevent(kq, &change, 1, NULL, 0, NULL) < 0) {
        close(kq);
        close(fd);
        free(wa->path);
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
        /* If file was deleted/renamed, re-establish after a short delay */
        if (event.fflags & (NOTE_DELETE | NOTE_RENAME)) {
            close(fd);
            usleep(500000);
            fd = open(wa->path, O_RDONLY);
            if (fd < 0) continue;
            EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
                   NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, NULL);
            if (kevent(kq, &change, 1, NULL, 0, NULL) < 0) break;
        }
    }

    close(kq);
    close(fd);
#endif

    free(wa->path);
    free(wa);
    return NULL;
}

int file_watch_start(const char *path) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return -1;

    WatchArgs *wa = malloc(sizeof(WatchArgs));
    if (!wa) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    wa->path = strdup(path);
    wa->write_fd = pipe_fds[1];

    pthread_t tid;
    if (pthread_create(&tid, NULL, watch_thread, wa) != 0) {
        free(wa->path);
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

void file_watch_stop(int write_fd) {
    if (write_fd >= 0) {
        close(write_fd);
    }
}
