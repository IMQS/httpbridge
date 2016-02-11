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
			if (backend.Connect("tcp", "127.0.0.1:8081"))
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
			hb::RequestPtr request = inframe.Request;

			if (inframe.BodyBytesLen != 0)
			{
				int bytes = (int) inframe.BodyBytesLen;
				printf("%d %d BODY(%d bytes)\n  %.*s\n", (int) request->Channel, (int) request->Stream, bytes, bytes, (const char*) inframe.BodyBytes);
			}

			/* The following block demonstrates how you explicitly inform Backend that you want this request to be buffered:

			if (inframe.IsHeader && !inframe.IsLast)
			{
				if (... request matches criteria ...)
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
				printf("%d %d %s %s %s\n", (int) request->Channel, (int) request->Stream, request->Method().CStr(), request->URI().CStr(), hb::VersionString(request->Version));
				for (int i = 0; i < request->HeaderCount(); i++)
				{
					const char *key, *val;
					request->HeaderAt(i, key, val);
					printf("  %-16s = %s\n", key, val);
				}
				hb::Response response(request, hb::Status200_OK);
				// write the request's body back out
				std::string responseBody = std::string("URL path: ") + request->Path().CStr() + "\n";
				const char *qkey, *qval;
				for (auto iter = request->NextQuery(0, qkey, qval); iter != 0; iter = request->NextQuery(iter, qkey, qval))
					responseBody += std::string("URL query: ") + qkey + "=" + qval + "\n";
				responseBody += "Body: ";
				if (request->BodyBuffer.Count != 0)
					responseBody.append((const char*) request->BodyBuffer.Data, request->BodyBuffer.Count);
				responseBody.append("\n");
				response.SetBody(responseBody.c_str(), responseBody.size());
				response.Send();
				printf("-----------------------------\n");
			}
			else
			{
				printf("- intermediate frame -\n");
			}
		}
	}

	hb::Shutdown();

	return 0;
}
