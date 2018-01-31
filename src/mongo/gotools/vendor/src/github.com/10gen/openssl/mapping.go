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

import (
	"sync"
	"unsafe"
)

// #include <stdlib.h>
import "C"

type mapping struct {
	lock   sync.Mutex
	values map[token]unsafe.Pointer
}

func newMapping() *mapping {
	return &mapping{
		values: make(map[token]unsafe.Pointer),
	}
}

type token unsafe.Pointer

func (m *mapping) Add(x unsafe.Pointer) token {
	res := token(C.malloc(1))

	m.lock.Lock()
	m.values[res] = x
	m.lock.Unlock()

	return res
}

func (m *mapping) Get(x token) unsafe.Pointer {
	m.lock.Lock()
	res := m.values[x]
	m.lock.Unlock()

	return res
}

func (m *mapping) Del(x token) {
	m.lock.Lock()
	delete(m.values, x)
	m.lock.Unlock()

	C.free(unsafe.Pointer(x))
}
