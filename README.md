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

## Building the example C++ client
### tundra
* Install [tundra2](https://github.com/deplinenoise/tundra)
* Simply run `tundra2` from the root directory to build

## Building the example Go server
* Change directory to `go`
* `env.bat`
* `go get github.com/google/flatbuffers`
* `go get github.com/benharper123/httpbridge`
* `go run cmd/example-server.go`
* Note that the above sequence of 'go get' will checkout two copies of the Go httpbridge code. This is just something
that you need to live with if you want to demonstrate using an external library from inside itself.
