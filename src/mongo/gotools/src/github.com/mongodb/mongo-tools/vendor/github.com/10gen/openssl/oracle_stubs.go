// Copyright (C) 2014 Space Monkey, Inc.
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

// +build !cgo

package openssl

import (
	"errors"
	"net"
	"time"
)

const (
	SSLRecordSize = 16 * 1024
)

type Conn struct{}

func Client(conn net.Conn, ctx *Ctx) (*Conn, error)
func Server(conn net.Conn, ctx *Ctx) (*Conn, error)

func (c *Conn) Handshake() error
func (c *Conn) PeerCertificate() (*Certificate, error)
func (c *Conn) Close() error
func (c *Conn) Read(b []byte) (n int, err error)
func (c *Conn) Write(b []byte) (written int, err error)

func (c *Conn) VerifyHostname(host string) error

func (c *Conn) LocalAddr() net.Addr
func (c *Conn) RemoteAddr() net.Addr
func (c *Conn) SetDeadline(t time.Time) error
func (c *Conn) SetReadDeadline(t time.Time) error
func (c *Conn) SetWriteDeadline(t time.Time) error

type Ctx struct{}

type SSLVersion int

const (
	SSLv3      SSLVersion = 0x02
	TLSv1      SSLVersion = 0x03
	TLSv1_1    SSLVersion = 0x04
	TLSv1_2    SSLVersion = 0x05
	AnyVersion SSLVersion = 0x06
)

func NewCtxWithVersion(version SSLVersion) (*Ctx, error)
func NewCtx() (*Ctx, error)
func NewCtxFromFiles(cert_file string, key_file string) (*Ctx, error)
func (c *Ctx) UseCertificate(cert *Certificate) error
func (c *Ctx) UsePrivateKey(key PrivateKey) error

type CertificateStore struct{}

func (c *Ctx) GetCertificateStore() *CertificateStore

func (s *CertificateStore) AddCertificate(cert *Certificate) error

func (c *Ctx) LoadVerifyLocations(ca_file string, ca_path string) error

type Options int

const (
	NoCompression                      Options = 0
	NoSSLv2                            Options = 0
	NoSSLv3                            Options = 0
	NoTLSv1                            Options = 0
	CipherServerPreference             Options = 0
	NoSessionResumptionOrRenegotiation Options = 0
	NoTicket                           Options = 0
)

func (c *Ctx) SetOptions(options Options) Options

type Modes int

const (
	ReleaseBuffers Modes = 0
)

func (c *Ctx) SetMode(modes Modes) Modes

type VerifyOptions int

const (
	VerifyNone             VerifyOptions = 0
	VerifyPeer             VerifyOptions = 0
	VerifyFailIfNoPeerCert VerifyOptions = 0
	VerifyClientOnce       VerifyOptions = 0
)

func (c *Ctx) SetVerify(options VerifyOptions)
func (c *Ctx) SetVerifyDepth(depth int)
func (c *Ctx) SetSessionId(session_id []byte) error

func (c *Ctx) SetCipherList(list string) error

type SessionCacheModes int

const (
	SessionCacheOff    SessionCacheModes = 0
	SessionCacheClient SessionCacheModes = 0
	SessionCacheServer SessionCacheModes = 0
	SessionCacheBoth   SessionCacheModes = 0
	NoAutoClear        SessionCacheModes = 0
	NoInternalLookup   SessionCacheModes = 0
	NoInternalStore    SessionCacheModes = 0
	NoInternal         SessionCacheModes = 0
)

func (c *Ctx) SetSessionCacheMode(modes SessionCacheModes) SessionCacheModes

var (
	ValidationError = errors.New("Host validation error")
)

type CheckFlags int

const (
	AlwaysCheckSubject CheckFlags = 0
	NoWildcards        CheckFlags = 0
)

func (c *Certificate) CheckHost(host string, flags CheckFlags) error
func (c *Certificate) CheckEmail(email string, flags CheckFlags) error
func (c *Certificate) CheckIP(ip net.IP, flags CheckFlags) error
func (c *Certificate) VerifyHostname(host string) error

type PublicKey interface {
	MarshalPKIXPublicKeyPEM() (pem_block []byte, err error)
	MarshalPKIXPublicKeyDER() (der_block []byte, err error)
	evpPKey() struct{}
}

type PrivateKey interface {
	PublicKey
	MarshalPKCS1PrivateKeyPEM() (pem_block []byte, err error)
	MarshalPKCS1PrivateKeyDER() (der_block []byte, err error)
}

func LoadPrivateKeyFromPEM(pem_block []byte) (PrivateKey, error)

type Certificate struct{}

func LoadCertificateFromPEM(pem_block []byte) (*Certificate, error)

func (c *Certificate) MarshalPEM() (pem_block []byte, err error)

func (c *Certificate) PublicKey() (PublicKey, error)
