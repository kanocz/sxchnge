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
	"sync"
	"time"
	"unsafe"
)

const (
	verHI = 1
	verLO = 1
)

var (
	crc32q          = crc32.MakeTable(crc32.Koopman)
	errPartialWrite = errors.New("partial write")
	errPartialRead  = errors.New("partial read")
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
	conn           *net.TCPConn         // real connection
	AESKey         string               // AES-128/192/256 key in hex for connecton encryption
	eWriter        *cipher.StreamWriter // just StreamWriter object to simplify encryption
	eReader        *cipher.StreamReader // the same for reader
	writeMutex     sync.Mutex           // we need to lock on write operations
	Types          map[uint8]DataTypeCB // map of type->params&callback
	KeepAlive      time.Duration        // tcp keep-alive
	ReadTimeout    time.Duration        // we need to receive new message at least once per this duration
	WriteTimeout   time.Duration        // maximal duration for every write operation
	ConnectTimeout time.Duration        // maximum duration of net.Dial
	MaxSize        uint32               // maximum number of bytes for one record/message
	Ctx            context.Context      // context for some values like server id
	CloseChan      chan interface{}     // channel for closing incoming messages circle
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

func (c *Connection) readAll(buf []byte, size int, ne bool) error {

	totalDeadline := time.Now().Add(c.ReadTimeout)
	c.conn.SetReadDeadline(totalDeadline)

	var (
		i   int
		err error
	)

	if ne || nil == c.eReader {
		i, err = io.ReadFull(c.conn, buf[:size])
	} else {
		i, err = io.ReadFull(c.eReader, buf[:size])
	}

	if nil != err {
		return err
	}
	if i != size {
		return errPartialRead
	}

	return nil
}

func (c *Connection) writeAll(buf []byte, size int, ne bool) error {
	totalDeadline := time.Now().Add(c.WriteTimeout)
	c.conn.SetWriteDeadline(totalDeadline)

	var (
		i   int
		err error
	)

	if ne || nil == c.eWriter {
		i, err = c.conn.Write(buf[:size])
	} else {
		i, err = c.eWriter.Write(buf[:size])
	}

	if nil != err {
		return err
	}
	if i != size {
		return errPartialWrite
	}

	return nil
}

func (c *Connection) encSetup() error {
	if c.AESKey != "" {
		aesKey, err := hex.DecodeString(c.AESKey)
		if err != nil {
			return err
		}

		block, err := aes.NewCipher(aesKey)
		if err != nil {
			return err
		}

		var iv [aes.BlockSize]byte
		if _, err := io.ReadFull(rand.Reader, iv[:]); nil != err {
			return err
		}

		c.conn.SetWriteDeadline(time.Now().Add(c.WriteTimeout))
		i, err := c.conn.Write(iv[:])
		if nil != err {
			return err
		}
		if i != len(iv) {
			return errPartialWrite
		}

		c.eWriter = &cipher.StreamWriter{S: cipher.NewCTR(block, iv[:]), W: c.conn}

		c.conn.SetReadDeadline(time.Now().Add(c.ReadTimeout))

		var ivread [aes.BlockSize]byte

		i, err = io.ReadFull(c.conn, ivread[:])
		if nil != err {
			return err
		}
		if i != len(ivread) {
			return errPartialRead
		}

		c.eReader = &cipher.StreamReader{S: cipher.NewCTR(block, ivread[:]), R: c.conn}
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
	err := c.writeAll(initialBuf, initialPacketSize, true)
	if nil != err {
		return err
	}

	err = c.readAll(receivedBuf, initialPacketSize, true)
	if nil != err {
		return err
	}

	if received.Header != initial.Header {
		return errors.New("HS error - different protocol version")
	}

	if received.MaxSize != initial.MaxSize {
		return errors.New("HS error - different message maximal size")
	}

	if (received.FixedSize != initial.FixedSize) || (received.MaxSize != initial.MaxSize) {
		return errors.New("HS error - different set of datatypes")
	}

	err = c.encSetup()
	if nil != err {
		return err
	}

	return nil
}

// internal run function
func (c *Connection) run() error {

	defer c.conn.Close()

	defer func() {
		if r := recover(); r != nil {
			fmt.Println("Recovered in sxconnection.run", r)
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
		case <-c.CloseChan:
			return nil
		default:
		}

		err := c.readAll(t[:], 1, false)
		if nil != err {
			return err
		}

		dt, ok := c.Types[t[0]]

		if !ok {
			return fmt.Errorf("structure type %d is not defined", t[0])
		}

		var size2read int
		if dt.SizeBytes > 0 {
			err = c.readAll(sbuf[0:dt.SizeBytes], int(dt.SizeBytes), false)
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

		if size2read > int(c.MaxSize) {
			return fmt.Errorf("size of message is greater than maximum (%d/%d)", size2read, c.MaxSize)
		}

		err = c.readAll(msg, size2read, false)
		if nil != err {
			return err
		}

		// one more check on done channel - we don't want to call callback after it has been closed
		select {
		case <-c.CloseChan:
			return nil
		default:
		}

		if size2read != 0 {
			err = c.readAll((*crcB)[:], 4, false)
			if nil != err {
				return err
			}
			if crc32.Checksum(msg[0:size2read], crc32q) != crcI {
				return errors.New("crc failed on read")
			}
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
			return fmt.Errorf("structure type %d has 1-byte size header but %d bytes givven", msgType, len(msg))
		}
		header[1] = byte(len(msg))
		size2write = len(msg)
		headerSize = 2
	case 2:
		if len(msg) > 255*255 {
			return fmt.Errorf("structure type %d has 2-byte size header but %d bytes givven", msgType, len(msg))
		}
		header[1] = byte(len(msg) & 0xff)
		header[2] = byte((len(msg) & 0xff00) >> 8)
		size2write = len(msg)
		headerSize = 3

	case 3:
		if len(msg) > 255*255*255 {
			return fmt.Errorf("structure type %d has 3-byte size header but %d bytes givven", msgType, len(msg))
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
		header[4] = byte((len(msg) & 0xff000000) >> 24)
		size2write = len(msg)
		headerSize = 5

	default:
		return fmt.Errorf("structure type %d has invalid SizeBytes setting (%d)", msgType, dt.SizeBytes)
	}

	if size2write > len(msg) {
		return fmt.Errorf("unable to write %d bytes for structure %d, have only %d in buffer", msgType, size2write, len(msg))
	}

	c.writeMutex.Lock()
	defer c.writeMutex.Unlock()

	err := c.writeAll(header[:], headerSize, false)
	if nil != err {
		return err
	}

	err = c.writeAll(msg, size2write, false)
	if nil != err {
		return err
	}

	if size2write > 0 {
		crcI = crc32.Checksum(msg[0:size2write], crc32q)
		err = c.writeAll((*crcB)[:], 4, false)
		if nil != err {
			return err
		}
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
		log.Fatalln("Golang uses different aligment withing structure, please modify sxchnge code!")
	}
}
