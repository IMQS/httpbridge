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

		hb::InFrame inframe;
		if (backend.Recv(inframe))
		{
			hb::Request* request = inframe.Request;
			
			if (inframe.BodyBytesLen != 0)
			{
				int bytes = (int) inframe.BodyBytesLen;
				printf("%d %d BODY(%d bytes)\n  %.*s\n", (int) request->Channel, (int) request->Stream, bytes, bytes, (const char*) inframe.BodyBytes);
			}

			/* The following block demonstrates how you explicitly inform Backend that you want this request to be buffered:

			if (inframe.IsHeader && !inframe.IsLast)
			{
				if (... URL matches criteria...)
				{
					inframe.ResendWhenBodyIsDone();
					continue;
					}
			}
			*/

			if (inframe.IsAborted)
			{
				printf("Request aborted\n");
			}
			else if (inframe.IsLast)
			{
				printf("-----------------------------\n");
				printf("%d %d %s %s %s\n", (int) request->Channel, (int) request->Stream, request->Method(), request->URI(), hb::VersionString(request->Version));
				for (int i = 0; i < request->HeaderCount(); i++)
				{
					const char *key, *val;
					request->HeaderAt(i, key, val);
					printf("  %-16s = %s\n", key, val);
				}
				hb::Response response;
				response.Init(*request);
				response.Status = hb::Status200_OK;
				response.SetBody(5, "hello");
				backend.Send(response);
				printf("-----------------------------\n");
			}
		}
	}

	hb::Shutdown();

	return 0;
}
