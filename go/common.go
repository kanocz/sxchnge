package sxchange

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"sync"
	"time"
	"unsafe"
)

const (
	verHI = 1
	verLO = 0
)

// CB type for callback function for each type of transfered data
type CB func([]byte, *Connection)

// DataTypeCB describes
type DataTypeCB struct {
	SizeBytes int8  // -1 for fixed size, 0 for no data (ping?), 1 for 0-255 bytes, 2 for 0-65535 bytes...
	FixedSize int32 // if some struct has fixed fize no other header needed
	Callback  CB    // which function to run after data received
}

// Connection informaion both for client or server
type Connection struct {
	conn         *net.TCPConn         // real connection
	writeMutex   sync.Mutex           // we need to lock on write operations
	Types        map[uint8]DataTypeCB // map of type->params&callback
	KeepAlive    time.Duration        // tcp keep-alive
	ReadTimeout  time.Duration        // we need to receive new message at least once per this duration
	WriteTimeout time.Duration        // maximal duration for every write operation
	MaxSize      uint32               // maximum number of bytes for one record/message
	Ctx          context.Context      // context for cancel and also for other values
}

const (
	initialPacketSize = 1292 // we need to compare it manualy to be sure that compiler doesn't change aligment!
)

type initialPacket struct {
	Header    [8]uint8 // sxchngXY where X.Y is version of protocol
	MaxSize   uint32
	SizeBytes [256]int8
	FixedSize [256]int32
}

func readAll(conn *net.TCPConn, buf []byte, size int, timeout time.Duration) error {

	totalDeadline := time.Now().Add(timeout)
	conn.SetReadDeadline(totalDeadline)
	i, err := io.ReadFull(conn, buf[:size])
	if nil != err {
		return err
	}
	if i != size {
		return errors.New("Partial read")
	}

	return nil
}

func writeAll(conn *net.TCPConn, buf []byte, size int, timeout time.Duration) error {
	totalDeadline := time.Now().Add(timeout)
	conn.SetWriteDeadline(totalDeadline)
	i, err := conn.Write(buf[:size])

	if nil != err {
		return err
	}
	if i != size {
		return errors.New("Partial write")
	}

	return nil
}

func (c *Connection) initConnection() error {
	if c.KeepAlive != 0 {
		c.conn.SetKeepAlivePeriod(c.KeepAlive)
		c.conn.SetKeepAlive(true)
	}

	// initial packets exchange
	initial := c.prepareInitialPacket()
	initialBuf := (*[initialPacketSize]byte)(unsafe.Pointer(&initial))[:]
	received := initialPacket{}
	receivedBuf := (*[initialPacketSize]byte)(unsafe.Pointer(&received))[:]

	// send out version
	err := writeAll(c.conn, initialBuf, initialPacketSize, c.WriteTimeout)
	if nil != err {
		return err
	}

	err = readAll(c.conn, receivedBuf, initialPacketSize, c.ReadTimeout)
	if nil != err {
		return err
	}

	if received.Header != initial.Header {
		return errors.New("HS error - different protocol version")
	}

	if received.MaxSize != initial.MaxSize {
		return errors.New("HS error - different message maximal size")
	}

	if (received.FixedSize != initial.FixedSize) || (received.MaxSize != received.MaxSize) {
		return errors.New("HS error - different set of datatypes")
	}

	return nil
}

// internal run function
func (c *Connection) run() error {

	defer c.conn.Close()
	if nil == c.Ctx {
		c.Ctx = context.Background()
	}

	var (
		sbuf [4]byte
		t    [1]byte
		msg  = make([]byte, 0, c.MaxSize)
	)

	for {

		select {
		case <-c.Ctx.Done():
			return nil
		default:
		}

		c.conn.SetReadDeadline(time.Now().Add(c.ReadTimeout))
		i, err := c.conn.Read(t[:])
		if nil != err {
			return err
		}
		if 0 == i { // TODO: check if this is an error?..
			continue
		}

		dt, ok := c.Types[t[0]]

		if !ok {
			return fmt.Errorf("Structure type %d is not defined", t[0])
		}

		var size2read int
		if dt.SizeBytes > 0 {
			err = readAll(c.conn, sbuf[0:dt.SizeBytes], int(dt.SizeBytes), c.ReadTimeout)
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
		default:
			return fmt.Errorf("Structure type %d has invalid SizeBytes setting (%d)", t[0], dt.SizeBytes)
		}

		err = readAll(c.conn, msg, size2read, c.ReadTimeout)
		if nil != err {
			return err
		}

		// one more check on done channel - we don't want to call callback after it has been closed
		select {
		case <-c.Ctx.Done():
			return nil
		default:
		}

		// we need to allow types that only can be send by one of the side, but also defined
		if nil != dt.Callback {
			dt.Callback(msg[0:size2read], c)
		}
	}
}

// WriteMsg creates header and then writes msg buffer via TCP connection
func (c *Connection) WriteMsg(msgType uint8, msg []byte) error {

	dt, ok := c.Types[msgType]
	if !ok {
		return fmt.Errorf("Unknown msg type %d", msgType)
	}

	header := [5]byte{}
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
			return fmt.Errorf("Structure type %d has 1-byte size header but %d bytes givven", msgType, len(msg))
		}
		header[1] = byte(len(msg))
		size2write = len(msg)
		headerSize = 2
	case 2:
		if len(msg) > 255*255 {
			return fmt.Errorf("Structure type %d has 2-byte size header but %d bytes givven", msgType, len(msg))
		}
		header[1] = byte(len(msg) & 0xff)
		header[2] = byte((len(msg) & 0xff00) >> 8)
		size2write = len(msg)
		headerSize = 3

	case 3:
		if len(msg) > 255*255*255 {
			return fmt.Errorf("Structure type %d has 2-byte size header but %d bytes givven", msgType, len(msg))
		}
		header[1] = byte(len(msg) & 0xff)
		header[2] = byte((len(msg) & 0xff00) >> 8)
		header[3] = byte((len(msg) & 0xff0000) >> 16)
		size2write = len(msg)
		headerSize = 4

	default:
		return fmt.Errorf("Structure type %d has invalid SizeBytes setting (%d)", msgType, dt.SizeBytes)
	}

	if size2write > len(msg) {
		return fmt.Errorf("Unable to write %d bytes for structure %d, have only %d in buffer", msgType, size2write, len(msg))
	}

	c.writeMutex.Lock()
	defer c.writeMutex.Unlock()

	err := writeAll(c.conn, header[:], headerSize, c.WriteTimeout)
	if nil != err {
		return err
	}

	err = writeAll(c.conn, msg, size2write, c.WriteTimeout)
	if nil != err {
		return err
	}

	return nil
}

// Remote address retrive
func (c *Connection) Remote() net.Addr {
	if nil == c.conn {
		return nil
	}

	return c.conn.RemoteAddr()
}

func (c *Connection) prepareInitialPacket() initialPacket {
	result := initialPacket{MaxSize: c.MaxSize, Header: [8]byte{'s', 'x', 'c', 'h', 'n', 'g', verHI, verLO}}

	for i := 0; i < 256; i++ {
		result.FixedSize[i] = c.Types[uint8(i)].FixedSize
		result.SizeBytes[i] = c.Types[uint8(i)].SizeBytes
	}

	return result
}

func init() {
	if initialPacketSize != unsafe.Sizeof(initialPacket{}) {
		log.Fatalln("Golang uses different aligment withing structure, please modify sxchnge code!")
	}
}
