package httpbridge

import (
	"fmt"
	"os"
	"os/exec"
	"testing"
)

var cpp_server *exec.Cmd

func build_cpp() error {
	//fmt.Printf("executing (%v) %v %v %v %v", cpp_test_build[0], cpp_test_build[1:])
	cmd := exec.Command(cpp_test_build[0], cpp_test_build[1:]...)
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("Build of test-backend failed: %v", err)
	}
	return nil
}

func setup() error {
	if err := build_cpp(); err != nil {
		return err
	}
	cpp_server = exec.Command(cpp_test_bin)
	if err := cpp_server.Start(); err != nil {
		return fmt.Errorf("Failed to launch cpp backend: %v", err)
	}
	return nil
}

func teardown() {
	if cpp_server != nil {
		cpp_server.Process.Kill()
		cpp_server = nil
	}
}

func TestMain(m *testing.M) {
	if err := setup(); err != nil {
		fmt.Printf("%v\n", err)
		os.Exit(1)
	}
	os.Exit(m.Run())
	teardown()
}

func TestEcho(t *testing.T) {
	t.Logf("Hello....\n")
}
