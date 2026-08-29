/* HolyD's standalone syscall shim , the sliver that bmp.c wants.
 *
 * TOS's real lib/syscall.h is the whole kernel interface; bmp.c pulls
 * it in for four calls, and those four are all a host build needs.
 *
 * The prototypes deliberately keep TOS's `long` returns rather than the
 * `int` a C runtime declares. bmp.c is written against these, the C runtime
 * satisfies them at link time, and pulling in <io.h> or <stdio.h> to get the
 * real ones instead is a hard error: MinGW redeclares open/close/unlink with
 * conflicting types the moment either header is visible.
 */
#ifndef HOLYD_STANDALONE_SYSCALL_H
#define HOLYD_STANDALONE_SYSCALL_H

#include <stddef.h>

#ifndef _WIN32
#error "The standalone HolyD build currently targets Windows. On another \
host, bmp.c's non-Windows path wants TOS's fstat_raw, which this shim does \
not provide; see README.md."
#endif

long open(const char *path, int flags);
long read(int fd, void *buf, size_t n);
long close(int fd);
long lseek(int fd, long off, int whence);

#endif
