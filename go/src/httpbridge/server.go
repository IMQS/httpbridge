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
// is a very frequent occurrence (ie we have more data than we can send in an instant).
// So, this queue is here mainly as a threshold that we can use to detect when to tell a backend
// that it must pause sending on a particular stream. Once we see that the client queue has drained,
// then we can inform the backend that it is once again clear to send.
// The number 10 is a thumb suck, and is just intended as a performance safeguard until we've
// implemented rate limiting. I think that a reasonable buffer size is somewhere between 1 and 3.
const responseChanBufferSize = 10

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
	responsesLock sync.Mutex
	responses     map[streamID]responseChan

	stoppedChan chan bool // We never send anything to this channel. But a select{} will wake when the channel is closed, which is how this get used.

	atomics *serverAtomics
}

// We place all atomic int64 variables in a struct of their own, to guarantee 64-bit alignment.
type serverAtomics struct {
	nextChannel uint64
	stopped     int32
}

type backendConnection struct {
	con            net.Conn
	id             backendID
	disconnectChan chan bool // We never send anything to this channel. But a select{} will wake when the channel is closed, which is how this get used.
}

func (s *Server) ListenAndServe() error {
	var err error
	enableHTTPListener := !s.DisableHttpListener
	s.nextBackendID = 1
	s.atomics = new(serverAtomics)
	s.stoppedChan = make(chan bool)
	s.responses = make(map[streamID]responseChan)
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
	done := make(chan bool)
	if enableHTTPListener {
		go func() {
			s.httpServer.Serve(s.httpListener)
			done <- true
		}()
	}
	go func() {
		s.AcceptBackendConnections()
		done <- true
	}()
	if enableHTTPListener {
		<-done // wait for HTTP accepter to finish
	}
	<-done // wait for Backend accepter to finish
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
		if findRetries < 5 {
			s.Log.Errorf("Error retrieving httpbridge backend conn for port %v: %v\n", s.BackendPort, err)
			findRetries++
			time.Sleep(time.Second*2)
		} else {
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

	responseChan := s.registerStream(channel, stream)
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
		res, responseFrame := s.sendBody(w, req, backend, channel, stream, responseChan)
		switch res {
		case sendBodyResult_Done:
		case sendBodyResult_SentError:
			sendResponse = false
		case sendBodyResult_PrematureResponse:
			// Backend has sent a response while we were still busy transmitting the body of the request.
			// The only case where we expect this is when the backend decides that the request is taking
			// too much memory/space. For example, a chunked upload that ends up being too large.
			s.Log.Infof("httpbridge request %v:%v aborted prematurely", channel, stream)
			go func() {
				// Because our response channel is not buffered, we need to fire up another goroutine
				// to resend the response frame, otherwise we'd block here.
				responseChan <- responseFrame
			}()
		case sendBodyResult_ServerStop:
			sendResponse = false
		}
	}

	if sendResponse {
		s.sendResponse(w, req, backend, channel, stream, responseChan)
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

	if err := s.endFrameAndSend(backend.con, builder); err != nil {
		http.Error(w, fmt.Sprintf("Error writing headers to backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
		return false
	}

	return true
}

// Send the body from the client to the backend
func (s *Server) sendBody(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, responseChan responseChan) (sendBodyResult, *TxFrame) {
	// I have NO IDEA what this buffer size should be. Thoughts revolve around the size of a regular ethernet frame (1522 bytes),
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
		case frame := <-responseChan:
			return sendBodyResult_PrematureResponse, frame
		case <-s.stoppedChan:
			return sendBodyResult_ServerStop, nil
		default:
			// continue transmitting body
		}

		var buf []byte
		// Only upgrade to a larger buffer if the user has actually sent enough bytes. This limits amplification attacks.
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
			s.sendAbortFrame(channel, stream, req, backend)
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

		if err := s.endFrameAndSend(backend.con, builder); err != nil {
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

func (s *Server) startFrame(builder *flatbuffers.Builder, frameType int8, channel, stream uint64, req *http.Request) {
	TxFrameStart(builder)
	TxFrameAddFrametype(builder, frameType)
	TxFrameAddVersion(builder, makeTxHttpVersionNumber(req))
	TxFrameAddChannel(builder, channel)
	TxFrameAddStream(builder, stream)
}

func (s *Server) sendAbortFrame(channel, stream uint64, req *http.Request, backend *backendConnection) {
	builder := flatbuffers.NewBuilder(frameBaseSize)
	s.startFrame(builder, TxFrameTypeAbort, channel, stream, req)
	if err := s.endFrameAndSend(backend.con, builder); err != nil {
		s.Log.Warnf("httpbridge Error sending ABORT frame to backend %v (%v)", backend.id, err)
	}
}

func (s *Server) endFrameAndSend(dst io.Writer, builder *flatbuffers.Builder) error {
	frame := TxFrameEnd(builder)
	builder.Finish(frame)
	frame_size := uint32(builder.Offset())

	// Our frame is done. We now prepend our frame with a 32-bit Little Endian frame size, and we are ready to transmit

	builder.Prep(flatbuffers.SizeUint32, flatbuffers.SizeUint32)
	builder.PrependUint32(frame_size)

	frame_buf := builder.Bytes[builder.Head() : builder.Head()+builder.Offset()]

	return s.sendBytes(dst, frame_buf)
}

// I suspect a more idiomatic way of writing this function would be to use io.Copy()
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

func (s *Server) sendResponse(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, responseChan responseChan) {
	haveSentHeader := false
	for done := false; !done; {
		// If we've already sent the header of the response to the client, and the backend faults
		// by timing out, or disconnecting, then we can't send another header, so we have to just
		// return, and let the Go HTTP serving infrastructure handle the abort to the client.
		select {
		case frame := <-responseChan:
			if err := s.sendResponseFrame(w, req, backend, channel, stream, frame); err != nil {
				if err != io.EOF {
					// The client is unable to receive our response, so inform the backend to stop trying
					// to send any more data.
					s.sendAbortFrame(channel, stream, req, backend)
				}
				return
			} else {
				haveSentHeader = true
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
			done = true
		}
	}

	// Backend Timeout; Drain the response channel
	for {
		select {
		case frame := <-responseChan:
			if (frame.Flags() & TxFrameFlagsFinal) != 0 {
				s.Log.Warnf("httpbridge timed-out response is finished: %v:%v", channel, stream)
				return
			}
		case <-backend.disconnectChan:
			return
		case <-s.stoppedChan:
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
				s.Log.Debugf("HB response is chunked")
			} else {
				s.Log.Debugf("HB sending header (%v)=(%v)", keyStr, valStr)
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
			s.Log.Errorf("httpbridge error writing body: %v:%v %v", channel, stream, err)
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

func (s *Server) AcceptBackendConnections() {
	for {
		con, err := s.backendListener.Accept()
		if err != nil {
			s.Log.Errorf("Error in HttpBridge Connection Accept: %v", err)
			break
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
	// One may be tempted here to try and reuse a single buffer to read incoming data.
	// That doesn't work though, because you end up having to produce a copy of the entire
	// frame, so that you can use that as a flatbuffer which you pass out to the channel
	// that ends up sending the data back over HTTP.
	// To keep things simple, we never read over a frame boundary.
	s.addBackend(backend)
	s.Log.Infof("New httpbridge backend connection on port %v. ID: %v", s.BackendPort, backend.id)
	var err error
	buf := []byte{}
	bufSize := 0 // distinct from len(buf). bufSize is how many bytes we've actually read.
	for {
		var nbytes int
		maxRead := clampInt(bufSize, 1024, 70*1024) // 64k of data plus some headers. no science behind these numbers.
		frameSize := 0
		if bufSize >= 4 {
			frameSize = int(binary.LittleEndian.Uint32(buf[:4]))
		} else {
			maxRead = 4
		}

		// Never read over a frame boundary
		if maxRead > frameSize+4-bufSize {
			maxRead = frameSize + 4 - bufSize
		}
		for len(buf)-bufSize < maxRead {
			buf = append(buf, 0)
		}
		nbytes, err = backend.con.Read(buf[bufSize : bufSize+maxRead])
		if err != nil {
			break
		}
		bufSize += nbytes
		s.Log.Debugf("HB Received %v bytes from backend %v (%v)", nbytes, backend.id, bufSize)
		if frameSize != 0 && bufSize >= 4+frameSize {
			frame := GetRootAsTxFrame(buf[4:], 0)
			rchan := s.findStreamChannel(frame.Channel(), frame.Stream())
			if rchan != nil {
				s.Log.Debugf("HB Sending frame to chan")
				rchan <- frame
			}
			buf = []byte{}
			bufSize = 0
		} else {
			s.Log.Debugf("HB Have %v/%v frame bytes", bufSize-4, frameSize)
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

func (s *Server) registerStream(channel, stream uint64) responseChan {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	if _, ok := s.responses[sid]; ok {
		s.Log.Fatalf("httpbridge registerStream called twice on the same stream (%v:%v)", channel, stream)
	}
	c := make(responseChan, responseChanBufferSize)
	s.responses[sid] = c
	s.responsesLock.Unlock()
	return c
}

func (s *Server) unregisterStream(channel, stream uint64) {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	if _, ok := s.responses[sid]; !ok {
		s.Log.Fatalf("httpbridge unregisterStream called on a non-existing stream (%v:%v)", channel, stream)
	}
	delete(s.responses, sid)
	s.responsesLock.Unlock()
}

func (s *Server) findStreamChannel(channel, stream uint64) responseChan {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	rchan, ok := s.responses[sid]
	if !ok {
		s.Log.Warnf("httpbridge findStreamChannel failed. Looks like backend has timed out (channel %v, stream %v)", channel, stream)
		return nil
	}
	s.responsesLock.Unlock()
	return rchan
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
