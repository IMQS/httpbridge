// +build windows

package httpbridge

var cpp_test_build []string

const cpp_test_bin = "test-backend.exe"

func init() {
	cpp_test_build = []string{"cl.exe", " -I../cpp/flatbuffers/include", "/EHsc", "Ws2_32.lib", "../cpp/test-backend.cpp", "../cpp/http-bridge.cpp"}
	//cpp_test_build = []string{"cl"}
}
