# httpbridge
A small C++ library for implementing an HTTP2 API endpoint.

We "outsource" the real HTTP2 server to an external agent. The only such agent that presently exists is one that uses Go's HTTP2 infrastructure.
There is also a tiny HTTP/1.1 server embedded in the code base, that can be suitable for unit tests of your C++ HTTP service.

In your C++ backend, you only need to include one .cpp file and one .h file in order to serve up requests.

The C++ backend and the HTTP server communicate via a simple protocol defined on top of Flatbuffers. Only one socket is opened between
your C++ backend and the Go server.

### C++ Dependencies:

* C++11 (VS 2013+, Clang, GCC)
* Flatbuffers
* Tested on Linux, Windows

### Why?
I do not want to embed the current crop of C++ HTTP2 server implementations, due to their size, as well as their dependency chain.
For my specific use case, we already use a Go HTTP2 front-end to load balance and route requests back to an array of services. It makes sense to
reuse the Go functionality for this purpose, since it lowers the total amount of code in our system.

FastCGI is an obvious alternative in this domain, but I chose to build something new for the following reasons:

1. I would need to write a FastCGI server in Go.
2. There is no clear path on how you'd implement HTTP2 features over FastCGI.
3. This seemed like an interesting thing to build.

## Building the example C++ backend
__MSVC:__ `cl -Icpp/flatbuffers/include /EHsc Ws2_32.lib cpp/example-backend.cpp cpp/http-bridge.cpp`  
__GCC:__ `gcc -Icpp/flatbuffers/include -std=c++11 cpp/example-backend.cpp cpp/http-bridge.cpp -lstdc++ -o example-backend`  
__Clang:__ `clang -Icpp/flatbuffers/include -std=c++11 cpp/example-backend.cpp cpp/http-bridge.cpp -lstdc++ -o example-backend`  

In order to build your own C++ backend, your need to include the following files into your project. There is no prepackaged
"static library" or "shared library". Just include these files and you're done.

* http-bridge.cpp
* http-bridge.h
* http-bridge_generated.h
* flatbuffers.h

## Building the example Go server
* Change directory to `go`
* `env.bat` (Windows)
* `. env.sh` (Unix)
* `go get github.com/google/flatbuffers/go`
* `go run cmd/example-server.go`
* You should now be able to launch the example C++ backend. Once you have both the Go server and C++ backend running,
you can try `curl localhost:8080`, and you should get a reply.

## Running tests
There are two test suites. One of them is written in Go, and it is responsible for actually performing HTTP requests
and having a real C++ backend respond to them.
The other test suite is pure C++, and it tests a few small an isolated pieces of C++ code.

### Running the pure C++ tests
Build the unit-test project using tundra, and run the executable.

### Running the Go tests
* From the "go" directory, run env(.bat/sh)
* go test httpbridge

The Go test suite automatically compiles the C++ backend tester, and launches it.

If you need to debug the C++ code, that is normally launched by the Go test suite, then you can launch the C++ server
from a C++ debugger, and then pass the "external_backend" flag to the Go test suite so that it doesn't try to launch the C++ server itself.
For example: `go test httpbridge -external_backend`

To raise the logging level of the Go server, alter it inside restart() in cpp_test.go
