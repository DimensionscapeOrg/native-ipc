#if defined(_WIN32)

#include "Internal.h"

#include <aclapi.h>
#include <sddl.h>

#include <cstring>
#include <vector>

// Vista's; older SDKs leave it out.
#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

namespace native_ipc
{
	namespace
	{
		constexpr int PIPE_BUFFER_SIZE = 65536;
		// The most clients one accept takes from the pipe before it looks at
		// their hellos.
		constexpr int ACCEPTS_PER_CALL = 16;

		bool isInvalid(HANDLE handle)
		{
			return handle == nullptr || handle == INVALID_HANDLE_VALUE;
		}

		// This process's user: its SID, and the SID as text. Read once; empty
		// when the process's token cannot be read.
		struct ProcessUser
		{
			std::vector<unsigned char> sid;
			std::string text;
		};

		const ProcessUser& processUser()
		{
			static const ProcessUser user = []() {
				ProcessUser found;
				HANDLE token = nullptr;
				if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
				{
					return found;
				}
				DWORD size = 0;
				GetTokenInformation(token, TokenUser, nullptr, 0, &size);
				std::vector<unsigned char> buffer(size > 0 ? size : 1);
				if (size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
				{
					PSID sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
					LPSTR text = nullptr;
					if (IsValidSid(sid) && ConvertSidToStringSidA(sid, &text))
					{
						DWORD length = GetLengthSid(sid);
						found.sid.assign(static_cast<unsigned char*>(sid), static_cast<unsigned char*>(sid) + length);
						found.text = text;
						LocalFree(text);
					}
				}
				CloseHandle(token);
				return found;
			}();
			return user;
		}

		// What a pipe or a doorbell of this user's is made with: this user as
		// its owner, and this user and SYSTEM alone admitted, where the
		// default security would let everyone (the anonymous user included)
		// open a pipe to read what the listener sent.
		struct OwnSecurity
		{
			PSECURITY_DESCRIPTOR descriptor = nullptr;
			SECURITY_ATTRIBUTES attributes;

			bool make()
			{
				const std::string& user = processUser().text;
				if (user.empty())
				{
					return false;
				}
				std::string sddl = "O:" + user + "D:P(A;;GA;;;" + user + ")(A;;GA;;;SY)";
				if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
				{
					descriptor = nullptr;
					return false;
				}
				attributes.nLength = sizeof(attributes);
				attributes.lpSecurityDescriptor = descriptor;
				attributes.bInheritHandle = FALSE;
				return true;
			}

			~OwnSecurity()
			{
				if (descriptor != nullptr)
				{
					LocalFree(descriptor);
				}
			}
		};

		const char* nameOrEmpty(const char* name)
		{
			return name == nullptr ? "" : name;
		}

		// The pipe for `name`: this user's, by the user's SID in it. Pipe names
		// are one namespace for every user of the machine, so without the SID
		// two users' names would be one pipe, and whoever made it first would
		// have the other's clients. A name of another user's cannot be this one.
		// Empty when the user cannot be read.
		std::string makePipeName(const char* name)
		{
			const ProcessUser& user = processUser();
			if (user.text.empty())
			{
				return std::string();
			}
			return std::string("\\\\.\\pipe\\native-ipc-") + user.text + "-" + nameOrEmpty(name);
		}

		// What the doorbells of `name` are called after: the user's, and a
		// hash of the name, so the doorbells of a long name fit where an
		// event's name must.
		std::string bellBase(const char* name)
		{
			return std::string("Local\\native-ipc-") + processUser().text + "-" + hex64(hashName(nameOrEmpty(name)));
		}

		// Doorbells. A pipe made PIPE_NOWAIT has nothing a thread can wait on,
		// so each end of a connection has an event its reader waits on between
		// looks at the pipe, which the other end sets when it has written to
		// it, made room in it, or gone: a reader wakes as its data arrives,
		// not at its next look. A listener has one more, which a client rings
		// once it has said hello. Each is this user's and SYSTEM's alone, and
		// auto-reset: one wait returns for each ring, and rings while nobody
		// waits come to one. nullptr where one cannot be made (another user's
		// object under the name, or a name an event cannot have); a reader
		// then looks between sleeps.
		HANDLE makeBell(const std::string& name)
		{
			OwnSecurity security;
			if (!security.make())
			{
				return nullptr;
			}
			return CreateEventA(&security.attributes, FALSE, FALSE, name.c_str());
		}

		void ring(HANDLE bell)
		{
			if (bell != nullptr)
			{
				SetEvent(bell);
			}
		}

		void closeBell(HANDLE bell)
		{
			if (bell != nullptr)
			{
				CloseHandle(bell);
			}
		}

		struct Connection : HandleBase
		{
			HANDLE pipe;
			// The doorbell this end's reader waits on, and the one it rings.
			HANDLE mine;
			HANDLE peer;
		};

		// A client the listener's pipe has taken that has not said hello yet.
		struct Pending
		{
			HANDLE pipe;
			unsigned char hello[HELLO_SIZE];
			int got;
			std::chrono::steady_clock::time_point deadline;
		};

		struct Listener : HandleBase
		{
			std::string name;
			std::string pipeName;
			// The instance the next client connects to.
			HANDLE pending;
			HANDLE acceptBell;
			std::vector<Pending> handshaking;
		};

		// An instance of the pipe `pipeName`, admitting this user and SYSTEM
		// alone, and no client on another machine.
		//
		// The first, made by native_ipc_listen, is made with
		// FILE_FLAG_FIRST_PIPE_INSTANCE, which refuses a name any instance of
		// which exists (another user's put there first included): a second
		// listen on a name in use does not make an instance beside the first
		// for clients to go to whichever the system picks. The listener makes
		// each next one itself, as a client takes the last.
		HANDLE createInstance(const std::string& pipeName, bool first)
		{
			OwnSecurity security;
			if (!security.make())
			{
				return INVALID_HANDLE_VALUE;
			}
			return CreateNamedPipeA(
				pipeName.c_str(),
				PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
				PIPE_UNLIMITED_INSTANCES,
				PIPE_BUFFER_SIZE,
				PIPE_BUFFER_SIZE,
				0,
				&security.attributes);
		}

		// The connection over `pipe`, with the doorbells of its id: the
		// listener's end, or (`listenerSide` false) the client's.
		Connection* makeConnection(HANDLE pipe, const char* name, uint64_t id, bool listenerSide)
		{
			auto* connection = new Connection();
			connection->kind = KIND_CONNECTION;
			connection->pipe = pipe;
			connection->mine = nullptr;
			connection->peer = nullptr;
			std::string base = bellBase(name) + "-" + hex64(id);
			HANDLE toListener = makeBell(base + "-l");
			HANDLE toClient = makeBell(base + "-c");
			if (toListener != nullptr && toClient != nullptr)
			{
				connection->mine = listenerSide ? toListener : toClient;
				connection->peer = listenerSide ? toClient : toListener;
			}
			else
			{
				closeBell(toListener);
				closeBell(toClient);
			}
			return connection;
		}

		void closeConnection(Connection* connection)
		{
			// Its peer is told.
			ring(connection->peer);
			if (!isInvalid(connection->pipe))
			{
				CloseHandle(connection->pipe);
			}
			closeBell(connection->mine);
			closeBell(connection->peer);
			delete connection;
		}

		bool isGone(DWORD error)
		{
			return error == ERROR_BROKEN_PIPE || error == ERROR_INVALID_HANDLE || error == ERROR_PIPE_NOT_CONNECTED;
		}

		// As much as `pipe` holds, up to `size`, without waiting: the bytes
		// read, 0 for none, -1 when the connection is gone.
		int readSomeRaw(HANDLE pipe, unsigned char* buffer, int size)
		{
			DWORD bytesRead = 0;
			if (ReadFile(pipe, buffer, static_cast<DWORD>(size), &bytesRead, nullptr))
			{
				return static_cast<int>(bytesRead);
			}
			DWORD error = GetLastError();
			if (error == ERROR_MORE_DATA)
			{
				return static_cast<int>(bytesRead);
			}
			if (error == ERROR_NO_DATA)
			{
				return 0;
			}
			return isGone(error) ? -1 : 0;
		}

		// As much of `buffer` as `pipe` takes now, without waiting: the bytes
		// written, 0 when the pipe is full, -1 when the connection is gone. A
		// nonblocking byte-mode pipe with too little room writes what fits.
		int writeSomeRaw(HANDLE pipe, const unsigned char* buffer, int size)
		{
			int totalWritten = 0;
			while (totalWritten < size)
			{
				int writeSize = size - totalWritten;
				if (writeSize > PIPE_BUFFER_SIZE)
				{
					writeSize = PIPE_BUFFER_SIZE;
				}
				DWORD bytesWritten = 0;
				if (!WriteFile(pipe, buffer + totalWritten, static_cast<DWORD>(writeSize), &bytesWritten, nullptr))
				{
					DWORD error = GetLastError();
					if (error == ERROR_NO_DATA || isGone(error))
					{
						return totalWritten > 0 ? totalWritten : -1;
					}
					break;
				}
				if (bytesWritten == 0)
				{
					break;
				}
				totalWritten += static_cast<int>(bytesWritten);
			}
			return totalWritten;
		}

		// Whether `pipe` is owned by this user. A listener sets this user as its
		// pipe's owner, which another user cannot: one finding a pipe under its
		// name owned by anyone else has found someone else's, put there first.
		bool ownedByProcessUser(HANDLE pipe)
		{
			const ProcessUser& user = processUser();
			if (user.sid.empty())
			{
				return false;
			}
			PSID owner = nullptr;
			PSECURITY_DESCRIPTOR descriptor = nullptr;
			if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS)
			{
				return false;
			}
			bool same = owner != nullptr && IsValidSid(owner) && EqualSid(owner, const_cast<unsigned char*>(user.sid.data()));
			LocalFree(descriptor);
			return same;
		}

		Connection* asConnection(void* handle)
		{
			return isKind(handle, KIND_CONNECTION) ? static_cast<Connection*>(handle) : nullptr;
		}

		Listener* asListener(void* handle)
		{
			return isKind(handle, KIND_LISTENER) ? static_cast<Listener*>(handle) : nullptr;
		}

		HANDLE bellOf(void* handle)
		{
			if (Connection* connection = asConnection(handle))
			{
				return connection->mine;
			}
			if (Listener* listener = asListener(handle))
			{
				return listener->acceptBell;
			}
			return nullptr;
		}
	}

	Waker::Waker()
	{
		event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	}

	Waker::~Waker()
	{
		if (event != nullptr)
		{
			CloseHandle(event);
		}
	}

	void Waker::signal()
	{
		if (event != nullptr)
		{
			SetEvent(event);
		}
	}

	// An auto-reset event drains itself.
	void Waker::drain()
	{
	}

	namespace detail
	{
		void* connect(const char* name, int timeoutMs, const std::atomic<bool>* cancel)
		{
			setLastError(NATIVE_IPC_ERROR_FAILED);
			std::string pipeName = makePipeName(name);
			if (pipeName.empty())
			{
				return nullptr;
			}
			bool forever = timeoutMs <= 0;
			auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(forever ? 0 : timeoutMs);

			while (true)
			{
				if (cancel != nullptr && cancel->load())
				{
					return nullptr;
				}

				// The listener may learn who this is, and no more: it cannot act
				// as this user, as by default any pipe's listener can.
				HANDLE pipe = CreateFileA(
					pipeName.c_str(),
					GENERIC_READ | GENERIC_WRITE,
					0,
					nullptr,
					OPEN_EXISTING,
					SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
					nullptr);

				if (pipe != INVALID_HANDLE_VALUE)
				{
					// A pipe under this user's name that another user made is
					// theirs, listening for this one's clients: refused.
					if (!ownedByProcessUser(pipe))
					{
						CloseHandle(pipe);
						setLastError(NATIVE_IPC_ERROR_NOT_OWNED);
						return nullptr;
					}
					DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
					SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

					uint64_t id = newConnectionId();
					Connection* connection = makeConnection(pipe, name, id, false);
					unsigned char hello[HELLO_SIZE];
					writeHello(hello, id);
					if (writeSomeRaw(pipe, hello, HELLO_SIZE) != HELLO_SIZE)
					{
						closeConnection(connection);
						return nullptr;
					}
					// The listener takes it at once.
					HANDLE acceptBell = makeBell(bellBase(name) + "-a");
					ring(acceptBell);
					closeBell(acceptBell);

					// Its answer, for as long as the connect has left, and a
					// slice at least: a listener found just before the deadline,
					// or by a connect of one try, has time to answer.
					auto answerBy = deadline;
					auto least = std::chrono::steady_clock::now() + std::chrono::milliseconds(CONNECT_WAIT_SLICE_MS);
					if (answerBy < least)
					{
						answerBy = least;
					}
					unsigned char answer[HELLO_SIZE];
					int got = 0;
					while (got < HELLO_SIZE)
					{
						int read = readSomeRaw(pipe, answer + got, HELLO_SIZE - got);
						if (read < 0)
						{
							closeConnection(connection);
							setLastError(NATIVE_IPC_ERROR_FAILED);
							return nullptr;
						}
						if (read > 0)
						{
							got += read;
							continue;
						}
						if (cancel != nullptr && cancel->load())
						{
							closeConnection(connection);
							return nullptr;
						}
						auto now = std::chrono::steady_clock::now();
						int waitMs = CONNECT_WAIT_SLICE_MS;
						if (!forever)
						{
							if (now >= answerBy)
							{
								closeConnection(connection);
								setLastError(NATIVE_IPC_ERROR_TIMEOUT);
								return nullptr;
							}
							int leftMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(answerBy - now).count()) + 1;
							waitMs = leftMs < waitMs ? leftMs : waitMs;
						}
						if (connection->mine != nullptr)
						{
							WaitForSingleObject(connection->mine, static_cast<DWORD>(waitMs));
						}
						else
						{
							Sleep(1);
						}
					}
					uint64_t echoed = 0;
					if (!readHello(answer, &echoed) || echoed != id)
					{
						closeConnection(connection);
						setLastError(NATIVE_IPC_ERROR_PROTOCOL);
						return nullptr;
					}
					setLastError(NATIVE_IPC_ERROR_NONE);
					return connection;
				}

				DWORD error = GetLastError();
				if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
				{
					return nullptr;
				}
				// No time left for another attempt: one try, without the wait.
				if (!anotherTry(forever, deadline))
				{
					setLastError(NATIVE_IPC_ERROR_TIMEOUT);
					return nullptr;
				}
				if (error == ERROR_PIPE_BUSY)
				{
					WaitNamedPipeA(pipeName.c_str(), CONNECT_WAIT_SLICE_MS);
				}
				else
				{
					Sleep(CONNECT_WAIT_SLICE_MS);
				}
			}
		}

		bool waitAny(void* handle, bool read, bool write, Waker& waker, int timeoutMs)
		{
			HANDLE objects[2];
			DWORD count = 0;
			HANDLE bell = (read || write) ? bellOf(handle) : nullptr;
			if (bell != nullptr)
			{
				objects[count++] = bell;
			}
			if (waker.event != nullptr)
			{
				objects[count++] = waker.event;
			}
			if (count == 0)
			{
				Sleep(static_cast<DWORD>(timeoutMs));
				return false;
			}
			DWORD result = WaitForMultipleObjects(count, objects, FALSE, static_cast<DWORD>(timeoutMs));
			return waker.event != nullptr && result == WAIT_OBJECT_0 + count - 1;
		}

		bool handshaking(void* handle)
		{
			Listener* listener = asListener(handle);
			return listener != nullptr && !listener->handshaking.empty();
		}
	}
}

using namespace native_ipc;

extern "C" void* native_ipc_listen(const char* name)
{
	setLastError(NATIVE_IPC_ERROR_FAILED);
	std::string pipeName = makePipeName(name);
	if (pipeName.empty())
	{
		return nullptr;
	}

	HANDLE pipe = createInstance(pipeName, true);
	if (pipe == INVALID_HANDLE_VALUE)
	{
		// The first instance is refused for a name with any instance: one of
		// this user's listening there, or another user's put there first.
		DWORD error = GetLastError();
		setLastError(error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY ? NATIVE_IPC_ERROR_IN_USE : NATIVE_IPC_ERROR_FAILED);
		return nullptr;
	}

	auto* listener = new Listener();
	listener->kind = KIND_LISTENER;
	listener->name = nameOrEmpty(name);
	listener->pipeName = pipeName;
	listener->pending = pipe;
	listener->acceptBell = makeBell(bellBase(name) + "-a");
	setLastError(NATIVE_IPC_ERROR_NONE);
	return listener;
}

extern "C" void* native_ipc_accept(void* handle)
{
	Listener* listener = asListener(handle);
	if (listener == nullptr)
	{
		setLastError(NATIVE_IPC_ERROR_FAILED);
		return nullptr;
	}
	setLastError(NATIVE_IPC_ERROR_NONE);

	// The clients the pipe has taken, each put to one side to say hello and
	// a new instance made for the next. ERROR_NO_DATA: a client came and went
	// before this looked. It is taken like any other, and its leaving found
	// when its hello is read.
	for (int i = 0; i < ACCEPTS_PER_CALL; i++)
	{
		if (isInvalid(listener->pending))
		{
			listener->pending = createInstance(listener->pipeName, false);
			if (isInvalid(listener->pending))
			{
				break;
			}
		}
		BOOL connected = ConnectNamedPipe(listener->pending, nullptr);
		DWORD error = connected ? ERROR_SUCCESS : GetLastError();
		if (!connected && error != ERROR_PIPE_CONNECTED && error != ERROR_NO_DATA)
		{
			// ERROR_PIPE_LISTENING: nobody yet.
			break;
		}
		Pending pending;
		pending.pipe = listener->pending;
		pending.got = 0;
		pending.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(HELLO_TIMEOUT_MS);
		listener->handshaking.push_back(pending);
		// Made again at the next accept if it cannot be now.
		listener->pending = createInstance(listener->pipeName, false);
	}

	// Their hellos. One whose hello is not this protocol's is answered with
	// this end's, so a client of another version can say why, and let go.
	auto now = std::chrono::steady_clock::now();
	for (size_t i = 0; i < listener->handshaking.size();)
	{
		Pending& pending = listener->handshaking[i];
		int read = readSomeRaw(pending.pipe, pending.hello + pending.got, HELLO_SIZE - pending.got);
		if (read > 0)
		{
			pending.got += read;
		}
		if (read < 0 || (pending.got < HELLO_SIZE && now >= pending.deadline))
		{
			CloseHandle(pending.pipe);
			listener->handshaking.erase(listener->handshaking.begin() + i);
			continue;
		}
		if (pending.got < HELLO_SIZE)
		{
			i++;
			continue;
		}

		HANDLE pipe = pending.pipe;
		uint64_t id = 0;
		bool spoken = readHello(pending.hello, &id);
		listener->handshaking.erase(listener->handshaking.begin() + i);
		unsigned char answer[HELLO_SIZE];
		writeHello(answer, spoken ? id : 0);
		if (!spoken)
		{
			writeSomeRaw(pipe, answer, HELLO_SIZE);
			CloseHandle(pipe);
			continue;
		}
		Connection* connection = makeConnection(pipe, listener->name.c_str(), id, true);
		if (writeSomeRaw(pipe, answer, HELLO_SIZE) != HELLO_SIZE)
		{
			closeConnection(connection);
			continue;
		}
		ring(connection->peer);
		return connection;
	}
	return nullptr;
}

extern "C" void native_ipc_close(void* handle)
{
	if (Connection* connection = asConnection(handle))
	{
		closeConnection(connection);
		return;
	}
	if (Listener* listener = asListener(handle))
	{
		if (!isInvalid(listener->pending))
		{
			CloseHandle(listener->pending);
		}
		for (Pending& pending : listener->handshaking)
		{
			CloseHandle(pending.pipe);
		}
		closeBell(listener->acceptBell);
		listener->kind = 0;
		delete listener;
	}
}

extern "C" int native_ipc_bytes_available(void* handle)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr)
	{
		return -1;
	}

	DWORD bytesAvailable = 0;
	if (PeekNamedPipe(connection->pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr))
	{
		return static_cast<int>(bytesAvailable);
	}

	DWORD error = GetLastError();
	if (error == ERROR_NO_DATA)
	{
		return 0;
	}
	return isGone(error) ? -1 : 0;
}

extern "C" int native_ipc_read(void* handle, unsigned char* buffer, int size)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr || buffer == nullptr || size <= 0)
	{
		return ERROR_INVALID_PARAMETER;
	}

	int totalRead = 0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(IO_TIMEOUT_MS);
	while (totalRead < size)
	{
		DWORD bytesRead = 0;
		if (ReadFile(connection->pipe, buffer + totalRead, static_cast<DWORD>(size - totalRead), &bytesRead, nullptr))
		{
			if (bytesRead > 0)
			{
				totalRead += static_cast<int>(bytesRead);
				continue;
			}
		}

		DWORD error = GetLastError();
		if (error == ERROR_MORE_DATA)
		{
			totalRead += static_cast<int>(bytesRead);
			continue;
		}
		if (error == ERROR_NO_DATA)
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return ERROR_TIMEOUT;
			}
			Sleep(1);
			continue;
		}
		if (isGone(error))
		{
			return static_cast<int>(error);
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return error == ERROR_SUCCESS ? ERROR_TIMEOUT : static_cast<int>(error);
		}
		Sleep(1);
	}

	// Room made in a pipe that may have been full: a writer waiting on it
	// goes on now.
	if (totalRead >= PIPE_BUFFER_SIZE / 2)
	{
		ring(connection->peer);
	}
	return 0;
}

extern "C" int native_ipc_read_some(void* handle, unsigned char* buffer, int size)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr || buffer == nullptr || size <= 0)
	{
		return -1;
	}
	int read = readSomeRaw(connection->pipe, buffer, size);
	if (read >= PIPE_BUFFER_SIZE / 2)
	{
		ring(connection->peer);
	}
	return read;
}

extern "C" int native_ipc_write_some(void* handle, const unsigned char* buffer, int size)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr || buffer == nullptr || size < 0)
	{
		return -1;
	}
	int written = writeSomeRaw(connection->pipe, buffer, size);
	if (written > 0)
	{
		ring(connection->peer);
	}
	return written;
}

// All of `buffer`, waiting up to IO_TIMEOUT_MS. It waits outside a host
// collector's reach (see Config.h), so a collection another thread starts
// meanwhile does not wait for it: the peer's reader among them, which this
// may be waiting on. `buffer` stays where it is: the caller holds it.
extern "C" bool native_ipc_write(void* handle, const unsigned char* buffer, int size)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr || buffer == nullptr || size <= 0)
	{
		return false;
	}

	BlockingScope waiting;
	int totalWritten = 0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(IO_TIMEOUT_MS);
	while (totalWritten < size)
	{
		int written = native_ipc_write_some(handle, buffer + totalWritten, size - totalWritten);
		if (written < 0)
		{
			return false;
		}
		totalWritten += written;
		if (totalWritten < size)
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			Sleep(1);
		}
	}
	return true;
}

extern "C" bool native_ipc_is_open(void* handle)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr)
	{
		return false;
	}

	DWORD bytesAvailable = 0;
	if (PeekNamedPipe(connection->pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr))
	{
		return true;
	}
	DWORD error = GetLastError();
	return error == ERROR_NO_DATA || !isGone(error);
}

// It may wait for a listener, on the caller's thread: outside a host
// collector's reach meanwhile. Nothing of the host's is touched from here on;
// the name is copied before any wait.
extern "C" void* native_ipc_connect(const char* name, int timeoutMs)
{
	BlockingScope waiting;
	return detail::connect(name, timeoutMs, nullptr);
}

// A pipe's name lives as long as its instances, with no file to keep.
extern "C" void native_ipc_keep_name(void* handle)
{
}

// What a pipe's reader waits on: its doorbell, as a number (a handle fits in
// 32 bits on 64-bit Windows, which keeps them so for 32-bit code), or -1 for
// none.
extern "C" int native_ipc_descriptor_of(void* handle)
{
	HANDLE bell = bellOf(handle);
	return bell == nullptr ? -1 : static_cast<int>(reinterpret_cast<intptr_t>(bell));
}

extern "C" bool native_ipc_wait_for_work(int descriptor, bool read, bool write, int timeoutMs)
{
	BlockingScope waiting;
	if (descriptor == -1)
	{
		Sleep(static_cast<DWORD>(timeoutMs));
		return false;
	}
	HANDLE bell = reinterpret_cast<HANDLE>(static_cast<intptr_t>(descriptor));
	WaitForSingleObject(bell, static_cast<DWORD>(timeoutMs));
	return false;
}

extern "C" bool native_ipc_admits_others_for_test(void* handle)
{
	Listener* listener = asListener(handle);
	const ProcessUser& user = processUser();
	if (listener == nullptr || isInvalid(listener->pending) || user.sid.empty())
	{
		return true;
	}
	HANDLE pipe = listener->pending;
	if (!ownedByProcessUser(pipe))
	{
		return true;
	}
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS)
	{
		return true;
	}
	// No DACL at all admits everyone.
	bool others = dacl == nullptr;
	BYTE systemSid[SECURITY_MAX_SID_SIZE];
	DWORD systemSize = sizeof(systemSid);
	CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &systemSize);
	for (DWORD i = 0; !others && i < dacl->AceCount; i++)
	{
		void* ace = nullptr;
		if (!GetAce(dacl, i, &ace))
		{
			others = true;
			break;
		}
		auto* header = static_cast<ACE_HEADER*>(ace);
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
		{
			continue;
		}
		PSID sid = reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart);
		if (!EqualSid(sid, const_cast<unsigned char*>(user.sid.data())) && !EqualSid(sid, systemSid))
		{
			others = true;
		}
	}
	LocalFree(descriptor);
	return others;
}

// A pipe's buffers are the size it is made with.
extern "C" void native_ipc_set_socket_buffer_for_test(int bytes)
{
}

#endif
