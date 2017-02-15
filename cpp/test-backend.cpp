//######################################################
//
// This is used by unit tests (ie spawned by Go tests)
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
		// We ignore other frames, such as pause/resume.
		// Our background sending thread uses Request.State to access that information.
		if (inframe.Type != hb::FrameType::Data)
			return;

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
			if (inframe.IsLast)
			{
				ThreadRequestQueueLock.lock();
				ThreadRequestQueue.push_back(inframe.Request);
				ThreadRequestQueueLock.unlock();
			}
		}
		else if (prefix_match("/garbage-stream"))
		{
			if (inframe.IsLast)
			{
				auto s = std::make_shared<StreamOut>();
				s->Remaining = (size_t) inframe.Request->QueryInt64("Size");
				s->Request = inframe.Request;
				s->ID = StreamOutNextID++;
				hb::Response r(s->Request);
				r.AddHeader_ContentLength(s->Remaining);
				r.Send();
				StreamOutQueueLock.lock();
				StreamOutQueue.push_back(s);
				printf("New garbage stream (%d total) [%llu:%llu]\n", (int) StreamOutQueue.size(), inframe.Request->Channel, inframe.Request->Stream);
				StreamOutQueueLock.unlock();
			}
		}
		else if (prefix_match("/echo"))
		{
			HttpEcho(inframe, lr);
		}
		else
		{
			Backend->Send(inframe.Request, hb::Status404_Not_Found);
		}

		// We forget about a request once we've received the last frame of it's request.
		// We might still be spending a long time sending it's response.
		if (inframe.IsLast)
			EndRequest(inframe);
	}

	void StartThreads()
	{
		Threads.resize(NumWorkerThreads);
		for (size_t i = 0; i < Threads.size(); i++)
			Threads[i] = std::thread(WorkerThread, this);
		StreamOutThread = std::thread(StreamOutThreadFunc, this);
	}

	void WaitForThreadsToDie()
	{
		for (auto& t : Threads)
			t.join();
		StreamOutThread.join();
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
	
	struct StreamOut
	{
		uint64_t		ID = 0;
		size_t			Remaining = 0;
		hb::RequestPtr	Request;
	};
	std::thread								StreamOutThread;
	std::mutex								StreamOutQueueLock;
	std::vector<std::shared_ptr<StreamOut>>	StreamOutQueue;
	std::atomic<uint64_t>					StreamOutNextID;

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

	void HttpControl(hb::InFrame& inframe, LocalRequest* lr)
	{
		const auto& buffer_max = inframe.Request->Query("MaxAutoBufferSize");
		const auto& waiting_buffer_max = inframe.Request->Query("MaxWaitingBufferTotal");
		if (buffer_max != nullptr)
			Backend->MaxAutoBufferSize = atoi(buffer_max);
		if (waiting_buffer_max != nullptr) 
			Backend->MaxWaitingBufferTotal = atoi(waiting_buffer_max);
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

	// We run one thread that is dedicated to streaming out responses
	static void StreamOutThreadFunc(Server* server)
	{
		// Every iteration that we are idle, we increase our sleep period, up until the maximum of maxSleepMS
		uint32_t sleepMS = 0;
		uint32_t maxSleepMS = 500;
		uint64_t totalSent = 0;
		int nDone = 0;
		size_t bufSize = 65536;
		uint8_t* buf = (uint8_t*) malloc(bufSize);
		for (uint32_t i = 0; i < bufSize; i++)
			buf[i] = (uint8_t) i;

		auto lastMsg = clock();
		bool started = false;

		while (!server->Stop)
		{
			// Build a list of the requests that are ready to receive data
			std::vector<std::shared_ptr<StreamOut>> ready;
			server->StreamOutQueueLock.lock();
			int nPaused = 0;
			for (size_t i = 0; i < server->StreamOutQueue.size(); i++)
			{
				started = true;
				auto q = server->StreamOutQueue[i];
				bool aborted = q->Request->State() == hb::StreamState::Aborted;
				if (q->Remaining == 0 || aborted)
				{
					if (q->Remaining == 0)
						nDone++;
					printf("%s %d\n", aborted ? "Aborted" : "Done", (int) q->ID);
					server->StreamOutQueue.erase(server->StreamOutQueue.begin() + i);
					i--;
				}
				else if (q->Request->State() == hb::StreamState::Active)
				{
					ready.push_back(q);
				}
				else if (q->Request->State() == hb::StreamState::Paused)
				{
					nPaused++;
				}
			}
			bool finished = nDone != 0 && server->StreamOutQueue.size() == 0;
			server->StreamOutQueueLock.unlock();
			//if (finished)
			//	break;

			//if (ready.size() != 0)
			//	printf("Sending to %d ready streams\n", (int) ready.size());

			// Send
			for (auto q : ready)
			{
				size_t chunkSize = std::min(bufSize, q->Remaining);
				//for (uint32_t i = 0; i < chunkSize; i++)
				//	buf[i] = (uint8_t) (q->Remaining - i);
				auto res = hb::Response::MakeBodyPart(q->Request, buf, chunkSize).Send();
				if (res == hb::SendResult_Closed)
					return;
				q->Remaining -= chunkSize;
				totalSent += chunkSize;
				//printf("Chunk %d bytes\n", (int) chunkSize);
				//printf("Sent %d bytes\n", (int) totalSent);
				//if (ready.size() == 1)
				//	printf("Sent %d MB on %d\n", (int) (totalSent / 1024 / 1024), (int) q->ID);
			}

			if (ready.size() == 0)
				sleepMS = std::min((uint32_t) ((sleepMS + 1) * 1.5), maxSleepMS);
			else
				sleepMS = 0;

			if (sleepMS != 0)
			{
				//printf("Sleeping for %d MS. Total sent: %d\n", (int) sleepMS, (int) totalSent);
				hb::SleepNano(sleepMS * 1000000);
			}
			else
			{
				//if (ready.size() > 1)
				//	printf("Sent %d MB\n", (int) (totalSent / 1024 / 1024));
			}
		
			if (started && !finished && (double) (clock() - lastMsg) / (double) CLOCKS_PER_SEC > 1.0)
			{
				printf("Total sent: %9d MB, Paused: %3d, Done: %3d\n", (int) (totalSent / (1024 * 1024)), nPaused, nDone);
				lastMsg = clock();
			}
		}

		free(buf);
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
				if (req->Path() == "/echo-thread")
				{
					// Echos the body back
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
				else if (req->Path() == "/garbage-stream")
				{
					char buf[8192];
					size_t qsize = (size_t) req->QueryInt64("Size");
					hb::Response head(req);
					head.AddHeader_ContentLength(qsize);
					head.Send();
					size_t remain = qsize;
					while (remain != 0)
					{
						size_t chunk = std::min((size_t) 8192, remain);
						hb::Response::MakeBodyPart(req, buf, chunk).Send();
						remain -= chunk;
					}
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
	
	hb::Logger stdlog;
	hb::Backend backend;
	backend.Log = &stdlog;
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
