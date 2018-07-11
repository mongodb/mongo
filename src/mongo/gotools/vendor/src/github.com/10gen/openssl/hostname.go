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
#include <openssl/ssl.h>
#include <openssl/conf.h>
#include <openssl/x509.h>

#ifndef X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT
#define X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT	0x1
#define X509_CHECK_FLAG_NO_WILDCARDS	0x2

extern int X509_check_host(X509 *x, const unsigned char *chk, size_t chklen,
    unsigned int flags, char **peername);
extern int X509_check_email(X509 *x, const unsigned char *chk, size_t chklen,
    unsigned int flags);
extern int X509_check_ip(X509 *x, const unsigned char *chk, size_t chklen,
		unsigned int flags);
#endif
*/
import "C"

import (
	"errors"
	"net"
	"unsafe"
)

var (
	ValidationError = errors.New("Host validation error")
)

type CheckFlags int

const (
	AlwaysCheckSubject CheckFlags = C.X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT
	NoWildcards        CheckFlags = C.X509_CHECK_FLAG_NO_WILDCARDS
)

// CheckHost checks that the X509 certificate is signed for the provided
// host name. See http://www.openssl.org/docs/crypto/X509_check_host.html for
// more. Note that CheckHost does not check the IP field. See VerifyHostname.
// Specifically returns ValidationError if the Certificate didn't match but
// there was no internal error.
func (c *Certificate) CheckHost(host string, flags CheckFlags) error {
	chost := unsafe.Pointer(C.CString(host))
	defer C.free(chost)

	rv := C.X509_check_host(c.x, (*C.uchar)(chost), C.size_t(len(host)),
		C.uint(flags), nil)
	if rv > 0 {
		return nil
	}
	if rv == 0 {
		return ValidationError
	}
	return errors.New("hostname validation had an internal failure")
}

// CheckEmail checks that the X509 certificate is signed for the provided
// email address. See http://www.openssl.org/docs/crypto/X509_check_host.html
// for more.
// Specifically returns ValidationError if the Certificate didn't match but
// there was no internal error.
func (c *Certificate) CheckEmail(email string, flags CheckFlags) error {
	cemail := unsafe.Pointer(C.CString(email))
	defer C.free(cemail)
	rv := C.X509_check_email(c.x, (*C.uchar)(cemail), C.size_t(len(email)),
		C.uint(flags))
	if rv > 0 {
		return nil
	}
	if rv == 0 {
		return ValidationError
	}
	return errors.New("email validation had an internal failure")
}

// CheckIP checks that the X509 certificate is signed for the provided
// IP address. See http://www.openssl.org/docs/crypto/X509_check_host.html
// for more.
// Specifically returns ValidationError if the Certificate didn't match but
// there was no internal error.
func (c *Certificate) CheckIP(ip net.IP, flags CheckFlags) error {
	cip := unsafe.Pointer(&ip[0])
	rv := C.X509_check_ip(c.x, (*C.uchar)(cip), C.size_t(len(ip)),
		C.uint(flags))
	if rv > 0 {
		return nil
	}
	if rv == 0 {
		return ValidationError
	}
	return errors.New("ip validation had an internal failure")
}

// VerifyHostname is a combination of CheckHost and CheckIP. If the provided
// hostname looks like an IP address, it will be checked as an IP address,
// otherwise it will be checked as a hostname.
// Specifically returns ValidationError if the Certificate didn't match but
// there was no internal error.
func (c *Certificate) VerifyHostname(host string) error {
	var ip net.IP
	if len(host) >= 3 && host[0] == '[' && host[len(host)-1] == ']' {
		ip = net.ParseIP(host[1 : len(host)-1])
	} else {
		ip = net.ParseIP(host)
	}
	if ip != nil {
		return c.CheckIP(ip, 0)
	}
	return c.CheckHost(host, 0)
}
