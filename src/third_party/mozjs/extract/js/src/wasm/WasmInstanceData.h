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

#ifndef wasm_instance_data_h
#define wasm_instance_data_h

#include <stdint.h>

#include "NamespaceImports.h"

#include "gc/Allocator.h"
#include "gc/Pretenuring.h"
#include "js/Utility.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace wasm {

// ExportArg holds the unboxed operands to the wasm entry trampoline which can
// be called through an ExportFuncPtr.

struct ExportArg {
  uint64_t lo;
  uint64_t hi;
};

using ExportFuncPtr = int32_t (*)(ExportArg*, Instance*);

// TypeDefInstanceData describes the runtime information associated with a
// module's type definition. This is accessed directly from JIT code and the
// Instance.

struct TypeDefInstanceData {
  TypeDefInstanceData()
      : typeDef(nullptr),
        superTypeVector(nullptr),
        shape(nullptr),
        clasp(nullptr),
        allocKind(gc::AllocKind::LIMIT) {}

  // The canonicalized pointer to this type definition. This is kept alive by
  // the type context associated with the instance.
  const wasm::TypeDef* typeDef;

  // The supertype vector for this type definition.  This is also kept alive
  // by the type context associated with the instance.
  //
  const wasm::SuperTypeVector* superTypeVector;

  // The remaining fields are only meaningful for, and used by, structs and
  // arrays.
  GCPtr<Shape*> shape;
  const JSClass* clasp;
  // The allocation site for GC types. This is used for pre-tenuring.
  gc::AllocSite allocSite;
  gc::AllocKind allocKind;
};

// FuncImportInstanceData describes the region of wasm global memory allocated
// in the instance's thread-local storage for a function import. This is
// accessed directly from JIT code and mutated by Instance as exits become
// optimized and deoptimized.

struct FuncImportInstanceData {
  // The code to call at an import site: a wasm callee, a thunk into C++, or a
  // thunk into JIT code.
  void* code;

  // The callee's Instance pointer, which must be loaded to InstanceReg
  // (along with any pinned registers) before calling 'code'.
  Instance* instance;

  // The callee function's realm.
  JS::Realm* realm;

  // A GC pointer which keeps the callee alive and is used to recover import
  // values for lazy table initialization.
  GCPtr<JSObject*> callable;
  static_assert(sizeof(GCPtr<JSObject*>) == sizeof(void*), "for JIT access");
};

// TableInstanceData describes the region of wasm global memory allocated in the
// instance's thread-local storage which is accessed directly from JIT code
// to bounds-check and index the table.

struct TableInstanceData {
  // Length of the table in number of elements (not bytes).
  uint32_t length;

  // Pointer to the array of elements (which can have various representations).
  // For tables of anyref this is null.
  // For tables of functions, this is a pointer to the array of code pointers.
  void* elements;
};

// TagInstanceData describes the instance state associated with a tag.

struct TagInstanceData {
  GCPtr<WasmTagObject*> object;
};

// Table element for TableRepr::Func which carries both the code pointer and
// a instance pointer (and thus anything reachable through the instance).

struct FunctionTableElem {
  // The code to call when calling this element. The table ABI is the system
  // ABI with the additional ABI requirements that:
  //  - InstanceReg and any pinned registers have been loaded appropriately
  //  - if this is a heterogeneous table that requires a signature check,
  //    WasmTableCallSigReg holds the signature id.
  void* code;

  // The pointer to the callee's instance's Instance. This must be loaded into
  // InstanceReg before calling 'code'.
  Instance* instance;
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_instance_data_h
