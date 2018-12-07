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

// +build cgo

package openssl

// #include <openssl/conf.h>
// #include <openssl/ssl.h>
// #include <openssl/x509v3.h>
//
// void OPENSSL_free_not_a_macro(void *ref) { OPENSSL_free(ref); }
//
import "C"

import (
	"errors"
	"io/ioutil"
	"math/big"
	"runtime"
	"time"
	"unsafe"
)

type EVP_MD int

const (
	EVP_NULL      EVP_MD = iota
	EVP_MD5       EVP_MD = iota
	EVP_SHA       EVP_MD = iota
	EVP_SHA1      EVP_MD = iota
	EVP_DSS       EVP_MD = iota
	EVP_DSS1      EVP_MD = iota
	EVP_MDC2      EVP_MD = iota
	EVP_RIPEMD160 EVP_MD = iota
	EVP_SHA224    EVP_MD = iota
	EVP_SHA256    EVP_MD = iota
	EVP_SHA384    EVP_MD = iota
	EVP_SHA512    EVP_MD = iota
)

type Certificate struct {
	x      *C.X509
	Issuer *Certificate
	ref    interface{}
	pubKey PublicKey
}

type CertificateInfo struct {
	Serial       *big.Int
	Issued       time.Duration
	Expires      time.Duration
	Country      string
	Organization string
	CommonName   string
}

type Name struct {
	name *C.X509_NAME
}

// Allocate and return a new Name object.
func NewName() (*Name, error) {
	n := C.X509_NAME_new()
	if n == nil {
		return nil, errors.New("could not create x509 name")
	}
	name := &Name{name: n}
	runtime.SetFinalizer(name, func(n *Name) {
		C.X509_NAME_free(n.name)
	})
	return name, nil
}

// AddTextEntry appends a text entry to an X509 NAME.
func (n *Name) AddTextEntry(field, value string) error {
	cfield := C.CString(field)
	defer C.free(unsafe.Pointer(cfield))
	cvalue := (*C.uchar)(unsafe.Pointer(C.CString(value)))
	defer C.free(unsafe.Pointer(cvalue))
	ret := C.X509_NAME_add_entry_by_txt(
		n.name, cfield, C.MBSTRING_ASC, cvalue, -1, -1, 0)
	if ret != 1 {
		return errors.New("failed to add x509 name text entry")
	}
	return nil
}

// AddTextEntries allows adding multiple entries to a name in one call.
func (n *Name) AddTextEntries(entries map[string]string) error {
	for f, v := range entries {
		if err := n.AddTextEntry(f, v); err != nil {
			return err
		}
	}
	return nil
}

// GetEntry returns a name entry based on NID.  If no entry, then ("", false) is
// returned.
func (n *Name) GetEntry(nid NID) (entry string, ok bool) {
	entrylen := C.X509_NAME_get_text_by_NID(n.name, C.int(nid), nil, 0)
	if entrylen == -1 {
		return "", false
	}
	buf := (*C.char)(C.malloc(C.size_t(entrylen + 1)))
	defer C.free(unsafe.Pointer(buf))
	C.X509_NAME_get_text_by_NID(n.name, C.int(nid), buf, entrylen+1)
	return C.GoStringN(buf, entrylen), true
}

// NewCertificate generates a basic certificate based
// on the provided CertificateInfo struct
func NewCertificate(info *CertificateInfo, key PublicKey) (*Certificate, error) {
	c := &Certificate{x: C.X509_new()}
	runtime.SetFinalizer(c, func(c *Certificate) {
		C.X509_free(c.x)
	})

	name, err := c.GetSubjectName()
	if err != nil {
		return nil, err
	}
	err = name.AddTextEntries(map[string]string{
		"C":  info.Country,
		"O":  info.Organization,
		"CN": info.CommonName,
	})
	if err != nil {
		return nil, err
	}
	// self-issue for now
	if err := c.SetIssuerName(name); err != nil {
		return nil, err
	}
	if err := c.SetSerial(info.Serial); err != nil {
		return nil, err
	}
	if err := c.SetIssueDate(info.Issued); err != nil {
		return nil, err
	}
	if err := c.SetExpireDate(info.Expires); err != nil {
		return nil, err
	}
	if err := c.SetPubKey(key); err != nil {
		return nil, err
	}
	return c, nil
}

func (c *Certificate) GetSubjectName() (*Name, error) {
	n := C.X509_get_subject_name(c.x)
	if n == nil {
		return nil, errors.New("failed to get subject name")
	}
	return &Name{name: n}, nil
}

func (c *Certificate) GetIssuerName() (*Name, error) {
	n := C.X509_get_issuer_name(c.x)
	if n == nil {
		return nil, errors.New("failed to get issuer name")
	}
	return &Name{name: n}, nil
}

func (c *Certificate) SetSubjectName(name *Name) error {
	if C.X509_set_subject_name(c.x, name.name) != 1 {
		return errors.New("failed to set subject name")
	}
	return nil
}

// SetIssuer updates the stored Issuer cert
// and the internal x509 Issuer Name of a certificate.
// The stored Issuer reference is used when adding extensions.
func (c *Certificate) SetIssuer(issuer *Certificate) error {
	name, err := issuer.GetSubjectName()
	if err != nil {
		return err
	}
	if err = c.SetIssuerName(name); err != nil {
		return err
	}
	c.Issuer = issuer
	return nil
}

// SetIssuerName populates the issuer name of a certificate.
// Use SetIssuer instead, if possible.
func (c *Certificate) SetIssuerName(name *Name) error {
	if C.X509_set_issuer_name(c.x, name.name) != 1 {
		return errors.New("failed to set subject name")
	}
	return nil
}

// SetSerial sets the serial of a certificate.
func (c *Certificate) SetSerial(serial *big.Int) error {
	sno := C.ASN1_INTEGER_new()
	defer C.ASN1_INTEGER_free(sno)
	bn := C.BN_new()
	defer C.BN_free(bn)

	serialBytes := serial.Bytes()
	if bn = C.BN_bin2bn((*C.uchar)(unsafe.Pointer(&serialBytes[0])), C.int(len(serialBytes)), bn); bn == nil {
		return errors.New("failed to set serial")
	}
	if sno = C.BN_to_ASN1_INTEGER(bn, sno); sno == nil {
		return errors.New("failed to set serial")
	}
	if C.X509_set_serialNumber(c.x, sno) != 1 {
		return errors.New("failed to set serial")
	}
	return nil
}

// SetIssueDate sets the certificate issue date relative to the current time.
func (c *Certificate) SetIssueDate(when time.Duration) error {
	offset := C.long(when / time.Second)
	result := C.X509_gmtime_adj(c.x.cert_info.validity.notBefore, offset)
	if result == nil {
		return errors.New("failed to set issue date")
	}
	return nil
}

// SetExpireDate sets the certificate issue date relative to the current time.
func (c *Certificate) SetExpireDate(when time.Duration) error {
	offset := C.long(when / time.Second)
	result := C.X509_gmtime_adj(c.x.cert_info.validity.notAfter, offset)
	if result == nil {
		return errors.New("failed to set expire date")
	}
	return nil
}

// SetPubKey assigns a new public key to a certificate.
func (c *Certificate) SetPubKey(pubKey PublicKey) error {
	c.pubKey = pubKey
	if C.X509_set_pubkey(c.x, pubKey.evpPKey()) != 1 {
		return errors.New("failed to set public key")
	}
	return nil
}

// Sign a certificate using a private key and a digest name.
// Accepted digest names are 'sha256', 'sha384', and 'sha512'.
func (c *Certificate) Sign(privKey PrivateKey, digest EVP_MD) error {
	switch digest {
	case EVP_SHA256:
	case EVP_SHA384:
	case EVP_SHA512:
	default:
		return errors.New("Unsupported digest" +
			"You're probably looking for 'EVP_SHA256' or 'EVP_SHA512'.")
	}
	return c.insecureSign(privKey, digest)
}

func (c *Certificate) insecureSign(privKey PrivateKey, digest EVP_MD) error {
	var md *C.EVP_MD
	switch digest {
	// please don't use these digest functions
	case EVP_NULL:
		md = C.EVP_md_null()
	case EVP_MD5:
		md = C.EVP_md5()
	case EVP_SHA:
		md = C.EVP_sha()
	case EVP_SHA1:
		md = C.EVP_sha1()
	case EVP_DSS:
		md = C.EVP_dss()
	case EVP_DSS1:
		md = C.EVP_dss1()
	case EVP_RIPEMD160:
		md = C.EVP_ripemd160()
	case EVP_SHA224:
		md = C.EVP_sha224()
	// you actually want one of these
	case EVP_SHA256:
		md = C.EVP_sha256()
	case EVP_SHA384:
		md = C.EVP_sha384()
	case EVP_SHA512:
		md = C.EVP_sha512()
	}
	if C.X509_sign(c.x, privKey.evpPKey(), md) <= 0 {
		return errors.New("failed to sign certificate")
	}
	return nil
}

// Add an extension to a certificate.
// Extension constants are NID_* as found in openssl.
func (c *Certificate) AddExtension(nid NID, value string) error {
	issuer := c
	if c.Issuer != nil {
		issuer = c.Issuer
	}
	var ctx C.X509V3_CTX
	C.X509V3_set_ctx(&ctx, c.x, issuer.x, nil, nil, 0)
	ex := C.X509V3_EXT_conf_nid(nil, &ctx, C.int(nid), C.CString(value))
	if ex == nil {
		return errors.New("failed to create x509v3 extension")
	}
	defer C.X509_EXTENSION_free(ex)
	if C.X509_add_ext(c.x, ex, -1) <= 0 {
		return errors.New("failed to add x509v3 extension")
	}
	return nil
}

// Wraps AddExtension using a map of NID to text extension.
// Will return without finishing if it encounters an error.
func (c *Certificate) AddExtensions(extensions map[NID]string) error {
	for nid, value := range extensions {
		if err := c.AddExtension(nid, value); err != nil {
			return err
		}
	}
	return nil
}

// LoadCertificateFromPEM loads an X509 certificate from a PEM-encoded block.
func LoadCertificateFromPEM(pem_block []byte) (*Certificate, error) {
	if len(pem_block) == 0 {
		return nil, errors.New("empty pem block")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	bio := C.BIO_new_mem_buf(unsafe.Pointer(&pem_block[0]),
		C.int(len(pem_block)))
	cert := C.PEM_read_bio_X509(bio, nil, nil, nil)
	C.BIO_free(bio)
	if cert == nil {
		return nil, errorFromErrorQueue()
	}
	x := &Certificate{x: cert}
	runtime.SetFinalizer(x, func(x *Certificate) {
		C.X509_free(x.x)
	})
	return x, nil
}

// MarshalPEM converts the X509 certificate to PEM-encoded format
func (c *Certificate) MarshalPEM() (pem_block []byte, err error) {
	bio := C.BIO_new(C.BIO_s_mem())
	if bio == nil {
		return nil, errors.New("failed to allocate memory BIO")
	}
	defer C.BIO_free(bio)
	if int(C.PEM_write_bio_X509(bio, c.x)) != 1 {
		return nil, errors.New("failed dumping certificate")
	}
	return ioutil.ReadAll(asAnyBio(bio))
}

// PublicKey returns the public key embedded in the X509 certificate.
func (c *Certificate) PublicKey() (PublicKey, error) {
	pkey := C.X509_get_pubkey(c.x)
	if pkey == nil {
		return nil, errors.New("no public key found")
	}
	key := &pKey{key: pkey}
	runtime.SetFinalizer(key, func(key *pKey) {
		C.EVP_PKEY_free(key.key)
	})
	return key, nil
}

// GetSerialNumberHex returns the certificate's serial number in hex format
func (c *Certificate) GetSerialNumberHex() (serial string) {
	asn1_i := C.X509_get_serialNumber(c.x)
	bignum := C.ASN1_INTEGER_to_BN(asn1_i, nil)
	hex := C.BN_bn2hex(bignum)
	serial = C.GoString(hex)
	C.BN_free(bignum)
	C.OPENSSL_free_not_a_macro(unsafe.Pointer(hex))
	return
}

func (c *Certificate) X509NamePrintEx() (out []byte, err error) {
	bio := C.BIO_new(C.BIO_s_mem())
	if bio == nil {
		return nil, errors.New("failed to allocate memory BIO")
	}
	defer C.BIO_free(bio)
	name := C.X509_get_subject_name(c.x)
	// TODO, pass in flags instead of using this hardcoded one
	if int(C.X509_NAME_print_ex(bio, name, 0, C.XN_FLAG_RFC2253)) < 0 {
		return nil, errors.New("failed formatting subject")
	}
	return ioutil.ReadAll(asAnyBio(bio))
}
