# httpbridge
A small C++ library for implementing an HTTP2 API endpoint.

We "outsource" the real HTTP2 server to an external agent. The only such agent that presently exists is one that uses Go's HTTP2 infrastructure.

In your C++ server, you only need to include one .cpp file and one .h file in order to serve up requests.

The C++ server and the HTTP server communicate via a simple protocol defined on top of Flatbuffers. Only one socket is opened between
your C++ process and the Go process.

### C++ Dependencies:

* C++11 (VS 2013, Clang, GCC)
* Flatbuffers

### Why?
I do not want to embed the current crop of C++ HTTP2 server implementations, due to their size, as well as their dependency chain.
For my specific use case, we already use a Go HTTP2 front-end to load balance and route requests back to an array of services. It makes sense to
reuse the Go functionality for this purpose, since it lowers the total amount of code in our system.

FastCGI is an obvious alternative in this domain, but I chose to build something new for the following reasons:

1. I would need to write a FastCGI server in Go.
2. There is no clear path on how you'd implement HTTP2 features over FastCGI.
3. This seemed like a fun thing to build.

## Building the example C++ backend
__MSVC:__ `cl -Icpp/flatbuffers/include /EHsc Ws2_32.lib cpp/example-backend.cpp cpp/http-bridge.cpp`  
__GCC:__ `gcc -Icpp/flatbuffers/include -std=c++11 cpp/example-backend.cpp cpp/http-bridge.cpp -lstdc++ -o example-backend`  
__Clang:__ `clang -Icpp/flatbuffers/include -std=c++11 cpp/example-backend.cpp cpp/http-bridge.cpp -lstdc++ -o example-backend`  

## Building the example Go server
* Change directory to `go`
* `env.bat` (Windows)
* `. env.sh` (Unix)
* `go get github.com/google/flatbuffers/go`
* `go get github.com/benharper123/httpbridge/go`
* `go run cmd/example-server.go`
* You should now be able to launch the example C++ backend. Once you have both the Go server and C++ backend running,
you can try `curl localhost:8081`, and you should get a reply.
* Note that the above sequence of 'go get' will checkout two copies of the Go httpbridge code. This is just something
that you need to live with if you want to demonstrate using an external library from inside itself.
