// +build windows

package httpbridge

var cpp_test_build []string

const cpp_test_bin = "test-backend.exe"

func init() {
	root := "../../../"
	cpp_test_build = []string{"cl.exe", "-I" + root + "cpp/flatbuffers/include", "/Zi", "/EHsc", "Ws2_32.lib", root + "cpp/test-backend.cpp", root + "cpp/http-bridge.cpp"}
	//cpp_test_build = []string{"cl"}
}
