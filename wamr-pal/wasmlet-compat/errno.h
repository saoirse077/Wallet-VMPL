/*
 * errno.h compat shim for wasmlet on VMPL1
 */

#ifndef _WASMLET_COMPAT_ERRNO_H
#define _WASMLET_COMPAT_ERRNO_H

extern int errno;

#define ENOMEM  12
#define EINVAL  22
#define ENOENT   2
#define ENOSYS  38
#define EBUSY   16
#define EAGAIN  11

#endif /* _WASMLET_COMPAT_ERRNO_H */
