#ifndef FILE_WATCH_H
#define FILE_WATCH_H

/* Returns the read end of a pipe. The watcher thread writes a byte when the directory changes.
   Call file_watch_stop(write_fd) to signal shutdown. */
int file_watch_start(const char *path);
void file_watch_stop(int write_fd);

#endif
