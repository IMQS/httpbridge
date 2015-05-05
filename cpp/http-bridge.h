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
3. Figure out in which conditions a request will have a body but no Content-Length header. Right now
	we're a bit ambivalent on this one, for example we copy Content-Length into Request.BodyLength,
	but then we rely on the Server to send us a flag indicating that a frame is Final.

------------------------------------------------------------------------------------------------

c:\dev\head\otaku\t2-output\win64-2013-debug-default\flatc -c -o cpp -b http-bridge.fbs
c:\dev\head\otaku\t2-output\win64-2013-debug-default\flatc -g -o go -b http-bridge.fbs
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
	class InFrame;
	class HeaderCacheRecv;		// Implemented in http-bridge.cpp

	enum SendResult
	{
		SendResult_All,			// All of the data was sent
		SendResult_BufferFull,	// Some of the data might have been sent, but the buffer is now full.
		SendResult_Closed,		// The transport channel has been closed by the OS
	};

	enum RecvResult
	{
		RecvResult_NoData,		// Nothing read
		RecvResult_Data,		// Some data has been read.
		RecvResult_Closed,		// The transport channel has been closed by the OS
	};

	//enum RequestStatus
	//{
	//	RequestStatus_Receiving,	// Body is busy being received
	//	RequestStatus_Received,		// Entire body has been received
	//};

	enum HttpVersion
	{
		HttpVersion10,
		HttpVersion11,
		HttpVersion2,
	};

	enum StatusCode
	{
		Status100_Continue = 100,
		Status101_Switching_Protocols = 101,
		Status102_Processing = 102,
		Status200_OK = 200,
		Status201_Created = 201,
		Status202_Accepted = 202,
		Status203_Non_Authoritative_Information = 203,
		Status204_No_Content = 204,
		Status205_Reset_Content = 205,
		Status206_Partial_Content = 206,
		Status207_Multi_Status = 207,
		Status208_Already_Reported = 208,
		Status226_IM_Used = 226,
		Status300_Multiple_Choices = 300,
		Status301_Moved_Permanently = 301,
		Status302_Found = 302,
		Status303_See_Other = 303,
		Status304_Not_Modified = 304,
		Status305_Use_Proxy = 305,
		Status307_Temporary_Redirect = 307,
		Status308_Permanent_Redirect = 308,
		Status400_Bad_Request = 400,
		Status401_Unauthorized = 401,
		Status402_Payment_Required = 402,
		Status403_Forbidden = 403,
		Status404_Not_Found = 404,
		Status405_Method_Not_Allowed = 405,
		Status406_Not_Acceptable = 406,
		Status407_Proxy_Authentication_Required = 407,
		Status408_Request_Timeout = 408,
		Status409_Conflict = 409,
		Status410_Gone = 410,
		Status411_Length_Required = 411,
		Status412_Precondition_Failed = 412,
		Status413_Payload_Too_Large = 413,
		Status414_URI_Too_Long = 414,
		Status415_Unsupported_Media_Type = 415,
		Status416_Range_Not_Satisfiable = 416,
		Status417_Expectation_Failed = 417,
		Status421_Misdirected_Request = 421,
		Status422_Unprocessable_Entity = 422,
		Status423_Locked = 423,
		Status424_Failed_Dependency = 424,
		Status425_Unassigned = 425,
		Status426_Upgrade_Required = 426,
		Status427_Unassigned = 427,
		Status428_Precondition_Required = 428,
		Status429_Too_Many_Requests = 429,
		Status430_Unassigned = 430,
		Status431_Request_Header_Fields_Too_Large = 431,
		Status500_Internal_Server_Error = 500,
		Status501_Not_Implemented = 501,
		Status502_Bad_Gateway = 502,
		Status503_Service_Unavailable = 503,
		Status504_Gateway_Timeout = 504,
		Status505_HTTP_Version_Not_Supported = 505,
		Status506_Variant_Also_Negotiates = 506,
		Status507_Insufficient_Storage = 507,
		Status508_Loop_Detected = 508,
		Status509_Unassigned = 509,
		Status510_Not_Extended = 510,
		Status511_Network_Authentication_Required = 511,		
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
	HTTPBRIDGE_API uint32_t		Read32LE(const void* buf);
	HTTPBRIDGE_API void			Write32LE(void* buf, uint32_t v);
	HTTPBRIDGE_API int			U32toa(uint32_t v, char* buf, size_t bufSize);
	HTTPBRIDGE_API int			U64toa(uint64_t v, char* buf, size_t bufSize);
	HTTPBRIDGE_API uint64_t		uatoi64(const char* s, size_t len);
	HTTPBRIDGE_API uint64_t		uatoi64(const char* s);

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

	// Byte buffer.
	class HTTPBRIDGE_API Buffer
	{
	public:
		uint8_t*	Data = nullptr;
		size_t		Count = 0;
		size_t		Capacity = 0;

					Buffer();
					~Buffer();

		uint8_t*	Preallocate(size_t n);
		void		EraseFromStart(size_t n);
		void		Write(const void* buf, size_t n);		// Uses GrowCapacityOrPanic()
		bool		TryWrite(const void* buf, size_t n);	// Returns false if allocation fails
		void		WriteStr(const char* s);				// Uses GrowCapacityOrPanic()
		void		WriteUInt64(uint64_t v);				// Uses GrowCapacityOrPanic()
		void		GrowCapacityOrPanic();
		bool		TryGrowCapacity();						// Returns false if allocation fails
		bool		IsPointerInside(const void* p) const { return ((size_t) ((uint8_t*) p - Data)) < Capacity; }
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
		
		// Maximum number of bytes that will be allocated for 'ResendWhenBodyIsDone' requests. Total shared by all pending requests.
		size_t				MaxWaitingBufferTotal = 1024 * 1024 * 1024;
		
		// Maximum size of a single request who's body will be automatically sent through 'ResendWhenBodyIsDone'. Set to zero to disable.
		// If a request is smaller or equal to MaxAutoBufferSize, but our total buffer quota (MaxWaitingBufferTotal) has been exceeded by the queue, then
		// the request will return with a Status503_Service_Unavailable.
		size_t				MaxAutoBufferSize = 16 * 1024 * 1024;		

		// Initial size of receiving buffer, per request. If this value is large, then it becomes trivial for an attacker to cause your server
		// to exhaust all of it's memory pool, without transmitting much data.
		size_t				InitialBufferSize = 4096;

							Backend();
							~Backend();							// This calls Close()
		bool				Connect(const char* network, const char* addr);
		bool				IsConnected();
		void				Close();
		SendResult			Send(Response& response);
		bool				Recv(InFrame& frame);						// Returns true if a frame was received
		bool				ResendWhenBodyIsDone(InFrame& frame);		// Called by InFrame.ResendWhenBodyIsDone(). Returns false if out of memory.
		void				RequestDestroyed(const StreamKey& key);		// Intended to be called ONLY by Request's destructor.

	private:
		hb::HeaderCacheRecv* HeaderCacheRecv = nullptr;
		ITransport*			Transport = nullptr;
		Logger				NullLog;
		uint8_t*			RecvBuf = nullptr;
		size_t				RecvBufCapacity = 0;
		size_t				RecvSize = 0;
		std::thread::id		ThreadId;
		StreamToRequestMap	CurrentRequests;
		size_t				BufferedRequestsTotalBytes = 0;		// Total number of body bytes allocated for "BufferedRequests"

		RecvResult			RecvInternal(InFrame& inframe);
		Logger*				AnyLog();
		bool				Connect(ITransport* transport, const char* addr);
		void				UnpackHeader(const httpbridge::TxFrame* txframe, InFrame& inframe);
		bool				UnpackBody(const httpbridge::TxFrame* txframe, InFrame& inframe);		// Returns false if there is a protocol violation
		size_t				TotalHeaderBlockSize(const httpbridge::TxFrame* frame);
		void				LogAndPanic(const char* msg);
		void				SendResponse(Request& request, StatusCode status);
		static StreamKey	MakeStreamKey(uint64_t channel, uint64_t stream);
		static StreamKey	MakeStreamKey(const Request& request);
	};

	/* HTTP request
	Note that header keys and values are always null terminated. The HTTP/2 spec allows headers
	to contain arbitrary binary data, so you may be missing something by not using the header accessor functions
	that allow you to read through null characters, but that's your choice.

	The lifetime of a Request object is determined by a liveness count. The liveness count starts out at 2.
	Liveness is decremented as follows:
		* When the frame marked as IsLast is destroyed, liveness is decremented by one.
		* When the response is sent, liveness is decremented by one.
		* When the frame marked as IsAborted is destroyed, liveness is decremented by two (because aborted streams never get a response).
	If liveness drops to zero, the request object is destroyed, and it is removed from Backend's table of current requests.
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

		hb::Backend*			Backend = nullptr;
		//hb::RequestStatus		Status = RequestStatus_Receiving;
		bool					IsBuffered = false;
		HttpVersion				Version = HttpVersion10;
		uint64_t				Channel = 0;
		uint64_t				Stream = 0;
		uint64_t				BodyLength = 0;			// The total length of the body of this request. This is simply a cache of the Content-Length header.
		hb::Buffer				BodyBuffer;				// If IsBuffered = true, then BodyBuffer stores the entire body

								Request();
								~Request();

		// Set a header. The Request object now owns headerBlock, and will Free() it in the destructor
		// The header block must have a terminal pair in inside in order to make iteration easy.
		void					Initialize(hb::Backend* backend, HttpVersion version, uint64_t channel, uint64_t stream, int32_t headerCount, const void* headerBlock);

		// Returns true if the Request object was destroyed
		bool					DecrementLiveness();

		// Free any memory that we own, and then initialize to a newly-constructed Request
		void					Reset();

		const char*				Method() const;		// Returns the method, such as GET or POST
		const char*				URI() const;		// Returns the URI of the request

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

	private:
		static const int		NumPseudoHeaderLines = 1;
		int32_t					_HeaderCount = 0;
		const uint8_t*			_HeaderBlock = nullptr;		// First HeaderLine[] array and then the headers themselves
		int						_Liveness = 2;				// A reference count on Request.

		void					Free();
	};

	// A frame received from the client
	class HTTPBRIDGE_API InFrame
	{
	public:
		hb::Request*	Request;
		bool			IsHeader;			// Is this the first frame of the request? Note that IsHeader and IsLast are both true for a request with an empty body.
		bool			IsLast;				// Is this the last frame of the request? If true, then the request is deleted by the frame's destructor.
		bool			IsAborted;			// True if the stream has been aborted (ie the browser connection timed out, etc). Such a frame contains no data, and IsLast is guaranteed to be false.

		uint8_t*		BodyBytes;			// Body bytes in this frame
		size_t			BodyBytesLen;		// Length of BodyBytes

		InFrame();
		~InFrame();

		void	Reset();	// Calls destructor and re-initializes

		// Calls Request->Backend->ResentWhenBodyIsDone(this)
		bool	ResendWhenBodyIsDone();

	private:
		InFrame(const InFrame&) = delete;
		InFrame& operator= (const InFrame&) = delete;
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