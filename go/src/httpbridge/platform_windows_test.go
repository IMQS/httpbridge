// +build windows

package httpbridge

var cpp_test_build []string

const cpp_test_bin = "test-backend.exe"

func init() {
	// O2 is necessary to get good throughput for performance tests
	root := "../../../"
	cpp_test_build = []string{"cl.exe", "-I" + root + "cpp/flatbuffers/include", "/Zi", "/O2", "/EHsc", "Ws2_32.lib", root + "cpp/test-backend.cpp", root + "cpp/http-bridge.cpp"}
}
