#ifndef UPDATE_H
#define UPDATE_H

/* Self-update from GitHub releases. Returns 0 on success. */
int self_update(const char *current_version, const char *target_triple);

#endif
