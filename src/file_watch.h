#ifndef FILE_WATCH_H
#define FILE_WATCH_H

/* Returns the read end of a pipe. The watcher thread writes a byte when either the
   primary data file (epanel.json) or the notes file (notes.txt) changes.
   `notes_path` may be NULL to watch only the primary path. */
int file_watch_start(const char *primary_path, const char *notes_path);

#endif
