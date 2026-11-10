package sxchange

import (
	"net"
	"time"
)

// ListenOne is one-connection server
func (c *Connection) ListenOne(address string, onConnect func(*Connection, <-chan interface{}), onError func(err error)) error {

	tcpaddr, err := net.ResolveTCPAddr("tcp", address)
	if nil != err {
		return err
	}

	listener, err := net.ListenTCP("tcp", tcpaddr)

	if nil != err {
		return err
	}

	go func() {
		for {
			if nil != c.conn {
				c.conn.Close()
			}

			conn, err := listener.AcceptTCP()
			if nil != err {
				onError(err)
				time.Sleep(100 * time.Millisecond) // don't spin on a persistent error (EMFILE)
				continue
			}

			err = c.initConnection(conn)
			if nil != err {
				onError(err)
				continue
			}

			c.CloseChan = make(chan interface{})
			threadCloseChan := make(chan interface{})

			go onConnect(c, threadCloseChan)

			err = c.run()
			if nil != err {
				onError(err)
			}
			close(threadCloseChan)
		}
	}()

	return nil
}
