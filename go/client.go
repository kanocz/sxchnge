package sxchange

import (
	"errors"
	"net"
)

var (
	errNonTCP = errors.New("non tcp connection returned")
)

// Connect to remote server. A previous connection of this Connection is closed.
func (c *Connection) Connect(address string) error {

	if _, err := c.aesBlock(); nil != err {
		return err // an invalid AESKey, before any network activity
	}

	c.Disconnect()

	conn, err := net.DialTimeout("tcp", address, c.ConnectTimeout)
	if nil != err {
		return err
	}
	tcpConn, ok := conn.(*net.TCPConn)
	if !ok {
		conn.Close()
		return errNonTCP
	}

	s, err := c.initConnection(tcpConn)
	if nil != err {
		return err
	}

	go c.run(s)

	return nil
}
