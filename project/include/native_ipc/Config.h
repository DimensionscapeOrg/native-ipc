#ifndef NATIVE_IPC_CONFIG_H
#define NATIVE_IPC_CONFIG_H

// How a host learns that a call is about to block in the OS.
//
// A host with a garbage collector that stops every thread it knows of (hxcpp's
// does) has to be told when one of its threads waits in a call here, or a
// collection another thread starts waits for that thread, and the thread may
// be waiting on the one collecting. NATIVE_IPC_BLOCKING_SCOPE names a type
// whose lifetime is such a wait: constructed as the wait begins, destroyed as
// it ends. Only the public calls that can wait make one; nothing they do
// inside it touches the host's memory.
//
// A host can supply the type itself, from a header named by
// NATIVE_IPC_CONFIG_HEADER, or define NATIVE_IPC_HXCPP for hxcpp's own. Hosts
// that call only from threads of their own that no collector stops (the
// Channel layer's threads among them) need neither: the default does nothing.
#if defined(NATIVE_IPC_CONFIG_HEADER)
#include NATIVE_IPC_CONFIG_HEADER
#elif defined(NATIVE_IPC_HXCPP)
#include <hxcpp.h>
#define NATIVE_IPC_BLOCKING_SCOPE hx::AutoGCFreeZone
#endif

#if defined(__cplusplus)
namespace native_ipc
{
#if defined(NATIVE_IPC_BLOCKING_SCOPE)
	typedef NATIVE_IPC_BLOCKING_SCOPE BlockingScope;
#else
	struct BlockingScope
	{
		// Not trivial, so a scope made for its lifetime alone is not taken
		// for an unused variable.
		BlockingScope()
		{
		}
		~BlockingScope()
		{
		}
	};
#endif
}
#endif

#endif
