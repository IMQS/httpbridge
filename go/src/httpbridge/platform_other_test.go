// +build !windows

package httpbridge

var cpp_test_build []string

const cpp_test_bin = "./test-backend"

func init() {
	root := "../../../"
	cpp_test_build = []string{"gcc", "-I" + root + "cpp/flatbuffers/include", "-pthread", "-std=c++11", root + "cpp/test-backend.cpp", root + "cpp/http-bridge.cpp", "-lstdc++", "-o", "test-backend"}
}
