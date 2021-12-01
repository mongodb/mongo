/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

class Table : public ShareableBase<Table>
{
    using InstanceSet = JS::WeakCache<GCHashSet<ReadBarrieredWasmInstanceObject,
                                                MovableCellHasher<ReadBarrieredWasmInstanceObject>,
                                                SystemAllocPolicy>>;
    using UniqueByteArray = UniquePtr<uint8_t[], JS::FreePolicy>;

    ReadBarrieredWasmTableObject maybeObject_;
    InstanceSet                  observers_;
    UniqueByteArray              array_;
    const TableKind              kind_;
    uint32_t                     length_;
    const Maybe<uint32_t>        maximum_;
    const bool                   external_;

    template <class> friend struct js::MallocProvider;
    Table(JSContext* cx, const TableDesc& td, HandleWasmTableObject maybeObject,
          UniqueByteArray array);

    void tracePrivate(JSTracer* trc);
    friend class js::WasmTableObject;

  public:
    static RefPtr<Table> create(JSContext* cx, const TableDesc& desc,
                                HandleWasmTableObject maybeObject);
    void trace(JSTracer* trc);

    bool external() const { return external_; }
    bool isTypedFunction() const { return kind_ == TableKind::TypedFunction; }
    uint32_t length() const { return length_; }
    Maybe<uint32_t> maximum() const { return maximum_; }
    uint8_t* base() const { return array_.get(); }

    // All table updates must go through set() or setNull().

    void** internalArray() const;
    ExternalTableElem* externalArray() const;
    void set(uint32_t index, void* code, Instance& instance);
    void setNull(uint32_t index);

    uint32_t grow(uint32_t delta, JSContext* cx);
    bool movingGrowable() const;
    bool addMovingGrowObserver(JSContext* cx, WasmInstanceObject* instance);

    // about:memory reporting:

    size_t sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const;
};

typedef RefPtr<Table> SharedTable;
typedef Vector<SharedTable, 0, SystemAllocPolicy> SharedTableVector;

} // namespace wasm
} // namespace js

#endif // wasm_table_h
