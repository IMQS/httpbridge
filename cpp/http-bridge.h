#pragma once
#ifndef HTTPBRIDGE_INCLUDED
#define HTTPBRIDGE_INCLUDED
/*

***************
* HTTP-Bridge *
***************

The purpose of this system is to allow one to write HTTP/2-based services without embedding 
an HTTP/2 server inside your code. This is basically fastcgi for HTTP/2.

The model is this:

You write a program that embeds an HTTP/2 server inside it. Inside that program, you
run an http-bridge server. That http-bridge server listens on your internal network,
on some special port.

You write another program that hosts your HTTP API. We call this program a backend.
From the backend, you use http-bridge to connect to your http-bridge server. You make
a single TCP connection (or pipe, or 'null' connection) between your backend and your
server, and all http-bridge traffic flows over that one connection.

Why?
Not everybody wants to embed an HTTP/2 server inside their programs.

Terms:
* CLIENT  - A browser or other agent that initiates HTTP/2 connections to a SERVER
* SERVER  - A genuine HTTP/2 server that accepts connections from a CLIENT and forwards them to a BACKEND
* BACKEND - An agent that communicates with a SERVER in order to receive requests
* CHANNEL - An HTTP/2 channel. A channel typically has multiple streams associated with it.
			CHANNEL is our own invention, necessary for multiplexing multiple HTTP/2 connections over
			a single TCP socket between a SERVER and a BACKEND.
* STREAM  - An HTTP/2 connection. There is a single connection between a CLIENT and a SERVER, and typically
            multiple streams within that single connection. Each stream is an HTTP/2 session between a CLIENT
			and a SERVER. STREAM has the same meaning that it does in the HTTP/2 specification. A stream
			initiated by a CLIENT must use odd-numbered identifiers. A stream initiated by a BACKEND must
			use even-numbered identifiers.

Conventions
Pairs of parameters to a function, where a buffer is specified, are written in the order [length,data]. This is
opposite to most of the standard C library, where it is [data,length].

TODO
1. Support more advanced features of HTTP/2 such as PUSH frames.
2. Support streaming responses out, instead of being forced to sent the entire response at once
	(this is a C++ implementation issue, not a protocol issue).

------------------------------------------------------------------------------------------------

t2-output\win64-2013-debug-default\flatc -c -o third_party\http-bridge -b third_party\http-bridge\http-bridge.fbs
t2-output\win64-2013-debug-default\flatc -g -o third_party\http-bridge\goserver\src -b third_party\http-bridge\http-bridge.fbs
*/

#include <stdint.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <thread>

namespace flatbuffers
{
	class FlatBufferBuilder;
}

namespace httpbridge
{
	// Flatbuffer classes
	struct TxFrame;
	struct TxFrameBuilder;
}

namespace hb
{
#ifdef _Printf_format_string_
#define HTTPBRIDGE_PRINTF_FORMAT_Z _In_z_ _Printf_format_string_
#else
#define HTTPBRIDGE_PRINTF_FORMAT_Z
#endif

#ifndef HTTPBRIDGE_API
#define HTTPBRIDGE_API
#endif

#ifdef _MSC_VER
#define HTTPBRIDGE_NORETURN_PREFIX __declspec(noreturn)
#define HTTPBRIDGE_NORETURN_SUFFIX
#else
#define HTTPBRIDGE_NORETURN_PREFIX
#define HTTPBRIDGE_NORETURN_SUFFIX __attribute__((noreturn))
#endif

HTTPBRIDGE_API                            void PanicMsg(const char* file, int line, const char* msg);
HTTPBRIDGE_API HTTPBRIDGE_NORETURN_PREFIX void BuiltinTrap() HTTPBRIDGE_NORETURN_SUFFIX;

// HTTPBRIDGE_ASSERT is compiled in all builds (not just debug)
#define HTTPBRIDGE_ASSERT(c)			(void) ((c) || (hb::PanicMsg(__FILE__,__LINE__,#c), hb::BuiltinTrap(), 0) )
#define HTTPBRIDGE_PANIC(msg)			(void) ((hb::PanicMsg(__FILE__,__LINE__,msg), hb::BuiltinTrap(), 0) )

#ifdef _WIN32
#define HTTPBRIDGE_PLATFORM_WINDOWS 1
#endif

	class ITransport;
	class IWriter;
	class Logger;
	class Request;
	class Response;
	class HeaderCacheRecv;		// Implemented in http-bridge.cpp

	enum SendResult
	{
		SendResult_All,			// All of the data was sent
		SendResult_BufferFull,	// Some of the data might have been sent, but the buffer is now full.
		SendResult_Closed		// The transport channel has been closed by the OS
	};

	enum RecvResult
	{
		RecvResult_NoData,		// Nothing read
		RecvResult_Data,		// Some data has been read.
		RecvResult_Closed		// The transport channel has been closed by the OS
	};

	enum HttpVersion
	{
		HttpVersion10,
		HttpVersion11,
		HttpVersion2,
	};

	enum StatusCode
	{
		Status200_OK = 200,
		Status400_BadRequest = 400,
		Status503_ServiceUnavailable = 503,
	};

	HTTPBRIDGE_API bool			Startup();								// Must be called before any other httpbridge functions are called
	HTTPBRIDGE_API void			Shutdown();								// Must be called after all use of httpbridge
	HTTPBRIDGE_API const char*	VersionString(HttpVersion version);		// Returns HTTP/1.0 HTTP/1.1 HTTP/2.0
	HTTPBRIDGE_API const char*	StatusString(StatusCode status);		// "OK", "Not Found", etc
	HTTPBRIDGE_API size_t		Hash16B(uint64_t pair[2]);				// Hash 16 bytes
	HTTPBRIDGE_API void			SleepNano(int64_t nanoseconds);
	HTTPBRIDGE_API void*		Alloc(size_t size, Logger* logger, bool panicOnFail = true);
	HTTPBRIDGE_API void*		Realloc(void* buf, size_t size, Logger* logger, bool panicOnFail = true);
	HTTPBRIDGE_API void			Free(void* buf);

	class HTTPBRIDGE_API Logger
	{
	public:
		virtual void	Log(const char* msg);				// Default implementation writes to stdout
		void			Logf(HTTPBRIDGE_PRINTF_FORMAT_Z const char* msg, ...);
	};

	class HTTPBRIDGE_API ITransport
	{
	public:
		Logger*				Log = nullptr;						// Server::Connect copies its log in here during successful Connect()

		virtual				~ITransport();						// This must close the socket/file/pipe/etc
		virtual bool		Connect(const char* addr) = 0;
		virtual SendResult	Send(size_t size, const void* data, size_t& sent) = 0;
		virtual RecvResult	Recv(size_t maxSize, size_t& bytesRead, void* data) = 0;
	};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 28182 6308 6001)	// /analyze doesn't understand that HTTPBRIDGE_ASSERT checks for realloc failure
#endif

	// Vector with 32-bit size and capacity. Type T must be relocatable,
	// meaning it may not store pointers into itself. This allows us to
	// use realloc to grow the vector, instead of new[]/delete[], which is
	// significantly faster when T has heap-allocated memory (eg Vector<string> or Vector<Vector<int>>).
	template<typename T>
	class Vector
	{
	public:
		typedef int32_t INT;

		Vector() {}
		~Vector() { for (INT i = 0; i < Count; i++) Items[i].T::~T(); free(Items); }

		void Push(const T& v)
		{
			if (Count == Capacity)
				Grow();
			Items[Count++] = v;
		}

		// Add enough space for 'n' more items. Return a pointer to the first of those 'n' items.
		T* AddSpace(INT n)
		{
			while (Count + n >= Capacity)
				Grow();
			Count += n;
			return &Items[Count - n];
		}

		INT Size() const { return Count; }

		void Resize(INT newSize)
		{
			if (newSize != Count || newSize != Capacity)
			{
				// destroy items that are going to be lost
				for (INT i = newSize; i < Capacity; i++)
					Items[i].T::~T();
				
				Items = (T*) realloc(Items, newSize * sizeof(T));
				HTTPBRIDGE_ASSERT(Items != nullptr);
				
				// initialize new items
				for (INT i = Capacity; i < newSize; i++)
					new(&Items[i]) T();

				Capacity = Count = newSize;
			}
		}

		T& operator[](INT i) { return Items[i]; }
		const T& operator[](INT i) const { return Items[i]; }

	private:
		INT		Capacity = 0;
		INT		Count = 0;
		T*		Items = nullptr;

		void Grow()
		{
			INT newCap = Capacity == 0 ? 1 : Capacity * 2;
			Items = (T*) realloc(Items, newCap * sizeof(T));
			HTTPBRIDGE_ASSERT(Items != nullptr);
			for (INT i = Capacity; i < newCap; i++)
				new(&Items[i]) T();
			Capacity = newCap;
		}
	};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

	struct StreamKey
	{
		uint64_t Channel;
		uint64_t Stream;
		bool operator==(const StreamKey& b) const { return Channel == b.Channel && Stream == b.Stream; }
	};
}
namespace std
{
	template <>
	class hash<hb::StreamKey>
	{
	public:
		size_t operator()(const hb::StreamKey& k) const
		{
			return hb::Hash16B((uint64_t*) &k);
		}
	};
}
namespace hb
{
	typedef std::unordered_map<StreamKey, Request*> StreamToRequestMap;

	/* A backend that wants to receive HTTP/2 requests
	To connect to an upstream http-bridge server, call Connect("tcp", "host:port")
	*/
	class HTTPBRIDGE_API Backend
	{
	public:
		Logger*				Log = nullptr;								// This is not owned by Backend. Backend will never delete this.
		size_t				MaxWaitingBufferTotal = 256 * 1024 * 1024;	// Maximum number of bytes that will be allocated for 'ResendWhenBodyIsDone' requests.

							Backend();
							~Backend();							// This calls Close()
		bool				Connect(const char* network, const char* addr);
		bool				IsConnected();
		void				Close();
		SendResult			Send(Response& response);
		RecvResult			Recv(Request& request);
		void				ResendWhenBodyIsDone(Request& request);	// Called by Request.ResendWhenBodyIsDone(). Panics if Request.IsEntireBodyInsideHeader() is false, or not a HEADER frame.

	private:
		hb::HeaderCacheRecv* HeaderCacheRecv = nullptr;
		ITransport*			Transport = nullptr;
		Logger				NullLog;
		uint8_t*			RecvBuf = nullptr;
		size_t				RecvBufCapacity = 0;
		size_t				RecvSize = 0;
		std::thread::id		ThreadId;
		StreamToRequestMap	WaitingRequests;		// Requests waiting to have all of their body transferred
		size_t				WaitingBufferTotal = 0;	// Total number of body bytes allocated for "WaitingRequests"

		RecvResult			RecvInternal(Request& request);
		Logger*				AnyLog();
		bool				Connect(ITransport* transport, const char* addr);
		void				UnpackHeader(const httpbridge::TxFrame* frame, Request& request);
		void				UnpackBody(const httpbridge::TxFrame* frame, Request& request);
		size_t				TotalHeaderBlockSize(const httpbridge::TxFrame* frame);
		void				LogAndPanic(const char* msg);
		void				SendResponse(const Request& request, StatusCode status);
		void				CloseWaiting(StreamKey key);
	};

	/* HTTP request
	Note that header keys and values are always null terminated. The HTTP/2 spec allows headers
	to contain arbitrary binary data, so you may be missing something by not using the header accessor functions
	that allow you to read through null characters, but that's your choice.
	*/
	class HTTPBRIDGE_API Request
	{
	public:
		/*
		Our header is packed as follows:
		HeaderLine[]				Each header line contains the start of the key, and the key length (as uint32, uint32).
		char[]						The header data itself (ie keys and values)
		Since all data is tightly packed, we can compute the position and size of the *values* of the headers, even though we
		only store the start and length of the *keys* of the headers.
		The first header line is pseudo. It's key is the HTTP verb, and it's value is the URI.
		This first special header line is the reason for all the "1" offsets.
		*/

		struct HeaderLine
		{
			uint32_t	KeyStart;
			uint32_t	KeyLen;
		};

								Request();
								~Request();

		// Set a header. The Request object now owns headerBlock, and will Free() it in the destructor
		void					InitHeader(Backend* backend, HttpVersion version, uint64_t channel, uint64_t stream, uint64_t bodyTotalLength, int32_t headerCount, const void* headerBlock);

		// Set a piece of the body. The Request object now owns body, and will Free() it in the destructor
		void					InitBody(Backend* backend, HttpVersion version, uint64_t channel, uint64_t stream, uint64_t bodyTotalLength, uint64_t bodyOffset, uint64_t bodyBytes, const void* body);

		hb::Backend*			Backend() const		{ return _Backend; }
		bool					IsHeader() const	{ return _IsHeader; }		// If not header, then only body
		uint64_t				Channel() const		{ return _Channel; }
		uint64_t				Stream() const		{ return _Stream; }

		HttpVersion				Version() const;	// Only valid on a HEADER frame
		const char*				Method() const;		// Only valid on a HEADER frame, returns the method, such as GET or POST
		const char*				URI() const;		// Only valid on a HEADER frame, returns the URI of the request

		// Returns the number of headers
		int32_t					HeaderCount() const { return _HeaderCount == 0 ? 0 : _HeaderCount - NumPseudoHeaderLines; }
		
		// Returns true if the header exists, in which case val points to the start of the header value,
		// and valLen contains the length of the header, excluding the null terminator.
		// The buffer is guaranteed to be null terminated, regardless of its contents.
		// 'nth' allows you to fetch multiple headers, if there is more than one header with the same name.
		// Set nth=0 to fetch the first header, nth=1 to fetch the 2nd, etc.
		// Returns false if the header does not exist.
		bool					HeaderByName(const char* name, size_t& valLen, const void*& val, int nth = 0) const;

		// Using this function means you cannot consume headers with embedded null characters in them, but a lot of the time that's OK.
		// Returns null if the header does not exist.
		const char*				HeaderByName(const char* name, int nth = 0) const;

		// Retrieve a header by index (valid indexes are 0 .. HeaderCount()-1)
		// Both key and val are guaranteed to be null terminated. keyLen and valLen are the length of
		// key and val, respectively, but the lengths do not include the null terminators.
		void					HeaderAt(int32_t index, int32_t& keyLen, const char*& key, int32_t& valLen, const char*& val) const;

		// Using this function means you cannot consume headers with embedded null characters in them, but a lot of the time that's OK.
		void					HeaderAt(int32_t index, const char*& key, const char*& val) const;

		// The body data in this frame
		const void*				FrameBody() const { return _FrameBody; }

		// The number of body bytes in this frame
		uint64_t				FrameBodyLength() const { return _FrameBodyLength; }

		// The offset from the start of the entire request's body, to the start of this frame's body bytes.
		uint64_t				FrameBodyOffset() const { return _FrameBodyOffset; }

		// The total length of the body of this request
		uint64_t				BodyLength() const { return _BodyLength; }

		// Return a BodyReader object, which allows you to synchronously read the entire body of this request
		//BodyReader*				BodyReader();

		// Put this request back in the queue, and call us again when the client has sent all of the request body.
		// If IsEntireBodyInsideHeader() is true, or this is not a HEADER frame, then this function panics.
		void					ResendWhenBodyIsDone();

		// Returns true if this is a header frame, and the entire body is contained inside it (or there is no body)
		bool					IsEntireBodyInsideHeader() const;

		// Called by Backend when it is filling a buffer waiting in the queue, after having called ResendWhenBodyIsDone().
		void					WriteBodyData(size_t offset, size_t len, const void* data);

		// Reallocate body buffer so that it can hold the entire body. This is called by Backend at the start of ResendWhenBodyIsDone().
		// This is one of the few functions that returns false if a memory allocation fails.
		bool					ReallocForEntireBody();

	private:
		static const int		NumPseudoHeaderLines = 1;
		hb::Backend*			_Backend = nullptr;
		//hb::BodyReader*		_BodyReader = nullptr;
		bool					_IsHeader = false;			// else Body
		HttpVersion				_Version = HttpVersion10;
		uint64_t				_Channel = 0;
		uint64_t				_Stream = 0;
		int32_t					_HeaderCount = 0;
		const uint8_t*			HeaderBlock = nullptr;		// First HeaderLine[] array and then the headers themselves
		const void*				_FrameBody = nullptr;
		uint64_t				_FrameBodyOffset = 0;
		uint64_t				_FrameBodyLength = 0;
		uint64_t				_BodyLength = 0;
	};

	/* HTTP Response
	*/
	class HTTPBRIDGE_API Response
	{
	public:
		typedef uint32_t ByteVectorOffset;

		HttpVersion			Version = HttpVersion10;
		uint64_t			Channel = 0;
		uint64_t			Stream = 0;
		StatusCode			Status = Status200_OK;

		Response();
		~Response();

		void			Init(const Request& request);														// Copy [Stream, Channel, Version] from request
		void			SetStatus(StatusCode status)														{ Status = status; }
		void			WriteHeader(const char* key, const char* value);									// Add a header
		void			WriteHeader(int32_t keyLen, const char* key, int32_t valLen, const char* value);	// Add a header
		void			SetBody(size_t len, const void* body);												// Set the entire body.
		
		int32_t			HeaderCount() const { return HeaderIndex.Size() / 2; }

		// Fetch a header by name.
		// Returns null if the header does not exist.
		const char*		HeaderByName(const char* name, int nth = 0) const;

		// Both key and val are guaranteed to be null terminated
		void			HeaderAt(int32_t index, int32_t& keyLen, const char*& key, int32_t& valLen, const char*& val) const;
		
		// Both key and val are guaranteed to be null terminated
		void			HeaderAt(int32_t index, const char*& key, const char*& val) const;

		void			FinishFlatbuffer(size_t& len, void*& buf);
		void			SerializeToHttp(size_t& len, void*& buf);											// The returned 'buf' must be freed with free()

	private:
		flatbuffers::FlatBufferBuilder*		FBB = nullptr;
		ByteVectorOffset					BodyOffset = 0;
		uint32_t							BodyLength = 0;
		bool								IsFlatBufferBuilt = false;

		// Our header keys and values are always null terminated. This is necessary in order
		// to provide a consistent API between Request and Response objects.
		Vector<uint32_t>					HeaderIndex;
		Vector<char>						HeaderBuf;

		void	CreateBuilder();
		int32_t	HeaderKeyLen(int32_t i) const;
		int32_t	HeaderValueLen(int32_t i) const;
	};
}

namespace std
{
	inline void swap(hb::Request& a, hb::Request& b)
	{
		hb::Request tmp;
		memcpy(&tmp, &a, sizeof(tmp));
		memcpy(&a, &b, sizeof(tmp));
		memcpy(&b, &tmp, sizeof(tmp));
		memset(&tmp, 0, sizeof(tmp));
	}
}
#endif