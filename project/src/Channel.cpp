#include "Internal.h"

#include <native_ipc/Channel.h>

#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace
{
	using namespace native_ipc;

	// How much a connection's thread reads at a time.
	constexpr int READ_SIZE = 65536;
	// How long a connection's thread waits at most between looks: with
	// something to write, for room (which a Windows peer rings only for a
	// read of half a pipe or more); with nothing, for its peer, whose writes
	// and leaving always wake it.
	constexpr int WRITE_WAIT_MAX_MS = 10;
	constexpr int READ_WAIT_MAX_MS = 100;
	// How long a server's thread waits at most between looks: briefly while
	// a client has connected and not said hello, which nothing wakes it for.
	constexpr int HELLO_WAIT_MS = 5;
	constexpr int ACCEPT_WAIT_MS = 250;

	enum State
	{
		STATE_CONNECTING,
		STATE_CONNECTED,
		STATE_ENDED
	};

	struct Queued
	{
		int type;
		native_ipc_endpoint* endpoint;
		native_ipc_endpoint* accepted;
		int reason;
		int error;
		std::vector<unsigned char> data;
	};
}

struct native_ipc_endpoint
{
	native_ipc_context* context;
	unsigned int id;
	bool server;
	std::string name;
	int timeoutMs;

	std::thread thread;
	std::atomic<bool> stop;
	Waker waker;

	// Guards everything below but the framing, which only the thread touches.
	std::mutex lock;
	// The listener or connection, once there is one.
	void* handle;
	std::atomic<int> state;
	std::vector<unsigned char> out;
	size_t outSent;
	std::atomic<int> maxQueued;
	// Set by a send that passed the limit: the thread ends the connection.
	std::atomic<bool> overflow;
	// What has been queued for the host and not polled, in payload bytes.
	std::atomic<long long> inQueued;

	unsigned char header[4];
	int headerGot;
	unsigned int frameLength;
	std::vector<unsigned char> frame;

	native_ipc_endpoint() :
		context(nullptr),
		id(0),
		server(false),
		timeoutMs(0),
		stop(false),
		handle(nullptr),
		state(STATE_CONNECTING),
		outSent(0),
		maxQueued(NATIVE_IPC_DEFAULT_MAX_QUEUED),
		overflow(false),
		inQueued(0),
		headerGot(0),
		frameLength(0)
	{
	}
};

struct native_ipc_context
{
	std::mutex lock;
	std::deque<Queued> queue;
	native_ipc_wake_callback wake;
	void* userData;
	// Whether the host was woken and has not yet found the queue empty.
	bool wakePending;
	// The payload of the event polled last, held until the next poll.
	std::vector<unsigned char> current;
	std::set<native_ipc_endpoint*> endpoints;
	unsigned int nextId;
};

namespace
{
	void push(native_ipc_context* context, Queued&& queued)
	{
		bool call = false;
		{
			std::lock_guard<std::mutex> guard(context->lock);
			context->queue.push_back(std::move(queued));
			if (!context->wakePending)
			{
				context->wakePending = true;
				call = true;
			}
		}
		if (call && context->wake != nullptr)
		{
			context->wake(context->userData);
		}
	}

	void pushEvent(native_ipc_endpoint* endpoint, int type, int reason = NATIVE_IPC_REASON_NONE, int error = NATIVE_IPC_ERROR_NONE)
	{
		Queued queued;
		queued.type = type;
		queued.endpoint = endpoint;
		queued.accepted = nullptr;
		queued.reason = reason;
		queued.error = error;
		push(endpoint->context, std::move(queued));
	}

	bool inboundFull(native_ipc_endpoint* endpoint)
	{
		return endpoint->maxQueued > 0 && endpoint->inQueued.load() >= endpoint->maxQueued;
	}

	size_t pendingOf(native_ipc_endpoint* endpoint)
	{
		return endpoint->out.size() - endpoint->outSent;
	}

	// Lets go of what has been written: all of it once all is, and otherwise
	// only once it is at least as much as what is left, so no byte is moved
	// more often than bytes are written. Under the endpoint's lock.
	void compactOut(native_ipc_endpoint* endpoint)
	{
		size_t pending = pendingOf(endpoint);
		if (pending == 0)
		{
			endpoint->out.clear();
			endpoint->outSent = 0;
		}
		else if (endpoint->outSent >= pending)
		{
			endpoint->out.erase(endpoint->out.begin(), endpoint->out.begin() + static_cast<std::ptrdiff_t>(endpoint->outSent));
			endpoint->outSent = 0;
		}
	}

	// Writes what the connection takes now: false when it has gone. Under
	// the endpoint's lock.
	bool flushOut(native_ipc_endpoint* endpoint, bool* wrote)
	{
		size_t pending = pendingOf(endpoint);
		if (pending == 0 || endpoint->handle == nullptr)
		{
			return true;
		}
		int size = pending > static_cast<size_t>(1 << 30) ? (1 << 30) : static_cast<int>(pending);
		int written = native_ipc_write_some(endpoint->handle, endpoint->out.data() + endpoint->outSent, size);
		if (written < 0)
		{
			return false;
		}
		if (written > 0)
		{
			endpoint->outSent += static_cast<size_t>(written);
			*wrote = true;
			compactOut(endpoint);
		}
		return true;
	}

	void deliver(native_ipc_endpoint* endpoint, std::vector<unsigned char>&& payload)
	{
		endpoint->inQueued += static_cast<long long>(payload.size());
		Queued queued;
		queued.type = NATIVE_IPC_EVENT_DATA;
		queued.endpoint = endpoint;
		queued.accepted = nullptr;
		queued.reason = NATIVE_IPC_REASON_NONE;
		queued.error = NATIVE_IPC_ERROR_NONE;
		queued.data = std::move(payload);
		push(endpoint->context, std::move(queued));
	}

	// Frames what was read, from where the last read stopped, and hands on
	// each whole frame: false for one that cannot be (PROTOCOL.md).
	bool frameInput(native_ipc_endpoint* endpoint, const unsigned char* data, int size)
	{
		while (size > 0)
		{
			if (endpoint->headerGot < 4)
			{
				int take = 4 - endpoint->headerGot;
				take = take < size ? take : size;
				std::memcpy(endpoint->header + endpoint->headerGot, data, static_cast<size_t>(take));
				endpoint->headerGot += take;
				data += take;
				size -= take;
				if (endpoint->headerGot < 4)
				{
					break;
				}
				unsigned int length = static_cast<unsigned int>(endpoint->header[0]) | (static_cast<unsigned int>(endpoint->header[1]) << 8)
					| (static_cast<unsigned int>(endpoint->header[2]) << 16) | (static_cast<unsigned int>(endpoint->header[3]) << 24);
				if (length > static_cast<unsigned int>(NATIVE_IPC_MAX_FRAME_SIZE))
				{
					return false;
				}
				endpoint->frameLength = length;
				endpoint->frame.clear();
				endpoint->frame.reserve(length);
				if (length == 0)
				{
					endpoint->headerGot = 0;
					deliver(endpoint, std::vector<unsigned char>());
				}
				continue;
			}

			size_t wanted = endpoint->frameLength - endpoint->frame.size();
			size_t take = wanted < static_cast<size_t>(size) ? wanted : static_cast<size_t>(size);
			endpoint->frame.insert(endpoint->frame.end(), data, data + take);
			data += take;
			size -= static_cast<int>(take);
			if (endpoint->frame.size() == endpoint->frameLength)
			{
				endpoint->headerGot = 0;
				deliver(endpoint, std::move(endpoint->frame));
				endpoint->frame = std::vector<unsigned char>();
			}
		}
		return true;
	}

	void runConnection(native_ipc_endpoint* endpoint)
	{
		std::vector<unsigned char> buffer(READ_SIZE);
		int idleMs = 1;
		int reason = NATIVE_IPC_REASON_CLOSED;
		bool failed = false;

		while (!endpoint->stop.load())
		{
			if (endpoint->overflow.load())
			{
				reason = NATIVE_IPC_REASON_OVERFLOW;
				failed = true;
				break;
			}

			bool busy = false;
			bool wantWrite = false;
			{
				std::lock_guard<std::mutex> guard(endpoint->lock);
				if (!flushOut(endpoint, &busy))
				{
					break;
				}
				wantWrite = pendingOf(endpoint) > 0;
			}

			bool full = inboundFull(endpoint);
			if (!full)
			{
				int read = native_ipc_read_some(endpoint->handle, buffer.data(), READ_SIZE);
				if (read < 0)
				{
					break;
				}
				if (read > 0)
				{
					busy = true;
					if (!frameInput(endpoint, buffer.data(), read))
					{
						reason = NATIVE_IPC_REASON_INVALID_FRAME;
						failed = true;
						break;
					}
				}
			}

			// A pass that read or wrote is followed by another at once, which
			// finds out whether there is more; then longer and longer waits.
			if (busy)
			{
				idleMs = 1;
				continue;
			}
			int limit = wantWrite ? WRITE_WAIT_MAX_MS : READ_WAIT_MAX_MS;
			int waitMs = idleMs < limit ? idleMs : limit;
			detail::waitAny(endpoint->handle, !inboundFull(endpoint), wantWrite, endpoint->waker, waitMs);
			idleMs = idleMs * 2 > READ_WAIT_MAX_MS ? READ_WAIT_MAX_MS : idleMs * 2;
		}

		// Its handle closed now, so its peer learns it has gone at once,
		// rather than when the host closes it.
		{
			std::lock_guard<std::mutex> guard(endpoint->lock);
			endpoint->state = STATE_ENDED;
			if (endpoint->handle != nullptr)
			{
				native_ipc_close(endpoint->handle);
				endpoint->handle = nullptr;
			}
			endpoint->out.clear();
			endpoint->outSent = 0;
		}
		if (!endpoint->stop.load())
		{
			if (failed)
			{
				pushEvent(endpoint, NATIVE_IPC_EVENT_ERROR, reason);
			}
			pushEvent(endpoint, NATIVE_IPC_EVENT_CLOSE, reason);
		}
	}

	native_ipc_endpoint* makeEndpoint(native_ipc_context* context, bool server)
	{
		auto* endpoint = new native_ipc_endpoint();
		endpoint->context = context;
		endpoint->server = server;
		std::lock_guard<std::mutex> guard(context->lock);
		endpoint->id = ++context->nextId;
		if (endpoint->id == 0)
		{
			endpoint->id = ++context->nextId;
		}
		context->endpoints.insert(endpoint);
		return endpoint;
	}

	void runServer(native_ipc_endpoint* server)
	{
		while (!server->stop.load())
		{
			void* taken = native_ipc_accept(server->handle);
			if (taken != nullptr)
			{
				native_ipc_endpoint* connection = makeEndpoint(server->context, false);
				connection->handle = taken;
				connection->state = STATE_CONNECTED;
				// The ACCEPT queued before the connection's thread can queue
				// anything of its own, and both under the connection's lock,
				// which a host closing it as soon as it polls the ACCEPT takes
				// first.
				std::lock_guard<std::mutex> guard(connection->lock);
				Queued queued;
				queued.type = NATIVE_IPC_EVENT_ACCEPT;
				queued.endpoint = server;
				queued.accepted = connection;
				queued.reason = NATIVE_IPC_REASON_NONE;
				queued.error = NATIVE_IPC_ERROR_NONE;
				push(server->context, std::move(queued));
				connection->thread = std::thread(runConnection, connection);
				continue;
			}
			native_ipc_keep_name(server->handle);
			int waitMs = detail::handshaking(server->handle) ? HELLO_WAIT_MS : ACCEPT_WAIT_MS;
			detail::waitAny(server->handle, true, false, server->waker, waitMs);
		}
	}

	void runClient(native_ipc_endpoint* endpoint)
	{
		void* handle = detail::connect(endpoint->name.c_str(), endpoint->timeoutMs, &endpoint->stop);
		int error = lastError();
		if (endpoint->stop.load())
		{
			if (handle != nullptr)
			{
				native_ipc_close(handle);
			}
			return;
		}
		if (handle == nullptr)
		{
			{
				std::lock_guard<std::mutex> guard(endpoint->lock);
				endpoint->state = STATE_ENDED;
				endpoint->out.clear();
				endpoint->outSent = 0;
			}
			pushEvent(endpoint, NATIVE_IPC_EVENT_ERROR, NATIVE_IPC_REASON_CONNECT, error == NATIVE_IPC_ERROR_NONE ? NATIVE_IPC_ERROR_FAILED : error);
			pushEvent(endpoint, NATIVE_IPC_EVENT_CLOSE, NATIVE_IPC_REASON_CONNECT);
			return;
		}
		{
			std::lock_guard<std::mutex> guard(endpoint->lock);
			endpoint->handle = handle;
			endpoint->state = STATE_CONNECTED;
		}
		pushEvent(endpoint, NATIVE_IPC_EVENT_CONNECT);
		runConnection(endpoint);
	}

	void closeEndpoint(native_ipc_endpoint* endpoint)
	{
		native_ipc_context* context = endpoint->context;
		{
			std::lock_guard<std::mutex> guard(endpoint->lock);
			endpoint->stop = true;
		}
		endpoint->waker.signal();
		if (endpoint->thread.joinable())
		{
			endpoint->thread.join();
		}

		// Nothing more is queued for it now. What is queued goes, and a
		// server's clients the host never heard of go with it.
		std::vector<native_ipc_endpoint*> orphans;
		{
			std::lock_guard<std::mutex> guard(context->lock);
			for (auto it = context->queue.begin(); it != context->queue.end();)
			{
				if (it->endpoint == endpoint)
				{
					if (it->accepted != nullptr)
					{
						orphans.push_back(it->accepted);
					}
					it = context->queue.erase(it);
				}
				else
				{
					++it;
				}
			}
			context->endpoints.erase(endpoint);
		}
		for (native_ipc_endpoint* orphan : orphans)
		{
			closeEndpoint(orphan);
		}

		if (endpoint->handle != nullptr)
		{
			native_ipc_close(endpoint->handle);
			endpoint->handle = nullptr;
		}
		delete endpoint;
	}
}

extern "C" native_ipc_context* native_ipc_context_create(native_ipc_wake_callback wake, void* userData)
{
	auto* context = new native_ipc_context();
	context->wake = wake;
	context->userData = userData;
	context->wakePending = false;
	context->nextId = 0;
	return context;
}

extern "C" void native_ipc_context_destroy(native_ipc_context* context)
{
	if (context == nullptr)
	{
		return;
	}
	while (true)
	{
		native_ipc_endpoint* endpoint = nullptr;
		{
			std::lock_guard<std::mutex> guard(context->lock);
			if (context->endpoints.empty())
			{
				break;
			}
			endpoint = *context->endpoints.begin();
		}
		closeEndpoint(endpoint);
	}
	delete context;
}

extern "C" bool native_ipc_context_poll(native_ipc_context* context, native_ipc_event* event)
{
	if (context == nullptr || event == nullptr)
	{
		return false;
	}
	Queued queued;
	{
		std::lock_guard<std::mutex> guard(context->lock);
		if (context->queue.empty())
		{
			context->wakePending = false;
			context->current.clear();
			return false;
		}
		queued = std::move(context->queue.front());
		context->queue.pop_front();
	}

	context->current = std::move(queued.data);
	native_ipc_endpoint* endpoint = queued.endpoint;
	if (queued.type == NATIVE_IPC_EVENT_DATA)
	{
		long long length = static_cast<long long>(context->current.size());
		long long before = endpoint->inQueued.fetch_sub(length);
		// Room again for a connection that had stopped reading.
		if (endpoint->maxQueued > 0 && before >= endpoint->maxQueued && before - length < endpoint->maxQueued)
		{
			endpoint->waker.signal();
		}
	}

	event->type = queued.type;
	event->endpoint = endpoint;
	event->id = endpoint->id;
	event->accepted = queued.accepted;
	event->acceptedId = queued.accepted != nullptr ? queued.accepted->id : 0;
	event->reason = queued.reason;
	event->error = queued.error;
	event->data = context->current.empty() ? nullptr : context->current.data();
	event->length = static_cast<int>(context->current.size());
	return true;
}

extern "C" native_ipc_endpoint* native_ipc_server_open(native_ipc_context* context, const char* name, int* error)
{
	if (context == nullptr)
	{
		if (error != nullptr)
		{
			*error = NATIVE_IPC_ERROR_FAILED;
		}
		return nullptr;
	}
	void* listener = native_ipc_listen(name);
	if (error != nullptr)
	{
		*error = native_ipc_last_error();
	}
	if (listener == nullptr)
	{
		return nullptr;
	}
	native_ipc_endpoint* server = makeEndpoint(context, true);
	server->name = name == nullptr ? "" : name;
	server->handle = listener;
	server->state = STATE_CONNECTED;
	server->thread = std::thread(runServer, server);
	return server;
}

extern "C" native_ipc_endpoint* native_ipc_client_open(native_ipc_context* context, const char* name, int timeoutMs)
{
	if (context == nullptr)
	{
		return nullptr;
	}
	native_ipc_endpoint* endpoint = makeEndpoint(context, false);
	endpoint->name = name == nullptr ? "" : name;
	endpoint->timeoutMs = timeoutMs;
	endpoint->thread = std::thread(runClient, endpoint);
	return endpoint;
}

extern "C" bool native_ipc_endpoint_send(native_ipc_endpoint* endpoint, const unsigned char* data, int length)
{
	if (endpoint == nullptr || endpoint->server || length < 0 || length > NATIVE_IPC_MAX_FRAME_SIZE || (length > 0 && data == nullptr))
	{
		return false;
	}

	bool signal = false;
	{
		std::lock_guard<std::mutex> guard(endpoint->lock);
		if (endpoint->state == STATE_ENDED || endpoint->stop.load() || endpoint->overflow.load())
		{
			return false;
		}
		size_t pending = pendingOf(endpoint);
		// Only with something already waiting: a frame larger than the limit
		// still goes to a peer that has kept up. Against what is left of the
		// limit, so a frame under a limit near the largest int cannot wrap.
		if (endpoint->maxQueued > 0 && pending > 0 && static_cast<long long>(length) + 4 > static_cast<long long>(endpoint->maxQueued.load()) - static_cast<long long>(pending))
		{
			endpoint->overflow = true;
			signal = true;
		}
		else
		{
			unsigned char header[4];
			unsigned int value = static_cast<unsigned int>(length);
			header[0] = static_cast<unsigned char>(value & 0xff);
			header[1] = static_cast<unsigned char>((value >> 8) & 0xff);
			header[2] = static_cast<unsigned char>((value >> 16) & 0xff);
			header[3] = static_cast<unsigned char>((value >> 24) & 0xff);
			endpoint->out.insert(endpoint->out.end(), header, header + 4);
			if (length > 0)
			{
				endpoint->out.insert(endpoint->out.end(), data, data + length);
			}
			// Written now if nothing was waiting before it, without a trip
			// through the connection's thread; otherwise that thread is
			// writing already.
			if (pending == 0 && endpoint->state == STATE_CONNECTED)
			{
				bool wrote = false;
				flushOut(endpoint, &wrote);
			}
			signal = pendingOf(endpoint) > 0;
		}
	}
	if (signal)
	{
		endpoint->waker.signal();
	}
	return !endpoint->overflow.load();
}

extern "C" int native_ipc_endpoint_bytes_pending(native_ipc_endpoint* endpoint)
{
	if (endpoint == nullptr)
	{
		return 0;
	}
	std::lock_guard<std::mutex> guard(endpoint->lock);
	size_t pending = pendingOf(endpoint);
	return pending > 0x7fffffff ? 0x7fffffff : static_cast<int>(pending);
}

extern "C" void native_ipc_endpoint_set_max_queued(native_ipc_endpoint* endpoint, int bytes)
{
	if (endpoint == nullptr)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> guard(endpoint->lock);
		endpoint->maxQueued = bytes < 0 ? 0 : bytes;
	}
	// A connection that had stopped reading may read again.
	endpoint->waker.signal();
}

extern "C" bool native_ipc_endpoint_connected(native_ipc_endpoint* endpoint)
{
	return endpoint != nullptr && !endpoint->server && endpoint->state == STATE_CONNECTED;
}

extern "C" unsigned int native_ipc_endpoint_id(native_ipc_endpoint* endpoint)
{
	return endpoint == nullptr ? 0 : endpoint->id;
}

extern "C" void native_ipc_endpoint_close(native_ipc_endpoint* endpoint)
{
	if (endpoint != nullptr)
	{
		closeEndpoint(endpoint);
	}
}
