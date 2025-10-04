/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_WasmGcObject_inl_h
#define wasm_WasmGcObject_inl_h

#include "wasm/WasmGcObject.h"

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "util/Memory.h"

#include "gc/Nursery-inl.h"
#include "gc/ObjectKind-inl.h"
#include "vm/JSContext-inl.h"

//=========================================================================
// WasmStructObject inlineable allocation methods

namespace js {

/* static */
template <bool ZeroFields>
MOZ_ALWAYS_INLINE WasmStructObject* WasmStructObject::createStructIL(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap) {
  // It is up to our caller to ensure that `typeDefData` refers to a type that
  // doesn't need OOL storage.

  MOZ_ASSERT(IsWasmGcObjectClass(typeDefData->clasp));
  MOZ_ASSERT(!typeDefData->clasp->isNativeObject());
  debugCheckNewObject(typeDefData->shape, typeDefData->allocKind, initialHeap);

  mozilla::DebugOnly<const wasm::TypeDef*> typeDef = typeDefData->typeDef;
  MOZ_ASSERT(typeDef->kind() == wasm::TypeDefKind::Struct);

  // This doesn't need to be rooted, since all we do with it prior to
  // return is to zero out the fields (and then only if ZeroFields is true).
  WasmStructObject* structObj = (WasmStructObject*)cx->newCell<WasmGcObject>(
      typeDefData->allocKind, initialHeap, typeDefData->clasp,
      &typeDefData->allocSite);
  if (MOZ_UNLIKELY(!structObj)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  MOZ_ASSERT((uintptr_t(structObj->inlineData()) % sizeof(uintptr_t)) == 0);
  structObj->initShape(typeDefData->shape);
  structObj->superTypeVector_ = typeDefData->superTypeVector;
  structObj->outlineData_ = nullptr;
  if constexpr (ZeroFields) {
    uint32_t totalBytes = typeDefData->structTypeSize;
    MOZ_ASSERT(totalBytes == typeDef->structType().size_);
    MOZ_ASSERT(totalBytes <= WasmStructObject_MaxInlineBytes);
    MOZ_ASSERT((totalBytes % sizeof(uintptr_t)) == 0);
    memset(structObj->inlineData(), 0, totalBytes);
  }

  js::gc::gcprobes::CreateObject(structObj);
  probes::CreateObject(cx, structObj);

  return structObj;
}

/* static */
template <bool ZeroFields>
MOZ_ALWAYS_INLINE WasmStructObject* WasmStructObject::createStructOOL(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap) {
  // It is up to our caller to ensure that `typeDefData` refers to a type that
  // needs OOL storage.

  MOZ_ASSERT(IsWasmGcObjectClass(typeDefData->clasp));
  MOZ_ASSERT(!typeDefData->clasp->isNativeObject());
  debugCheckNewObject(typeDefData->shape, typeDefData->allocKind, initialHeap);

  mozilla::DebugOnly<const wasm::TypeDef*> typeDef = typeDefData->typeDef;
  MOZ_ASSERT(typeDef->kind() == wasm::TypeDefKind::Struct);

  uint32_t totalBytes = typeDefData->structTypeSize;
  MOZ_ASSERT(totalBytes == typeDef->structType().size_);
  MOZ_ASSERT(totalBytes > WasmStructObject_MaxInlineBytes);
  MOZ_ASSERT((totalBytes % sizeof(uintptr_t)) == 0);

  uint32_t inlineBytes, outlineBytes;
  WasmStructObject::getDataByteSizes(totalBytes, &inlineBytes, &outlineBytes);
  MOZ_ASSERT(inlineBytes == WasmStructObject_MaxInlineBytes);
  MOZ_ASSERT(outlineBytes > 0);

  // Allocate the outline data area before allocating the object so that we can
  // infallibly initialize the outline data area.
  Nursery& nursery = cx->nursery();
  PointerAndUint7 outlineData =
      nursery.mallocedBlockCache().alloc(outlineBytes);
  if (MOZ_UNLIKELY(!outlineData.pointer())) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  // See corresponding comment in WasmArrayObject::createArray.
  Rooted<WasmStructObject*> structObj(cx);
  structObj = (WasmStructObject*)cx->newCell<WasmGcObject>(
      typeDefData->allocKind, initialHeap, typeDefData->clasp,
      &typeDefData->allocSite);
  if (MOZ_UNLIKELY(!structObj)) {
    ReportOutOfMemory(cx);
    if (outlineData.pointer()) {
      nursery.mallocedBlockCache().free(outlineData);
    }
    return nullptr;
  }

  MOZ_ASSERT((uintptr_t(structObj->inlineData()) % sizeof(uintptr_t)) == 0);
  structObj->initShape(typeDefData->shape);
  structObj->superTypeVector_ = typeDefData->superTypeVector;

  // Initialize the outline data fields
  structObj->outlineData_ = (uint8_t*)outlineData.pointer();
  if constexpr (ZeroFields) {
    memset(structObj->inlineData(), 0, inlineBytes);
    memset(outlineData.pointer(), 0, outlineBytes);
  }

  if (MOZ_LIKELY(js::gc::IsInsideNursery(structObj))) {
    // See corresponding comment in WasmArrayObject::createArrayNonEmpty.
    if (MOZ_UNLIKELY(!nursery.registerTrailer(outlineData, outlineBytes))) {
      nursery.mallocedBlockCache().free(outlineData);
      ReportOutOfMemory(cx);
      return nullptr;
    }
  } else {
    // See corresponding comment in WasmArrayObject::createArrayNonEmpty.
    MOZ_ASSERT(structObj->isTenured());
    AddCellMemory(structObj, outlineBytes + wasm::TrailerBlockOverhead,
                  MemoryUse::WasmTrailerBlock);
  }

  js::gc::gcprobes::CreateObject(structObj);
  probes::CreateObject(cx, structObj);

  return structObj;
}

//=========================================================================
// WasmArrayObject inlineable allocation methods

/* static */
inline gc::AllocKind WasmArrayObject::allocKindForOOL() {
  gc::AllocKind allocKind =
      gc::GetGCObjectKindForBytes(sizeof(WasmArrayObject));
  if (CanChangeToBackgroundAllocKind(allocKind, &WasmArrayObject::class_)) {
    allocKind = ForegroundToBackgroundAllocKind(allocKind);
  }
  return allocKind;
}

/* static */
inline gc::AllocKind WasmArrayObject::allocKindForIL(uint32_t storageBytes) {
  gc::AllocKind allocKind =
      gc::GetGCObjectKindForBytes(sizeof(WasmArrayObject) + storageBytes);
  if (CanChangeToBackgroundAllocKind(allocKind, &WasmArrayObject::class_)) {
    allocKind = ForegroundToBackgroundAllocKind(allocKind);
  }
  return allocKind;
}

inline gc::AllocKind WasmArrayObject::allocKind() const {
  if (isDataInline()) {
    uint32_t storageBytes = calcStorageBytes(
        typeDef().arrayType().elementType().size(), numElements_);
    return allocKindForIL(storageBytes);
  }

  return allocKindForOOL();
}

/* static */
template <bool ZeroFields>
MOZ_ALWAYS_INLINE WasmArrayObject* WasmArrayObject::createArrayOOL(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements, uint32_t storageBytes) {
  STATIC_ASSERT_WASMARRAYELEMENTS_NUMELEMENTS_IS_U32;

  MOZ_ASSERT(IsWasmGcObjectClass(typeDefData->clasp));
  MOZ_ASSERT(!typeDefData->clasp->isNativeObject());
  MOZ_ASSERT(typeDefData->allocKind == gc::AllocKind::INVALID);
  gc::AllocKind allocKind = allocKindForOOL();
  debugCheckNewObject(typeDefData->shape, allocKind, initialHeap);

  mozilla::DebugOnly<const wasm::TypeDef*> typeDef = typeDefData->typeDef;
  MOZ_ASSERT(typeDef->kind() == wasm::TypeDefKind::Array);

  // This routine is for large arrays with out-of-line data only. For small
  // arrays use createArrayIL.
  MOZ_ASSERT(storageBytes > WasmArrayObject_MaxInlineBytes);

  // Allocate the outline data before allocating the object so that we can
  // infallibly initialize the pointer on the array object after it is
  // allocated.
  Nursery& nursery = cx->nursery();
  PointerAndUint7 outlineAlloc(nullptr, 0);
  outlineAlloc = nursery.mallocedBlockCache().alloc(storageBytes);
  if (MOZ_UNLIKELY(!outlineAlloc.pointer())) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  // It's unfortunate that `arrayObj` has to be rooted, since this is a hot
  // path and rooting costs around 15 instructions.  It is the call to
  // registerTrailer that makes it necessary.
  Rooted<WasmArrayObject*> arrayObj(cx);
  arrayObj = (WasmArrayObject*)cx->newCell<WasmGcObject>(
      allocKind, initialHeap, typeDefData->clasp, &typeDefData->allocSite);
  if (MOZ_UNLIKELY(!arrayObj)) {
    ReportOutOfMemory(cx);
    if (outlineAlloc.pointer()) {
      nursery.mallocedBlockCache().free(outlineAlloc);
    }
    return nullptr;
  }

  DataHeader* outlineHeader = (DataHeader*)outlineAlloc.pointer();
  uint8_t* outlineData = (uint8_t*)(outlineHeader + 1);
  *outlineHeader = DataIsOOL;

  arrayObj->initShape(typeDefData->shape);
  arrayObj->superTypeVector_ = typeDefData->superTypeVector;
  arrayObj->numElements_ = numElements;
  arrayObj->data_ = outlineData;
  if constexpr (ZeroFields) {
    uint32_t dataBytes = storageBytes - sizeof(DataHeader);
    MOZ_ASSERT(dataBytes >= numElements * typeDefData->arrayElemSize);
    memset(arrayObj->data_, 0, dataBytes);
  }

  MOZ_ASSERT(!arrayObj->isDataInline());

  if (MOZ_LIKELY(js::gc::IsInsideNursery(arrayObj))) {
    // We need to register the OOL area with the nursery, so it will be freed
    // after GCing of the nursery if `arrayObj_` doesn't make it into the
    // tenured heap.  Note, the nursery will keep a running total of the
    // current trailer block sizes, so it can decide to do a (minor)
    // collection if that becomes excessive.
    if (MOZ_UNLIKELY(!nursery.registerTrailer(outlineAlloc, storageBytes))) {
      nursery.mallocedBlockCache().free(outlineAlloc);
      ReportOutOfMemory(cx);
      return nullptr;
    }
  } else {
    MOZ_ASSERT(arrayObj->isTenured());
    // Register the trailer size with the major GC mechanism, so that can also
    // is able to decide if that space use warrants a (major) collection.
    AddCellMemory(arrayObj, storageBytes + wasm::TrailerBlockOverhead,
                  MemoryUse::WasmTrailerBlock);
  }

  js::gc::gcprobes::CreateObject(arrayObj);
  probes::CreateObject(cx, arrayObj);

  return arrayObj;
}

template WasmArrayObject* WasmArrayObject::createArrayOOL<true>(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements, uint32_t storageBytes);
template WasmArrayObject* WasmArrayObject::createArrayOOL<false>(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements, uint32_t storageBytes);

/* static */
template <bool ZeroFields>
MOZ_ALWAYS_INLINE WasmArrayObject* WasmArrayObject::createArrayIL(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements, uint32_t storageBytes) {
  STATIC_ASSERT_WASMARRAYELEMENTS_NUMELEMENTS_IS_U32;

  MOZ_ASSERT(IsWasmGcObjectClass(typeDefData->clasp));
  MOZ_ASSERT(!typeDefData->clasp->isNativeObject());
  MOZ_ASSERT(typeDefData->allocKind == gc::AllocKind::INVALID);
  gc::AllocKind allocKind = allocKindForIL(storageBytes);
  debugCheckNewObject(typeDefData->shape, allocKind, initialHeap);

  mozilla::DebugOnly<const wasm::TypeDef*> typeDef = typeDefData->typeDef;
  MOZ_ASSERT(typeDef->kind() == wasm::TypeDefKind::Array);

  MOZ_ASSERT(storageBytes <= WasmArrayObject_MaxInlineBytes);

  // There's no need for `arrayObj` to be rooted, since the only thing we're
  // going to do is fill in some bits of it, then return it.
  WasmArrayObject* arrayObj = (WasmArrayObject*)cx->newCell<WasmGcObject>(
      allocKind, initialHeap, typeDefData->clasp, &typeDefData->allocSite);
  if (MOZ_UNLIKELY(!arrayObj)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  arrayObj->initShape(typeDefData->shape);
  arrayObj->superTypeVector_ = typeDefData->superTypeVector;
  arrayObj->numElements_ = numElements;

  DataHeader* inlineHeader =
      WasmArrayObject::addressOfInlineDataHeader(arrayObj);
  uint8_t* inlineData = WasmArrayObject::addressOfInlineData(arrayObj);
  *inlineHeader = DataIsIL;
  arrayObj->data_ = inlineData;

  if constexpr (ZeroFields) {
    uint32_t dataBytes = storageBytes - sizeof(DataHeader);
    MOZ_ASSERT(dataBytes >= numElements * typeDefData->arrayElemSize);

    if (numElements > 0) {
      memset(arrayObj->data_, 0, dataBytes);
    }
  }

  MOZ_ASSERT(arrayObj->isDataInline());

  js::gc::gcprobes::CreateObject(arrayObj);
  probes::CreateObject(cx, arrayObj);

  return arrayObj;
}

template WasmArrayObject* WasmArrayObject::createArrayIL<true>(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements, uint32_t storageBytes);
template WasmArrayObject* WasmArrayObject::createArrayIL<false>(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements, uint32_t storageBytes);

/* static */
template <bool ZeroFields>
MOZ_ALWAYS_INLINE WasmArrayObject* WasmArrayObject::createArray(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements) {
  MOZ_ASSERT(typeDefData->arrayElemSize ==
             typeDefData->typeDef->arrayType().elementType().size());
  CheckedUint32 storageBytes =
      calcStorageBytesChecked(typeDefData->arrayElemSize, numElements);
  if (!storageBytes.isValid() ||
      storageBytes.value() > uint32_t(wasm::MaxArrayPayloadBytes)) {
    wasm::ReportTrapError(cx, JSMSG_WASM_ARRAY_IMP_LIMIT);
    return nullptr;
  }

  if (storageBytes.value() <= WasmArrayObject_MaxInlineBytes) {
    return createArrayIL<ZeroFields>(cx, typeDefData, initialHeap, numElements,
                                     storageBytes.value());
  }

  return createArrayOOL<ZeroFields>(cx, typeDefData, initialHeap, numElements,
                                    storageBytes.value());
}

template WasmArrayObject* WasmArrayObject::createArray<true>(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements);
template WasmArrayObject* WasmArrayObject::createArray<false>(
    JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
    js::gc::Heap initialHeap, uint32_t numElements);

}  // namespace js

#endif /* wasm_WasmGcObject_inl_h */
