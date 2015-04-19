#include "http-bridge.h"
#include <stdio.h>

int main(int argc, char** argv)
{
	hb::Startup();

	hb::Backend backend;

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
				hb::SleepNano(1000 * 1000 * 1000);
			}
		}

		hb::Request request;
		hb::RecvResult res = backend.Recv(request);
		if (res == hb::RecvResult_Data)
		{
			if (request.IsHeader())
			{
				printf("-----------------------------\n");
				printf("%d %d %s %s %s\n", (int) request.Channel(), (int) request.Stream(), request.Method(), request.URI(), hb::VersionString(request.Version()));
				for (int i = 0; i < request.HeaderCount(); i++)
				{
					const char *key, *val;
					request.HeaderAt(i, key, val);
					printf("  %-16s = %s\n", key, val);
				}
				hb::Response response;
				response.Init(request);
				response.Status = hb::Status200_OK;
				response.SetBody(5, "hello");
				backend.Send(response);
			}
			else
			{
				int offset = (int) request.FrameBodyOffset();
				int bytes = (int) request.FrameBodyLength();
				printf("%d %d BODY(%d..%d / %d)\n  %.*s\n", (int) request.Channel(), (int) request.Stream(), offset, offset + bytes, (int) request.BodyLength(), bytes, (const char*) request.FrameBody());
			}
		}
	}

	hb::Shutdown();

	return 0;
}
