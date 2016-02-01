#include "http-bridge.h"
#include "http-bridge_generated.h"
#include <string>
#include <string.h>
#include <stdio.h>

#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
#include <Ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h> // #TODO Get rid of this at 1.0 if we no longer set sockets to non-blocking
#endif

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

namespace hb
{
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	// These are arbitrary maximums, chosen for sanity, and to fit within a signed 32-bit int
	static const int MaxHeaderKeyLen = 1024;
	static const int MaxHeaderValueLen = 1024 * 1024;

	const char* Header_Content_Length = "Content-Length";

	void PanicMsg(const char* file, int line, const char* msg)
	{
		fprintf(stderr, "httpbridge panic %s:%d: %s\n", file, line, msg);
	}

	HTTPBRIDGE_NORETURN_PREFIX void BuiltinTrap()
	{
#ifdef _MSC_VER
		__debugbreak();
#else
		__builtin_trap();
#endif
	}

	bool Startup()
	{
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
		WSADATA wsaData;
		int wsa_startup = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (wsa_startup != 0)
		{
			printf("WSAStartup failed: %d\n", wsa_startup);
			return false;
		}
#endif
		return true;
	}

	void Shutdown()
	{
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
		WSACleanup();
#endif
	}

	const char* VersionString(HttpVersion version)
	{
		switch (version)
		{
		case HttpVersion10: return "HTTP/1.0";
		case HttpVersion11: return "HTTP/1.1";
		case HttpVersion2: return "HTTP/2";
		default: return "HTTP/?.?";
		}
	}

	const char*	StatusString(StatusCode status)
	{
		switch (status)
		{
		case Status100_Continue:                        return "Continue";
		case Status101_Switching_Protocols:             return "Switching Protocols";
		case Status102_Processing:                      return "Processing";
		case Status200_OK:                              return "OK";
		case Status201_Created:                         return "Created";
		case Status202_Accepted:                        return "Accepted";
		case Status203_Non_Authoritative_Information:   return "Non-Authoritative Information";
		case Status204_No_Content:                      return "No Content";
		case Status205_Reset_Content:                   return "Reset Content";
		case Status206_Partial_Content:                 return "Partial Content";
		case Status207_Multi_Status:                    return "Multi-Status";
		case Status208_Already_Reported:                return "Already Reported";
		case Status226_IM_Used:                         return "IM Used";
		case Status300_Multiple_Choices:                return "Multiple Choices";
		case Status301_Moved_Permanently:               return "Moved Permanently";
		case Status302_Found:                           return "Found";
		case Status303_See_Other:                       return "See Other";
		case Status304_Not_Modified:                    return "Not Modified";
		case Status305_Use_Proxy:                       return "Use Proxy";
		case Status307_Temporary_Redirect:              return "Temporary Redirect";
		case Status308_Permanent_Redirect:              return "Permanent Redirect";
		case Status400_Bad_Request:                     return "Bad Request";
		case Status401_Unauthorized:                    return "Unauthorized";
		case Status402_Payment_Required:                return "Payment Required";
		case Status403_Forbidden:                       return "Forbidden";
		case Status404_Not_Found:                       return "Not Found";
		case Status405_Method_Not_Allowed:              return "Method Not Allowed";
		case Status406_Not_Acceptable:                  return "Not Acceptable";
		case Status407_Proxy_Authentication_Required:   return "Proxy Authentication Required";
		case Status408_Request_Timeout:                 return "Request Timeout";
		case Status409_Conflict:                        return "Conflict";
		case Status410_Gone:                            return "Gone";
		case Status411_Length_Required:                 return "Length Required";
		case Status412_Precondition_Failed:             return "Precondition Failed";
		case Status413_Payload_Too_Large:               return "Payload Too Large";
		case Status414_URI_Too_Long:                    return "URI Too Long";
		case Status415_Unsupported_Media_Type:          return "Unsupported Media Type";
		case Status416_Range_Not_Satisfiable:           return "Range Not Satisfiable";
		case Status417_Expectation_Failed:              return "Expectation Failed";
		case Status421_Misdirected_Request:             return "Misdirected Request";
		case Status422_Unprocessable_Entity:            return "Unprocessable Entity";
		case Status423_Locked:                          return "Locked";
		case Status424_Failed_Dependency:               return "Failed Dependency";
		case Status425_Unassigned:                      return "Unassigned";
		case Status426_Upgrade_Required:                return "Upgrade Required";
		case Status427_Unassigned:                      return "Unassigned";
		case Status428_Precondition_Required:           return "Precondition Required";
		case Status429_Too_Many_Requests:               return "Too Many Requests";
		case Status430_Unassigned:                      return "Unassigned";
		case Status431_Request_Header_Fields_Too_Large: return "Request Header Fields Too Large";
		case Status500_Internal_Server_Error:           return "Internal Server Error";
		case Status501_Not_Implemented:                 return "Not Implemented";
		case Status502_Bad_Gateway:                     return "Bad Gateway";
		case Status503_Service_Unavailable:             return "Service Unavailable";
		case Status504_Gateway_Timeout:                 return "Gateway Timeout";
		case Status505_HTTP_Version_Not_Supported:      return "HTTP Version Not Supported";
		case Status506_Variant_Also_Negotiates:         return "Variant Also Negotiates";
		case Status507_Insufficient_Storage:            return "Insufficient Storage";
		case Status508_Loop_Detected:                   return "Loop Detected";
		case Status509_Unassigned:                      return "Unassigned";
		case Status510_Not_Extended:                    return "Not Extended";
		case Status511_Network_Authentication_Required: return "Network Authentication Required";
		}
		return "Unrecognized HTTP Status Code";
	}

	size_t Hash16B(uint64_t pair[2])
	{
		// This is the final mixing step of xxhash64
		uint64_t PRIME64_2 = 14029467366897019727ULL;
		uint64_t PRIME64_3 = 1609587929392839161ULL;
		uint64_t a = pair[0];
		a ^= a >> 33;
		a *= PRIME64_2;
		a ^= a >> 29;
		a *= PRIME64_3;
		a ^= a >> 32;
		uint64_t b = pair[1];
		b ^= b >> 33;
		b *= PRIME64_2;
		b ^= b >> 29;
		b *= PRIME64_3;
		b ^= b >> 32;
		return (size_t) (a ^ b);
	}

	// Returns the length of the string, excluding the null terminator
	// You need a buffer of 11 bytes to be able to hold any result, including the null terminator
	int U32toa(uint32_t v, char* buf, size_t buf_size)
	{
		int i = 0;
		for (;;)
		{
			if (i >= (int) buf_size - 1)
				break;
			uint32_t new_v = v / 10;
			char mod_v = (char) (v % 10);
			buf[i++] = mod_v + '0';
			if (new_v == 0)
				break;
			v = new_v;
		}
		buf[i] = 0;
		i--;
		for (int j = 0; j < i; j++, i--)
		{
			char t = buf[j];
			buf[j] = buf[i];
			buf[i] = t;
		}
		return i + 1;
	}

	// Returns the length of the string, excluding the null terminator
	// You need a buffer of 21 bytes to be able to hold any result, including the null terminator
	int U64toa(uint64_t v, char* buf, size_t buf_size)
	{
		if (v <= 4294967295u)
			return U32toa((uint32_t) v, buf, buf_size);
		int i = 0;
		for (;;)
		{
			if (i >= (int) buf_size - 1)
				break;
			uint64_t new_v = v / 10;
			char mod_v = (char) (v % 10);
			buf[i++] = mod_v + '0';
			if (new_v == 0)
				break;
			v = new_v;
		}
		buf[i] = 0;
		i--;
		for (int j = 0; j < i; j++, i--)
		{
			char t = buf[j];
			buf[j] = buf[i];
			buf[i] = t;
		}
		return i + 1;
	}

	template <size_t buf_size>
	int u32toa(uint32_t v, char(&buf)[buf_size])
	{
		return U32toa(v, buf, buf_size);
	}

	template <size_t buf_size>
	int u64toa(uint64_t v, char(&buf)[buf_size])
	{
		return U64toa(v, buf, buf_size);
	}

	uint64_t uatoi64(const char* s, size_t len)
	{
		uint64_t v = 0;
		for (size_t i = 0; i < len; i++)
			v = v * 10 + (s[i] - '0');
		return v;
	}

	uint64_t uatoi64(const char* s)
	{
		uint64_t v = 0;
		for (size_t i = 0; s[i]; i++)
			v = v * 10 + (s[i] - '0');
		return v;
	}

#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
	void SleepNano(int64_t nanoseconds)
	{
		YieldProcessor();
		Sleep((DWORD) (nanoseconds / 1000000));
	}
#else
	void SleepNano(int64_t nanoseconds)
	{
		timespec t;
		t.tv_nsec = nanoseconds % 1000000000; 
		t.tv_sec = (nanoseconds - t.tv_nsec) / 1000000000;
		nanosleep(&t, nullptr);
	}
#endif

	// Read 32-bit little endian
	uint32_t Read32LE(const void* buf)
	{
		const uint8_t* b = (const uint8_t*) buf;
		return ((uint32_t) b[0]) | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
	}

	// Write 32-bit little endian
	void Write32LE(void* buf, uint32_t v)
	{
		uint8_t* b = (uint8_t*) buf;
		b[0] = v;
		b[1] = v >> 8;
		b[2] = v >> 16;
		b[3] = v >> 24;
	}

	void* Alloc(size_t size, Logger* logger, bool panicOnFail)
	{
		void* b = malloc(size);
		if (b == nullptr)
		{
			if (logger != nullptr)
				logger->Logf("Out of memory allocating %llu bytes", (uint64_t) size);
			if (panicOnFail)
				HTTPBRIDGE_PANIC("Out of memory (alloc)");
		}
		return b;
	}

	void* Realloc(void* buf, size_t size, Logger* logger, bool panicOnFail)
	{
		void* newBuf = realloc(buf, size);
		if (newBuf == nullptr)
		{
			if (logger != nullptr)
				logger->Logf("Out of memory allocating %llu bytes", (uint64_t) size);
			if (panicOnFail)
				HTTPBRIDGE_PANIC("Out of memory (realloc)");
			return buf;
		}
		return newBuf;
	}

	void Free(void* buf)
	{
		free(buf);
	}

	template<typename T> T min(T a, T b) { return a < b ? a : b; }
	template<typename T> T max(T a, T b) { return a < b ? b : a; }

	static int HexToInt(char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return 10 + c - 'a';
		if (c >= 'A' && c <= 'F')
			return 10 + c - 'A';
		return 0;
	}

	static void BufWrite(uint8_t*& buf, const void* src, size_t len)
	{
		memcpy(buf, src, len);
		buf += len;
	}

	// Round up to the next even number
	template<typename T> T RoundUpEven(T v) { return (v + 1) & ~1; }

	// Round up to the next odd number
	template<typename T> T RoundUpOdd(T v) { return (v & ~1) + 1; }

	hb::HttpVersion TranslateVersion(httpbridge::TxHttpVersion v)
	{
		switch (v)
		{
		case httpbridge::TxHttpVersion_Http10: return HttpVersion10;
		case httpbridge::TxHttpVersion_Http11: return HttpVersion11;
		case httpbridge::TxHttpVersion_Http2: return HttpVersion2;
		}
		HTTPBRIDGE_PANIC("Unknown TxHttpVersion");
		return HttpVersion2;
	}

	httpbridge::TxHttpVersion TranslateVersion(hb::HttpVersion v)
	{
		switch (v)
		{
		case HttpVersion10: return httpbridge::TxHttpVersion_Http10;
		case HttpVersion11: return httpbridge::TxHttpVersion_Http11;
		case HttpVersion2: return httpbridge::TxHttpVersion_Http2;
		}
		HTTPBRIDGE_PANIC("Unknown HttpVersion");
		return httpbridge::TxHttpVersion_Http2;
	}

	int TranslateVersionToFlatBuffer(hb::HttpVersion v)
	{
		return TranslateVersion(v);
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void Logger::Log(const char* msg)
	{
		fputs(msg, stdout);
		auto len = strlen(msg);
		if (len != 0 && msg[len - 1] != '\n')
			fputs("\n", stdout);
	}

	void Logger::Logf(HTTPBRIDGE_PRINTF_FORMAT_Z const char* msg, ...)
	{
		char buf[4096];
		va_list va;
		va_start(va, msg);
#ifdef _MSC_VER
		vsnprintf_s(buf, sizeof(buf), msg, va);
#else
		vsnprintf(buf, sizeof(buf), msg, va);
#endif
		va_end(va);
		buf[sizeof(buf) - 1] = 0;
		Log(buf);
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	ITransport::~ITransport()
	{
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	class TransportTCP : public ITransport
	{
	public:
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
		typedef SOCKET socket_t;
		typedef DWORD setsockopt_t;
		static const uint32_t	Infinite = INFINITE;
		static const socket_t	InvalidSocket = INVALID_SOCKET;
		static const int		ErrWOULDBLOCK = WSAEWOULDBLOCK;
		static const int		ErrTIMEOUT = WSAETIMEDOUT;
		static const int		ErrSEND_BUFFER_FULL = WSAENOBUFS;	// untested
		static const int		ErrSOCKET_ERROR = SOCKET_ERROR;
#else
		typedef int socket_t;
		typedef int setsockopt_t;
		static const uint32_t	Infinite			= 0xFFFFFFFF;
		static const socket_t	InvalidSocket		= (socket_t)(~0);
		static const int		ErrWOULDBLOCK		= EWOULDBLOCK;
		static const int		ErrTIMEOUT			= ETIMEDOUT;
		static const int		ErrSEND_BUFFER_FULL = EMSGSIZE;		// untested
		static const int		ErrSOCKET_ERROR		= -1;
#endif
		static const int		ChunkSize = 1048576;
		static const uint32_t	ReadTimeoutMilliseconds = 500;
		socket_t				Socket = InvalidSocket;

		virtual				~TransportTCP() override;
		virtual bool		Connect(const char* addr) override;
		virtual SendResult	Send(const void* data, size_t size, size_t& sent) override;
		virtual RecvResult	Recv(size_t maxSize, void* data, size_t& bytesRead) override;

	private:
		void			Close();
		bool			SetNonBlocking();
		bool			SetReadTimeout(uint32_t timeoutMilliseconds);
		static int		LastError();
	};

	TransportTCP::~TransportTCP()
	{
		Close();
	}

	void TransportTCP::Close()
	{
		if (Socket != InvalidSocket)
		{
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
			closesocket(Socket);
#else
			::close(Socket);
#endif
			Socket = InvalidSocket;
		}
	}

	bool TransportTCP::Connect(const char* addr)
	{
		std::string port;
		const char* colon = strrchr(addr, ':');
		if (colon != nullptr)
			port = colon + 1;
		else
			return false;

		std::string host(addr, colon - addr);

		addrinfo hints;
		addrinfo* res = nullptr;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = PF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		//hints.ai_flags = AI_V4MAPPED;
		getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
		for (addrinfo* it = res; it; it = it->ai_next)
		{
			Socket = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
			if (Socket == InvalidSocket)
				continue;

			if (it->ai_family == AF_INET6)
			{
				// allow IPv4 on this IPv6 socket
				setsockopt_t only6 = 0;
				if (setsockopt(Socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &only6, sizeof(only6)))
				{
					Close();
					continue;
				}
			}

			if (connect(Socket, it->ai_addr, (int) it->ai_addrlen) == 0)
			{
				//if (!SetNonBlocking())
				//	Log->Log("Unable to set socket to non-blocking");
				if (!SetReadTimeout(ReadTimeoutMilliseconds))
				{
					Log->Log("Unable to set socket read timeout");
					Close();
					continue;
				}
				break;
			}
			else
			{
				const char* proto = it->ai_family == PF_INET6 ? "IPv6" : "IP4";
				Log->Logf("Unable to connect (%s): %d", proto, (int) LastError());
				Close();
			}
		}
		if (res)
			freeaddrinfo(res);

		return Socket != InvalidSocket;
	}

	hb::SendResult TransportTCP::Send(const void* data, size_t size, size_t& sent)
	{
		const char* datab = (const char*) data;
		sent = 0;
		while (sent != size)
		{
			int try_send = (int) (size - sent < ChunkSize ? size - sent : ChunkSize);
			int sent_now = send(Socket, datab + sent, try_send, 0);
			if (sent_now == ErrSOCKET_ERROR)
			{
				int e = LastError();
				if (e == ErrWOULDBLOCK || e == ErrSEND_BUFFER_FULL)
				{
					// #TODO: This concept here, of having the send buffer full, is untested
					return SendResult_BufferFull;
				}
				else
				{
					// #TODO: test possible results here that are not in fact the socket being closed
					return SendResult_Closed;
				}
			}
			else
			{
				sent += sent_now;
			}
		}
		return SendResult_All;
	}

	hb::RecvResult TransportTCP::Recv(size_t maxSize, void* data, size_t& bytesRead)
	{
		char* bdata = (char*) data;
		bytesRead = 0;
		while (bytesRead < maxSize)
		{
			int try_read = (int) (maxSize - bytesRead < ChunkSize ? maxSize - bytesRead : ChunkSize);
			int read_now = recv(Socket, bdata + bytesRead, try_read, 0);
			if (read_now == ErrSOCKET_ERROR)
			{
				int e = LastError();
				// Timeout occurs because we've set a read timeout on the socket.
				// WouldBlock would occur if we were to set the socket to non-blocking.
				if (e == ErrWOULDBLOCK || e == ErrTIMEOUT)
					break;
				// #TODO: test possible error codes here - there might be some that are recoverable
				return RecvResult_Closed;
			}
			else if (read_now == 0)
			{
				// closed gracefully
				return RecvResult_Closed;
			}
			else
			{
				bytesRead += read_now;
				// If we received less than we asked for, then it's very likely that there is no more data waiting
				// for us. In this case, return immediately, instead of facing the prospect of having to wait for
				// the read timeout to complete.
				if (read_now < try_read)
					break;
			}
		}
		return bytesRead == 0 ? RecvResult_NoData : RecvResult_Data;
	}

	int TransportTCP::LastError()
	{
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
		return WSAGetLastError();
#else
		return errno;
#endif
	}

	bool TransportTCP::SetNonBlocking()
	{
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
		u_long mode = 1;
		return ioctlsocket(Socket, FIONBIO, &mode) == 0;
#else
		int flags = fcntl(Socket, F_GETFL, 0);
		if (flags < 0)
			return false;
		flags |= O_NONBLOCK;
		return fcntl(Socket, F_SETFL, flags) == 0;
#endif
	}

	bool TransportTCP::SetReadTimeout(uint32_t timeoutMilliseconds)
	{
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
		uint32_t tv = timeoutMilliseconds;
		return setsockopt(Socket, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof(tv)) == 0;
#else
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = timeoutMilliseconds * 1000;
		return setsockopt(Socket, SOL_SOCKET, SO_RCVTIMEO, (const void*) &tv, sizeof(tv)) == 0;
#endif
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	// Cache of header pairs received. The sizes here do not include a null terminator.
	class HeaderCacheRecv
	{
	public:
		void Insert(uint16_t id, int32_t keyLen, const void* key, int32_t valLen, const void* val);
		void Get(uint16_t id, int32_t& keyLen, const void*& key, int32_t& valLen, const void*& val);		// A non-existent item will yield 0,0,0,0

	private:
		struct Item
		{
			Vector<uint8_t> Key;
			Vector<uint8_t> Value;
		};
		Vector<Item> Items;
	};

	void HeaderCacheRecv::Insert(uint16_t id, int32_t keyLen, const void* key, int32_t valLen, const void* val)
	{
		while (Items.Size() <= (int32_t) id)
			Items.Push(Item());
		Items[id].Key.Resize(keyLen);
		Items[id].Value.Resize(valLen);
		memcpy(&Items[id].Key[0], key, keyLen);
		memcpy(&Items[id].Value[0], val, valLen);
	}

	void HeaderCacheRecv::Get(uint16_t id, int32_t& keyLen, const void*& key, int32_t& valLen, const void*& val)
	{
		if (id >= Items.Size())
		{
			keyLen = 0;
			valLen = 0;
			key = nullptr;
			val = nullptr;
		}
		else
		{
			keyLen = Items[id].Key.Size();
			valLen = Items[id].Value.Size();
			key = &Items[id].Key[0];
			val = &Items[id].Value[0];
		}
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	bool ConstString::StartsWith(const char* s) const
	{
		if (Data == nullptr)
			return s == nullptr || !s[0];
		int i = 0;
		for (; Data[i] == s[i] && Data[i]; i++) {}
		return !s[i];
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	Buffer::Buffer()
	{
	}

	Buffer::~Buffer()
	{
		Free(Data);
	}
	
	void Buffer::Clear()
	{
		Free(Data);
		Data = nullptr;
		Count = 0;
		Capacity = 0;
	}

	uint8_t* Buffer::Preallocate(size_t n)
	{
		while (Count + n > Capacity)
			GrowCapacityOrPanic();
		return Data + Count;
	}

	void Buffer::EraseFromStart(size_t n)
	{
		memmove(Data, Data + n, Count - n);
		Count -= n;
	}

	void Buffer::Write(const void* buf, size_t n)
	{
		while (Count + n > Capacity)
			GrowCapacityOrPanic();
		memcpy(Data + Count, buf, n);
		Count += n;
	}
	
	bool Buffer::TryWrite(const void* buf, size_t n)
	{
		while (Count + n > Capacity)
		{
			if (!TryGrowCapacity())
				return false;
		}
		memcpy(Data + Count, buf, n);
		Count += n;
		return true;
	}

	void Buffer::WriteStr(const char* s)
	{
		Write(s, strlen(s));
	}

	void Buffer::WriteUInt64(uint64_t v)
	{
		// 18446744073709551615 (max uint64) is 20 characters, and U64toa insists on adding a null terminator
		while (Count + 21 > Capacity)
			GrowCapacityOrPanic();
		Count += U64toa(v, (char*) (Data + Count), 21);
	}

	void Buffer::GrowCapacityOrPanic()
	{
		Capacity = Capacity == 0 ? 64 : Capacity * 2;
		Data = (uint8_t*) Realloc(Data, Capacity, nullptr);
	}

	bool Buffer::TryGrowCapacity()
	{
		auto newCapacity = Capacity == 0 ? 64 : Capacity * 2;
		auto newData = (uint8_t*) Realloc(Data, newCapacity, nullptr, false);
		if (newData == nullptr)
		{
			return false;
		}
		else
		{
			Capacity = newCapacity;
			Data = newData;
			return true;
		}
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	InFrame::InFrame()
	{
		IsHeader = false;
		IsLast = false;
		IsAborted = false;
		Request = nullptr;
		Reset();
	}

	InFrame::~InFrame()
	{
		Reset();
	}

	void InFrame::Reset()
	{
		if (Request != nullptr)
		{
			if (!Request->BodyBuffer.IsPointerInside(BodyBytes))
				Free(BodyBytes);

			if (IsAborted)
			{
				Request->DecrementLiveness();
				Request->DecrementLiveness();
			}
			else if (IsLast)
			{
				Request->DecrementLiveness();
			}
			// Note that Request can have destroyed itself by this point, so NULL is the only legal value for it now
			Request = nullptr;
		}

		IsHeader = false;
		IsLast = false;
		IsAborted = false;

		BodyBytes = nullptr;
		BodyBytesLen = 0;
	}

	void InFrame::DeleteRequestAndReset()
	{
		if (Request != nullptr)
		{
			Request->DecrementLiveness();
			Request->DecrementLiveness();
		}
		Request = nullptr;
		Reset();
	}

	bool InFrame::ResendWhenBodyIsDone()
	{
		return Request->Backend->ResendWhenBodyIsDone(*this);
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	Backend::Backend()
	{
		MaxWaitingBufferTotal.store(1024 * 1024 * 1024);
		MaxAutoBufferSize.store(16 * 1024 * 1024);
		InitialBufferSize.store(4096);
		BufferedRequestsTotalBytes.store(0);
	}

	Backend::~Backend()
	{
		Close();
	}

	Logger* Backend::AnyLog()
	{
		return Log ? Log : &NullLog;
	}

	bool Backend::Connect(const char* network, const char* addr)
	{
		ITransport* tx = nullptr;
		if (strcmp(network, "tcp") == 0)
			tx = new TransportTCP();
		else
			return false;

		if (!Connect(tx, addr))
		{
			delete tx;
			return false;
		}
		return true;
	}

	bool Backend::IsConnected()
	{
		return Transport != nullptr;
	}

	void Backend::Close()
	{
		// Pull items out of CurrentRequests hash map, because we can't iterate over them while deleting them
		std::vector<Request*> waiting;
		for (auto& item : CurrentRequests)
			waiting.push_back(item.second.Request);

		for (auto item : waiting)
		{
			while (!item->DecrementLiveness()) {}
		}

		HTTPBRIDGE_ASSERT(BufferedRequestsTotalBytes == 0);

		delete Transport;
		Transport = nullptr;
		delete HeaderCacheRecv;
		HeaderCacheRecv = nullptr;
		RecvBuf.Clear();
	}

	bool Backend::Connect(ITransport* transport, const char* addr)
	{
		Close();
		transport->Log = Log != nullptr ? Log : &NullLog;
		if (transport->Connect(addr))
		{
			ThreadId = std::this_thread::get_id();
			Transport = transport;
			HeaderCacheRecv = new hb::HeaderCacheRecv();
			return true;
		}
		return false;
	}

	SendResult Backend::Send(Response& response)
	{
		GiantLock.lock();
		// TODO: rate limiting. How? Before attempting to send this frame, we return SendResult_BufferFull,
		// then we wait for the server to tell us that we are clear to send again. This does imply
		// that the server would also need to tell us when we are not clear to send.
		RequestState* rs = GetRequestOrDie(response.Channel, response.Stream);
		bool isResponseHeader = response.Status != StatusMeta_BodyPart;
		HTTPBRIDGE_ASSERT(isResponseHeader == !rs->IsResponseHeaderSent);
		if (!isResponseHeader)
			HTTPBRIDGE_ASSERT(response.HeaderCount() == 0);

		if (isResponseHeader)
		{
			const char* contentLen = response.HeaderByName("Content-Length");
			if (contentLen != nullptr)
			{
				// If you set Content-Length, then that is the expected length of the response.
				rs->ResponseBodyRemaining = uatoi64(contentLen);
			}
			else
			{
				// If you don't set Content-Length, then whatever bytes are inside your response, is the expected length of the response.
				rs->ResponseBodyRemaining = response.BodyBytes();
				if (response.BodyBytes() != 0)
					response.AddHeader_ContentLength(response.BodyBytes());
			}
		}
		HTTPBRIDGE_ASSERT(rs->ResponseBodyRemaining >= response.BodyBytes()); // You have sent more data than Content-Length
		rs->ResponseBodyRemaining -= response.BodyBytes();
		GiantLock.unlock();

		bool isLast = rs->ResponseBodyRemaining == 0;
		if (isLast)
		{
			rs->Request->DecrementLiveness(); // this takes the GiantLock
			rs = nullptr;
		}

		size_t offset = 0;
		size_t len = 0;
		void* buf = nullptr;
		response.FinishFlatbuffer(buf, len, isLast);
		TransmitLock.lock();
		while (offset != len)
		{
			size_t sent = 0;
			auto res = Transport->Send((uint8_t*) buf + offset, len - offset, sent);
			offset += sent;
			if (res == SendResult_Closed)
			{
				TransmitLock.unlock();
				return res;
			}
		}
		TransmitLock.unlock();
		return SendResult_All;
	}

	SendResult Backend::Send(const Request* request, StatusCode status)
	{
		Response response(request, status);
		return Send(response);
	}

	size_t Backend::SendBodyPart(const Request* request, const void* body, size_t len)
	{
		Response response(request, StatusMeta_BodyPart);
		response.SetBody(body, len);
		return Send(response);
	}

	bool Backend::Recv(InFrame& frame)
	{
		frame.Reset();

		if (!IsConnected())
			return false;

		// It is not strictly necessary that Recv is called from the same thread that called Connect(). We simply use the
		// Connect() call as a moment in time when we can record a known thread, and use that to sense-check future
		// calls to Recv.
		if (std::this_thread::get_id() != ThreadId)
			LogAndPanic("Recv() called from a different thread than the one that called Connect()");

		RecvResult res = RecvInternal(frame);
		if (res != RecvResult_Data)
			return false;

		StreamKey streamKey = MakeStreamKey(*frame.Request);

		if (frame.IsHeader)
		{
			RequestState rs;
			rs.Request = frame.Request;
			rs.ResponseBodyRemaining = ResponseBodyUninitialized;
			rs.IsResponseHeaderSent = false;
			GiantLock.lock();
			CurrentRequests[streamKey] = rs;
			GiantLock.unlock();

			if (frame.Request->BodyLength == 0)
			{
				HTTPBRIDGE_ASSERT(frame.IsLast);
				frame.Request->IsBuffered = true;
				frame.Request->BodyBuffer.Data = frame.BodyBytes;
				frame.Request->BodyBuffer.Capacity = frame.BodyBytesLen;
				frame.Request->BodyBuffer.Count = frame.BodyBytesLen;
				BufferedRequestsTotalBytes += frame.BodyBytesLen;
				return true;
			}

			if (frame.Request->BodyLength <= MaxAutoBufferSize)
			{
				// Automatically place requests into the 'ResendWhenBodyIsDone' queue
				if (ResendWhenBodyIsDone(frame))
					return true;
				frame.Reset();
				return false;
			}

			return true;
		}

		return true;
	}

	bool Backend::ResendWhenBodyIsDone(InFrame& frame)
	{
		Request* request = frame.Request;

		if (!frame.IsHeader || frame.IsLast || request->BodyLength == 0)
			LogAndPanic("ResendWhenBodyIsDone may only be called on the first frame of a request (the header frame), with a non-zero body length");

		GiantLock.lock();
		StreamKey key = MakeStreamKey(*request);
		if (CurrentRequests.find(key) == CurrentRequests.end())
			LogAndPanic("ResendWhenBodyIsDone called on an unknown request");
		GiantLock.unlock();

		size_t initialSize = min(InitialBufferSize.load(), (size_t) request->BodyLength);
		initialSize = max(initialSize, frame.BodyBytesLen);
		initialSize = max(initialSize, (size_t) 16);

		if (initialSize + (uint64_t) BufferedRequestsTotalBytes > (uint64_t) MaxWaitingBufferTotal)
		{
			AnyLog()->Log("MaxWaitingBufferTotal exceeded");
			SendResponse(*request, Status503_Service_Unavailable);
			return false;
		}

		request->BodyBuffer.Data = (uint8_t*) Alloc(initialSize, AnyLog(), false);
		if (request->BodyBuffer.Data == nullptr)
		{
			AnyLog()->Log("Alloc for request buffer failed");
			SendResponse(*request, Status503_Service_Unavailable);
			return false;
		}

		memcpy(request->BodyBuffer.Data, frame.BodyBytes, frame.BodyBytesLen);
		request->BodyBuffer.Count = frame.BodyBytesLen;
		request->BodyBuffer.Capacity = initialSize;
		Free(frame.BodyBytes);
		frame.BodyBytes = request->BodyBuffer.Data;

		BufferedRequestsTotalBytes += (size_t) request->BodyBuffer.Capacity;

		request->IsBuffered = true;
		return true;
	}

	void Backend::RequestDestroyed(const StreamKey& key)
	{
		GiantLock.lock();
		auto cr = CurrentRequests.find(key);
		HTTPBRIDGE_ASSERT(cr != CurrentRequests.end());
		Request* request = cr->second.Request;

		if (request->IsBuffered)
		{
			if (BufferedRequestsTotalBytes < request->BodyBuffer.Capacity)
				LogAndPanic("BufferedRequestsTotalBytes underflow");
			BufferedRequestsTotalBytes -= (size_t) request->BodyBuffer.Capacity;
		}

		CurrentRequests.erase(key);
		GiantLock.unlock();
	}

	// TODO: Experiment with reading exactly one frame at a time. First read 4 bytes for the frame length, and thereafter read only
	// exactly as much as is necessary for that single frame. Then we don't need to do a memmove, and things become simpler.
	// See if that really produces faster results for average workloads.
	RecvResult Backend::RecvInternal(InFrame& inframe)
	{
		if (Transport == nullptr)
			return RecvResult_Closed;

		const size_t maxRecv = 65536;
		const size_t maxBufSize = 1024*1024;
		RecvBuf.Preallocate(maxRecv);

		if (RecvBuf.Count >= maxBufSize)
		{
			AnyLog()->Logf("Server is trying to send us a frame larger than %d bytes. Closing connection.", (int) maxBufSize);
			Close();
			return RecvResult_Closed;
		}

		// If we don't have at least one frame ready, then read more
		if (RecvBuf.Count < 4 || RecvBuf.Count < 4 + Read32LE(RecvBuf.Data))
		{
			size_t read = 0;
			auto result = Transport->Recv(maxRecv, RecvBuf.Data + RecvBuf.Count, read);
			if (result == RecvResult_Closed)
			{
				AnyLog()->Logf("Server closed connection");
				Close();
				return result;
			}
			RecvBuf.Count += read;
		}

		// Process frame
		if (RecvBuf.Count >= 4)
		{
			uint32_t frameSize = Read32LE(RecvBuf.Data);
			if (RecvBuf.Count >= frameSize + 4)
			{
				// We have a frame to process.
				const httpbridge::TxFrame* txframe = httpbridge::GetTxFrame((uint8_t*) RecvBuf.Data + 4);
				FrameStatus headStatus = FrameOK;
				FrameStatus bodyStatus = FrameOK;
				if (txframe->frametype() == httpbridge::TxFrameType_Header)
				{
					headStatus = UnpackHeader(txframe, inframe);
					inframe.IsHeader = true;
					inframe.IsLast = !!(txframe->flags() & httpbridge::TxFrameFlags_Final);
					bodyStatus = UnpackBody(txframe, inframe);
					if (headStatus == FrameOK && !inframe.Request->ParseURI())
						headStatus = FrameURITooLong;
				}
				else if (txframe->frametype() == httpbridge::TxFrameType_Body)
				{
					bodyStatus = UnpackBody(txframe, inframe);
					inframe.IsLast = !!(txframe->flags() & httpbridge::TxFrameFlags_Final);
				}
				else if (txframe->frametype() == httpbridge::TxFrameType_Abort)
				{
					inframe.IsAborted = true;
				}
				else
				{
					AnyLog()->Logf("Unrecognized frame type %d. Closing connection.", (int) txframe->frametype());
					Close();
					return RecvResult_Closed;
				}
				RecvBuf.EraseFromStart(4 + frameSize);
				if (headStatus != FrameOK || bodyStatus != FrameOK)
				{
					if (headStatus == FrameURITooLong)
					{
						SendResponse(txframe->channel(), txframe->stream(), Status414_URI_Too_Long);
					}
					else if (headStatus == FrameOutOfMemory || bodyStatus == FrameOutOfMemory)
					{
						SendResponse(txframe->channel(), txframe->stream(), Status503_Service_Unavailable);
					}
					inframe.DeleteRequestAndReset();
					return RecvResult_NoData;
				}
				return RecvResult_Data;
			}
		}

		return RecvResult_NoData;
	}

	Backend::FrameStatus Backend::UnpackHeader(const httpbridge::TxFrame* txframe, InFrame& inframe)
	{
		auto headers = txframe->headers();
		size_t headerBlockSize = TotalHeaderBlockSize(txframe);
		uint8_t* hblock = (uint8_t*) Alloc(headerBlockSize, Log, false);
		if (hblock == nullptr)
			return FrameOutOfMemory;

		auto lines = (Request::HeaderLine*) hblock;								// header lines
		int32_t hpos = (headers->size() + 1) * sizeof(Request::HeaderLine);		// offset into hblock

		for (uint32_t i = 0; i < headers->size(); i++)
		{
			const httpbridge::TxHeaderLine* line = headers->Get(i);
			int32_t keyLen, valueLen;
			const void *key, *value;
			if (line->id() != 0)
			{
				if (line->key()->size() != 0)
					HeaderCacheRecv->Insert(line->id(), line->key()->size(), line->key()->Data(), line->value()->size(), line->value()->Data());
				HeaderCacheRecv->Get(line->id(), keyLen, key, valueLen, value);
			}
			else
			{
				keyLen = line->key()->size();
				key = line->key()->Data();
				valueLen = line->value()->size();
				value = line->value()->Data();
			}

			memcpy(hblock + hpos, key, keyLen);
			hblock[hpos + keyLen] = 0;
			lines[i].KeyStart = hpos;
			lines[i].KeyLen = keyLen;
			hpos += keyLen + 1;
			memcpy(hblock + hpos, value, valueLen);
			hblock[hpos + valueLen] = 0;
			hpos += valueLen + 1;
		}
		// add terminal HeaderLine
		lines[headers->size()].KeyStart = hpos;
		lines[headers->size()].KeyLen = 0;

		inframe.Request = new hb::Request();
		inframe.Request->Initialize(this, TranslateVersion(txframe->version()), txframe->channel(), txframe->stream(), headers->size(), hblock);
		return FrameOK;
	}

	Backend::FrameStatus Backend::UnpackBody(const httpbridge::TxFrame* txframe, InFrame& inframe)
	{
		if (inframe.Request == nullptr)
		{
			GiantLock.lock();
			auto cr = CurrentRequests.find(MakeStreamKey(txframe->channel(), txframe->stream()));
			if (cr == CurrentRequests.end())
			{
				AnyLog()->Logf("Received body bytes for unknown stream [%llu:%llu]", txframe->channel(), txframe->stream());
				GiantLock.unlock();
				return FrameInvalid;
			}
			inframe.Request = cr->second.Request;
			GiantLock.unlock();
		}

		// Empty body frames are a waste, but not an error. Likely to be the final frame.
		if (txframe->body() == nullptr)
			return FrameOK;

		if (inframe.Request->IsBuffered)
		{
			Buffer& buf = inframe.Request->BodyBuffer;
			if (buf.Count + inframe.BodyBytesLen > inframe.Request->BodyLength)
			{
				AnyLog()->Logf("Request sent too many body bytes. Ignoring frame [%llu:%llu]", txframe->channel(), txframe->stream());
				return FrameInvalid;
			}
			// Assume that a buffer realloc is going to grow by buf.Capacity (ie 2x growth).
			if (buf.Count + txframe->body()->size() > buf.Capacity && BufferedRequestsTotalBytes + buf.Capacity > MaxWaitingBufferTotal)
			{
				// TODO: same as below - terminate the stream
			}
			auto oldBufCapacity = buf.Capacity;
			if (!buf.TryWrite(txframe->body()->Data(), txframe->body()->size()))
			{
				// TODO: figure out a way to terminate the stream right here, with a 503 response
				AnyLog()->Logf("Failed to allocate memory for body frame [%llu:%llu]", txframe->channel(), txframe->stream());
				return FrameOutOfMemory;
			}
			BufferedRequestsTotalBytes += buf.Capacity - oldBufCapacity;
			inframe.BodyBytesLen = txframe->body()->size();
			inframe.BodyBytes = buf.Data + buf.Count - txframe->body()->size();
			return FrameOK;
		}
		else
		{
			// non-buffered
			inframe.BodyBytesLen = txframe->body()->size();
			if (inframe.BodyBytesLen != 0)
			{
				inframe.BodyBytes = (uint8_t*) Alloc(inframe.BodyBytesLen, Log);
				memcpy(inframe.BodyBytes, txframe->body()->Data(), inframe.BodyBytesLen);
			}
			return FrameOK;
		}
	}

	size_t Backend::TotalHeaderBlockSize(const httpbridge::TxFrame* frame)
	{
		// the +1 is for the terminal HeaderLine
		size_t total = sizeof(Request::HeaderLine) * (frame->headers()->size() + 1);

		for (uint32_t i = 0; i < frame->headers()->size(); i++)
		{
			const httpbridge::TxHeaderLine* line = frame->headers()->Get(i);
			if (line->key()->size() != 0)
			{
				total += line->key()->size() + line->value()->size();
			}
			else
			{
				int keyLen = 0;
				int valLen = 0;
				const void *key = nullptr, *val = nullptr;
				HeaderCacheRecv->Get(line->id(), keyLen, key, valLen, val);
				total += keyLen + valLen;
			}
			// +2 for the null terminators
			total += 2;
		}
		return total;
	}

	void Backend::LogAndPanic(const char* msg)
	{
		AnyLog()->Log(msg);
		HTTPBRIDGE_PANIC(msg);
	}

	void Backend::SendResponse(Request& request, StatusCode status)
	{
		Response response(&request, status);
		Send(response);
	}

	void Backend::SendResponse(uint64_t channel, uint64_t stream, StatusCode status)
	{
		Request tmp;
		tmp.Backend = this;
		tmp.Channel = channel;
		tmp.Stream = stream;
		Response response(&tmp, status);
		Send(response);
	}

	Backend::RequestState* Backend::GetRequestOrDie(uint64_t channel, uint64_t stream)
	{
		auto iter = CurrentRequests.find(MakeStreamKey(channel, stream));
		HTTPBRIDGE_ASSERT(iter != CurrentRequests.end());
		return &iter->second;
	}

	StreamKey Backend::MakeStreamKey(uint64_t channel, uint64_t stream)
	{
		return StreamKey{ channel, stream };
	}

	StreamKey Backend::MakeStreamKey(const Request& request)
	{
		return StreamKey{ request.Channel, request.Stream };
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void UrlPathParser::MeasurePath(const char* buf, int& rawLen, int& decodedLen)
	{
		int i = 0;
		int _decodedLen = 0;
		for (; buf[i] != 0 && buf[i] != '?'; i++)
		{
			if (buf[i] == '%' && buf[i + 1] != 0 && buf[i + 2] != 0)
				i += 2;
			_decodedLen++;
		}
		rawLen = i;
		decodedLen = _decodedLen;
	}

	void UrlPathParser::DecodePath(const char* buf, char* decodedPath, bool addNullTerminator)
	{
		int j = 0;
		for (int i = 0; buf[i] != 0 && buf[i] != '?'; i++)
		{
			if (buf[i] == '%' && buf[i + 1] != 0 && buf[i + 2] != 0)
			{
				decodedPath[j++] = (HexToInt(buf[i + 1]) << 4) + HexToInt(buf[i + 2]);
				i += 2;
			}
			else
				decodedPath[j++] = buf[i];
		}
		if (addNullTerminator)
			decodedPath[j++] = 0;
	}


	/* Parse a=b&c=d

	The following truth table is not really what is output from the parser, but it's the final result
	of what we do with the parser's output. For example, if a key is empty but the value is not,
	then the parser will simply tell you those facts. It's your choice to discard pairs with empty keys.

	a		->	[a,]
	&a		->	[a,]			Pairs with empty keys are discarded
	=x&a	->	[a,]
	a&b		->	[a,] [b,]
	a=&b	->	[a,] [b,]
	a=1&b	->	[a,1] [b,]
	a==1&b	->	[a,=1] [b,]
	a=1=	->	[a,1=]

	The rule, basically, is that no matter what you're parsing, if you find an ampersand, then it's a new key/value pair.
	If you're parsing a key, then an equal sign ends the key and switches to the value.
	*/
	bool UrlQueryParser::Next(int& key, int& keyDecodedLen, int& val, int& valDecodedLen)
	{
		if (Src[P] == 0)
			return false;
			
		// Keep this in sync with UrlDecodePiece()
		key = P;
		int ndecoded = 0;
		for (; Src[P] != 0 && Src[P] != '=' && Src[P] != '&'; P++)
		{
			if (Src[P] == '%' && Src[P + 1] != 0 && Src[P + 2] != 0)
			{
				P += 2;
				ndecoded++;
			}
			else
				ndecoded++;
		}
		keyDecodedLen = ndecoded;
		if (Src[P] == 0 || Src[P] == '&')
		{
			if (Src[P] == '&')
				P++;
			val = -1;
			valDecodedLen = 0;
			return true;
		}

		// Keep this in sync with UrlDecodePiece()
		P++;
		val = P;
		ndecoded = 0;
		for (; Src[P] != 0 && Src[P] != '&'; P++)
		{
			if (Src[P] == '%' && Src[P + 1] != 0 && Src[P + 2] != 0)
			{
				P += 2;
				ndecoded++;
			}
			else
				ndecoded++;
		}
		valDecodedLen = ndecoded;
		if (Src[P] == '&')
			P++;
		return true;
	}

	void UrlQueryParser::DecodeKey(int start, char* key, bool addNullTerminator)
	{
		DecodeKey(Src, start, key, addNullTerminator);
	}

	void UrlQueryParser::DecodeVal(int start, char* val, bool addNullTerminator)
	{
		DecodeVal(Src, start, val, addNullTerminator);
	}

	template<bool IsVal>
	void UrlDecodePiece(const char* buf, char* decoded, bool addNullTerminator)
	{
		// Keep this in sync with Next()
		int p = 0;
		int j = 0;
		for (; buf[p] != 0 && buf[p] != '&' && (IsVal || (buf[p] != '=')); p++)
		{
			if (buf[p] == '%' && buf[p + 1] != 0 && buf[p + 2] != 0)
			{
				decoded[j++] = (HexToInt(buf[p + 1]) << 4) + HexToInt(buf[p + 2]);
				p += 2;
			}
			else
				decoded[j++] = buf[p];
		}
		if (addNullTerminator)
			decoded[j++] = 0;
	}
	void UrlQueryParser::DecodeKey(const char* buf, int start, char* key, bool addNullTerminator)
	{
		UrlDecodePiece<false>(buf + start, key, addNullTerminator);
	}

	void UrlQueryParser::DecodeVal(const char* buf, int start, char* val, bool addNullTerminator)
	{
		UrlDecodePiece<true>(buf + start, val, addNullTerminator);
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	Request::Request()
	{
		_Liveness = 2;
	}

	Request::~Request()
	{
		Free();
	}

	void Request::Initialize(hb::Backend* backend, HttpVersion version, uint64_t channel, uint64_t stream, int32_t headerCount, const void* headerBlock)
	{
		Backend = backend;
		Version = version;
		Channel = channel;
		Stream = stream;
		_HeaderCount = headerCount;
		_HeaderBlock = (const uint8_t*) headerBlock;
		const char* contentLength = HeaderByName(Header_Content_Length);
		if (contentLength != nullptr)
			BodyLength = (uint64_t) uatoi64(contentLength);
	}

	bool Request::DecrementLiveness()
	{
		_Liveness--;
		if (_Liveness == 0)
		{
			if (Backend != nullptr)
				Backend->RequestDestroyed(StreamKey{ Channel, Stream });
			delete this;
			return true;
		}
		return false;
	}

	void Request::Reset()
	{
		Free();
		// Use memcpy to reset ourselves, because atomic<int> _Liveness prevents the compiler
		// from generating Reset::operator=
		Request tmp;
		memcpy(this, &tmp, sizeof(tmp));
	}

	ConstString Request::Method() const
	{
		// actually the 'key' of header[0]
		auto lines = (const HeaderLine*) _HeaderBlock;
		return (const char*) (_HeaderBlock + lines[0].KeyStart);
	}

	ConstString Request::URI() const
	{
		// actually the 'value' of header[0]
		auto lines = (const HeaderLine*) _HeaderBlock;
		return (const char*) (_HeaderBlock + lines[0].KeyStart + lines[0].KeyLen + 1);
	}

	ConstString Request::Path() const
	{
		return _CachedURI + 2;
	}

	ConstString Request::Query(const char* key) const
	{
		const char* p = _CachedURI;
		uint16_t len = *((uint16_t*) p);
		p += 2 + RoundUpOdd(len) + 1; // skip path
		for (len = *((uint16_t*) p); len != EndOfQueryMarker; )
		{
			p += 2;
			if (strcmp(key, p) == 0)
				return p + RoundUpOdd(len) + 1 + 2;
			
			// key
			p += RoundUpOdd(len) + 1;
			len = *((uint16_t*) p);

			// value
			p += 2 + RoundUpOdd(len) + 1;
			len = *((uint16_t*) p);
		}
		return nullptr;
	}

	std::string Request::QueryStr(const char* key) const
	{
		const char* v = Query(key);
		return v == nullptr ? "" : v;
	}

	int32_t Request::NextQuery(int32_t iterator, const char*& key, const char*& value) const
	{
		const char* p = _CachedURI + iterator;
		uint16_t len = *((uint16_t*) p);
		if (iterator == 0)
		{
			// skip path
			p += 2 + RoundUpOdd(len) + 1;
			len = *((uint16_t*) p);
		}
		if (len == EndOfQueryMarker)
		{
			key = nullptr;
			value = nullptr;
			return 0;
		}

		key = p + 2;
		p += 2 + RoundUpOdd(len) + 1;
		len = *((uint16_t*) p);
		value = p + 2;
		p += 2 + RoundUpOdd(len) + 1;

		return (int32_t) (p - _CachedURI);
	}

	bool Request::HeaderByName(const char* name, size_t& len, const void*& buf, int nth) const
	{
		auto nameLen = strlen(name);
		auto count = HeaderCount() + NumPseudoHeaderLines;
		auto lines = (const HeaderLine*) _HeaderBlock;
		for (int32_t i = NumPseudoHeaderLines; i < count; i++)
		{
			const uint8_t* bKey = _HeaderBlock + lines[i].KeyStart;
			if (memcmp(bKey, name, nameLen) == 0)
				nth--;

			if (nth == -1)
			{
				buf = _HeaderBlock + (lines[i].KeyStart + lines[i].KeyLen + 1);
				len = lines[i + 1].KeyStart - 1 - (lines[i].KeyStart + lines[i].KeyLen + 1);
				return true;
			}
		}
		return false;
	}

	const char* Request::HeaderByName(const char* name, int nth) const
	{
		size_t len;
		const void* buf;
		if (HeaderByName(name, len, buf, nth))
			return (const char*) buf;
		else
			return nullptr;
	}

	void Request::HeaderAt(int32_t index, int32_t& keyLen, const char*& key, int32_t& valLen, const char*& val) const
	{
		if ((uint32_t) index >= (uint32_t) HeaderCount())
		{
			keyLen = 0;
			valLen = 0;
			key = nullptr;
			val = nullptr;
		}
		else
		{
			index += NumPseudoHeaderLines;
			auto lines = (const HeaderLine*) _HeaderBlock;
			keyLen = lines[index].KeyLen;
			key = (const char*) (_HeaderBlock + lines[index].KeyStart);
			valLen = lines[index + 1].KeyStart - (lines[index].KeyStart + lines[index].KeyLen);
			val = (const char*) (_HeaderBlock + lines[index].KeyStart + lines[index].KeyLen + 1);
		}
	}

	void Request::HeaderAt(int32_t index, const char*& key, const char*& val) const
	{
		int32_t keyLen, valLen;
		HeaderAt(index, keyLen, key, valLen, val);
	}

	void Request::Free()
	{
		hb::Free(_CachedURI);
		hb::Free((void*) _HeaderBlock);
	}

	// Return false if the decoded length of either the path or any query item exceeds 65534 characters
	bool Request::ParseURI()
	{
		// Decode the URI into Path and Query key=value pairs. We perform percent-decoding here.
		// Our final output buffer looks like this:
		// len,path, len,key, len,val, len,key, len,val...
		// len = unsigned 16-bit length, excluding null terminator. so maximum decoded key/value length is 65534, and same limit applies to the decoded path too.
		// Why 65534? Because we use 65535 as an end-of-list marker
		// Also, we make sure to keep the 16-bit length counters aligned.
		// We store the exact lengths in the 16-bit numbers, so we always need to perform the rounding when scanning through the list.
		const int MAXLEN = 65534;
		static_assert(MAXLEN < EndOfQueryMarker, "MAXLEN must be less than EndOfQueryMarker");

		// Measure the size of the buffer that we'll need
		const char* uri = URI();

		// measure path length
		int pathRawLen, pathDecodedLen;
		UrlPathParser::MeasurePath(uri, pathRawLen, pathDecodedLen);
		if (pathDecodedLen > MAXLEN)
			return false;

		const bool hasQuery = uri[pathRawLen] != 0;

		// measure query key=value pairs
		int queryTotalBytes = 0;
		if (hasQuery)
		{
			UrlQueryParser p(uri + pathRawLen + 1); // +1 to skip the "?"
			int key, keyLen, val, valLen;
			while (p.Next(key, keyLen, val, valLen))
			{
				if (keyLen > MAXLEN || valLen > MAXLEN)
					return false;
				queryTotalBytes += 2 + RoundUpOdd(keyLen) + 1 + 2 + RoundUpOdd(valLen) + 1; // +2 for lengths, +1 for null terminators. Round to keep lengths 16-bit aligned.
			}
		}

		// Alloc buffer and decode path
		int bufLen = 2 + RoundUpOdd(pathDecodedLen) + 1 + queryTotalBytes + 2;
		_CachedURI = (char*) hb::Alloc(bufLen, Backend ? Backend->AnyLog() : nullptr); // 2+ for the length of the path, 1+ for the null terminator
		char* out = _CachedURI;
		*((uint16_t*) out) = pathDecodedLen;
		out += 2;
		UrlPathParser::DecodePath(uri, out, true);
		out += RoundUpOdd(pathDecodedLen) + 1;

		// decode query key=value pairs
		if (hasQuery)
		{
			UrlQueryParser p(uri + pathRawLen + 1);
			int key, keyLen, val, valLen;
			while (p.Next(key, keyLen, val, valLen))
			{
				*((uint16_t*) out) = keyLen;
				out += 2;
				p.DecodeKey(key, out, true);
				out += RoundUpOdd(keyLen) + 1;

				*((uint16_t*) out) = valLen;
				out += 2;
				p.DecodeVal(val, out, true);
				out += RoundUpOdd(valLen) + 1;
			}
		}

		*((uint16_t*) out) = EndOfQueryMarker;
		out += 2;
		HTTPBRIDGE_ASSERT(out == _CachedURI + bufLen);
		return true;
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	static_assert(sizeof(Response::ByteVectorOffset) == sizeof(flatbuffers::Offset<flatbuffers::Vector<uint8_t>>), "Assumed flatbuffers offset size is wrong");

	Response::Response()
	{
	}

	Response::Response(const Request* request, StatusCode status)
	{
		Status = status;
		Backend = request->Backend;
		Version = request->Version;
		Stream = request->Stream;
		Channel = request->Channel;
	}

	Response::~Response()
	{
		if (FBB != nullptr)
			delete FBB;
	}

	void Response::SetStatusAndBody(StatusCode status, const char* body)
	{
		SetStatus(status);
		SetBody(body, strlen(body));
	}

	void Response::AddHeader(const char* key, const char* value)
	{
		size_t keyLen = strlen(key);
		size_t valLen = strlen(value);
		HTTPBRIDGE_ASSERT(keyLen <= MaxHeaderKeyLen);
		HTTPBRIDGE_ASSERT(valLen <= MaxHeaderValueLen);
		AddHeader((int) keyLen, key, (int) valLen, value);
	}

	void Response::AddHeader(int32_t keyLen, const char* key, int32_t valLen, const char* value)
	{
		// Ensure that our uint32_t casts down below are safe, and also header sanity
		HTTPBRIDGE_ASSERT(keyLen <= MaxHeaderKeyLen && valLen <= MaxHeaderValueLen);
		
		// Keys may not be empty, but values are allowed to be empty
		HTTPBRIDGE_ASSERT(keyLen > 0 && valLen >= 0);

		HeaderIndex.Push(HeaderBuf.Size());
		memcpy(HeaderBuf.AddSpace((uint32_t) keyLen), key, keyLen);
		HeaderBuf.Push(0);

		HeaderIndex.Push(HeaderBuf.Size());
		memcpy(HeaderBuf.AddSpace((uint32_t) valLen), value, valLen);
		HeaderBuf.Push(0);
	}

	void Response::AddHeader_ContentLength(uint64_t contentLength)
	{
		// You must first call WriteHeader_ContentLength(), and then call SetBodyPart()
		HTTPBRIDGE_ASSERT(Status != StatusMeta_BodyPart);

		char buf[21];
		u64toa(contentLength, buf);
		AddHeader(Header_Content_Length, buf);
	}

	void Response::SetBody(const void* body, size_t len)
	{
		// Ensure sanity, as well as safety because BodyLength is uint32
		HTTPBRIDGE_ASSERT(len <= 1024 * 1024 * 1024);

		// Although it's theoretically possible to allow SetBody to be called multiple times
		// (ie discard FBB every time) it is so wasteful that we rather force the user to
		// construct their code in such a manner that this is not necessary.
		HTTPBRIDGE_ASSERT(BodyOffset == 0 && BodyLength == 0);

		CreateBuilder();
		BodyOffset = (ByteVectorOffset) FBB->CreateVector((const uint8_t*) body, len).o;
		BodyLength = (uint32_t) len;
	}

	void Response::SetBodyPart(const void* part, size_t len)
	{
		SetBody(part, len);
		if (HeaderCount() == 0)
			Status = StatusMeta_BodyPart;
	}

	SendResult Response::Send()
	{
		return Backend->Send(*this);
	}

	const char* Response::HeaderByName(const char* name, int nth) const
	{
		for (int32_t index = 0; index < HeaderCount(); index++)
		{
			int i = index << 1;
			const char* key = &HeaderBuf[HeaderIndex[i]];
			const char* val = &HeaderBuf[HeaderIndex[i + 1]];
			if (strcmp(key, name) == 0)
				nth--;
			if (nth == -1)
				return val;
		}
		return nullptr;
	}

	bool Response::HasHeader(const char* name) const
	{
		return HeaderByName(name) != nullptr;
	}

	void Response::HeaderAt(int32_t index, int32_t& keyLen, const char*& key, int32_t& valLen, const char*& val) const
	{
		if (index > HeaderCount())
		{
			keyLen = 0;
			valLen = 0;
			key = nullptr;
			val = nullptr;
			return;
		}
		keyLen = HeaderKeyLen(index);
		valLen = HeaderValueLen(index);
		int i = index << 1;
		key = &HeaderBuf[HeaderIndex[i]];
		val = &HeaderBuf[HeaderIndex[i + 1]];
	}

	void Response::HeaderAt(int32_t index, const char*& key, const char*& val) const
	{
		int32_t keyLen, valLen;
		HeaderAt(index, keyLen, key, valLen, val);
	}

	void Response::FinishFlatbuffer(void*& buf, size_t& len, bool isLast)
	{
		HTTPBRIDGE_ASSERT(!IsFlatBufferBuilt);

		CreateBuilder();

		std::vector<flatbuffers::Offset<httpbridge::TxHeaderLine>> lines;
		{
			char statusStr[4];
			u32toa((uint32_t) Status, statusStr);
			auto key = FBB->CreateVector((const uint8_t*) statusStr, 3);
			auto line = httpbridge::CreateTxHeaderLine(*FBB, key, 0, 0);
			lines.push_back(line);
		}

		HeaderIndex.Push(HeaderBuf.Size()); // add a terminator

		for (int32_t i = 0; i < HeaderIndex.Size() - 2; i += 2)
		{
			// The extra 1's here are for removing the null terminator that we add to keys and values
			uint32_t keyLen = HeaderIndex[i + 1] - HeaderIndex[i] - 1;
			uint32_t valLen = HeaderIndex[i + 2] - HeaderIndex[i + 1] - 1;
			auto key = FBB->CreateVector((const uint8_t*) &HeaderBuf[HeaderIndex[i]], keyLen);
			auto val = FBB->CreateVector((const uint8_t*) &HeaderBuf[HeaderIndex[i + 1]], valLen);
			auto line = httpbridge::CreateTxHeaderLine(*FBB, key, val, 0);
			lines.push_back(line);
		}
		auto linesVector = FBB->CreateVector(lines);

		uint8_t flags = 0;
		if (isLast)
			flags |= httpbridge::TxFrameFlags_Final;

		httpbridge::TxFrameBuilder frame(*FBB);
		frame.add_frametype(httpbridge::TxFrameType_Header);
		frame.add_version(TranslateVersion(Version));
		frame.add_flags(flags);
		frame.add_channel(Channel);
		frame.add_stream(Stream);
		frame.add_headers(linesVector);
		frame.add_body(BodyOffset);
		auto root = frame.Finish();
		httpbridge::FinishTxFrameBuffer(*FBB, root);
		len = FBB->GetSize();
		buf = FBB->GetBufferPointer();
		
		// Hack the FBB to write our frame size at the start of the buffer.
		// This is not a 'hack' in the sense that it's bad or needs to be removed at some point.
		// It's simply not the intended use of FBB.
		// Right here 'len' contains the size of the flatbuffer, which is our "frame size"
		uint8_t frame_size[4];
		Write32LE(frame_size, (uint32_t) len);
		FBB->PushBytes(frame_size, 4);
		len = FBB->GetSize();
		buf = FBB->GetBufferPointer();
		// Now 'len' contains the size of the flatbuffer + 4

		IsFlatBufferBuilt = true;
	}

	void Response::SerializeToHttp(void*& buf, size_t& len)
	{
		// This doesn't make any sense. Additionally, we need the guarantee that our
		// Body is always FBB->GetBufferPointer() + 4, because we're reaching into the
		// FBB internals here.
		HTTPBRIDGE_ASSERT(!IsFlatBufferBuilt);

		if (HeaderByName(Header_Content_Length) == nullptr)
		{
			char s[11]; // 10 characters needed for 4294967295
			u32toa(BodyLength, s);
			AddHeader(Header_Content_Length, s);
		}

		// Compute response size
		const size_t sCRLF = 2;
		const int32_t headerCount = HeaderCount();
		
		// HTTP/1.1 200		12 bytes
		len = 12 + 1 + strlen(StatusString(Status)) + sCRLF;
		
		// Headers
		// 2		The ": " in between the key and value of each header
		// CRLF		The newline after each header
		// -2		The null terminators that we add to each key and each value (and counted up inside HeaderBuf.size)
		len += headerCount * (2 + sCRLF - 2) + HeaderBuf.Size();
		len += sCRLF;
		len += BodyLength;

		// Write response
		buf = Alloc(len, Backend ? Backend->AnyLog() : nullptr, true);
		HTTPBRIDGE_ASSERT(buf != nullptr);
		uint8_t* out = (uint8_t*) buf;
		const char* CRLF = "\r\n";

		switch (Version)
		{
		case HttpVersion10: BufWrite(out, "HTTP/1.0 ", 9); break;
		case HttpVersion11: BufWrite(out, "HTTP/1.1 ", 9); break;
		default: HTTPBRIDGE_PANIC("Only HTTP 1 supported for SerializeToHttp");
		}

		char statusNStr[4];
		u32toa((uint32_t) Status, statusNStr);
		BufWrite(out, statusNStr, 3);
		BufWrite(out, " ", 1);
		BufWrite(out, StatusString(Status), strlen(StatusString(Status)));
		BufWrite(out, CRLF, 2);

		for (int32_t i = 0; i < headerCount * 2; i += 2)
		{
			// The extra 1's here are for removing the null terminator that we add to keys and values
			uint32_t valTop = i + 2 == HeaderIndex.Size() ? HeaderBuf.Size() : HeaderIndex[i + 2];
			uint32_t keyLen = HeaderIndex[i + 1] - HeaderIndex[i] - 1;
			uint32_t valLen = valTop - HeaderIndex[i + 1] - 1;
			BufWrite(out, &HeaderBuf[HeaderIndex[i]], keyLen);
			BufWrite(out, ": ", 2);
			BufWrite(out, &HeaderBuf[HeaderIndex[i + 1]], valLen);
			BufWrite(out, CRLF, 2);
		}
		BufWrite(out, CRLF, 2);

		// Write body
		uint8_t* flatbuf = FBB->GetBufferPointer();
		BufWrite(out, flatbuf + 4, BodyLength);
	}

	void Response::GetBody(const void*& buf, size_t& len) const
	{
		len = BodyLength;
		if (BodyLength == 0)
		{
			buf = nullptr;
		}
		else
		{
			buf = FBB->GetBufferPointer() + BodyOffset;
		}
	}

	void Response::CreateBuilder()
	{
		if (FBB == nullptr)
			FBB = new flatbuffers::FlatBufferBuilder();
	}

	int32_t Response::HeaderKeyLen(int32_t _i) const
	{
		// key_0_start, val_0_start, key_1_start, val_1_start, ...
		// (with null terminators after each key and each value)
		uint32_t i = _i;
		i <<= 1;
		if (i + 1 >= (uint32_t) HeaderIndex.Size())
			return 0;
		return HeaderIndex[i + 1] - HeaderIndex[i] - 1; // extra -1 is to remove null terminator
	}

	int32_t Response::HeaderValueLen(int32_t _i) const
	{
		uint32_t i = _i;
		i <<= 1;
		uint32_t top;
		if (i + 3 >= (uint32_t) HeaderIndex.Size())
			return 0;

		if (i + 2 == HeaderIndex.Size())
			top = HeaderBuf.Size();
		else
			top = HeaderIndex[i + 2];

		return top - HeaderIndex[i + 1] - 1; // extra -1 is to remove null terminator
	}

}
