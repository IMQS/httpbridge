package httpbridge

import (
	"bytes"
	"fmt"
	"io"
	"io/ioutil"
	"net/http"
	"os"
	"os/exec"
	"testing"
	"time"
)

const (
	baseUrl = "http://127.0.0.1:8080"
)

var cpp_server *exec.Cmd
var cpp_server_out *bytes.Buffer
var front_server *Server

func build_cpp() error {
	//cwd, _ := os.Getwd()
	//fmt.Printf("pwd = %v\n", cwd)
	cmd := exec.Command(cpp_test_build[0], cpp_test_build[1:]...)
	cmdOut := &bytes.Buffer{}
	cmd.Stdout = cmdOut
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("Build of test-backend failed: %v\n%v", err, string(cmdOut.Bytes()))
	}
	return nil
}

// process setup
func setup() error {
	// Compile backend
	if err := build_cpp(); err != nil {
		return err
	}
	return nil
}

// process tear-down
func teardown() {
	fmt.Printf("teardown... ")
	kill_cpp(nil)
	//kill_front()
	fmt.Printf("teardown done\n")
}

func kill_cpp(t *testing.T) {
	if cpp_server != nil {
		fmt.Printf("Stopping cpp server\n")
		time.Sleep(10 * time.Second)
		resp, _ := http.Get(baseUrl + "/stop")
		fmt.Printf("read\n")
		if resp.Body != nil {
			io.Copy(ioutil.Discard, resp.Body)
		}
		fmt.Printf("close\n")
		resp.Body.Close()
		fmt.Printf("wait\n")
		cpp_server.Wait()
		fmt.Printf("cpp server stopped\n")
		if t != nil {
			if !cpp_server.ProcessState.Success() {
				t.Logf("cpp output:\n%v\n", string(cpp_server_out.Bytes()))
				t.Fatalf("cpp server exited with non-zero exit code")
			}
		}
		//cpp_server.Process.Kill()
		cpp_server = nil
		cpp_server_out = nil
	}
}

//func kill_front() {
//	if front_server != nil {
//		front_server.Stop()
//		front_server = nil
//	}
//}

func restart(t *testing.T) {
	//kill_front()
	kill_cpp(t)

	// Launch HTTP server (front-end)
	if front_server == nil {
		front_server = &Server{}
		front_server.HttpPort = "127.0.0.1:8080"
		front_server.BackendPort = "127.0.0.1:8081"
		front_server.Log.Level = LogLevelDebug
		go front_server.ListenAndServe()
	}

	// Launch backend
	fmt.Printf("Starting cpp server")
	cpp_server = exec.Command(cpp_test_bin)
	cpp_server_out = &bytes.Buffer{}
	cpp_server.Stdout = cpp_server_out
	if err := cpp_server.Start(); err != nil {
		t.Fatalf("Failed to launch cpp backend: %v", err)
	}

	time.Sleep(2 * time.Second)
}

func doPost(t *testing.T, url string, body string) *http.Response {
	req, err := http.NewRequest("POST", baseUrl+url, bytes.NewReader([]byte(body)))
	if err != nil {
		t.Fatalf("%v: Error creating request: %v", url, err)
	}
	client := http.Client{}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("%v: Error executing request: %v", url, err)
	}
	return resp
}

func testPost(t *testing.T, url string, body string, expectCode int, expectBody string) {
	resp := doPost(t, url, body)
	respBodyB, err := ioutil.ReadAll(resp.Body)
	respBody := string(respBodyB)
	if err != nil {
		t.Fatalf("%v: Error reading response body: %v", url, err)
	}
	resp.Body.Close()
	if resp.StatusCode != expectCode {
		t.Errorf("%v:\nexpected code (%v)\nreceived code (%v)", url, expectCode, resp.StatusCode)
	}
	if respBody != expectBody {
		t.Errorf("%v:\nexpected (%v)\nreceived (%v)", url, expectBody, respBody)
	}
}

func TestMain(m *testing.M) {
	if err := setup(); err != nil {
		fmt.Printf("%v\n", err)
		os.Exit(1)
	}
	res := m.Run()
	teardown()
	os.Exit(res)
}

func TestEcho(t *testing.T) {
	restart(t)
	testPost(t, "/echo", "Hello!", 200, "Hello!")
}
