#ifndef NATIVE_IPC_INTERNAL_H
#define NATIVE_IPC_INTERNAL_H

#include <native_ipc/Config.h>
#include <native_ipc/Pipe.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace native_ipc
{
	// What every handle begins with, so one passed where the other kind was
	// meant is refused rather than misread.
	enum HandleKind : uint32_t
	{
		KIND_LISTENER = 0x4c495354, // 'LIST'
		KIND_CONNECTION = 0x434f4e4e // 'CONN'
	};

	struct HandleBase
	{
		uint32_t kind;
	};

	inline bool isKind(void* handle, HandleKind kind)
	{
		return handle != nullptr && static_cast<HandleBase*>(handle)->kind == kind;
	}

	// How long a connect waits between tries, and the longest a listener
	// waits for a client that has connected to say hello.
	constexpr int CONNECT_WAIT_SLICE_MS = 50;
	constexpr int HELLO_TIMEOUT_MS = 1000;
	// How long native_ipc_read and native_ipc_write wait at most.
	constexpr int IO_TIMEOUT_MS = 5000;

	// The hello each end sends first: see PROTOCOL.md.
	constexpr int HELLO_SIZE = 16;
	void writeHello(unsigned char* out, uint64_t id);
	// Whether `in` is a hello of this protocol's version, and its id.
	bool readHello(const unsigned char* in, uint64_t* id);
	// An id no other connection of this process has, and almost certainly
	// no other process's.
	uint64_t newConnectionId();

	uint64_t hashName(const std::string& input);
	std::string hex64(uint64_t value);

	void setLastError(int error);
	int lastError();

	// Whether another attempt fits before `deadline`: a connect with no time
	// left makes one attempt, and does not sleep a slice first. A connect
	// with no deadline (`forever`) always has another.
	inline bool anotherTry(bool forever, std::chrono::steady_clock::time_point deadline)
	{
		return forever || std::chrono::steady_clock::now() + std::chrono::milliseconds(CONNECT_WAIT_SLICE_MS) < deadline;
	}

	// Wakes a thread waiting in waitAny from another.
	class Waker
	{
	public:
		Waker();
		~Waker();
		Waker(const Waker&) = delete;
		Waker& operator=(const Waker&) = delete;
		void signal();
		// Takes what signal() left, after a wait it ended.
		void drain();
#if defined(_WIN32)
		HANDLE event;
#else
		int fds[2];
#endif
	};

	namespace detail
	{
		// The public calls without their BlockingScope, for threads no
		// collector stops (the Channel layer's). connect gives up once
		// `cancel` is set, between tries and while waiting for the
		// listener's hello.
		void* connect(const char* name, int timeoutMs, const std::atomic<bool>* cancel);

		// Waits up to `timeoutMs` for `handle` to have something to read (or
		// a client to take, for a listener), when `read`; to have room to
		// write, when `write`; or for `waker` to be signalled. Whether
		// `waker` was.
		bool waitAny(void* handle, bool read, bool write, Waker& waker, int timeoutMs);

		// Whether a listener has clients that have connected and not said
		// hello yet, so that its host should look again soon.
		bool handshaking(void* listener);
	}
}

#endif
