package sxchange

import (
	"net"
)

// Connect to remote server
func (c *Connection) Connect(address string) error {

	var err error

	tcpaddr, err := net.ResolveTCPAddr("tcp", address)
	if nil != err {
		return err
	}

	c.conn, err = net.DialTCP("tcp", nil, tcpaddr)
	if nil != err {
		return err
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
