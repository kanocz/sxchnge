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

	conn, err := net.DialTimeout("tcp", address, c.ConnectTimeout)
	if nil != err {
		return err
	}
	tcpConn, ok := conn.(*net.TCPConn)
	if !ok {
		conn.Close()
		return errNonTCP
	}

	err = c.initConnection(tcpConn)
	if nil != err {
		tcpConn.Close()
		return err
	}

	c.CloseChan = make(chan interface{})

	go c.run()

	return nil
}
