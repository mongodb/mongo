package util

import (
	"net"
	"time"
)

// EnableTCPKeepAlive enables TCP keepalive on the underlying TCP connection.
func EnableTCPKeepAlive(conn net.Conn, keepAlivePeriod time.Duration) error {
	if keepAlivePeriod == 0 {
		return nil
	}
	if tcpconn, ok := conn.(*net.TCPConn); ok {
		err := tcpconn.SetKeepAlive(true)
		if err != nil {
			return err
		}
		err = tcpconn.SetKeepAlivePeriod(keepAlivePeriod)
		if err != nil {
			return err
		}
	}
	return nil
}
