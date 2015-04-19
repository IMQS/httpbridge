#pragma once
#ifndef HTTPBRIDGE_SERVER_H_INCLUDED
#define HTTPBRIDGE_SERVER_H_INCLUDED

#include "../http-bridge.h"

#include <thread>
#include <atomic>

#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
#include <Ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <stdarg.h>
#include <unistd.h>
#endif

namespace hb
{
class Server;
class IServerHandler;

// An object that responds to Server callbacks
class HTTPBRIDGE_API IServerHandler
{
public:
	virtual void HandleRequest(Request& req, Response& resp) = 0;
};

/* A minimal HTTP/1.1 server.

Usage

	* Set Listener to your own implementation of IServerListener
	* Call ListenAndRun()
	* From a signal handler, or another thread, call Stop(), which will cause ListenAndRun() to return

Threading model

	We do not launch any threads. The server is single-threaded.

Limitations

	Maximum of 62 simultaneous connections.

*/
class HTTPBRIDGE_API Server
{
public:
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
	typedef SOCKET socket_t;
	static const socket_t	InvalidSocket = INVALID_SOCKET;
#else
	typedef int socket_t;
	static const socket_t	InvalidSocket = (socket_t) (~0);
#endif

	// A socket talking HTTP
	struct Channel
	{
		socket_t		Socket = InvalidSocket;
		void*			Parser = nullptr;
		hb::Request		Request;
		uint64_t		ChannelID = 0;

		// Details of the request
		std::string		Method;
		std::string		URI;
		uint64_t		ContentLength = 0;
		HttpVersion		Version = HttpVersion10;
		std::vector<std::pair<std::string, std::string>> Headers;
	};

	// This limit is inherent to the size of FD_SETSIZE on Windows. I don't know if other OSes have the same limit,
	// but if you want a real HTTP server, you've come to the wrong place. The total we can select on is 64, but
	// but we also need two slots to see if we have a new channel that can be accept()'ed (one for HTTP and one for Backend).
	static const int MaxChannels = 62;

	IServerHandler*			Handler;
	std::atomic<uint32_t>	StopSignal;	// When this is non-zero, then ListenAndRun will exit
	FILE*					Log;		// All logs are printed here. Default is stdout.

	Server();
	~Server();

	// Addr is the address to listen on, such as "127.0.0.1", or "0.0.0.0" to listen on all addresses.
	// If we manage to listen on both ports, then this function only returns when Stop() is called
	bool ListenAndRun(const char* addr, uint16_t httpPort, uint16_t backendPort);
	
	void Stop()							{ StopSignal = 1;  }

private:
	socket_t				HttpListenSocket = InvalidSocket;
	socket_t				BackendListenSocket = InvalidSocket;
	std::thread				AcceptThread;
	uint64_t				NextChannelID;

	std::vector<Channel*>	Channels;						// HTTP Channels
	Buffer					HttpSendBuf;					// Buffer of an HTTP response

	socket_t				BackendSock = InvalidSocket;	// We only support a single backend connection
	Buffer					BackendRecvBuf;					// Buffer until we have a whole httpbridge frame

	bool CreateSocketAndListen(socket_t& sock, const char* addr, uint16_t port);
	void AcceptHttp();
	void AcceptBackend();
	void Process();
	bool ReadFromChannel(Channel& c);	// Returns false if we must close the socket
	void ReadFromBackend();
	void HandleBackendFrame(uint32_t frameSize, const void* frameBuf);
	void HandleRequest(Channel& c);
	void ResetChannel(Channel& c);		// Reset channel, so that it can serve another request on the same socket
	void Close();
	void CloseSocket(socket_t& sock);

	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// http_parser callbacks
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	static void cb_http_field(void *data, const char *field, size_t flen, const char *value, size_t vlen);
	static void cb_request_method(void *data, const char *at, size_t length);
	static void cb_request_uri(void *data, const char *at, size_t length);
	static void cb_fragment(void *data, const char *at, size_t length);
	static void cb_request_path(void *data, const char *at, size_t length);
	static void cb_query_string(void *data, const char *at, size_t length);
	static void cb_http_version(void *data, const char *at, size_t length);
	static void cb_header_done(void *data, const char *at, size_t length);

	static int LastError();

};

}

#endif // HTTPBRIDGE_SERVER_H_INCLUDED