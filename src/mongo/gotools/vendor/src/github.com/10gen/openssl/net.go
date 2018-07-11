// Copyright (C) 2017. See AUTHORS.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package openssl

import (
	"errors"
	"net"
)

type listener struct {
	net.Listener
	ctx *Ctx
}

func (l *listener) Accept() (c net.Conn, err error) {
	c, err = l.Listener.Accept()
	if err != nil {
		return nil, err
	}
	ssl_c, err := Server(c, l.ctx)
	if err != nil {
		c.Close()
		return nil, err
	}
	return ssl_c, nil
}

// NewListener wraps an existing net.Listener such that all accepted
// connections are wrapped as OpenSSL server connections using the provided
// context ctx.
func NewListener(inner net.Listener, ctx *Ctx) net.Listener {
	return &listener{
		Listener: inner,
		ctx:      ctx}
}

// Listen is a wrapper around net.Listen that wraps incoming connections with
// an OpenSSL server connection using the provided context ctx.
func Listen(network, laddr string, ctx *Ctx) (net.Listener, error) {
	if ctx == nil {
		return nil, errors.New("no ssl context provided")
	}
	l, err := net.Listen(network, laddr)
	if err != nil {
		return nil, err
	}
	return NewListener(l, ctx), nil
}

type DialFlags int

const (
	InsecureSkipHostVerification DialFlags = 1 << iota
	DisableSNI
)

// Dial will connect to network/address and then wrap the corresponding
// underlying connection with an OpenSSL client connection using context ctx.
// If flags includes InsecureSkipHostVerification, the server certificate's
// hostname will not be checked to match the hostname in addr. Otherwise, flags
// should be 0.
//
// Dial probably won't work for you unless you set a verify location or add
// some certs to the certificate store of the client context you're using.
// This library is not nice enough to use the system certificate store by
// default for you yet.
func Dial(network, addr string, ctx *Ctx, flags DialFlags) (*Conn, error) {
	return DialSession(network, addr, ctx, flags, nil)
}

// DialWithDialer will connect to network/address using the provided dialer and
// then wrap the corresponding underlying connection with an OpenSSL client
// connection using context ctx. If flags includes InsecureSkipHostVerification,
// the server certificate's hostname will not be checked to match the hostname
// in addr. Otherwise, flags should be 0.
//
// Dial probably won't work for you unless you set a verify location or add
// some certs to the certificate store of the client context you're using.
// This library is not nice enough to use the system certificate store by
// default for you yet.
func DialWithDialer(dialer *net.Dialer, network, addr string, ctx *Ctx, flags DialFlags) (*Conn, error) {
	return dialSessionWithDialer(
		dialer,
		network,
		addr,
		ctx,
		flags,
		nil,
	)
}

// DialSession will connect to network/address and then wrap the corresponding
// underlying connection with an OpenSSL client connection using context ctx.
// If flags includes InsecureSkipHostVerification, the server certificate's
// hostname will not be checked to match the hostname in addr. Otherwise, flags
// should be 0.
//
// Dial probably won't work for you unless you set a verify location or add
// some certs to the certificate store of the client context you're using.
// This library is not nice enough to use the system certificate store by
// default for you yet.
//
// If session is not nil it will be used to resume the tls state. The session
// can be retrieved from the GetSession method on the Conn.
func DialSession(network, addr string, ctx *Ctx, flags DialFlags,
	session []byte) (*Conn, error) {
	return dialSessionWithDialer(
		new(net.Dialer),
		network,
		addr,
		ctx,
		flags,
		session,
	)
}

func dialSessionWithDialer(dialer *net.Dialer, network, addr string, ctx *Ctx, flags DialFlags,
	session []byte) (*Conn, error) {

	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return nil, err
	}
	if ctx == nil {
		var err error
		ctx, err = NewCtx()
		if err != nil {
			return nil, err
		}
		// TODO: use operating system default certificate chain?
	}
	c, err := dialer.Dial(network, addr)
	if err != nil {
		return nil, err
	}
	conn, err := Client(c, ctx)
	if err != nil {
		c.Close()
		return nil, err
	}
	if session != nil {
		err := conn.setSession(session)
		if err != nil {
			c.Close()
			return nil, err
		}
	}
	if flags&DisableSNI == 0 {
		err = conn.SetTlsExtHostName(host)
		if err != nil {
			conn.Close()
			return nil, err
		}
	}
	err = conn.Handshake()
	if err != nil {
		conn.Close()
		return nil, err
	}
	if flags&InsecureSkipHostVerification == 0 {
		err = conn.VerifyHostname(host)
		if err != nil {
			conn.Close()
			return nil, err
		}
	}
	return conn, nil
}
