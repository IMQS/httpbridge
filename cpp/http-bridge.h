// clang-format off
#pragma once
#ifndef HTTPBRIDGE_INCLUDED
#define HTTPBRIDGE_INCLUDED

/*

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

TODO
1. Support more advanced features of HTTP/2 such as PUSH frames.

*/

#include <stdint.h>
#include <string.h>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>

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
	class HeaderCacheRecv;		// Implementation and header inside in http-bridge.cpp

	typedef std::shared_ptr<Request>		RequestPtr;
	typedef std::shared_ptr<const Request>	ConstRequestPtr;
	typedef void(*RequestDestroyCallback)(Request*);

	// This dword appears before every frame. It is followed by 4 bytes of frame size, and then the flatbuffer.
	const uint32_t MagicFrameMarker = 0x48426268; // "HBbh"

	enum SendResult
	{
		SendResult_All,			// All of the data was sent
		SendResult_BufferFull,	// Some of the data might have been sent, but the buffer is now full.
		SendResult_Closed,		// The transport channel has been closed.
	};

	enum RecvResult
	{
		RecvResult_NoData,		// Nothing read
		RecvResult_Data,		// Some data has been read.
		RecvResult_Closed,		// The transport channel has been closed by the OS
	};

	enum HttpVersion
	{
		HttpVersion10,
		HttpVersion11,
		HttpVersion2,
	};

	enum class FrameType
	{
		Data,		// Header and/or Body data
		Abort,		// Abort stream
		Pause,		// Pause transmission of response
		Resume,		// Resume transmission of response
	};

	enum class StreamState : uint32_t
	{
		Active,		// The stream is in it's normal state; Receiving a request, or sending a response.
		Aborted,	// The stream has been aborted. Do not send any more frames.
		Paused,		// The stream has been paused. Wait until the stream is Active again before sending any more frames. TCP buffer from server -> client is full.
	};

	enum StatusCode
	{
		StatusMeta_BodyPart = 1000,	// Used in a Response message to indicate that this is a body part that is being transmitted
		StatusMeta_WillSend = 1001,	// Used to inform an HTTP dispatcher (but not httpbridge) that you will send the body out from another thread
		Status000_NULL = 0,			// Used where a NULL value is required

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

	extern const char* Header_Content_Length;

	HTTPBRIDGE_API bool			Startup();								// Must be called before any other httpbridge functions are called
	HTTPBRIDGE_API void			Shutdown();								// Must be called after all use of httpbridge
	HTTPBRIDGE_API const char*	VersionString(HttpVersion version);		// Returns HTTP/1.0 HTTP/1.1 HTTP/2.0
	HTTPBRIDGE_API const char*	StatusString(StatusCode status);		// "OK", "Not Found", etc
	HTTPBRIDGE_API uint64_t		siphash24(const void *src, unsigned long src_sz, const char key[16]); // Implementation of siphash-2-4
	HTTPBRIDGE_API size_t		Hash16B(uint64_t pair[2]);				// Hash 16 bytes with a decent (but not cryptographic) hash function.
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
	HTTPBRIDGE_API int64_t		atoi64(const char* s);
	HTTPBRIDGE_API int			TranslateVersionToFlatBuffer(hb::HttpVersion v);

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
		virtual SendResult	Send(const void* data, size_t size, size_t& sent) = 0;
		virtual RecvResult	Recv(size_t maxSize, void* data, size_t& bytesRead) = 0;
	};

	// Expose a compressor for compressing responses with gzip, deflate, etc.
	class HTTPBRIDGE_API ICompressor
	{
	public:
		static const int ResponseEncodingBufferSize = 60;
		virtual ~ICompressor();
		// Compress the body data. responseEncoding is a buffer of ResponseEncodingBufferSize bytes, which you must fill with your encoding string.
		// If you return false, then the body is sent uncompressed.
		virtual bool Compress(const char* acceptEncoding, const void* raw, size_t rawLen, void*& enc, size_t& encLen, char* responseEncoding) = 0;

		// Free a buffer that you returned from Compress
		virtual void Free(const char* acceptEncoding, void* enc) = 0;
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
		~Vector()
		{
			Clear();
		}

		// Throw away all memory (ie do not free), and reset all members to zero
		void Discard()
		{
			memset(this, 0, sizeof(*this));
		}

		void Clear()
		{
			for (INT i = 0; i < Count; i++)
				Items[i].T::~T();
			free(Items);
			Capacity = 0;
			Count = 0;
			Items = nullptr;
		}

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

	// 8-bit immutable string
	// This class was created in order to provide a decent interface into hb::Request, without
	// incurring the large number of allocs that std::string would impose on us.
	// Because ConstString holds an internal pointer into an hb::Request object, you should never
	// store these in a data structure that will outlive the Request. Basically, don't store them
	// anywhere other than on the stack. If you need to store a string, just use your favorite
	// string class to store it. Don't store a ConstString.
	class HTTPBRIDGE_API ConstString
	{
	private:
		// By having only a single member, which is a pointer, we can pass this
		// class directly to printf, when it is expecting a %s.
		const char* Data;
	public:
		ConstString(const char* str) : Data(str) {}

		// I tried this, in an attempt to prevent people accidentally storing a ConstString. Unfortunately this doesn't work,
		// because we need to return "ConstString" from hb::Request::Path(), for example, and in doing so, the compiler needs
		// to use the copy-constructor. Perhaps there is a clever C++ way around this, but I don't know of any.
		//ConstString(const ConstString&) = delete;
		//ConstString& operator=(const ConstString&) = delete;

		bool			StartsWith(const char* s) const;
		const char*		CStr() const { return Data; }		// This generally looks neater than a (const char*) cast

		bool operator==(const ConstString& b) const	{ return (Data == nullptr && b.Data == nullptr) || (Data != nullptr && b.Data != nullptr && strcmp(Data, b.Data) == 0); }
		bool operator!=(const ConstString& b) const	{ return !(*this == b); }

		operator const char*() const				{ return Data; }
	};

	inline bool operator==(const ConstString& a, const char* b)				{ return (a.CStr() == nullptr && b == nullptr) || (a.CStr() != nullptr && b != nullptr && strcmp(a.CStr(), b) == 0); }
	inline bool operator!=(const ConstString& a, const char* b)				{ return !(a == b); }

	inline bool operator==(const char* a, const ConstString& b)				{ return b == a; }
	inline bool operator!=(const char* a, const ConstString& b)				{ return b != a; }

	inline bool operator==(const ConstString& a, const std::string& b)		{ return ((a.CStr() == nullptr || a[0] == 0) && b.size() == 0) || (a.CStr() != nullptr && b.size() != 0 && strcmp(a.CStr(), b.c_str()) == 0); }
	inline bool operator!=(const ConstString& a, const std::string& b)		{ return !(a == b); }

	inline bool operator==(const std::string& a, const ConstString& b)		{ return b == a; }
	inline bool operator!=(const std::string& a, const ConstString& b)		{ return b != a; }

	// Byte buffer.
	class HTTPBRIDGE_API Buffer
	{
	public:
		uint8_t*	Data = nullptr;
		size_t		Count = 0;
		size_t		Capacity = 0;

					Buffer();
					~Buffer();

		void		Clear();
		uint8_t*	Preallocate(size_t n);
		void		EraseFromStart(size_t n);
		void		Write(const void* buf, size_t n);		// Uses GrowCapacityOrPanic()
		bool		TryWrite(const void* buf, size_t n);	// Returns false if allocation fails
		void		WriteStr(const char* s);				// Uses GrowCapacityOrPanic()
		void		WriteUInt64(uint64_t v);				// Uses GrowCapacityOrPanic()
		void		GrowCapacityOrPanic();
		bool		TryGrowCapacity();						// Returns false if allocation fails
		bool		IsPointerInside(const void* p) const { return ((uintptr_t) ((uint8_t*) p - Data)) < (uintptr_t) Capacity; }
		std::string AsString() const { return std::string((const char*) Data, Count); }
	};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

	class HTTPBRIDGE_API UrlPathParser
	{
	public:
		static void MeasurePath(const char* buf, int& rawLen, int& decodedLen);					// Returns the raw and decoded length of everything up to the first ? character
		static void DecodePath(const char* buf, char* decodedPath, bool addNullTerminator);		// Decodes everything up to the first ? character
	};

	class HTTPBRIDGE_API UrlQueryParser
	{
	public:
		const char* Src;
		int			P = 0;
		
				UrlQueryParser(const char* s) : Src(s) {}
		bool	Next(int& key, int& keyDecodedLen, int& val, int& valDecodedLen);
		void	DecodeKey(int start, char* key, bool addNullTerminator);
		void	DecodeVal(int start, char* val, bool addNullTerminator);
		static void	DecodeKey(const char* buf, int start, char* key, bool addNullTerminator);
		static void	DecodeVal(const char* buf, int start, char* val, bool addNullTerminator);
	};

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
	/* A backend that wants to receive HTTP/2 requests
	To connect to an upstream http-bridge server, call Connect("tcp", "host:port")
	*/
	class HTTPBRIDGE_API Backend
	{
	public:
		// Log is not owned by Backend (ie Backend will never delete Log).
		// Do not change this after Connect() has been called.
		Logger*				Log = nullptr;
		
		// Maximum number of bytes that will be allocated for 'ResendWhenBodyIsDone' requests. Total shared by all pending requests.
		std::atomic<size_t>	MaxWaitingBufferTotal;
		
		// Maximum size of a single request who's body will be automatically sent through 'ResendWhenBodyIsDone'. Set to zero to disable.
		// If a request is smaller or equal to MaxAutoBufferSize, but our total buffer quota (MaxWaitingBufferTotal) has been exceeded by the queue, then
		// the request will return with a Status503_Service_Unavailable.
		std::atomic<size_t>	MaxAutoBufferSize;

		// Initial size of receiving buffer, per request. If this value is large, then it becomes trivial for an attacker to cause your server
		// to exhaust all of it's memory pool, without transmitting much data. The initial buffer size is actually min(InitialBufferSize, Content-Length).
		std::atomic<size_t>	InitialBufferSize;

		// Optionally implement a response compressor. To use ICompressor, do not set the Content-Encoding header,
		// as it will be set automatically after calling your compressor.
		ICompressor*		Compressor = nullptr;

							Backend();
							~Backend();																// Destructor calls Close()
		bool				Connect(const char* network, const char* addr);
		bool				IsConnected();
		void				Close();
		SendResult			Send(Response& response);
		SendResult			Send(ConstRequestPtr request, StatusCode status);									// Convenience method for sending a simple response
		SendResult			SendBodyPart(ConstRequestPtr request, const void* body, size_t len, bool isFinal);	// Stream out the body of a response. isFinal is necessary for chunked responses; must be true on the final frame.
		bool				Recv(InFrame& frame);																// Returns true if a frame was received
		bool				ResendWhenBodyIsDone(InFrame& frame);												// Called by InFrame.ResendWhenBodyIsDone(). Returns false if out of memory.
		Logger*				AnyLog();
		void				UnregisterBufferedBytes(size_t bytes);												// Called by Request's destructor, if it has a buffered request.

	private:
		enum class FrameStatus
		{
			OK,
			StreamNotFound,
			OutOfMemory,
			URITooLong,
			BodyLongerThanContentLength,
		};
		// InternalRecvResult is guaranteed to be a strict super set of RecvResult.
		// The reason we keep these separate is to avoid confusing the user with enums
		// that they don't need to worry about.
		enum class InternalRecvResult
		{
			NoData	= RecvResult_NoData,
			Data	= RecvResult_Data,
			Closed	= RecvResult_Closed,
			BadFrame,
		};
		struct InternalRecvResponse
		{
			InternalRecvResult	Result;
			StatusCode			Status;
		};
		static const uint64_t ResponseBodyUninitialized = -1;
		struct RequestState
		{
			RequestPtr	Request;
			uint64_t	ResponseBodyRemaining;
			bool		IsResponseHeaderSent;
		};
		typedef std::unordered_map<StreamKey, RequestState> StreamToRequestMap;

		hb::HeaderCacheRecv* HeaderCacheRecv = nullptr;
		Logger				NullLog;
		hb::Buffer			RecvBuf;
		std::thread::id		ThreadId;

		std::mutex			TransportLock;					// Guards sending data out over Transport
		ITransport*			Transport = nullptr;

		std::mutex			CurrentRequestLock;				// Guards access to the map, as well as the RequestState objects stored inside the map
		StreamToRequestMap	CurrentRequests;

		std::atomic<size_t>	BufferedRequestsTotalBytes;		// Total number of body bytes allocated for "BufferedRequests"

		InternalRecvResponse	RecvInternal(InFrame& inframe);
		void					RequestFinished(const StreamKey& key);
		bool					Connect(ITransport* transport, const char* addr);
		FrameStatus				UnpackHeader(const httpbridge::TxFrame* txframe, InFrame& inframe);
		FrameStatus				UnpackBody(const httpbridge::TxFrame* txframe, InFrame& inframe);
		FrameStatus				UnpackControlFrame(const httpbridge::TxFrame* txframe, InFrame& inframe);
		size_t					TotalHeaderBlockSize(const httpbridge::TxFrame* frame);
		void					LogAndPanic(const char* msg);
		void					SendResponse(RequestPtr request, StatusCode status);
		void					SendResponse(uint64_t channel, uint64_t stream, StatusCode status);
		RequestState*			GetRequest(uint64_t channel, uint64_t stream);
		RequestState*			GetRequestOrDie(uint64_t channel, uint64_t stream);
		static StreamKey		MakeStreamKey(uint64_t channel, uint64_t stream);
		static StreamKey		MakeStreamKey(ConstRequestPtr request);
		static StreamKey		MakeStreamKey(const httpbridge::TxFrame* txframe);
	};

	/* HTTP request

	Note that header keys and values are always null terminated. The HTTP/2 spec allows headers
	to contain arbitrary binary data, so you may be missing something by not using the header accessor functions
	that allow you to read through null characters, but that's your choice.

	A Request object is typically used as a shared_ptr, because the lifetime of requests varies greatly,
	and other solutions are tricky to get right. When implementing httpbridge services, you must always
	use RequestPtr or ConstRequestPtr (aka shared_ptr<Request> and shared_ptr<const Request>) when storing
	a Request object. shared_ptr's reference counting only works if there is an unbroken chain of shared_ptr
	objects from start to finish. If you use a Request* object somewhere along the line, then the chain is
	broken. So just stick to using RequestPtr or ConstRequestPtr everywhere, and things will just work.

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
		bool					IsBuffered = false;
		HttpVersion				Version = HttpVersion10;
		uint64_t				Channel = 0;
		uint64_t				Stream = 0;

		// You can use UserData to store any information that you want. httpbridge ignores this.
		// If you need to be notified when a Request object is destroyed, then you can also
		// populate OnDestroy with a function that will be called when the Request object
		// is deleted.
		void*					UserData = nullptr;
		RequestDestroyCallback	OnDestroy = nullptr; // This is never touched by the httpbridge framework. Intended to be used for cleaning up UserData.

		// The total length of the body of this request.
		// If the Content-Length header is given, then this is a cache of that value.
		// If this is a chunked upload, then ContentLength is -1, and the final frame
		// will be marked with IsLast = true.
		uint64_t				ContentLength = 0;
		
		// If IsBuffered = true, then BodyBuffer stores the entire body
		hb::Buffer				BodyBuffer;
		
								Request();
								~Request();

		// Set a header. The Request object now owns headerBlock, and will Free() it in the destructor
		// The header block must have a terminal pair inside in order to make iteration easy.
		void					Initialize(hb::Backend* backend, HttpVersion version, uint64_t channel, uint64_t stream, int32_t headerCount, const void* headerBlock);

		// Create a mocked request object, to use in tests
		static RequestPtr		CreateMocked(const std::string& method, const std::string& uri, const std::unordered_map<std::string, std::string>& headers, const std::string& body = "");
		
		StreamState				State() const;								// If State is not Active, then you shouldn't be sending any frames
		void					SetState(StreamState newState);				// Atomically set the state, but once in Aborted state, always stay Aborted.

		// This is called automatically by Backend. Returns false if an element is too long
		bool					ParseURI();
		
		ConstString				Method() const;		// Returns the method, such as GET or POST
		ConstString				URI() const;		// Returns the raw URI of the request

		ConstString				Path() const;		// Returns the Path of the request

		ConstString				Query(const char* key) const;		// Returns the first URL query parameter for the given key, or NULL if none found
		std::string				QueryStr(const char* key) const;	// Returns the first URL query parameter for the given key, or an empty string if none found
		int64_t  				QueryInt64(const char* key) const;	// Returns the first URL query parameter for the given key, as int64, or 0 if none found
		double  				QueryDouble(const char* key) const;	// Returns the first URL query parameter for the given key, as double, or 0 if none found

		// Use NextQuery to iterate over the query parameters.
		// Returns zero if there are no more items.
		// Example: for (auto iter = req->NextQuery(0, qkey, qval); iter != 0; iter = req->NextQuery(iter, qkey, qval)) {...}
		int32_t					NextQuery(int32_t iterator, const char*& key, const char*& value) const;

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
		static const int			NumPseudoHeaderLines = 1;
		static const uint16_t		EndOfQueryMarker = 65535;
		int32_t						_HeaderCount = 0;
		const uint8_t*				_HeaderBlock = nullptr;		// First HeaderLine[] array and then the headers themselves
		char*						_CachedURI = nullptr;
		std::atomic<StreamState>	_State;
	};

	/* A frame received from the server

	The first thing to look at in an incoming frame is the Type. If Type is Data, then this
	frame containers HTTP headers and/or body. If Type is not Data, then this is a control frame
	which can be one of Abort, Pause, or Resume.
	*/
	class HTTPBRIDGE_API InFrame
	{
	public:
		RequestPtr		Request;
		FrameType		Type;				// Usually you're only interested in Data frames, but you may be interested in the others - ie Abort, Pause, Resume, but those states are also available in Request.
		bool			IsHeader;			// Is this the first frame of the request? For a request with an empty body, IsHeader and IsLast are both true.
		bool			IsLast;				// Is this the last frame of the request?

		uint8_t*		BodyBytes;			// Body bytes in this frame, unless the frame is being buffered, in which case BodyBytes is null.
		size_t			BodyBytesLen;		// Length of BodyBytes, unless the frame is being buffered, in which case BodyBytesLen is 0.

		InFrame();
		~InFrame();

		void	Reset();					// Reset the frame object to it's default state.
		bool	ResendWhenBodyIsDone();		// Calls Request->Backend->ResentWhenBodyIsDone(this)

	private:
		InFrame(const InFrame&) = delete;
		InFrame& operator= (const InFrame&) = delete;
	};

	/* HTTP Response (or part of a response)

	If you don't set a Content-Length header, then Backend implicitly adds a Content-Length header equal to the size
	of the body data inside the Response object. If that size is zero, then no implicit header is added.

	If you want to stream your response out, then you must call AddHeader_ContentLength (which is just a convenience method
	which does AddHeader("Content-Length", ...)). If you don't do that, then httpbridge will assume that the body
	of your response consists solely of what is present in the first response frame.

	To send a chunked response, set Content-Length to -1 (eg using AddHeader_ContentLength(-1)).
	Then, on your last frame, set IsFinalChunkedFrame = true.

	A Response is either one of these two things:
	1. A Header response, which must be the first response frame sent on a stream. A header response can optionally contain part, or all, of the response body.
	2. A BodyPart response, which may not contain any headers, and is strictly just a chunk of bytes that
		forms the next part of the body being transmitted.
	*/
	class HTTPBRIDGE_API Response
	{
	public:
		typedef uint32_t ByteVectorOffset;

		hb::Backend*		Backend = nullptr;
		ConstRequestPtr		Request = nullptr;
		HttpVersion			Version = HttpVersion10;
		uint64_t			Channel = 0;
		uint64_t			Stream = 0;
		StatusCode			Status = Status200_OK;
		bool				IsFinalChunkedFrame = false;

		Response();
		Response(ConstRequestPtr request, StatusCode status = Status200_OK);
		Response(hb::Backend* backend, HttpVersion version, uint64_t channel, uint64_t stream, StatusCode status = Status200_OK);
		Response(Response&& b);

		Response& operator=(Response&& b);

		// Return a Response object that contains a portion of the response body
		// If this is a chunked response (ie Content-Length = -1), and this is the final frame of the response,
		// then isFinal must be true.
		static Response MakeBodyPart(ConstRequestPtr request, const void* part, size_t len, bool isFinal);

		~Response();

		bool			IsOK() const																		{ return Status == Status200_OK; }
		void			SetStatus(StatusCode status);
		void			SetStatusAndBody(StatusCode status, const char* body);								// Sets the status and the body.
		void			SetStatusAndBody(StatusCode status, const std::string& body);						// Sets the status and the body.
		void			AddHeader(const char* key, const char* value);										// Add a header
		void			AddHeader(int32_t keyLen, const char* key, int32_t valLen, const char* value);		// Add a header
		void			AddHeader_ContentLength(uint64_t contentLength);									// Convenience method to add a Content-Length header
		void			SetBody(const void* body, size_t len);												// Set body. Panics if called more than once.
		SendResult		Send();																				// Call Backend->Send(this)

		int32_t			HeaderCount() const { return HeaderIndex.Size() / 2; }
		size_t			BodyBytes() const { return BodyLength; }

		// Fetch a header by name.
		// Returns null if the header does not exist.
		const char*		HeaderByName(const char* name, int nth = 0) const;
		
		// Returns true if a header exists
		bool			HasHeader(const char* name) const;

		// Both key and val are guaranteed to be null terminated
		void			HeaderAt(int32_t index, int32_t& keyLen, const char*& key, int32_t& valLen, const char*& val) const;
		
		// Both key and val are guaranteed to be null terminated
		void			HeaderAt(int32_t index, const char*& key, const char*& val) const;

		void			FinishFlatbuffer(void*& buf, size_t& len, bool isLast);
		void			SerializeToHttp(void*& buf, size_t& len);											// The returned 'buf' must be freed with hb::Free()
		void			GetBody(const void*& buf, size_t& len) const;										// Retrieve a pointer to the Body buffer, as well as it's size
		std::string		GetBody() const;																	// Retrieve a copy of the Body buffer.

	private:
		flatbuffers::FlatBufferBuilder*		FBB = nullptr;
		ByteVectorOffset					BodyOffset = 0;
		uint32_t							BodyLength = 0;
		bool								IsFlatBufferBuilt = false;
		
		// Our header keys and values are always null terminated. This is necessary in order
		// to provide a consistent API between Request and Response objects.
		Vector<uint32_t>					HeaderIndex;
		Vector<char>						HeaderBuf;

		void	Free();
		void	CreateBuilder();
		int32_t	HeaderKeyLen(int32_t i) const;
		int32_t	HeaderValueLen(int32_t i) const;
		void	SetBodyInternal(const void* body, size_t len, bool isFullBody);
	};
}

#endif