//go:build darwin || linux

package sxchange

import (
	"runtime"
	"syscall"
	"testing"
	"time"
)

// KeepAlive sets the idle time and the probe interval, as in the C version.
// SetKeepAlivePeriod sets the idle time only since Go 1.23.
func TestKeepAliveSetsIdleAndInterval(t *testing.T) {
	// TCP_KEEPALIVE and TCP_KEEPINTVL on macOS, TCP_KEEPIDLE and TCP_KEEPINTVL on Linux
	opts := map[string][2]int{"darwin": {0x10, 0x101}, "linux": {4, 5}}[runtime.GOOS]

	a, b := newConn("", allTypes(nil)), newConn("", allTypes(nil))
	a.KeepAlive = 40 * time.Second
	sa, _ := connect(t, a, b)
	rc, err := sa.conn.SyscallConn()
	if err != nil {
		t.Fatal(err)
	}
	for _, opt := range opts {
		var v int
		rc.Control(func(fd uintptr) { v, err = syscall.GetsockoptInt(int(fd), syscall.IPPROTO_TCP, opt) })
		if err != nil || v != 40 {
			t.Errorf("TCP option %#x is %d s (%v), want 40 s", opt, v, err)
		}
	}
}
