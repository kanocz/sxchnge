package sxchange

import (
	"log"
	"net"
	"time"
)

// ListenOne is one-connection server
func (c *Connection) ListenOne(address string, onConnect func(*Connection, <-chan interface{})) error {

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
			c.conn, err = listener.AcceptTCP()
			if nil != err {
				log.Println("Failed accepting connection:", err)
				time.Sleep(time.Second)
				continue
			}

			err = c.initConnection()
			if nil != err {
				log.Println(err)
				c.conn.Close()
				continue
			}

			closeChan := make(chan interface{})
			go onConnect(c, closeChan)

			c.run()
			close(closeChan)
		}
	}()

	return nil
}
