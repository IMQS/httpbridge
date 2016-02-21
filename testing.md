For general instructions on running tests, see README.md

## Generating valgrind suppressions

Build test-backend standalone, with -O1 and -g:

	gcc -g -O1 -I cpp/flatbuffers/include/ -pthread -std=c++11 cpp/test-backend.cpp  cpp/http-bridge.cpp -lstdc++ -o test-backend

Then, launch the two sides follows:

	valgrind --tool=helgrind --suppressions=valgrind-suppressions --gen-suppressions=yes ./test-backend
	go test httpbridge -run Thread -external_backend

## Generating Go line coverage

* go test httpbridge -coverprofile=coverage.out
* go tool cover -html=coverage.out

## Generating C++ line coverage

These instructions are for Windows. I haven't tried generating C++ line coverage on Linux.

* Install OpenCppCoverage, and add it's directory to your PATH
* Change directory to the httpbridge root
* Run `tundra2 win64-msvc-debug-default`
* Run scripts\gen-coverage-windows-cpp.bat
