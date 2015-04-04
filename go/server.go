package httpbridge

import (
	"encoding/binary"
	"fmt"
	flatbuffers "github.com/google/flatbuffers/go"
	"io"
	"log"
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
	Log            *log.Logger

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
	if s.Log == nil {
		s.Log = log.New(os.Stdout, "", 0)
	}
	httpPort := s.HttpPort
	if httpPort == "" {
		httpPort = ":http"
	}
	backendPort := s.BackendPort
	if backendPort == "" {
		backendPort = ":81"
	}
	s.Log.Printf("http-bridge server starting")
	s.Log.Printf("  http port     %v", httpPort)
	s.Log.Printf("  backend port  %v", backendPort)
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

	if !s.sendHeaderFrame(w, req, backend, channel, stream) {
		return
	}
	if req.Body != nil && req.ContentLength != 0 {
		if !s.sendBody(w, req, backend, channel, stream) {
			return
		}
	}

	//<-responseChan
	//w.Header().Set("Content-Length", "5")
	//w.WriteHeader(200)
	//w.Write([]byte("hello"))
	s.sendResponse(w, req, backend, channel, stream, responseChan)
}

func (s *Server) sendHeaderFrame(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64) bool {
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

	// Frame
	TxFrameStart(builder)
	TxFrameAddFrametype(builder, TxFrameTypeHeader)
	TxFrameAddVersion(builder, makeTxHttpVersionNumber(req))
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
	static_buf := [4096]byte{}
	buf := static_buf[:]
	offset := uint64(0)
	eof := false

	for !eof {
		nread, err := req.Body.Read(buf)
		eof = err == io.EOF
		if err != nil && !eof {
			http.Error(w, fmt.Sprintf("Error reading body for backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
			return false
		}

		builder := flatbuffers.NewBuilder(nread + 100)

		body := builder.CreateByteVector(buf[:nread])

		TxFrameStart(builder)
		TxFrameAddFrametype(builder, TxFrameTypeBody)
		TxFrameAddVersion(builder, makeTxHttpVersionNumber(req))
		TxFrameAddChannel(builder, channel)
		TxFrameAddStream(builder, stream)
		TxFrameAddBodyOffset(builder, offset)
		TxFrameAddBodyTotalLength(builder, uint64(req.ContentLength))
		TxFrameAddBody(builder, body)

		if err := s.endFrameAndSend(backend.con, builder); err != nil {
			http.Error(w, fmt.Sprintf("Error writing body to backend %v (%v)", backend.id, err), http.StatusGatewayTimeout)
			return false
		}

		offset += uint64(nread)
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

	for {
		select {
		case frame := <-responseChan:
			lastMsg = time.Now()
			s.sendResponseFrame(w, req, backend, channel, stream, frame)
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

func (s *Server) sendResponseFrame(w http.ResponseWriter, req *http.Request, backend *backendConnection, channel, stream uint64, frame *TxFrame) {
	s.Log.Printf("sendResponseFrame: %v %v %v", channel, stream, frame.Frametype())
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
		s.Log.Printf("Sending header (%v)=(%v)", string(key), string(val))
		w.Header().Set(string(key), string(val))
	}
	s.Log.Printf("Writing status (%v)", statusCode)
	w.WriteHeader(statusCode)
	// this is ridiculous. We need to just use strings instead. or ask on the flatbuffers mailing list.
	body := []byte{}
	for i := 0; i < frame.BodyLength(); i++ {
		body = append(body, frame.Body(i))
	}
	s.Log.Printf("Writing body (len = %v) (%v)", len(body), string(body))
	for start := 0; start != len(body); {
		written, err := w.Write(body[start:])
		if err != nil {
			s.Log.Printf("Error writing body: %v %v %v", channel, stream, err)
			break
		}
		s.Log.Printf("Wrote %v bytes", written)
		start += written
	}
	s.Log.Printf("Done writing response")
}

func (s *Server) AcceptBackendConnections() {
	for {
		con, err := s.backendListener.Accept()
		if err != nil {
			s.Log.Printf("Error in Bridge Connection Accept: %v", err)
			break
		}
		backend := &backendConnection{
			con: con,
			id:  0,
		}
		go s.HandleBackendConnection(backend)
	}
}

func (s *Server) HandleBackendConnection(backend *backendConnection) {
	// I first thought that I'd use a circular buffer here, to store data that
	// we receive from the backend. That has complications though, because
	// flatbuffers needs a linear buffer as input. Instead, I've opted for
	// optimistically resetting a buffer to zero, assuming that there will fairly
	// often be points where our buffer ends precisely on the edge of a frame,
	// allowing us to rewind the buffer. We can even take that a step further
	// and only read as much as one frame, thereby ensuring that we frequently
	// allow ourselves to reset our buffer..
	s.addBackend(backend)
	s.Log.Printf("New backend connection %v", backend.id)
	var err error
	buf := []byte{}
	bufStart := 0
	bufEnd := 0
	for {
		var nbytes int
		for len(buf)-bufStart < 1024 {
			buf = append(buf, 0)
		}
		nbytes, err = backend.con.Read(buf[bufStart:])
		if err != nil {
			break
		}
		bufEnd += nbytes
		s.Log.Printf("Received %v bytes from backend %v", nbytes, backend.id)
		//sid := makeStreamID(backend., stream)
		if bufEnd-bufStart >= 4 {
			frameSize := int(binary.LittleEndian.Uint32(buf[bufStart : bufStart+4]))
			if bufEnd-bufStart >= 4+frameSize {
				bufStart += 4
				// TODO: get rid of this copy. Can do so by letting a single linear byte buffer be used for each mesasge.
				bufCopy := make([]byte, frameSize)
				copy(bufCopy, buf[bufStart:bufStart+frameSize])
				s.Log.Printf("frameSize: %v, len(bufCopy): %v", frameSize, len(bufCopy))
				frame := GetRootAsTxFrame(bufCopy, 0)
				rchan := s.findStreamChannel(frame.Channel(), frame.Stream())
				if rchan != nil {
					s.Log.Printf("Sending frame to chan")
					rchan <- frame
				}

				bufStart += frameSize
				if bufStart == bufEnd {
					// rewind the buffer -- NOTE this means we can avoid the mem copy for the above code
					bufStart = 0
					bufEnd = 0
				} else if len(buf) > bufStart*10 {
					// force rewind of buffer if more than 10x waste
					buf2 := make([]byte, bufEnd-bufStart)
					copy(buf2, buf[bufStart:bufEnd])
					buf = buf2
					bufStart = 0
					bufEnd = len(buf2)
				}
			}
		}
	}
	s.Log.Printf("Closing backend connection %v (%v)", backend.id, err)
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
		s.Log.Panicf("registerStream called twice on the same stream (%v:%v)", channel, stream)
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
		s.Log.Panicf("unregisterStream called on a non-existing stream (%v:%v)", channel, stream)
	}
	delete(s.responses, sid)
	s.responsesLock.Unlock()
}

func (s *Server) findStreamChannel(channel, stream uint64) responseChan {
	s.responsesLock.Lock()
	sid := makeStreamID(channel, stream)
	rchan, ok := s.responses[sid]
	if !ok {
		s.Log.Printf("findStreamChannel failed. Looks like backend has timed out (channel %v, stream %v)", channel, stream)
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
