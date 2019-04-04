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

// #include "shim.h"
import "C"

import (
	"fmt"
	"unsafe"
)

// Digest represents and openssl message digest.
type Digest struct {
	ptr *C.EVP_MD
}

// GetDigestByName returns the Digest with the name or nil and an error if the
// digest was not found.
func GetDigestByName(name string) (*Digest, error) {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	p := C.X_EVP_get_digestbyname(cname)
	if p == nil {
		return nil, fmt.Errorf("Digest %v not found", name)
	}
	// we can consider digests to use static mem; don't need to free
	return &Digest{ptr: p}, nil
}

// GetDigestByName returns the Digest with the NID or nil and an error if the
// digest was not found.
func GetDigestByNid(nid NID) (*Digest, error) {
	sn, err := Nid2ShortName(nid)
	if err != nil {
		return nil, err
	}
	return GetDigestByName(sn)
}
