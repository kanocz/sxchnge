package sxchange

import (
	"errors"
	"net"
)

var (
	errNonTCP = errors.New("non tcp connection returned")
)

// Connect to remote server
func (c *Connection) Connect(address string) error {

	var (
		err error
		ok  bool
	)

	conn, err := net.DialTimeout("tcp", address, c.ConnectTimeout)
	if nil != err {
		return err
	}
	c.conn, ok = conn.(*net.TCPConn)
	if !ok {
		c.conn = nil
		return errNonTCP
	}

	err = c.initConnection()
	if nil != err {
		c.conn.Close()
		return err
	}

	c.CloseChan = make(chan interface{})

	go c.run()

	return nil
}
