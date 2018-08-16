package httpbridge

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"math/rand"
	"net/http"
	"os"
	"os/exec"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

const (
	serverFrontPort   = "127.0.0.1:8080"
	serverBackendPort = "127.0.0.1:8081"
	baseUrl           = "http://" + serverFrontPort
)

const BodyDontCare = "<ANYTHING>"

var cpp_server *exec.Cmd
var cpp_server_out *bytes.Buffer
var cpp_server_err *bytes.Buffer
var front_server *Server
var pingClient http.Client
var requestClient http.Client
var slowReaderSuicide = errors.New("Slow reader suicide")

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
	cmdErr := &bytes.Buffer{}
	cmd.Stdout = cmdOut
	cmd.Stderr = cmdErr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("Build of test-backend failed: %v\n%v\n%v", err, string(cmdOut.Bytes()), string(cmdErr.Bytes()))
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
	kill_front()
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
		} else {
			fmt.Printf("Error stopping cpp server: %v\n", err)
		}
	}

	if cpp_server != nil {
		cpp_server.Wait()
		fmt.Printf("cpp server stopped\n")
		if t != nil {
			t.Logf("cpp server stopped")
		}
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
	//kill_cpp(t, false)

	// Launch HTTP server (front-end).
	// I can't figure out how to properly terminate the net/http serving infrastructure, so we just
	// leave the front-end running.
	if front_server == nil {
		front_server = &Server{}
		front_server.HttpPort = serverFrontPort
		front_server.BackendPort = serverBackendPort
		front_server.Log.Level = LogLevelInfo // You'll sometimes want to change this to LogLevelDebug when debugging.
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
	time.Sleep(30 * time.Millisecond)
	for time.Now().Sub(startWait) < timeout {
		fmt.Printf("ping...\n")
		resp, err := pingClient.Get(baseUrl + "/ping")
		if err == nil {
			io.Copy(ioutil.Discard, resp.Body)
			resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				fmt.Printf("Backend is alive\n")
				t.Logf("Backend is alive")
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

// A wrapped reader that reads in predictable chunks.
// Interval between chunks is predictable.
// Max size of each chunk is predictable.
type slowReader struct {
	raw           io.Reader
	lastTime      time.Time
	chunkMaxSize  int
	chunkInterval time.Duration
	totalRead     int
	errorAt       int
}

type slowReaderFull struct {
	slowReader
}

func (s *slowReader) Read(p []byte) (int, error) {
	if s.errorAt != 0 && s.totalRead >= s.errorAt {
		return 0, slowReaderSuicide
	}
	sleep_for := s.lastTime.Add(s.chunkInterval).Sub(time.Now())
	if sleep_for > 0 {
		time.Sleep(sleep_for)
	}
	start := time.Now()
	maxRead := len(p)
	if s.chunkMaxSize < maxRead {
		maxRead = s.chunkMaxSize
	}
	buf := make([]byte, maxRead)
	n, err := s.raw.Read(buf)
	s.totalRead += n
	copy(p, buf[:n])
	if err == nil {
		s.lastTime = start
		//fmt.Printf("Slow sending %v bytes (total = %v)\n", n, s.totalRead)
	} else {
		fmt.Printf("Error slow sending %v\n", err)
	}
	return n, err
}

func newSlowReader(raw io.Reader, intervalTime time.Duration, bytesPerSecond int) *slowReader {
	return &slowReader{
		raw:           raw,
		lastTime:      time.Now(),
		chunkMaxSize:  bytesPerSecond,
		chunkInterval: intervalTime,
	}
}

// Wraps a buffer in a dumb reader, that provides ONLY the Read() interface
// This prevents the Go http runtime from figuring out the content length,
// so it transmits the request chunked.
type dumbReader struct {
	raw io.Reader
}

func (r *dumbReader) Read(p []byte) (int, error) {
	return r.raw.Read(p)
}

func doRequestAndReadSlowly(t *testing.T, url string, bytesPerSecond int, totalRead *uint64) {
	req, err := http.NewRequest("GET", baseUrl+url, nil)
	if err != nil {
		t.Fatalf("%v: Error creating request: %v", url, err)
	}
	resp, err := requestClient.Do(req)
	if err != nil {
		t.Fatalf("%v: Error executing request: %v", url, err)
	}
	defer resp.Body.Close()
	chunkSize := 65536
	if bytesPerSecond > 0 {
		chunkSize = bytesPerSecond / 5
		if chunkSize > 65536 {
			chunkSize = 65536
		}
	}
	// if bytesPerSecond is more than threshold, then never sleep
	enableThrottle := bytesPerSecond > 0 && bytesPerSecond < 10*1000*1000
	chunk := make([]byte, chunkSize)
	start := time.Now()
	pos := 0
	for {
		ideal_pos := int(time.Now().Sub(start).Seconds() * float64(bytesPerSecond))
		if enableThrottle && pos > ideal_pos {
			// slow down by a random amount. Randomness prevents a thundering hurd.
			time.Sleep(time.Duration(rand.Int31n(20)) * time.Millisecond)
		} else {
			// catch up
			n, err := resp.Body.Read(chunk)
			//fmt.Printf("SlowRead %v (%v)\n", n, err)
			pos += n
			atomic.AddUint64(totalRead, uint64(n))
			if err == io.EOF {
				break
			}
			if err != nil {
				t.Fatalf("Error slow reading from %v: %v", url, err)
			}
		}
	}
}

// In order to get a chunked request, set bodyLen = -1. In addition, you must use an io.Reader
// that doesn't support seeking, so that the Go http runtime can't figure out the length
// of the stream. Our slowReader is just such an io.Reader.
func doRequest(t *testing.T, method string, url string, bodyLen int, body io.Reader) *http.Response {
	if *verbose_http {
		fmt.Printf("%v %v\n", method, url)
	}
	req, err := http.NewRequest(method, baseUrl+url, body)
	if err != nil {
		t.Fatalf("%v: Error creating request: %v", url, err)
	}
	if bodyLen != -1 && body != nil {
		req.ContentLength = int64(bodyLen)
	}
	resp, err := requestClient.Do(req)
	if err != nil {
		t.Fatalf("%v: Error executing request: %v", url, err)
	}
	return resp
}

func testRequest(t *testing.T, method string, url string, bodyLen int, body io.Reader, expectCode int, expectBody string) {
	resp := doRequest(t, method, url, bodyLen, body)
	defer resp.Body.Close()
	respBodyB, err := ioutil.ReadAll(resp.Body)
	respBody := string(respBodyB)
	if err != nil {
		t.Fatalf("%v: Error reading response body: %v", url, err)
	}
	if resp.StatusCode != expectCode {
		t.Fatalf("%v:\nexpected code (%v)\nreceived code (%v)", url, expectCode, resp.StatusCode)
	}
	if expectBody != BodyDontCare && respBody != expectBody {
		n1 := 100
		if len(expectBody) < n1 {
			n1 = len(expectBody)
		}
		n2 := 100
		if len(respBody) < n2 {
			n2 = len(respBody)
		}
		t.Fatalf("%v:\nexpected %v(%v)\nreceived %v(%v)", url, len(expectBody), expectBody[:n1], len(respBody), respBody[:n2])
	}
}

func testPost(t *testing.T, url string, body string, expectCode int, expectBody string) {
	testRequest(t, "POST", url, len(body), bytes.NewReader([]byte(body)), expectCode, expectBody)
}

func testPostBodyReader(t *testing.T, url string, bodyLen int, body io.Reader, expectCode int, expectBody string) {
	testRequest(t, "POST", url, bodyLen, body, expectCode, expectBody)
}

func testGet(t *testing.T, url string, expectCode int, expectBody string) {
	testRequest(t, "GET", url, 0, nil, expectCode, expectBody)
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

func withCombinations(t *testing.T, f func(title string)) {
	testGet(t, "/control?MaxAutoBufferSize=50000000", 200, "")
	f("MaxAutoBufferSize=50000000")

	testGet(t, "/control?MaxAutoBufferSize=0", 200, "")
	f("MaxAutoBufferSize=0")
}

func TestMain(m *testing.M) {
	fmt.Printf("TestMain enter\n")
	if err := setup(); err != nil {
		fmt.Printf("SETUP ERROR: %v\n", err)
		os.Exit(1)
	}
	res := m.Run()
	teardown()
	os.Exit(res)
	fmt.Printf("TestMain exit: %v\n", res)
}

func TestHelloWorld(t *testing.T) {
	restart(t)
	testPost(t, "/echo", "Hello!", 200, "Hello!")
}

func TestEcho(t *testing.T) {
	restart(t)

	withCombinations(t, func(title string) {
		for singleResponseFrame := 0; singleResponseFrame < 2; singleResponseFrame++ {
			maxResponseSize := singleResponseFrame * 4000 // 0 = disabled (ie no limit on frame size)
			url := fmt.Sprintf("/echo?MaxTransmitBodyChunkSize=%v", maxResponseSize)
			t.Logf("Mode: %v. URL: %v\n", title, url)

			testPost(t, url, "Hello!", 200, "Hello!")

			smallBuf := generateBuf(5 * 1024)
			testPost(t, url, smallBuf, 200, smallBuf)

			bigBuf := generateBuf(3 * 1024 * 1024)
			testPost(t, url, bigBuf, 200, bigBuf)
		}
	})
}

func TestChunkedRequest(t *testing.T) {
	restart(t)
	buf := generateBuf(3 * 1024 * 1024)
	testPostBodyReader(t, "/echo", -1, &dumbReader{bytes.NewReader([]byte(buf))}, 200, buf)
}

func TestChunkedResponse(t *testing.T) {
	restart(t)
	buf := generateBuf(3 * 1024 * 1024)
	testPostBodyReader(t, "/echo?NoContentLength=1", -1, bytes.NewReader([]byte(buf)), 200, buf)
}

func TestTooLongURI(t *testing.T) {
	restart(t)
	url := "/" + generateBuf(65537)
	testGet(t, url, 414, "")
}
func TestURI(t *testing.T) {
	restart(t)
	testGet(t, "/echo-path/hello", 200, "/echo-path/hello")
	testGet(t, "/echo-path/hello%20%25!", 200, "/echo-path/hello %!")
}

func TestServerOutOfMemory_Early(t *testing.T) {
	restart(t)

	// This tests an early path in the C++ code, where it rejects the message
	// before receiving the body for it.
	testGet(t, "/control?MaxWaitingBufferTotal=5", 200, "")
	testPost(t, "/echo", generateBuf(100), 503, "")
}

func TestServerOutOfMemory_Late(t *testing.T) {
	restart(t)

	// This tests a late path in the C++ code, where it's busy trying to
	// buffer up a body frame, and only then does it notice that it would
	// exceed it's quota. This is not a typical scenario, because it implies
	// changing the quota after bootup. However, it IS an error path in
	// the code, so we should check it.
	// Additionally, this also happens to stress the ABORT path from
	// backend to server, where it aborts an upload in progress.

	// The combination of timings here is crucial, because the Go http client
	// seems to buffer up request data until we hit around 5 KB of data,
	// before it starts sending the request. We need our backend to receive
	// that initial header frame, and mark the request as buffered, so that
	// we can trigger the code path that we're looking for.
	const bytesPerChunk = 1000
	const sleepTime = 1 * time.Second

	// Start a slow upload
	testGet(t, "/control?MaxWaitingBufferTotal=1000000", 200, "")
	body := generateBuf(10 * 1024 * 1024)
	slow := newSlowReader(bytes.NewReader([]byte(body)), 100*time.Millisecond, bytesPerChunk)
	wait := make(chan bool)
	go func() {
		fmt.Printf("Start slow sending\n")
		testPostBodyReader(t, "/echo", len(body), slow, 503, BodyDontCare)
		fmt.Printf("Done slow sending\n")
		wait <- true
	}()

	// Give the slow /echo send some time to get going before bringing the limit back down
	// Note that the timing here is crucial. See the comment above the timings constants.
	time.Sleep(sleepTime)

	fmt.Printf("Reducing time\n")
	testGet(t, "/control?MaxWaitingBufferTotal=1", 200, "")
	fmt.Printf("Done reducing time\n")
	<-wait
}

// Test connection throughput by firing up numConnections simultaneous connections,
// with an expected aggregate throughput equal to averageAggregateBytesPerSecond.
func testThroughputWith(t *testing.T, numConnections int, averageAggregateBytesPerSecond int, withSlow bool) {
	restart(t)
	totalExpected := uint64(0)
	totalRead := uint64(0)
	done := uint64(0)
	duration_seconds := 10

	// Create some slow and some fast readers
	numSlow := numConnections / 2
	if !withSlow {
		numSlow = 0
	}
	// The slow speed can't be TOO slow, otherwise it takes too long to fill the TCP buffer.
	slowSpeed := 200000
	fastSpeed := (averageAggregateBytesPerSecond - (numSlow * slowSpeed)) / (numConnections - numSlow)

	const meg = 1024 * 1024

	// You must make the minimum size quite large, like 2MB, otherwise the TCP receive buffer
	// of the HTTP client just eats it all up, and you don't end up with blocking buffers on the send side.
	minSize := tcpBufferSize

	fmt.Printf("Theoretical speed: %v MB/s\n", (numSlow*slowSpeed+(numConnections-numSlow)*fastSpeed)/meg)
	fmt.Printf("Slow speed: %v KB/s\n", slowSpeed/1024)
	fmt.Printf("Fast speed: %v KB/s\n", fastSpeed/1024)

	for i := 0; i < numConnections; i++ {
		launch := func(bytesPerSecond int) {
			fmt.Printf("Launching %v\n", bytesPerSecond)
			size := minSize + bytesPerSecond*duration_seconds
			limit := bytesPerSecond
			if !withSlow {
				limit = -1
			}
			atomic.AddUint64(&totalExpected, uint64(size))
			doRequestAndReadSlowly(t, fmt.Sprintf("/garbage-stream?Size=%v", size), limit, &totalRead)
			if bytesPerSecond == slowSpeed {
				fmt.Printf("Finished slow\n")
			} else {
				fmt.Printf("Finished fast\n")
			}
			atomic.AddUint64(&done, 1)
		}
		if i < numSlow {
			go launch(slowSpeed)
		} else {
			go launch(fastSpeed)
		}
	}

	start := time.Now()
	totalReadAtStart := atomic.LoadUint64(&totalRead)

	for {
		elapsed := time.Now().Sub(start)
		bps := float64(atomic.LoadUint64(&totalRead)-totalReadAtStart) / elapsed.Seconds()
		expected := float64(averageAggregateBytesPerSecond)
		bps_percent := 100.0 * float64(bps) / float64(expected)
		meg := 1024.0 * 1024.0
		fmt.Printf("MB per second: %.3f (%.1f%%) (expected %v, %v connections)\n", bps/meg, bps_percent, expected/meg, numConnections)
		totalReadAtStart = atomic.LoadUint64(&totalRead)
		start = time.Now()
		time.Sleep(1 * time.Second)
		if atomic.LoadUint64(&done) == uint64(numConnections) {
			break
		}
	}
	if totalRead != totalExpected {
		t.Errorf("Total bytes read != expected bytes read (%v expected. %v actual)", totalExpected, totalRead)
	}
}

func TestThroughputSlow(t *testing.T) {

	/* How to validate this test?

	Run "go test -v httpbridge -run TestThroughputSlow", and you should see:

		MB per second: 246.416 (82.1%) (expected 300, 2 connections)
		MB per second: 240.714 (80.2%) (expected 300, 2 connections)
		MB per second: 229.215 (76.4%) (expected 300, 2 connections)
		MB per second: 237.031 (79.0%) (expected 300, 2 connections)
		MB per second: 235.924 (78.6%) (expected 300, 2 connections)

	Then, in server.go, set enablePause = false, and enableFallbackForFullChannels = false, and run the same test again, and you should see:

		MB per second: 3.069 (1.0%) (expected 300, 2 connections)
		MB per second: 0.191 (0.1%) (expected 300, 2 connections)
		MB per second: 0.691 (0.2%) (expected 300, 2 connections)
		MB per second: 0.191 (0.1%) (expected 300, 2 connections)
		MB per second: 0.191 (0.1%) (expected 300, 2 connections)

	There are some tweaky numbers that you need to get right here in order for this
	issue to show it's face:

	* responseChanBufferSize must not be too large
	* The minimum size of the response must be around 2MB (see minSize in testThroughputWith), otherwise
		the OS TCP receive buffer of the HTTP client eats up all the slack, and you never get
		TCP blocking.
	* The "slow" speed must be reasonably high, otherwise you never end up saturating that 2MB TCP buffer.

	*/
	testThroughputWith(t, 2, 300*1024*1024, true)
}

func TestThroughputManySmall(t *testing.T) {
	// On my Sandy Bridge 2600K, I get about 300 MB/s. Peak observed: 343
	testThroughputWith(t, 300, 300*1024*1024, false)
}

func TestThroughputFewBig(t *testing.T) {
	testThroughputWith(t, 3, 300*1024*1024, false)
}

func TestTimeout(t *testing.T) {
	restart(t)
	oldTimeout := front_server.BackendTimeout
	defer func() {
		front_server.BackendTimeout = oldTimeout
		fmt.Printf("timeout restored to %v\n", oldTimeout)
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

func TestAbortedRequest(t *testing.T) {
	restart(t)

	// Start a slow upload, which we abort midway
	// We can't really test this well from the Go side - it's really here to stress the C++
	// backend code, to make sure that it handles aborts correctly.
	// When developing this, I would place some breakpoints and printf's inside the
	// C++ code to sense-check it.
	bpc := 10000
	body := generateBuf(5 * 1024 * 1024)
	slow := newSlowReader(bytes.NewReader([]byte(body)), 100*time.Millisecond, bpc)
	slow.errorAt = bpc * 2
	wait := make(chan bool)
	go func() {
		req, _ := http.NewRequest("GET", baseUrl+"/echo", slow)
		req.ContentLength = int64(len(body))
		_, err := requestClient.Do(req)
		if strings.Index(err.Error(), "suicide") == -1 {
			t.Errorf("Expected error containing %v", slowReaderSuicide.Error())
		}
		// Give the backend some time to receive the abort frame
		time.Sleep(10 * time.Millisecond)
		wait <- true
	}()

	<-wait
}

// Client abandons download
func TestAbortedResponseFromClient(t *testing.T) {
	restart(t)
	wait := make(chan bool)
	go func() {
		// keep this size below the auto-buffered size, for the echo-thread's sake
		body := generateBuf(10 * 1024 * 1024)
		req, _ := http.NewRequest("GET", baseUrl+"/echo-thread?MaxTransmitBodyChunkSize=65536", bytes.NewReader([]byte(body)))
		resp, err := requestClient.Do(req)
		if err != nil {
			t.Errorf("Expected success\n")
		}
		// Close the body early
		resp.Body.Close()
		wait <- true
	}()

	<-wait
}
