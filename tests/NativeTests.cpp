#include <native_ipc/Channel.h>
#include <native_ipc/Pipe.h>
#include <native_ipc/SharedMemory.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace
{
	int failures = 0;
	int checks = 0;

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

	void check(bool condition, const char* text, const char* file, int line)
	{
		checks++;
		if (!condition)
		{
			failures++;
			std::fprintf(stderr, "FAILED %s:%d: %s\n", file, line, text);
		}
	}

	int processId()
	{
#if defined(_WIN32)
		return static_cast<int>(GetCurrentProcessId());
#else
		return static_cast<int>(getpid());
#endif
	}

	std::string uniqueName(const char* what)
	{
		static int counter = 0;
		return std::string("nipc-test-") + what + "-" + std::to_string(processId()) + "-" + std::to_string(++counter);
	}

	using Clock = std::chrono::steady_clock;

	bool waitFor(const std::function<bool()>& done, int timeoutMs = 5000)
	{
		auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
		while (!done())
		{
			if (Clock::now() >= deadline)
			{
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}

	// A connection made on another thread while this one accepts it: a
	// connect waits for the listener's hello, which only an accept sends.
	struct Pair
	{
		void* listener = nullptr;
		void* client = nullptr;
		void* server = nullptr;
	};

	Pair connectPair(const std::string& name)
	{
		Pair pair;
		pair.listener = native_ipc_listen(name.c_str());
		if (pair.listener == nullptr)
		{
			return pair;
		}
		std::thread connecting([&]() { pair.client = native_ipc_connect(name.c_str(), 3000); });
		waitFor([&]() {
			if (pair.server == nullptr)
			{
				pair.server = native_ipc_accept(pair.listener);
			}
			return pair.server != nullptr;
		});
		connecting.join();
		return pair;
	}

	void closePair(Pair& pair)
	{
		native_ipc_close(pair.client);
		native_ipc_close(pair.server);
		native_ipc_close(pair.listener);
	}

	bool writeAll(void* connection, const std::vector<unsigned char>& data)
	{
		size_t sent = 0;
		auto deadline = Clock::now() + std::chrono::seconds(5);
		while (sent < data.size())
		{
			int written = native_ipc_write_some(connection, data.data() + sent, static_cast<int>(data.size() - sent));
			if (written < 0 || Clock::now() >= deadline)
			{
				return false;
			}
			sent += static_cast<size_t>(written);
			if (written == 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		return true;
	}

	bool readAll(void* connection, std::vector<unsigned char>& into, size_t size)
	{
		into.resize(size);
		size_t got = 0;
		auto deadline = Clock::now() + std::chrono::seconds(5);
		while (got < size)
		{
			int read = native_ipc_read_some(connection, into.data() + got, static_cast<int>(size - got));
			if (read < 0 || Clock::now() >= deadline)
			{
				return false;
			}
			got += static_cast<size_t>(read);
			if (read == 0)
			{
				native_ipc_wait_for_work(native_ipc_descriptor_of(connection), true, false, 5);
			}
		}
		return true;
	}

	std::vector<unsigned char> pattern(size_t size, unsigned seed)
	{
		std::vector<unsigned char> data(size);
		for (size_t i = 0; i < size; i++)
		{
			data[i] = static_cast<unsigned char>((i * 131 + seed * 7) & 0xff);
		}
		return data;
	}

	// Primitives.

	void testRoundTrip()
	{
		std::string name = uniqueName("roundtrip");
		Pair pair = connectPair(name);
		CHECK(pair.listener != nullptr);
		CHECK(pair.client != nullptr);
		CHECK(pair.server != nullptr);
		if (pair.client == nullptr || pair.server == nullptr)
		{
			closePair(pair);
			return;
		}
		CHECK(native_ipc_is_open(pair.client));
		CHECK(native_ipc_is_open(pair.server));

		std::vector<unsigned char> toServer = pattern(200000, 1);
		std::vector<unsigned char> received;
		std::thread writer([&]() { CHECK(writeAll(pair.client, toServer)); });
		CHECK(readAll(pair.server, received, toServer.size()));
		writer.join();
		CHECK(received == toServer);

		std::vector<unsigned char> toClient = pattern(1000, 2);
		CHECK(native_ipc_write(pair.server, toClient.data(), static_cast<int>(toClient.size())));
		CHECK(waitFor([&]() { return native_ipc_bytes_available(pair.client) >= static_cast<int>(toClient.size()); }));
		std::vector<unsigned char> back(toClient.size());
		CHECK(native_ipc_read(pair.client, back.data(), static_cast<int>(back.size())) == 0);
		CHECK(back == toClient);

		// The peer going is seen.
		native_ipc_close(pair.client);
		pair.client = nullptr;
		unsigned char probe[16];
		CHECK(waitFor([&]() { return native_ipc_read_some(pair.server, probe, sizeof(probe)) < 0; }));
		closePair(pair);
	}

	void testNameInUse()
	{
		std::string name = uniqueName("inuse");
		void* first = native_ipc_listen(name.c_str());
		CHECK(first != nullptr);
		void* second = native_ipc_listen(name.c_str());
		CHECK(second == nullptr);
		CHECK(native_ipc_last_error() == NATIVE_IPC_ERROR_IN_USE);
		native_ipc_close(first);
		// Free again once closed.
		void* third = native_ipc_listen(name.c_str());
		CHECK(third != nullptr);
		native_ipc_close(third);
	}

	void testConnectTimeout()
	{
		std::string name = uniqueName("nobody");
		auto start = Clock::now();
		void* client = native_ipc_connect(name.c_str(), 150);
		auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
		CHECK(client == nullptr);
		CHECK(native_ipc_last_error() == NATIVE_IPC_ERROR_TIMEOUT);
		CHECK(took < 2000);
	}

	void testManyClients()
	{
		std::string name = uniqueName("many");
		void* listener = native_ipc_listen(name.c_str());
		CHECK(listener != nullptr);
		const int count = 5;
		std::vector<void*> clients(count, nullptr);
		std::vector<std::thread> threads;
		for (int i = 0; i < count; i++)
		{
			threads.emplace_back([&, i]() {
				clients[i] = native_ipc_connect(name.c_str(), 3000);
				if (clients[i] != nullptr)
				{
					unsigned char byte = static_cast<unsigned char>(i);
					native_ipc_write(clients[i], &byte, 1);
				}
			});
		}
		std::vector<void*> taken;
		waitFor([&]() {
			void* connection = native_ipc_accept(listener);
			if (connection != nullptr)
			{
				taken.push_back(connection);
			}
			return static_cast<int>(taken.size()) == count;
		});
		for (std::thread& thread : threads)
		{
			thread.join();
		}
		CHECK(static_cast<int>(taken.size()) == count);
		std::vector<bool> seen(count, false);
		for (void* connection : taken)
		{
			std::vector<unsigned char> byte;
			if (readAll(connection, byte, 1) && byte[0] < count)
			{
				seen[byte[0]] = true;
			}
		}
		for (int i = 0; i < count; i++)
		{
			CHECK(clients[i] != nullptr);
			CHECK(seen[i]);
			native_ipc_close(clients[i]);
		}
		for (void* connection : taken)
		{
			native_ipc_close(connection);
		}
		native_ipc_close(listener);
	}

	// A client that does not speak the protocol: what the OS has under the
	// name, opened without native-ipc.
	void* rawConnect(const std::string& name)
	{
#if defined(_WIN32)
		HANDLE token = nullptr;
		OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<unsigned char> buffer(size);
		GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
		CloseHandle(token);
		LPSTR sid = nullptr;
		ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid);
		std::string pipeName = std::string("\\\\.\\pipe\\native-ipc-") + sid + "-" + name;
		LocalFree(sid);
		HANDLE pipe = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		return pipe == INVALID_HANDLE_VALUE ? nullptr : pipe;
#else
		std::string path = "/tmp/native-ipc-" + std::to_string(static_cast<unsigned long>(geteuid())) + "/" + name;
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		sockaddr_un address;
		std::memset(&address, 0, sizeof(address));
		address.sun_family = AF_UNIX;
		std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
		if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			close(fd);
			return nullptr;
		}
		return reinterpret_cast<void*>(static_cast<intptr_t>(fd + 1));
#endif
	}

	bool rawWrite(void* raw, const void* data, int size)
	{
#if defined(_WIN32)
		DWORD written = 0;
		return WriteFile(static_cast<HANDLE>(raw), data, static_cast<DWORD>(size), &written, nullptr) && static_cast<int>(written) == size;
#else
		return send(static_cast<int>(reinterpret_cast<intptr_t>(raw)) - 1, data, static_cast<size_t>(size), 0) == size;
#endif
	}

	// Blocking: what the peer sent, up to `size`, or -1 once it has gone.
	int rawRead(void* raw, void* data, int size)
	{
#if defined(_WIN32)
		DWORD read = 0;
		if (!ReadFile(static_cast<HANDLE>(raw), data, static_cast<DWORD>(size), &read, nullptr))
		{
			return -1;
		}
		return static_cast<int>(read);
#else
		ssize_t read = recv(static_cast<int>(reinterpret_cast<intptr_t>(raw)) - 1, data, static_cast<size_t>(size), 0);
		return read <= 0 ? -1 : static_cast<int>(read);
#endif
	}

	void rawClose(void* raw)
	{
#if defined(_WIN32)
		CloseHandle(static_cast<HANDLE>(raw));
#else
		close(static_cast<int>(reinterpret_cast<intptr_t>(raw)) - 1);
#endif
	}

	void testForeignClientRefused()
	{
		std::string name = uniqueName("foreign");
		void* listener = native_ipc_listen(name.c_str());
		CHECK(listener != nullptr);
		void* raw = rawConnect(name);
		CHECK(raw != nullptr);
		if (raw == nullptr)
		{
			native_ipc_close(listener);
			return;
		}
		// What an older peer sends first: a frame.
		unsigned char frame[16] = {4, 0, 0, 0, 'p', 'i', 'n', 'g', 0, 0, 0, 0, 0, 0, 0, 0};
		CHECK(rawWrite(raw, frame, sizeof(frame)));
		bool accepted = false;
		auto deadline = Clock::now() + std::chrono::milliseconds(300);
		while (Clock::now() < deadline)
		{
			void* connection = native_ipc_accept(listener);
			if (connection != nullptr)
			{
				accepted = true;
				native_ipc_close(connection);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		CHECK(!accepted);
		// It was answered with this end's hello, so it can tell why, and let go.
		unsigned char answer[16];
		int got = 0;
		while (got < 16)
		{
			int read = rawRead(raw, answer + got, 16 - got);
			if (read < 0)
			{
				break;
			}
			got += read;
		}
		CHECK(got == 16);
		CHECK(std::memcmp(answer, "NIPC", 4) == 0);
		CHECK(answer[4] == NATIVE_IPC_PROTOCOL_VERSION && answer[5] == 0);
		unsigned char more;
		CHECK(rawRead(raw, &more, 1) < 0);
		rawClose(raw);
		native_ipc_close(listener);
	}

	void testSilentClientDropped()
	{
		std::string name = uniqueName("silent");
		void* listener = native_ipc_listen(name.c_str());
		void* raw = rawConnect(name);
		CHECK(raw != nullptr);
		// It never says hello: let go after the listener's wait for one.
		auto start = Clock::now();
		CHECK(waitFor([&]() {
			native_ipc_accept(listener);
			unsigned char byte;
			(void)byte;
			return false;
		}, 1300) == false);
		unsigned char answer;
		CHECK(rawRead(raw, &answer, 1) < 0);
		CHECK(Clock::now() - start < std::chrono::seconds(5));
		rawClose(raw);
		native_ipc_close(listener);
	}

	void testWrongKindRefused()
	{
		std::string name = uniqueName("kind");
		void* listener = native_ipc_listen(name.c_str());
		unsigned char buffer[4];
		CHECK(native_ipc_read_some(listener, buffer, 4) == -1);
		CHECK(native_ipc_write_some(listener, buffer, 4) == -1);
		CHECK(native_ipc_bytes_available(listener) == -1);
		CHECK(native_ipc_accept(nullptr) == nullptr);
		CHECK(!native_ipc_admits_others_for_test(listener));
		native_ipc_close(listener);
	}

	// Channel.

	struct Host
	{
		native_ipc_context* context;
		std::atomic<int> wakes;

		Host() : wakes(0)
		{
			context = native_ipc_context_create(&Host::wake, this);
		}

		~Host()
		{
			native_ipc_context_destroy(context);
		}

		static void wake(void* userData)
		{
			static_cast<Host*>(userData)->wakes++;
		}

		struct Event
		{
			int type;
			native_ipc_endpoint* endpoint;
			unsigned int id;
			native_ipc_endpoint* accepted;
			int reason;
			int error;
			std::vector<unsigned char> data;
		};

		std::vector<Event> events;

		void poll()
		{
			native_ipc_event event;
			while (native_ipc_context_poll(context, &event))
			{
				Event copy;
				copy.type = event.type;
				copy.endpoint = event.endpoint;
				copy.id = event.id;
				copy.accepted = event.accepted;
				copy.reason = event.reason;
				copy.error = event.error;
				if (event.length > 0)
				{
					copy.data.assign(event.data, event.data + event.length);
				}
				events.push_back(std::move(copy));
			}
		}

		const Event* find(int type, native_ipc_endpoint* endpoint, size_t from = 0)
		{
			for (size_t i = from; i < events.size(); i++)
			{
				if (events[i].type == type && (endpoint == nullptr || events[i].endpoint == endpoint))
				{
					return &events[i];
				}
			}
			return nullptr;
		}

		const Event* await(int type, native_ipc_endpoint* endpoint, int timeoutMs = 5000)
		{
			const Event* found = nullptr;
			waitFor([&]() {
				poll();
				found = find(type, endpoint);
				return found != nullptr;
			}, timeoutMs);
			return found;
		}

		std::vector<std::vector<unsigned char>> dataOf(native_ipc_endpoint* endpoint)
		{
			std::vector<std::vector<unsigned char>> all;
			for (Event& event : events)
			{
				if (event.type == NATIVE_IPC_EVENT_DATA && event.endpoint == endpoint)
				{
					all.push_back(event.data);
				}
			}
			return all;
		}
	};

	void testChannelMessages()
	{
		Host server;
		Host client;
		std::string name = uniqueName("channel");
		int error = -1;
		native_ipc_endpoint* listening = native_ipc_server_open(server.context, name.c_str(), &error);
		CHECK(listening != nullptr);
		CHECK(error == NATIVE_IPC_ERROR_NONE);

		native_ipc_endpoint* connection = native_ipc_client_open(client.context, name.c_str(), 3000);
		// Sent before it is made: goes once it is.
		std::vector<unsigned char> early = pattern(10, 3);
		CHECK(native_ipc_endpoint_send(connection, early.data(), static_cast<int>(early.size())));
		CHECK(client.await(NATIVE_IPC_EVENT_CONNECT, connection) != nullptr);
		CHECK(native_ipc_endpoint_connected(connection));
		const Host::Event* accept = server.await(NATIVE_IPC_EVENT_ACCEPT, listening);
		CHECK(accept != nullptr);
		if (accept == nullptr)
		{
			return;
		}
		native_ipc_endpoint* taken = accept->accepted;
		CHECK(taken != nullptr && native_ipc_endpoint_id(taken) != 0);

		std::vector<std::vector<unsigned char>> sent;
		sent.push_back(early);
		sent.push_back(std::vector<unsigned char>());
		sent.push_back(pattern(1, 4));
		sent.push_back(pattern(70000, 5));
		sent.push_back(pattern(3 * 1024 * 1024, 6));
		for (size_t i = 1; i < sent.size(); i++)
		{
			CHECK(native_ipc_endpoint_send(connection, sent[i].empty() ? nullptr : sent[i].data(), static_cast<int>(sent[i].size())));
		}
		CHECK(waitFor([&]() {
			server.poll();
			return server.dataOf(taken).size() == sent.size();
		}));
		CHECK(server.dataOf(taken) == sent);

		std::vector<unsigned char> reply = pattern(500, 7);
		CHECK(native_ipc_endpoint_send(taken, reply.data(), static_cast<int>(reply.size())));
		CHECK(client.await(NATIVE_IPC_EVENT_DATA, connection) != nullptr);
		CHECK(client.dataOf(connection).size() == 1 && client.dataOf(connection)[0] == reply);

		// Too long for a frame.
		std::vector<unsigned char> huge(NATIVE_IPC_MAX_FRAME_SIZE + 1);
		CHECK(!native_ipc_endpoint_send(connection, huge.data(), static_cast<int>(huge.size())));

		// The client going is the server's CLOSE.
		native_ipc_endpoint_close(connection);
		const Host::Event* closed = server.await(NATIVE_IPC_EVENT_CLOSE, taken);
		CHECK(closed != nullptr && closed->reason == NATIVE_IPC_REASON_CLOSED);
		CHECK(!native_ipc_endpoint_send(taken, reply.data(), static_cast<int>(reply.size())));
		native_ipc_endpoint_close(taken);
		native_ipc_endpoint_close(listening);
	}

	void testChannelManyClients()
	{
		Host server;
		Host clients;
		std::string name = uniqueName("channel-many");
		native_ipc_endpoint* listening = native_ipc_server_open(server.context, name.c_str(), nullptr);
		CHECK(listening != nullptr);
		const int count = 6;
		std::vector<native_ipc_endpoint*> connections;
		for (int i = 0; i < count; i++)
		{
			native_ipc_endpoint* connection = native_ipc_client_open(clients.context, name.c_str(), 3000);
			unsigned char byte = static_cast<unsigned char>(i);
			native_ipc_endpoint_send(connection, &byte, 1);
			connections.push_back(connection);
		}
		CHECK(waitFor([&]() {
			server.poll();
			int data = 0;
			for (Host::Event& event : server.events)
			{
				data += event.type == NATIVE_IPC_EVENT_DATA ? 1 : 0;
			}
			return data == count;
		}));
		std::vector<bool> seen(count, false);
		int accepts = 0;
		for (Host::Event& event : server.events)
		{
			accepts += event.type == NATIVE_IPC_EVENT_ACCEPT ? 1 : 0;
			if (event.type == NATIVE_IPC_EVENT_DATA && event.data.size() == 1 && event.data[0] < count)
			{
				seen[event.data[0]] = true;
			}
		}
		CHECK(accepts == count);
		for (int i = 0; i < count; i++)
		{
			CHECK(seen[i]);
		}
		// Closing the server leaves the connections it took, which are the
		// host's; closing the context closes them all.
		native_ipc_endpoint_close(listening);
	}

	void testChannelConnectFails()
	{
		Host client;
		std::string name = uniqueName("channel-nobody");
		native_ipc_endpoint* connection = native_ipc_client_open(client.context, name.c_str(), 150);
		const Host::Event* error = client.await(NATIVE_IPC_EVENT_ERROR, connection);
		CHECK(error != nullptr && error->reason == NATIVE_IPC_REASON_CONNECT && error->error == NATIVE_IPC_ERROR_TIMEOUT);
		const Host::Event* closed = client.await(NATIVE_IPC_EVENT_CLOSE, connection);
		CHECK(closed != nullptr && closed->reason == NATIVE_IPC_REASON_CONNECT);
		CHECK(!native_ipc_endpoint_connected(connection));
		native_ipc_endpoint_close(connection);

		// One waiting for good is let go by a close, at once.
		native_ipc_endpoint* forever = native_ipc_client_open(client.context, name.c_str(), 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
		auto start = Clock::now();
		native_ipc_endpoint_close(forever);
		CHECK(Clock::now() - start < std::chrono::milliseconds(500));
	}

	void testChannelServerNameInUse()
	{
		Host host;
		std::string name = uniqueName("channel-inuse");
		native_ipc_endpoint* first = native_ipc_server_open(host.context, name.c_str(), nullptr);
		int error = 0;
		native_ipc_endpoint* second = native_ipc_server_open(host.context, name.c_str(), &error);
		CHECK(first != nullptr);
		CHECK(second == nullptr);
		CHECK(error == NATIVE_IPC_ERROR_IN_USE);
	}

	void testChannelOverflow()
	{
		Host server;
		Host client;
		std::string name = uniqueName("channel-overflow");
		native_ipc_endpoint* listening = native_ipc_server_open(server.context, name.c_str(), nullptr);
		native_ipc_endpoint* connection = native_ipc_client_open(client.context, name.c_str(), 3000);
		CHECK(client.await(NATIVE_IPC_EVENT_CONNECT, connection) != nullptr);
		const Host::Event* accept = server.await(NATIVE_IPC_EVENT_ACCEPT, listening);
		CHECK(accept != nullptr);
		if (accept == nullptr)
		{
			return;
		}
		// The server takes a little and then no more, unpolled: its peer's
		// sends pile up past the client's limit.
		native_ipc_endpoint_set_max_queued(accept->accepted, 1024);
		native_ipc_endpoint_set_max_queued(connection, 256 * 1024);
		std::vector<unsigned char> chunk = pattern(64 * 1024, 8);
		bool refused = false;
		for (int i = 0; i < 1000 && !refused; i++)
		{
			refused = !native_ipc_endpoint_send(connection, chunk.data(), static_cast<int>(chunk.size()));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK(refused);
		const Host::Event* error = client.await(NATIVE_IPC_EVENT_ERROR, connection);
		CHECK(error != nullptr && error->reason == NATIVE_IPC_REASON_OVERFLOW);
		const Host::Event* closed = client.await(NATIVE_IPC_EVENT_CLOSE, connection);
		CHECK(closed != nullptr && closed->reason == NATIVE_IPC_REASON_OVERFLOW);
		native_ipc_endpoint_close(connection);
		native_ipc_endpoint_close(listening);
	}

	void testChannelBackPressure()
	{
		Host server;
		Host client;
		std::string name = uniqueName("channel-pressure");
		native_ipc_endpoint* listening = native_ipc_server_open(server.context, name.c_str(), nullptr);
		native_ipc_endpoint* connection = native_ipc_client_open(client.context, name.c_str(), 3000);
		const Host::Event* accept = server.await(NATIVE_IPC_EVENT_ACCEPT, listening);
		CHECK(accept != nullptr);
		if (accept == nullptr)
		{
			return;
		}
		native_ipc_endpoint* taken = accept->accepted;
		// What waits for the host is held to the limit; polled, it reads on,
		// and everything arrives.
		native_ipc_endpoint_set_max_queued(taken, 8 * 1024);
		native_ipc_endpoint_set_max_queued(connection, 0);
		const int count = 200;
		std::vector<unsigned char> message = pattern(4096, 9);
		for (int i = 0; i < count; i++)
		{
			CHECK(native_ipc_endpoint_send(connection, message.data(), static_cast<int>(message.size())));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		CHECK(native_ipc_endpoint_bytes_pending(connection) > 0);
		CHECK(waitFor([&]() {
			server.poll();
			return static_cast<int>(server.dataOf(taken).size()) == count;
		}));
		CHECK(waitFor([&]() { return native_ipc_endpoint_bytes_pending(connection) == 0; }));
		native_ipc_endpoint_close(connection);
		native_ipc_endpoint_close(listening);
	}

	void testChannelWakeCoalesced()
	{
		Host server;
		Host client;
		std::string name = uniqueName("channel-wake");
		native_ipc_endpoint* listening = native_ipc_server_open(server.context, name.c_str(), nullptr);
		native_ipc_endpoint* connection = native_ipc_client_open(client.context, name.c_str(), 3000);
		const Host::Event* accept = server.await(NATIVE_IPC_EVENT_ACCEPT, listening);
		CHECK(accept != nullptr);
		if (accept == nullptr)
		{
			return;
		}
		native_ipc_endpoint* taken = accept->accepted;
		server.poll();
		int before = server.wakes.load();
		for (int i = 0; i < 100; i++)
		{
			unsigned char byte = static_cast<unsigned char>(i);
			native_ipc_endpoint_send(connection, &byte, 1);
		}
		// Unpolled, a hundred messages are one wake.
		CHECK(waitFor([&]() { return server.wakes.load() > before; }));
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		CHECK(server.wakes.load() == before + 1);
		CHECK(waitFor([&]() {
			server.poll();
			return server.dataOf(taken).size() == 100;
		}));
		// Polled empty, the next message wakes it again.
		server.poll();
		int after = server.wakes.load();
		unsigned char byte = 1;
		native_ipc_endpoint_send(connection, &byte, 1);
		CHECK(waitFor([&]() { return server.wakes.load() == after + 1; }));
		native_ipc_endpoint_close(connection);
		native_ipc_endpoint_close(listening);
	}

	void testChannelCloseWithQueuedAccepts()
	{
		Host server;
		Host clients;
		std::string name = uniqueName("channel-orphans");
		native_ipc_endpoint* listening = native_ipc_server_open(server.context, name.c_str(), nullptr);
		std::vector<native_ipc_endpoint*> connections;
		for (int i = 0; i < 3; i++)
		{
			connections.push_back(native_ipc_client_open(clients.context, name.c_str(), 3000));
		}
		for (native_ipc_endpoint* connection : connections)
		{
			CHECK(clients.await(NATIVE_IPC_EVENT_CONNECT, connection) != nullptr);
		}
		// Never polled: the accepted connections go with the server, and
		// their clients see them go.
		native_ipc_endpoint_close(listening);
		for (native_ipc_endpoint* connection : connections)
		{
			CHECK(clients.await(NATIVE_IPC_EVENT_CLOSE, connection) != nullptr);
		}
		server.poll();
		CHECK(server.events.empty());
	}

	// Shared memory.

	void testSharedMemory()
	{
		std::string name = uniqueName("shm");
		void* first = native_ipc_shm_open(name.c_str(), 4096, 1000);
		void* second = native_ipc_shm_open(name.c_str(), 4096, 1000);
		CHECK(first != nullptr && second != nullptr);
		if (first == nullptr || second == nullptr)
		{
			return;
		}
		CHECK(native_ipc_shm_capacity(first, 1000) >= 4096);
		std::vector<unsigned char> data = pattern(3000, 10);
		CHECK(native_ipc_shm_write(first, data.data(), static_cast<int>(data.size()), 1000));
		std::vector<unsigned char> read(4096);
		CHECK(native_ipc_shm_read(second, read.data(), static_cast<int>(read.size()), 1000) == static_cast<int>(data.size()));
		read.resize(data.size());
		CHECK(read == data);
		std::vector<unsigned char> tooBig(1 << 20);
		CHECK(!native_ipc_shm_write(first, tooBig.data(), static_cast<int>(tooBig.size()), 1000));
		CHECK(native_ipc_shm_clear(second, 1000));
		CHECK(native_ipc_shm_read(first, read.data(), static_cast<int>(read.size()), 1000) == 0);

		// A lock held past the deadline is reported as such.
		CHECK(native_ipc_shm_hold_lock_for_test(first));
		std::thread other([&]() {
			CHECK(!native_ipc_shm_write(second, data.data(), 10, 100));
			CHECK(native_ipc_shm_last_error() == NATIVE_IPC_SHM_ERROR_LOCK_TIMEOUT);
		});
		other.join();
		native_ipc_shm_release_lock_for_test(first);

		native_ipc_shm_close(first);
		native_ipc_shm_close(second);
		CHECK(native_ipc_shm_remove(name.c_str(), 1000) >= 0);
	}
}

int main()
{
	struct Test
	{
		const char* name;
		void (*run)();
	};
	Test tests[] = {
		{"round trip", testRoundTrip},
		{"name in use", testNameInUse},
		{"connect timeout", testConnectTimeout},
		{"many clients", testManyClients},
		{"foreign client refused", testForeignClientRefused},
		{"silent client dropped", testSilentClientDropped},
		{"wrong kind refused", testWrongKindRefused},
		{"channel messages", testChannelMessages},
		{"channel many clients", testChannelManyClients},
		{"channel connect fails", testChannelConnectFails},
		{"channel server name in use", testChannelServerNameInUse},
		{"channel overflow", testChannelOverflow},
		{"channel back pressure", testChannelBackPressure},
		{"channel wake coalesced", testChannelWakeCoalesced},
		{"channel close with queued accepts", testChannelCloseWithQueuedAccepts},
		{"shared memory", testSharedMemory},
	};
	for (Test& test : tests)
	{
		int before = failures;
		auto start = Clock::now();
		test.run();
		auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
		std::printf("%s %s (%lld ms)\n", failures == before ? "ok  " : "FAIL", test.name, static_cast<long long>(took));
		std::fflush(stdout);
	}
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
