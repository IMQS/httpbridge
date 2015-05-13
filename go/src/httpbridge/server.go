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

type Server struct {
	HttpPort       string
	BackendPort    string
	BackendTimeout time.Duration
	Log            Logger

	httpServer      http.Server
	httpListener    net.Listener
	backendListener net.Listener

	// Access to 'backends' and 'nextBackendID' is guarded by 'backendsLock'
	backends      []*backendConnection
	backendsLock  sync.Mutex
	nextBackendID backendID

	// responses holds response frames for each HTTP2 stream
	// Access to 'responses' is guarded by 'responsesLock'. You do not need to own the lock
	// to send data to a channel. The lock is merely guarding the map data structure.
	responsesLock sync.Mutex
	responses     map[streamID]responseChan

	atomics *serverAtomics
}

// We place all atomic int64 variables in a struct of their own, to guarantee 64-bit alignment.
type serverAtomics struct {
	nextChannel uint64
}

type backendConnection struct {
	con net.Conn
	id  backendID
}

func (s *Server) ListenAndServe() error {
	var err error
	s.nextBackendID = 1
	s.atomics = new(serverAtomics)
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
	backendPort := s.BackendPort
	if backendPort == "" {
		backendPort = ":81"
	}
	s.Log.Warnf("http-bridge server starting")
	s.Log.Warnf("  http port     %v", httpPort)
	s.Log.Warnf("  backend port  %v", backendPort)
	s.httpServer.Handler = s
	if s.httpListener, err = net.Listen("tcp", httpPort); err != nil {
		return err
	}
	if s.backendListener, err = net.Listen("tcp", backendPort); err != nil {
		return err
	}
	done := make(chan bool)
	go func() {
		s.httpServer.Serve(s.httpListener)
		done <- true
	}()
	go func() {
		s.AcceptBackendConnections()
		done <- true
	}()
	<-done
	<-done
	return nil
}

// By the time ServeHTTP is called, the header has been received. The body
// may still be busy transmitting though.
func (s *Server) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	if req.Body != nil {
		defer req.Body.Close()
	}

	backend := s.findBackend(req)
	if backend == nil {
		http.Error(w, "No httpbridge backend is listening", http.StatusGatewayTimeout)
		return
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

	hasBody := req.Body != nil && req.ContentLength != 0

	if !s.sendHeaderFrame(w, req, backend, channel, stream, hasBody) {
		return
	}
	if hasBody {
		if !s.sendBody(w, req, backend, channel, stream) {
			return
		}
	}

	s.sendResponse(w, req, backend, channel, stream, responseChan)

	s.Log.Infof("Request %v:%v finished", channel, stream)
}

func (s *Server) sendHeaderFrame(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, hasBody bool) bool {
	builder := flatbuffers.NewBuilder(0)

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
	for k, varr := range req.Header {
		fbKey := createByteVectorFromString(builder, k)

		for _, v := range varr {
			fbVal := createByteVectorFromString(builder, v)
			TxHeaderLineStart(builder)
			TxHeaderLineAddKey(builder, fbKey)
			TxHeaderLineAddValue(builder, fbVal)
			header_lines = append(header_lines, TxHeaderLineEnd(builder))
		}
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
	TxFrameStart(builder)
	TxFrameAddFrametype(builder, TxFrameTypeHeader)
	TxFrameAddVersion(builder, makeTxHttpVersionNumber(req))
	TxFrameAddFlags(builder, flags)
	TxFrameAddChannel(builder, channel)
	TxFrameAddStream(builder, stream)
	TxFrameAddHeaders(builder, headers)

	if err := s.endFrameAndSend(backend.con, builder); err != nil {
		http.Error(w, fmt.Sprintf("Error writing headers to backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
		return false
	}

	return true
}

func (s *Server) sendBody(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64) bool {
	// I have NO IDEA what this buffer size should be. Thoughts revolve around the size of a regular ethernet frame (1522 bytes),
	// or jumbo frames (9000 bytes). Also, you have the multiple simultaneous streams to consider (ie you don't want to bloat
	// up your front-end's memory with buffers). If the HTTP process and the backend are on the same machine, then I'm guessing you'd
	// want a buffer quite a bit bigger than an ethernet frame.
	static_buf := [8 * 1024]byte{}
	total_body_sent := 0
	eof := false
	var dynamic_buf []byte
	dynamic_buf_size := int(clampInt64(req.ContentLength, int64(len(static_buf)), 64*1024))

	s.Log.Debug("sendBody START")

	for !eof {
		var buf []byte
		// Only upgrade to a larger buffer if the user has actually sent 8KB. This limits amplification attacks.
		if total_body_sent >= 8*1024 && dynamic_buf_size >= len(static_buf) {
			if dynamic_buf == nil {
				dynamic_buf = make([]byte, dynamic_buf_size)
			}
			buf = dynamic_buf[:]
		} else {
			buf = static_buf[:]
		}
		nread, err := req.Body.Read(buf)
		eof = err == io.EOF
		if err != nil && !eof {
			// TODO: Send an ABORT frame
			http.Error(w, fmt.Sprintf("Error reading body for backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
			return false
		}

		s.Log.Debugf("sendBody %v", nread)

		builder := flatbuffers.NewBuilder(nread + 100)
		body := builder.CreateByteVector(buf[:nread])
		total_body_sent += nread

		flags := byte(0)
		if eof {
			flags |= TxFrameFlagsFinal
		}

		TxFrameStart(builder)
		TxFrameAddFrametype(builder, TxFrameTypeBody)
		TxFrameAddVersion(builder, makeTxHttpVersionNumber(req))
		TxFrameAddFlags(builder, flags)
		TxFrameAddChannel(builder, channel)
		TxFrameAddStream(builder, stream)
		TxFrameAddBody(builder, body)

		if err := s.endFrameAndSend(backend.con, builder); err != nil {
			http.Error(w, fmt.Sprintf("Error writing body to backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
			return false
		}
	}

	return true
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
	lastMsg := time.Now()
	remainingBytes := int64(1)

	for remainingBytes > 0 {
		select {
		case frame := <-responseChan:
			lastMsg = time.Now()
			res := s.sendResponseFrame(w, req, backend, channel, stream, frame)
			if frame.Frametype() == TxFrameTypeHeader {
				remainingBytes = res
			} else {
				remainingBytes -= res
			}
			return
		case <-time.After(time.Second * 3):
			if time.Now().Sub(lastMsg) > s.BackendTimeout {
				http.Error(w, "Backend timeout", http.StatusGatewayTimeout)
				return
			}
		}
	}

	//http.Error(w, "Message dispatched", http.StatusOK)
}

func (s *Server) sendResponseFrame(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, frame *TxFrame) int64 {
	remaining_bytes := int64(0)
	body := frame.BodyBytes()
	s.Log.Debugf("sendResponseFrame: %v:%v %v", channel, stream, frame.Frametype())
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
			s.Log.Debugf("Sending header (%v)=(%v)", keyStr, valStr)
			w.Header().Set(keyStr, valStr)
			if keyStr == "Content-Length" {
				remaining_bytes, _ = strconv.ParseInt(valStr, 10, 64)
				remaining_bytes -= int64(len(body))
			}
		}
		s.Log.Debugf("Writing status (%v)", statusCode)
		w.WriteHeader(statusCode)
	}
	s.Log.Debugf("Writing body (len = %v)", len(body))
	for start := 0; start != len(body); {
		written, err := w.Write(body[start:])
		if err != nil {
			s.Log.Errorf("Error writing body: %v:%v %v", channel, stream, err)
			break
		}
		s.Log.Debugf("Wrote %v body bytes", written)
		start += written
	}
	s.Log.Debugf("Done writing response frame")
	if frame.Frametype() == TxFrameTypeHeader {
		return remaining_bytes
	} else {
		return int64(len(body))
	}
}

func (s *Server) AcceptBackendConnections() {
	for {
		con, err := s.backendListener.Accept()
		if err != nil {
			s.Log.Errorf("Error in Bridge Connection Accept: %v", err)
			break
		}
		backend := &backendConnection{
			con: con,
			id:  0,
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
	s.Log.Warnf("New backend connection %v", backend.id)
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
		s.Log.Debugf("Received %v bytes from backend %v (%v)", nbytes, backend.id, bufSize)
		if frameSize != 0 && bufSize >= 4+frameSize {
			frame := GetRootAsTxFrame(buf[4:], 0)
			rchan := s.findStreamChannel(frame.Channel(), frame.Stream())
			if rchan != nil {
				s.Log.Debugf("Sending frame to chan")
				rchan <- frame
			}
			buf = []byte{}
			bufSize = 0
		} else {
			s.Log.Debugf("Have %v/%v frame bytes", bufSize-4, frameSize)
		}
	}
	s.Log.Warnf("Closing backend connection %v (%v)", backend.id, err)
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
func (s *Server) findBackend(req *http.Request) *backendConnection {
	s.backendsLock.Lock()
	var match *backendConnection
	if len(s.backends) == 1 {
		match = s.backends[0]
	}
	s.backendsLock.Unlock()
	return match
}

func (s *Server) registerStream(channel, stream uint64) responseChan {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	if _, ok := s.responses[sid]; ok {
		s.Log.Fatalf("registerStream called twice on the same stream (%v:%v)", channel, stream)
	}
	c := make(responseChan)
	s.responses[sid] = c
	s.responsesLock.Unlock()
	return c
}

func (s *Server) unregisterStream(channel, stream uint64) {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	if _, ok := s.responses[sid]; !ok {
		s.Log.Fatalf("unregisterStream called on a non-existing stream (%v:%v)", channel, stream)
	}
	delete(s.responses, sid)
	s.responsesLock.Unlock()
}

func (s *Server) findStreamChannel(channel, stream uint64) responseChan {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	rchan, ok := s.responses[sid]
	if !ok {
		s.Log.Warnf("findStreamChannel failed. Looks like backend has timed out (channel %v, stream %v)", channel, stream)
		return nil
	}
	s.responsesLock.Unlock()
	return rchan
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
