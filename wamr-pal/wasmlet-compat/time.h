/*
 * time.h compat shim for wasmlet on VMPL1
 *
 * All time operations go through wasmlet_platform.h.
 * This header is empty — it exists only to satisfy #include <time.h>.
 */

#ifndef _WASMLET_COMPAT_TIME_H
#define _WASMLET_COMPAT_TIME_H

/* time_t replaced by uint64_t in wasmlet types.h */

#endif /* _WASMLET_COMPAT_TIME_H */
