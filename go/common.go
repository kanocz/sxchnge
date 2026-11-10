package sxchange

import (
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"log"
	"net"
	"runtime/debug"
	"sync"
	"time"
	"unsafe"
)

const (
	verHI = 1
	verLO = 1
)

var (
	crc32q            = crc32.MakeTable(crc32.Koopman)
	errNotConnected   = errors.New("not connected")
	errAlreadyListens = errors.New("already listening")
	errNilCallback    = errors.New("onConnect and onError must not be nil")
)

// CB type for callback function for each type of transferred data.
// The slice is reused for the next message: copy it if it has to outlive the callback.
type CB func([]byte, *Connection)

// DataTypeCB describes one message type: how its size goes over the wire and
// what to call when a message of this type arrives.
type DataTypeCB struct {
	SizeBytes int8  // -1 for fixed size, 0 for no data (ping?), 1 for 0-255 bytes, 2 for 0-65535 bytes...
	FixedSize int32 // if some struct has fixed size no other header needed
	Callback  CB    // which function to run after data received
}

// Connection information both for client or server
type Connection struct {
	conn           *net.TCPConn         // real connection, set when its handshake starts
	listener       *net.TCPListener     // set by ListenOne, cleared by Close
	done           chan struct{}        // closed by Disconnect/Close: no more callbacks for conn
	connMutex      sync.Mutex           // guards conn, listener and done
	AESKey         string               // AES-128/192/256 key in hex for connection encryption
	wconn          *net.TCPConn         // where WriteMsg writes: conn after its handshake, nil before it and after a failed write
	wStream        cipher.Stream        // keystream of the outgoing direction, nil without encryption
	writeMutex     sync.Mutex           // guards wconn and wStream, serializes the frames
	cbMutex        sync.Mutex           // held while a callback runs
	Types          map[uint8]DataTypeCB // map of type->params&callback
	KeepAlive      time.Duration        // tcp keep-alive: idle time and probe interval; 0 = off
	ReadTimeout    time.Duration        // we need to receive new message at least once per this duration; 0 fails every read
	WriteTimeout   time.Duration        // maximal duration for every write operation; 0 fails every write
	ConnectTimeout time.Duration        // maximum duration of net.Dial; 0 = no timeout
	MaxSize        uint32               // maximum number of bytes for one record/message; 0 rejects any non-empty message (the C version treats 0 as "no limit")
	Ctx            context.Context      // context for some values like server id
}

// session is what the read loop needs from one established connection. A
// reconnect replaces the Connection fields, a running loop keeps its own.
type session struct {
	conn *net.TCPConn
	r    io.Reader     // conn, or the decrypting reader on top of it
	done chan struct{} // Connection.done of this connection
}

const (
	initialPacketSize = 1292 // we need to compare it manually to be sure that compiler doesn't change alignment!
)

type initialPacket struct {
	Header    [8]uint8 // sxchngXY where X.Y is version of protocol
	MaxSize   uint32
	SizeBytes [256]int8
	FixedSize [256]int32
}

func (c *Connection) readAll(conn *net.TCPConn, r io.Reader, buf []byte, size int) error {
	conn.SetReadDeadline(time.Now().Add(c.ReadTimeout))

	_, err := io.ReadFull(r, buf[:size])
	return err
}

// writeFrame encrypts frame in place when the connection has a cipher and
// sends it. It needs writeMutex to be held and wconn to be set.
func (c *Connection) writeFrame(frame []byte) error {
	if nil != c.wStream {
		c.wStream.XORKeyStream(frame, frame)
	} else if c.AESKey != "" {
		return errNotConnected // a key without a cipher: never send in the clear
	}

	c.wconn.SetWriteDeadline(time.Now().Add(c.WriteTimeout))
	_, err := c.wconn.Write(frame) // a short write comes with an error
	return err
}

// aesBlock returns the cipher for AESKey, nil when encryption is off
func (c *Connection) aesBlock() (cipher.Block, error) {
	if c.AESKey == "" {
		return nil, nil
	}
	aesKey, err := hex.DecodeString(c.AESKey)
	if err != nil {
		return nil, err
	}
	return aes.NewCipher(aesKey)
}

// encSetup exchanges the IVs. It returns the reader for incoming data and the
// keystream for outgoing data, which is nil without encryption.
func (c *Connection) encSetup(conn *net.TCPConn) (io.Reader, cipher.Stream, error) {
	block, err := c.aesBlock()
	if nil == block {
		return conn, nil, err
	}

	var iv [aes.BlockSize]byte
	if _, err := io.ReadFull(rand.Reader, iv[:]); nil != err {
		return nil, nil, err
	}

	conn.SetWriteDeadline(time.Now().Add(c.WriteTimeout))
	if _, err := conn.Write(iv[:]); nil != err {
		return nil, nil, err
	}

	var ivread [aes.BlockSize]byte
	if err := c.readAll(conn, conn, ivread[:], len(ivread)); nil != err {
		return nil, nil, err
	}

	return &cipher.StreamReader{S: cipher.NewCTR(block, ivread[:]), R: conn}, cipher.NewCTR(block, iv[:]), nil
}

// initConnection does the handshake; on an error it closes conn.
func (c *Connection) initConnection(conn *net.TCPConn) (s session, err error) {
	defer func() {
		if nil != err {
			conn.Close()
		}
	}()

	s = session{conn: conn, done: make(chan struct{})}

	// Until the handshake is done WriteMsg gets errNotConnected. The lock is
	// not held over the handshake: a silent peer must not block the writers.
	c.writeMutex.Lock()
	c.wconn, c.wStream = nil, nil
	c.writeMutex.Unlock()

	c.connMutex.Lock()
	c.conn, c.done = conn, s.done
	c.connMutex.Unlock()

	// Go turns keep-alive on by default, 0 is "off" as in C. Not
	// SetKeepAlivePeriod: since Go 1.23 it sets the idle time only.
	conn.SetKeepAliveConfig(net.KeepAliveConfig{Enable: c.KeepAlive != 0, Idle: c.KeepAlive, Interval: c.KeepAlive})

	// initial packets exchange
	initial := c.prepareInitialPacket()
	initialBuf := (*[initialPacketSize]byte)(unsafe.Pointer(&initial))[:]
	received := initialPacket{}
	receivedBuf := (*[initialPacketSize]byte)(unsafe.Pointer(&received))[:]

	// send out version
	conn.SetWriteDeadline(time.Now().Add(c.WriteTimeout))
	if _, err = conn.Write(initialBuf); nil != err {
		return s, err
	}

	err = c.readAll(conn, conn, receivedBuf, initialPacketSize)
	if nil != err {
		return s, err
	}

	if received.Header != initial.Header {
		return s, errors.New("HS error - different protocol version")
	}

	if received.MaxSize != initial.MaxSize {
		return s, errors.New("HS error - different message maximal size")
	}

	if (received.FixedSize != initial.FixedSize) || (received.SizeBytes != initial.SizeBytes) {
		return s, errors.New("HS error - different set of datatypes")
	}

	var wStream cipher.Stream
	s.r, wStream, err = c.encSetup(conn)
	if nil != err {
		return s, err
	}

	c.writeMutex.Lock()
	c.wconn, c.wStream = conn, wStream
	c.writeMutex.Unlock()
	return s, nil
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

	var (
		sbuf [5]byte
		t    [1]byte
		msg  = make([]byte, 0, c.MaxSize)
		crcI uint32
		crcB = (*[4]byte)(unsafe.Pointer(&crcI))
	)

	for {

		select {
		case <-s.done:
			return nil
		default:
		}

		err := c.readAll(s.conn, s.r, t[:], 1)
		if nil != err {
			return err
		}

		dt, ok := c.Types[t[0]]

		if !ok {
			return fmt.Errorf("structure type %d is not defined", t[0])
		}

		var size2read int
		if dt.SizeBytes > 0 && dt.SizeBytes <= 4 {
			err = c.readAll(s.conn, s.r, sbuf[0:dt.SizeBytes], int(dt.SizeBytes))
			if nil != err {
				return err
			}
		}

		switch dt.SizeBytes {
		case -1:
			size2read = int(dt.FixedSize)
		case 0:
			size2read = 0
		case 1:
			size2read = int(sbuf[0])
		case 2:
			size2read = int(sbuf[0]) | (int(sbuf[1]) << 8)
		case 3:
			size2read = int(sbuf[0]) | (int(sbuf[1]) << 8) | (int(sbuf[2]) << 16)
		case 4:
			size2read = int(sbuf[0]) | (int(sbuf[1]) << 8) | (int(sbuf[2]) << 16) | (int(sbuf[3]) << 24)
		default:
			return fmt.Errorf("structure type %d has invalid SizeBytes setting (%d)", t[0], dt.SizeBytes)
		}

		// negative: bad FixedSize, or a 4-byte size on a 32-bit platform
		if size2read < 0 || size2read > int(c.MaxSize) {
			return fmt.Errorf("size of message is greater than maximum (%d/%d)", size2read, c.MaxSize)
		}

		err = c.readAll(s.conn, s.r, msg, size2read)
		if nil != err {
			return err
		}

		if size2read != 0 {
			err = c.readAll(s.conn, s.r, (*crcB)[:], 4)
			if nil != err {
				return err
			}
			if crc32.Checksum(msg[0:size2read], crc32q) != crcI {
				return errors.New("crc failed on read")
			}
		}

		// we need to allow types that only can be send by one of the side, but also defined
		if nil != dt.Callback && !c.callback(s, dt.Callback, msg[0:size2read]) {
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

	var (
		crcI uint32
		crcB = (*[4]byte)(unsafe.Pointer(&crcI))
	)

	header := [6]byte{}
	header[0] = msgType

	size2write := 0
	headerSize := 1

	switch dt.SizeBytes {
	case -1:
		size2write = int(dt.FixedSize)
	case 0:
		size2write = 0
	case 1:
		if len(msg) > 255 {
			return fmt.Errorf("structure type %d has 1-byte size header but %d bytes given", msgType, len(msg))
		}
		header[1] = byte(len(msg))
		size2write = len(msg)
		headerSize = 2
	case 2:
		if len(msg) > 0xffff {
			return fmt.Errorf("structure type %d has 2-byte size header but %d bytes given", msgType, len(msg))
		}
		header[1] = byte(len(msg) & 0xff)
		header[2] = byte((len(msg) & 0xff00) >> 8)
		size2write = len(msg)
		headerSize = 3

	case 3:
		if len(msg) > 0xffffff {
			return fmt.Errorf("structure type %d has 3-byte size header but %d bytes given", msgType, len(msg))
		}
		header[1] = byte(len(msg) & 0xff)
		header[2] = byte((len(msg) & 0xff00) >> 8)
		header[3] = byte((len(msg) & 0xff0000) >> 16)
		size2write = len(msg)
		headerSize = 4

	case 4:
		header[1] = byte(len(msg) & 0xff)
		header[2] = byte((len(msg) & 0xff00) >> 8)
		header[3] = byte((len(msg) & 0xff0000) >> 16)
		header[4] = byte(len(msg) >> 24) // no 0xff000000 mask: it does not fit a 32-bit int
		size2write = len(msg)
		headerSize = 5

	default:
		return fmt.Errorf("structure type %d has invalid SizeBytes setting (%d)", msgType, dt.SizeBytes)
	}

	if size2write < 0 || size2write > len(msg) {
		return fmt.Errorf("unable to write %d bytes for structure %d, have only %d in buffer", size2write, msgType, len(msg))
	}

	// the peer has the same MaxSize and would drop the connection
	if size2write > int(c.MaxSize) {
		return fmt.Errorf("size of message is greater than maximum (%d/%d)", size2write, c.MaxSize)
	}

	// One buffer and one write per frame: three small writes cost three TCP
	// segments here (Go sets TCP_NODELAY).
	frame := make([]byte, 0, headerSize+size2write+4)
	frame = append(frame, header[:headerSize]...)
	frame = append(frame, msg[:size2write]...)
	if size2write > 0 {
		crcI = crc32.Checksum(msg[0:size2write], crc32q)
		frame = append(frame, (*crcB)[:]...)
	}

	c.writeMutex.Lock()
	defer c.writeMutex.Unlock()

	if nil == c.wconn {
		return errNotConnected
	}

	if err := c.writeFrame(frame); nil != err {
		// A part of the frame may be on the wire, and the keystream has moved
		// on: a frame written after it would be read as garbage.
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

func (c *Connection) prepareInitialPacket() initialPacket {
	result := initialPacket{MaxSize: c.MaxSize, Header: [8]byte{'s', 'x', 'c', 'h', 'n', 'g', verHI, verLO}}
	if c.AESKey != "" {
		result.Header[4] = 'E'
	}

	for i := 0; i < 256; i++ {
		result.FixedSize[i] = c.Types[uint8(i)].FixedSize
		result.SizeBytes[i] = c.Types[uint8(i)].SizeBytes
	}

	return result
}

func init() {
	if initialPacketSize != unsafe.Sizeof(initialPacket{}) {
		log.Fatalln("Golang uses different alignment within structure, please modify sxchnge code!")
	}
}
