#ifndef NATIVE_IPC_PIPE_H
#define NATIVE_IPC_PIPE_H

#include <stdbool.h>

// Local connections between the processes of one user: a named pipe on
// Windows, a Unix domain socket on Linux and macOS. See PROTOCOL.md for what
// goes over one and how names map to the OS.
//
// These are the primitives: a listener, the connections it takes, and the
// connections a client makes, each a handle read and written without waiting
// (but for native_ipc_read and native_ipc_write, which wait a bounded time).
// A host runs its own threads over them; Channel.h is a threaded layer over
// them for hosts that would rather not.
//
// A handle is used by one thread at a time; the host serialises its calls on
// one. Different handles are independent.

#ifdef __cplusplus
extern "C" {
#endif

// Why the last native_ipc_listen, native_ipc_accept or native_ipc_connect on
// this thread failed, as native_ipc_last_error() reports it.
#define NATIVE_IPC_ERROR_NONE 0
// The name cannot be used, or something failed that has no better word.
#define NATIVE_IPC_ERROR_FAILED 1
// What is under the name is not this user's own: on Linux and macOS the
// directory names live in, or what is at the name's socket path; on Windows
// the pipe a client found.
#define NATIVE_IPC_ERROR_NOT_OWNED 2
// The peer is not speaking this protocol, or a version of it this is not.
#define NATIVE_IPC_ERROR_PROTOCOL 3
// Another listener of this user's has the name.
#define NATIVE_IPC_ERROR_IN_USE 4
// Nothing listened on the name, or took this client, in the time allowed.
#define NATIVE_IPC_ERROR_TIMEOUT 5

// The protocol version this speaks; see PROTOCOL.md.
#define NATIVE_IPC_PROTOCOL_VERSION 1

// The most a frame's payload may hold, in bytes; see PROTOCOL.md.
#define NATIVE_IPC_MAX_FRAME_SIZE (8 * 1024 * 1024)

// A listener for `name`, or NULL (native_ipc_last_error says why). It holds
// the name until closed: another listen on it fails with
// NATIVE_IPC_ERROR_IN_USE.
void* native_ipc_listen(const char* name);

// A connection a client made to `listener` and has finished its handshake
// on, without waiting; NULL when there is none yet (or one failed its
// handshake and was let go). Each is a handle of its own, closed with
// native_ipc_close; the listener goes on taking the next.
void* native_ipc_accept(void* listener);

// A connection to the listener on `name`, waiting for one to be there and
// take this client for up to `timeoutMs`, or for as long as it takes with 0
// or less; NULL when there is none (native_ipc_last_error says why).
void* native_ipc_connect(const char* name, int timeoutMs);

// Closes a listener or a connection, and frees it.
void native_ipc_close(void* handle);

// The bytes a connection holds to read, or -1 when it is gone.
int native_ipc_bytes_available(void* connection);

// Exactly `size` bytes, ones native_ipc_bytes_available said were there,
// waiting up to five seconds for them: 0 when read, non-zero otherwise.
int native_ipc_read(void* connection, unsigned char* buffer, int size);

// As much as is there, up to `size`, without waiting: the bytes read, 0 when
// there is nothing, -1 when the connection is gone.
int native_ipc_read_some(void* connection, unsigned char* buffer, int size);

// As much of `buffer` as the connection takes now, without waiting: the bytes
// written, 0 when it is full, -1 when the connection is gone.
int native_ipc_write_some(void* connection, const unsigned char* buffer, int size);

// All of `buffer`, waiting up to five seconds: for tests and tools. A host
// writes through native_ipc_write_some.
bool native_ipc_write(void* connection, const unsigned char* buffer, int size);

// Whether a connection is still open.
bool native_ipc_is_open(void* connection);

// Keeps a listener's name from looking unused to a cleaner of old files in
// /tmp; nothing for anything else, and nothing on Windows. A host calls it
// from time to time (each pass of its reader is fine: past a clock read it
// costs a call an hour).
void native_ipc_keep_name(void* handle);

// What a host's thread waits on for `handle`, as a number: on Linux and macOS
// the socket's descriptor; on Windows the doorbell its peer rings as it
// writes, makes room or goes (for a listener, as a client arrives). -1 for
// none, which a wait takes as a plain sleep.
int native_ipc_descriptor_of(void* handle);

// Waits up to `timeoutMs` for `descriptor` to be readable (`read`) or writable
// (`write`): whether it woke for that. On Windows a ring wakes it, and it
// never says it woke for work: a ring is for room made as well as for data,
// so one that finds nothing is no sign of a handle that stays ready.
bool native_ipc_wait_for_work(int descriptor, bool read, bool write, int timeoutMs);

// Why the last listen, accept or connect on this thread failed: one of the
// NATIVE_IPC_ERROR_ values.
int native_ipc_last_error(void);

// Tests only: whether a listener's pipe admits anyone but this user and
// SYSTEM, or is owned by anyone else. Always false off Windows.
bool native_ipc_admits_others_for_test(void* listener);

// Tests only: the buffer size asked of each socket connected or taken from
// now on, in each direction; 0 leaves the system's. Nothing on Windows.
void native_ipc_set_socket_buffer_for_test(int bytes);

#ifdef __cplusplus
}
#endif

#endif
