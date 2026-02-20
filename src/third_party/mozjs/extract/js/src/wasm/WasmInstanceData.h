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

#include "gc/Pretenuring.h"
#include "js/Utility.h"
#include "vm/JSFunction.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmMemory.h"
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
        allocKind(gc::AllocKind::LIMIT),
        unused(0) {}

  // The canonicalized pointer to this type definition. This is kept alive by
  // the type context associated with the instance.
  const wasm::TypeDef* typeDef;

  // The supertype vector for this type definition.  This is also kept alive
  // by the type context associated with the instance.
  //
  const wasm::SuperTypeVector* superTypeVector;

  // The next four fields are only meaningful for, and used by, structs and
  // arrays.
  GCPtr<Shape*> shape;
  const JSClass* clasp;
  // Only valid for structs.
  gc::AllocKind allocKind;

  // This union is only meaningful for structs and arrays, and should
  // otherwise be set to zero:
  //
  // * if `typeDef` refers to a struct type, then it caches the value of
  //   `typeDef->structType().size_` (a size in bytes)
  //
  // * if `typeDef` refers to an array type, then it caches the value of
  //   `typeDef->arrayType().elementType_.size()` (also a size in bytes)
  //
  // This is so that allocators of structs and arrays don't need to chase from
  // this TypeDefInstanceData through `typeDef` to find the value.
  union {
    uint32_t structTypeSize;
    uint32_t arrayElemSize;
    uint32_t unused;
  };

  static constexpr size_t offsetOfShape() {
    return offsetof(TypeDefInstanceData, shape);
  }
  static constexpr size_t offsetOfSuperTypeVector() {
    return offsetof(TypeDefInstanceData, superTypeVector);
  }
  static constexpr size_t offsetOfArrayElemSize() {
    return offsetof(TypeDefInstanceData, arrayElemSize);
  }
};

// FuncDefInstanceData maintains the per-instance hotness state for a locally
// defined wasm function.  This is a signed-int32 value that counts downwards
// from an initially non-negative value.  At the point where the value
// transitions below zero (not *to* zero), we deem the owning function to
// have become hot.  Transitions from one negative value to any other (even
// more) negative value are meaningless and should not happen.
struct FuncDefInstanceData {
  int32_t hotnessCounter;
};

// FuncExportInstanceData maintains the exported function JS wrapper for an
// exported function.
struct FuncExportInstanceData {
  GCPtr<JSFunction*> func;
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

struct MemoryInstanceData {
  // Pointer to the memory object.
  GCPtr<WasmMemoryObject*> memory;

  // Pointer to the base of the memory.
  uint8_t* base;

  // Bounds check limit in bytes (or zero if there is no memory).  This is
  // 64-bits on 64-bit systems so as to allow for heap lengths up to and beyond
  // 4GB, and 32-bits on 32-bit systems, where heaps are limited to 2GB.
  //
  // See "Linear memory addresses and bounds checking" in WasmMemory.cpp.
  uintptr_t boundsCheckLimit;

  // Whether this memory is shared or not.
  bool isShared;
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

// A collection of metrics for a `call_ref` instruction. This is tracked by
// baseline when we are using lazy tiering to perform speculative inlining.
//
// See MacroAssembler::updateCallRefMetrics for how this is written into.
//
// Because it contains thread-local data and is written into without
// synchronization, we cannot access this directly from our function compilers
// and so we use CallRefHints for that (see WasmModuleTypes.h).
struct CallRefMetrics {
  // We track up to NUM_SLOTS targets with associated count, plus a count for
  // "all other" targets, including cross-instance calls.  This facilitates
  // knowing the total number of calls made by the instruction, which is
  // required in order to know whether the hottest tracked function is more
  // than some percentage (eg, 50%) of all calls.
  //
  // In order to keep search costs low (in the baseline code), we rely on the
  // fact that most call sites have distributions which are heavily skewed
  // towards one target.  This struct is updated by the code generated in
  // GenerateUpdateCallRefMetricsStub.  That tries to make
  // `targets[0]`/`counts[0]` be the hottest target, so that for most calls,
  // the monitoring code will only check `targets[0]` for a match.
  //
  // For NUM_SLOTS <= 2, GenerateUpdateCallRefMetricsStub's incremental-sort
  // heuristic maintains the `counts` array in strictly non-decreasing order.
  // For NUM_SLOTS > 2, in the worst case we will have counts[N+1] at most
  // (NUM_SLOTS - 2) larger than counts[N].  In practice the incremental
  // sorting heuristic is very effective and so counts are in decreasing order,
  // as we desire.
  //
  // Once NUM_SLOTS targets are being tracked, all new targets will be lumped
  // together in the `countOther` bucket.  This can lead to the unfortunate
  // case of having NUM_SLOTS different cold targets show up first, after which
  // follows a different target that is hot, but cannot be inlined because it
  // goes in the `countOther` bucket, so its identity is unknown.  This is
  // unlikely but could happen.  The only known fix is to increase NUM_SLOTS.
  //
  // The `targets` values may be nullptr only to indicate that the slot is not
  // in use.  No legitimate target can be nullptr.  Given that the state is
  // updated by generated code and that code isn't entirely simple, we place
  // emphasis on checking invariants carefully.
  //
  // Stores of funcrefs in `targets[]`: These CallRefMetrics structs logically
  // belong to the Instance data, and do not require any GC barriers for two
  // reasons:
  //
  // 1. The pre-write barrier protects against an unmarked object being stored
  //    into a marked object during an incremental GC.  However this funcref is
  //    from the Instance we're storing it into (see above) and so if the
  //    instance has already been traced, this function will already have been
  //    traced (all exported functions are kept alive by an instance cache).
  //
  // 2. The post-write barrier tracks edges from tenured objects to nursery
  //    objects.  However wasm exported functions are not nursery allocated and
  //    so no new edge can be created.
  //
  // Overflows in `counts[]` and `countOther`: increments of these values are
  // not checked for overflow and so could wrap around from 2^32-1 to zero.
  // We ignore but tolerate this, because:
  //
  // 1. This is extremely unlikely to happen in practice, since the function
  //    containing the call site is almost certain to get tiered up long before
  //    any of these counters gets anywhere near the limit.
  //
  // 2. Performing saturating increments is possible, but has a minimum extra
  //    cost of two instructions, and given (1.) it is pointless.
  //
  // This does however require that interpretation of the `counts[]` and
  // `countOther` values needs to be aware that zeroes could mean 2^32 or any
  // multiple of it.  Hence a zero in `counts[]` does not necessarily mean that
  // the corresponding `target[]` was never called, nor is it the case that a
  // `countsOther` of zero means no "other" targets were observed.

  static constexpr size_t NUM_SLOTS = 3;
  static_assert(NUM_SLOTS >= 1);  // 1 slot + others is the minimal config

  // An array of pairs of (target, count) ..
  GCPtr<JSFunction*> targets[NUM_SLOTS];
  uint32_t counts[NUM_SLOTS];
  // .. and a count for all other targets.
  uint32_t countOther;

  // Generated code assumes this
  static_assert(sizeof(GCPtr<JSFunction*>) == sizeof(void*));
  static_assert(sizeof(uint32_t) == 4);

  CallRefMetrics() {
    for (size_t i = 0; i < NUM_SLOTS; i++) {
      targets[i] = nullptr;
      counts[i] = 0;
    }
    countOther = 0;
    MOZ_ASSERT(checkInvariants());
  }

  [[nodiscard]] bool checkInvariants() const {
    // If targets[N] is null, then this slot is not in use and so counts[N]
    // must be zero.  Per comments above about overflow, the implication in the
    // other direction does not hold.
    size_t i;
    for (i = 0; i < NUM_SLOTS; i++) {
      if (targets[i] == nullptr && counts[i] != 0) {
        return false;
      }
    }
    // The targets/counts slots must be filled in in sequence.
    for (i = 0; i < NUM_SLOTS; i++) {
      if (targets[i] == nullptr) {
        break;
      }
    }
    size_t numUsed = i;
    for (/*keepgoing*/; i < NUM_SLOTS; i++) {
      if (targets[i] != nullptr) {
        return false;
      }
    }
    // For the slots in use, the target values must be different
    for (i = 0; i < numUsed; i++) {
      for (size_t j = i + 1; j < numUsed; j++) {
        if (targets[j] == targets[i]) {
          return false;
        }
      }
    }
    // Note we don't say anything about `countOther`.  This gets incremented in
    // the cases when we (1) see a new target when all slots are already in
    // use, or (2) have a cross-instance call.  The effect of (2) is that
    // `countOther` can be non-zero regardless of how many slots are in use.
    return true;
  }

  static size_t offsetOfTarget(size_t n) {
    MOZ_ASSERT(n < NUM_SLOTS);
    return offsetof(CallRefMetrics, targets) + n * sizeof(GCPtr<JSFunction*>);
  }
  static size_t offsetOfCount(size_t n) {
    MOZ_ASSERT(n < NUM_SLOTS);
    return offsetof(CallRefMetrics, counts) + n * sizeof(uint32_t);
  }
  static size_t offsetOfCountOther() {
    return offsetof(CallRefMetrics, countOther);
  }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_instance_data_h
