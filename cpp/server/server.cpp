#include "../http-bridge.h"
#include "../http-bridge_generated.h"
#include "server.h"
#include "http11/http11_parser.h"
#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace hb
{
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
static const uint32_t	Infinite = INFINITE;
static const int		ErrWOULDBLOCK = WSAEWOULDBLOCK;
static const int		ErrTIMEOUT = WSAETIMEDOUT;
static const int		ErrSEND_BUFFER_FULL = WSAENOBUFS;	// untested
static const int		ErrSOCKET_ERROR = SOCKET_ERROR;
#else
static const uint32_t	Infinite = 0xFFFFFFFF;
static const int		ErrWOULDBLOCK = EWOULDBLOCK;
static const int		ErrTIMEOUT = ETIMEDOUT;
static const int		ErrSEND_BUFFER_FULL = EMSGSIZE;		// untested
static const int		ErrSOCKET_ERROR = -1;
#define closesocket close
#endif

// one millisecond, in nanoseconds
static const int MillisecondNS = 1000 * 1000;

static uint64_t uatoi64(const char* s, size_t len)
{
	uint64_t v = 0;
	for (int i = 0; i < len; i++)
		v = v * 10 + s[i];
	return v;
}

// We use this wrapper function to keep http_parser out of the global namespace
static http_parser* GetParser(Server::Channel& c)
{
	return (http_parser*) c.Parser;
}

Server::Server()
{
	Log = stdout;
}

Server::~Server()
{
	Close();
}

bool Server::ListenAndRun(const char* addr, uint16_t httpPort, uint16_t backendPort)
{
	StopSignal = 0;
	NextChannelID = 1;

	bool httpOK = CreateSocketAndListen(HttpListenSocket, addr, httpPort);
	bool backendOK = CreateSocketAndListen(BackendListenSocket, addr, backendPort);
	if (!(httpOK && backendOK))
	{
		Close();
		return false;
	}

	fprintf(Log, "http listening on %d\n", (int) httpPort);
	fprintf(Log, "backend listening on %d\n", (int) backendPort);

	Process();
	Close();
	return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// http_parser callbacks

void Server::cb_http_field(void *data, const char *field, size_t flen, const char *value, size_t vlen)
{
	Channel* c = (Channel*) data;
	c->Headers.push_back({ std::string(field, flen), std::string(value, vlen) });
	if (flen == 14 && strncmp(field, "Content-Length", 14) == 0)
		c->ContentLength = uatoi64(value, vlen);
}

void Server::cb_request_method(void *data, const char *at, size_t length)
{
	Channel* c = (Channel*) data;
	c->Method = std::string(at, length);
}

void Server::cb_request_uri(void *data, const char *at, size_t length)
{
	Channel* c = (Channel*) data;
	c->URI = std::string(at, length);
}

void Server::cb_fragment(void *data, const char *at, size_t length)
{
}

void Server::cb_request_path(void *data, const char *at, size_t length)
{
}

void Server::cb_query_string(void *data, const char *at, size_t length)
{
}

void Server::cb_http_version(void *data, const char *at, size_t length)
{
	Channel* c = (Channel*) data;
	if (length == 8 && strncmp(at, "HTTP/1.0", length))
		c->Version = HttpVersion10;
	else if (length == 8 && strncmp(at, "HTTP/1.1", length))
		c->Version = HttpVersion11;
	else
		c->Version = HttpVersion2;
}

static void WriteHeaderBlockItem(uint8_t* hblock, Request::HeaderLine* index, int& ipos, uint32_t& bufpos, size_t keyLen, const char* key, size_t valLen, const char* val)
{
	index[ipos].KeyStart = bufpos;
	index[ipos].KeyLen = (uint32_t) keyLen;
	memcpy(hblock + bufpos, key, keyLen);
	hblock[bufpos + keyLen] = 0;
	memcpy(hblock + bufpos + keyLen + 1, val, valLen);
	hblock[bufpos + keyLen + 1 + valLen] = 0;
	ipos++;
	bufpos += (uint32_t) (keyLen + valLen + 2);
}

void Server::cb_header_done(void *data, const char *at, size_t length)
{
	Channel* c = (Channel*) data;
	size_t total = sizeof(uint32_t) * 2 * (c->Headers.size() + 2);	// +1 header line for terminator, and +1 header line for pseudo-first-header-line
	total += c->Method.length() + c->URI.length() + 2;				// +2 for null terminators
	for (size_t i = 0; i < c->Headers.size(); i++)
		total += c->Headers[i].first.length() + c->Headers[i].second.length() + 2;

	uint8_t* hblock = (uint8_t*) Alloc(total, nullptr);
	Request::HeaderLine* index = (Request::HeaderLine*) hblock;

	int ipos = 0;
	uint32_t bufpos = (uint32_t) (sizeof(index[0]) * (c->Headers.size() + 2));
	
	// write pseudo-header (special first header line)
	WriteHeaderBlockItem(hblock, index, ipos, bufpos, c->Method.length(), c->Method.c_str(), c->URI.length(), c->URI.c_str());
	
	// write regular headers
	for (const auto& header : c->Headers)
		WriteHeaderBlockItem(hblock, index, ipos, bufpos, header.first.length(), header.first.c_str(), header.second.length(), header.second.c_str());

	// write terminal header
	index[ipos].KeyStart = bufpos;
	index[ipos].KeyLen = 0;

	HTTPBRIDGE_ASSERT(bufpos == total);

	c->Request.InitHeader(nullptr, c->Version, 0, 0, c->ContentLength, (int32_t) c->Headers.size() + 1, hblock);
}


// http_parser callbacks (end)
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool Server::CreateSocketAndListen(socket_t& sock, const char* addr, uint16_t port)
{
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == InvalidSocket)
	{
		fprintf(Log, "socket() failed: %d\n", LastError());
		return false;
	}

	sockaddr_in service;
	service.sin_family = AF_INET;
	inet_pton(AF_INET, addr, &service.sin_addr);
	service.sin_port = htons(port);

	int err = bind(sock, (sockaddr*) &service, sizeof(service));
	if (err == ErrSOCKET_ERROR)
	{
		fprintf(Log, "bind() on %s:%d failed: %d\n", addr, (int) port, LastError());
		return false;
	}

	if (listen(sock, SOMAXCONN) == ErrSOCKET_ERROR)
	{
		fprintf(Log, "listen() on %s:%d failed: %d\n", addr, (int) port, LastError());
		return false;
	}

	return true;
}

void Server::AcceptHttp()
{
	sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	socket_t newSock = accept(HttpListenSocket, (sockaddr*) &addr, &addr_len);
	if (newSock == InvalidSocket)
	{
		fprintf(Log, "http accept() failed: %d\n", LastError());
		return;
	}
	Channel* chan = new Channel();
	chan->Socket = newSock;
	chan->ChannelID = NextChannelID++;
	auto parser = new http_parser();
	http_parser_init(parser);
	parser->data = chan;
	parser->http_field = cb_http_field;
	parser->request_method = cb_request_method;
	parser->request_uri = cb_request_uri;
	parser->fragment = cb_fragment;
	parser->request_path = cb_request_path;
	parser->query_string = cb_query_string;
	parser->http_version = cb_http_version;
	parser->header_done = cb_header_done;
	chan->Parser = parser;
	Channels.push_back(chan);
	fprintf(Log, "[%d] socked opened\n", (int) chan->Socket);
}

void Server::AcceptBackend()
{
	sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	BackendSock = accept(BackendListenSocket, (sockaddr*) &addr, &addr_len);
	if (BackendSock == InvalidSocket)
		fprintf(Log, "backend accept() failed: %d\n", LastError());
}

void Server::Process()
{
	while (StopSignal.load() == 0)
	{
		fd_set readable;
		socket_t maxSocket = HttpListenSocket;
		FD_ZERO(&readable);
		FD_SET(HttpListenSocket, &readable);
		if (BackendSock == InvalidSocket)
		{
			FD_SET(BackendListenSocket, &readable);
			maxSocket = BackendListenSocket > maxSocket ? BackendListenSocket : maxSocket;
		}
		for (auto c : Channels)
		{
			FD_SET(c->Socket, &readable);
			maxSocket = c->Socket > maxSocket ? c->Socket : maxSocket;
		}
		timeval to;
		to.tv_sec = 0;
		to.tv_usec = 1000 * 1000; // 1000 milliseconds
		int n = select((int) (maxSocket + 1), &readable, nullptr, nullptr, &to);
		if (n <= 0)
			continue;

		// This is necesary on unix, because select() will abort on Ctrl+C, and then accept will block
		// [Check the above statement. It was written before changing the condition from (n == 0) to (n <= 0)]
		if (StopSignal.load() != 0)
			break;

		if (FD_ISSET(HttpListenSocket, &readable) && Channels.size() < MaxChannels)
			AcceptHttp();

		if (FD_ISSET(BackendListenSocket, &readable))
			AcceptBackend();

		if (FD_ISSET(BackendSock, &readable))
			ReadFromBackend();

		for (size_t i = 0; i < Channels.size(); i++)
		{
			Channel* c = Channels[i];
			if (FD_ISSET(c->Socket, &readable))
			{
				if (!ReadFromChannel(*c))
				{
					closesocket(c->Socket);
					Channels.erase(Channels.begin() + i);
					delete c;
					i--;
				}
			}
		}
	}
}

bool Server::ReadFromChannel(Channel& c)
{
	char buf[4096];
	auto parser = GetParser(c);
	int nread = recv(c.Socket, buf, sizeof(buf), 0);
	if (nread == 0)
	{
		// socket is closed
		fprintf(Log, "[%d] socket closed\n", (int) c.Socket);
		return false;
	}
	else if (nread > 0)
	{
		http_parser_execute(parser, buf, nread, parser->nread);
		if (!!http_parser_has_error(parser))
		{
			fprintf(Log, "[%d] http parser error\n", (int) c.Socket);
			return false;
		}
		if (!!http_parser_is_finished(parser))
		{
			HandleRequest(c);
			ResetChannel(c);
		}
		return true;
	}
	else
	{
		fprintf(Log, "[%d] recv error (%d)\n", (int) c.Socket, LastError());
		return false;
	}
}

void Server::ReadFromBackend()
{
	int maxRead = 65536;
	uint8_t* dst = BackendRecvBuf.Preallocate(maxRead);
	int nread = recv(BackendSock, (char*) dst, maxRead, 0);
	if (nread > 0)
	{
		BackendRecvBuf.Count += nread;
		if (BackendRecvBuf.Count > 4)
		{
			uint32_t frameSize = Read32LE(BackendRecvBuf.Data);
			if (BackendRecvBuf.Count >= frameSize + 4)
			{
				// We have a frame
				HandleBackendFrame(frameSize, BackendRecvBuf.Data + 4);
				BackendRecvBuf.EraseFromStart(4 + frameSize);
			}
		}
	}
	else
	{
		if (nread == 0)
			fprintf(Log, "[%d] backend socket closed\n", (int) BackendSock);
		else
			fprintf(Log, "[%d] backend socket recv error: %d\n", (int) BackendSock, LastError());
		closesocket(BackendSock);
		BackendSock = InvalidSocket;
	}
}

void Server::HandleBackendFrame(uint32_t frameSize, const void* frameBuf)
{
	auto frame = httpbridge::GetTxFrame(frameBuf);
	Channel* c = nullptr;
	for (auto tc : Channels)
	{
		if (tc->ChannelID == frame->channel())
		{
			c = tc;
			break;
		}
	}
	if (c == nullptr)
		return;

	if (frame->frametype() == httpbridge::TxFrameType_Header)
	{
		const char* CRLF = "\r\n";

		HttpSendBuf.Count = 0;
		
		// status line
		HttpSendBuf.Write("HTTP/1.1 ", 9);
		auto statusLine = frame->headers()->Get(0);
		const char* statusStr = (const char*) statusLine->key()->Data();
		int statusStrLen = (int) statusLine->key()->size();
		HttpSendBuf.Write(statusStr, statusStrLen);

		hb::StatusCode status = (hb::StatusCode) uatoi64(statusStr, statusStrLen);
		HttpSendBuf.WriteStr(StatusString(status));
		HttpSendBuf.WriteStr(CRLF);

		// headers
		for (uint32_t i = 0; i < frame->headers()->size(); i++)
		{
			auto header = frame->headers()->Get(i);
			HttpSendBuf.Write(header->key()->Data(), header->key()->size());
			HttpSendBuf.WriteStr(": ");
			HttpSendBuf.Write(header->value()->Data(), header->value()->size());
			HttpSendBuf.WriteStr(CRLF);
		}
		HttpSendBuf.WriteStr(CRLF);

		// body
		HttpSendBuf.Write(frame->body()->Data(), frame->body()->size());
	}
	else if (frame->frametype() == httpbridge::TxFrameType_Body)
	{
		HttpSendBuf.Write(frame->body()->Data(), frame->body()->size());
	}
	else
	{
		HTTPBRIDGE_PANIC("Unrecognized frame type");
	}
}

void Server::HandleRequest(Channel& c)
{
	fprintf(Log, "[%d] request [%s]\n", (int) c.Socket, c.URI.c_str());

	Response resp;
	Handler->HandleRequest(c.Request, resp);

	size_t len = 0;
	void* buf = nullptr;
	resp.SerializeToHttp(len, buf);
	size_t pos = 0;
	while (pos != len)
	{
		size_t trywrite = len - pos;
		if (trywrite > 1024 * 1024)
			trywrite = 1024 * 1024;
		int nwrite = send(c.Socket, (char*) buf + pos, (int) trywrite, 0);
		if (nwrite > 0)
		{
			pos += nwrite;
		}
		else if (nwrite < 0)
		{
			fprintf(Log, "[%d] send failed: %d\n", (int) c.Socket, LastError());
			break;
		}
	}

	Free(buf);
}

void Server::ResetChannel(Channel& c)
{
	http_parser_init(GetParser(c));
	c.ContentLength = 0;
	c.Headers.clear();
	c.Method = "";
	c.Request.Reset();
	c.URI = "";
	c.Version = hb::HttpVersion10;
}

void Server::Close()
{
	CloseSocket(HttpListenSocket);
	CloseSocket(BackendListenSocket);
}

void Server::CloseSocket(socket_t& sock)
{
	if (sock != InvalidSocket)
	{
		int err = closesocket(sock);
		if (err == ErrSOCKET_ERROR)
			fprintf(Log, "[%d] closesocket() failed: %d\n", (int) sock, LastError());
	}
	sock = InvalidSocket;
}

int Server::LastError()
{
#ifdef HTTPBRIDGE_PLATFORM_WINDOWS
	return (int) WSAGetLastError();
#else
	return errno;
#endif
}

}