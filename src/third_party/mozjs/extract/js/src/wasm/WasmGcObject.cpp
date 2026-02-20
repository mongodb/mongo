/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "wasm/WasmGcObject-inl.h"

#include <algorithm>

#include "gc/Marking.h"
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/Vector.h"
#include "util/StringBuilder.h"
#include "vm/GlobalObject.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/PropertyResult.h"
#include "vm/Realm.h"
#include "vm/SelfHosting.h"
#include "vm/StringType.h"
#include "vm/TypedArrayObject.h"
#include "vm/Uint8Clamped.h"

#include "gc/GCContext-inl.h"  // GCContext::removeCellMemory
#include "gc/ObjectKind-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;
using namespace wasm;

// [SMDOC] Management of OOL storage areas for Wasm{Array,Struct}Object.
//
// WasmArrayObject always has its payload data stored in a block the C++-heap,
// which is pointed to from the WasmArrayObject.  The same is true for
// WasmStructObject in the case where the fields cannot fit in the object
// itself.  These C++ blocks are in some places referred to as "trailer blocks".
//
// The presence of trailer blocks complicates the use of generational GC (that
// is, Nursery allocation) of Wasm{Array,Struct}Object.  In particular:
//
// (1) For objects which do not get tenured at minor collection, there must be
//     a way to free the associated trailer, but there is no way to visit
//     non-tenured blocks during minor collection.
//
// (2) Even if (1) were solved, calling js_malloc/js_free for every object
//     creation-death cycle is expensive, possibly around 400 machine
//     instructions, and we expressly want to avoid that in a generational GC
//     scenario.
//
// The following scheme is therefore employed.
//
// (a) gc::Nursery maintains a pool of available C++-heap-allocated blocks --
//     a js::MallocedBlockCache -- and the intention is that trailers are
//     allocated from this pool and freed back into it whenever possible.
//
// (b) WasmArrayObject::createArrayNonEmpty and
//     WasmStructObject::createStructOOL always request trailer allocation
//     from the nursery's cache (a).  If the cache cannot honour the request
//     directly it will allocate directly from js_malloc; we hope this happens
//     only infrequently.
//
// (c) The allocated block is returned as a js::PointerAndUint7, a pair that
//     holds the trailer block pointer and an auxiliary tag that the
//     js::MallocedBlockCache needs to see when the block is freed.
//
//     The raw trailer block pointer (a `void*`) is stored in the
//     Wasm{Array,Struct}Object OOL data field.  These objects are not aware
//     of and do not interact with js::PointerAndUint7, and nor does any
//     JIT-generated code.
//
// (d) Still in WasmArrayObject::createArrayNonEmpty and
//     WasmStructObject::createStructOOL, if the object was allocated in the
//     nursery, then the resulting js::PointerAndUint7 is "registered" with
//     the nursery by handing it to Nursery::registerTrailer.
//
// (e) When a minor collection happens (Nursery::doCollection), we are
//     notified of objects that are moved by calls to the ::obj_moved methods
//     in this file.  For those objects that have been tenured, the raw
//     trailer pointer is "unregistered" with the nursery by handing it to
//     Nursery::unregisterTrailer.
//
// (f) Still during minor collection: The nursery now knows both the set of
//     trailer blocks added, and those removed because the corresponding
//     object has been tenured.  The difference between these two sets (that
//     is, `added - removed`) is the set of trailer blocks corresponding to
//     blocks that didn't get tenured.  That set is computed and freed (back
//     to the nursery's js::MallocedBlockCache) by Nursery::freeTrailerBlocks.
//
// (g) At the end of minor collection, the added and removed sets are made
//     empty, and the cycle begins again.
//
// (h) Also at the end of minor collection, a call to
//     `mallocedBlockCache_.preen` hands a few blocks in the cache back to
//     js_free.  This mechanism exists so as to ensure that unused blocks do
//     not remain in the cache indefinitely.
//
// (i) In order that the tenured heap is collected "often enough" in the case
//     where the trailer blocks are large (relative to their owning objects),
//     we have to tell the tenured heap about the sizes of trailers entering
//     and leaving it.  This is done via calls to AddCellMemory and
//     GCContext::removeCellMemory.
//
// (j) For objects that got tenured, we are eventually notified of their death
//     by a call to the ::obj_finalize methods below.  At that point we hand
//     their block pointers to js_free.
//
// (k) When the nursery is eventually destroyed, all blocks in its block cache
//     are handed to js_free.  Hence, at process exit, provided all nurseries
//     are first collected and then their destructors run, no C++ heap blocks
//     are leaked.
//
// As a result of this scheme, trailer blocks associated with what we hope is
// the frequent case -- objects that are allocated but never make it out of
// the nursery -- are cycled through the nursery's block cache.
//
// Trailers associated with tenured blocks cannot participate though; they are
// always returned to js_free.  Making them participate is difficult: it would
// require changing their owning object's OOL data pointer to be a
// js::PointerAndUint7 rather than a raw `void*`, so that then the blocks
// could be released to the cache in the ::obj_finalize methods.  This would
// however require changes in the generated code for array element and OOL
// struct element accesses.
//
// It would also lead to threading difficulties, because the ::obj_finalize
// methods run on a background thread, whilst allocation from the cache
// happens on the main thread, but the MallocedBlockCache is not thread safe.
// Making it thread safe would entail adding a locking mechanism, but that's
// potentially slow and so negates the point of having a cache at all.
//
// Here's a short summary of the trailer block life cycle:
//
// * allocated:
//
//   - in WasmArrayObject::createArrayNonEmpty
//     and WasmStructObject::createStructOOL
//
//   - by calling the nursery's MallocBlockCache alloc method
//
// * deallocated:
//
//   - for non-tenured objects, in the collector itself,
//     in Nursery::doCollection calling Nursery::freeTrailerBlocks,
//     releasing to the nursery's block cache
//
//   - for tenured objects, in the ::obj_finalize methods, releasing directly
//     to js_free
//
// If this seems confusing ("why is it ok to allocate from the cache but
// release to js_free?"), remember that the cache holds blocks previously
// obtained from js_malloc but which are *not* currently in use.  Hence it is
// fine to give them back to js_free; that just makes the cache a bit emptier
// but has no effect on correctness.

//=========================================================================
// WasmGcObject

const ObjectOps WasmGcObject::objectOps_ = {
    WasmGcObject::obj_lookupProperty,            // lookupProperty
    WasmGcObject::obj_defineProperty,            // defineProperty
    WasmGcObject::obj_hasProperty,               // hasProperty
    WasmGcObject::obj_getProperty,               // getProperty
    WasmGcObject::obj_setProperty,               // setProperty
    WasmGcObject::obj_getOwnPropertyDescriptor,  // getOwnPropertyDescriptor
    WasmGcObject::obj_deleteProperty,            // deleteProperty
    nullptr,                                     // getElements
    nullptr,                                     // funToString
};

/* static */
bool WasmGcObject::obj_lookupProperty(JSContext* cx, HandleObject obj,
                                      HandleId id, MutableHandleObject objp,
                                      PropertyResult* propp) {
  objp.set(nullptr);
  propp->setNotFound();
  return true;
}

bool WasmGcObject::obj_defineProperty(JSContext* cx, HandleObject obj,
                                      HandleId id,
                                      Handle<PropertyDescriptor> desc,
                                      ObjectOpResult& result) {
  result.failReadOnly();
  return true;
}

bool WasmGcObject::obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   bool* foundp) {
  *foundp = false;
  return true;
}

bool WasmGcObject::obj_getProperty(JSContext* cx, HandleObject obj,
                                   HandleValue receiver, HandleId id,
                                   MutableHandleValue vp) {
  vp.setUndefined();
  return true;
}

bool WasmGcObject::obj_setProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   HandleValue v, HandleValue receiver,
                                   ObjectOpResult& result) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_WASM_MODIFIED_GC_OBJECT);
  return false;
}

bool WasmGcObject::obj_getOwnPropertyDescriptor(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc) {
  desc.reset();
  return true;
}

bool WasmGcObject::obj_deleteProperty(JSContext* cx, HandleObject obj,
                                      HandleId id, ObjectOpResult& result) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_WASM_MODIFIED_GC_OBJECT);
  return false;
}

bool WasmGcObject::lookUpProperty(JSContext* cx, Handle<WasmGcObject*> obj,
                                  jsid id, WasmGcObject::PropOffset* offset,
                                  StorageType* type) {
  switch (obj->kind()) {
    case wasm::TypeDefKind::Struct: {
      const auto& structType = obj->typeDef().structType();
      uint32_t index;
      if (!IdIsIndex(id, &index)) {
        return false;
      }
      if (index >= structType.fields_.length()) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_OUT_OF_BOUNDS);
        return false;
      }
      offset->set(structType.fieldOffset(index));
      *type = structType.fields_[index].type;
      return true;
    }
    case wasm::TypeDefKind::Array: {
      const auto& arrayType = obj->typeDef().arrayType();

      uint32_t index;
      if (!IdIsIndex(id, &index)) {
        return false;
      }
      uint32_t numElements = obj->as<WasmArrayObject>().numElements_;
      if (index >= numElements) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_OUT_OF_BOUNDS);
        return false;
      }
      uint64_t scaledIndex =
          uint64_t(index) * uint64_t(arrayType.elementType().size());
      if (scaledIndex >= uint64_t(UINT32_MAX)) {
        // It's unrepresentable as an WasmGcObject::PropOffset. Give up.
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_OUT_OF_BOUNDS);
        return false;
      }
      offset->set(uint32_t(scaledIndex));
      *type = arrayType.elementType();
      return true;
    }
    default:
      MOZ_ASSERT_UNREACHABLE();
      return false;
  }
}

bool WasmGcObject::loadValue(JSContext* cx, Handle<WasmGcObject*> obj, jsid id,
                             MutableHandleValue vp) {
  WasmGcObject::PropOffset offset;
  StorageType type;
  if (!lookUpProperty(cx, obj, id, &offset, &type)) {
    return false;
  }

  // Temporary hack, (ref T) is not exposable to JS yet but some tests would
  // like to access it so we erase (ref T) with eqref when loading. This is
  // safe as (ref T) <: eqref and we're not in the writing case where we
  // would need to perform a type check.
  if (type.isTypeRef()) {
    type = RefType::fromTypeCode(TypeCode::EqRef, true);
  }

  if (!type.isExposable()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }

  if (obj->is<WasmStructObject>()) {
    // `offset` is the field offset, without regard to the in/out-line split.
    // That is handled by the call to `fieldOffsetToAddress`.
    WasmStructObject& structObj = obj->as<WasmStructObject>();
    // Ensure no out-of-range access possible
    MOZ_RELEASE_ASSERT(structObj.kind() == TypeDefKind::Struct);
    MOZ_RELEASE_ASSERT(offset.get() + type.size() <=
                       structObj.typeDef().structType().size_);
    return ToJSValue(cx, structObj.fieldOffsetToAddress(type, offset.get()),
                     type, vp);
  }

  MOZ_ASSERT(obj->is<WasmArrayObject>());
  const WasmArrayObject& arrayObj = obj->as<WasmArrayObject>();
  return ToJSValue(cx, arrayObj.data_ + offset.get(), type, vp);
}

bool WasmGcObject::isRuntimeSubtypeOf(
    const wasm::TypeDef* parentTypeDef) const {
  return TypeDef::isSubTypeOf(&typeDef(), parentTypeDef);
}

bool WasmGcObject::obj_newEnumerate(JSContext* cx, HandleObject obj,
                                    MutableHandleIdVector properties,
                                    bool enumerableOnly) {
  return true;
}

static void WriteValTo(const Val& val, StorageType ty, void* dest) {
  switch (ty.kind()) {
    case StorageType::I8:
      *((uint8_t*)dest) = val.i32();
      break;
    case StorageType::I16:
      *((uint16_t*)dest) = val.i32();
      break;
    case StorageType::I32:
      *((uint32_t*)dest) = val.i32();
      break;
    case StorageType::I64:
      *((uint64_t*)dest) = val.i64();
      break;
    case StorageType::F32:
      *((float*)dest) = val.f32();
      break;
    case StorageType::F64:
      *((double*)dest) = val.f64();
      break;
    case StorageType::V128:
      *((V128*)dest) = val.v128();
      break;
    case StorageType::Ref:
      *((GCPtr<AnyRef>*)dest) = val.ref();
      break;
  }
}

//=========================================================================
// WasmArrayObject

/* static */
void WasmArrayObject::obj_trace(JSTracer* trc, JSObject* object) {
  WasmArrayObject& arrayObj = object->as<WasmArrayObject>();
  uint8_t* data = arrayObj.data_;

  const auto& typeDef = arrayObj.typeDef();
  const auto& arrayType = typeDef.arrayType();
  if (!arrayType.elementType().isRefRepr()) {
    return;
  }

  uint32_t numElements = arrayObj.numElements_;
  uint32_t elemSize = arrayType.elementType().size();
  for (uint32_t i = 0; i < numElements; i++) {
    AnyRef* elementPtr = reinterpret_cast<AnyRef*>(data + i * elemSize);
    TraceManuallyBarrieredEdge(trc, elementPtr, "wasm-array-element");
  }
}

/* static */
void WasmArrayObject::obj_finalize(JS::GCContext* gcx, JSObject* object) {
  // This method, and also ::obj_moved and the WasmStructObject equivalents,
  // assumes that the object's TypeDef (as reachable via its SuperTypeVector*)
  // stays alive at least as long as the object.
  WasmArrayObject& arrayObj = object->as<WasmArrayObject>();
  if (!arrayObj.isDataInline()) {
    // Free the trailer block.  Unfortunately we can't give it back to the
    // malloc'd block cache because we might not be running on the main
    // thread, and the cache isn't thread-safe.
    js_free(arrayObj.dataHeader());
    // And tell the tenured-heap accounting machinery that the trailer has
    // been freed.
    const TypeDef& typeDef = arrayObj.typeDef();
    MOZ_ASSERT(typeDef.isArrayType());
    // arrayObj.numElements_ was validated to not overflow when constructing the
    // array
    size_t trailerSize = calcStorageBytesUnchecked(
        typeDef.arrayType().elementType().size(), arrayObj.numElements_);
    // Ensured by WasmArrayObject::createArrayNonEmpty.
    MOZ_RELEASE_ASSERT(trailerSize <= size_t(MaxArrayPayloadBytes));
    gcx->removeCellMemory(&arrayObj, trailerSize + TrailerBlockOverhead,
                          MemoryUse::WasmTrailerBlock);
    // For safety
    arrayObj.data_ = nullptr;
  }
}

/* static */
size_t WasmArrayObject::obj_moved(JSObject* obj, JSObject* old) {
  // Moving inline arrays requires us to update the data pointer.
  WasmArrayObject& arrayObj = obj->as<WasmArrayObject>();
  WasmArrayObject& oldArrayObj = old->as<WasmArrayObject>();
  if (oldArrayObj.isDataInline()) {
    // The old array had inline storage, which has been copied.
    // Fix up the data pointer on the new array to point to it.
    arrayObj.data_ = WasmArrayObject::addressOfInlineData(&arrayObj);
  }
  MOZ_ASSERT(arrayObj.isDataInline() == oldArrayObj.isDataInline());

  if (IsInsideNursery(old)) {
    Nursery& nursery = obj->runtimeFromMainThread()->gc.nursery();
    // It's been tenured.
    if (!arrayObj.isDataInline()) {
      const TypeDef& typeDef = arrayObj.typeDef();
      MOZ_ASSERT(typeDef.isArrayType());
      // arrayObj.numElements_ was validated not to overflow when constructing
      // the array
      size_t trailerSize = calcStorageBytesUnchecked(
          typeDef.arrayType().elementType().size(), arrayObj.numElements_);
      // Ensured by WasmArrayObject::createArrayOOL.
      MOZ_RELEASE_ASSERT(trailerSize <= size_t(MaxArrayPayloadBytes));
      nursery.trackTrailerOnPromotion(arrayObj.dataHeader(), obj, trailerSize,
                                      TrailerBlockOverhead,
                                      MemoryUse::WasmTrailerBlock);
    }
  }

  return 0;
}

void WasmArrayObject::storeVal(const Val& val, uint32_t itemIndex) {
  const ArrayType& arrayType = typeDef().arrayType();
  size_t elementSize = arrayType.elementType().size();
  MOZ_ASSERT(itemIndex < numElements_);
  uint8_t* data = data_ + elementSize * itemIndex;
  WriteValTo(val, arrayType.elementType(), data);
}

void WasmArrayObject::fillVal(const Val& val, uint32_t itemIndex,
                              uint32_t len) {
  const ArrayType& arrayType = typeDef().arrayType();
  size_t elementSize = arrayType.elementType().size();
  uint8_t* data = data_ + elementSize * itemIndex;
  MOZ_ASSERT(itemIndex <= numElements_ && len <= numElements_ - itemIndex);
  for (uint32_t i = 0; i < len; i++) {
    WriteValTo(val, arrayType.elementType(), data);
    data += elementSize;
  }
}

static const JSClassOps WasmArrayObjectClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate   */
    WasmGcObject::obj_newEnumerate,
    nullptr,                       /* resolve     */
    nullptr,                       /* mayResolve  */
    WasmArrayObject::obj_finalize, /* finalize    */
    nullptr,                       /* call        */
    nullptr,                       /* construct   */
    WasmArrayObject::obj_trace,
};
static const ClassExtension WasmArrayObjectClassExt = {
    WasmArrayObject::obj_moved, /* objectMovedOp */
};
const JSClass WasmArrayObject::class_ = {
    "WasmArrayObject",
    JSClass::NON_NATIVE | JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_BACKGROUND_FINALIZE | JSCLASS_SKIP_NURSERY_FINALIZE,
    &WasmArrayObjectClassOps,
    JS_NULL_CLASS_SPEC,
    &WasmArrayObjectClassExt,
    &WasmGcObject::objectOps_,
};

//=========================================================================
// WasmStructObject

/* static */
const JSClass* js::WasmStructObject::classForTypeDef(
    const wasm::TypeDef* typeDef) {
  MOZ_ASSERT(typeDef->kind() == wasm::TypeDefKind::Struct);
  size_t nbytes = typeDef->structType().size_;
  return nbytes > WasmStructObject_MaxInlineBytes
             ? &WasmStructObject::classOutline_
             : &WasmStructObject::classInline_;
}

/* static */
js::gc::AllocKind js::WasmStructObject::allocKindForTypeDef(
    const wasm::TypeDef* typeDef) {
  MOZ_ASSERT(typeDef->kind() == wasm::TypeDefKind::Struct);
  size_t nbytes = typeDef->structType().size_;

  // `nbytes` is the total required size for all struct fields, including
  // padding.  What we need is the size of resulting WasmStructObject,
  // ignoring any space used for out-of-line data.  First, restrict `nbytes`
  // to cover just the inline data.
  if (nbytes > WasmStructObject_MaxInlineBytes) {
    nbytes = WasmStructObject_MaxInlineBytes;
  }

  // Now convert it to size of the WasmStructObject as a whole.
  nbytes = sizeOfIncludingInlineData(nbytes);

  return gc::GetGCObjectKindForBytes(nbytes);
}

bool WasmStructObject::getField(JSContext* cx, uint32_t index,
                                MutableHandle<Value> val) {
  const StructType& resultType = typeDef().structType();
  MOZ_ASSERT(index <= resultType.fields_.length());
  const FieldType& field = resultType.fields_[index];
  uint32_t fieldOffset = resultType.fieldOffset(index);
  StorageType ty = field.type.storageType();
  return ToJSValue(cx, fieldOffsetToAddress(ty, fieldOffset), ty, val);
}

/* static */
void WasmStructObject::obj_trace(JSTracer* trc, JSObject* object) {
  WasmStructObject& structObj = object->as<WasmStructObject>();

  const auto& structType = structObj.typeDef().structType();
  for (uint32_t offset : structType.inlineTraceOffsets_) {
    AnyRef* fieldPtr =
        reinterpret_cast<AnyRef*>(structObj.inlineData() + offset);
    TraceManuallyBarrieredEdge(trc, fieldPtr, "wasm-struct-field");
  }
  for (uint32_t offset : structType.outlineTraceOffsets_) {
    AnyRef* fieldPtr =
        reinterpret_cast<AnyRef*>(structObj.outlineData_ + offset);
    TraceManuallyBarrieredEdge(trc, fieldPtr, "wasm-struct-field");
  }
}

/* static */
void WasmStructObject::obj_finalize(JS::GCContext* gcx, JSObject* object) {
  // See corresponding comments in WasmArrayObject::obj_finalize.
  WasmStructObject& structObj = object->as<WasmStructObject>();
  if (structObj.outlineData_) {
    js_free(structObj.outlineData_);
    const TypeDef& typeDef = structObj.typeDef();
    MOZ_ASSERT(typeDef.isStructType());
    uint32_t totalBytes = typeDef.structType().size_;
    uint32_t inlineBytes, outlineBytes;
    WasmStructObject::getDataByteSizes(totalBytes, &inlineBytes, &outlineBytes);
    MOZ_ASSERT(inlineBytes == WasmStructObject_MaxInlineBytes);
    MOZ_ASSERT(outlineBytes > 0);
    gcx->removeCellMemory(&structObj, outlineBytes + TrailerBlockOverhead,
                          MemoryUse::WasmTrailerBlock);
    structObj.outlineData_ = nullptr;
  }
}

/* static */
size_t WasmStructObject::obj_moved(JSObject* obj, JSObject* old) {
  // See also, corresponding comments in WasmArrayObject::obj_moved.
  if (IsInsideNursery(old)) {
    Nursery& nursery = obj->runtimeFromMainThread()->gc.nursery();
    WasmStructObject& structObj = obj->as<WasmStructObject>();
    const TypeDef& typeDef = structObj.typeDef();
    MOZ_ASSERT(typeDef.isStructType());
    uint32_t totalBytes = typeDef.structType().size_;
    uint32_t inlineBytes, outlineBytes;
    WasmStructObject::getDataByteSizes(totalBytes, &inlineBytes, &outlineBytes);
    MOZ_ASSERT(inlineBytes == WasmStructObject_MaxInlineBytes);
    MOZ_ASSERT(outlineBytes > 0);
    nursery.trackTrailerOnPromotion(structObj.outlineData_, obj, outlineBytes,
                                    TrailerBlockOverhead,
                                    MemoryUse::WasmTrailerBlock);
  }

  return 0;
}

void WasmStructObject::storeVal(const Val& val, uint32_t fieldIndex) {
  const StructType& structType = typeDef().structType();
  StorageType fieldType = structType.fields_[fieldIndex].type;
  uint32_t fieldOffset = structType.fieldOffset(fieldIndex);

  MOZ_ASSERT(fieldIndex < structType.fields_.length());
  bool areaIsOutline;
  uint32_t areaOffset;
  fieldOffsetToAreaAndOffset(fieldType, fieldOffset, &areaIsOutline,
                             &areaOffset);

  uint8_t* data;
  if (areaIsOutline) {
    data = outlineData_ + areaOffset;
  } else {
    data = inlineData() + areaOffset;
  }

  WriteValTo(val, fieldType, data);
}

static const JSClassOps WasmStructObjectOutlineClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate   */
    WasmGcObject::obj_newEnumerate,
    nullptr,                        /* resolve     */
    nullptr,                        /* mayResolve  */
    WasmStructObject::obj_finalize, /* finalize    */
    nullptr,                        /* call        */
    nullptr,                        /* construct   */
    WasmStructObject::obj_trace,
};
static const ClassExtension WasmStructObjectOutlineClassExt = {
    WasmStructObject::obj_moved, /* objectMovedOp */
};
const JSClass WasmStructObject::classOutline_ = {
    "WasmStructObject",
    JSClass::NON_NATIVE | JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_BACKGROUND_FINALIZE | JSCLASS_SKIP_NURSERY_FINALIZE,
    &WasmStructObjectOutlineClassOps,
    JS_NULL_CLASS_SPEC,
    &WasmStructObjectOutlineClassExt,
    &WasmGcObject::objectOps_,
};

// Structs that only have inline data get a different class without a
// finalizer. This class should otherwise be identical to the class for
// structs with outline data.
static const JSClassOps WasmStructObjectInlineClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate   */
    WasmGcObject::obj_newEnumerate,
    nullptr, /* resolve     */
    nullptr, /* mayResolve  */
    nullptr, /* finalize    */
    nullptr, /* call        */
    nullptr, /* construct   */
    WasmStructObject::obj_trace,
};
static const ClassExtension WasmStructObjectInlineClassExt = {
    nullptr, /* objectMovedOp */
};
const JSClass WasmStructObject::classInline_ = {
    "WasmStructObject",
    JSClass::NON_NATIVE | JSCLASS_DELAY_METADATA_BUILDER,
    &WasmStructObjectInlineClassOps,
    JS_NULL_CLASS_SPEC,
    &WasmStructObjectInlineClassExt,
    &WasmGcObject::objectOps_,
};
