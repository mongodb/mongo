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
#include "openssl/engine.h"
*/
import "C"

import (
	"fmt"
	"runtime"
	"unsafe"
)

type Engine struct {
	e *C.ENGINE
}

func EngineById(name string) (*Engine, error) {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	e := &Engine{
		e: C.ENGINE_by_id(cname),
	}
	if e.e == nil {
		return nil, fmt.Errorf("engine %s missing", name)
	}
	if C.ENGINE_init(e.e) == 0 {
		C.ENGINE_free(e.e)
		return nil, fmt.Errorf("engine %s not initialized", name)
	}
	runtime.SetFinalizer(e, func(e *Engine) {
		C.ENGINE_finish(e.e)
		C.ENGINE_free(e.e)
	})
	return e, nil
}
