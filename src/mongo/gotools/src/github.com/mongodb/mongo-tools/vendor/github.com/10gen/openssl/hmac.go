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
	"errors"
	"runtime"
	"unsafe"
)

type HMAC struct {
	ctx    *C.HMAC_CTX
	engine *Engine
	md     *C.EVP_MD
}

func NewHMAC(key []byte, digestAlgorithm EVP_MD) (*HMAC, error) {
	return NewHMACWithEngine(key, digestAlgorithm, nil)
}

func NewHMACWithEngine(key []byte, digestAlgorithm EVP_MD, e *Engine) (*HMAC, error) {
	var md *C.EVP_MD = getDigestFunction(digestAlgorithm)
	h := &HMAC{engine: e, md: md}
	h.ctx = C.X_HMAC_CTX_new()
	if h.ctx == nil {
		return nil, errors.New("unable to allocate HMAC_CTX")
	}

	var c_e *C.ENGINE
	if e != nil {
		c_e = e.e
	}
	if rc := C.X_HMAC_Init_ex(h.ctx,
		unsafe.Pointer(&key[0]),
		C.int(len(key)),
		md,
		c_e); rc != 1 {
		C.X_HMAC_CTX_free(h.ctx)
		return nil, errors.New("failed to initialize HMAC_CTX")
	}

	runtime.SetFinalizer(h, func(h *HMAC) { h.Close() })
	return h, nil
}

func (h *HMAC) Close() {
	C.X_HMAC_CTX_free(h.ctx)
}

func (h *HMAC) Write(data []byte) (n int, err error) {
	if len(data) == 0 {
		return 0, nil
	}
	if rc := C.X_HMAC_Update(h.ctx, (*C.uchar)(unsafe.Pointer(&data[0])),
		C.size_t(len(data))); rc != 1 {
		return 0, errors.New("failed to update HMAC")
	}
	return len(data), nil
}

func (h *HMAC) Reset() error {
	if 1 != C.X_HMAC_Init_ex(h.ctx, nil, 0, nil, nil) {
		return errors.New("failed to reset HMAC_CTX")
	}
	return nil
}

func (h *HMAC) Final() (result []byte, err error) {
	mdLength := C.X_EVP_MD_size(h.md)
	result = make([]byte, mdLength)
	if rc := C.X_HMAC_Final(h.ctx, (*C.uchar)(unsafe.Pointer(&result[0])),
		(*C.uint)(unsafe.Pointer(&mdLength))); rc != 1 {
		return nil, errors.New("failed to finalized HMAC")
	}
	return result, h.Reset()
}
