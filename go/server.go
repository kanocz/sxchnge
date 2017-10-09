package sxchange

import (
	"net"
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

			c.conn, err = listener.AcceptTCP()
			if nil != err {
				onError(err)
				continue
			}

			err = c.initConnection()
			if nil != err {
				onError(err)
				continue
			}

			closeChan := make(chan interface{})
			go onConnect(c, closeChan)

			err = c.run()
			if nil != err {
				onError(err)
			}
			close(closeChan)
		}
	}()

	return nil
}
