/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmTlsData.h"

#include "js/HeapAPI.h"
#include "util/Memory.h"
#include "vm/JSContext.h"

using namespace js;
using namespace js::wasm;

UniqueTlsData wasm::CreateTlsData(uint32_t globalDataLength) {
  void* allocatedBase = js_calloc(TlsDataAlign + offsetof(TlsData, globalArea) +
                                  globalDataLength);
  if (!allocatedBase) {
    return nullptr;
  }

  auto* tlsData = reinterpret_cast<TlsData*>(
      AlignBytes(uintptr_t(allocatedBase), TlsDataAlign));
  tlsData->allocatedBase = allocatedBase;

  return UniqueTlsData(tlsData);
}

void TlsData::setInterrupt() {
  interrupt = true;
  stackLimit = UINTPTR_MAX;
}

bool TlsData::isInterrupted() const {
  return interrupt || stackLimit == UINTPTR_MAX;
}

void TlsData::resetInterrupt(JSContext* cx) {
  interrupt = false;
  stackLimit = cx->stackLimitForJitCode(JS::StackForUntrustedScript);
}
