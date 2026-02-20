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

#include "wasm/WasmTable.h"

#include "mozilla/CheckedInt.h"

#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmValue.h"

#include "gc/StableCellHasher-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::wasm;
using mozilla::CheckedInt;

Table::Table(JSContext* cx, const TableDesc& desc,
             Handle<WasmTableObject*> maybeObject, FuncRefVector&& functions)
    : maybeObject_(maybeObject),
      observers_(cx->zone()),
      functions_(std::move(functions)),
      addressType_(desc.addressType()),
      elemType_(desc.elemType),
      isAsmJS_(desc.isAsmJS),
      length_(desc.initialLength()),
      maximum_(desc.maximumLength()) {
  // Acquire a strong reference to the type definition this table may be
  // referencing.
  elemType_.AddRef();
  MOZ_ASSERT(repr() == TableRepr::Func);
  MOZ_ASSERT(length_ <= MaxTableElemsRuntime);
}

Table::Table(JSContext* cx, const TableDesc& desc,
             Handle<WasmTableObject*> maybeObject, TableAnyRefVector&& objects)
    : maybeObject_(maybeObject),
      observers_(cx->zone()),
      objects_(std::move(objects)),
      addressType_(desc.addressType()),
      elemType_(desc.elemType),
      isAsmJS_(desc.isAsmJS),
      length_(desc.initialLength()),
      maximum_(desc.maximumLength()) {
  // Acquire a strong reference to the type definition this table may be
  // referencing.
  elemType_.AddRef();
  MOZ_ASSERT(repr() == TableRepr::Ref);
  MOZ_ASSERT(length_ <= MaxTableElemsRuntime);
}

Table::~Table() {
  // Release the strong reference, if any.
  elemType_.Release();
}

/* static */
SharedTable Table::create(JSContext* cx, const TableDesc& desc,
                          Handle<WasmTableObject*> maybeObject) {
  // Tables are initialized with init_expr values at Instance::init or
  // WasmTableObject::create.

  switch (desc.elemType.tableRepr()) {
    case TableRepr::Func: {
      FuncRefVector functions;
      if (!functions.resize(desc.initialLength())) {
        ReportOutOfMemory(cx);
        return nullptr;
      }
      return SharedTable(
          cx->new_<Table>(cx, desc, maybeObject, std::move(functions)));
    }
    case TableRepr::Ref: {
      TableAnyRefVector objects;
      if (!objects.resize(desc.initialLength())) {
        ReportOutOfMemory(cx);
        return nullptr;
      }
      return SharedTable(
          cx->new_<Table>(cx, desc, maybeObject, std::move(objects)));
    }
  }
  MOZ_CRASH("switch is exhaustive");
}

void Table::tracePrivate(JSTracer* trc) {
  // If this table has a WasmTableObject, then this method is only called by
  // WasmTableObject's trace hook so maybeObject_ must already be marked.
  // TraceEdge is called so that the pointer can be updated during a moving
  // GC.
  TraceNullableEdge(trc, &maybeObject_, "wasm table object");

  switch (repr()) {
    case TableRepr::Func: {
      if (isAsmJS_) {
#ifdef DEBUG
        for (uint32_t i = 0; i < length_; i++) {
          MOZ_ASSERT(!functions_[i].instance);
        }
#endif
        break;
      }

      for (uint32_t i = 0; i < length_; i++) {
        if (functions_[i].instance) {
          wasm::TraceInstanceEdge(trc, functions_[i].instance,
                                  "wasm table instance");
        } else {
          MOZ_ASSERT(!functions_[i].code);
        }
      }
      break;
    }
    case TableRepr::Ref: {
      objects_.trace(trc);
      break;
    }
  }
}

void Table::trace(JSTracer* trc) {
  // The trace hook of WasmTableObject will call Table::tracePrivate at
  // which point we can mark the rest of the children. If there is no
  // WasmTableObject, call Table::tracePrivate directly. Redirecting through
  // the WasmTableObject avoids marking the entire Table on each incoming
  // edge (once per dependent Instance).
  if (maybeObject_) {
    TraceEdge(trc, &maybeObject_, "wasm table object");
  } else {
    tracePrivate(trc);
  }
}

uint8_t* Table::instanceElements() const {
  if (repr() == TableRepr::Ref) {
    return (uint8_t*)objects_.begin();
  }
  return (uint8_t*)functions_.begin();
}

const FunctionTableElem& Table::getFuncRef(uint32_t address) const {
  MOZ_ASSERT(isFunction());
  return functions_[address];
}

bool Table::getFuncRef(JSContext* cx, uint32_t address,
                       MutableHandleFunction fun) const {
  MOZ_ASSERT(isFunction());

  const FunctionTableElem& elem = getFuncRef(address);
  if (!elem.code) {
    fun.set(nullptr);
    return true;
  }

  Instance& instance = *elem.instance;
  const CodeRange& codeRange = *instance.code().lookupFuncRange(elem.code);
  return instance.getExportedFunction(cx, codeRange.funcIndex(), fun);
}

void Table::setFuncRef(uint32_t address, JSFunction* fun) {
  MOZ_ASSERT(isFunction());
  MOZ_ASSERT(fun->isWasm());

  // Tables can store references to wasm functions from other instances. To
  // preserve the === function identity required by the JS embedding spec, we
  // must set the element to the function's underlying
  // CodeRange.funcCheckedCallEntry and Instance so that Table.get()s always
  // produce the same function object as was imported.
  setFuncRef(address, fun->wasmCheckedCallEntry(), &fun->wasmInstance());
}

void Table::setFuncRef(uint32_t address, void* code, Instance* instance) {
  MOZ_ASSERT(isFunction());

  FunctionTableElem& elem = functions_[address];
  if (elem.instance) {
    gc::PreWriteBarrier(elem.instance->objectUnbarriered());
  }

  if (!isAsmJS_) {
    elem.code = code;
    elem.instance = instance;
    MOZ_ASSERT(elem.instance->objectUnbarriered()->isTenured(),
               "no postWriteBarrier (Table::set)");
  } else {
    elem.code = code;
    elem.instance = nullptr;
  }
}

void Table::fillFuncRef(uint32_t address, uint32_t fillCount, FuncRef ref,
                        JSContext* cx) {
  MOZ_ASSERT(isFunction());

  if (ref.isNull()) {
    for (uint32_t i = address, end = address + fillCount; i != end; i++) {
      setNull(i);
    }
    return;
  }

  RootedFunction fun(cx, ref.asJSFunction());
  void* code = fun->wasmCheckedCallEntry();
  Instance& instance = fun->wasmInstance();
  for (uint32_t i = address, end = address + fillCount; i != end; i++) {
    setFuncRef(i, code, &instance);
  }
}

AnyRef Table::getAnyRef(uint32_t address) const {
  MOZ_ASSERT(!isFunction());
  return objects_[address];
}

void Table::setAnyRef(uint32_t address, AnyRef ref) {
  MOZ_ASSERT(!isFunction());
  objects_[address] = ref;
}

void Table::fillAnyRef(uint32_t address, uint32_t fillCount, AnyRef ref) {
  MOZ_ASSERT(!isFunction());
  for (uint32_t i = address, end = address + fillCount; i != end; i++) {
    objects_[i] = ref;
  }
}

void Table::setRef(uint32_t address, AnyRef ref) {
  if (ref.isNull()) {
    setNull(address);
  } else if (isFunction()) {
    JSFunction* func = &ref.toJSObject().as<JSFunction>();
    setFuncRef(address, func);
  } else {
    setAnyRef(address, ref);
  }
}

bool Table::getValue(JSContext* cx, uint32_t address,
                     MutableHandleValue result) const {
  switch (repr()) {
    case TableRepr::Func: {
      MOZ_RELEASE_ASSERT(!isAsmJS());
      RootedFunction fun(cx);
      if (!getFuncRef(cx, address, &fun)) {
        return false;
      }
      result.setObjectOrNull(fun);
      return true;
    }
    case TableRepr::Ref: {
      if (!ValType(elemType_).isExposable()) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_BAD_VAL_TYPE);
        return false;
      }
      return ToJSValue(cx, &objects_[address], ValType(elemType_), result);
    }
    default:
      MOZ_CRASH();
  }
}

void Table::setNull(uint32_t address) {
  switch (repr()) {
    case TableRepr::Func: {
      MOZ_RELEASE_ASSERT(!isAsmJS_);
      FunctionTableElem& elem = functions_[address];
      if (elem.instance) {
        gc::PreWriteBarrier(elem.instance->objectUnbarriered());
      }

      elem.code = nullptr;
      elem.instance = nullptr;
      break;
    }
    case TableRepr::Ref: {
      setAnyRef(address, AnyRef::null());
      break;
    }
  }
}

bool Table::copy(JSContext* cx, const Table& srcTable, uint32_t dstIndex,
                 uint32_t srcIndex) {
  MOZ_RELEASE_ASSERT(!srcTable.isAsmJS_);
  switch (repr()) {
    case TableRepr::Func: {
      MOZ_RELEASE_ASSERT(elemType().isFuncHierarchy() &&
                         srcTable.elemType().isFuncHierarchy());
      FunctionTableElem& dst = functions_[dstIndex];
      if (dst.instance) {
        gc::PreWriteBarrier(dst.instance->objectUnbarriered());
      }

      const FunctionTableElem& src = srcTable.functions_[srcIndex];
      dst.code = src.code;
      dst.instance = src.instance;

      if (dst.instance) {
        MOZ_ASSERT(dst.code);
        MOZ_ASSERT(dst.instance->objectUnbarriered()->isTenured(),
                   "no postWriteBarrier (Table::copy)");
      } else {
        MOZ_ASSERT(!dst.code);
      }
      break;
    }
    case TableRepr::Ref: {
      switch (srcTable.repr()) {
        case TableRepr::Ref: {
          setAnyRef(dstIndex, srcTable.getAnyRef(srcIndex));
          break;
        }
        case TableRepr::Func: {
          MOZ_RELEASE_ASSERT(srcTable.elemType().isFuncHierarchy());
          // Upcast.
          RootedFunction fun(cx);
          if (!srcTable.getFuncRef(cx, srcIndex, &fun)) {
            // OOM, so just pass it on.
            return false;
          }
          setAnyRef(dstIndex, AnyRef::fromJSObject(*fun));
          break;
        }
      }
      break;
    }
  }
  return true;
}

uint32_t Table::grow(uint32_t delta) {
  // This isn't just an optimization: movingGrowable() assumes that
  // onMovingGrowTable does not fire when length == maximum.
  if (!delta) {
    return length_;
  }

  uint32_t oldLength = length_;

  CheckedInt<uint32_t> newLength = oldLength;
  newLength += delta;
  if (!newLength.isValid() || newLength.value() > MaxTableElemsRuntime) {
    return -1;
  }

  if (maximum_ && newLength.value() > maximum_.value()) {
    return -1;
  }

  MOZ_ASSERT(movingGrowable());

  switch (repr()) {
    case TableRepr::Func: {
      MOZ_RELEASE_ASSERT(!isAsmJS_);
      if (!functions_.resize(newLength.value())) {
        return -1;
      }
      break;
    }
    case TableRepr::Ref: {
      if (!objects_.resize(newLength.value())) {
        return -1;
      }
      break;
    }
  }

  if (auto* object = maybeObject_.unbarrieredGet()) {
    RemoveCellMemory(object, gcMallocBytes(), MemoryUse::WasmTableTable);
  }

  length_ = newLength.value();

  if (auto* object = maybeObject_.unbarrieredGet()) {
    AddCellMemory(object, gcMallocBytes(), MemoryUse::WasmTableTable);
  }

  for (InstanceSet::Range r = observers_.all(); !r.empty(); r.popFront()) {
    r.front()->instance().onMovingGrowTable(this);
  }

  return oldLength;
}

bool Table::movingGrowable() const {
  return !maximum_ || length_ < maximum_.value();
}

bool Table::addMovingGrowObserver(JSContext* cx, WasmInstanceObject* instance) {
  MOZ_ASSERT(movingGrowable());

  // A table can be imported multiple times into an instance, but we only
  // register the instance as an observer once.

  if (!observers_.put(instance)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

void Table::fillUninitialized(uint32_t address, uint32_t fillCount,
                              HandleAnyRef ref, JSContext* cx) {
#ifdef DEBUG
  assertRangeNull(address, fillCount);
#endif  // DEBUG
  switch (repr()) {
    case TableRepr::Func: {
      MOZ_RELEASE_ASSERT(!isAsmJS_);
      fillFuncRef(address, fillCount, FuncRef::fromAnyRefUnchecked(ref), cx);
      break;
    }
    case TableRepr::Ref: {
      fillAnyRef(address, fillCount, ref);
      break;
    }
  }
}

#ifdef DEBUG
void Table::assertRangeNull(uint32_t address, uint32_t length) const {
  switch (repr()) {
    case TableRepr::Func:
      for (uint32_t i = address; i < address + length; i++) {
        MOZ_ASSERT(getFuncRef(i).instance == nullptr);
        MOZ_ASSERT(getFuncRef(i).code == nullptr);
      }
      break;
    case TableRepr::Ref:
      for (uint32_t i = address; i < address + length; i++) {
        MOZ_ASSERT(getAnyRef(i).isNull());
      }
      break;
  }
}

void Table::assertRangeNotNull(uint32_t address, uint32_t length) const {
  switch (repr()) {
    case TableRepr::Func:
      for (uint32_t i = address; i < address + length; i++) {
        MOZ_ASSERT_IF(!isAsmJS_, getFuncRef(i).instance != nullptr);
        MOZ_ASSERT(getFuncRef(i).code != nullptr);
      }
      break;
    case TableRepr::Ref:
      for (uint32_t i = address; i < address + length; i++) {
        MOZ_ASSERT(!getAnyRef(i).isNull());
      }
      break;
  }
}
#endif  // DEBUG

size_t Table::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
  if (isFunction()) {
    return functions_.sizeOfExcludingThis(mallocSizeOf);
  }
  return objects_.sizeOfExcludingThis(mallocSizeOf);
}

size_t Table::gcMallocBytes() const {
  size_t size = sizeof(*this);
  if (isFunction()) {
    size += length() * sizeof(FunctionTableElem);
  } else {
    size += length() * sizeof(TableAnyRefVector::ElementType);
  }
  return size;
}
