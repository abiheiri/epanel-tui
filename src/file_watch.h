#ifndef FILE_WATCH_H
#define FILE_WATCH_H

/* Returns the read end of a pipe. The watcher thread writes a byte when the directory changes. */
int file_watch_start(const char *path);

#endif
