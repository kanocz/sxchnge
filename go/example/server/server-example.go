package main

import (
	"encoding/json"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"
	"unsafe"

	sxchange "github.com/kanocz/sxchnge/go"
)

const (
	typePing = iota
	typePong
	typeString
	typeSomeData
	typeSomeDataJSON
)

type someData struct {
	// we use only scalar data to be able transfer it as-is
	ID    uint64
	Value float64
}

func pingHandler(_ []byte, c *sxchange.Connection) {
	log.Println("Ping received, sending pong")
	go c.WriteMsg(1, []byte{})
}

func pongHandler(_ []byte, _ *sxchange.Connection) {
	log.Println("Pong received")
}

func stringHandler(str []byte, c *sxchange.Connection) {
	log.Println("String message received:", string(str))
}

func someDataHandler(value []byte, c *sxchange.Connection) {
	// we don't need to check size -> we used FixedSize param
	// just unsafe conversion :D
	data := (*someData)(unsafe.Pointer(&value[0]))
	log.Printf("Received somedata: %+v", data)
}

func someDataJSONHandler(value []byte, c *sxchange.Connection) {
	var data someData
	err := json.Unmarshal(value, &data)
	if nil != err {
		log.Println("Unable to unmarshal json:", err)
		log.Println("Received data:", string(value))
		return
	}

	log.Printf("Received json-encoded somedata: %+v, raw: %s\n", data, string(value))
}

func main() {
	connection := sxchange.Connection{
		KeepAlive:    time.Second * 10,
		MaxSize:      1024, // for this example even 256 will be enough
		ReadTimeout:  time.Second * 3,
		WriteTimeout: time.Second * 3,
		AESKey:       "00112233445566778899AABBCCDDEEFF",
		Types: map[uint8]sxchange.DataTypeCB{
			typePing: sxchange.DataTypeCB{
				Callback:  pingHandler,
				FixedSize: 0, // no data for ping-pong
				SizeBytes: 0,
			},
			typePong: sxchange.DataTypeCB{
				Callback:  pongHandler,
				FixedSize: 0, // no data for ping-pong
				SizeBytes: 0,
			},
			typeString: sxchange.DataTypeCB{
				Callback:  stringHandler,
				SizeBytes: 1, // up to 255 bytes string length
			},
			typeSomeData: sxchange.DataTypeCB{
				Callback:  someDataHandler,
				SizeBytes: -1, // using fixed size
				FixedSize: int32(unsafe.Sizeof(someData{})),
			},
			typeSomeDataJSON: sxchange.DataTypeCB{
				Callback:  someDataJSONHandler,
				SizeBytes: 2, // up to 65535 bytes string length
			},
		},
	}

	err := connection.ListenOne(":3456", func(c *sxchange.Connection, closeChan <-chan interface{}) {
		// send ping on connect
		log.Println("New connection from", c.Remote(), ", sending ping!")
		err := connection.WriteMsg(typePing, []byte{})
		if nil != err {
			log.Println("Error sending ping:", err)
		}

		<-closeChan
		log.Println("Connection closed")
	}, func(err error) {
		log.Println("SXError:", err)
	})
	if nil != err {
		log.Fatalln("Error listening:", err)
	}

	// and wait for Ctrl-C
	exitChan := make(chan os.Signal, 1)
	signal.Notify(exitChan, syscall.SIGTERM)
	signal.Notify(exitChan, syscall.SIGINT)
	<-exitChan

}
