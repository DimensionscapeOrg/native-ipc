#ifndef NATIVE_IPC_CHANNEL_H
#define NATIVE_IPC_CHANNEL_H

#include <stdbool.h>

// A threaded layer over Pipe.h for hosts that would rather not run threads of
// their own: servers that take any number of clients, and connections that
// send and receive framed messages (PROTOCOL.md), with every blocking wait on
// threads of this library's own.
//
// A host makes one context, and its servers and connections in it. What
// happens to them is queued on the context as events, which the host takes
// with native_ipc_context_poll, on one thread of its own: every call here is
// made from that thread. The context's wake callback, called from this
// library's threads, says that an event is waiting; the host then polls until
// none is left. It is called once, however many events follow, until a poll
// finds the queue empty.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct native_ipc_context native_ipc_context;
// A server, or a connection: one a client made, or one a server took.
typedef struct native_ipc_endpoint native_ipc_endpoint;
typedef void (*native_ipc_wake_callback)(void* userData);

// A client's connection was made.
#define NATIVE_IPC_EVENT_CONNECT 1
// A server took a client: `accepted` is the connection, which is the host's
// to close.
#define NATIVE_IPC_EVENT_ACCEPT 2
// A message arrived: `data` and `length`, valid until the next poll.
#define NATIVE_IPC_EVENT_DATA 3
// Something went wrong: `reason` says what, and `error` (one of Pipe.h's
// NATIVE_IPC_ERROR_ values) why a connect failed. A CLOSE follows.
#define NATIVE_IPC_EVENT_ERROR 4
// The connection ended: the last event it has. It stays the host's to close.
#define NATIVE_IPC_EVENT_CLOSE 5

#define NATIVE_IPC_REASON_NONE 0
// The peer closed it, or went.
#define NATIVE_IPC_REASON_CLOSED 1
// The peer left more than the connection's limit unread.
#define NATIVE_IPC_REASON_OVERFLOW 2
// The peer sent something that is not a frame.
#define NATIVE_IPC_REASON_INVALID_FRAME 3
// The connect failed: `error` says why.
#define NATIVE_IPC_REASON_CONNECT 4

typedef struct native_ipc_event
{
	int type;
	native_ipc_endpoint* endpoint;
	unsigned int id;
	native_ipc_endpoint* accepted;
	unsigned int acceptedId;
	int reason;
	int error;
	const unsigned char* data;
	int length;
} native_ipc_event;

// The default limit on what a connection holds in each direction: two frames
// of the largest size, with their lengths.
#define NATIVE_IPC_DEFAULT_MAX_QUEUED (2 * (8 * 1024 * 1024 + 4))

native_ipc_context* native_ipc_context_create(native_ipc_wake_callback wake, void* userData);
// Closes every endpoint in the context, and frees it.
void native_ipc_context_destroy(native_ipc_context* context);
// The next event, if there is one.
bool native_ipc_context_poll(native_ipc_context* context, native_ipc_event* event);

// A server on `name`, listening by the time this returns; NULL when it cannot
// listen, `error` (if given) saying why.
native_ipc_endpoint* native_ipc_server_open(native_ipc_context* context, const char* name, int* error);
// A connection to the server on `name`, made in the background: a CONNECT
// event when it is, or ERROR and CLOSE when nothing took it within
// `timeoutMs` (0 or less: no limit). What is sent meanwhile goes once it is
// made.
native_ipc_endpoint* native_ipc_client_open(native_ipc_context* context, const char* name, int timeoutMs);

// Sends `length` bytes as one message, without waiting: false when the
// connection has ended, the message is too long (NATIVE_IPC_MAX_FRAME_SIZE),
// or it would take what waits to be sent past the connection's limit, which
// ends the connection with NATIVE_IPC_REASON_OVERFLOW.
bool native_ipc_endpoint_send(native_ipc_endpoint* connection, const unsigned char* data, int length);
// What waits to be sent, framing included.
int native_ipc_endpoint_bytes_pending(native_ipc_endpoint* connection);
// The most a connection holds in each direction: what waits to be sent (see
// native_ipc_endpoint_send) and what has arrived that the host has not
// polled, past which it stops reading and the peer waits. 0: no limit.
void native_ipc_endpoint_set_max_queued(native_ipc_endpoint* connection, int bytes);
bool native_ipc_endpoint_connected(native_ipc_endpoint* connection);
// The endpoint's id, unique in its context, as events carry it.
unsigned int native_ipc_endpoint_id(native_ipc_endpoint* endpoint);
// Closes an endpoint and frees it; it has no events after this. A server's
// clients taken and not yet polled are closed with it.
void native_ipc_endpoint_close(native_ipc_endpoint* endpoint);

#ifdef __cplusplus
}
#endif

#endif
