//######################################################
//
// This is used by unit tests
//
//######################################################
#include "http-bridge.h"
#include <stdio.h>

struct RequestKey
{
	uint64_t Channel;
	uint64_t Stream;
	bool operator==(const RequestKey& b) const { return Channel == b.Channel && Stream == b.Stream; }
};

namespace std
{
	template <>
	class hash<RequestKey>
	{
	public:
		size_t operator()(const RequestKey& k) const
		{
			return hb::Hash16B((uint64_t*) &k);
		}
	};
}

// Maintain state of current requests. We need to do this so that we can verify unbuffered
// requests, where we're streaming the bodies in frame by frame.
class Server
{
public:
	hb::Backend*	Backend = nullptr;
	bool			Stop = false;

	void HandleFrame(hb::InFrame& inframe)
	{
		auto match = [&inframe](const char* prefix) { return strstr(inframe.Request->Path(), prefix) == inframe.Request->Path(); };

		LocalRequest* lr = nullptr;
		if (inframe.IsHeader)
			lr = StartRequest(inframe);
		else
			lr = Requests.at(RequestKey{ inframe.Request->Channel, inframe.Request->Stream });

		if (match("/echo"))			HttpEcho(inframe, lr);
		else if (match("/control"))	HttpControl(inframe, lr);
		else if (match("/ping"))
		{
			Backend->Send(inframe.Request, hb::Status200_OK);
		}
		else if (match("/stop"))
		{
			Stop = true;
			Backend->Send(inframe.Request, hb::Status200_OK);
		}
		else
		{
			Backend->Send(inframe.Request, hb::Status404_Not_Found);
		}

		if (inframe.IsLast || inframe.IsAborted)
			EndRequest(inframe);
	}

private:
	struct LocalRequest
	{
		hb::Buffer Body;
	};
	std::unordered_map<RequestKey, LocalRequest*> Requests;

	// Echos the body back
	void HttpEcho(hb::InFrame& inframe, LocalRequest* lr)
	{
		if (!inframe.Request->IsBuffered)
			lr->Body.Write(inframe.BodyBytes, inframe.BodyBytesLen);

		if (inframe.IsLast)
		{
			const hb::Buffer& body = inframe.Request->IsBuffered ? inframe.Request->BodyBuffer : lr->Body;
			hb::Response resp(inframe.Request);
			resp.SetBody(body.Count, body.Data);
			resp.Send();
		}
	}

	void HttpControl(hb::InFrame& inframe, LocalRequest* lr)
	{
		auto buffer_max = inframe.Request->Query("MaxAutoBufferSize");
		if (buffer_max != nullptr)
			Backend->MaxAutoBufferSize = atoi(buffer_max);
		Backend->Send(inframe.Request, hb::Status200_OK);
	}

	LocalRequest* StartRequest(hb::InFrame& inframe)
	{
		auto lr = new LocalRequest();
		Requests[RequestKey{ inframe.Request->Channel, inframe.Request->Stream }] = lr;
		return lr;
	}

	void EndRequest(hb::InFrame& inframe)
	{
		auto key = RequestKey{ inframe.Request->Channel, inframe.Request->Stream };
		auto lr = Requests.at(key);
		delete lr;
		Requests.erase(key);
	}
};

int main(int argc, char** argv)
{
	hb::Startup();
	
	hb::Backend backend;
	Server server;
	server.Backend = &backend;

	while (!server.Stop)
	{
		if (!backend.IsConnected())
		{
			if (!backend.Connect("tcp", "127.0.0.1:8081"))
				hb::SleepNano(10 * 1000 * 1000);
		}

		hb::InFrame inframe;
		if (backend.Recv(inframe))
			server.HandleFrame(inframe);
	}

	// This is polite. If we don't do this, then our TCP socket can be closed before we've finished transmitting
	// our final reply, and then the Go test server stalls while waiting for us.
	hb::SleepNano(10 * 1000 * 1000);

	hb::Shutdown();

	return 0;
}


