package httpbridge

import (
	"bytes"
	"flag"
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
	serverFrontPort   = "127.0.0.1:8080"
	serverBackendPort = "127.0.0.1:8081"
	baseUrl           = "http://" + serverFrontPort
)

var cpp_server *exec.Cmd
var cpp_server_out *bytes.Buffer
var cpp_server_err *bytes.Buffer
var front_server *Server
var pingClient http.Client
var requestClient http.Client

// This is useful when you want to launch test-backend.exe from the C++ debugger
// go test httpbridge -external_backend
// Also, make sure to test with at least -cpu 2
var external_backend = flag.Bool("external_backend", false, "Use an externally launched backend server")
var skip_build = flag.Bool("skip_build", false, "Don't build the C++ backend server")
var valgrind = flag.Bool("valgrind", false, "Run test-backend through valgrind")
var verbose_http = flag.Bool("verbose_http", false, "Show verbose http log messages")

func build_cpp() error {
	if *skip_build {
		return nil
	}
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
	flag.Parse()
	if !*external_backend {
		// Compile backend
		if err := build_cpp(); err != nil {
			return err
		}
	}

	return nil
}

// process tear-down
func teardown() {
	kill_cpp(nil, true)
	//kill_front()
}

func kill_cpp(t *testing.T, kill_external_backend bool) {
	if cpp_server != nil || (*external_backend && kill_external_backend) {
		fmt.Printf("Stopping cpp server\n")
		if t != nil {
			t.Logf("Stopping cpp server")
		}
		resp, err := http.Get(baseUrl + "/stop")
		if err == nil {
			io.Copy(ioutil.Discard, resp.Body)
			resp.Body.Close()
		}
	}

	if cpp_server != nil {
		cpp_server.Wait()
		fmt.Printf("cpp server stopped\n")
		success := cpp_server.ProcessState.Success()
		if !success || *valgrind {
			msg := fmt.Sprintf("cpp stdout:\n%v\ncpp stderr:\n%v\n", string(cpp_server_out.Bytes()), string(cpp_server_err.Bytes()))
			if t != nil {
				t.Log(msg)
			} else {
				fmt.Print(msg)
			}
		}
		if !success {
			if t != nil {
				t.Fatalf("cpp server exited with non-zero exit code")
			} else {
				fmt.Printf("cpp server exited with non-zero exit code")
			}
		}
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
	kill_cpp(t, false)

	// Launch HTTP server (front-end).
	// I can't figure out how to properly terminate the net/http serving infrastructure, so we just
	// leave the front-end running.
	if front_server == nil {
		front_server = &Server{}
		front_server.HttpPort = serverFrontPort
		front_server.BackendPort = serverBackendPort
		front_server.Log.Level = LogLevelWarn // You'll sometimes want to change this to LogLevelDebug when debugging.
		go front_server.ListenAndServe()
	}

	// Launch backend
	if !*external_backend {
		cmd := cpp_test_bin
		args := []string{}
		if *valgrind {
			cmd = "valgrind"
			//args = []string{"--leak-check=yes", cpp_test_bin}
			args = []string{"--tool=helgrind", "--suppressions=../../../valgrind-suppressions", cpp_test_bin}
		}
		cpp_server = exec.Command(cmd, args[0:]...)
		cpp_server_out = &bytes.Buffer{}
		cpp_server_err = &bytes.Buffer{}
		cpp_server.Stdout = cpp_server_out
		cpp_server.Stderr = cpp_server_err
		if err := cpp_server.Start(); err != nil {
			t.Fatalf("Failed to launch cpp backend: %v", err)
		}
	}

	// Wait for backend to come alive
	startWait := time.Now()
	timeout := 5 * time.Second
	time.Sleep(10 * time.Millisecond)
	for time.Now().Sub(startWait) < timeout {
		resp, err := pingClient.Get(baseUrl + "/ping")
		if err == nil {
			io.Copy(ioutil.Discard, resp.Body)
			resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				break
			} else {
				t.Logf("Waiting for backend to come alive... code = %v\n", resp.StatusCode)
			}
		} else {
			t.Logf("Waiting for backend to come alive... error = %v\n", err)
		}
		time.Sleep(50 * time.Millisecond)
	}
	if time.Now().Sub(startWait) >= timeout {
		t.Fatalf("Timed out waiting for backend to come alive")
	}
}

func doRequest(t *testing.T, method string, url string, body string) *http.Response {
	if *verbose_http {
		fmt.Printf("%v %v\n", method, url)
	}
	var bodyReader io.Reader
	if method == "POST" || method == "PUT" {
		bodyReader = bytes.NewReader([]byte(body))
	}
	req, err := http.NewRequest(method, baseUrl+url, bodyReader)
	if err != nil {
		t.Fatalf("%v: Error creating request: %v", url, err)
	}
	resp, err := requestClient.Do(req)
	if err != nil {
		t.Fatalf("%v: Error executing request: %v", url, err)
	}
	return resp
}

func testRequest(t *testing.T, method string, url string, body string, expectCode int, expectBody string) {
	resp := doRequest(t, method, url, body)
	defer resp.Body.Close()
	respBodyB, err := ioutil.ReadAll(resp.Body)
	respBody := string(respBodyB)
	if err != nil {
		t.Fatalf("%v: Error reading response body: %v", url, err)
	}
	if resp.StatusCode != expectCode {
		t.Errorf("%v:\nexpected code (%v)\nreceived code (%v)", url, expectCode, resp.StatusCode)
	}
	if respBody != expectBody {
		t.Errorf("%v:\nexpected (%v)\nreceived (%v)", url, expectBody, respBody)
	}
}

func testPost(t *testing.T, url string, body string, expectCode int, expectBody string) {
	testRequest(t, "POST", url, body, expectCode, expectBody)
}

func testGet(t *testing.T, url string, expectCode int, expectBody string) {
	testRequest(t, "GET", url, "", expectCode, expectBody)
}

func generateBuf(numBytes int) string {
	if numBytes > 1024*1024 && *verbose_http {
		fmt.Printf("Generating %v junk buffer\n", numBytes)
	}
	buf := make([]byte, 0, numBytes)
	for i := 0; len(buf) < numBytes; i++ {
		chunk := fmt.Sprintf("%v_", len(buf))
		if len(buf)+len(chunk) > numBytes {
			chunk = chunk[0 : numBytes-len(buf)]
		}
		buf = append(buf, []byte(chunk)...)
	}
	if numBytes > 1024*1024 && *verbose_http {
		fmt.Printf("Generation done\n")
	}
	return string(buf)
}

func withCombinations(t *testing.T, f func()) {
	testGet(t, "/control?MaxAutoBufferSize=50000000", 200, "")
	f()
	testGet(t, "/control?MaxAutoBufferSize=0", 200, "")
	f()
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

func TestHelloWorld(t *testing.T) {
	restart(t)
	testPost(t, "/echo", "Hello!", 200, "Hello!")
}

func TestEcho(t *testing.T) {
	restart(t)
	withCombinations(t, func() {
		for chunked := 0; chunked < 2; chunked++ {
			maxResponseSize := chunked * 4000
			url := fmt.Sprintf("/echo?MaxTransmitBodyChunkSize=%v", maxResponseSize)

			testPost(t, url, "Hello!", 200, "Hello!")

			smallBuf := generateBuf(5 * 1024)
			testPost(t, url, smallBuf, 200, smallBuf)

			bigBuf := generateBuf(3 * 1024 * 1024)
			testPost(t, url, bigBuf, 200, bigBuf)
		}
	})
}

func TestTimeout(t *testing.T) {
	restart(t)
	oldTimeout := front_server.BackendTimeout
	defer func() {
		front_server.BackendTimeout = oldTimeout
	}()
	// cpp server sleeps for 100 ms
	front_server.BackendTimeout = 50 * time.Millisecond
	testGet(t, "/timeout", 504, "httpbridge backend timeout\n") // I don't understand why the Go HTTP server infrastructure is adding this \n
}

func TestThreadedBackend(t *testing.T) {
	restart(t)
	nthreads := 8
	done := make(chan bool)
	for i := 0; i < nthreads; i++ {
		go func() {
			for j := 0; j < 2000; j++ {
				msg := fmt.Sprintf("(%v,%v) (Thread: %v, Request number: %v) (%v,%v)", i, j, i, j, i, j)
				testPost(t, "/echo-thread", msg, 200, msg)
			}
			done <- true
		}()
	}
	for i := 0; i < nthreads; i++ {
		<-done
	}
}
