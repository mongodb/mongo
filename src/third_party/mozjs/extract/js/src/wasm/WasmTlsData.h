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

#ifndef wasm_tls_data_h
#define wasm_tls_data_h

#include "mozilla/Atomics.h"

#include <stdint.h>

#include "NamespaceImports.h"

#include "js/Utility.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace wasm {

using mozilla::Atomic;

// TLS data for a single module instance.
//
// Every WebAssembly function expects to be passed a hidden TLS pointer argument
// in WasmTlsReg. The TLS pointer argument points to a TlsData struct.
// Compiled functions expect that the TLS pointer does not change for the
// lifetime of the thread.
//
// There is a TlsData per module instance per thread, so inter-module calls need
// to pass the TLS pointer appropriate for the callee module.
//
// After the TlsData struct follows the module's declared TLS variables.

struct TlsData {
  // Pointer to the base of the default memory (or null if there is none).
  uint8_t* memoryBase;

  // Bounds check limit in bytes (or zero if there is no memory).  This is
  // 64-bits on 64-bit systems so as to allow for heap lengths up to and beyond
  // 4GB, and 32-bits on 32-bit systems, where heaps are limited to 2GB.
  //
  // On 64-bit systems, the upper bits of this value will be zero and 32-bit
  // bounds checks can be used under the following circumstances:
  //
  // - The heap is for asm.js; asm.js heaps are limited to 2GB
  // - The max size of the heap is encoded in the module and is less than 4GB
  // - Cranelift is present in the system; Cranelift-compatible heaps are
  //   limited to 4GB-128K
  //
  // All our jits require little-endian byte order, so the address of the 32-bit
  // heap limit is the same as the address of the 64-bit heap limit: the address
  // of this member.
  uintptr_t boundsCheckLimit;

  // Pointer to the Instance that contains this TLS data.
  Instance* instance;

  // Equal to instance->realm_.
  JS::Realm* realm;

  // The containing JSContext.
  JSContext* cx;

  // The class_ of WasmValueBox, this is a per-process value.
  const JSClass* valueBoxClass;

  // Usually equal to cx->stackLimitForJitCode(JS::StackForUntrustedScript),
  // but can be racily set to trigger immediate trap as an opportunity to
  // CheckForInterrupt without an additional branch.
  Atomic<uintptr_t, mozilla::Relaxed> stackLimit;

  // Set to 1 when wasm should call CheckForInterrupt.
  Atomic<uint32_t, mozilla::Relaxed> interrupt;

  uint8_t* addressOfNeedsIncrementalBarrier;

  // Methods to set, test and clear the above two fields. Both interrupt
  // fields are Relaxed and so no consistency/ordering can be assumed.
  void setInterrupt();
  bool isInterrupted() const;
  void resetInterrupt(JSContext* cx);

  // Pointer that should be freed (due to padding before the TlsData).
  void* allocatedBase;

  // When compiling with tiering, the jumpTable has one entry for each
  // baseline-compiled function.
  void** jumpTable;

  // The globalArea must be the last field.  Globals for the module start here
  // and are inline in this structure.  16-byte alignment is required for SIMD
  // data.
  MOZ_ALIGNED_DECL(16, char globalArea);
};

static const size_t TlsDataAlign = 16;  // = Simd128DataSize
static_assert(offsetof(TlsData, globalArea) % TlsDataAlign == 0, "aligned");

struct TlsDataDeleter {
  void operator()(TlsData* tlsData) { js_free(tlsData->allocatedBase); }
};

using UniqueTlsData = UniquePtr<TlsData, TlsDataDeleter>;

extern UniqueTlsData CreateTlsData(uint32_t globalDataLength);

// ExportArg holds the unboxed operands to the wasm entry trampoline which can
// be called through an ExportFuncPtr.

struct ExportArg {
  uint64_t lo;
  uint64_t hi;
};

using ExportFuncPtr = int32_t (*)(ExportArg*, TlsData*);

// FuncImportTls describes the region of wasm global memory allocated in the
// instance's thread-local storage for a function import. This is accessed
// directly from JIT code and mutated by Instance as exits become optimized and
// deoptimized.

struct FuncImportTls {
  // The code to call at an import site: a wasm callee, a thunk into C++, or a
  // thunk into JIT code.
  void* code;

  // The callee's TlsData pointer, which must be loaded to WasmTlsReg (along
  // with any pinned registers) before calling 'code'.
  TlsData* tls;

  // The callee function's realm.
  JS::Realm* realm;

  // A GC pointer which keeps the callee alive and is used to recover import
  // values for lazy table initialization.
  GCPtrFunction fun;
  static_assert(sizeof(GCPtrFunction) == sizeof(void*), "for JIT access");
};

// TableTls describes the region of wasm global memory allocated in the
// instance's thread-local storage which is accessed directly from JIT code
// to bounds-check and index the table.

struct TableTls {
  // Length of the table in number of elements (not bytes).
  uint32_t length;

  // Pointer to the array of elements (which can have various representations).
  // For tables of anyref this is null.
  void* functionBase;
};

// Table element for TableRepr::Func which carries both the code pointer and
// a tls pointer (and thus anything reachable through the tls, including the
// instance).

struct FunctionTableElem {
  // The code to call when calling this element. The table ABI is the system
  // ABI with the additional ABI requirements that:
  //  - WasmTlsReg and any pinned registers have been loaded appropriately
  //  - if this is a heterogeneous table that requires a signature check,
  //    WasmTableCallSigReg holds the signature id.
  void* code;

  // The pointer to the callee's instance's TlsData. This must be loaded into
  // WasmTlsReg before calling 'code'.
  TlsData* tls;
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_tls_data_h
