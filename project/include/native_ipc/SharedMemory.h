#ifndef NATIVE_IPC_SHARED_MEMORY_H
#define NATIVE_IPC_SHARED_MEMORY_H

#include <stdbool.h>

// A named region of shared memory holding one payload, which the processes
// of one user write whole and read whole under a lock they all take. See
// PROTOCOL.md for its layout and names.

#ifdef __cplusplus
extern "C" {
#endif

// Why the last call on this thread failed, as native_ipc_shm_last_error()
// reports it.
#define NATIVE_IPC_SHM_ERROR_NONE 0
// Another participant held the region's lock past the call's deadline.
#define NATIVE_IPC_SHM_ERROR_LOCK_TIMEOUT 1
// Anything else: the region could not be made, mapped, locked or read.
#define NATIVE_IPC_SHM_ERROR_FAILED 2
// Linux and macOS: what is under the name (the region, or on macOS its
// lock file) is not this user's own: another user made it, or it is a
// link, a directory or anything else that is not one of ours.
#define NATIVE_IPC_SHM_ERROR_NOT_OWNED 3

// Each call that takes the region's lock waits at most lockTimeoutMs for it,
// or for as long as it takes with 0 or less.
void* native_ipc_shm_open(const char* name, int maxSize, int lockTimeoutMs);
void native_ipc_shm_close(void* handle);
// The payload's length, and the payload copied into `buffer` when it fits
// there; -1 when it cannot be read.
int native_ipc_shm_read(void* handle, unsigned char* buffer, int bufferSize, int lockTimeoutMs);
bool native_ipc_shm_write(void* handle, const unsigned char* data, int dataSize, int lockTimeoutMs);
// Whether the region's lock was taken; a region not set up as one of ours is
// left as it is.
bool native_ipc_shm_clear(void* handle, int lockTimeoutMs);
int native_ipc_shm_capacity(void* handle, int lockTimeoutMs);
int native_ipc_shm_last_error(void);
// 1 when a region had the name and lost it, 0 when none had it (and always on
// Windows), -1 when it could not be done.
int native_ipc_shm_remove(const char* name, int lockTimeoutMs);

// Tests only: takes the region's lock on the calling thread and keeps it
// until native_ipc_shm_release_lock_for_test, called on the same thread.
bool native_ipc_shm_hold_lock_for_test(void* handle);
void native_ipc_shm_release_lock_for_test(void* handle);

#ifdef __cplusplus
}
#endif

#endif
