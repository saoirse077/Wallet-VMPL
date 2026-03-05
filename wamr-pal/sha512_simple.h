/*
 * sha512_simple.h - Lightweight SHA-512 for bare-metal VMPL1
 *
 * Self-contained implementation with no libc dependencies.
 * Used by Phase 4 differential attestation to compute env_hash
 * in VMPL-1 before writing to the output channel.
 */

#ifndef SHA512_SIMPLE_H
#define SHA512_SIMPLE_H

#include <stdint.h>
#include <stddef.h>

#define SHA512_DIGEST_SIZE 64
#define SHA512_BLOCK_SIZE  128

/*
 * Compute SHA-512 hash of a data buffer.
 *
 * @param data   Pointer to input data
 * @param len    Length of input data in bytes
 * @param digest Output buffer (must be at least 64 bytes)
 */
void sha512_simple(const void *data, uint64_t len, uint8_t digest[SHA512_DIGEST_SIZE]);

#endif /* SHA512_SIMPLE_H */
