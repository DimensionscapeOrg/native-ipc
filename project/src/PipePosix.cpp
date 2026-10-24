#if !defined(_WIN32)

#include "Internal.h"

#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace native_ipc
{
	namespace
	{
		constexpr int PIPE_LISTEN_BACKLOG = 16;
		// The most clients one accept takes from the socket before it looks
		// at their hellos.
		constexpr int ACCEPTS_PER_CALL = 16;

		// A send to a peer that has gone raises SIGPIPE, whose default action
		// ends the process, unless the send says not to. macOS has no flag for
		// it and sets SO_NOSIGPIPE on the socket instead; see
		// configureConnectedSocket.
#if defined(MSG_NOSIGNAL)
		constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
		constexpr int SEND_FLAGS = 0;
#endif

		// How often, at most, a listener's lock file has its times brought up
		// to date: often enough that a cleaner of old files in /tmp (macOS's
		// takes what nobody has touched for three days, systemd's for ten)
		// never finds a listener's old.
		constexpr long LOCK_FILE_REFRESH_SECONDS = 3600;

		struct Connection : HandleBase
		{
			int fd;
		};

		// A client the listener's socket has taken that has not said hello
		// yet.
		struct Pending
		{
			int fd;
			unsigned char hello[HELLO_SIZE];
			int got;
			std::chrono::steady_clock::time_point deadline;
		};

		struct Listener : HandleBase
		{
			int listenFd;
			// Held for as long as this listens on its name: see native_ipc_listen.
			int lockFd;
			char path[108];
			// When the lock file's times were last brought up to date; see
			// native_ipc_keep_name.
			std::chrono::steady_clock::time_point lockRefreshed;
			std::vector<Pending> handshaking;
		};

		Connection* asConnection(void* handle)
		{
			return isKind(handle, KIND_CONNECTION) ? static_cast<Connection*>(handle) : nullptr;
		}

		Listener* asListener(void* handle)
		{
			return isKind(handle, KIND_LISTENER) ? static_cast<Listener*>(handle) : nullptr;
		}

		Connection* makeConnection(int fd)
		{
			auto* connection = new (std::nothrow) Connection();
			if (connection == nullptr)
			{
				close(fd);
				return nullptr;
			}
			connection->kind = KIND_CONNECTION;
			connection->fd = fd;
			return connection;
		}

		void setNonBlocking(int fd)
		{
			int flags = fcntl(fd, F_GETFL, 0);
			if (flags != -1)
			{
				fcntl(fd, F_SETFL, flags | O_NONBLOCK);
			}
		}

		// Tests only: the buffer size asked of each socket connected or taken
		// from now on, in each direction; 0 leaves the system's. A Linux build
		// asks for macOS's small one with it.
		std::atomic<int> testSocketBuffer(0);

		// A connected socket, and one about to connect, never makes its caller
		// wait: each read, write and connect takes what the socket can do now,
		// and what has to wait does so in poll(), with a deadline.
		//
		// A blocking socket with MSG_DONTWAIT would not do: Linux honours that
		// for a send, but macOS's kernel gives up rather than waits only on a
		// socket set non-blocking, so a send to a peer that had stopped
		// reading, once the 8 KB macOS gives a local socket was full, would
		// wait for good on the host's thread. And a connect to a listener
		// whose backlog is full waits in Linux's kernel for it to take
		// someone, past any timeout.
		void configureConnectedSocket(int fd)
		{
			setNonBlocking(fd);
			fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(__APPLE__)
			int optionValue = 1;
			setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &optionValue, sizeof(optionValue));
#endif
			int buffer = testSocketBuffer.load();
			if (buffer > 0)
			{
				setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
				setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
			}
		}

		// Waits for `fd` to be ready for `events` until `deadline`: whether it
		// became so.
		bool waitReady(int fd, short events, std::chrono::steady_clock::time_point deadline)
		{
			while (true)
			{
				auto now = std::chrono::steady_clock::now();
				if (now >= deadline)
				{
					return false;
				}
				int leftMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()) + 1;
				pollfd entry;
				entry.fd = fd;
				entry.events = events;
				entry.revents = 0;
				int ready = poll(&entry, 1, leftMs);
				if (ready > 0)
				{
					return true;
				}
				if (ready < 0 && errno != EINTR)
				{
					return false;
				}
			}
		}

		// The directory this user's names live in: /tmp/native-ipc-<uid>, made
		// 0700.
		//
		// Not /tmp itself, where any user can put anything at a name first: a
		// listener there would take the name's clients, and a link there would
		// take them wherever it pointed, since connect() follows one. In a
		// directory of this user's that nobody else can enter, nobody else can
		// put anything. The same directory for every process of the user,
		// however started: one under $XDG_RUNTIME_DIR, which a login session
		// has and a cron job or a service running as the user does not, would
		// part a command-line tool from a service it talks to; and macOS's
		// per-user $TMPDIR is too long for a socket path, which has 104 bytes
		// there.
		std::string userDirectory()
		{
			return "/tmp/native-ipc-" + std::to_string(static_cast<unsigned long>(geteuid()));
		}

		// Whether `directory` is there and this user's own: 1 if so; 0 if
		// nothing is there (made first when `make`); -1 if something else is,
		// or it cannot be looked at, the last error saying which.
		//
		// Its own is a directory, not a link to one, that this user owns and
		// nobody else can enter or write. Anything else is refused, not
		// repaired: what another user's, or one others could write, holds may
		// be theirs.
		int checkUserDirectory(const std::string& directory, bool make)
		{
			struct stat info;
			if (lstat(directory.c_str(), &info) != 0)
			{
				if (errno != ENOENT)
				{
					setLastError(NATIVE_IPC_ERROR_FAILED);
					return -1;
				}
				if (!make)
				{
					return 0;
				}
				if ((mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) || lstat(directory.c_str(), &info) != 0)
				{
					setLastError(NATIVE_IPC_ERROR_FAILED);
					return -1;
				}
			}
			if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 077) != 0)
			{
				setLastError(NATIVE_IPC_ERROR_NOT_OWNED);
				return -1;
			}
			return 1;
		}

		// The socket's name for `name`, in the user's directory.
		//
		// A name of [A-Za-z0-9_-] alone, 48 characters or fewer, keeps its
		// path; any other is a readable part of it and a 64-bit FNV-1a hash of
		// the whole, so "a.b" and "a_b", or two long names alike for their
		// first 48 characters, are not one socket.
		std::string sanitizePipeName(const char* name)
		{
			const size_t budget = 48;
			std::string source = (name == nullptr || name[0] == '\0') ? "default" : name;
			std::string sanitized;
			bool changed = false;

			for (char c : source)
			{
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
				{
					sanitized.push_back(c);
				}
				else
				{
					sanitized.push_back('_');
					changed = true;
				}
			}

			if (!changed && sanitized.size() <= budget)
			{
				return sanitized;
			}

			const size_t keep = budget - 17;
			return sanitized.substr(0, sanitized.size() < keep ? sanitized.size() : keep) + "_" + hex64(hashName(source));
		}

		// The socket path for `name`: in `directory`, the user's.
		std::string makePipeName(const std::string& directory, const char* name)
		{
			return directory + "/" + sanitizePipeName(name);
		}

		// An exclusive lock on `path`.lock, taken without waiting: the
		// descriptor holding it, or -1 when another listener has it (the last
		// error then NATIVE_IPC_ERROR_IN_USE). The system lets it go when its
		// holder exits, however it exits, so a name a crashed listener left
		// is free again. Checked to be the file now at that path, since a
		// listener closing removes it: a lock on one already removed holds
		// nothing.
		//
		// In the user's own directory, not /tmp, where any user can put
		// something at a name first; a link there is not followed, a FIFO is
		// not waited on, and only a regular file this user owns is taken. Its
		// times are brought up to date as it is taken.
		int lockName(const std::string& path)
		{
			std::string lockPath = path + ".lock";
			for (int attempt = 0; attempt < 8; attempt++)
			{
				int fd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
				if (fd < 0)
				{
					setLastError(NATIVE_IPC_ERROR_FAILED);
					return -1;
				}
				struct stat held;
				if (fstat(fd, &held) != 0 || !S_ISREG(held.st_mode) || held.st_uid != geteuid())
				{
					close(fd);
					setLastError(NATIVE_IPC_ERROR_NOT_OWNED);
					return -1;
				}
				if (flock(fd, LOCK_EX | LOCK_NB) != 0)
				{
					close(fd);
					setLastError(errno == EWOULDBLOCK ? NATIVE_IPC_ERROR_IN_USE : NATIVE_IPC_ERROR_FAILED);
					return -1;
				}
				struct stat current;
				if (lstat(lockPath.c_str(), &current) == 0 && held.st_ino == current.st_ino && held.st_dev == current.st_dev)
				{
					futimes(fd, nullptr);
					return fd;
				}
				close(fd);
			}
			setLastError(NATIVE_IPC_ERROR_FAILED);
			return -1;
		}

		// Whether what is at `path` may be connected to: 1 if a socket of this
		// user's is there, 0 if nothing is, -1 if anything else is (a link,
		// which connect() would follow wherever it led, or something another
		// user made), the last error saying so.
		int checkSocketPath(const std::string& path)
		{
			struct stat info;
			if (lstat(path.c_str(), &info) != 0)
			{
				if (errno == ENOENT)
				{
					return 0;
				}
				setLastError(NATIVE_IPC_ERROR_FAILED);
				return -1;
			}
			if (!S_ISSOCK(info.st_mode) || info.st_uid != geteuid())
			{
				setLastError(NATIVE_IPC_ERROR_NOT_OWNED);
				return -1;
			}
			return 1;
		}

		// As much as `fd` holds, up to `size`, without waiting: the bytes read,
		// 0 for none, -1 when the connection is gone.
		int readSomeRaw(int fd, unsigned char* buffer, int size)
		{
			while (true)
			{
				ssize_t bytesRead = recv(fd, reinterpret_cast<char*>(buffer), static_cast<size_t>(size), 0);
				if (bytesRead > 0)
				{
					return static_cast<int>(bytesRead);
				}
				if (bytesRead == 0)
				{
					return -1;
				}
				if (errno == EINTR)
				{
					continue;
				}
				return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
			}
		}

		// As much of `buffer` as `fd` takes now, without waiting: the bytes
		// written, 0 when it is full, -1 when the connection is gone.
		int writeSomeRaw(int fd, const unsigned char* buffer, int size)
		{
			int bytesWritten = 0;
			while (bytesWritten < size)
			{
				ssize_t sendResult = send(fd, reinterpret_cast<const char*>(buffer) + bytesWritten, static_cast<size_t>(size - bytesWritten), MSG_DONTWAIT | SEND_FLAGS);
				if (sendResult > 0)
				{
					bytesWritten += static_cast<int>(sendResult);
					continue;
				}
				if (sendResult < 0 && errno == EINTR)
				{
					continue;
				}
				// Full for now. macOS may also say ENOBUFS of a local socket
				// whose peer has not caught up ("the operation may succeed when
				// buffers become available"), which is not the end of the
				// connection.
				if (sendResult < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS))
				{
					break;
				}
				return bytesWritten > 0 ? bytesWritten : -1;
			}
			return bytesWritten;
		}

		int descriptorOf(void* handle)
		{
			if (Connection* connection = asConnection(handle))
			{
				return connection->fd;
			}
			if (Listener* listener = asListener(handle))
			{
				return listener->listenFd;
			}
			return -1;
		}
	}

	Waker::Waker()
	{
		fds[0] = -1;
		fds[1] = -1;
		if (pipe(fds) == 0)
		{
			for (int fd : fds)
			{
				setNonBlocking(fd);
				fcntl(fd, F_SETFD, FD_CLOEXEC);
			}
		}
		else
		{
			fds[0] = -1;
			fds[1] = -1;
		}
	}

	Waker::~Waker()
	{
		for (int fd : fds)
		{
			if (fd >= 0)
			{
				close(fd);
			}
		}
	}

	void Waker::signal()
	{
		if (fds[1] >= 0)
		{
			unsigned char byte = 1;
			// Full means a signal is waiting already.
			ssize_t ignored = write(fds[1], &byte, 1);
			(void)ignored;
		}
	}

	void Waker::drain()
	{
		if (fds[0] >= 0)
		{
			unsigned char buffer[64];
			while (read(fds[0], buffer, sizeof(buffer)) > 0)
			{
			}
		}
	}

	namespace detail
	{
		// A connect to the user's own listener on `name`: the user's directory
		// and the socket in it are checked before each try, and anything not
		// the user's own is refused at once rather than waited out. Nothing
		// there yet (the directory or the socket) is a name nobody listens on.
		void* connect(const char* name, int timeoutMs, const std::atomic<bool>* cancel)
		{
			setLastError(NATIVE_IPC_ERROR_FAILED);
			std::string directory = userDirectory();
			std::string pipeName = makePipeName(directory, name);
			if (pipeName.size() >= sizeof(((sockaddr_un*)nullptr)->sun_path))
			{
				return nullptr;
			}

			sockaddr_un address;
			std::memset(&address, 0, sizeof(address));
			address.sun_family = AF_UNIX;
			std::memcpy(address.sun_path, pipeName.data(), pipeName.size());
			address.sun_path[pipeName.size()] = '\0';

			bool forever = timeoutMs <= 0;
			auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(forever ? 0 : timeoutMs);
			while (true)
			{
				if (cancel != nullptr && cancel->load())
				{
					return nullptr;
				}

				int present = checkUserDirectory(directory, false);
				if (present > 0)
				{
					present = checkSocketPath(pipeName);
				}
				if (present < 0)
				{
					return nullptr;
				}

				int error = ENOENT;
				int fd = -1;
				if (present > 0)
				{
					fd = socket(AF_UNIX, SOCK_STREAM, 0);
					if (fd < 0)
					{
						return nullptr;
					}

					// Non-blocking before it connects: a listener whose backlog is
					// full answers EAGAIN on Linux, tried again below like a name
					// nobody listens on yet, where the connect waited in the
					// kernel for it to take someone, past any deadline. macOS
					// refuses one at once, and either may say the connect is in
					// progress, which is waited for a slice at most.
					configureConnectedSocket(fd);
					int result = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
					error = result == 0 ? 0 : errno;
					if (result != 0 && error == EINPROGRESS)
					{
						auto slice = std::chrono::steady_clock::now() + std::chrono::milliseconds(CONNECT_WAIT_SLICE_MS);
						socklen_t length = sizeof(error);
						if (!waitReady(fd, POLLOUT, slice) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0)
						{
							error = EAGAIN;
						}
					}
					if (error != 0)
					{
						close(fd);
						fd = -1;
					}
				}

				if (fd >= 0)
				{
					uint64_t id = newConnectionId();
					unsigned char hello[HELLO_SIZE];
					writeHello(hello, id);
					if (writeSomeRaw(fd, hello, HELLO_SIZE) != HELLO_SIZE)
					{
						close(fd);
						return nullptr;
					}

					// The listener's answer, for as long as the connect has left,
					// and a slice at least: a listener found just before the
					// deadline, or by a connect of one try, has time to answer.
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
						int read = readSomeRaw(fd, answer + got, HELLO_SIZE - got);
						if (read < 0)
						{
							close(fd);
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
							close(fd);
							return nullptr;
						}
						auto now = std::chrono::steady_clock::now();
						if (!forever && now >= answerBy)
						{
							close(fd);
							setLastError(NATIVE_IPC_ERROR_TIMEOUT);
							return nullptr;
						}
						auto slice = now + std::chrono::milliseconds(CONNECT_WAIT_SLICE_MS);
						waitReady(fd, POLLIN, !forever && answerBy < slice ? answerBy : slice);
					}
					uint64_t echoed = 0;
					if (!readHello(answer, &echoed) || echoed != id)
					{
						close(fd);
						setLastError(NATIVE_IPC_ERROR_PROTOCOL);
						return nullptr;
					}
					Connection* connection = makeConnection(fd);
					if (connection != nullptr)
					{
						setLastError(NATIVE_IPC_ERROR_NONE);
					}
					return connection;
				}

				if (error != ENOENT && error != ECONNREFUSED && error != EAGAIN && error != EWOULDBLOCK)
				{
					return nullptr;
				}
				if (!anotherTry(forever, deadline))
				{
					setLastError(NATIVE_IPC_ERROR_TIMEOUT);
					return nullptr;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(CONNECT_WAIT_SLICE_MS));
			}
		}

		bool waitAny(void* handle, bool read, bool write, Waker& waker, int timeoutMs)
		{
			pollfd entries[2];
			int count = 0;
			int fd = (read || write) ? descriptorOf(handle) : -1;
			if (fd >= 0)
			{
				entries[count].fd = fd;
				entries[count].events = static_cast<short>((read ? POLLIN : 0) | (write ? POLLOUT : 0));
				entries[count].revents = 0;
				count++;
			}
			int wakerIndex = -1;
			if (waker.fds[0] >= 0)
			{
				wakerIndex = count;
				entries[count].fd = waker.fds[0];
				entries[count].events = POLLIN;
				entries[count].revents = 0;
				count++;
			}
			int ready = poll(count > 0 ? entries : nullptr, static_cast<nfds_t>(count), timeoutMs);
			if (ready > 0 && wakerIndex >= 0 && (entries[wakerIndex].revents & POLLIN) != 0)
			{
				waker.drain();
				return true;
			}
			return false;
		}

		bool handshaking(void* handle)
		{
			Listener* listener = asListener(handle);
			return listener != nullptr && !listener->handshaking.empty();
		}
	}
}

using namespace native_ipc;

// The listener for `name`, or nothing if another listener has it.
//
// The name is held by a lock: a live listener's name is refused, so a second
// listen on a name in use cannot take the first's place and its clients, and
// only a stale file (from a listener that has gone) is removed.
//
// In the user's own directory, made if it is not there yet; refused if what
// is there is not the user's own (see checkUserDirectory).
extern "C" void* native_ipc_listen(const char* name)
{
	setLastError(NATIVE_IPC_ERROR_FAILED);
	std::string directory = userDirectory();
	if (checkUserDirectory(directory, true) < 0)
	{
		return nullptr;
	}
	std::string pipeName = makePipeName(directory, name);
	if (pipeName.size() + 5 >= sizeof(((sockaddr_un*)nullptr)->sun_path))
	{
		return nullptr;
	}

	int lockFd = lockName(pipeName);
	if (lockFd < 0)
	{
		return nullptr;
	}

	int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listenFd < 0)
	{
		close(lockFd);
		setLastError(NATIVE_IPC_ERROR_FAILED);
		return nullptr;
	}

	sockaddr_un address;
	std::memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, pipeName.data(), pipeName.size());
	address.sun_path[pipeName.size()] = '\0';

	unlink(pipeName.c_str());
	if (bind(listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(listenFd, PIPE_LISTEN_BACKLOG) < 0)
	{
		close(listenFd);
		close(lockFd);
		setLastError(NATIVE_IPC_ERROR_FAILED);
		return nullptr;
	}

	auto* listener = new (std::nothrow) Listener();
	if (listener == nullptr)
	{
		close(listenFd);
		close(lockFd);
		setLastError(NATIVE_IPC_ERROR_FAILED);
		return nullptr;
	}
	setNonBlocking(listenFd);
	fcntl(listenFd, F_SETFD, FD_CLOEXEC);
	listener->kind = KIND_LISTENER;
	listener->listenFd = listenFd;
	listener->lockFd = lockFd;
	std::memcpy(listener->path, pipeName.data(), pipeName.size());
	listener->path[pipeName.size()] = '\0';
	listener->lockRefreshed = std::chrono::steady_clock::now();
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

	// The clients the socket has taken, each put to one side to say hello.
	for (int i = 0; i < ACCEPTS_PER_CALL; i++)
	{
		int fd = accept(listener->listenFd, nullptr, nullptr);
		if (fd < 0)
		{
			break;
		}
		configureConnectedSocket(fd);
		Pending pending;
		pending.fd = fd;
		pending.got = 0;
		pending.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(HELLO_TIMEOUT_MS);
		listener->handshaking.push_back(pending);
	}

	// Their hellos. One whose hello is not this protocol's is answered with
	// this end's, so a client of another version can say why, and let go.
	auto now = std::chrono::steady_clock::now();
	for (size_t i = 0; i < listener->handshaking.size();)
	{
		Pending& pending = listener->handshaking[i];
		int read = readSomeRaw(pending.fd, pending.hello + pending.got, HELLO_SIZE - pending.got);
		if (read > 0)
		{
			pending.got += read;
		}
		if (read < 0 || (pending.got < HELLO_SIZE && now >= pending.deadline))
		{
			close(pending.fd);
			listener->handshaking.erase(listener->handshaking.begin() + i);
			continue;
		}
		if (pending.got < HELLO_SIZE)
		{
			i++;
			continue;
		}

		int fd = pending.fd;
		uint64_t id = 0;
		bool spoken = readHello(pending.hello, &id);
		listener->handshaking.erase(listener->handshaking.begin() + i);
		unsigned char answer[HELLO_SIZE];
		writeHello(answer, spoken ? id : 0);
		bool answered = writeSomeRaw(fd, answer, HELLO_SIZE) == HELLO_SIZE;
		if (!spoken || !answered)
		{
			close(fd);
			continue;
		}
		Connection* connection = makeConnection(fd);
		if (connection != nullptr)
		{
			return connection;
		}
	}
	return nullptr;
}

extern "C" void native_ipc_close(void* handle)
{
	if (Connection* connection = asConnection(handle))
	{
		close(connection->fd);
		connection->kind = 0;
		delete connection;
		return;
	}
	if (Listener* listener = asListener(handle))
	{
		for (Pending& pending : listener->handshaking)
		{
			close(pending.fd);
		}
		if (listener->listenFd >= 0)
		{
			close(listener->listenFd);
			unlink(listener->path);
		}
		if (listener->lockFd >= 0)
		{
			// Removed while still held, so nobody locks the file on its way
			// out; see lockName.
			std::string lockPath = std::string(listener->path) + ".lock";
			unlink(lockPath.c_str());
			close(listener->lockFd);
		}
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

	int bytesAvailable = 0;
	if (ioctl(connection->fd, FIONREAD, &bytesAvailable) == 0)
	{
		return bytesAvailable;
	}
	if (errno == EBADF || errno == ENOTCONN || errno == ECONNRESET)
	{
		return -1;
	}
	return 0;
}

extern "C" int native_ipc_read(void* handle, unsigned char* buffer, int size)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr || buffer == nullptr || size <= 0)
	{
		return -1;
	}

	// The bytes asked for are ones the socket said it holds, so a read that
	// finds none yet waits for them a while, as Windows' does: it never waits
	// for good on a socket that is not blocking.
	int fd = connection->fd;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(IO_TIMEOUT_MS);
	int bytesReadTotal = 0;
	while (bytesReadTotal < size)
	{
		ssize_t bytesRead = recv(fd, reinterpret_cast<char*>(buffer) + bytesReadTotal, static_cast<size_t>(size - bytesReadTotal), 0);
		if (bytesRead > 0)
		{
			bytesReadTotal += static_cast<int>(bytesRead);
			continue;
		}
		if (bytesRead == 0)
		{
			return -1;
		}
		if (errno == EINTR)
		{
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK)
		{
			// `buffer` stays where it is meanwhile: the caller holds it.
			BlockingScope waiting;
			if (waitReady(fd, POLLIN, deadline))
			{
				continue;
			}
		}
		return -1;
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
	return readSomeRaw(connection->fd, buffer, size);
}

extern "C" int native_ipc_write_some(void* handle, const unsigned char* buffer, int size)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr || buffer == nullptr || size < 0)
	{
		return -1;
	}
	return writeSomeRaw(connection->fd, buffer, size);
}

// All of `buffer`, waiting up to IO_TIMEOUT_MS: for tests and tools. It waits
// outside a host collector's reach (see Config.h). It does not wait without a
// deadline for a peer that has stopped reading.
extern "C" bool native_ipc_write(void* handle, const unsigned char* buffer, int size)
{
	Connection* connection = asConnection(handle);
	if (connection == nullptr || buffer == nullptr || size <= 0)
	{
		return false;
	}

	int fd = connection->fd;
	BlockingScope waiting;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(IO_TIMEOUT_MS);
	int bytesWritten = 0;
	while (bytesWritten < size)
	{
		ssize_t sendResult = send(fd, reinterpret_cast<const char*>(buffer) + bytesWritten, static_cast<size_t>(size - bytesWritten), SEND_FLAGS);
		if (sendResult > 0)
		{
			bytesWritten += static_cast<int>(sendResult);
			continue;
		}
		if (sendResult == 0 || errno == EPIPE)
		{
			return false;
		}
		if (errno == EINTR)
		{
			continue;
		}
		if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) && waitReady(fd, POLLOUT, deadline))
		{
			continue;
		}
		return false;
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

	char probe;
	ssize_t result = recv(connection->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
	if (result > 0)
	{
		return true;
	}
	if (result == 0)
	{
		return false;
	}
	return errno == EAGAIN || errno == EWOULDBLOCK;
}

// It may wait for a listener, on the caller's thread: outside a host
// collector's reach meanwhile, having copied the name.
extern "C" void* native_ipc_connect(const char* name, int timeoutMs)
{
	BlockingScope waiting;
	return detail::connect(name, timeoutMs, nullptr);
}

// A listener's lock file, its times brought up to date once an hour: a
// cleaner of old files in /tmp would delete a long-lived listener's, since a
// lock held touches nothing of the file, and the next listen on the name
// would make a new one, lock it, and take the name from the listener. Nothing
// for a handle that holds no lock file.
extern "C" void native_ipc_keep_name(void* handle)
{
	Listener* listener = asListener(handle);
	if (listener == nullptr || listener->lockFd < 0)
	{
		return;
	}
	auto now = std::chrono::steady_clock::now();
	if (now - listener->lockRefreshed >= std::chrono::seconds(LOCK_FILE_REFRESH_SECONDS))
	{
		futimes(listener->lockFd, nullptr);
		listener->lockRefreshed = now;
	}
}

// The descriptor a reader waits on: a connection's socket, or a listener's.
// Read under whatever lock the host closes the handle under, and waited on
// after: a descriptor closed and its number reused meanwhile costs one wait,
// not a handle freed under it.
extern "C" int native_ipc_descriptor_of(void* handle)
{
	return descriptorOf(handle);
}

// The reader thread waits here rather than sleeping between passes, which
// through the 8 KB macOS gives a local socket would have the two sides take
// turns at 8 KB a sleep.
extern "C" bool native_ipc_wait_for_work(int descriptor, bool read, bool write, int timeoutMs)
{
	BlockingScope waiting;
	if (descriptor < 0 || (!read && !write))
	{
		poll(nullptr, 0, timeoutMs);
		return false;
	}
	pollfd entry;
	entry.fd = descriptor;
	entry.events = static_cast<short>((read ? POLLIN : 0) | (write ? POLLOUT : 0));
	entry.revents = 0;
	return poll(&entry, 1, timeoutMs) > 0;
}

// No pipe security to look at.
extern "C" bool native_ipc_admits_others_for_test(void* handle)
{
	return false;
}

extern "C" void native_ipc_set_socket_buffer_for_test(int bytes)
{
	testSocketBuffer.store(bytes);
}

#endif
