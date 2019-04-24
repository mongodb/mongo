// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

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
