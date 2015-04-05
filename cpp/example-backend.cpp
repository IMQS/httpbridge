#include "http-bridge.h"
#include <stdio.h>

int main(int argc, char** argv)
{
	httpbridge::Startup();

	httpbridge::Backend backend;

	for (;;)
	{
		if (!backend.IsConnected())
		{
			if (backend.Connect("tcp", "localhost:8081"))
			{
				printf("Connected\n");
			}
			else
			{
				printf("Unable to connect\n");
				httpbridge::SleepNano(1000 * 1000 * 1000);
			}
		}

		httpbridge::Request request;
		httpbridge::RecvResult res = backend.Recv(request);
		if (res == httpbridge::RecvResult_Data)
		{
			if (request.IsHeader())
			{
				if (!request.IsEntireBodyInsideHeader() && request.BodyLength() < 1024 * 1024)
				{
					backend.ResendWhenBodyIsDone(request);
				}
				else
				{
					printf("-----------------------------\n");
					printf("%d %d %s %s %s\n", (int) request.Channel(), (int) request.Stream(), request.Method(), request.URI(), httpbridge::VersionString(request.Version()));
					for (int i = 0; i < request.HeaderCount(); i++)
					{
						const char *key, *val;
						request.HeaderAt(i, key, val);
						printf("  %-16s = %s\n", key, val);
					}
					httpbridge::Response response;
					response.Init(request);
					response.Status = httpbridge::Status200_OK;
					//response.WriteHeader("Content-Length", "5");
					response.SetBody(5, "hello");
					backend.Send(response);
				}
			}
			else
			{
				int offset = (int) request.FrameBodyOffset();
				int bytes = (int) request.FrameBodyLength();
				printf("%d %d BODY(%d..%d / %d)\n  %.*s\n", (int) request.Channel(), (int) request.Stream(), offset, offset + bytes, (int) request.BodyLength(), bytes, (const char*) request.FrameBody());
			}
		}
	}

	httpbridge::Shutdown();

	return 0;
}
