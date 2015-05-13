// +build !windows

package httpbridge

var cpp_test_build []string

const cpp_test_bin = "test-backend"

func init() {
	cpp_test_build = []string{"gcc", "-I../cpp/flatbuffers/include", "-std=c++11", "../cpp/test-backend.cpp", "../cpp/http-bridge.cpp", "-lstdc++", "-o", "example-backend"}
}
