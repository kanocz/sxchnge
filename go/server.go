package sxchange

import (
	"errors"
	"net"
	"time"
)

// ListenOne is one-connection server. Close stops it.
// Both callbacks are required.
func (c *Connection) ListenOne(address string, onConnect func(*Connection, <-chan interface{}), onError func(err error)) error {

	if nil == onConnect || nil == onError {
		return errNilCallback // the accept goroutine calls both
	}

	if _, err := c.psk(); nil != err {
		return err // an invalid AESKey, not an error on every connection
	}

	tcpaddr, err := net.ResolveTCPAddr("tcp", address)
	if nil != err {
		return err
	}

	listener, err := net.ListenTCP("tcp", tcpaddr)

	if nil != err {
		return err
	}

	c.connMutex.Lock()
	if nil != c.listener {
		c.connMutex.Unlock()
		listener.Close()
		return errAlreadyListens
	}
	c.listener = listener
	c.connMutex.Unlock()

	go func() {
		for {
			conn, err := listener.AcceptTCP()
			if nil != err {
				if errors.Is(err, net.ErrClosed) {
					return // Close was called
				}
				onError(err)
				time.Sleep(100 * time.Millisecond) // don't spin on a persistent error (EMFILE)
				continue
			}

			s, err := c.initConnection(conn, true)

			// Close that ran before initConnection registered conn could not close it
			c.connMutex.Lock()
			stopped := c.listener != listener
			c.connMutex.Unlock()
			if stopped {
				conn.Close()
				return
			}

			if nil != err {
				onError(err)
				continue
			}

			threadCloseChan := make(chan interface{})

			go onConnect(c, threadCloseChan)

			err = c.run(s)
			if nil != err {
				onError(err)
			}
			close(threadCloseChan)
		}
	}()

	return nil
}
