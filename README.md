# native-ipc

Local IPC between the processes of one user, on Windows, macOS and Linux:
connections with framed messages, and shared memory. The C++ implementation
depends on nothing beyond the OS and the C++11 standard library: no Haxe, no
Lime, no SDL, no CrossByte. Hosts compile it into their own binaries.
[PROTOCOL.md](PROTOCOL.md) specifies the wire format and the names, so any
implementation of it can join in.

Windows uses named pipes; Linux and macOS use Unix domain sockets. Either way, a
name belongs to the user who opened it. Processes run by different users never
meet under the same name, and neither can listen on, or take over, the other's.

## Three layers

**`native_ipc/Pipe.h`: the primitives.**
- Listeners, the connections they accept, and connections a client opens.
- Every call returns without waiting, except two (`native_ipc_read` and
  `native_ipc_write`) that wait a bounded time.
- `native_ipc_descriptor_of` and `native_ipc_wait_for_work` let a host's own
  thread wait for work.
- This is the layer CrossByte runs its own reader threads over.

**`native_ipc/Channel.h`: threaded connections.**
- Servers that accept any number of clients, and connections that send and
  receive whole messages.
- The library runs one thread per server and per connection.
- The host takes events on a thread of its own, after a wake callback says some
  are waiting.
- Lime builds `lime.net.LocalServer` and `lime.net.LocalSocket` on this layer.

**`native_ipc/SharedMemory.h`: shared memory.**
- A named region holding one payload, which participants write whole and read
  whole under a lock they all take.

## Using it

Install it:

```sh
haxelib install native-ipc
```

For development, clone this repository and register it:

```sh
haxelib dev native-ipc /path/to/native-ipc
```

- **hxcpp programs:** include `project/Build.xml` from a class's `@:buildXml`.
  That compiles all three layers with `NATIVE_IPC_HXCPP`, so calls that wait
  happen outside hxcpp's collector. Leave a layer out with
  `NATIVE_IPC_NO_CHANNEL` or `NATIVE_IPC_NO_SHARED_MEMORY`.
- **Other builds:** include `project/Files.xml` and use its file groups
  `native-ipc`, `native-ipc-channel` and `native-ipc-shared-memory`, or build
  with CMake.
- **Windows linking:** link `advapi32`.

See [Config.h](project/include/native_ipc/Config.h) for how a host with a
garbage collector is told that a call blocks.

```cpp
#include <native_ipc/Channel.h>

static void wake (void* host) { /* ask the host's loop to poll */ }

native_ipc_context* context = native_ipc_context_create (wake, host);
native_ipc_endpoint* server = native_ipc_server_open (context, "my-app", NULL);
native_ipc_endpoint* client = native_ipc_client_open (context, "my-app", 5000);
native_ipc_endpoint_send (client, (const unsigned char*)"hello", 5);

native_ipc_event event;
while (native_ipc_context_poll (context, &event)) {
	switch (event.type) {
		case NATIVE_IPC_EVENT_ACCEPT: /* event.accepted is a new connection */ break;
		case NATIVE_IPC_EVENT_DATA: /* event.data, event.length */ break;
		case NATIVE_IPC_EVENT_CLOSE: native_ipc_endpoint_close (event.endpoint); break;
	}
}

native_ipc_context_destroy (context);
```

## Tests

```sh
cmake -S . -B tests/bin/native -DBUILD_TESTING=ON
cmake --build tests/bin/native --config Debug
ctest --test-dir tests/bin/native -C Debug --output-on-failure
```

The tests cover:
- round trips and peer loss;
- names in use, and connect timeouts;
- servers with many clients;
- clients that don't speak the protocol, or never say hello;
- framing, overflow, back pressure and wake coalescing in the Channel layer;
- shared memory, including a lock held past its deadline.

CI runs them on Windows, macOS and Linux.

## License

Apache 2.0. See [LICENSE](LICENSE).
