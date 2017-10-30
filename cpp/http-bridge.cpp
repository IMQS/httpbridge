#include "http-bridge.h"
#include "http-bridge_generated.h"
#include <string>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

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
		default:                                        return "Unrecognized HTTP Status Code";
		}
	}


	// siphash implementation:
	// <MIT License>
	// Copyright (c) 2013  Marek Majkowski <marek@popcount.org>
	// https://github.com/majek/csiphash/

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
	__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define _le64toh(x) ((uint64_t)(x))
#elif defined(_WIN32)
	/* Windows is always little endian, unless you're on xbox360
	http://msdn.microsoft.com/en-us/library/b0084kay(v=vs.80).aspx */
#  define _le64toh(x) ((uint64_t)(x))
#elif defined(__APPLE__)
#  include <libkern/OSByteOrder.h>
#  define _le64toh(x) OSSwapLittleToHostInt64(x)
#else

	/* See: http://sourceforge.net/p/predef/wiki/Endianness/ */
#  if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#    include <sys/endian.h>
#  else
#    include <endian.h>
#  endif
#  if defined(__BYTE_ORDER) && defined(__LITTLE_ENDIAN) && \
	__BYTE_ORDER == __LITTLE_ENDIAN
#    define _le64toh(x) ((uint64_t)(x))
#  else
#    define _le64toh(x) le64toh(x)
#  endif

#endif


#define ROTATE(x, b) (uint64_t)( ((x) << (b)) | ( (x) >> (64 - (b))) )

#define HALF_ROUND(a,b,c,d,s,t)			\
	a += b; c += d;				\
	b = ROTATE(b, s) ^ a;			\
	d = ROTATE(d, t) ^ c;			\
	a = ROTATE(a, 32);

#define DOUBLE_ROUND(v0,v1,v2,v3)		\
	HALF_ROUND(v0,v1,v2,v3,13,16);		\
	HALF_ROUND(v2,v1,v0,v3,17,21);		\
	HALF_ROUND(v0,v1,v2,v3,13,16);		\
	HALF_ROUND(v2,v1,v0,v3,17,21);

	HTTPBRIDGE_API uint64_t siphash24(const void *src, unsigned long src_sz, const char key[16]) {
		const uint64_t *_key = (uint64_t *) key;
		uint64_t k0 = _le64toh(_key[0]);
		uint64_t k1 = _le64toh(_key[1]);
		uint64_t b = (uint64_t) src_sz << 56;
		const uint64_t *in = (uint64_t*) src;

		uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
		uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
		uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
		uint64_t v3 = k1 ^ 0x7465646279746573ULL;

		while (src_sz >= 8) {
			uint64_t mi = _le64toh(*in);
			in += 1; src_sz -= 8;
			v3 ^= mi;
			DOUBLE_ROUND(v0, v1, v2, v3);
			v0 ^= mi;
		}

		uint64_t t = 0; uint8_t *pt = (uint8_t *) &t; uint8_t *m = (uint8_t *) in;
		switch (src_sz) {
		case 7: pt[6] = m[6];
		case 6: pt[5] = m[5];
		case 5: pt[4] = m[4];
		case 4: *((uint32_t*) &pt[0]) = *((uint32_t*) &m[0]); break;
		case 3: pt[2] = m[2];
		case 2: pt[1] = m[1];
		case 1: pt[0] = m[0];
		}
		b |= _le64toh(t);

		v3 ^= b;
		DOUBLE_ROUND(v0, v1, v2, v3);
		v0 ^= b; v2 ^= 0xff;
		DOUBLE_ROUND(v0, v1, v2, v3);
		DOUBLE_ROUND(v0, v1, v2, v3);
		return (v0 ^ v1) ^ (v2 ^ v3);
	}

	size_t Hash16B(uint64_t pair[2])
	{
		// Initially, I wanted to use the final mixing step of xxhash64 here. That is pretty fast (4 multiplies, 6 shifts, and 6 xors).
		// However, it eventually occurred to me that the stream number is controllable by the client, and that's what we're ostensibly
		// hashing here (the channel and the stream). I don't know enough about this stuff to be able to figure out whether the final
		// mixing step of xxhash64 is susceptible to the kind of attacks that siphash was built for, but in the absence of that
		// knowledge, I'm doing the conservative thing.
		const uint64_t key[2] = { 0, 0 };
		return (size_t) siphash24(pair, sizeof(pair[0]) * 2, (const char*) key);
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

	int64_t atoi64(const char* s)
	{
		int64_t v = 0;
		size_t i = s[0] == '-' ? 1 : 0;
		for (; s[i]; i++)
			v = v * 10 + (s[i] - '0');
		if (s[0] == '-')
			v = -v;
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

	ICompressor::~ICompressor()
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
		Request = nullptr;
		BodyBytes = nullptr;
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
			// This is often the point at which the last reference to Request* is lost, so 
			// Request is likely to be destroyed by this next line.
			Request = nullptr;
		}

		if (BodyBytes)
		{
			HTTPBRIDGE_ASSERT(BodyBytesLen != 0);
			Free(BodyBytes);
		}

		Type = FrameType::Data;
		IsHeader = false;
		IsLast = false;

		BodyBytes = nullptr;
		BodyBytesLen = 0;
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
		CurrentRequestLock.lock();
		CurrentRequests.clear();
		CurrentRequestLock.unlock();

		if (BufferedRequestsTotalBytes.load() != 0)
			AnyLog()->Logf("BufferedRequestsTotalBytes is %llu, instead of zero", (uint64_t) BufferedRequestsTotalBytes.load());

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
		CurrentRequestLock.lock();
		RequestState* rs = GetRequest(response.Channel, response.Stream);
		if (!rs)
		{
			// The stream has been closed. A typical thing that causes this is an aborted stream.
			CurrentRequestLock.unlock();
			return SendResult_Closed;
		}
		bool isResponseHeader = response.Status != StatusMeta_BodyPart;

		if (!isResponseHeader)
			HTTPBRIDGE_ASSERT(response.HeaderCount() == 0);

		// The first response must contain a response header
		HTTPBRIDGE_ASSERT(isResponseHeader == !rs->IsResponseHeaderSent);

		if (isResponseHeader)
		{
			const char* contentLen = response.HeaderByName("Content-Length");
			if (contentLen != nullptr)
			{
				// If you set Content-Length, then that is the expected length of the response, unless it's -1, in which case it's chunked.
				rs->ResponseBodyRemaining = (uint64_t) atoi64(contentLen);
			}
			else
			{
				// If you don't set Content-Length, then whatever bytes are inside your response, is the expected length of the response.
				rs->ResponseBodyRemaining = response.BodyBytes();
				if (response.BodyBytes() != 0)
					response.AddHeader_ContentLength(response.BodyBytes());
			}
			rs->IsResponseHeaderSent = true;
		}
		HTTPBRIDGE_ASSERT(rs->ResponseBodyRemaining >= response.BodyBytes()); // You have sent more data than Content-Length
		rs->ResponseBodyRemaining -= response.BodyBytes();
		CurrentRequestLock.unlock();

		bool isLast = rs->ResponseBodyRemaining == 0 || response.IsFinalChunkedFrame;
		if (isLast)
		{
			RequestFinished(MakeStreamKey(rs->Request));
			rs = nullptr;
		}

		size_t offset = 0;
		size_t len = 0;
		void* buf = nullptr;
		response.FinishFlatbuffer(buf, len, isLast);
		TransportLock.lock();
		while (offset != len)
		{
			size_t sent = 0;
			auto res = Transport->Send((uint8_t*) buf + offset, len - offset, sent);
			offset += sent;
			if (res == SendResult_Closed)
			{
				TransportLock.unlock();
				return res;
			}
		}
		TransportLock.unlock();
		return SendResult_All;
	}

	SendResult Backend::Send(ConstRequestPtr request, StatusCode status)
	{
		Response response(request, status);
		return Send(response);
	}

	SendResult Backend::SendBodyPart(ConstRequestPtr request, const void* body, size_t len, bool isFinal)
	{
		Response response(request, Status200_OK);
		response.SetBody(body, len);
		response.Status = StatusMeta_BodyPart;
		response.IsFinalChunkedFrame = isFinal;
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

		InternalRecvResponse res = RecvInternal(frame);
		if (res.Result == InternalRecvResult::BadFrame)
		{
			// Something went wrong inside httpbridge, such as out of memory, or URI too long.
			// Send a response to the server immediately, and do not inform the httpbridge user.
			StreamKey streamKey = MakeStreamKey(frame.Request);
			CurrentRequestLock.lock();
			CurrentRequests[streamKey] = {frame.Request, ResponseBodyUninitialized, false};
			CurrentRequestLock.unlock();
			SendResponse(streamKey.Channel, streamKey.Stream, res.Status);
			return false;
		}

		if (res.Result != InternalRecvResult::Data)
			return false;
		
		StreamKey streamKey = MakeStreamKey(frame.Request);

		if (frame.IsHeader)
		{
			CurrentRequestLock.lock();
			CurrentRequests[streamKey] = {frame.Request, ResponseBodyUninitialized, false};
			CurrentRequestLock.unlock();

			if (frame.IsLast && frame.Request->ContentLength != -1 && frame.Request->ContentLength != 0)
			{
				frame.Request->IsBuffered = true;
				frame.Request->BodyBuffer.Data = frame.BodyBytes;
				frame.Request->BodyBuffer.Capacity = frame.BodyBytesLen;
				frame.Request->BodyBuffer.Count = frame.BodyBytesLen;
				BufferedRequestsTotalBytes += frame.BodyBytesLen;
				return true;
			}

			if (frame.Request->ContentLength != -1 && frame.Request->ContentLength <= MaxAutoBufferSize && !frame.IsLast)
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
		RequestPtr request = frame.Request;

		if (!frame.IsHeader || frame.IsLast || request->ContentLength == 0 || request->ContentLength == -1)
			LogAndPanic("ResendWhenBodyIsDone may only be called on the first frame of a request (the header frame), with a positive body length");

		// Although it is tempting to allocate all the buffer memory up-front, to reduce memory getting
		// copied around upon buffer resizing, it's probably not worth the memory cost.
		size_t initialSize = min(InitialBufferSize.load(), (size_t) request->ContentLength);
		initialSize = max(initialSize, frame.BodyBytesLen);
		initialSize = max(initialSize, (size_t) 16);

		if (initialSize + (uint64_t) BufferedRequestsTotalBytes > (uint64_t) MaxWaitingBufferTotal)
		{
			AnyLog()->Log("MaxWaitingBufferTotal exceeded during ResendWhenBodyIsDone");
			SendResponse(request, Status503_Service_Unavailable);
			return false;
		}

		request->BodyBuffer.Data = (uint8_t*) Alloc(initialSize, AnyLog(), false);
		if (request->BodyBuffer.Data == nullptr)
		{
			AnyLog()->Log("Alloc for request buffer failed");
			SendResponse(request, Status503_Service_Unavailable);
			return false;
		}

		memcpy(request->BodyBuffer.Data, frame.BodyBytes, frame.BodyBytesLen);
		request->BodyBuffer.Count = frame.BodyBytesLen;
		request->BodyBuffer.Capacity = initialSize;
		Free(frame.BodyBytes);
		frame.BodyBytes = nullptr;
		frame.BodyBytesLen = 0;

		BufferedRequestsTotalBytes += (size_t) request->BodyBuffer.Capacity;

		request->IsBuffered = true;
		return true;
	}

	void Backend::RequestFinished(const StreamKey& key)
	{
		CurrentRequestLock.lock();
		auto cr = CurrentRequests.find(key);
		HTTPBRIDGE_ASSERT(cr != CurrentRequests.end());
		
		// Initially, I would call UnregisterBufferedBytes here, but that introduces a race condition
		// inside ResendWhenBodyIsDone, if the frame is aborted during the execution of ResendWhenBodyIsDone.
		// So instead, Request's destructor now calls UnregisterBufferedBytes.

		cr->second.Request = nullptr; // ensure that smart_ptr reference is decremented now
		CurrentRequests.erase(key);
		CurrentRequestLock.unlock();
	}

	void Backend::UnregisterBufferedBytes(size_t bytes)
	{
		if (BufferedRequestsTotalBytes < bytes)
			LogAndPanic("BufferedRequestsTotalBytes underflow");
		BufferedRequestsTotalBytes -= bytes;
	}

	static bool IsControlFrame(httpbridge::TxFrameType type)
	{
		return type == httpbridge::TxFrameType_Abort || type == httpbridge::TxFrameType_Pause || type == httpbridge::TxFrameType_Resume;
	}

	static FrameType FBTypeToFrameType(httpbridge::TxFrameType type)
	{
		switch (type)
		{
		case httpbridge::TxFrameType_Header:
		case httpbridge::TxFrameType_Body: return FrameType::Data;
		case httpbridge::TxFrameType_Abort: return FrameType::Abort;
		case httpbridge::TxFrameType_Pause: return FrameType::Pause;
		case httpbridge::TxFrameType_Resume: return FrameType::Resume;
		default:
			HTTPBRIDGE_PANIC("Unrecognized FB frame type");
		}
	}

	// TODO: Experiment with reading exactly one frame, and the next frame's first 8 bytes, at a time.
	// First read 8 bytes for the magic marker and frame length, and thereafter read only
	// exactly as much as is necessary for that single frame, and the following frame's length.
	// Then we don't need to do a memmove, and things become simpler.
	// See if that really produces faster results for average workloads.
	Backend::InternalRecvResponse Backend::RecvInternal(InFrame& inframe)
	{
		if (Transport == nullptr)
			return {InternalRecvResult::Closed, Status000_NULL};

		const size_t maxRecv = 65536;
		const size_t maxBufSize = 1024*1024;
		const size_t maxFrameSize = 100*1024*1024;
		RecvBuf.Preallocate(maxRecv);

		if (RecvBuf.Count >= maxBufSize)
		{
			AnyLog()->Logf("Server is trying to send us a frame larger than %d bytes. Closing connection.", (int) maxBufSize);
			Close();
			return {InternalRecvResult::Closed, Status000_NULL};
		}

		// If we don't have at least one frame ready, then read more
		if (RecvBuf.Count < 8 || RecvBuf.Count < 8 + Read32LE(RecvBuf.Data + 4))
		{
			size_t read = 0;
			auto result = Transport->Recv(maxRecv, RecvBuf.Data + RecvBuf.Count, read);
			if (result == RecvResult_Closed)
			{
				AnyLog()->Logf("Server closed connection");
				Close();
				return {(InternalRecvResult) result, Status000_NULL};
			}
			RecvBuf.Count += read;
		}

		// Process frame
		if (RecvBuf.Count >= 8)
		{
			uint32_t magic = Read32LE(RecvBuf.Data);
			uint32_t frameSize = Read32LE(RecvBuf.Data + 4);
			if (magic != MagicFrameMarker || frameSize > maxFrameSize)
			{
				AnyLog()->Logf("Received invalid frame. First 2 dwords: %x %x\n", magic, frameSize);
				Close();
				return {InternalRecvResult::Closed, Status000_NULL};
			}
			if (RecvBuf.Count >= frameSize + 8)
			{
				// We have a frame to process.
				const httpbridge::TxFrame* txframe = httpbridge::GetTxFrame((uint8_t*) RecvBuf.Data + 8);
				FrameStatus headStatus = FrameStatus::OK;
				FrameStatus bodyStatus = FrameStatus::OK;
				if (txframe->frametype() == httpbridge::TxFrameType_Header)
				{
					headStatus = UnpackHeader(txframe, inframe);
					inframe.IsHeader = true;
					inframe.IsLast = !!(txframe->flags() & httpbridge::TxFrameFlags_Final);
					bodyStatus = UnpackBody(txframe, inframe);
					if (headStatus == FrameStatus::OK && !inframe.Request->ParseURI())
						headStatus = FrameStatus::URITooLong;
				}
				else if (txframe->frametype() == httpbridge::TxFrameType_Body)
				{
					bodyStatus = UnpackBody(txframe, inframe);
					inframe.IsLast = !!(txframe->flags() & httpbridge::TxFrameFlags_Final);
				}
				else if (IsControlFrame(txframe->frametype()))
				{
					bodyStatus = UnpackControlFrame(txframe, inframe);
				}
				else
				{
					AnyLog()->Logf("Unrecognized frame type %d. Closing connection.", (int) txframe->frametype());
					Close();
					return {InternalRecvResult::Closed, Status000_NULL};
				}
				RecvBuf.EraseFromStart(8 + frameSize);
				if (headStatus != FrameStatus::OK || bodyStatus != FrameStatus::OK)
				{
					if (headStatus == FrameStatus::URITooLong)
					{
						return {InternalRecvResult::BadFrame, Status414_URI_Too_Long};
					}
					else if (headStatus == FrameStatus::OutOfMemory || bodyStatus == FrameStatus::OutOfMemory)
					{
						return {InternalRecvResult::BadFrame, Status503_Service_Unavailable};
					}
					else if (bodyStatus == FrameStatus::BodyLongerThanContentLength)
					{
						return {InternalRecvResult::BadFrame, Status400_Bad_Request};
					}
					else if (bodyStatus == FrameStatus::StreamNotFound)
					{
						// The only time we expect to receive FrameStreamNotFound, is when we have already
						// terminated a stream, but the client is still trying to send us data on that stream.
						// Since we have already sent a termination response, there is nothing further that
						// we can do, except for ignore the incoming stream data.
						inframe.Reset();
						return {InternalRecvResult::NoData, Status000_NULL};
					}
					else
					{
						HTTPBRIDGE_PANIC("Unexpected invalid frame state");
					}
				}
				return {InternalRecvResult::Data, Status000_NULL};
			}
		}

		return {InternalRecvResult::NoData, Status000_NULL};
	}

	Backend::FrameStatus Backend::UnpackHeader(const httpbridge::TxFrame* txframe, InFrame& inframe)
	{
		auto headers = txframe->headers();
		size_t headerBlockSize = TotalHeaderBlockSize(txframe);
		uint8_t* hblock = (uint8_t*) Alloc(headerBlockSize, Log, false);
		if (hblock == nullptr)
			return FrameStatus::OutOfMemory;

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

		inframe.Request.reset(new hb::Request());
		inframe.Request->Initialize(this, TranslateVersion(txframe->version()), txframe->channel(), txframe->stream(), headers->size(), hblock);
		return FrameStatus::OK;
	}

	Backend::FrameStatus Backend::UnpackBody(const httpbridge::TxFrame* txframe, InFrame& inframe)
	{
		if (inframe.Request == nullptr)
		{
			CurrentRequestLock.lock();
			auto cr = CurrentRequests.find(MakeStreamKey(txframe));
			if (cr == CurrentRequests.end())
			{
				flatbuffers::uoffset_t bodySize = 0;
				if (txframe->body())
					bodySize = txframe->body()->size();
				AnyLog()->Logf("Received body bytes for unknown stream [%llu:%llu] (%d body bytes)", txframe->channel(), txframe->stream(), (int) bodySize);
				CurrentRequestLock.unlock();
				return FrameStatus::StreamNotFound;
			}
			inframe.Request = cr->second.Request;
			CurrentRequestLock.unlock();
		}

		// Empty body frames are a waste, but not an error. Likely to be the final frame.
		if (txframe->body() == nullptr)
			return FrameStatus::OK;

		if (inframe.Request->IsBuffered)
		{
			HTTPBRIDGE_ASSERT(inframe.Request->ContentLength != -1);
			Buffer& buf = inframe.Request->BodyBuffer;
			if (buf.Count + inframe.BodyBytesLen > inframe.Request->ContentLength)
			{
				AnyLog()->Logf("Request sent too many body bytes. Ignoring frame [%llu:%llu] and ending stream", txframe->channel(), txframe->stream());
				return FrameStatus::BodyLongerThanContentLength;
			}
			// Check to see if we're going to exceed MaxWaitingBufferTotal.
			// Assume that a buffer realloc is going to grow by buf.Capacity (ie 2x growth).
			if (buf.Count + txframe->body()->size() > buf.Capacity && BufferedRequestsTotalBytes + buf.Capacity > MaxWaitingBufferTotal)
			{
				AnyLog()->Logf("MaxWaitingBufferTotal exceeded when receiving frame [%llu:%llu]", txframe->channel(), txframe->stream());
				return FrameStatus::OutOfMemory;
			}
			auto oldBufCapacity = buf.Capacity;
			if (!buf.TryWrite(txframe->body()->Data(), txframe->body()->size()))
			{
				AnyLog()->Logf("Failed to allocate memory for body frame [%llu:%llu]", txframe->channel(), txframe->stream());
				return FrameStatus::OutOfMemory;
			}
			BufferedRequestsTotalBytes += buf.Capacity - oldBufCapacity;
			// When buffering, just leave BodyBytes null, and BodyBytesLen zero. See comment in notes.md, from 2017-05-18
			return FrameStatus::OK;
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
			return FrameStatus::OK;
		}
	}

	Backend::FrameStatus Backend::UnpackControlFrame(const httpbridge::TxFrame* txframe, InFrame& inframe)
	{
		inframe.Type = FBTypeToFrameType(txframe->frametype());
		if (inframe.Request == nullptr)
		{
			CurrentRequestLock.lock();
			auto cr = CurrentRequests.find(MakeStreamKey(txframe));
			if (cr == CurrentRequests.end())
			{
				AnyLog()->Logf("Received control frame '%s' for unknown stream [%llu:%llu]", httpbridge::EnumNameTxFrameType(txframe->frametype()), txframe->channel(), txframe->stream());
				CurrentRequestLock.unlock();
				return FrameStatus::StreamNotFound;
			}
			inframe.Request = cr->second.Request;
			CurrentRequestLock.unlock();
			switch (inframe.Type)
			{
			case FrameType::Abort:
				RequestFinished(MakeStreamKey(txframe));
				inframe.Request->SetState(StreamState::Aborted);
				break;
			case FrameType::Pause:
				inframe.Request->SetState(StreamState::Paused);
				break;
			case FrameType::Resume:
				inframe.Request->SetState(StreamState::Active);
				break;
			default:
				HTTPBRIDGE_PANIC("Unexpected control frame type");
			}
		}
		return FrameStatus::OK;
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

	void Backend::SendResponse(RequestPtr request, StatusCode status)
	{
		Response response(request, status);
		Send(response);
	}

	void Backend::SendResponse(uint64_t channel, uint64_t stream, StatusCode status)
	{
		auto tmp = RequestPtr(new Request);
		tmp->Backend = this;
		tmp->Channel = channel;
		tmp->Stream = stream;
		Response response(tmp, status);
		Send(response);
	}

	Backend::RequestState* Backend::GetRequest(uint64_t channel, uint64_t stream)
	{
		auto iter = CurrentRequests.find(MakeStreamKey(channel, stream));
		if (iter == CurrentRequests.end())
			return nullptr;
		return &iter->second;
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

	StreamKey Backend::MakeStreamKey(ConstRequestPtr request)
	{
		return StreamKey{ request->Channel, request->Stream };
	}

	StreamKey Backend::MakeStreamKey(const httpbridge::TxFrame* txframe)
	{
		return StreamKey{ txframe->channel(), txframe->stream() };
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
			{
				if (buf[i] == '+')
					decodedPath[j++] = ' ';
				else
					decodedPath[j++] = buf[i];
			}
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
			{
				if (buf[p] == '+')
					decoded[j++] = ' ';
				else
					decoded[j++] = buf[p];
			}
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
		_State = StreamState::Active;
	}

	Request::~Request()
	{
		if (OnDestroy)
			OnDestroy(this);

		if (Backend && IsBuffered)
			Backend->UnregisterBufferedBytes(BodyBuffer.Capacity);

		hb::Free(_CachedURI);
		hb::Free((void*) _HeaderBlock);
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
			ContentLength = (uint64_t) atoi64(contentLength);
	}

	RequestPtr Request::CreateMocked(const std::string& method, const std::string& uri, const std::unordered_map<std::string, std::string>& headers, const std::string& body)
	{
		std::string hbContent;
		std::vector<HeaderLine> lines;
		size_t kvSize = sizeof(HeaderLine) * (2 + headers.size());
		auto append = [&](const std::string& key, const std::string& val)
		{
			HeaderLine line;
			line.KeyStart = (uint32_t) (kvSize + hbContent.size());
			line.KeyLen = (uint32_t) key.size();
			hbContent += key;
			hbContent.append(1, (char) 0);
			hbContent += val;
			hbContent.append(1, (char) 0);
			lines.push_back(line);
		};
		append(method, uri);
		for (auto& h : headers)
			append(h.first, h.second);

		// add a terminal line
		append("", "");

		// merge lines + hbContent into one block of memory
		char* hb = (char*) Alloc((uint32_t) kvSize + hbContent.size(), nullptr);
		memcpy(hb, &lines[0], kvSize);
		memcpy(hb + kvSize, hbContent.c_str(), hbContent.size());

		auto r = std::make_shared<Request>();
		r->Initialize(nullptr, HttpVersion11, 0, 0, (int32_t) lines.size() - 1, hb);
		r->ParseURI();
		r->IsBuffered = true;
		if (body.size() != 0)
			r->BodyBuffer.Write(body.c_str(), body.size());
		return r;
	}

	StreamState Request::State() const
	{
		return _State;
	}

	void Request::SetState(StreamState newState)
	{
		if (newState == StreamState::Aborted)
			_State = newState;
		else
		{
			// A stream that has been aborted may never leave the aborted state, so we guarantee that here
			StreamState existing = _State;
			if (existing != StreamState::Aborted)
				_State.compare_exchange_strong(existing, newState);
		}
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

	int64_t Request::QueryInt64(const char* key) const
	{
		const char* v = Query(key);
		return v ? atoi64(v) : 0;
	}

	double Request::QueryDouble(const char* key) const
	{
		const char* v = Query(key);
		return v ? strtod(v, nullptr) : 0;
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
				if (valLen == 0)
					*out = 0;
				else
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

	Response::Response(ConstRequestPtr request, StatusCode status)
	{
		Status = status;
		Request = request;
		Backend = request->Backend;
		Version = request->Version;
		Channel = request->Channel;
		Stream = request->Stream;
	}

	Response::Response(hb::Backend* backend, HttpVersion version, uint64_t channel, uint64_t stream, StatusCode status)
	{
		Backend = backend;
		Version = version;
		Channel = channel;
		Stream = stream;
		Status = status;
	}

	Response::Response(Response&& b)
	{
		*this = std::move(b);
	}

	Response& Response::operator=(Response&& b)
	{
		if (this != &b)
		{
			Free();
			memcpy(this, &b, sizeof(*this));
			memset(&b, 0, sizeof(*this));
		}
		return *this;
	}

	Response Response::MakeBodyPart(ConstRequestPtr request, const void* part, size_t len, bool isFinal)
	{
		Response r(request);
		r.SetBodyInternal(part, len);
		r.Status = StatusMeta_BodyPart;
		r.IsFinalChunkedFrame = isFinal;
		return r;
	}

	Response::~Response()
	{
		Free();
	}

	void Response::SetStatus(StatusCode status)
	{
		// A BodyPart frame may not contain any status. If you want to cancel a stream, then ... TODO
		HTTPBRIDGE_ASSERT(Status != StatusMeta_BodyPart);

		Status = status;
	}

	void Response::SetStatusAndBody(StatusCode status, const char* body)
	{
		// Body Part is a standalone frame. Use MakeBodyPart() or Backend.SendBodyPart().
		HTTPBRIDGE_ASSERT(Status != StatusMeta_BodyPart);

		SetStatus(status);
		SetBody(body, strlen(body));
	}

	void Response::SetStatusAndBody(StatusCode status, const std::string& body) {
		// Body Part is a standalone frame. Use MakeBodyPart() or Backend.SendBodyPart().
		HTTPBRIDGE_ASSERT(Status != StatusMeta_BodyPart);

		SetStatus(status);
		SetBody(body.c_str(), body.size());
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
		// Body part may not contain headers
		HTTPBRIDGE_ASSERT(Status != StatusMeta_BodyPart);

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
		char buf[21];
		if (contentLength == -1)
			memcpy(buf, "-1", 3);
		else
			u64toa(contentLength, buf);
		AddHeader(Header_Content_Length, buf);
	}

	void Response::SetBody(const void* body, size_t len)
	{
		// Use MakeBodyPart()
		HTTPBRIDGE_ASSERT(Status != StatusMeta_BodyPart);

		SetBodyInternal(body, len);
	}

	void Response::SetBodyInternal(const void* body, size_t len)
	{
		// Ensure sanity, as well as safety because BodyLength is uint32
		HTTPBRIDGE_ASSERT(len <= 1024 * 1024 * 1024);

		// Although it's theoretically possible to allow SetBody to be called multiple times
		// (ie discard FBB every time) it is so wasteful that we rather force the user to
		// construct their code in such a manner that this is not necessary.
		HTTPBRIDGE_ASSERT(BodyOffset == 0 && BodyLength == 0);

		void* enc = nullptr;
		size_t encLen = -1;

		const char* acceptEncoding = nullptr;

		if (Request && Backend && Backend->Compressor)
		{
			// The following two conditions disable transparent compression:
			// * Content-Encoding is set
			// * Content-Length is set
			acceptEncoding = Request->HeaderByName("Accept-Encoding");
			if (acceptEncoding && !HeaderByName("Content-Encoding") && !HeaderByName("Content-Length"))
			{
				char responseEncoding[ICompressor::ResponseEncodingBufferSize];
				responseEncoding[0] = 0;
				if (Backend->Compressor->Compress(acceptEncoding, body, len, enc, encLen, responseEncoding))
				{
					HTTPBRIDGE_ASSERT(responseEncoding[0] != 0);
					responseEncoding[sizeof(responseEncoding) - 1] = 0;
					AddHeader("Content-Encoding", responseEncoding);
					body = enc;
					len = encLen;
				}
			}
		}

		CreateBuilder();

		FBB->NotNested();
		FBB->StartVector(len, sizeof(uint8_t));
		FBB->PushBytes((const uint8_t*) body, len);
		BodyOffset = (ByteVectorOffset) FBB->EndVector(len);
		BodyLength = (uint32_t) len;

		if (enc)
			Backend->Compressor->Free(acceptEncoding, enc);
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
		if (index >= HeaderCount())
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
		frame.add_frametype(Status == StatusMeta_BodyPart ? httpbridge::TxFrameType_Body : httpbridge::TxFrameType_Header);
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
		uint8_t b4[4];
		Write32LE(b4, (uint32_t) len);
		FBB->PushBytes(b4, 4);

		// Write out magic frame marker. This is just used to catch bugs in framing code.
		Write32LE(b4, (uint32_t) MagicFrameMarker);
		FBB->PushBytes(b4, 4);

		len = FBB->GetSize();
		buf = FBB->GetBufferPointer();
		// Now 'len' contains the size of the flatbuffer + 8

		IsFlatBufferBuilt = true;
	}

	void Response::SerializeToHttp(void*& buf, size_t& len)
	{
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
		const void* bodyBuf = nullptr;
		size_t bodyLen = 0;
		GetBody(bodyBuf, bodyLen);
		BufWrite(out, bodyBuf, bodyLen);
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
			// The flatbuffer buffer grows downward, and GetCurrentBufferPointer() returns the low point.
			// So we add back the FBB size to get to the starting high point, and then subtract our BodyOffset,
			// then add 4, to get our body pointer. I don't know where the 4 comes from.
			buf = FBB->GetCurrentBufferPointer() + FBB->GetSize() - BodyOffset + 4;
		}
	}

	std::string Response::GetBody() const
	{
		const void* buf = nullptr;
		size_t len = 0;
		GetBody(buf, len);
		return std::string((const char*) buf, len);
	}

	void Response::Free()
	{
		delete FBB;
		FBB = nullptr;
		HeaderIndex.Clear();
		HeaderBuf.Clear();
		Backend = nullptr;
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
		if (i + 2 > (uint32_t) HeaderIndex.Size())
			return 0;

		if (i + 2 == HeaderIndex.Size())
			top = HeaderBuf.Size();
		else
			top = HeaderIndex[i + 2];

		return top - HeaderIndex[i + 1] - 1; // extra -1 is to remove null terminator
	}

}
