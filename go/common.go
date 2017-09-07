package sxchange

import (
	"fmt"
	"net"
	"sync"
	"time"
)

// CB type for callback function for each type of transfered data
type CB func([]byte, *Connection)

// DataTypeCB describes
type DataTypeCB struct {
	SizeBytes int // -1 for fixed size, 0 for no data (ping?), 1 for 0-255 bytes, 2 for 0-65535 bytes...
	FixedSize int // if some struct has fixed fize no other header needed
	Callback  CB  // which function to run after data received
}

// Connection informaion both for client or server
type Connection struct {
	conn         *net.TCPConn         // real connection
	writeMutex   sync.Mutex           // we need to lock on write operations
	sentMessages uint32               // atomic counter
	Types        map[uint8]DataTypeCB // map of type->params&callback
	KeepAlive    time.Duration        // tcp keep-alive
	ReadTimeout  time.Duration        // we need to receive new message at least once per this duration
	WriteTimeout time.Duration        // maximal duration for every write operation
	MaxSize      int                  // maximum number of bytes for one record/message
}

// internal run function
func (c *Connection) run() error {

	defer c.conn.Close()

	if c.KeepAlive != 0 {
		c.conn.SetKeepAlivePeriod(c.KeepAlive)
		c.conn.SetKeepAlive(true)
	}

	var (
		sbuf [4]byte
		t    [1]byte
		msg  = make([]byte, 0, c.MaxSize)
	)

	for {
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
			c.conn.SetReadDeadline(time.Now().Add(c.ReadTimeout))
			i, err = c.conn.Read(sbuf[0:dt.SizeBytes])
			if nil != err {
				return err
			}
		}

		switch dt.SizeBytes {
		case -1:
			size2read = dt.FixedSize
		case 0:
			size2read = 0
			continue
		case 1:
			size2read = int(sbuf[0])
		case 2:
			size2read = int(sbuf[0]) | (int(sbuf[1]) << 8)
		case 3:
			size2read = int(sbuf[0]) | (int(sbuf[1]) << 8) | (int(sbuf[2]) << 16)
		default:
			return fmt.Errorf("Structure type %d has invalid SizeBytes setting (%d)", t[0], dt.SizeBytes)
		}

		for i := 0; i < size2read; i++ {
			c.conn.SetReadDeadline(time.Now().Add(c.ReadTimeout))
			j, err := c.conn.Read(msg[i:size2read])
			i += j
			if nil != err {
				return err
			}
		}

		dt.Callback(msg[0:size2read], c)
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
		size2write = dt.FixedSize
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
		header[2] = byte((len(msg) & 0xff0000) >> 16)
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

	for i := 0; i < headerSize; i++ {
		c.conn.SetWriteDeadline(time.Now().Add(c.WriteTimeout))
		j, err := c.conn.Write(header[i:headerSize])
		if nil != err {
			return err
		}
		i += j
	}

	for i := 0; i < size2write; i++ {
		c.conn.SetWriteDeadline(time.Now().Add(c.WriteTimeout))
		j, err := c.conn.Write(msg[i:size2write])
		if nil != err {
			return err
		}
		i += j
	}

	return nil
}
