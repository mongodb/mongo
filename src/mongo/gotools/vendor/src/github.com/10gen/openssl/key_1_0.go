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

// +build !openssl_pre_1.0

package openssl

// #include "shim.h"
import "C"

import (
	"errors"
	"io/ioutil"
	"runtime"
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

	// KeyType returns an identifier for what kind of key is represented by this
	// object.
	KeyType() NID

	// BaseType returns an identifier for what kind of key is represented
	// by this object.
	// Keys that share same algorithm but use different legacy formats
	// will have the same BaseType.
	//
	// For example, a key with a `KeyType() == KeyTypeRSA` and a key with a
	// `KeyType() == KeyTypeRSA2` would both have `BaseType() == KeyTypeRSA`.
	BaseType() NID

	evpPKey() *C.EVP_PKEY
}

func (key *pKey) MarshalPKCS1PrivateKeyPEM() (pem_block []byte,
	err error) {
	bio := C.BIO_new(C.BIO_s_mem())
	if bio == nil {
		return nil, errors.New("failed to allocate memory BIO")
	}
	defer C.BIO_free(bio)

	// PEM_write_bio_PrivateKey_traditional will use the key-specific PKCS1
	// format if one is available for that key type, otherwise it will encode
	// to a PKCS8 key.
	if int(C.X_PEM_write_bio_PrivateKey_traditional(bio, key.key, nil, nil,
		C.int(0), nil, nil)) != 1 {
		return nil, errors.New("failed dumping private key")
	}

	return ioutil.ReadAll(asAnyBio(bio))
}

func (key *pKey) KeyType() NID {
	return NID(C.EVP_PKEY_id(key.key))
}

func (key *pKey) BaseType() NID {
	return NID(C.EVP_PKEY_base_id(key.key))
}

// GenerateECKey generates a new elliptic curve private key on the speicified
// curve.
func GenerateECKey(curve EllipticCurve) (PrivateKey, error) {

	// Create context for parameter generation
	paramCtx := C.EVP_PKEY_CTX_new_id(C.EVP_PKEY_EC, nil)
	if paramCtx == nil {
		return nil, errors.New("failed creating EC parameter generation context")
	}
	defer C.EVP_PKEY_CTX_free(paramCtx)

	// Intialize the parameter generation
	if int(C.EVP_PKEY_paramgen_init(paramCtx)) != 1 {
		return nil, errors.New("failed initializing EC parameter generation context")
	}

	// Set curve in EC parameter generation context
	if int(C.X_EVP_PKEY_CTX_set_ec_paramgen_curve_nid(paramCtx, C.int(curve))) != 1 {
		return nil, errors.New("failed setting curve in EC parameter generation context")
	}

	// Create parameter object
	var params *C.EVP_PKEY
	if int(C.EVP_PKEY_paramgen(paramCtx, &params)) != 1 {
		return nil, errors.New("failed creating EC key generation parameters")
	}
	defer C.EVP_PKEY_free(params)

	// Create context for the key generation
	keyCtx := C.EVP_PKEY_CTX_new(params, nil)
	if keyCtx == nil {
		return nil, errors.New("failed creating EC key generation context")
	}
	defer C.EVP_PKEY_CTX_free(keyCtx)

	// Generate the key
	var privKey *C.EVP_PKEY
	if int(C.EVP_PKEY_keygen_init(keyCtx)) != 1 {
		return nil, errors.New("failed initializing EC key generation context")
	}
	if int(C.EVP_PKEY_keygen(keyCtx, &privKey)) != 1 {
		return nil, errors.New("failed generating EC private key")
	}

	p := &pKey{key: privKey}
	runtime.SetFinalizer(p, func(p *pKey) {
		C.X_EVP_PKEY_free(p.key)
	})
	return p, nil
}
