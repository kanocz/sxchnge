package sxchange

import (
	"bytes"
	"errors"
	"fmt"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	tEmpty = iota // SizeBytes 0
	tOne          // SizeBytes 1
	tTwo          // SizeBytes 2
	tThree        // SizeBytes 3
	tFour         // SizeBytes 4
	tFixed        // SizeBytes -1
	tBad          // SizeBytes 5, invalid

	fixedSize = 16
	key128    = "00112233445566778899AABBCCDDEEFF"
	key192    = key128 + "0011223344556677"
	key256    = key128 + key128
)

type received struct {
	typ  uint8
	data []byte
}

// allTypes defines every valid kind of type; each callback reports to out.
func allTypes(out chan<- received) map[uint8]DataTypeCB {
	cb := func(typ uint8) CB {
		if out == nil {
			return nil
		}
		return func(msg []byte, _ *Connection) { out <- received{typ, append([]byte(nil), msg...)} }
	}
	return map[uint8]DataTypeCB{
		tEmpty: {SizeBytes: 0, Callback: cb(tEmpty)},
		tOne:   {SizeBytes: 1, Callback: cb(tOne)},
		tTwo:   {SizeBytes: 2, Callback: cb(tTwo)},
		tThree: {SizeBytes: 3, Callback: cb(tThree)},
		tFour:  {SizeBytes: 4, Callback: cb(tFour)},
		tFixed: {SizeBytes: -1, FixedSize: fixedSize, Callback: cb(tFixed)},
	}
}

func newConn(key string, types map[uint8]DataTypeCB) *Connection {
	return &Connection{
		MaxSize:        100000,
		ReadTimeout:    2 * time.Second,
		WriteTimeout:   2 * time.Second,
		ConnectTimeout: 2 * time.Second,
		AESKey:         key,
		Types:          types,
	}
}

// tcpPair returns two connected loopback sockets.
func tcpPair(t *testing.T) (*net.TCPConn, *net.TCPConn) {
	t.Helper()
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

type hsResult struct {
	s   session
	err error
}

// handshake runs initConnection of both sides over a fresh socket pair.
func handshake(t *testing.T, a, b *Connection) (hsResult, hsResult) {
	t.Helper()
	ca, cb := tcpPair(t)
	resA := make(chan hsResult, 1)
	go func() {
		s, err := a.initConnection(ca)
		resA <- hsResult{s, err}
	}()
	s, err := b.initConnection(cb)
	return <-resA, hsResult{s, err}
}

// connect is handshake that must succeed.
func connect(t *testing.T, a, b *Connection) (session, session) {
	t.Helper()
	ra, rb := handshake(t, a, b)
	if ra.err != nil || rb.err != nil {
		t.Fatalf("handshake: %v / %v", ra.err, rb.err)
	}
	return ra.s, rb.s
}

func wait(t *testing.T, ch <-chan received) received {
	t.Helper()
	select {
	case m := <-ch:
		return m
	case <-time.After(3 * time.Second):
		t.Fatal("message not received")
		return received{}
	}
}

func pattern(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i * 7)
	}
	return b
}

// freeAddr returns a loopback address that was free a moment ago.
func freeAddr(t *testing.T) string {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	return l.Addr().String()
}

func TestHandshakeMismatch(t *testing.T) {
	base := func() *Connection { return newConn(key128, allTypes(nil)) }
	cases := map[string]func(c *Connection){
		"SizeBytes":  func(c *Connection) { c.Types[tOne] = DataTypeCB{SizeBytes: 2} },
		"FixedSize":  func(c *Connection) { c.Types[tFixed] = DataTypeCB{SizeBytes: -1, FixedSize: fixedSize + 1} },
		"MaxSize":    func(c *Connection) { c.MaxSize++ },
		"extra type": func(c *Connection) { c.Types[200] = DataTypeCB{SizeBytes: 1} },
		"encryption": func(c *Connection) { c.AESKey = "" },
	}
	for name, change := range cases {
		t.Run(name, func(t *testing.T) {
			other := base()
			change(other)
			ra, rb := handshake(t, base(), other)
			if ra.err == nil || rb.err == nil {
				t.Fatalf("handshake accepted the difference: %v / %v", ra.err, rb.err)
			}
		})
	}
}

func TestHandshakeBadKey(t *testing.T) {
	for _, key := range []string{"zz", "0011", key128 + "00"} {
		ra, rb := handshake(t, newConn(key, allTypes(nil)), newConn(key, allTypes(nil)))
		if ra.err == nil || rb.err == nil {
			t.Fatalf("key %q accepted: %v / %v", key, ra.err, rb.err)
		}
	}
}

func TestRoundTrip(t *testing.T) {
	msgs := []received{
		{tEmpty, nil},
		{tOne, nil}, // a sized type with no data goes without crc
		{tOne, pattern(255)},
		{tTwo, pattern(0xffff)},
		{tThree, pattern(70000)},
		{tFour, pattern(70000)},
		{tFixed, pattern(fixedSize)},
	}
	for name, key := range map[string]string{"plain": "", "aes128": key128, "aes192": key192, "aes256": key256} {
		t.Run(name, func(t *testing.T) {
			got := make(chan received, len(msgs))
			a, b := newConn(key, allTypes(nil)), newConn(key, allTypes(got))
			_, sb := connect(t, a, b)
			go b.run(sb)

			for _, m := range msgs {
				if err := a.WriteMsg(m.typ, m.data); err != nil {
					t.Fatalf("type %d: %v", m.typ, err)
				}
			}
			for _, want := range msgs {
				m := wait(t, got)
				if m.typ != want.typ || !bytes.Equal(m.data, want.data) {
					t.Fatalf("got type %d with %d bytes, want type %d with %d bytes", m.typ, len(m.data), want.typ, len(want.data))
				}
			}
		})
	}
}

func TestWriteMsgErrors(t *testing.T) {
	types := allTypes(nil)
	types[tBad] = DataTypeCB{SizeBytes: 5}
	c := newConn("", types)

	cases := []struct {
		name string
		typ  uint8
		size int
		want string
	}{
		{"unknown type", 200, 0, "unknown msg type"},
		{"1-byte size", tOne, 256, "1-byte size header"},
		{"2-byte size", tTwo, 0x10000, "2-byte size header"},
		{"short fixed buffer", tFixed, fixedSize - 1, "have only"},
		{"over MaxSize", tThree, int(c.MaxSize) + 1, "greater than maximum"},
		{"invalid SizeBytes", tBad, 1, "invalid SizeBytes"},
	}
	for _, tc := range cases {
		err := c.WriteMsg(tc.typ, make([]byte, tc.size))
		if err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s: got %v, want an error with %q", tc.name, err, tc.want)
		}
	}

	if err := c.WriteMsg(tOne, []byte("hi")); !errors.Is(err, errNotConnected) {
		t.Errorf("write without a connection: %v", err)
	}
}

// Raw bytes from the peer that the read loop has to refuse.
func TestReadLoopErrors(t *testing.T) {
	cases := []struct {
		name string
		raw  []byte
		want string
	}{
		{"unknown type", []byte{200}, "not defined"},
		{"bad crc", []byte{tOne, 2, 'h', 'i', 0, 0, 0, 0}, "crc failed"},
		{"over MaxSize", []byte{tThree, 0xff, 0xff, 0xff}, "greater than maximum"},
		{"invalid SizeBytes", []byte{tBad}, "invalid SizeBytes"},
		{"truncated frame", []byte{tOne, 5, 'h'}, "EOF"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			mk := func() *Connection {
				types := allTypes(nil)
				types[tBad] = DataTypeCB{SizeBytes: 5}
				return newConn("", types)
			}
			a, b := mk(), mk()
			sa, sb := connect(t, a, b)
			if _, err := sa.conn.Write(tc.raw); err != nil {
				t.Fatal(err)
			}
			sa.conn.Close()

			err := b.run(sb)
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("got %v, want an error with %q", err, tc.want)
			}
		})
	}
}

func TestReadTimeout(t *testing.T) {
	a, b := newConn(key128, allTypes(nil)), newConn(key128, allTypes(nil))
	b.ReadTimeout = 100 * time.Millisecond
	_, sb := connect(t, a, b)

	err := b.run(sb)
	var ne net.Error
	if !errors.As(err, &ne) || !ne.Timeout() {
		t.Fatalf("got %v, want a timeout", err)
	}
}

func TestWrongKey(t *testing.T) {
	// only a sized type: a zero-size one would pass for 1 of 256 garbage bytes
	types := func() map[uint8]DataTypeCB { return map[uint8]DataTypeCB{tTwo: {SizeBytes: 2}} }
	a, b := newConn(key128, types()), newConn(strings.Replace(key128, "00", "01", 1), types())
	b.ReadTimeout = 300 * time.Millisecond
	_, sb := connect(t, a, b) // the handshake does not check the key

	if err := a.WriteMsg(tTwo, pattern(100)); err != nil {
		t.Fatal(err)
	}
	if err := b.run(sb); err == nil {
		t.Fatal("message encrypted with another key was accepted")
	}
}

func TestCallbackPanic(t *testing.T) {
	types := func(cb CB) map[uint8]DataTypeCB { return map[uint8]DataTypeCB{tEmpty: {Callback: cb}} }
	a, b := newConn("", types(nil)), newConn("", types(func([]byte, *Connection) { panic("boom") }))
	_, sb := connect(t, a, b)

	if err := a.WriteMsg(tEmpty, nil); err != nil {
		t.Fatal(err)
	}
	if err := b.run(sb); err == nil || !strings.Contains(err.Error(), "boom") {
		t.Fatalf("got %v, want the panic as an error", err)
	}
}

func TestDisconnectStopsReadLoop(t *testing.T) {
	a, b := newConn(key128, allTypes(nil)), newConn(key128, allTypes(nil))
	b.ReadTimeout = 30 * time.Second
	_, sb := connect(t, a, b)

	result := make(chan error, 1)
	go func() { result <- b.run(sb) }()
	time.Sleep(50 * time.Millisecond) // let the loop block in read
	b.Disconnect()
	b.Disconnect() // a second call is harmless

	select {
	case err := <-result:
		if err != nil {
			t.Fatalf("Disconnect reported as a failure: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("read loop still runs after Disconnect")
	}
	if err := b.WriteMsg(tEmpty, nil); err == nil {
		t.Fatal("write after Disconnect succeeded")
	}
}

// Frames of concurrent writers must not interleave. Run with -race.
func TestConcurrentWrites(t *testing.T) {
	const writers, perWriter = 8, 50
	got := make(chan received, writers*perWriter)
	a, b := newConn(key128, allTypes(nil)), newConn(key128, allTypes(got))
	_, sb := connect(t, a, b)
	go b.run(sb)

	var wg sync.WaitGroup
	for w := 0; w < writers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			for i := 0; i < perWriter; i++ {
				// every byte of a message is the writer id, the length varies
				if err := a.WriteMsg(tTwo, bytes.Repeat([]byte{byte(w)}, 1+w*100+i)); err != nil {
					t.Error(err)
					return
				}
			}
		}(w)
	}
	wg.Wait()

	for i := 0; i < writers*perWriter; i++ {
		m := wait(t, got)
		if len(m.data) == 0 || !bytes.Equal(m.data, bytes.Repeat(m.data[:1], len(m.data))) {
			t.Fatalf("message %d is mixed from several writers", i)
		}
	}
}

// Connect and ListenOne end to end: exchange, reconnect, Disconnect, Close.
func TestListenOneLifecycle(t *testing.T) {
	addr := freeAddr(t)
	srvGot, cliGot := make(chan received, 10), make(chan received, 10)
	closed := make(chan struct{}, 10)

	server := newConn(key256, allTypes(srvGot))
	err := server.ListenOne(addr, func(c *Connection, closeChan <-chan interface{}) {
		if err := c.WriteMsg(tOne, []byte("hello")); err != nil {
			t.Error(err)
		}
		<-closeChan
		closed <- struct{}{}
	}, func(error) {})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()

	if err := server.ListenOne(freeAddr(t), nil, nil); !errors.Is(err, errAlreadyListens) {
		t.Fatalf("second ListenOne: %v", err)
	}

	client := newConn(key256, allTypes(cliGot))
	defer client.Close()

	connectClient := func() {
		t.Helper()
		if err := client.Connect(addr); err != nil {
			t.Fatal(err)
		}
		if m := wait(t, cliGot); m.typ != tOne || string(m.data) != "hello" {
			t.Fatalf("client got type %d %q", m.typ, m.data)
		}
	}

	// one exchange in both directions; ends is how the connection gets closed
	exchange := func(ends func()) {
		t.Helper()
		connectClient()
		if client.Remote() == nil || client.Remote().String() != addr {
			t.Fatalf("client remote is %v, want %s", client.Remote(), addr)
		}
		if err := client.WriteMsg(tTwo, pattern(1000)); err != nil {
			t.Fatal(err)
		}
		if m := wait(t, srvGot); m.typ != tTwo || !bytes.Equal(m.data, pattern(1000)) {
			t.Fatalf("server got type %d with %d bytes", m.typ, len(m.data))
		}
		ends()
		select {
		case <-closed:
		case <-time.After(3 * time.Second):
			t.Fatal("server did not notice the end of the connection")
		}
	}

	exchange(func() { client.Close() })
	exchange(server.Disconnect) // the server drops the client and keeps listening
	exchange(connectClient)     // Connect on a connected client replaces the connection

	if err := server.Close(); err != nil {
		t.Fatal(err)
	}
	if err := client.Connect(addr); err == nil {
		t.Fatal("connected to a closed server")
	}
	l, err := net.Listen("tcp", addr)
	if err != nil {
		t.Fatalf("port is still taken after Close: %v", err)
	}
	l.Close()

	// the Connection can listen again after Close
	if err := server.ListenOne(addr, func(*Connection, <-chan interface{}) {}, func(error) {}); err != nil {
		t.Fatal(fmt.Errorf("ListenOne after Close: %w", err))
	}
}
