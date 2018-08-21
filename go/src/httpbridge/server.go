package httpbridge

import (
	"encoding/binary"
	"fmt"
	flatbuffers "github.com/google/flatbuffers/go"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

type backendID int64
type streamID string
type responseChan chan *TxFrame

// Allocate this much size up front for frame buffer, so that flatbuffer doesn't need to be reallocated.
// When last checked, actual size was around 40 bytes.
const frameBaseSize = 100

// Number of frames that we will queue up in each response channel. This is tricky territory.
// Why do we want any queue here at all?
// If we don't have a queue, then the big main loop inside handleBackendConnection, which fetches
// data out of the backend's TCP socket, will block if it tries to write to a responseChan that
// is full. This will happen when the TCP socket that is writing to the client stalls, which
// is a very frequent occurrence (ie we are exceeding the data rate of the browser's TCP socket).
// So, this queue is here partly as a threshold that we can use to detect when to tell a backend
// that it must pause sending on a particular stream. Once we see that the client queue has drained,
// then we inform the backend that it is once again clear to send.
// This is an annoying burden to place on the client, and it may someday be necessary to implement
// a send buffer on the client side, so that most high level code doesn't need to worry about this,
// and it can rely instead on the backend to automatically buffer up some amount of transmit data,
// for paused streams. Increasing the buffer size here is definitely not an ideal thing to do, because
// it adds more memory pressure to the single load balancer process.
// We make this number large - much larger than responseChanBufferHigh - because once a channel
// is full, we are totally hosed.
const responseChanBufferSize = 200

// When the queue length of a response channel reaches this number, we send a PAUSE frame to the backend.
const responseChanBufferHigh = 15

// When the queue length of a response channel drops to this number, we send a RESUME frame to the backend.
const responseChanBufferLow = 5

// This is always on, but I leave the switch up here to make testing easier.
const enablePause = true

const magicFrameMarker = 0x48426268

type sendBodyResult int

const (
	sendBodyResult_Done sendBodyResult = iota
	sendBodyResult_SentError
	sendBodyResult_PrematureResponse
	sendBodyResult_ServerStop
)

// HttpBridge Server
//
// When Stop() is called, we try to stop sending all communication, to backends or clients.
// We assume that the closed TCP socket will be interpreted correctly by backends and clients.
// The alternative is to send an ABORT frame to backends, but that poses the risk of delaying
// shutdown time significantly, for no apparent gain.
type Server struct {
	// If you already have an HTTP listener, then you can forward requests from there
	// to this Server, by calling Server.ServeHTTP(). In such a case, you'll want to
	// set DisableHttpListener to true.
	DisableHttpListener bool
	HttpPort            string
	BackendPort         string
	BackendTimeout      time.Duration
	Log                 Logger

	httpServer      http.Server
	httpListener    net.Listener
	backendListener net.Listener

	// Access to 'backends' and 'nextBackendID' is guarded by 'backendsLock'
	backends      []*backendConnection
	backendsLock  sync.Mutex
	nextBackendID backendID

	// 'responses' holds a response channel for each HTTP2 stream
	// Access to 'responses' is guarded by 'responsesLock'. You do not need to own the lock
	// to send data to a channel. The lock is only guarding the map data structure.
	// However, if you use streamInfo.state, then you must do so using atomics
	responsesLock sync.Mutex
	responses     map[streamID]*streamInfo

	stoppedChan chan bool // We never send anything to this channel. But a select{} will wake when the channel is closed, which is how this get used.

	atomics *serverAtomics
}

// We place all atomic int64 variables in a struct of their own, to guarantee 64-bit alignment.
type serverAtomics struct {
	nextChannel uint64
	stopped     int32
}

type streamState uint32

const (
	streamStateActive streamState = iota
	streamStatePaused
	streamStateAborted
)

type streamInfo struct {
	state streamState // This is manipulated atomically. Use getState() and setState(), which do atomic accesses.
	rchan responseChan
}

func (i *streamInfo) getState() streamState {
	return streamState(atomic.LoadUint32((*uint32)(&i.state)))
}

func (i *streamInfo) setState(s streamState) {
	mem := (*uint32)(&i.state)
	if s == streamStateAborted {
		atomic.StoreUint32(mem, uint32(s))
	} else {
		// Make sure that we never bring a stream out of the aborted state, so...
		// Try Active -> NewState,
		// Try Paused -> NewState,
		// But never try Aborted -> NewState
		if !atomic.CompareAndSwapUint32(mem, uint32(streamStateActive), uint32(s)) {
			atomic.CompareAndSwapUint32(mem, uint32(streamStatePaused), uint32(s))
		}
	}
}

type backendConnection struct {
	con            net.Conn
	id             backendID
	disconnectChan chan bool  // We never send anything to this channel. But a select{} will wake when the channel is closed, which is how this get used.
	conWriteLock   sync.Mutex // Take this whenever you send a frame to con. This is necessary so that a partial send doesn't end up splicing two frames into each other.
}

func (s *Server) ListenAndServe() error {
	var err error
	enableHTTPListener := !s.DisableHttpListener
	s.nextBackendID = 1
	s.atomics = new(serverAtomics)
	s.stoppedChan = make(chan bool)
	s.responses = make(map[streamID]*streamInfo)
	if s.BackendTimeout == 0 {
		s.BackendTimeout = time.Second * 120
	}
	if s.Log.Target == nil {
		s.Log.Target = os.Stdout
	}
	httpPort := s.HttpPort
	if httpPort == "" {
		httpPort = ":http"
	}
	if !enableHTTPListener {
		httpPort = "disabled"
	}
	backendPort := s.BackendPort
	if backendPort == "" {
		backendPort = ":81"
	}
	s.Log.Infof("http-bridge server starting (http port %v, backend port %v)", httpPort, backendPort)
	s.httpServer.Handler = s
	if enableHTTPListener {
		if s.httpListener, err = net.Listen("tcp", httpPort); err != nil {
			return err
		}
	}
	if s.backendListener, err = net.Listen("tcp", backendPort); err != nil {
		return err
	}
	errPipe := make(chan error)
	defer close(errPipe)
	if enableHTTPListener {
		go func() {
			errPipe <- s.httpServer.Serve(s.httpListener)
		}()
	}
	go func() {
		errPipe <- s.AcceptBackendConnections()
	}()

	for err := range errPipe {
		// loop does blocking read on channel and will only exit after non-nil error is received
		if err != nil {
			return err
		}
	}
	// unreachable
	return nil
}

// By the time ServeHTTP is called, the header has been received. The body
// may still be busy transmitting though.
func (s *Server) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	if req.Body != nil {
		defer req.Body.Close()
	}

	// Temp: We currently have problems between the router's httpbridge server and the client in ImqsCrud.
	// The ping route /crud/ping also fails intermittently, so we add this retry mechanism and log to try and
	// determine where the problem lies.
	var backend *backendConnection
	findRetries := 0
	for {
		var err error
		backend, err = s.findBackend(req)
		if err == nil {
			break
		}
		if findRetries < 3 {
			findRetries++
			time.Sleep(time.Second * 1)
		} else {
			s.Log.Errorf("Error retrieving httpbridge backend conn for port %v: %v\n", s.BackendPort, err)
			http.Error(w, err.Error(), http.StatusGatewayTimeout)
			return
		}
	}

	// This is not true under HTTP/2. I haven't bothered yet to try and determine the channel correctly.
	// For now we just pretend that we're running under HTTP/1.1, although with an unlimited number of
	// simultaneous connections from the client.
	channel := atomic.AddUint64(&s.atomics.nextChannel, 1)

	// This goes hand in hand with our phoney channel number. I haven't checked, but from the spec,
	// I assume that the first HTTP/2 stream from a client will usually be 3.
	// If you fix channel, then you must also fix stream. Our Stream IDs depend upon the combination
	// of channel + stream being unique for every request/response.
	stream := uint64(3)

	streamInfo := s.registerStream(channel, stream)
	defer s.unregisterStream(channel, stream)

	// ContentLength is -1 when unknown
	hasBody := req.Body != nil && req.ContentLength != 0

	if s.Log.Level <= LogLevelDebug {
		s.Log.Debugf("HB Request %v:%v started (%v)", channel, stream, req.URL.String())
	}

	if !s.sendHeaderFrame(w, req, backend, channel, stream, hasBody) {
		return
	}

	sendResponse := true
	if hasBody {
		res, responseFrame := s.sendBody(w, req, backend, channel, stream, streamInfo)
		switch res {
		case sendBodyResult_Done:
		case sendBodyResult_SentError:
			sendResponse = false
		case sendBodyResult_PrematureResponse:
			// Backend has sent a response while we were still busy transmitting the body of the request.
			// The only case where we expect this is when the backend decides that the request is taking
			// too much memory/space. For example, a chunked upload that ends up being too large.
			s.Log.Infof("httpbridge request %v:%v aborted prematurely", channel, stream)
			// Put the frame back into the queue, and let sendResponse send it.
			streamInfo.rchan <- responseFrame
		case sendBodyResult_ServerStop:
			sendResponse = false
		}
	}

	if sendResponse {
		s.sendResponse(w, req, backend, channel, stream, streamInfo)
	}

	s.Log.Debugf("HB Request %v:%v finished", channel, stream)
}

func (s *Server) sendHeaderFrame(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, hasBody bool) bool {
	builder := flatbuffers.NewBuilder(1000)

	// Headers
	header_lines := []flatbuffers.UOffsetT{}

	// First header line is special "GET /uri" (key is HTTP method, value is HTTP Path)
	method := createByteVectorFromString(builder, req.Method)
	uri := createByteVectorFromString(builder, req.RequestURI)
	TxHeaderLineStart(builder)
	TxHeaderLineAddKey(builder, method)
	TxHeaderLineAddValue(builder, uri)
	header_lines = append(header_lines, TxHeaderLineEnd(builder))

	// Header lines proper
	haveContentLength := false
	for k, varr := range req.Header {
		if k == "Content-Length" {
			haveContentLength = true
		}
		fbKey := createByteVectorFromString(builder, k)

		for _, v := range varr {
			fbVal := createByteVectorFromString(builder, v)
			TxHeaderLineStart(builder)
			TxHeaderLineAddKey(builder, fbKey)
			TxHeaderLineAddValue(builder, fbVal)
			header_lines = append(header_lines, TxHeaderLineEnd(builder))
		}
	}

	// Unknown content length, so write -1 for it, so that backend knows this is coming.
	// Both httpbridge and Go use -1 to signal unknown content length.
	if !haveContentLength && hasBody {
		key := createByteVectorFromString(builder, "Content-Length")
		val := createByteVectorFromString(builder, "-1")
		TxHeaderLineStart(builder)
		TxHeaderLineAddKey(builder, key)
		TxHeaderLineAddValue(builder, val)
		header_lines = append(header_lines, TxHeaderLineEnd(builder))
	}

	TxFrameStartHeadersVector(builder, len(header_lines))
	for i := len(header_lines) - 1; i >= 0; i-- {
		builder.PrependUOffsetT(header_lines[i])
	}
	headers := builder.EndVector(len(header_lines))

	flags := byte(0)
	if !hasBody {
		flags |= TxFrameFlagsFinal
	}

	// Frame
	s.startFrame(builder, TxFrameTypeHeader, channel, stream, req)
	TxFrameAddFlags(builder, flags)
	TxFrameAddHeaders(builder, headers)

	if err := s.endFrameAndSend(backend, builder); err != nil {
		http.Error(w, fmt.Sprintf("Error writing headers to backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
		return false
	}

	return true
}

// Send the body from the client to the backend
func (s *Server) sendBody(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, info *streamInfo) (sendBodyResult, *TxFrame) {
	// I have no idea what this buffer size should be. Thoughts revolve around the size of a regular ethernet frame (1522 bytes),
	// or jumbo frames (9000 bytes). Also, you have the multiple simultaneous streams to consider (ie you don't want to bloat
	// up your front-end's memory with buffers). If the HTTP process and the backend are on the same machine, then I'm guessing you'd
	// want a buffer quite a bit bigger than an ethernet frame.
	// We start with a statically allocated buffer of 8K, and switch to a dynamically allocated buffer of 32K if the
	// body is large. These numbers are thumb suck.
	const dynamic_size = 32 * 1024
	var dynamic_buf []byte
	static_buf := [8 * 1024]byte{}
	total_body_sent := 0
	eof := false

	s.Log.Debug("HB sendBody START")

	for !eof {
		// Check to see if the backend has sent a premature response, or the server is shutting down
		select {
		case frame := <-info.rchan:
			return sendBodyResult_PrematureResponse, frame
		case <-s.stoppedChan:
			return sendBodyResult_ServerStop, nil
		default:
			// continue transmitting body
		}

		// Only upgrade to a larger buffer if the user has actually sent enough bytes. This limits amplification attacks.
		var buf []byte
		if total_body_sent >= dynamic_size {
			if dynamic_buf == nil {
				dynamic_buf = make([]byte, dynamic_size)
			}
			buf = dynamic_buf[:]
		} else {
			buf = static_buf[:]
		}
		nread, err := req.Body.Read(buf)
		eof = err == io.EOF
		if err != nil && !eof {
			s.abortStream(channel, stream, info, backend)
			http.Error(w, fmt.Sprintf("Error reading body for backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
			return sendBodyResult_SentError, nil
		}

		s.Log.Debugf("HB sendBody %v", nread)

		total_body_sent += nread

		fbSizeEstimate := nread + frameBaseSize
		builder := flatbuffers.NewBuilder(fbSizeEstimate)
		body := builder.CreateByteVector(buf[:nread])
		flags := byte(0)
		if eof {
			flags |= TxFrameFlagsFinal
		}
		s.startFrame(builder, TxFrameTypeBody, channel, stream, req)
		TxFrameAddFlags(builder, flags)
		TxFrameAddBody(builder, body)

		if err := s.endFrameAndSend(backend, builder); err != nil {
			http.Error(w, fmt.Sprintf("Error writing body to backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
			return sendBodyResult_SentError, nil
		}
		if len(builder.FinishedBytes()) > fbSizeEstimate {
			// This is a major performance issue, and I don't expect it to ever happen
			s.Log.Warnf("httpbridge Body flatbuffer was larger than estimated (%v > %v)", len(builder.FinishedBytes()), fbSizeEstimate)
		}
	}

	return sendBodyResult_Done, nil
}

func (s *Server) sendControlFrame(frameType int8, channel, stream uint64, backend *backendConnection) {
	builder := flatbuffers.NewBuilder(frameBaseSize)
	s.startFrame(builder, frameType, channel, stream, nil)
	if err := s.endFrameAndSend(backend, builder); err != nil {
		s.Log.Warnf("httpbridge Error sending %v frame to backend %v (%v)", EnumNamesTxFrameType[int(frameType)], backend.id, err)
	}
}

func (s *Server) abortStream(channel, stream uint64, info *streamInfo, backend *backendConnection) {
	s.Log.Infof("httpbridge aborting stream %v:%v on backend %v", channel, stream, backend.id)
	info.setState(streamStateAborted)
	s.sendControlFrame(TxFrameTypeAbort, channel, stream, backend)
}

func (s *Server) startFrame(builder *flatbuffers.Builder, frameType int8, channel, stream uint64, req *http.Request) {
	TxFrameStart(builder)
	TxFrameAddFrametype(builder, frameType)
	if req != nil {
		TxFrameAddVersion(builder, makeTxHttpVersionNumber(req))
	}
	TxFrameAddChannel(builder, channel)
	TxFrameAddStream(builder, stream)
}

func (s *Server) endFrameAndSend(backend *backendConnection, builder *flatbuffers.Builder) error {
	frame := TxFrameEnd(builder)
	builder.Finish(frame)
	frame_size := uint32(builder.Offset())

	// Our frame is done.
	// We now prepend our frame with a 32-bit Little Endian frame size
	builder.Prep(flatbuffers.SizeUint32, flatbuffers.SizeUint32)
	builder.PrependUint32(frame_size)

	// Also add our magic marker, which is just here to catch bugs in our frame packing/unpacking code
	builder.Prep(flatbuffers.SizeUint32, flatbuffers.SizeUint32)
	builder.PrependUint32(magicFrameMarker)

	frame_buf := builder.Bytes[builder.Head() : builder.Head()+builder.Offset()]

	backend.conWriteLock.Lock()
	err := s.sendBytes(backend.con, frame_buf)
	backend.conWriteLock.Unlock()
	return err
}

func (s *Server) sendBytes(dst io.Writer, buf []byte) error {
	for len(buf) != 0 {
		nwrite, err := dst.Write(buf)
		if err != nil {
			return err
		}
		buf = buf[nwrite:]
	}
	return nil
}

func (s *Server) sendResponse(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, info *streamInfo) {
	haveSentHeader := false
	for {
		// If we've already sent the header of the response to the client, and the backend faults
		// by timing out, or disconnecting, then we can't send another header, so we have to just
		// return, and let the Go HTTP serving infrastructure handle the abort to the client.
		select {
		case frame := <-info.rchan:
			if err := s.sendResponseFrame(w, req, backend, channel, stream, frame); err != nil {
				if err != io.EOF {
					// The client is unable to receive our response, so inform the backend to stop trying
					// to send any more data.
					s.abortStream(channel, stream, info, backend)
				}
				return
			} else {
				haveSentHeader = true
			}

			if info.getState() == streamStatePaused && len(info.rchan) <= responseChanBufferLow {
				// Resume
				//fmt.Printf("Resuming %v:%v\n", channel, stream)
				s.sendControlFrame(TxFrameTypeResume, channel, stream, backend)
				info.setState(streamStateActive)
			}
		case <-backend.disconnectChan:
			if !haveSentHeader {
				http.Error(w, "httpbridge backend disconnect", http.StatusBadGateway)
			}
			return
		case <-s.stoppedChan:
			return
		case <-time.After(s.BackendTimeout):
			if !haveSentHeader {
				http.Error(w, "httpbridge backend timeout", http.StatusGatewayTimeout)
			}
			s.Log.Warnf("httpbridge backend timeout: %v:%v", channel, stream)
			s.abortStream(channel, stream, info, backend)
			return
		}
	}
}

// Returns io.EOF when the stream is finished
func (s *Server) sendResponseFrame(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, frame *TxFrame) error {
	if frame.Frametype() == TxFrameTypeAbort {
		// Don't do anything else - it's possible that we've already sent the header out, so it's pointless trying to send
		// another one. If the backend had an intelligible error to send, then it would have sent it as a plain old HTTP
		// response. ABORT from the backend is used when something unexpected happens during the transmission of a response,
		// such as out of memory.
		return io.EOF
	}

	body := frame.BodyBytes()
	s.Log.Debugf("HB sendResponseFrame: %v:%v %v", channel, stream, frame.Frametype())
	if frame.Frametype() == TxFrameTypeHeader {
		line := &TxHeaderLine{}
		frame.Headers(line, 0)
		codeStr := [3]byte{}
		codeStr[0] = line.Key(0)
		codeStr[1] = line.Key(1)
		codeStr[2] = line.Key(2)
		statusCode, _ := strconv.Atoi(string(codeStr[:]))
		keyBuf := [40]byte{}
		valBuf := [100]byte{}
		for i := 1; i < frame.HeadersLength(); i++ {
			frame.Headers(line, i)
			key := keyBuf[:0]
			val := valBuf[:0]
			for j := 0; j < line.KeyLength(); j++ {
				key = append(key, line.Key(j))
			}
			for j := 0; j < line.ValueLength(); j++ {
				val = append(val, line.Value(j))
			}
			// We're explicitly not supporting multiple lines with the same key here. So multiple cookies won't work; I think.
			keyStr := string(key)
			valStr := string(val)
			if keyStr == "Content-Length" && valStr == "-1" {
				// Don't write our special chunked Content-Length value of -1 to the client
				//s.Log.Debugf("HB response is chunked")
			} else {
				//s.Log.Debugf("HB sending header (%v)=(%v)", keyStr, valStr)
				w.Header().Set(keyStr, valStr)
			}
		}
		s.Log.Debugf("HB Writing status (%v)", statusCode)
		w.WriteHeader(statusCode)
	}
	s.Log.Debugf("HB Writing body (len = %v)", len(body))
	for start := 0; start != len(body); {
		written, err := w.Write(body[start:])
		if err != nil {
			// This is typically hit when the client closes the connection before receiving the entire response
			s.Log.Infof("httpbridge error writing body: %v:%v %v", channel, stream, err)
			return err
		}
		s.Log.Debugf("HB Wrote %v body bytes", written)
		start += written
	}
	s.Log.Debugf("HB Done writing response frame")
	if (frame.Flags() & TxFrameFlagsFinal) != 0 {
		return io.EOF
	}
	return nil
}

func (s *Server) AcceptBackendConnections() error {
	for {
		con, err := s.backendListener.Accept()
		if err != nil {
			s.Log.Errorf("Error in HttpBridge Connection Accept: %v", err)
			return err
		}
		backend := &backendConnection{
			con:            con,
			id:             0,
			disconnectChan: make(chan bool),
		}
		go s.handleBackendConnection(backend)
	}
}

func clampInt(v, min, max int) int {
	if v < min {
		return min
	} else if v > max {
		return max
	}
	return v
}

func clampInt64(v, min, max int64) int64 {
	if v < min {
		return min
	} else if v > max {
		return max
	}
	return v
}

func (s *Server) handleBackendConnection(backend *backendConnection) {
	// One may be tempted here to try and reuse a single circular buffer to read incoming data.
	// That doesn't work though, because you end up having to produce a copy of the entire
	// frame, so that you can use that as a flatbuffer which you pass out to the channel
	// that ends up sending the data back over HTTP. The thing reading from that channel
	// needs a copy of the data, so that it can write it out at it's own pace,
	// hence circular buffer is out of the question.

	// However, the socket read behaviour here is not all that bad. What we do is this:
	// 1. Read 8 bytes of magic,framesize
	// 2. Read framesize + 8 bytes of next frame
	// X. Repeat (2)
	// The key thing here is that we usually perform just one read per frame. Initially,
	// we would read just 8 bytes for the magic + frame size, and then the frame, and
	// then another 8 byte read, but I then figured out that we can speculatively read
	// the first 8 bytes of the next frame, in order to get rid of that tiny 8 byte read.

	s.addBackend(backend)
	s.Log.Infof("httpbridge New backend connection on port %v. ID: %v", s.BackendPort, backend.id)
	var err error
	buf := make([]byte, 1024)
	bufSize := 0 // distinct from len(buf). bufSize is how many bytes we've actually read.
	nFrames := 0
	sentLargeFrameWarning := false
	for {
		var nbytes int
		maxBufSize := 0
		frameSize := 0
		if bufSize >= 8 {
			magic := binary.LittleEndian.Uint32(buf[0:4])
			frameSize = int(binary.LittleEndian.Uint32(buf[4:8]))
			if magic != magicFrameMarker {
				s.Log.Errorf("httpbridge Backend %v sent invalid frame #%v. First two dwords: %x %x", backend.id, nFrames, magic, frameSize)
				break
			}
			if frameSize > 100*1024*1024 && !sentLargeFrameWarning {
				sentLargeFrameWarning = true
				s.Log.Warnf("httpbridge Backend %v sent very large frame #%v. First two dwords: %x %x", backend.id, nFrames, magic, frameSize)
			}
			maxBufSize = frameSize + 16
			//fmt.Printf("Have a valid frame %x %x\n", magic, frameSize)
		} else {
			maxBufSize = 8
		}

		// Check if we need to grow buf
		if len(buf) < maxBufSize {
			if maxBufSize >= 50*1024*1024 {
				s.Log.Warnf("httpbridge frame from backend %v is %v MB", backend.id, maxBufSize/(1024*1024))
			}
			oldBuf := buf
			buf = make([]byte, maxBufSize)
			copy(buf[0:bufSize], oldBuf[0:bufSize])
		}
		//fmt.Printf("Trying to read %v:%v (%v) bytes (frameSize = %v)\n", bufSize, maxBufSize, maxBufSize-bufSize, frameSize)
		nbytes, err = backend.con.Read(buf[bufSize:maxBufSize])
		if err != nil {
			break
		}
		bufSize += nbytes

		s.Log.Debugf("HB Received %v bytes from backend %v (%v)", nbytes, backend.id, bufSize)
		if frameSize != 0 && bufSize >= 8+frameSize {
			nFrames++
			sentLargeFrameWarning = false
			// We already have the first few bytes of the next frame (hopefully 8, but not always), so save it for the new buffer
			extraSize := bufSize - (8 + frameSize)
			extraStatic := [8]byte{}
			extraBytes := []byte{}
			if extraSize != 0 {
				extraBytes = extraStatic[0:extraSize]
				copy(extraBytes[:], buf[8+frameSize:bufSize])
			}

			frame := GetRootAsTxFrame(buf[8:8+frameSize], 0)
			info := s.findStreamInfo(frame.Channel(), frame.Stream(), backend)
			if info != nil {
				s.Log.Debugf("HB Sending frame to chan")
				state := info.getState()
				if state != streamStateAborted {
					isFull := len(info.rchan) == responseChanBufferSize
					if isFull {
						// I initially thought that I could have a fallback path here, by sending the frame from a different goroutine,
						// but that doesn't work because now your frames are arriving out of order!
						s.Log.Warnf("httpbridge response channel %v:%v is full for backend %v", frame.Channel(), frame.Stream(), backend.id)
					}
					info.rchan <- frame
					if isFull {
						s.Log.Warnf("httpbridge finished sending frame to full response channel %v:%v, for backend %v", frame.Channel(), frame.Stream(), backend.id)
					}
				}
				if state == streamStateActive && len(info.rchan) >= responseChanBufferHigh && enablePause {
					// Pause
					//fmt.Printf("Pausing %v:%v\n", frame.Channel(), frame.Stream())
					info.setState(streamStatePaused)
					s.sendControlFrame(TxFrameTypePause, frame.Channel(), frame.Stream(), backend)
				}
			}

			// Unfortunately we cannot recycle 'buf', because buf now belongs to the flatbuffer
			// that we have sent to the response channel.
			// The big question is - how large should the next buffer be. We arbitrarily choose
			// to make it a little larger than the current one.
			nextBufSize := bufSize + 256
			if nextBufSize > 10*1024*1024 {
				nextBufSize = 10 * 1024 * 1024
			}
			buf = make([]byte, nextBufSize)
			bufSize = 0
			if extraSize != 0 {
				copy(buf[0:extraSize], extraBytes[:])
				bufSize = extraSize
			}
		} else {
			s.Log.Debugf("HB Have %v/%v frame bytes", bufSize-8, frameSize)
		}
	}
	s.Log.Infof("Closing httpbridge backend connection %v (%v)", backend.id, err)
	s.removeBackend(backend)
}

func (s *Server) addBackend(backend *backendConnection) {
	s.backendsLock.Lock()
	backend.id = s.nextBackendID
	s.nextBackendID++
	s.backends = append(s.backends, backend)
	s.Log.Infof("Backend %v added. %v active", backend.id, len(s.backends))
	s.backendsLock.Unlock()
}

func (s *Server) removeBackend(backend *backendConnection) {
	close(backend.disconnectChan)
	s.backendsLock.Lock()
	for i, b := range s.backends {
		if b == backend {
			s.backends = append(s.backends[:i], s.backends[i+1:]...)
			break
		}
	}
	s.Log.Infof("Backend %v removed. %v remaining", backend.id, len(s.backends))
	s.backendsLock.Unlock()
}

// At some point we'll probably want to have multiple backends, matched by HTTP route.
// Right now we simply return the one and only backend, if it exists. Otherwise null.
// Of course, if we have more than 1 backend, we could just round-robin between them,
// or whatever. BUT, before we do that, we need to establish a scheme, and do it properly.
func (s *Server) findBackend(req *http.Request) (*backendConnection, error) {
	s.backendsLock.Lock()
	defer s.backendsLock.Unlock()
	if len(s.backends) == 1 {
		return s.backends[0], nil
	} else {
		return nil, fmt.Errorf("Expected 1 httpbridge backend, but found %v", len(s.backends))
	}
}

func (s *Server) registerStream(channel, stream uint64) *streamInfo {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	if _, ok := s.responses[sid]; ok {
		s.Log.Fatalf("httpbridge registerStream called twice on the same stream (%v:%v)", channel, stream)
	}
	info := &streamInfo{
		state: streamStateActive,
		rchan: make(responseChan, responseChanBufferSize),
	}
	s.responses[sid] = info
	s.responsesLock.Unlock()
	return info
}

func (s *Server) unregisterStream(channel, stream uint64) {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	if _, ok := s.responses[sid]; !ok {
		s.Log.Fatalf("httpbridge unregisterStream called on a non-existing stream (%v:%v)", channel, stream)
	}
	delete(s.responses, sid)
	// We don't close the channel here, because writing to a closed channel causes a panic.
	// How could we end up trying to write to this channel if it's closed? That could come
	// about if we, for some reason, sent an ABORT to the backend for this stream, but
	// the backend only processes that message after trying to send another frame for
	// for the stream. So, we just let the garbage collector handle this, and allow those
	// stray frames to disappear.
	s.responsesLock.Unlock()
}

func (s *Server) findStreamInfo(channel, stream uint64, backend *backendConnection) *streamInfo {
	s.responsesLock.Lock()
	defer s.responsesLock.Unlock()
	sid := makeStreamID(channel, stream)
	info, ok := s.responses[sid]
	if !ok {
		// This happens if a stream has been aborted, but the backend hasn't noticed that yet
		s.Log.Infof("httpbridge findStreamInfo failed %v:%v for backend %v", channel, stream, backend.id)
		return nil
	}
	return info
}

func (s *Server) Stop() {
	atomic.StoreInt32(&s.atomics.stopped, 1)
	close(s.stoppedChan)
	s.backendListener.Close()
	s.httpListener.Close()
}

func (s *Server) isStopped() bool {
	return atomic.LoadInt32(&s.atomics.stopped) != 0
}

func makeStreamID(channel, stream uint64) streamID {
	return streamID(fmt.Sprintf("%v:%v", channel, stream))
}

func makeTxHttpVersionNumber(req *http.Request) int8 {
	switch req.ProtoMajor {
	case 1:
		switch req.ProtoMinor {
		case 0:
			return TxHttpVersionHttp10
		default:
			return TxHttpVersionHttp11
		}
	default:
		return TxHttpVersionHttp2
	}
}

// flatbuffer helpers

func createByteVectorFromString(b *flatbuffers.Builder, value string) flatbuffers.UOffsetT {
	return b.CreateByteVector([]byte(value))
}
