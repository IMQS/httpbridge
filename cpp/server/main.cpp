#include "server.h"
#ifndef _WIN32
#include <signal.h>
#endif

static hb::Server* SingleServer;

#ifdef _WIN32
BOOL ctrl_handler(DWORD ev)
{ 
	if (ev == CTRL_C_EVENT && SingleServer != nullptr)
	{
		SingleServer->Stop();
		return TRUE;
	}
	return FALSE;
}
void setup_ctrl_c_handler()
{
	SetConsoleCtrlHandler((PHANDLER_ROUTINE) ctrl_handler, TRUE);
}
#else
void signal_handler(int sig)
{
	if (SingleServer != nullptr)
		SingleServer->Stop();
}
void setup_ctrl_c_handler()
{
	struct sigaction sig;
	sig.sa_handler = signal_handler;
	sigemptyset(&sig.sa_mask);
	sig.sa_flags = 0;
	sigaction(SIGINT, &sig, nullptr);
}
#endif

int main(int argc, char** argv)
{
	setup_ctrl_c_handler();

	hb::Startup();

	hb::Server server;
	SingleServer = &server;
	server.ListenAndRun("127.0.0.1", 8080, 8081);
	
	hb::Shutdown();

	SingleServer = nullptr;
	return 0;
}