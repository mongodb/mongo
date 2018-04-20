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

// +build openssl_pre_1.0

package openssl

// #include "shim.h"
import "C"
import (
	"errors"
	"io/ioutil"
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
