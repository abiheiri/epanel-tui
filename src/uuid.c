#include "uuid.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

void uuid_gen(char out[37]) {
    unsigned char buf[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        // Fallback to a deterministic pattern (should never happen on Linux/macOS)
        for (int i = 0; i < 16; i++) buf[i] = (unsigned char)(i * 17);
    } else {
        ssize_t n = read(fd, buf, 16);
        (void)n;
        close(fd);
    }
    buf[6] = (buf[6] & 0x0f) | 0x40;
    buf[8] = (buf[8] & 0x3f) | 0x80;
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        buf[0], buf[1], buf[2], buf[3],
        buf[4], buf[5], buf[6], buf[7],
        buf[8], buf[9], buf[10], buf[11],
        buf[12], buf[13], buf[14], buf[15]);
}
