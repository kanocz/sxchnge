package sxchange

import (
	"bytes"
	"net"
	"testing"
	"time"
)

const testType = 7

// tcpPair returns two connected loopback sockets.
func tcpPair(t *testing.T) (*net.TCPConn, *net.TCPConn) {
	l, err := net.ListenTCP("tcp", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a, err := net.DialTCP("tcp", nil, l.Addr().(*net.TCPAddr))
	if err != nil {
		t.Fatal(err)
	}
	b, err := l.AcceptTCP()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { a.Close(); b.Close() })
	return a, b
}

func testConn(sizeBytes int8, cb CB) *Connection {
	return &Connection{
		MaxSize:      0xffff,
		ReadTimeout:  2 * time.Second,
		WriteTimeout: 2 * time.Second,
		AESKey:       "00112233445566778899AABBCCDDEEFF",
		Types:        map[uint8]DataTypeCB{testType: {SizeBytes: sizeBytes, Callback: cb}},
		CloseChan:    make(chan interface{}),
	}
}

func handshake(t *testing.T, a, b *Connection) (error, error) {
	ca, cb := tcpPair(t)
	errA := make(chan error, 1)
	go func() { errA <- a.initConnection(ca) }()
	errB := b.initConnection(cb)
	return <-errA, errB
}

func TestHandshakeRejectsSizeBytesMismatch(t *testing.T) {
	errA, errB := handshake(t, testConn(1, nil), testConn(2, nil))
	if errA == nil || errB == nil {
		t.Fatalf("handshake accepted different SizeBytes: %v / %v", errA, errB)
	}
}

func TestRoundTripMaxTwoByteSize(t *testing.T) {
	got := make(chan []byte, 1)
	a := testConn(2, nil)
	b := testConn(2, func(msg []byte, _ *Connection) { got <- append([]byte(nil), msg...) })
	if errA, errB := handshake(t, a, b); errA != nil || errB != nil {
		t.Fatalf("handshake: %v / %v", errA, errB)
	}
	go b.run()

	msg := bytes.Repeat([]byte{0xa5}, 0xffff)
	if err := a.WriteMsg(testType, msg); err != nil {
		t.Fatal(err)
	}
	select {
	case m := <-got:
		if !bytes.Equal(m, msg) {
			t.Fatalf("received %d bytes, differs from the %d sent", len(m), len(msg))
		}
	case <-time.After(3 * time.Second):
		t.Fatal("message not received")
	}
}
