package sxchange

import (
	"bytes"
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"math"
	"net"
	"runtime/debug"
	"sync"
	"time"
)

// The wire format is described in PROTOCOL.md.
const (
	verHI = 2
	verLO = 0

	// offsets in the hello packet
	helloSize      = 1324
	helloNonce     = 8 // 32 random bytes with a key, zero without
	helloMaxSize   = 40
	helloSizeBytes = 44
	helloFixedSize = 300

	undefinedType = 0x80 // SizeBytes of a slot without a type: -128
	tagSize       = 16   // AES-GCM tag
)

// Limit for the handshake when ReadTimeout or WriteTimeout is zero: a silent
// peer must not hold a server forever. A variable for the tests.
var handshakeTimeout = 10 * time.Second

var (
	crc32q            = crc32.MakeTable(crc32.Koopman)
	errNotConnected   = errors.New("not connected")
	errAlreadyListens = errors.New("already listening")
	errNilCallback    = errors.New("onConnect and onError must not be nil")
	errWrongKey       = errors.New("HS error - wrong key or handshake modified in transit")
	errAuth           = errors.New("message authentication failed")
)

// CB type for callback function for each type of transferred data.
// The slice is reused for the next message: copy it if it has to outlive the callback.
type CB func([]byte, *Connection)

// DataTypeCB describes one message type: how its size goes over the wire and
// what to call when a message of this type arrives.
type DataTypeCB struct {
	SizeBytes int8  // -1 for fixed size, 0 for no data (ping?), 1 for 0-255 bytes, 2 for 0-65535 bytes... up to 4
	FixedSize int32 // if some struct has fixed size no other header needed
	Callback  CB    // which function to run after data received
}

// checkSize tells whether a message of n bytes fits the type.
func (dt DataTypeCB) checkSize(msgType uint8, n int) error {
	switch dt.SizeBytes {
	case -1:
		if n != int(dt.FixedSize) {
			return fmt.Errorf("structure type %d has fixed size %d but %d bytes given", msgType, dt.FixedSize, n)
		}
	case 0:
		if n != 0 {
			return fmt.Errorf("structure type %d has no data but %d bytes given", msgType, n)
		}
	case 1, 2, 3:
		if n >= 1<<(8*dt.SizeBytes) {
			return fmt.Errorf("structure type %d has %d-byte size header but %d bytes given", msgType, dt.SizeBytes, n)
		}
	case 4: // MaxSize is the limit
	default:
		return fmt.Errorf("structure type %d has invalid SizeBytes setting (%d)", msgType, dt.SizeBytes)
	}
	return nil
}

// Connection information both for client or server
type Connection struct {
	conn           *net.TCPConn         // real connection, set when its handshake starts
	listener       *net.TCPListener     // set by ListenOne, cleared by Close
	done           chan struct{}        // closed by Disconnect/Close: no more callbacks for conn
	connMutex      sync.Mutex           // guards conn, listener and done
	AESKey         string               // 16, 24 or 32 random bytes in hex: encrypts and authenticates the connection
	wconn          *net.TCPConn         // where WriteMsg writes: conn after its handshake, nil before it and after a failed write
	wDir           *direction           // AES-GCM state of the outgoing direction, nil without a key
	writeMutex     sync.Mutex           // guards wconn and wDir, serializes the frames
	cbMutex        sync.Mutex           // held while a callback runs
	Types          map[uint8]DataTypeCB // map of type->params&callback
	KeepAlive      time.Duration        // tcp keep-alive: idle time and probe interval; 0 = off
	ReadTimeout    time.Duration        // we need to receive new message at least once per this duration; 0 = no limit
	WriteTimeout   time.Duration        // maximal duration for every write operation; 0 = no limit
	ConnectTimeout time.Duration        // maximum duration of net.Dial; 0 = no timeout
	MaxSize        uint32               // maximum number of bytes for one record/message; 0 rejects any non-empty message
	Ctx            context.Context      // context for some values like server id
}

// direction is the AES-GCM state of one direction of a connection: its key
// and the number of the next record, which is the nonce.
type direction struct {
	aead cipher.AEAD
	seq  uint64
}

// newDirection derives the key of one direction from the shared key and the
// hello packets of both sides.
func newDirection(psk []byte, label string, helloClient, helloServer []byte) (*direction, error) {
	mac := hmac.New(sha256.New, psk)
	mac.Write([]byte(label))
	mac.Write(helloClient)
	mac.Write(helloServer)
	block, err := aes.NewCipher(mac.Sum(nil))
	if nil != err {
		return nil, err
	}
	aead, err := cipher.NewGCM(block)
	return &direction{aead: aead}, err
}

func (d *direction) nonce() []byte {
	n := make([]byte, 12)
	binary.LittleEndian.PutUint64(n, d.seq)
	d.seq++
	return n
}

// session is what the read loop needs from one established connection. A
// reconnect replaces the Connection fields, a running loop keeps its own.
type session struct {
	conn *net.TCPConn
	rDir *direction    // AES-GCM state of the incoming direction, nil without a key
	done chan struct{} // Connection.done of this connection
}

// deadline for a timeout setting, where zero means no limit
func deadline(timeout time.Duration) time.Time {
	if timeout <= 0 {
		return time.Time{}
	}
	return time.Now().Add(timeout)
}

func (c *Connection) readAll(conn *net.TCPConn, buf []byte) error {
	conn.SetReadDeadline(deadline(c.ReadTimeout))

	_, err := io.ReadFull(conn, buf)
	return err
}

// psk decodes AESKey; nil when encryption is off
func (c *Connection) psk() ([]byte, error) {
	if c.AESKey == "" {
		return nil, nil
	}
	key, err := hex.DecodeString(c.AESKey)
	if nil != err {
		return nil, err
	}
	if n := len(key); n != 16 && n != 24 && n != 32 {
		return nil, aes.KeySizeError(n)
	}
	return key, nil
}

func (c *Connection) hello(encrypted bool) ([]byte, error) {
	h := make([]byte, helloSize)
	copy(h, []byte{'s', 'x', 'c', 'h', 'n', 'g', verHI, verLO})
	if encrypted {
		h[4] = 'E'
		if _, err := rand.Read(h[helloNonce:helloMaxSize]); nil != err {
			return nil, err
		}
	}
	binary.LittleEndian.PutUint32(h[helloMaxSize:], c.MaxSize)

	for i := 0; i < 256; i++ {
		dt, ok := c.Types[uint8(i)]
		if !ok {
			h[helloSizeBytes+i] = undefinedType
			continue
		}
		h[helloSizeBytes+i] = byte(dt.SizeBytes)
		binary.LittleEndian.PutUint32(h[helloFixedSize+4*i:], uint32(dt.FixedSize))
	}
	return h, nil
}

// initConnection does the handshake; on an error it closes conn. server tells
// which side we are: the two directions have different keys.
func (c *Connection) initConnection(conn *net.TCPConn, server bool) (s session, err error) {
	defer func() {
		if nil != err {
			conn.Close()
		}
	}()

	s = session{conn: conn, done: make(chan struct{})}

	// Until the handshake is done WriteMsg gets errNotConnected. The lock is
	// not held over the handshake: a silent peer must not block the writers.
	c.writeMutex.Lock()
	c.wconn, c.wDir = nil, nil
	c.writeMutex.Unlock()

	c.connMutex.Lock()
	c.conn, c.done = conn, s.done
	c.connMutex.Unlock()

	// Go turns keep-alive on by default, 0 is "off" as in C. Not
	// SetKeepAlivePeriod: since Go 1.23 it sets the idle time only.
	conn.SetKeepAliveConfig(net.KeepAliveConfig{Enable: c.KeepAlive != 0, Idle: c.KeepAlive, Interval: c.KeepAlive})

	readTimeout, writeTimeout := c.ReadTimeout, c.WriteTimeout
	if readTimeout <= 0 {
		readTimeout = handshakeTimeout
	}
	if writeTimeout <= 0 {
		writeTimeout = handshakeTimeout
	}
	write := func(buf []byte) error {
		conn.SetWriteDeadline(time.Now().Add(writeTimeout))
		_, err := conn.Write(buf)
		return err
	}
	read := func(buf []byte) error {
		conn.SetReadDeadline(time.Now().Add(readTimeout))
		_, err := io.ReadFull(conn, buf)
		return err
	}

	psk, err := c.psk()
	if nil != err {
		return s, err
	}
	mine, err := c.hello(nil != psk)
	if nil != err {
		return s, err
	}
	peer := make([]byte, helloSize)

	if err = write(mine); nil != err {
		return s, err
	}
	if err = read(peer); nil != err {
		return s, err
	}

	// everything but the nonce has to be equal
	if !bytes.Equal(peer[:helloNonce], mine[:helloNonce]) {
		return s, errors.New("HS error - different protocol version or encryption setting")
	}
	if !bytes.Equal(peer[helloMaxSize:helloSizeBytes], mine[helloMaxSize:helloSizeBytes]) {
		return s, errors.New("HS error - different message maximal size")
	}
	if !bytes.Equal(peer[helloSizeBytes:], mine[helloSizeBytes:]) {
		return s, errors.New("HS error - different set of datatypes")
	}

	var wDir *direction
	if nil != psk {
		helloClient, helloServer := mine, peer
		if server {
			helloClient, helloServer = peer, mine
		}
		if wDir, err = newDirection(psk, "sxchnge2 client", helloClient, helloServer); nil != err {
			return s, err
		}
		if s.rDir, err = newDirection(psk, "sxchnge2 server", helloClient, helloServer); nil != err {
			return s, err
		}
		if server {
			wDir, s.rDir = s.rDir, wDir
		}

		// Key confirmation: record 0 of each direction, without data. The
		// keys depend on both hello packets, so it also proves that nobody
		// changed them on the way.
		if err = write(wDir.aead.Seal(nil, wDir.nonce(), nil, nil)); nil != err {
			return s, err
		}
		tag := make([]byte, tagSize)
		if err = read(tag); nil != err {
			return s, err
		}
		if _, err = s.rDir.aead.Open(nil, s.rDir.nonce(), tag, nil); nil != err {
			return s, errWrongKey
		}
	}

	c.writeMutex.Lock()
	c.wconn, c.wDir = conn, wDir
	c.writeMutex.Unlock()
	return s, nil
}

// readFrame reads one message of a connection without a key: type, size,
// data, crc.
func (c *Connection) readFrame(s session, buf []byte) (dt DataTypeCB, data []byte, err error) {
	var hdr [5]byte

	if err = c.readAll(s.conn, hdr[:1]); nil != err {
		return
	}

	dt, ok := c.Types[hdr[0]]
	if !ok {
		return dt, nil, fmt.Errorf("structure type %d is not defined", hdr[0])
	}
	if dt.SizeBytes < -1 || dt.SizeBytes > 4 {
		return dt, nil, fmt.Errorf("structure type %d has invalid SizeBytes setting (%d)", hdr[0], dt.SizeBytes)
	}

	var size uint64
	if dt.SizeBytes == -1 {
		size = uint64(uint32(dt.FixedSize))
	} else if dt.SizeBytes > 0 {
		sizeBuf := hdr[1 : 1+dt.SizeBytes]
		if err = c.readAll(s.conn, sizeBuf); nil != err {
			return
		}
		for i, b := range sizeBuf {
			size |= uint64(b) << (8 * i)
		}
	}

	if size > uint64(c.MaxSize) {
		return dt, nil, fmt.Errorf("size of message is greater than maximum (%d/%d)", size, c.MaxSize)
	}

	data = buf[:size]
	if err = c.readAll(s.conn, data); nil != err {
		return
	}

	if size != 0 {
		if err = c.readAll(s.conn, hdr[:4]); nil != err {
			return
		}
		if crc32.Checksum(data, crc32q) != binary.LittleEndian.Uint32(hdr[:4]) {
			return dt, nil, errors.New("crc failed on read")
		}
	}
	return dt, data, nil
}

// readRecord reads one message of a connection with a key: the length, then
// data and type encrypted, then the tag.
func (c *Connection) readRecord(s session, buf []byte) (dt DataTypeCB, data []byte, err error) {
	var lenBuf [4]byte

	if err = c.readAll(s.conn, lenBuf[:]); nil != err {
		return
	}
	n := uint64(binary.LittleEndian.Uint32(lenBuf[:])) // data and type
	if n == 0 {
		return dt, nil, errors.New("record without a type")
	}
	if n-1 > uint64(c.MaxSize) {
		return dt, nil, fmt.Errorf("size of message is greater than maximum (%d/%d)", n-1, c.MaxSize)
	}

	record := buf[:n+tagSize]
	if err = c.readAll(s.conn, record); nil != err {
		return
	}
	plain, err := s.rDir.aead.Open(record[:0], s.rDir.nonce(), record, lenBuf[:])
	if nil != err {
		return dt, nil, errAuth
	}

	msgType := plain[n-1]
	dt, ok := c.Types[msgType]
	if !ok {
		return dt, nil, fmt.Errorf("structure type %d is not defined", msgType)
	}
	return dt, plain[:n-1], dt.checkSize(msgType, int(n-1))
}

// internal run function
func (c *Connection) run(s session) (err error) {

	defer s.conn.Close()

	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("panic in sxconnection.run: %v\n%s", r, debug.Stack())
			return
		}
		select {
		case <-s.done:
			err = nil // closed by Disconnect or Close, not a failure
		default:
		}
	}()

	// one buffer for every message; with a key it also holds the type and the tag
	read, bufSize := c.readFrame, uint64(c.MaxSize)
	if nil != s.rDir {
		read, bufSize = c.readRecord, bufSize+1+tagSize
	}
	buf := make([]byte, bufSize)

	for {

		select {
		case <-s.done:
			return nil
		default:
		}

		dt, data, err := read(s, buf)
		if nil != err {
			return err
		}

		// we need to allow types that only can be send by one of the side, but also defined
		if nil != dt.Callback && !c.callback(s, dt.Callback, data) {
			return nil
		}
	}
}

// callback calls cb, unless Disconnect or Close has ended the connection: it
// returns false then. The lock keeps callbacks one at a time: after Connect on
// a connected client the read loop of the old connection may still be in one.
func (c *Connection) callback(s session, cb CB, msg []byte) bool {
	c.cbMutex.Lock()
	defer c.cbMutex.Unlock() // also when cb panics

	select {
	case <-s.done:
		return false
	default:
	}
	cb(msg, c)
	return true
}

// WriteMsg creates header and then writes msg buffer via TCP connection
func (c *Connection) WriteMsg(msgType uint8, msg []byte) error {

	dt, ok := c.Types[msgType]
	if !ok {
		return fmt.Errorf("unknown msg type %d", msgType)
	}

	if err := dt.checkSize(msgType, len(msg)); nil != err {
		return err
	}

	// the peer has the same MaxSize and would drop the connection; a record
	// counts the type byte into its 4-byte length
	if uint64(len(msg)) > uint64(c.MaxSize) || uint64(len(msg)) >= math.MaxUint32 {
		return fmt.Errorf("size of message is greater than maximum (%d/%d)", len(msg), c.MaxSize)
	}

	c.writeMutex.Lock()
	defer c.writeMutex.Unlock()

	if nil == c.wconn {
		return errNotConnected
	}

	// One buffer and one write per frame: three small writes cost three TCP
	// segments here (Go sets TCP_NODELAY).
	var frame []byte
	if nil != c.wDir {
		// length, then data and type encrypted in place, then the tag
		frame = make([]byte, 4, 4+len(msg)+1+tagSize)
		binary.LittleEndian.PutUint32(frame, uint32(len(msg)+1))
		frame = append(append(frame, msg...), msgType)
		frame = c.wDir.aead.Seal(frame[:4], c.wDir.nonce(), frame[4:], frame[:4])
	} else {
		// type, size, data, crc
		frame = make([]byte, 0, 1+4+len(msg)+4)
		frame = append(frame, msgType)
		for i := 0; i < int(dt.SizeBytes); i++ {
			frame = append(frame, byte(len(msg)>>(8*i)))
		}
		frame = append(frame, msg...)
		if len(msg) > 0 {
			frame = binary.LittleEndian.AppendUint32(frame, crc32.Checksum(msg, crc32q))
		}
	}

	c.wconn.SetWriteDeadline(deadline(c.WriteTimeout))
	if _, err := c.wconn.Write(frame); nil != err { // a short write comes with an error
		// A part of the frame may be on the wire, and the record number has
		// moved on: a frame written after it would not be accepted.
		c.wconn.Close()
		c.wconn = nil
		return err
	}
	return nil
}

// Disconnect closes the current connection, if any. A blocked read returns at
// once and no callback is called after it. A ListenOne server keeps accepting
// new connections.
func (c *Connection) Disconnect() {
	c.connMutex.Lock()
	defer c.connMutex.Unlock()
	c.disconnect()
}

// disconnect needs connMutex to be held
func (c *Connection) disconnect() {
	if nil == c.conn {
		return
	}
	select {
	case <-c.done:
	default:
		close(c.done)
	}
	c.conn.Close()
}

// Close closes the current connection and, for a ListenOne server, stops
// listening. The Connection can be used for Connect or ListenOne again.
func (c *Connection) Close() error {
	c.connMutex.Lock()
	defer c.connMutex.Unlock()

	c.disconnect()

	if nil == c.listener {
		return nil
	}
	err := c.listener.Close()
	c.listener = nil
	return err
}

// Remote address of the current connection, nil before the first one
func (c *Connection) Remote() net.Addr {
	c.connMutex.Lock()
	defer c.connMutex.Unlock()

	if nil == c.conn {
		return nil
	}

	return c.conn.RemoteAddr()
}
