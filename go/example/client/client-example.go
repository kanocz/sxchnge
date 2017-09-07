package main

import (
	"encoding/json"
	"log"
	"math"
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

func main() {
	connection := sxchange.Connection{
		KeepAlive:    time.Second * 10,
		MaxSize:      1024, // for this example even 256 will be enough
		ReadTimeout:  time.Second * 3,
		WriteTimeout: time.Second * 3,
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
			typeString: sxchange.DataTypeCB{ // send-only type without callback
				SizeBytes: 1, // up to 255 bytes string length
			},
			typeSomeData: sxchange.DataTypeCB{ // send-only type without callback
				SizeBytes: -1, // using fixed size
				FixedSize: int(unsafe.Sizeof(someData{})),
			},
			typeSomeDataJSON: sxchange.DataTypeCB{ // send-only type without callback
				SizeBytes: 2, // up to 65535 bytes string length
			},
		},
	}

	err := connection.Connect("127.0.0.1:3456")
	if nil != err {
		log.Fatalln("Error connecting to server:", err)
	}

	// send ping
	log.Println("Sending ping")
	err = connection.WriteMsg(typePing, []byte{})
	if nil != err {
		log.Println(err)
	}

	// some string
	log.Println("Sending string")
	err = connection.WriteMsg(typeString, []byte("Hello, world!"))
	if nil != err {
		log.Println(err)
	}

	// and someData
	log.Println("Sending raw struct")
	buf := make([]byte, unsafe.Sizeof(someData{}))
	data := (*someData)(unsafe.Pointer(&buf[0]))
	data.ID = 123
	data.Value = math.Pi
	err = connection.WriteMsg(typeSomeData, buf)
	if nil != err {
		log.Println(err)
	}

	// and someData json-encoded
	log.Println("Sending json struct")
	jdata := someData{ID: 345, Value: math.E}
	j, err := json.Marshal(jdata)
	if nil != err {
		log.Fatal("Error json encoding:", err)
	}
	err = connection.WriteMsg(typeSomeDataJSON, j)
	if nil != err {
		log.Println(err)
	}

	log.Println("All done, waiting for Ctrl-C")

	// and wait for Ctrl-C
	exitChan := make(chan os.Signal, 1)
	signal.Notify(exitChan, syscall.SIGTERM)
	signal.Notify(exitChan, syscall.SIGINT)
	<-exitChan

}
