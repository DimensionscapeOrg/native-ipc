# native-ipc protocol, version 1

This document specifies what goes over a native-ipc connection and what each OS
object is called. Any implementation that follows it can talk to native-ipc,
whatever language or framework it is written in.

All integers are little-endian.

## Connections

A connection is a byte stream between two processes run by the same user on the
same machine:

- **Windows:** a byte-mode named pipe.
- **Linux and macOS:** a `SOCK_STREAM` Unix domain socket.

A listener takes any number of clients, each on a connection of its own.

### Hello

Before anything else, each end sends a 16-byte hello:

| Offset | Size | Field                                      |
|-------:|-----:|--------------------------------------------|
| 0      | 4    | Magic: the ASCII bytes `NIPC`              |
| 4      | 2    | Version: `1`                               |
| 6      | 2    | Flags: `0`; a receiver ignores flags it does not know |
| 8      | 8    | Connection id                              |

The handshake runs in this order:

1. The client sends its hello as soon as it has connected. Its id is any
   nonzero value that its process gives no other open connection to the same
   name.
2. The listener reads the client's hello and answers with its own, echoing the
   client's id.
3. The listener closes a connection whose hello does not arrive within one
   second.
4. If the client's magic or version is not this protocol's, the listener still
   sends its own hello (so a client of another version can tell why), then
   closes the connection.
5. The client closes the connection if the listener's hello has the wrong magic
   or version, or echoes a different id. It reports this as a protocol error.

The version changes only when the format of the stream changes. Any version
other than the receiver's own is refused. Optional behaviour that both ends
can do without is announced with flag bits instead, and needs no new version.

### Frames

After the hellos, each end sends frames:

| Offset | Size     | Field                    |
|-------:|---------:|--------------------------|
| 0      | 4        | Payload length, unsigned |
| 4      | length   | Payload                  |

- A payload may be empty.
- The longest payload allowed is 8 MiB (8,388,608 bytes). A receiver closes a
  connection that announces a longer one.
- A frame is one message. Frames carry no type and no other header: what a
  payload means is the application's business. See
  [Message conventions](#message-conventions) for the one convention in use.

Either end may close the connection at any time. Bytes after the last whole
frame are discarded.

## Names

A name is any string the application chooses. The OS objects for it carry the
user's identity, so processes run by different users never share a name, and
neither user can take over or listen in on the other's.

### Windows

- **Pipe:** `\\.\pipe\native-ipc-<SID>-<name>`, where `<SID>` is the user's SID
  in string form (for example `S-1-5-21-…`).
- **Pipe security:** owned by the user, with a DACL that admits only that user
  and SYSTEM. It is created with `PIPE_REJECT_REMOTE_CLIENTS`.
- **First instance:** the listener creates the first instance with
  `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a second listener on the same name
  fails. It creates each later instance itself.
- **Client checks:** a client opens the pipe with `SECURITY_IDENTIFICATION`, and
  refuses a pipe that another user owns.

Pipes are opened `PIPE_NOWAIT`, which gives a thread nothing to wait on.
Doorbells fill that gap. They are auto-reset events, with the same owner and
DACL as the pipe, named after `<base>` =
`Local\native-ipc-<SID>-<hash>`, where `<hash>` is the 64-bit FNV-1a hash of
the name, as 16 lowercase hex digits:

| Event               | Rung by                                                     | Waited on by       |
|---------------------|-------------------------------------------------------------|--------------------|
| `<base>-a`          | a client, once it has sent its hello                        | the listener       |
| `<base>-<id>-l`     | the client, when it writes, reads half a pipe or more, or closes | the listener's end |
| `<base>-<id>-c`     | the listener's end, when it writes, reads half a pipe or more, or closes | the client     |

Here `<id>` is the connection id from the hello, as 16 lowercase hex digits.
A ring wakes a waiter early. Implementations must still look at the pipe from
time to time, because a doorbell may be missing.

### Linux and macOS

- **Directory:** `/tmp/native-ipc-<uid>`, with `<uid>` the effective user id.
  It must be a directory, not a link, owned by the user, with mode `0700`.
  Whatever finds anything else there refuses to use it.
- **Socket:** `/tmp/native-ipc-<uid>/<socket-name>`. `<socket-name>` is the
  name itself when it is 48 characters or fewer of `[A-Za-z0-9_-]`. Otherwise
  it is the name with every other character replaced by `_`, cut to 31
  characters, followed by `_` and the name's 64-bit FNV-1a hash as 16
  lowercase hex digits. An empty name is `default`.
- **Lock:** a listener holds an exclusive `flock` on `<socket path>.lock`, which
  must be a regular file the user owns, for as long as it listens. A second
  listener on the same name fails to take the lock.
- **Client checks:** a client refuses anything at the socket path that is not a
  socket the user owns.

## Shared memory

A shared-memory region holds one payload, behind a 12-byte header. Every
participant reads and writes the region under its lock.

| Offset | Size | Field                                       |
|-------:|-----:|---------------------------------------------|
| 0      | 4    | Magic `0x4F424A53`                          |
| 4      | 4    | Payload length                              |
| 8      | 4    | Capacity: the most the payload may hold     |
| 12     | …    | Payload                                     |

A region's name is built from `<suffix>`: the name with every character outside
`[A-Za-z0-9_-]` replaced by `_` and cut to 160 characters, then `_` and the
name's FNV-1a hash in hex. An empty name is `default`.

- **Windows:** the file mapping is `Local\native-ipc-shm-<suffix>`. Its lock is
  the mutex `Local\native-ipc-shm-<suffix>-mutex`.
- **Linux:** the region is `shm_open("/native-ipc-shm-<suffix>")`, with mode
  `0600`. Its lock is a `flock` on the region itself.
- **macOS:** shared-memory names are limited to 31 characters, so the region is
  `/nipc_<hash>`. Its lock is a `flock` on `/tmp/nipc_<hash>.lock`, a regular
  file the user owns.

## Message conventions

This section is informative: native-ipc itself does not use it.

CrossByte's `SharedChannel` sends each method call as one frame, with this
payload:

| Size | Field                                             |
|-----:|---------------------------------------------------|
| 4    | Method name length, signed, 1 to 256              |
| n    | Method name, UTF-8                                |
| 4    | Arguments length, signed                          |
| m    | Arguments: an array, in Haxe serialization format |

A payload is limited to 1 MiB.
