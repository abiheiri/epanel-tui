#ifndef SAFARI_SYNC_H
#define SAFARI_SYNC_H

#include <pthread.h>
#include "app.h"

/* Parse Safari Bookmarks.plist into folder structures.
   Returns NULL on success, or an error string that must be freed. */
char *safari_import_plist(const char *path, Folder **folders, size_t *count, Folder *reading_list);

/* Write epanel data back to Safari Bookmarks.plist. Returns 0 on success. */
int safari_writeback_plist(const char *path, Folder *root);

/* Watcher state */
typedef struct SafariState {
    int exit_pipe_write;
    pthread_t thread;
} SafariState;

SafariState *safari_state_new(const char *path, int signal_pipe_write);
void safari_state_free(SafariState *s);

#endif
