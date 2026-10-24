#include "Internal.h"

#include <cstdio>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace native_ipc
{
	namespace
	{
		// Why the last listen, accept or connect on this thread failed: what
		// native_ipc_last_error answers, so the caller can tell a name that is
		// not this user's from one in use or not listened on.
		thread_local int lastErrorValue = NATIVE_IPC_ERROR_NONE;

		const unsigned char HELLO_MAGIC[4] = {'N', 'I', 'P', 'C'};

		uint64_t mix(uint64_t value)
		{
			// splitmix64's finaliser.
			value += 0x9e3779b97f4a7c15ULL;
			value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
			value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
			return value ^ (value >> 31);
		}
	}

	void setLastError(int error)
	{
		lastErrorValue = error;
	}

	int lastError()
	{
		return lastErrorValue;
	}

	void writeHello(unsigned char* out, uint64_t id)
	{
		out[0] = HELLO_MAGIC[0];
		out[1] = HELLO_MAGIC[1];
		out[2] = HELLO_MAGIC[2];
		out[3] = HELLO_MAGIC[3];
		out[4] = static_cast<unsigned char>(NATIVE_IPC_PROTOCOL_VERSION & 0xff);
		out[5] = static_cast<unsigned char>((NATIVE_IPC_PROTOCOL_VERSION >> 8) & 0xff);
		// Flags: none are defined, and a peer sending any is still spoken to.
		out[6] = 0;
		out[7] = 0;
		for (int i = 0; i < 8; i++)
		{
			out[8 + i] = static_cast<unsigned char>((id >> (8 * i)) & 0xff);
		}
	}

	bool readHello(const unsigned char* in, uint64_t* id)
	{
		if (in[0] != HELLO_MAGIC[0] || in[1] != HELLO_MAGIC[1] || in[2] != HELLO_MAGIC[2] || in[3] != HELLO_MAGIC[3])
		{
			return false;
		}
		int version = in[4] | (in[5] << 8);
		if (version != NATIVE_IPC_PROTOCOL_VERSION)
		{
			return false;
		}
		uint64_t value = 0;
		for (int i = 0; i < 8; i++)
		{
			value |= static_cast<uint64_t>(in[8 + i]) << (8 * i);
		}
		*id = value;
		return true;
	}

	uint64_t newConnectionId()
	{
		static std::atomic<uint64_t> counter(0);
#if defined(_WIN32)
		uint64_t process = static_cast<uint64_t>(GetCurrentProcessId());
#else
		uint64_t process = static_cast<uint64_t>(getpid());
#endif
		uint64_t now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		uint64_t id = 0;
		while (id == 0)
		{
			id = mix(now ^ (process << 32) ^ mix(counter.fetch_add(1) + 1));
		}
		return id;
	}

	uint64_t hashName(const std::string& input)
	{
		// 64-bit FNV-1a.
		uint64_t hash = 1469598103934665603ULL;
		for (unsigned char c : input)
		{
			hash ^= c;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	std::string hex64(uint64_t value)
	{
		char hex[17];
		std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(value));
		return std::string(hex);
	}
}

extern "C" int native_ipc_last_error(void)
{
	return native_ipc::lastError();
}
