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
// A table of FuncRef holds FunctionTableElems, which are (code*,instance*)
// pairs, where the instance must be traced.
//
// A table of AnyRef holds pointers, which must be traced.

using TableAnyRefVector = GCVector<HeapPtr<AnyRef>, 0, SystemAllocPolicy>;

class Table : public ShareableBase<Table> {
  using InstanceSet = JS::WeakCache<GCHashSet<
      WeakHeapPtr<WasmInstanceObject*>,
      StableCellHasher<WeakHeapPtr<WasmInstanceObject*>>, SystemAllocPolicy>>;
  using FuncRefVector = Vector<FunctionTableElem, 0, SystemAllocPolicy>;

  WeakHeapPtr<WasmTableObject*> maybeObject_;
  InstanceSet observers_;
  FuncRefVector functions_;    // either functions_ has data
  TableAnyRefVector objects_;  // or objects_, but not both
  const AddressType addressType_;
  const RefType elemType_;
  const bool isAsmJS_;
  uint32_t length_;
  const mozilla::Maybe<uint64_t> maximum_;

  template <class>
  friend struct js::MallocProvider;
  Table(JSContext* cx, const TableDesc& desc,
        Handle<WasmTableObject*> maybeObject, FuncRefVector&& functions);
  Table(JSContext* cx, const TableDesc& desc,
        Handle<WasmTableObject*> maybeObject, TableAnyRefVector&& objects);

  void tracePrivate(JSTracer* trc);
  friend class js::WasmTableObject;

 public:
  static RefPtr<Table> create(JSContext* cx, const TableDesc& desc,
                              Handle<WasmTableObject*> maybeObject);
  ~Table();
  void trace(JSTracer* trc);

  AddressType addressType() const { return addressType_; }
  RefType elemType() const { return elemType_; }
  TableRepr repr() const { return elemType_.tableRepr(); }

  bool isAsmJS() const {
    MOZ_ASSERT(elemType_.isFuncHierarchy());
    return isAsmJS_;
  }

  bool isFunction() const { return elemType().isFuncHierarchy(); }
  uint32_t length() const { return length_; }
  mozilla::Maybe<uint64_t> maximum() const { return maximum_; }

  // Raw pointer to the table for use in TableInstanceData.
  uint8_t* instanceElements() const;

  // set/get/fillFuncRef is allowed only on table-of-funcref.
  // get/fillAnyRef is allowed only on table-of-anyref.
  // setNull is allowed on either.

  const FunctionTableElem& getFuncRef(uint32_t address) const;
  [[nodiscard]] bool getFuncRef(JSContext* cx, uint32_t address,
                                MutableHandleFunction fun) const;
  void setFuncRef(uint32_t address, JSFunction* func);
  void setFuncRef(uint32_t address, void* code, Instance* instance);
  void fillFuncRef(uint32_t address, uint32_t fillCount, FuncRef ref,
                   JSContext* cx);

  AnyRef getAnyRef(uint32_t address) const;
  void setAnyRef(uint32_t address, AnyRef ref);
  void fillAnyRef(uint32_t address, uint32_t fillCount, AnyRef ref);

  // Sets ref automatically using the correct setter depending on the ref and
  // table type (setNull, setFuncRef, or setAnyRef)
  void setRef(uint32_t address, AnyRef ref);

  // Get the element at address and convert it to a JS value.
  [[nodiscard]] bool getValue(JSContext* cx, uint32_t address,
                              MutableHandleValue result) const;

  void setNull(uint32_t address);

  // Copy entry from |srcTable| at |srcIndex| to this table at |dstIndex|.  Used
  // by table.copy.  May OOM if it needs to box up a function during an upcast.
  [[nodiscard]] bool copy(JSContext* cx, const Table& srcTable,
                          uint32_t dstIndex, uint32_t srcIndex);

  // grow() returns (uint32_t)-1 if it could not grow.
  [[nodiscard]] uint32_t grow(uint32_t delta);
  [[nodiscard]] bool movingGrowable() const;
  [[nodiscard]] bool addMovingGrowObserver(JSContext* cx,
                                           WasmInstanceObject* instance);

  void fillUninitialized(uint32_t address, uint32_t fillCount, HandleAnyRef ref,
                         JSContext* cx);
#ifdef DEBUG
  void assertRangeNull(uint32_t address, uint32_t length) const;
  void assertRangeNotNull(uint32_t address, uint32_t length) const;
#endif  // DEBUG

  // about:memory reporting:

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  size_t gcMallocBytes() const;
};

using SharedTable = RefPtr<Table>;
using SharedTableVector = Vector<SharedTable, 0, SystemAllocPolicy>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_table_h
