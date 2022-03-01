/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

#ifndef wasm_table_h
#define wasm_table_h

#include "gc/Policy.h"
#include "wasm/WasmCode.h"

namespace js {
namespace wasm {

// A Table is an indexable array of opaque values. Tables are first-class
// stateful objects exposed to WebAssembly. asm.js also uses Tables to represent
// its homogeneous function-pointer tables.
//
// A table of FuncRef holds FunctionTableElems, which are (code*,tls*) pairs,
// where the tls must be traced.
//
// A table of AnyRef holds JSObject pointers, which must be traced.

// TODO/AnyRef-boxing: With boxed immediates and strings, JSObject* is no longer
// the most appropriate representation for Cell::anyref.
STATIC_ASSERT_ANYREF_IS_JSOBJECT;

using TableAnyRefVector = GCVector<HeapPtr<JSObject*>, 0, SystemAllocPolicy>;

class Table : public ShareableBase<Table> {
  using InstanceSet =
      JS::WeakCache<GCHashSet<WeakHeapPtrWasmInstanceObject,
                              MovableCellHasher<WeakHeapPtrWasmInstanceObject>,
                              SystemAllocPolicy>>;
  using UniqueFuncRefArray = UniquePtr<FunctionTableElem[], JS::FreePolicy>;

  WeakHeapPtrWasmTableObject maybeObject_;
  InstanceSet observers_;
  UniqueFuncRefArray functions_;  // either functions_ has data
  TableAnyRefVector objects_;     //   or objects_, but not both
  const RefType elemType_;
  const bool isAsmJS_;
  uint32_t length_;
  const Maybe<uint32_t> maximum_;

  template <class>
  friend struct js::MallocProvider;
  Table(JSContext* cx, const TableDesc& desc, HandleWasmTableObject maybeObject,
        UniqueFuncRefArray functions);
  Table(JSContext* cx, const TableDesc& desc, HandleWasmTableObject maybeObject,
        TableAnyRefVector&& objects);

  void tracePrivate(JSTracer* trc);
  friend class js::WasmTableObject;

 public:
  static RefPtr<Table> create(JSContext* cx, const TableDesc& desc,
                              HandleWasmTableObject maybeObject);
  void trace(JSTracer* trc);

  RefType elemType() const { return elemType_; }
  TableRepr repr() const { return elemType_.tableRepr(); }

  bool isAsmJS() const {
    MOZ_ASSERT(elemType_.isFunc());
    return isAsmJS_;
  }
  bool isFunction() const { return elemType().isFunc(); }
  uint32_t length() const { return length_; }
  Maybe<uint32_t> maximum() const { return maximum_; }

  // Only for function values.  Raw pointer to the table.
  uint8_t* functionBase() const;

  // set/get/fillFuncRef is allowed only on table-of-funcref.
  // get/fillAnyRef is allowed only on table-of-anyref.
  // setNull is allowed on either.

  const FunctionTableElem& getFuncRef(uint32_t index) const;
  bool getFuncRef(JSContext* cx, uint32_t index,
                  MutableHandleFunction fun) const;
  void setFuncRef(uint32_t index, void* code, const Instance* instance);
  void fillFuncRef(uint32_t index, uint32_t fillCount, FuncRef ref,
                   JSContext* cx);

  AnyRef getAnyRef(uint32_t index) const;
  void fillAnyRef(uint32_t index, uint32_t fillCount, AnyRef ref);

  void setNull(uint32_t index);

  // Copy entry from |srcTable| at |srcIndex| to this table at |dstIndex|.  Used
  // by table.copy.  May OOM if it needs to box up a function during an upcast.
  bool copy(const Table& srcTable, uint32_t dstIndex, uint32_t srcIndex);

  // grow() returns (uint32_t)-1 if it could not grow.
  uint32_t grow(uint32_t delta);
  bool movingGrowable() const;
  bool addMovingGrowObserver(JSContext* cx, WasmInstanceObject* instance);

  // about:memory reporting:

  size_t sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const;

  size_t gcMallocBytes() const;
};

using SharedTable = RefPtr<Table>;
using SharedTableVector = Vector<SharedTable, 0, SystemAllocPolicy>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_table_h
