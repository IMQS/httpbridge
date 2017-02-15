# httpbridge
A small C++ library for implementing an HTTP2 API endpoint.

We "outsource" the real HTTP2 server to an external agent. The only such agent that presently exists is one that uses Go's HTTP2 infrastructure.
There is also a tiny HTTP/1.1 server embedded in the code base, that can be suitable for unit tests of your C++ HTTP service.

In your C++ backend, you only need to include one .cpp file and three .h files in order to serve up requests.

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
3. I wanted a nice API in C++ for writing an HTTP service. This is harder than it may sound.

## Building the example C++ backend
__MSVC:__ `cl -Icpp/flatbuffers/include /EHsc Ws2_32.lib cpp/example-backend.cpp cpp/http-bridge.cpp`  
__GCC:__ `gcc -Icpp/flatbuffers/include -std=c++11 cpp/example-backend.cpp cpp/http-bridge.cpp -lstdc++ -o example-backend`  
__Clang:__ `clang -Icpp/flatbuffers/include -std=c++11 cpp/example-backend.cpp cpp/http-bridge.cpp -lstdc++ -o example-backend`  

In order to build your own C++ backend, you need to include the following files into your project. There is no prepackaged
"static library" or "shared library". Just include these files and you're done. However, if you do want to create a static
or shared library, nothing stops you doing that.

* http-bridge.cpp
* http-bridge.h
* http-bridge_generated.h
* flatbuffers.h

## Building the example Go server
* Change directory to `go`
* `env.bat` (Windows)
* `. env.sh` (Unix) - note this is NOT the same as `./env.sh`, because we need to alter environment variables.
* Ensure git submodules are up to date `git submodule update --init`
* `go run cmd/example-server.go`
* You should now be able to launch the example C++ backend. Once you have both the Go server and C++ backend running,
you can try `curl localhost:8080`, and you should get a reply.

## Running tests
There are two test suites. One of them is written in Go, and it is responsible for actually performing HTTP requests
and having a real C++ backend respond to them.
The other test suite is pure C++, and it tests a few small, isolated pieces of C++ code.

### Running the pure C++ tests
Build the unit-test project using tundra, and run the executable.

### Running the Go tests
* From the "go" directory, run env(.bat/sh)
* Ensure that your compiler is on the PATH (on Windows, this usually means running `vcvarsall.bat amd64`)
* go test httpbridge

The Go test suite automatically compiles the C++ backend tester (using CL or GCC), and launches it.

If you need to debug the C++ code, that is normally launched by the Go test suite, then you can launch the C++ server
from a C++ debugger, and then pass the "external_backend" flag to the Go test suite so that it doesn't try to launch the C++ server itself.
For example: `go test httpbridge -external_backend`

To raise the logging level of the Go server, alter it inside restart() in cpp_test.go

I'm still experimenting with integrating valgrind into the test suite. For right now, you can get some example output by running the tests like this:
* go test httpbridge -v -valgrind -run Thread

### More information on testing
For more details on testing, including Valgrind and line coverage, see testing.md

## How to write C++ code that uses httpbridge
To get started, copy code from [example-backend.cpp](cpp/example-backend.cpp).

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
decide what kind of action to take on that frame.
Firstly, you will typically only act on frames where `inframe.Type == hb::FrameType::Data`. The other
frame types are control frames, and those are described in detail below.

If this is a data frame, then the InFrame::IsHeader tells you that the frame
is the first frame of a request. You are guaranteed here to have access to all of the HTTP headers, but
not all of the body. Whether or not you act on the request at this stage will depend on whether you need
to wait for the entire body or not. For example, if this is a file upload, then you may want to start
filing away the frames to wherever they are being stored, and only when the final frame is received, then
send the response. On the other hand, if this is a GET request, then you will probably be sending the result
immediately, because you do not need to wait for the body to be sent, because there is no body. Yet another
scenario is a POST request that includes a small body. Instead of acting on the first frame, you instead
wait for the last frame, and send a response on that. The IsLast flag on a frame tells you whether this
is the last frame for a request. It is common for IsFirst and IsLast to both be set on a frame, if the entire
request consists of just one frame (or if the server has buffered up the request for you).

Unless a stream has been aborted, you must send a response for every request.

#### Sending a response
Most responses are sent with a single Response object, which includes the entire body of the response.
However, there are cases where it makes sense to split the response into multiple frames (for example
a file download). In order to send a response over multiple frames, set the Content-Length header field
(you can use Response::AddHeader_ContentLength to do this). If you don't know the size of the response,
then set the Content-Length to -1. This signals to the server that this is a chunked response.
To send body frames, call Backend.SendBodyPart() repeatedly, until the last frame is sent. You can also
use Response::MakeBodyPart(). When sending your final frame, you must set the last parameter "isFinal = true",
when calling SendBodyPart or MakeBodyPart.

#### Threads
You must poll Backend from a single thread. The same thread that calls Connect() must also call
Recv(). This is merely a sanity check, but httpbridge will panic if this is violated. The intended
threading model is that you create as many worker threads as you need, and you farm requests
out to them. As soon as a worker thread is finished with a request, it calls Backend->Send(),
or the equivalent Response->Send(). You don't need to run multiple threads, but it is better to
have a dedicated thread that is calling Recv() continually, so that you can be informed by the
backend of control frames (ie Pause, Resume, Abort).

The functions on Backend that deal with a request/response are all callable from
multiple threads. The exact list of functions that are safe to call from multiple threads is:

* The two Send() functions
* SendBodyPart()
* ResendWhenBodyIsDone(),
* RequestDestroyed()
* AnyLog()

#### Abort, Pause, Resume
Every frame that the server sends you fall into one of 4 categories:

* Data
* Abort
* Pause
* Resume

A data frame contains HTTP headers and/or the body of the request.
The other frames are control frames.

An Abort frame is sent by the server when something goes wrong, such as
a client disconnecting. If you receive an Abort frame, then you should perform no further
processing on that stream. Any response that you send to an aborted stream will be discarded.

A Pause frame is sent by the server when the TCP send buffer to the client is full.
The backend must not send any more response frames until it receives a subsequent
Resume frame for that stream.

When hb::Backend receives a control frame, it changes the state of the Request object,
and you can get that state at any time by calling Request::State(). In addition to changing
the state, Backend::Recv() will also return a frame with the relevant control message
inside InFrame::Type. Depending on your threading model, you may want to respond
immediately to control frames that are emitted by Recv(). Alternatively, you can poll
Request::State() to detect changes.

## Backpressure
httpbridge uses a single TCP socket between the server and the backend. This is obviously
more efficient than a TCP socket per client connection, but it also has a downside,
in that a slow client has the potential to cause our single socket to block.

How exactly can this happen?

The Go httpbridge server has a single loop that fetches frames from the backend's TCP socket,
repackages them as HTTP frames, and sends them over Go's HTTP server infrastructure.
Because there is just one thread doing this work, it is possible for this thread to block,
if one of the clients is receiving data slowly. For example, imagine you have ten concurrent
downloads, and nine of them are on fiber optic lines, but one of them is on a 56K modem. The 
TCP send buffer for the modem client will fill up soon, and so OS writes into that buffer
will block until the buffer has free space inside it. This has a knock-on effect, causing
the single thread that reads from the backend, to block also. Now, packets aren't being
fetched and dispatched for the 9 fast clients, so the one slow reader is causing all others
to stop. Obviously this is not acceptable.

Our solution, is to use a buffered channel in Go, for each client stream. When the length
of a channel's queue rises to a certain high threshold, we send a Pause frame to the backend,
telling is to stop sending data for that stream. The backend continue to send data for
streams that still have capacity. Over time, the channel queue of the paused stream
will drain, and once it reaches a low threshold, we send a Resume frame to the backend,
and it starts sending more frames.




