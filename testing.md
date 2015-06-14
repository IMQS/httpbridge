For general instructions on running tests, see README.md

## Generating valgrind suppressions

Build test-backend standalone, with -O1 and -g:

	gcc -g -O1 -I cpp/flatbuffers/include/ -pthread -std=c++11 cpp/test-backend.cpp  cpp/http-bridge.cpp -lstdc++ -o test-backend

Then, launch the two sides follows:

	valgrind --tool=helgrind --suppressions=valgrind-suppressions --gen-suppressions=yes ./test-backend
	go test httpbridge -run Thread -external_backend

