/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_WasmGcObject_h
#define wasm_WasmGcObject_h

#include "mozilla/Attributes.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"

#include "gc/GCProbes.h"
#include "gc/Pretenuring.h"
#include "gc/ZoneAllocator.h"  // AddCellMemory
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/Probes.h"
#include "wasm/WasmInstanceData.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"

using js::wasm::StorageType;
using mozilla::CheckedUint32;

namespace js::wasm {

// For trailer blocks whose owning Wasm{Struct,Array}Objects make it into the
// tenured heap, we have to tell the tenured heap how big those trailers are
// in order to get major GCs to happen sufficiently frequently.  In an attempt
// to make the numbers more accurate, for each block we overstate the size by
// the following amount, on the assumption that:
//
// * mozjemalloc has an overhead of at least one word per block
//
// * the malloc-cache mechanism rounds up small block sizes to the nearest 16;
//   hence the average increase is 16 / 2.
static const size_t TrailerBlockOverhead = (16 / 2) + (1 * sizeof(void*));

}  // namespace js::wasm

namespace js {

//=========================================================================
// WasmGcObject

class WasmGcObject : public JSObject {
 protected:
  const wasm::SuperTypeVector* superTypeVector_;

  static const ObjectOps objectOps_;

  [[nodiscard]] static bool obj_lookupProperty(JSContext* cx, HandleObject obj,
                                               HandleId id,
                                               MutableHandleObject objp,
                                               PropertyResult* propp);

  [[nodiscard]] static bool obj_defineProperty(JSContext* cx, HandleObject obj,
                                               HandleId id,
                                               Handle<PropertyDescriptor> desc,
                                               ObjectOpResult& result);

  [[nodiscard]] static bool obj_hasProperty(JSContext* cx, HandleObject obj,
                                            HandleId id, bool* foundp);

  [[nodiscard]] static bool obj_getProperty(JSContext* cx, HandleObject obj,
                                            HandleValue receiver, HandleId id,
                                            MutableHandleValue vp);

  [[nodiscard]] static bool obj_setProperty(JSContext* cx, HandleObject obj,
                                            HandleId id, HandleValue v,
                                            HandleValue receiver,
                                            ObjectOpResult& result);

  [[nodiscard]] static bool obj_getOwnPropertyDescriptor(
      JSContext* cx, HandleObject obj, HandleId id,
      MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc);

  [[nodiscard]] static bool obj_deleteProperty(JSContext* cx, HandleObject obj,
                                               HandleId id,
                                               ObjectOpResult& result);

  // PropOffset is a uint32_t that is used to carry information about the
  // location of an value from WasmGcObject::lookupProperty to
  // WasmGcObject::loadValue.  It is distinct from a normal uint32_t to
  // emphasise the fact that it cannot be interpreted as an offset in any
  // single contiguous area of memory:
  //
  // * If the object in question is a WasmStructObject, it is the value of
  //   `wasm::StructType::fieldOffset()` for the relevant field, without regard
  //   to the inline/outline split.
  //
  // * If the object in question is a WasmArrayObject, then
  //   - u32 == UINT32_MAX (0xFFFF'FFFF) means the "length" property
  //     is requested
  //   - u32 < UINT32_MAX means the array element starting at that byte
  //     offset in WasmArrayObject::data_.  It is not an array index value.
  //   See WasmGcObject::lookupProperty for details.
  class PropOffset {
    uint32_t u32_;

   public:
    PropOffset() : u32_(0) {}
    uint32_t get() const { return u32_; }
    void set(uint32_t u32) { u32_ = u32; }
  };

  [[nodiscard]] static bool lookUpProperty(JSContext* cx,
                                           Handle<WasmGcObject*> obj, jsid id,
                                           PropOffset* offset,
                                           StorageType* type);

 public:
  [[nodiscard]] static bool loadValue(JSContext* cx, Handle<WasmGcObject*> obj,
                                      jsid id, MutableHandleValue vp);

  const wasm::SuperTypeVector& superTypeVector() const {
    return *superTypeVector_;
  }

  static constexpr size_t offsetOfSuperTypeVector() {
    return offsetof(WasmGcObject, superTypeVector_);
  }

  // These are both expensive in that they involve a double indirection.
  // Avoid them if possible.
  const wasm::TypeDef& typeDef() const { return *superTypeVector().typeDef(); }
  wasm::TypeDefKind kind() const { return superTypeVector().typeDef()->kind(); }

  [[nodiscard]] bool isRuntimeSubtypeOf(
      const wasm::TypeDef* parentTypeDef) const;

  [[nodiscard]] static bool obj_newEnumerate(JSContext* cx, HandleObject obj,
                                             MutableHandleIdVector properties,
                                             bool enumerableOnly);
};

//=========================================================================
// WasmArrayObject

// Class for a wasm array. It contains a pointer to the array contents and
// possibly inline data. Array data is allocated with a DataHeader that tracks
// whether the array data is stored inline in a trailing array, or out of line
// in heap memory. The array's data pointer will always point at the start of
// the array data, and the data header can always be read by subtracting
// sizeof(DataHeader).
class WasmArrayObject : public WasmGcObject,
                        public TrailingArray<WasmArrayObject> {
 public:
  static const JSClass class_;

  // The number of elements in the array.
  uint32_t numElements_;

  // Owned data pointer, holding `numElements_` entries. This may point to
  // `inlineStorage` or to an externally-allocated block of memory. It points
  // to the start of the array data, after the data header.
  //
  // This pointer is never null. An empty array will be stored like any other
  // inline-storage array.
  uint8_t* data_;

  // The inline (wasm-array-level) data fields, stored as a trailing array. We
  // request this field to begin at an 8-aligned offset relative to the start of
  // the object, so as to guarantee that `double` typed fields are not subject
  // to misaligned-access penalties on any target, whilst wasting at maximum 4
  // bytes of space. (v128 fields are possible, but we have opted to favor
  // slightly smaller objects over requiring a 16-byte alignment.)
  //
  // If used, the inline storage area will begin with the data header, followed
  // by the actual array data. See the main comment on WasmArrayObject.
  //
  // Remember that `inlineStorage` is in reality a variable length block with
  // maximum size WasmArrayObject_MaxInlineBytes bytes.  Do not add any
  // (C++-level) fields after this point!
  uint8_t* inlineStorage() {
    return offsetToPointer<uint8_t>(offsetOfInlineStorage());
  }

  // This tells us how big the object is if we know the number of inline bytes
  // it was created with.
  static inline constexpr size_t sizeOfIncludingInlineStorage(
      size_t sizeOfInlineStorage) {
    size_t n = sizeof(WasmArrayObject) + sizeOfInlineStorage;
    MOZ_ASSERT(n <= JSObject::MAX_BYTE_SIZE);
    return n;
  }

  // This tells us how big the object is if we know the number of inline bytes
  // it was created with.
  static inline constexpr size_t sizeOfIncludingInlineData(
      size_t sizeOfInlineData) {
    size_t n = sizeof(WasmArrayObject) + sizeOfInlineData;
    MOZ_ASSERT(n <= JSObject::MAX_BYTE_SIZE);
    return n;
  }

  // AllocKind for object creation
  static inline gc::AllocKind allocKindForOOL();
  static inline gc::AllocKind allocKindForIL(uint32_t storageBytes);
  inline gc::AllocKind allocKind() const;

  // Calculate the byte length of the array's data storage, being careful to
  // check for overflow. This includes the data header, data, and any extra
  // space for alignment with GC sizes. Note this logic assumes that
  // MaxArrayPayloadBytes is within uint32_t range.
  //
  // This logic is mirrored in WasmArrayObject::maxInlineElementsForElemSize and
  // MacroAssembler::wasmNewArrayObject.
  static CheckedUint32 calcStorageBytesChecked(uint32_t elemSize,
                                               uint32_t numElements) {
    static_assert(sizeof(WasmArrayObject) % gc::CellAlignBytes == 0);
    CheckedUint32 storageBytes = elemSize;
    storageBytes *= numElements;
    storageBytes += sizeof(WasmArrayObject::DataHeader);
    // Round total allocation up to gc::CellAlignBytes
    storageBytes -= 1;
    storageBytes += gc::CellAlignBytes - (storageBytes % gc::CellAlignBytes);
    return storageBytes;
  }
  static uint32_t calcStorageBytes(uint32_t elemSize, uint32_t numElements) {
    CheckedUint32 storageBytes = calcStorageBytesChecked(elemSize, numElements);
    MOZ_ASSERT(storageBytes.isValid());
    return storageBytes.value();
  }
  // Compute the maximum number of elements that can be stored inline for the
  // given element size.
  static inline uint32_t maxInlineElementsForElemSize(uint32_t elemSize);

  using DataHeader = uintptr_t;
  static const DataHeader DataIsIL = 0;
  static const DataHeader DataIsOOL = 1;

  // Creates a new array object with out-of-line storage. Reports an error on
  // OOM. The element type, shape, class pointer, alloc site and alloc kind are
  // taken from `typeDefData`; the initial heap must be specified separately.
  // The size of storage is debug-asserted to be larger than
  // WasmArrayObject_MaxInlineBytes - generally, C++ code should use
  // WasmArrayObject::createArray.
  template <bool ZeroFields>
  static MOZ_ALWAYS_INLINE WasmArrayObject* createArrayOOL(
      JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
      js::gc::Heap initialHeap, uint32_t numElements, uint32_t storageBytes);

  // Creates a new array object with inline storage. Reports an error on OOM.
  // The element type, shape, class pointer, alloc site and alloc kind are taken
  // from `typeDefData`; the initial heap must be specified separately. The size
  // of storage is debug-asserted to be within WasmArrayObject_MaxInlineBytes -
  // generally, C++ code should use WasmArrayObject::createArray.
  template <bool ZeroFields>
  static MOZ_ALWAYS_INLINE WasmArrayObject* createArrayIL(
      JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
      js::gc::Heap initialHeap, uint32_t numElements, uint32_t storageBytes);

  // This selects one of the above two routines, depending on how much storage
  // is required for the given type and number of elements.
  template <bool ZeroFields>
  static MOZ_ALWAYS_INLINE WasmArrayObject* createArray(
      JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
      js::gc::Heap initialHeap, uint32_t numElements);

  // JIT accessors
  static constexpr size_t offsetOfNumElements() {
    return offsetof(WasmArrayObject, numElements_);
  }
  static constexpr size_t offsetOfData() {
    return offsetof(WasmArrayObject, data_);
  }
  static const uint32_t inlineStorageAlignment = 8;
  static constexpr size_t offsetOfInlineStorage() {
    return AlignBytes(sizeof(WasmArrayObject), inlineStorageAlignment);
  }
  static constexpr size_t offsetOfInlineArrayData() {
    return offsetOfInlineStorage() + sizeof(DataHeader);
  }

  // Tracing and finalization
  static void obj_trace(JSTracer* trc, JSObject* object);
  static void obj_finalize(JS::GCContext* gcx, JSObject* object);
  static size_t obj_moved(JSObject* obj, JSObject* old);

  void storeVal(const wasm::Val& val, uint32_t itemIndex);
  void fillVal(const wasm::Val& val, uint32_t itemIndex, uint32_t len);

  static DataHeader* dataHeaderFromDataPointer(const uint8_t* data) {
    MOZ_ASSERT(data);
    return (DataHeader*)data - 1;
  }
  DataHeader* dataHeader() const {
    return WasmArrayObject::dataHeaderFromDataPointer(data_);
  }

  static bool isDataInline(uint8_t* data) {
    const DataHeader* header = dataHeaderFromDataPointer(data);
    MOZ_ASSERT(*header == DataIsIL || *header == DataIsOOL);
    return *header == DataIsIL;
  }
  bool isDataInline() const { return WasmArrayObject::isDataInline(data_); }

  static WasmArrayObject* fromInlineDataPointer(uint8_t* data) {
    MOZ_ASSERT(isDataInline(data));
    return (WasmArrayObject*)(data -
                              WasmArrayObject::offsetOfInlineArrayData());
  }

  static DataHeader* addressOfInlineDataHeader(WasmArrayObject* base) {
    return base->offsetToPointer<DataHeader>(offsetOfInlineStorage());
  }
  static uint8_t* addressOfInlineData(WasmArrayObject* base) {
    return base->offsetToPointer<uint8_t>(offsetOfInlineArrayData());
  }
};

static_assert((WasmArrayObject::offsetOfInlineStorage() % 8) == 0);

// Helper to mark all locations that assume that the type of
// WasmArrayObject::numElements is uint32_t.
#define STATIC_ASSERT_WASMARRAYELEMENTS_NUMELEMENTS_IS_U32 \
  static_assert(sizeof(js::WasmArrayObject::numElements_) == sizeof(uint32_t))

//=========================================================================
// WasmStructObject

// Class for a wasm struct.  It has inline data and, if the inline area is
// insufficient, a pointer to outline data that lives in the C++ heap.
// Computing the field offsets is somewhat tricky; see block comment on `class
// StructLayout` for background.

class WasmStructObject : public WasmGcObject,
                         public TrailingArray<WasmStructObject> {
 public:
  static const JSClass classInline_;
  static const JSClass classOutline_;

  // Owned pointer to a malloc'd block containing out-of-line fields, or
  // nullptr if none.  Note that MIR alias analysis assumes this is readonly
  // for the life of the object; do not change it once the object is created.
  // See MWasmLoadObjectField::congruentTo.
  uint8_t* outlineData_;

  // The inline (wasm-struct-level) data fields, stored as a trailing array.
  // This must be a multiple of 16 bytes long in order to ensure that no field
  // gets split across the inline-outline boundary.  As a refinement, we request
  // this field to begin at an 8-aligned offset relative to the start of the
  // object, so as to guarantee that `double` typed fields are not subject to
  // misaligned-access penalties on any target, whilst wasting at maximum 4
  // bytes of space.
  //
  // Remember that `inlineData` is in reality a variable length block with
  // maximum size WasmStructObject_MaxInlineBytes bytes.  Do not add any
  // (C++-level) fields after this point!
  uint8_t* inlineData() {
    return offsetToPointer<uint8_t>(offsetOfInlineData());
  }

  // This tells us how big the object is if we know the number of inline bytes
  // it was created with.
  static inline constexpr size_t sizeOfIncludingInlineData(
      size_t sizeOfInlineData) {
    size_t n = sizeof(WasmStructObject) + sizeOfInlineData;
    MOZ_ASSERT(n <= JSObject::MAX_BYTE_SIZE);
    return n;
  }

  static const JSClass* classForTypeDef(const wasm::TypeDef* typeDef);
  static js::gc::AllocKind allocKindForTypeDef(const wasm::TypeDef* typeDef);

  // Creates a new struct typed object, optionally initialized to zero.
  // Reports if there is an out of memory error.  The structure's type, shape,
  // class pointer, alloc site and alloc kind are taken from `typeDefData`;
  // the initial heap must be specified separately.  It is assumed and debug-
  // asserted that `typeDefData` refers to a type that does not need OOL
  // storage.
  template <bool ZeroFields>
  static MOZ_ALWAYS_INLINE WasmStructObject* createStructIL(
      JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
      js::gc::Heap initialHeap);

  // Same as ::createStructIL, except it is assumed and debug-asserted that
  // `typeDefData` refers to a type that does need OOL storage.
  template <bool ZeroFields>
  static MOZ_ALWAYS_INLINE WasmStructObject* createStructOOL(
      JSContext* cx, wasm::TypeDefInstanceData* typeDefData,
      js::gc::Heap initialHeap);

  // Given the total number of data bytes required (including alignment
  // holes), return the number of inline and outline bytes required.
  static inline void getDataByteSizes(uint32_t totalBytes,
                                      uint32_t* inlineBytes,
                                      uint32_t* outlineBytes);

  // Convenience function; returns true iff ::getDataByteSizes would set
  // *outlineBytes to a non-zero value.
  static inline bool requiresOutlineBytes(uint32_t totalBytes);

  // Given the offset of a field, produce the offset in `inlineData` or
  // `*outlineData_` to use, plus a bool indicating which area it is.
  // `fieldType` is for assertional purposes only.
  static inline void fieldOffsetToAreaAndOffset(StorageType fieldType,
                                                uint32_t fieldOffset,
                                                bool* areaIsOutline,
                                                uint32_t* areaOffset);

  // Given the offset of a field, return its actual address.  `fieldType` is
  // for assertional purposes only.
  inline uint8_t* fieldOffsetToAddress(StorageType fieldType,
                                       uint32_t fieldOffset);

  // Gets JS Value of the structure field.
  bool getField(JSContext* cx, uint32_t index, MutableHandle<Value> val);

  // JIT accessors
  static const uint32_t inlineDataAlignment = 8;
  static constexpr size_t offsetOfOutlineData() {
    return offsetof(WasmStructObject, outlineData_);
  }
  static constexpr size_t offsetOfInlineData() {
    return AlignBytes(sizeof(WasmStructObject), inlineDataAlignment);
  }

  // Tracing and finalization
  static void obj_trace(JSTracer* trc, JSObject* object);
  static void obj_finalize(JS::GCContext* gcx, JSObject* object);
  static size_t obj_moved(JSObject* obj, JSObject* old);

  void storeVal(const wasm::Val& val, uint32_t fieldIndex);
};

static_assert((WasmStructObject::offsetOfInlineData() % 8) == 0);

// MaxInlineBytes must be a multiple of 16 for reasons described in the
// comment on `class StructLayout`.  This unfortunately can't be defined
// inside the class definition itself because the sizeof(..) expression isn't
// valid until after the end of the class definition.
const size_t WasmStructObject_MaxInlineBytes =
    ((JSObject::MAX_BYTE_SIZE - sizeof(WasmStructObject)) / 16) * 16;
const size_t WasmArrayObject_MaxInlineBytes =
    ((JSObject::MAX_BYTE_SIZE - sizeof(WasmArrayObject)) / 16) * 16;

static_assert((WasmStructObject_MaxInlineBytes % 16) == 0);
static_assert((WasmArrayObject_MaxInlineBytes % 16) == 0);

/* static */
inline uint32_t WasmArrayObject::maxInlineElementsForElemSize(
    uint32_t elemSize) {
  // This implementation inverts the logic of WasmArrayObject::calcStorageBytes
  // to compute numElements.
  MOZ_RELEASE_ASSERT(elemSize > 0);
  uint32_t result = WasmArrayObject_MaxInlineBytes;
  static_assert(WasmArrayObject_MaxInlineBytes % gc::CellAlignBytes == 0);
  result -= sizeof(WasmArrayObject::DataHeader);
  result /= elemSize;

  MOZ_RELEASE_ASSERT(calcStorageBytesChecked(elemSize, result).isValid());
  return result;
}

/*static*/
inline void WasmStructObject::getDataByteSizes(uint32_t totalBytes,
                                               uint32_t* inlineBytes,
                                               uint32_t* outlineBytes) {
  if (MOZ_UNLIKELY(totalBytes > WasmStructObject_MaxInlineBytes)) {
    *inlineBytes = WasmStructObject_MaxInlineBytes;
    *outlineBytes = totalBytes - WasmStructObject_MaxInlineBytes;
  } else {
    *inlineBytes = totalBytes;
    *outlineBytes = 0;
  }
}

/* static */
inline bool WasmStructObject::requiresOutlineBytes(uint32_t totalBytes) {
  uint32_t inlineBytes, outlineBytes;
  WasmStructObject::getDataByteSizes(totalBytes, &inlineBytes, &outlineBytes);
  return outlineBytes > 0;
}

/*static*/
inline void WasmStructObject::fieldOffsetToAreaAndOffset(StorageType fieldType,
                                                         uint32_t fieldOffset,
                                                         bool* areaIsOutline,
                                                         uint32_t* areaOffset) {
  if (fieldOffset < WasmStructObject_MaxInlineBytes) {
    *areaIsOutline = false;
    *areaOffset = fieldOffset;
  } else {
    *areaIsOutline = true;
    *areaOffset = fieldOffset - WasmStructObject_MaxInlineBytes;
  }
  // Assert that the first and last bytes for the field agree on which side of
  // the inline/outline boundary they live.
  MOZ_RELEASE_ASSERT(
      (fieldOffset < WasmStructObject_MaxInlineBytes) ==
      ((fieldOffset + fieldType.size() - 1) < WasmStructObject_MaxInlineBytes));
}

inline uint8_t* WasmStructObject::fieldOffsetToAddress(StorageType fieldType,
                                                       uint32_t fieldOffset) {
  bool areaIsOutline;
  uint32_t areaOffset;
  fieldOffsetToAreaAndOffset(fieldType, fieldOffset, &areaIsOutline,
                             &areaOffset);
  return (areaIsOutline ? outlineData_ : inlineData()) + areaOffset;
}

// Ensure that faulting loads/stores for WasmStructObject and WasmArrayObject
// are in the NULL pointer guard page.
static_assert(WasmStructObject_MaxInlineBytes <= wasm::NullPtrGuardSize);
static_assert(sizeof(WasmArrayObject) <= wasm::NullPtrGuardSize);

}  // namespace js

//=========================================================================
// misc

namespace js {

inline bool IsWasmGcObjectClass(const JSClass* class_) {
  return class_ == &WasmArrayObject::class_ ||
         class_ == &WasmStructObject::classInline_ ||
         class_ == &WasmStructObject::classOutline_;
}

}  // namespace js

template <>
inline bool JSObject::is<js::WasmGcObject>() const {
  return js::IsWasmGcObjectClass(getClass());
}

template <>
inline bool JSObject::is<js::WasmStructObject>() const {
  const JSClass* class_ = getClass();
  return class_ == &js::WasmStructObject::classInline_ ||
         class_ == &js::WasmStructObject::classOutline_;
}

#endif /* wasm_WasmGcObject_h */
