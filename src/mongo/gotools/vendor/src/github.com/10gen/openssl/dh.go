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
	"unsafe"
)

// DeriveSharedSecret derives a shared secret using a private key and a peer's
// public key.
// The specific algorithm that is used depends on the types of the
// keys, but it is most commonly a variant of Diffie-Hellman.
func DeriveSharedSecret(private PrivateKey, public PublicKey) ([]byte, error) {
	// Create context for the shared secret derivation
	dhCtx := C.EVP_PKEY_CTX_new(private.evpPKey(), nil)
	if dhCtx == nil {
		return nil, errors.New("failed creating shared secret derivation context")
	}
	defer C.EVP_PKEY_CTX_free(dhCtx)

	// Initialize the context
	if int(C.EVP_PKEY_derive_init(dhCtx)) != 1 {
		return nil, errors.New("failed initializing shared secret derivation context")
	}

	// Provide the peer's public key
	if int(C.EVP_PKEY_derive_set_peer(dhCtx, public.evpPKey())) != 1 {
		return nil, errors.New("failed adding peer public key to context")
	}

	// Determine how large of a buffer we need for the shared secret
	var buffLen C.size_t
	if int(C.EVP_PKEY_derive(dhCtx, nil, &buffLen)) != 1 {
		return nil, errors.New("failed determining shared secret length")
	}

	// Allocate a buffer
	buffer := C.X_OPENSSL_malloc(buffLen)
	if buffer == nil {
		return nil, errors.New("failed allocating buffer for shared secret")
	}
	defer C.X_OPENSSL_free(buffer)

	// Derive the shared secret
	if int(C.EVP_PKEY_derive(dhCtx, (*C.uchar)(buffer), &buffLen)) != 1 {
		return nil, errors.New("failed deriving the shared secret")
	}

	secret := C.GoBytes(unsafe.Pointer(buffer), C.int(buffLen))
	return secret, nil
}
