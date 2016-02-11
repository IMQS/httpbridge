//######################################################
//
// This is used by unit tests
//
//######################################################
#include "http-bridge.h"
#include <stdio.h>
#include <thread>
#include <mutex>
#include <algorithm>

const int NumWorkerThreads = 4;

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
	hb::Backend*				Backend = nullptr;
	std::atomic<bool>			Stop;

	Server()
	{
		Stop = false;
	}

	void HandleFrame(hb::InFrame& inframe)
	{
		auto prefix_match = [&inframe](const char* prefix) { return strstr(inframe.Request->Path(), prefix) == inframe.Request->Path(); };

		LocalRequest* lr = nullptr;
		if (inframe.IsHeader)
			lr = StartRequest(inframe);
		else
			lr = Requests.at(RequestKey{ inframe.Request->Channel, inframe.Request->Stream });

		if (prefix_match("/control"))
		{
			HttpControl(inframe, lr);
		}
		else if (prefix_match("/ping"))
		{
			Backend->Send(inframe.Request, hb::Status200_OK);
		}
		else if (prefix_match("/timeout"))
		{
			// Go server sets it's timeout to 50 milliseconds, so 100 is plenty
			hb::SleepNano(100 * 1000 * 1000);
			Backend->Send(inframe.Request, hb::Status200_OK);
		}
		else if (prefix_match("/stop"))
		{
			Stop = true;
			Backend->Send(inframe.Request, hb::Status200_OK);
		}
		else if (prefix_match("/echo-thread"))
		{
			HttpEchoThread(inframe, lr);
		}
		else if (prefix_match("/echo"))
		{
			HttpEcho(inframe, lr);
		}
		else
		{
			Backend->Send(inframe.Request, hb::Status404_Not_Found);
		}

		if (inframe.IsLast || inframe.IsAborted)
			EndRequest(inframe);
	}

	void StartThreads()
	{
		Threads.resize(NumWorkerThreads);
		for (size_t i = 0; i < Threads.size(); i++)
			Threads[i] = std::thread(WorkerThread, this);
	}

	void WaitForThreadsToDie()
	{
		for (auto& t : Threads)
			t.join();
		Threads.clear();
	}

private:
	struct LocalRequest
	{
		hb::Buffer	Body;
		size_t		MaxTransmitBodyChunkSize = 0;
	};
	std::unordered_map<RequestKey, LocalRequest*>	Requests;
	std::vector<std::thread>						Threads;
	std::vector<hb::RequestPtr>						ThreadRequestQueue;
	std::mutex										ThreadRequestQueueLock;

	// Echos the body back
	void HttpEcho(hb::InFrame& inframe, LocalRequest* lr)
	{
		if (inframe.IsHeader && inframe.Request->Query("MaxTransmitBodyChunkSize") != nullptr)
			lr->MaxTransmitBodyChunkSize = atoi(inframe.Request->Query("MaxTransmitBodyChunkSize"));

		if (!inframe.Request->IsBuffered)
			lr->Body.Write(inframe.BodyBytes, inframe.BodyBytesLen);

		if (inframe.IsLast)
		{
			const hb::Buffer& body = inframe.Request->IsBuffered ? inframe.Request->BodyBuffer : lr->Body;
			SendResponseInChunks(inframe.Request, hb::Status200_OK, body.Data, body.Count, lr->MaxTransmitBodyChunkSize);
		}
	}

	// Echos the body back, but from one of the worker threads
	void HttpEchoThread(hb::InFrame& inframe, LocalRequest* lr)
	{
		if (inframe.IsLast)
		{
			ThreadRequestQueueLock.lock();
			ThreadRequestQueue.push_back(inframe.Request);
			ThreadRequestQueueLock.unlock();
		}
	}

	void HttpControl(hb::InFrame& inframe, LocalRequest* lr)
	{
		auto& buffer_max = inframe.Request->Query("MaxAutoBufferSize");
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

	static void SendResponseInChunks(hb::ConstRequestPtr request, hb::StatusCode status, const void* body, size_t bodyLen, size_t maxBodyChunkSize)
	{
		hb::Response head(request, status);
		head.AddHeader_ContentLength(bodyLen);
		auto res = head.Send();
		if (res != hb::SendResult_All)
			printf("Send head failed!\n");

		size_t bodyPos = 0;
		while (bodyPos < bodyLen)
		{
			size_t chunk = bodyLen;
			if (maxBodyChunkSize != 0)
				chunk = std::min(maxBodyChunkSize, bodyLen - bodyPos);
			res = request->Backend->SendBodyPart(request, (uint8_t*) body + bodyPos, chunk);
			if (res == hb::SendResult_All)
				bodyPos += chunk;
			else if (res == hb::SendResult_Closed)
				printf("Backend closed\n");			// We should stress this path in tests
			else
				printf("SendResponse failed unexpectedly\n");
		}
	}

	static void WorkerThread(Server* server)
	{
		int backoffMicroSeconds = 0;
		unsigned int tick = 0;
		while (!server->Stop)
		{
			tick++;
			hb::RequestPtr req = nullptr;
			server->ThreadRequestQueueLock.lock();
			if (server->ThreadRequestQueue.size() != 0)
			{
				req = server->ThreadRequestQueue.back();
				server->ThreadRequestQueue.pop_back();
			}
			server->ThreadRequestQueueLock.unlock();

			if (req != nullptr)
			{
				//if (tick % 4 != 0)
				if (true)
				{
					// send response as single frame
					hb::Response resp(req);
					resp.SetBody(req->BodyBuffer.Data, req->BodyBuffer.Count);
					resp.Send();
				}
				else
				{
					// split response over two frames
					hb::Response r1(req);
					int half = (int) req->BodyBuffer.Count / 2;
					r1.AddHeader_ContentLength(req->BodyBuffer.Count);
					r1.SetBody(req->BodyBuffer.Data, half);
					r1.Send();
					hb::SleepNano(50 * 1000);
					auto r2 = hb::Response::MakeBodyPart(req, req->BodyBuffer.Data + half, req->BodyBuffer.Count - half);
					r2.Send();
				}
			}
			else
			{
				backoffMicroSeconds = 1 + backoffMicroSeconds * 2;
				if (backoffMicroSeconds > 1000)
					backoffMicroSeconds = 1000;
				hb::SleepNano(backoffMicroSeconds * 1000);
			}
		}
	}

};


int main(int argc, char** argv)
{
	hb::Startup();
	
	hb::Backend backend;
	Server server;
	server.Backend = &backend;
	server.StartThreads();

	while (!server.Stop)
	{
		if (!backend.IsConnected())
		{
			if (!backend.Connect("tcp", "127.0.0.1:8081"))
				hb::SleepNano(500 * 1000 * 1000);
			else
				printf("Connected\n");
		}

		hb::InFrame inframe;
		if (backend.Recv(inframe))
			server.HandleFrame(inframe);
	}

	server.WaitForThreadsToDie();

	// This is polite. If we don't do this, then our TCP socket can be closed before we've finished transmitting
	// our final reply, and then the Go test server stalls while waiting for us.
	hb::SleepNano(10 * 1000 * 1000);

	hb::Shutdown();

	return 0;
}


