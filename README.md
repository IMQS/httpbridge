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
3. I wanted a nice API in C++ for writing an HTTP service.
4. This seemed like an interesting thing to build.

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

## How to write C++ code that uses httpbridge
httpbridge forces upon you an event-driven model. The events that you wait for are HTTP frames. One or more
frames make up a single request. Often, a request is just a single frame. You can also get httpbridge to
buffer up all the frames of a small request, so that you just receive a single event when that request
has finished transmitting. However, since we do need to support the case of a long running, large request
(such as a file upload), the event-driven model needs to be exposed to you. The default automatic buffering
size of httpbridge is 16MB. In other words, if the Content-Length header of a request is less than or equal
to 16MB, then that request will be buffered up by Backend before Backend.Recv() will return with a frame.
You can change this value with the MaxAutoBufferSize setting (a member variable of Backend). There is also
a limit on the total amount of memory that Backend will allocate to such buffers. This is controllable with
the MaxWaitingBufferTotal setting. If a request is small enough to be buffered, but the total amount of
memory has been exhausted, then the Backend will respond to the request with a 503.

To start implementing your own server, you'll probably want to take a look at example-backend.cpp.
Essentially what you need is a single thread that repeatedly calls Backend.Recv(). Also, it is probably
a good idea to reconnect to the server automatically inside that loop, for the case where the HTTP server
is restarted (the example does this).

Frames that are received by Backend.Recv have a few flags that you need to pay attention to in order to
decide what kind of action to take on that frame. Firstly, the IsHeader flag tells you that the frame
is the first frame of a request. You are guaranteed here to have access to all of the HTTP headers, but
not all of the body. Whether or not you act on the request at this stage will depend on whether you need
to wait for the entire body or not. For example, if this is a file upload, then you may want to start
filing away the frames to wherever they are being stored, and only when the final frame is received, then
send the response. On the other hand, if this is a GET request, then you will probably be sending the result
immediately, because you do not need to wait for the body to be sent, because there is no body. Yet another
scenario is a POST request that includes a small body. Instead of acting on the first frame, you instead
wait for the last frame, and send a response on that. The IsLast flag on a frame tells you whether this
is the last frame for a request. There is one more flag, IsAborted. This tells you that the client has
disconnected this request, so you should abandon whatever work you were busy doing with this request.
You must not send a response to an aborted request. For all other requests, you must send a response.

#### Sending a response
Most responses are sent with a single Response object, which includes the entire body of the response.
However, there are cases where it makes sense to split the response into multiple frames (for example
a file download). In order to send a response over multiple frames, make sure that the initial Response
has a Content-Length header field set. Thereafter, use Backend.SendBodyPart() to stream out the rest
of the response body. When the last byte has been sent, the request object will be deleted. An implication
of this mechanism is that you cannot stream a result without specifying it's size up front (via the
Content-Length header).

TODO: Finish implementation of streaming responses on the Go side, and add a test

#### Threads
You must poll Backend from a single thread. The same thread that calls Connect() must also call
Recv(). This is merely a sanity check, but httpbridge will panic if this is violated. The intended
threading model is that you create as many worker threads as you need, and you farm requests
out to them. As soon as a worker thread is finished with a request, it calls Backend->Send(),
or the equivalent Response->Send(). You don't need to run multiple threads - it is perfectly
fine to run just one thread.

The functions on Backend that deal with a request/response are all callable from
multiple threads. These exact list of functions that are safe to call from multiple threads is:

* The two Send() functions
* SendBodyPart()
* ResendWhenBodyIsDone(),
* RequestDestroyed()
* AnyLog()

TODO: Add a mode to test-backend that executes requests on multiple threads, and see if we can
get any race conditions to occur.

## TODO
* Implement rate limiting mechanism, which relies on the HTTP server telling us that the TCP socket
doesn't want any more data yet. This would require a new kind of frame that gets sent from server
to backend.
