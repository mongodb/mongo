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

// #include <openssl/evp.h>
// #include <openssl/ssl.h>
// #include <openssl/conf.h>
//
// void OPENSSL_free_not_a_macro(void *ref) { OPENSSL_free(ref); }
//
// int EVP_SignInit_not_a_macro(EVP_MD_CTX *ctx, const EVP_MD *type) {
//     return EVP_SignInit(ctx, type);
// }
//
// int EVP_SignUpdate_not_a_macro(EVP_MD_CTX *ctx, const void *d,
//   unsigned int cnt) {
//     return EVP_SignUpdate(ctx, d, cnt);
// }
//
// int EVP_VerifyInit_not_a_macro(EVP_MD_CTX *ctx, const EVP_MD *type) {
//     return EVP_VerifyInit(ctx, type);
// }
//
// int EVP_VerifyUpdate_not_a_macro(EVP_MD_CTX *ctx, const void *d,
//   unsigned int cnt) {
//     return EVP_VerifyUpdate(ctx, d, cnt);
// }
import "C"

import (
	"errors"
	"io/ioutil"
	"runtime"
	"unsafe"
)

type Method *C.EVP_MD

var (
	SHA256_Method Method = C.EVP_sha256()
)

type PublicKey interface {
	// Verifies the data signature using PKCS1.15
	VerifyPKCS1v15(method Method, data, sig []byte) error

	// MarshalPKIXPublicKeyPEM converts the public key to PEM-encoded PKIX
	// format
	MarshalPKIXPublicKeyPEM() (pem_block []byte, err error)

	// MarshalPKIXPublicKeyDER converts the public key to DER-encoded PKIX
	// format
	MarshalPKIXPublicKeyDER() (der_block []byte, err error)

	evpPKey() *C.EVP_PKEY
}

type PrivateKey interface {
	PublicKey

	// Signs the data using PKCS1.15
	SignPKCS1v15(Method, []byte) ([]byte, error)

	// MarshalPKCS1PrivateKeyPEM converts the private key to PEM-encoded PKCS1
	// format
	MarshalPKCS1PrivateKeyPEM() (pem_block []byte, err error)

	// MarshalPKCS1PrivateKeyDER converts the private key to DER-encoded PKCS1
	// format
	MarshalPKCS1PrivateKeyDER() (der_block []byte, err error)
}

type pKey struct {
	key *C.EVP_PKEY
}

func (key *pKey) evpPKey() *C.EVP_PKEY { return key.key }

func (key *pKey) SignPKCS1v15(method Method, data []byte) ([]byte, error) {
	var ctx C.EVP_MD_CTX
	C.EVP_MD_CTX_init(&ctx)
	defer C.EVP_MD_CTX_cleanup(&ctx)

	if 1 != C.EVP_SignInit_not_a_macro(&ctx, method) {
		return nil, errors.New("signpkcs1v15: failed to init signature")
	}
	if len(data) > 0 {
		if 1 != C.EVP_SignUpdate_not_a_macro(
			&ctx, unsafe.Pointer(&data[0]), C.uint(len(data))) {
			return nil, errors.New("signpkcs1v15: failed to update signature")
		}
	}
	sig := make([]byte, C.EVP_PKEY_size(key.key))
	var sigblen C.uint
	if 1 != C.EVP_SignFinal(&ctx,
		((*C.uchar)(unsafe.Pointer(&sig[0]))), &sigblen, key.key) {
		return nil, errors.New("signpkcs1v15: failed to finalize signature")
	}
	return sig[:sigblen], nil
}

func (key *pKey) VerifyPKCS1v15(method Method, data, sig []byte) error {
	var ctx C.EVP_MD_CTX
	C.EVP_MD_CTX_init(&ctx)
	defer C.EVP_MD_CTX_cleanup(&ctx)

	if 1 != C.EVP_VerifyInit_not_a_macro(&ctx, method) {
		return errors.New("verifypkcs1v15: failed to init verify")
	}
	if len(data) > 0 {
		if 1 != C.EVP_VerifyUpdate_not_a_macro(
			&ctx, unsafe.Pointer(&data[0]), C.uint(len(data))) {
			return errors.New("verifypkcs1v15: failed to update verify")
		}
	}
	if 1 != C.EVP_VerifyFinal(&ctx,
		((*C.uchar)(unsafe.Pointer(&sig[0]))), C.uint(len(sig)), key.key) {
		return errors.New("verifypkcs1v15: failed to finalize verify")
	}
	return nil
}

func (key *pKey) MarshalPKCS1PrivateKeyPEM() (pem_block []byte,
	err error) {
	bio := C.BIO_new(C.BIO_s_mem())
	if bio == nil {
		return nil, errors.New("failed to allocate memory BIO")
	}
	defer C.BIO_free(bio)
	rsa := (*C.RSA)(C.EVP_PKEY_get1_RSA(key.key))
	if rsa == nil {
		return nil, errors.New("failed getting rsa key")
	}
	defer C.RSA_free(rsa)
	if int(C.PEM_write_bio_RSAPrivateKey(bio, rsa, nil, nil, C.int(0), nil,
		nil)) != 1 {
		return nil, errors.New("failed dumping private key")
	}
	return ioutil.ReadAll(asAnyBio(bio))
}

func (key *pKey) MarshalPKCS1PrivateKeyDER() (der_block []byte,
	err error) {
	bio := C.BIO_new(C.BIO_s_mem())
	if bio == nil {
		return nil, errors.New("failed to allocate memory BIO")
	}
	defer C.BIO_free(bio)
	rsa := (*C.RSA)(C.EVP_PKEY_get1_RSA(key.key))
	if rsa == nil {
		return nil, errors.New("failed getting rsa key")
	}
	defer C.RSA_free(rsa)
	if int(C.i2d_RSAPrivateKey_bio(bio, rsa)) != 1 {
		return nil, errors.New("failed dumping private key der")
	}
	return ioutil.ReadAll(asAnyBio(bio))
}

func (key *pKey) MarshalPKIXPublicKeyPEM() (pem_block []byte,
	err error) {
	bio := C.BIO_new(C.BIO_s_mem())
	if bio == nil {
		return nil, errors.New("failed to allocate memory BIO")
	}
	defer C.BIO_free(bio)
	rsa := (*C.RSA)(C.EVP_PKEY_get1_RSA(key.key))
	if rsa == nil {
		return nil, errors.New("failed getting rsa key")
	}
	defer C.RSA_free(rsa)
	if int(C.PEM_write_bio_RSA_PUBKEY(bio, rsa)) != 1 {
		return nil, errors.New("failed dumping public key pem")
	}
	return ioutil.ReadAll(asAnyBio(bio))
}

func (key *pKey) MarshalPKIXPublicKeyDER() (der_block []byte,
	err error) {
	bio := C.BIO_new(C.BIO_s_mem())
	if bio == nil {
		return nil, errors.New("failed to allocate memory BIO")
	}
	defer C.BIO_free(bio)
	rsa := (*C.RSA)(C.EVP_PKEY_get1_RSA(key.key))
	if rsa == nil {
		return nil, errors.New("failed getting rsa key")
	}
	defer C.RSA_free(rsa)
	if int(C.i2d_RSA_PUBKEY_bio(bio, rsa)) != 1 {
		return nil, errors.New("failed dumping public key der")
	}
	return ioutil.ReadAll(asAnyBio(bio))
}

// LoadPrivateKeyFromPEM loads a private key from a PEM-encoded block.
func LoadPrivateKeyFromPEM(pem_block []byte) (PrivateKey, error) {
	if len(pem_block) == 0 {
		return nil, errors.New("empty pem block")
	}
	bio := C.BIO_new_mem_buf(unsafe.Pointer(&pem_block[0]),
		C.int(len(pem_block)))
	if bio == nil {
		return nil, errors.New("failed creating bio")
	}
	defer C.BIO_free(bio)

	rsakey := C.PEM_read_bio_RSAPrivateKey(bio, nil, nil, nil)
	if rsakey == nil {
		return nil, errors.New("failed reading rsa key")
	}
	defer C.RSA_free(rsakey)

	// convert to PKEY
	key := C.EVP_PKEY_new()
	if key == nil {
		return nil, errors.New("failed converting to evp_pkey")
	}
	if C.EVP_PKEY_set1_RSA(key, (*C.struct_rsa_st)(rsakey)) != 1 {
		C.EVP_PKEY_free(key)
		return nil, errors.New("failed converting to evp_pkey")
	}

	p := &pKey{key: key}
	runtime.SetFinalizer(p, func(p *pKey) {
		C.EVP_PKEY_free(p.key)
	})
	return p, nil
}

// LoadPublicKeyFromPEM loads a public key from a PEM-encoded block.
func LoadPublicKeyFromPEM(pem_block []byte) (PublicKey, error) {
	if len(pem_block) == 0 {
		return nil, errors.New("empty pem block")
	}
	bio := C.BIO_new_mem_buf(unsafe.Pointer(&pem_block[0]),
		C.int(len(pem_block)))
	if bio == nil {
		return nil, errors.New("failed creating bio")
	}
	defer C.BIO_free(bio)

	rsakey := C.PEM_read_bio_RSA_PUBKEY(bio, nil, nil, nil)
	if rsakey == nil {
		return nil, errors.New("failed reading rsa key")
	}
	defer C.RSA_free(rsakey)

	// convert to PKEY
	key := C.EVP_PKEY_new()
	if key == nil {
		return nil, errors.New("failed converting to evp_pkey")
	}
	if C.EVP_PKEY_set1_RSA(key, (*C.struct_rsa_st)(rsakey)) != 1 {
		C.EVP_PKEY_free(key)
		return nil, errors.New("failed converting to evp_pkey")
	}

	p := &pKey{key: key}
	runtime.SetFinalizer(p, func(p *pKey) {
		C.EVP_PKEY_free(p.key)
	})
	return p, nil
}

// LoadPublicKeyFromDER loads a public key from a DER-encoded block.
func LoadPublicKeyFromDER(der_block []byte) (PublicKey, error) {
	if len(der_block) == 0 {
		return nil, errors.New("empty der block")
	}
	bio := C.BIO_new_mem_buf(unsafe.Pointer(&der_block[0]),
		C.int(len(der_block)))
	if bio == nil {
		return nil, errors.New("failed creating bio")
	}
	defer C.BIO_free(bio)

	rsakey := C.d2i_RSA_PUBKEY_bio(bio, nil)
	if rsakey == nil {
		return nil, errors.New("failed reading rsa key")
	}
	defer C.RSA_free(rsakey)

	// convert to PKEY
	key := C.EVP_PKEY_new()
	if key == nil {
		return nil, errors.New("failed converting to evp_pkey")
	}
	if C.EVP_PKEY_set1_RSA(key, (*C.struct_rsa_st)(rsakey)) != 1 {
		C.EVP_PKEY_free(key)
		return nil, errors.New("failed converting to evp_pkey")
	}

	p := &pKey{key: key}
	runtime.SetFinalizer(p, func(p *pKey) {
		C.EVP_PKEY_free(p.key)
	})
	return p, nil
}

type Certificate struct {
	x   *C.X509
	ref interface{}
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
