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

/*
#include "shim.h"
#include <openssl/err.h>

typedef STACK_OF(X509_NAME) *STACK_OF_X509_NAME_not_a_macro;

static void sk_X509_NAME_pop_free_not_a_macro(STACK_OF_X509_NAME_not_a_macro st) {
		sk_X509_NAME_pop_free(st, X509_NAME_free);
}

extern int password_cb(char *buf, int size, int rwflag, void *password);

*/
import "C"

import (
	"errors"
	"fmt"
	"io/ioutil"
	"os"
	"runtime"
	"sync"
	"time"
	"unsafe"

	"github.com/spacemonkeygo/spacelog"
)

var (
	ssl_ctx_idx = C.X_SSL_CTX_new_index()

	logger = spacelog.GetLogger()
)

type Ctx struct {
	ctx       *C.SSL_CTX
	cert      *Certificate
	chain     []*Certificate
	key       PrivateKey
	verify_cb VerifyCallback
	sni_cb    TLSExtServernameCallback

	ticket_store_mu sync.Mutex
	ticket_store    *TicketStore
}

//export get_ssl_ctx_idx
func get_ssl_ctx_idx() C.int {
	return ssl_ctx_idx
}

func newCtx(method *C.SSL_METHOD) (*Ctx, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	ctx := C.SSL_CTX_new(method)
	if ctx == nil {
		return nil, errorFromErrorQueue()
	}
	c := &Ctx{ctx: ctx}
	C.SSL_CTX_set_ex_data(ctx, get_ssl_ctx_idx(), unsafe.Pointer(c))
	runtime.SetFinalizer(c, func(c *Ctx) {
		C.SSL_CTX_free(c.ctx)
	})
	return c, nil
}

type SSLVersion int

const (
	SSLv3   SSLVersion = 0x02 // Vulnerable to "POODLE" attack.
	TLSv1   SSLVersion = 0x03
	TLSv1_1 SSLVersion = 0x04
	TLSv1_2 SSLVersion = 0x05

	// Make sure to disable SSLv2 and SSLv3 if you use this. SSLv3 is vulnerable
	// to the "POODLE" attack, and SSLv2 is what, just don't even.
	AnyVersion SSLVersion = 0x06
)

// NewCtxWithVersion creates an SSL context that is specific to the provided
// SSL version. See http://www.openssl.org/docs/ssl/SSL_CTX_new.html for more.
func NewCtxWithVersion(version SSLVersion) (*Ctx, error) {
	var method *C.SSL_METHOD
	switch version {
	case SSLv3:
		method = C.X_SSLv3_method()
	case TLSv1:
		method = C.X_TLSv1_method()
	case TLSv1_1:
		method = C.X_TLSv1_1_method()
	case TLSv1_2:
		method = C.X_TLSv1_2_method()
	case AnyVersion:
		method = C.X_SSLv23_method()
	}
	if method == nil {
		return nil, errors.New("unknown ssl/tls version")
	}
	return newCtx(method)
}

// NewCtx creates a context that supports any TLS version 1.0 and newer.
func NewCtx() (*Ctx, error) {
	c, err := NewCtxWithVersion(AnyVersion)
	if err == nil {
		c.SetOptions(NoSSLv2 | NoSSLv3)
	}
	return c, err
}

// NewCtxFromFiles calls NewCtx, loads the provided files, and configures the
// context to use them.
func NewCtxFromFiles(cert_file string, key_file string) (*Ctx, error) {
	ctx, err := NewCtx()
	if err != nil {
		return nil, err
	}

	cert_bytes, err := ioutil.ReadFile(cert_file)
	if err != nil {
		return nil, err
	}

	certs := SplitPEM(cert_bytes)
	if len(certs) == 0 {
		return nil, fmt.Errorf("No PEM certificate found in '%s'", cert_file)
	}
	first, certs := certs[0], certs[1:]
	cert, err := LoadCertificateFromPEM(first)
	if err != nil {
		return nil, err
	}

	err = ctx.UseCertificate(cert)
	if err != nil {
		return nil, err
	}

	for _, pem := range certs {
		cert, err := LoadCertificateFromPEM(pem)
		if err != nil {
			return nil, err
		}
		err = ctx.AddChainCertificate(cert)
		if err != nil {
			return nil, err
		}
	}

	key_bytes, err := ioutil.ReadFile(key_file)
	if err != nil {
		return nil, err
	}

	key, err := LoadPrivateKeyFromPEM(key_bytes)
	if err != nil {
		return nil, err
	}

	err = ctx.UsePrivateKey(key)
	if err != nil {
		return nil, err
	}

	return ctx, nil
}

// EllipticCurve repesents the ASN.1 OID of an elliptic curve.
// see https://www.openssl.org/docs/apps/ecparam.html for a list of implemented curves.
type EllipticCurve int

const (
	// P-256: X9.62/SECG curve over a 256 bit prime field
	Prime256v1 EllipticCurve = C.NID_X9_62_prime256v1
	// P-384: NIST/SECG curve over a 384 bit prime field
	Secp384r1 EllipticCurve = C.NID_secp384r1
	// P-521: NIST/SECG curve over a 521 bit prime field
	Secp521r1 EllipticCurve = C.NID_secp521r1
)

// UseCertificate configures the context to present the given certificate to
// peers.
func (c *Ctx) UseCertificate(cert *Certificate) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	c.cert = cert
	if int(C.SSL_CTX_use_certificate(c.ctx, cert.x)) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

// UseCertificateChainFromFile loads a certificate chain from file into ctx.
// The certificates must be in PEM format and must be sorted starting with the
// subject's certificate (actual client or server certificate), followed by
// intermediate CA certificates if applicable, and ending at the highest level
// (root) CA. See
// https://www.openssl.org/docs/ssl/SSL_CTX_use_certificate.html
func (c *Ctx) UseCertificateChainFile(cert_file string) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var c_cert_file *C.char
	if cert_file != "" {
		c_cert_file = C.CString(cert_file)
		defer C.free(unsafe.Pointer(c_cert_file))
	}
	if int(C.SSL_CTX_use_certificate_chain_file(c.ctx, c_cert_file)) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

// UsePrivateKeyFile adds the first private key found in file to the *Ctx, c. The
// formatting type of the certificate must be specified from the known types
// FiletypePEM, and FiletypeASN1
func (c *Ctx) UsePrivateKeyFile(key_file string, file_type Filetypes) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var c_key_file *C.char
	if key_file != "" {
		c_key_file = C.CString(key_file)
		defer C.free(unsafe.Pointer(c_key_file))
	}
	if int(C.SSL_CTX_use_PrivateKey_file(c.ctx, c_key_file, C.int(file_type))) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

func (c *Ctx) UsePrivateKeyFileWithPassword(key_file string, file_type Filetypes, password string) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var c_key_file *C.char

	c_pwd := C.CString(password)
	defer C.free(unsafe.Pointer(c_pwd))
	C.SSL_CTX_set_default_passwd_cb_userdata(c.ctx, unsafe.Pointer(c_pwd))
	C.SSL_CTX_set_default_passwd_cb(c.ctx, (*C.pem_password_cb)(C.password_cb))

	if key_file != "" {
		c_key_file = C.CString(key_file)
		defer C.free(unsafe.Pointer(c_key_file))
	}
	if int(C.SSL_CTX_use_PrivateKey_file(c.ctx, c_key_file, C.int(file_type))) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

// CheckPrivateKey verifies that the private key agrees with the corresponding
// public key in the certificate
func (c *Ctx) CheckPrivateKey() error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if int(C.SSL_CTX_check_private_key(c.ctx)) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

type StackOfX509Name struct {
	stack C.STACK_OF_X509_NAME_not_a_macro
	// shared indicates weather we are the sole owner of this pointer, and implies
	// weather we should or shouldn't free the underlying data structure
	// when this go data structure goes out of scope
	shared bool
}

// LoadClientCAFile reads certificates from file and returns a StackOfX509Name
// with the subject names found. See
// https://www.openssl.org/docs/ssl/SSL_load_client_CA_file.html
func LoadClientCAFile(ca_file string) (*StackOfX509Name, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var c_ca_file *C.char
	if ca_file != "" {
		c_ca_file = C.CString(ca_file)
		defer C.free(unsafe.Pointer(c_ca_file))
	}
	stack := C.SSL_load_client_CA_file(c_ca_file)
	if stack == nil {
		return nil, errorFromErrorQueue()
	}
	caList := StackOfX509Name{
		stack:  stack,
		shared: false,
	}
	runtime.SetFinalizer(&caList, func(c *StackOfX509Name) {
		if !c.shared {
			C.sk_X509_NAME_pop_free_not_a_macro(c.stack)
		}
	})
	return &caList, nil
}

// SetClientCAList sets the list of CAs sent to the client when requesting a
// client certificate for Ctx. See
// https://www.openssl.org/docs/ssl/SSL_CTX_set_client_CA_list.html
func (c *Ctx) SetClientCAList(caList *StackOfX509Name) {
	C.SSL_CTX_set_client_CA_list(c.ctx, caList.stack)
	caList.shared = true
}

// AddChainCertificate adds a certificate to the chain presented in the
// handshake.
func (c *Ctx) AddChainCertificate(cert *Certificate) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	c.chain = append(c.chain, cert)
	if int(C.X_SSL_CTX_add_extra_chain_cert(c.ctx, cert.x)) != 1 {
		return errorFromErrorQueue()
	}
	// OpenSSL takes ownership via SSL_CTX_add_extra_chain_cert
	runtime.SetFinalizer(cert, nil)
	return nil
}

// UsePrivateKey configures the context to use the given private key for SSL
// handshakes.
func (c *Ctx) UsePrivateKey(key PrivateKey) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	c.key = key
	if int(C.SSL_CTX_use_PrivateKey(c.ctx, key.evpPKey())) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

type CertificateStore struct {
	store *C.X509_STORE
	// for GC
	ctx   *Ctx
	certs []*Certificate
}

// Allocate a new, empty CertificateStore
func NewCertificateStore() (*CertificateStore, error) {
	s := C.X509_STORE_new()
	if s == nil {
		return nil, errors.New("failed to allocate X509_STORE")
	}
	store := &CertificateStore{store: s}
	runtime.SetFinalizer(store, func(s *CertificateStore) {
		C.X509_STORE_free(s.store)
	})
	return store, nil
}

// Parse a chained PEM file, loading all certificates into the Store.
func (s *CertificateStore) LoadCertificatesFromPEM(data []byte) error {
	pems := SplitPEM(data)
	for _, pem := range pems {
		cert, err := LoadCertificateFromPEM(pem)
		if err != nil {
			return err
		}
		err = s.AddCertificate(cert)
		if err != nil {
			return err
		}
	}
	return nil
}

// GetCertificateStore returns the context's certificate store that will be
// used for peer validation.
func (c *Ctx) GetCertificateStore() *CertificateStore {
	// we don't need to dealloc the cert store pointer here, because it points
	// to a ctx internal. so we do need to keep the ctx around
	return &CertificateStore{
		store: C.SSL_CTX_get_cert_store(c.ctx),
		ctx:   c}
}

// AddCertificate marks the provided Certificate as a trusted certificate in
// the given CertificateStore.
func (s *CertificateStore) AddCertificate(cert *Certificate) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	s.certs = append(s.certs, cert)
	if int(C.X509_STORE_add_cert(s.store, cert.x)) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

type X509VerificationFlag int

func (s *CertificateStore) SetFlags(flags X509VerificationFlag) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if int(C.X509_STORE_set_flags(s.store, C.ulong(flags))) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

// See https://www.openssl.org/docs/crypto/X509_VERIFY_PARAM_set_flags.html
const (
	CBIssuerCheck   X509VerificationFlag = C.X509_V_FLAG_CB_ISSUER_CHECK
	UseCheckTime    X509VerificationFlag = C.X509_V_FLAG_USE_CHECK_TIME
	CRLCheck        X509VerificationFlag = C.X509_V_FLAG_CRL_CHECK
	CRLCheckAll     X509VerificationFlag = C.X509_V_FLAG_CRL_CHECK_ALL
	IgnoreCritical  X509VerificationFlag = C.X509_V_FLAG_IGNORE_CRITICAL
	X509Strict      X509VerificationFlag = C.X509_V_FLAG_X509_STRICT
	AllowProxyCerts X509VerificationFlag = C.X509_V_FLAG_ALLOW_PROXY_CERTS
	PolicyCheck     X509VerificationFlag = C.X509_V_FLAG_POLICY_CHECK
	ExplicitPolicy  X509VerificationFlag = C.X509_V_FLAG_EXPLICIT_POLICY
	InhibitAny      X509VerificationFlag = C.X509_V_FLAG_INHIBIT_ANY
	InhibitMap      X509VerificationFlag = C.X509_V_FLAG_INHIBIT_MAP
	NotifyPolicy    X509VerificationFlag = C.X509_V_FLAG_NOTIFY_POLICY
	//	ExtendedCRLSupport X509VerificationFlag = C.X509_V_FLAG_EXTENDED_CRL_SUPPORT
	//	UseDeltas          X509VerificationFlag = C.X509_V_FLAG_USE_DELTAS
	//	CheckSsSignature   X509VerificationFlag = C.X509_V_FLAG_CHECK_SS_SIGNATURE
	//	TrustedFirst       X509VerificationFlag = C.X509_V_FLAG_TRUSTED_FIRST
	PolicyMask X509VerificationFlag = C.X509_V_FLAG_POLICY_MASK
)

type CertificateStoreLookup struct {
	lookup *C.X509_LOOKUP
	store  *CertificateStore
}

// an X509LookupMethod is required to build a a CertificateStoreLookup in a
// CertificateStore.  The X509LookupMethod indicates the type or functionality
// of the CertificateStoreLookup
type X509LookupMethod *C.X509_LOOKUP_METHOD

// CertificateStoreLookups with X509LookupFile methods look for certs in a file
func X509LookupFile() X509LookupMethod {
	return X509LookupMethod(C.X509_LOOKUP_file())
}

// CertificateStoreLookups with X509LookupHashDir methods look for certs in a
// directory
func X509LookupHashDir() X509LookupMethod {
	return X509LookupMethod(C.X509_LOOKUP_hash_dir())
}

// AddLookup creates a CertificateStoreLookup of type X509LookupMethod in the
// CertificateStore
func (s *CertificateStore) AddLookup(method X509LookupMethod) (*CertificateStoreLookup, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var lookup *C.X509_LOOKUP
	lookup = C.X509_STORE_add_lookup(s.store, method)
	if lookup != nil {
		return &CertificateStoreLookup{
			lookup: lookup,
			store:  s,
		}, nil
	}
	return nil, errorFromErrorQueue()
}

// LoadCRLFile adds a file to a CertificateStoreLookup in the
// CertificateStore
// I suspect that the CertificateStoreLookup needs to have been created with
// X509LookupFile as the lookup method
func (l *CertificateStoreLookup) LoadCRLFile(crl_file string) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var c_crl_file *C.char
	if crl_file != "" {
		c_crl_file = C.CString(crl_file)
		defer C.free(unsafe.Pointer(c_crl_file))
	}
	if int(C.X509_load_crl_file(l.lookup, c_crl_file, C.X509_FILETYPE_PEM)) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

type CertificateStoreCtx struct {
	ctx     *C.X509_STORE_CTX
	ssl_ctx *Ctx
}

func (self *CertificateStoreCtx) VerifyResult() VerifyResult {
	return VerifyResult(C.X509_STORE_CTX_get_error(self.ctx))
}

func (self *CertificateStoreCtx) Err() error {
	code := C.X509_STORE_CTX_get_error(self.ctx)
	if code == C.X509_V_OK {
		return nil
	}
	return fmt.Errorf("openssl: %s",
		C.GoString(C.X509_verify_cert_error_string(C.long(code))))
}

func (self *CertificateStoreCtx) Depth() int {
	return int(C.X509_STORE_CTX_get_error_depth(self.ctx))
}

// the certicate returned is only valid for the lifetime of the underlying
// X509_STORE_CTX
func (self *CertificateStoreCtx) GetCurrentCert() *Certificate {
	x509 := C.X509_STORE_CTX_get_current_cert(self.ctx)
	if x509 == nil {
		return nil
	}
	// add a ref
	if 1 != C.X_X509_add_ref(x509) {
		return nil
	}
	cert := &Certificate{
		x: x509,
	}
	runtime.SetFinalizer(cert, func(cert *Certificate) {
		C.X509_free(cert.x)
	})
	return cert
}

// LoadVerifyLocations tells the context to trust all certificate authorities
// provided in either the ca_file or the ca_path.
// See http://www.openssl.org/docs/ssl/SSL_CTX_load_verify_locations.html for
// more.
func (c *Ctx) LoadVerifyLocations(ca_file string, ca_path string) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var c_ca_file, c_ca_path *C.char
	if ca_file != "" {
		c_ca_file = C.CString(ca_file)
		defer C.free(unsafe.Pointer(c_ca_file))
	}
	if ca_path != "" {
		c_ca_path = C.CString(ca_path)
		defer C.free(unsafe.Pointer(c_ca_path))
	}
	if C.SSL_CTX_load_verify_locations(c.ctx, c_ca_file, c_ca_path) != 1 {
		return errorFromErrorQueue()
	}
	return nil
}

type Options uint

const (
	// NoCompression is only valid if you are using OpenSSL 1.0.1 or newer
	NoCompression Options = C.SSL_OP_NO_COMPRESSION
	NoSSLv2       Options = C.SSL_OP_NO_SSLv2
	NoSSLv3       Options = C.SSL_OP_NO_SSLv3
	NoTLSv1       Options = C.SSL_OP_NO_TLSv1
	// NoTLSv1_1 and NoTLSv1_2 are only valid if you are using OpenSSL 1.0.1 or newer
	NoTLSv1_1                          Options = C.SSL_OP_NO_TLSv1_1
	NoTLSv1_2                          Options = C.SSL_OP_NO_TLSv1_2
	CipherServerPreference             Options = C.SSL_OP_CIPHER_SERVER_PREFERENCE
	NoSessionResumptionOrRenegotiation Options = C.SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION
	NoTicket                           Options = C.SSL_OP_NO_TICKET
	OpAll                              Options = C.SSL_OP_ALL
)

// SetOptions sets context options. See
// http://www.openssl.org/docs/ssl/SSL_CTX_set_options.html
func (c *Ctx) SetOptions(options Options) Options {
	return Options(C.X_SSL_CTX_set_options(
		c.ctx, C.long(options)))
}

func (c *Ctx) ClearOptions(options Options) Options {
	return Options(C.X_SSL_CTX_clear_options(
		c.ctx, C.long(options)))
}

// GetOptions returns context options. See
// https://www.openssl.org/docs/ssl/SSL_CTX_set_options.html
func (c *Ctx) GetOptions() Options {
	return Options(C.X_SSL_CTX_get_options(c.ctx))
}

type Modes int

const (
	// ReleaseBuffers is only valid if you are using OpenSSL 1.0.1 or newer
	ReleaseBuffers Modes = C.SSL_MODE_RELEASE_BUFFERS
	AutoRetry      Modes = C.SSL_MODE_AUTO_RETRY
)

// SetMode sets context modes. See
// http://www.openssl.org/docs/ssl/SSL_CTX_set_mode.html
func (c *Ctx) SetMode(modes Modes) Modes {
	return Modes(C.X_SSL_CTX_set_mode(c.ctx, C.long(modes)))
}

// GetMode returns context modes. See
// http://www.openssl.org/docs/ssl/SSL_CTX_set_mode.html
func (c *Ctx) GetMode() Modes {
	return Modes(C.X_SSL_CTX_get_mode(c.ctx))
}

type VerifyOptions int

const (
	VerifyNone             VerifyOptions = C.SSL_VERIFY_NONE
	VerifyPeer             VerifyOptions = C.SSL_VERIFY_PEER
	VerifyFailIfNoPeerCert VerifyOptions = C.SSL_VERIFY_FAIL_IF_NO_PEER_CERT
	VerifyClientOnce       VerifyOptions = C.SSL_VERIFY_CLIENT_ONCE
)

type Filetypes int

const (
	FiletypePEM  Filetypes = C.SSL_FILETYPE_PEM
	FiletypeASN1 Filetypes = C.SSL_FILETYPE_ASN1
)

type VerifyCallback func(ok bool, store *CertificateStoreCtx) bool

//export go_ssl_ctx_verify_cb_thunk
func go_ssl_ctx_verify_cb_thunk(p unsafe.Pointer, ok C.int, ctx *C.X509_STORE_CTX) C.int {
	defer func() {
		if err := recover(); err != nil {
			logger.Critf("openssl: verify callback panic'd: %v", err)
			os.Exit(1)
		}
	}()
	verify_cb := (*Ctx)(p).verify_cb
	// set up defaults just in case verify_cb is nil
	if verify_cb != nil {
		store := &CertificateStoreCtx{ctx: ctx}
		if verify_cb(ok == 1, store) {
			ok = 1
		} else {
			ok = 0
		}
	}
	return ok
}

// SetVerify controls peer verification settings. See
// http://www.openssl.org/docs/ssl/SSL_CTX_set_verify.html
func (c *Ctx) SetVerify(options VerifyOptions, verify_cb VerifyCallback) {
	c.verify_cb = verify_cb
	if verify_cb != nil {
		C.SSL_CTX_set_verify(c.ctx, C.int(options), (*[0]byte)(C.X_SSL_CTX_verify_cb))
	} else {
		C.SSL_CTX_set_verify(c.ctx, C.int(options), nil)
	}
}

func (c *Ctx) SetVerifyMode(options VerifyOptions) {
	c.SetVerify(options, c.verify_cb)
}

func (c *Ctx) SetVerifyCallback(verify_cb VerifyCallback) {
	c.SetVerify(c.VerifyMode(), verify_cb)
}

func (c *Ctx) GetVerifyCallback() VerifyCallback {
	return c.verify_cb
}

func (c *Ctx) VerifyMode() VerifyOptions {
	return VerifyOptions(C.SSL_CTX_get_verify_mode(c.ctx))
}

// SetVerifyDepth controls how many certificates deep the certificate
// verification logic is willing to follow a certificate chain. See
// https://www.openssl.org/docs/ssl/SSL_CTX_set_verify.html
func (c *Ctx) SetVerifyDepth(depth int) {
	C.SSL_CTX_set_verify_depth(c.ctx, C.int(depth))
}

// GetVerifyDepth controls how many certificates deep the certificate
// verification logic is willing to follow a certificate chain. See
// https://www.openssl.org/docs/ssl/SSL_CTX_set_verify.html
func (c *Ctx) GetVerifyDepth() int {
	return int(C.SSL_CTX_get_verify_depth(c.ctx))
}

type TLSExtServernameCallback func(ssl *SSL) SSLTLSExtErr

// SetTLSExtServernameCallback sets callback function for Server Name Indication
// (SNI) rfc6066 (http://tools.ietf.org/html/rfc6066). See
// http://stackoverflow.com/questions/22373332/serving-multiple-domains-in-one-box-with-sni
func (c *Ctx) SetTLSExtServernameCallback(sni_cb TLSExtServernameCallback) {
	c.sni_cb = sni_cb
	C.X_SSL_CTX_set_tlsext_servername_callback(c.ctx, (*[0]byte)(C.sni_cb))
}

func (c *Ctx) SetSessionId(session_id []byte) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var ptr *C.uchar
	if len(session_id) > 0 {
		ptr = (*C.uchar)(unsafe.Pointer(&session_id[0]))
	}
	if int(C.SSL_CTX_set_session_id_context(c.ctx, ptr,
		C.uint(len(session_id)))) == 0 {
		return errorFromErrorQueue()
	}
	return nil
}

// SetCipherList sets the list of available ciphers. The format of the list is
// described at http://www.openssl.org/docs/apps/ciphers.html, but see
// http://www.openssl.org/docs/ssl/SSL_CTX_set_cipher_list.html for more.
func (c *Ctx) SetCipherList(list string) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	clist := C.CString(list)
	defer C.free(unsafe.Pointer(clist))
	if int(C.SSL_CTX_set_cipher_list(c.ctx, clist)) == 0 {
		return errorFromErrorQueue()
	}
	return nil
}

type SessionCacheModes int

const (
	SessionCacheOff    SessionCacheModes = C.SSL_SESS_CACHE_OFF
	SessionCacheClient SessionCacheModes = C.SSL_SESS_CACHE_CLIENT
	SessionCacheServer SessionCacheModes = C.SSL_SESS_CACHE_SERVER
	SessionCacheBoth   SessionCacheModes = C.SSL_SESS_CACHE_BOTH
	NoAutoClear        SessionCacheModes = C.SSL_SESS_CACHE_NO_AUTO_CLEAR
	NoInternalLookup   SessionCacheModes = C.SSL_SESS_CACHE_NO_INTERNAL_LOOKUP
	NoInternalStore    SessionCacheModes = C.SSL_SESS_CACHE_NO_INTERNAL_STORE
	NoInternal         SessionCacheModes = C.SSL_SESS_CACHE_NO_INTERNAL
)

// SetSessionCacheMode enables or disables session caching. See
// http://www.openssl.org/docs/ssl/SSL_CTX_set_session_cache_mode.html
func (c *Ctx) SetSessionCacheMode(modes SessionCacheModes) SessionCacheModes {
	return SessionCacheModes(
		C.X_SSL_CTX_set_session_cache_mode(c.ctx, C.long(modes)))
}

// Set session cache timeout. Returns previously set value.
// See https://www.openssl.org/docs/ssl/SSL_CTX_set_timeout.html
func (c *Ctx) SetTimeout(t time.Duration) time.Duration {
	prev := C.X_SSL_CTX_set_timeout(c.ctx, C.long(t/time.Second))
	return time.Duration(prev) * time.Second
}

// Get session cache timeout.
// See https://www.openssl.org/docs/ssl/SSL_CTX_set_timeout.html
func (c *Ctx) GetTimeout() time.Duration {
	return time.Duration(C.X_SSL_CTX_get_timeout(c.ctx)) * time.Second
}

// Set session cache size. Returns previously set value.
// https://www.openssl.org/docs/ssl/SSL_CTX_sess_set_cache_size.html
func (c *Ctx) SessSetCacheSize(t int) int {
	return int(C.X_SSL_CTX_sess_set_cache_size(c.ctx, C.long(t)))
}

// Get session cache size.
// https://www.openssl.org/docs/ssl/SSL_CTX_sess_set_cache_size.html
func (c *Ctx) SessGetCacheSize() int {
	return int(C.X_SSL_CTX_sess_get_cache_size(c.ctx))
}
