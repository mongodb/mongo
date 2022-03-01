/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayBufferObject_h
#define vm_ArrayBufferObject_h

#include "mozilla/Maybe.h"

#include <tuple>  // std::tuple

#include "builtin/TypedArrayConstants.h"
#include "gc/Memory.h"
#include "gc/ZoneAllocator.h"
#include "js/ArrayBuffer.h"
#include "js/GCHashTable.h"
#include "vm/JSObject.h"
#include "vm/Runtime.h"
#include "vm/SharedMem.h"
#include "wasm/WasmPages.h"

namespace js {

class ArrayBufferViewObject;
class WasmArrayRawBuffer;

namespace wasm {
struct MemoryDesc;
}  // namespace wasm

// Create a new mapping of size `mappedSize` with an initially committed prefix
// of size `initialCommittedSize`.  Both arguments denote bytes and must be
// multiples of the page size, with `initialCommittedSize` <= `mappedSize`.
// Returns nullptr on failure.
void* MapBufferMemory(size_t mappedSize, size_t initialCommittedSize);

// Commit additional memory in an existing mapping.  `dataEnd` must be the
// correct value for the end of the existing committed area, and `delta` must be
// a byte amount to grow the mapping by, and must be a multiple of the page
// size.  Returns false on failure.
bool CommitBufferMemory(void* dataEnd, size_t delta);

// Extend an existing mapping by adding uncommited pages to it.  `dataStart`
// must be the pointer to the start of the existing mapping, `mappedSize` the
// size of the existing mapping, and `newMappedSize` the size of the extended
// mapping (sizes in bytes), with `mappedSize` <= `newMappedSize`.  Both sizes
// must be divisible by the page size.  Returns false on failure.
bool ExtendBufferMapping(void* dataStart, size_t mappedSize,
                         size_t newMappedSize);

// Remove an existing mapping.  `dataStart` must be the pointer to the start of
// the mapping, and `mappedSize` the size of that mapping.
void UnmapBufferMemory(void* dataStart, size_t mappedSize);

// Return the number of currently live mapped buffers.
int32_t LiveMappedBufferCount();

// The inheritance hierarchy for the various classes relating to typed arrays
// is as follows.
//
//
// - JSObject
//   - TypedObject (declared in wasm/TypedObject.h)
//   - NativeObject
//     - ArrayBufferObjectMaybeShared
//       - ArrayBufferObject
//       - SharedArrayBufferObject
//     - ArrayBufferViewObject
//       - DataViewObject
//       - TypedArrayObject (declared in vm/TypedArrayObject.h)
//         - TypedArrayObjectTemplate
//           - Int8ArrayObject
//           - Uint8ArrayObject
//           - ...
//
// Note that |TypedArrayObjectTemplate| is just an implementation
// detail that makes implementing its various subclasses easier.
//
// ArrayBufferObject and SharedArrayBufferObject are unrelated data types:
// the racy memory of the latter cannot substitute for the non-racy memory of
// the former; the non-racy memory of the former cannot be used with the
// atomics; the former can be detached and the latter not.  Hence they have been
// separated completely.
//
// Most APIs will only accept ArrayBufferObject.  ArrayBufferObjectMaybeShared
// exists as a join point to allow APIs that can take or use either, notably
// AsmJS.
//
// In contrast with the separation of ArrayBufferObject and
// SharedArrayBufferObject, the TypedArray types can map either.
//
// The possible data ownership and reference relationships with ArrayBuffers
// and related classes are enumerated below. These are the possible locations
// for typed data:
//
// (1) malloc'ed or mmap'ed data owned by an ArrayBufferObject.
// (2) Data allocated inline with an ArrayBufferObject.
// (3) Data allocated inline with a TypedArrayObject.
// (4) Data allocated inline with an InlineTypedObject.
//
// An ArrayBufferObject may point to any of these sources of data, except (3).
// All array buffer views may point to any of these sources of data, except
// that (3) may only be pointed to by the typed array the data is inline with.
//
// During a minor GC, (3) and (4) may move. During a compacting GC, (2), (3),
// and (4) may move.

class ArrayBufferObjectMaybeShared;

wasm::Pages WasmArrayBufferPages(const ArrayBufferObjectMaybeShared* buf);
mozilla::Maybe<wasm::Pages> WasmArrayBufferMaxPages(
    const ArrayBufferObjectMaybeShared* buf);
size_t WasmArrayBufferMappedSize(const ArrayBufferObjectMaybeShared* buf);

class ArrayBufferObjectMaybeShared : public NativeObject {
 public:
  inline size_t byteLength() const;
  inline bool isDetached() const;
  inline SharedMem<uint8_t*> dataPointerEither();

  // WebAssembly support:
  // Note: the eventual goal is to remove this from ArrayBuffer and have
  // (Shared)ArrayBuffers alias memory owned by some wasm::Memory object.

  wasm::Pages wasmPages() const { return WasmArrayBufferPages(this); }
  mozilla::Maybe<wasm::Pages> wasmMaxPages() const {
    return WasmArrayBufferMaxPages(this);
  }
  size_t wasmMappedSize() const { return WasmArrayBufferMappedSize(this); }

  inline bool isPreparedForAsmJS() const;
  inline bool isWasm() const;
};

using RootedArrayBufferObjectMaybeShared =
    Rooted<ArrayBufferObjectMaybeShared*>;
using HandleArrayBufferObjectMaybeShared =
    Handle<ArrayBufferObjectMaybeShared*>;
using MutableHandleArrayBufferObjectMaybeShared =
    MutableHandle<ArrayBufferObjectMaybeShared*>;

/*
 * ArrayBufferObject
 *
 * This class holds the underlying raw buffer that the various ArrayBufferViews
 * (eg DataViewObject, the TypedArrays, TypedObjects) access. It can be created
 * explicitly and used to construct an ArrayBufferView, or can be created
 * lazily when it is first accessed for a TypedArrayObject or TypedObject that
 * doesn't have an explicit buffer.
 *
 * ArrayBufferObject (or really the underlying memory) /is not racy/: the
 * memory is private to a single worker.
 */
class ArrayBufferObject : public ArrayBufferObjectMaybeShared {
  static bool byteLengthGetterImpl(JSContext* cx, const CallArgs& args);

 public:
  static const uint8_t DATA_SLOT = 0;
  static const uint8_t BYTE_LENGTH_SLOT = 1;
  static const uint8_t FIRST_VIEW_SLOT = 2;
  static const uint8_t FLAGS_SLOT = 3;

  static const uint8_t RESERVED_SLOTS = 4;

  static const size_t ARRAY_BUFFER_ALIGNMENT = 8;

  static_assert(FLAGS_SLOT == JS_ARRAYBUFFER_FLAGS_SLOT,
                "self-hosted code with burned-in constants must get the "
                "right flags slot");

  static bool supportLargeBuffers;

  static constexpr size_t MaxByteLengthForSmallBuffer = INT32_MAX;

  // The length of an ArrayBuffer or SharedArrayBuffer can be at most
  // INT32_MAX. Allow a larger limit on friendly 64-bit platforms if the
  // experimental large-buffers flag is used.
  static size_t maxBufferByteLength() {
#ifdef JS_64BIT
#  ifdef JS_CODEGEN_MIPS64
    // Fallthrough to the "small" case because there's no evidence that the
    // platform code can handle buffers > 2GB.
#  else
    if (supportLargeBuffers) {
      return size_t(8) * 1024 * 1024 * 1024;  // 8 GB.
    }
#  endif
#endif
    return MaxByteLengthForSmallBuffer;
  }

  /** The largest number of bytes that can be stored inline. */
  static constexpr size_t MaxInlineBytes =
      (NativeObject::MAX_FIXED_SLOTS - RESERVED_SLOTS) * sizeof(JS::Value);

 public:
  enum BufferKind {
    /** Inline data kept in the repurposed slots of this ArrayBufferObject. */
    INLINE_DATA = 0b000,

    /* Data allocated using the SpiderMonkey allocator. */
    MALLOCED = 0b001,

    /**
     * No bytes are associated with this buffer.  (This could be because the
     * buffer is detached, because it's an internal, newborn buffer not yet
     * overwritten with user-exposable semantics, or some other reason.  The
     * point is, don't read precise language semantics into this kind.)
     */
    NO_DATA = 0b010,

    /**
     * User-owned memory.  The associated buffer must be manually detached
     * before the user invalidates (deallocates, reuses the storage of, &c.)
     * the user-owned memory.
     */
    USER_OWNED = 0b011,

    WASM = 0b100,
    MAPPED = 0b101,
    EXTERNAL = 0b110,

    // These kind-values are currently invalid.  We intend to expand valid
    // BufferKinds in the future to either partly or fully use these values.
    BAD1 = 0b111,

    KIND_MASK = 0b111
  };

 public:
  enum ArrayBufferFlags {
    // The flags also store the BufferKind
    BUFFER_KIND_MASK = BufferKind::KIND_MASK,

    DETACHED = 0b1000,

    // This MALLOCED, MAPPED, or EXTERNAL buffer has been prepared for asm.js
    // and cannot henceforth be transferred/detached.  (WASM, USER_OWNED, and
    // INLINE_DATA buffers can't be prepared for asm.js -- although if an
    // INLINE_DATA buffer is used with asm.js, it's silently rewritten into a
    // MALLOCED buffer which *can* be prepared.)
    FOR_ASMJS = 0b10'0000,
  };

  static_assert(JS_ARRAYBUFFER_DETACHED_FLAG == DETACHED,
                "self-hosted code with burned-in constants must use the "
                "correct DETACHED bit value");

 protected:
  enum class FillContents { Zero, Uninitialized };

  template <FillContents FillType>
  static std::tuple<ArrayBufferObject*, uint8_t*> createBufferAndData(
      JSContext* cx, size_t nbytes, AutoSetNewObjectMetadata&,
      JS::Handle<JSObject*> proto = nullptr);

 public:
  class BufferContents {
    uint8_t* data_;
    BufferKind kind_;
    JS::BufferContentsFreeFunc free_;
    void* freeUserData_;

    friend class ArrayBufferObject;

    BufferContents(uint8_t* data, BufferKind kind,
                   JS::BufferContentsFreeFunc freeFunc = nullptr,
                   void* freeUserData = nullptr)
        : data_(data),
          kind_(kind),
          free_(freeFunc),
          freeUserData_(freeUserData) {
      MOZ_ASSERT((kind_ & ~KIND_MASK) == 0);
      MOZ_ASSERT_IF(free_ || freeUserData_, kind_ == EXTERNAL);

      // It is the caller's responsibility to ensure that the
      // BufferContents does not outlive the data.
    }

   public:
    static BufferContents createInlineData(void* data) {
      return BufferContents(static_cast<uint8_t*>(data), INLINE_DATA);
    }

    static BufferContents createMalloced(void* data) {
      return BufferContents(static_cast<uint8_t*>(data), MALLOCED);
    }

    static BufferContents createNoData() {
      return BufferContents(nullptr, NO_DATA);
    }

    static BufferContents createUserOwned(void* data) {
      return BufferContents(static_cast<uint8_t*>(data), USER_OWNED);
    }

    static BufferContents createWasm(void* data) {
      return BufferContents(static_cast<uint8_t*>(data), WASM);
    }

    static BufferContents createMapped(void* data) {
      return BufferContents(static_cast<uint8_t*>(data), MAPPED);
    }

    static BufferContents createExternal(void* data,
                                         JS::BufferContentsFreeFunc freeFunc,
                                         void* freeUserData = nullptr) {
      return BufferContents(static_cast<uint8_t*>(data), EXTERNAL, freeFunc,
                            freeUserData);
    }

    static BufferContents createFailed() {
      // There's no harm in tagging this as MALLOCED, even tho obviously it
      // isn't.  And adding an extra tag purely for this case is a complication
      // that presently appears avoidable.
      return BufferContents(nullptr, MALLOCED);
    }

    uint8_t* data() const { return data_; }
    BufferKind kind() const { return kind_; }
    JS::BufferContentsFreeFunc freeFunc() const { return free_; }
    void* freeUserData() const { return freeUserData_; }

    explicit operator bool() const { return data_ != nullptr; }
    WasmArrayRawBuffer* wasmBuffer() const;
  };

  static const JSClass class_;
  static const JSClass protoClass_;

  static bool byteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

  static bool fun_isView(JSContext* cx, unsigned argc, Value* vp);

  static bool class_constructor(JSContext* cx, unsigned argc, Value* vp);

  static bool isOriginalByteLengthGetter(Native native) {
    return native == byteLengthGetter;
  }

  static ArrayBufferObject* createForContents(JSContext* cx, size_t nbytes,
                                              BufferContents contents);

  static ArrayBufferObject* copy(
      JSContext* cx, JS::Handle<ArrayBufferObject*> unwrappedArrayBuffer);

  static ArrayBufferObject* createZeroed(JSContext* cx, size_t nbytes,
                                         HandleObject proto = nullptr);

  // Create an ArrayBufferObject that is safely finalizable and can later be
  // initialize()d to become a real, content-visible ArrayBufferObject.
  static ArrayBufferObject* createEmpty(JSContext* cx);

  // Create an ArrayBufferObject using the provided buffer and size.  Assumes
  // ownership of |buffer| even in case of failure, i.e. on failure |buffer|
  // is deallocated.
  static ArrayBufferObject* createFromNewRawBuffer(JSContext* cx,
                                                   WasmArrayRawBuffer* buffer,
                                                   size_t initialSize);

  static void copyData(Handle<ArrayBufferObject*> toBuffer, size_t toIndex,
                       Handle<ArrayBufferObject*> fromBuffer, size_t fromIndex,
                       size_t count);

  static size_t objectMoved(JSObject* obj, JSObject* old);

  static uint8_t* stealMallocedContents(JSContext* cx,
                                        Handle<ArrayBufferObject*> buffer);

  static BufferContents extractStructuredCloneContents(
      JSContext* cx, Handle<ArrayBufferObject*> buffer);

  static void addSizeOfExcludingThis(JSObject* obj,
                                     mozilla::MallocSizeOf mallocSizeOf,
                                     JS::ClassInfo* info);

  // ArrayBufferObjects (strongly) store the first view added to them, while
  // later views are (weakly) stored in the compartment's InnerViewTable
  // below. Buffers usually only have one view, so this slot optimizes for
  // the common case. Avoiding entries in the InnerViewTable saves memory and
  // non-incrementalized sweep time.
  JSObject* firstView();

  bool addView(JSContext* cx, ArrayBufferViewObject* view);

  // Detach this buffer from its original memory.  (This necessarily makes
  // views of this buffer unusable for modifying that original memory.)
  static void detach(JSContext* cx, Handle<ArrayBufferObject*> buffer);

  static constexpr size_t offsetOfByteLengthSlot() {
    return getFixedSlotOffset(BYTE_LENGTH_SLOT);
  }
  static constexpr size_t offsetOfFlagsSlot() {
    return getFixedSlotOffset(FLAGS_SLOT);
  }

 private:
  void setFirstView(ArrayBufferViewObject* view);

  uint8_t* inlineDataPointer() const;

  struct FreeInfo {
    JS::BufferContentsFreeFunc freeFunc;
    void* freeUserData;
  };
  FreeInfo* freeInfo() const;

 public:
  uint8_t* dataPointer() const;
  SharedMem<uint8_t*> dataPointerShared() const;
  size_t byteLength() const;

  BufferContents contents() const {
    if (isExternal()) {
      return BufferContents(dataPointer(), EXTERNAL, freeInfo()->freeFunc,
                            freeInfo()->freeUserData);
    }
    return BufferContents(dataPointer(), bufferKind());
  }
  bool hasInlineData() const { return dataPointer() == inlineDataPointer(); }

  void releaseData(JSFreeOp* fop);

  BufferKind bufferKind() const {
    return BufferKind(flags() & BUFFER_KIND_MASK);
  }

  bool isInlineData() const { return bufferKind() == INLINE_DATA; }
  bool isMalloced() const { return bufferKind() == MALLOCED; }
  bool isNoData() const { return bufferKind() == NO_DATA; }
  bool hasUserOwnedData() const { return bufferKind() == USER_OWNED; }

  bool isWasm() const { return bufferKind() == WASM; }
  bool isMapped() const { return bufferKind() == MAPPED; }
  bool isExternal() const { return bufferKind() == EXTERNAL; }

  bool isDetached() const { return flags() & DETACHED; }
  bool isPreparedForAsmJS() const { return flags() & FOR_ASMJS; }

  // WebAssembly support:

  /**
   * Prepare this ArrayBuffer for use with asm.js.  Returns true on success,
   * false on failure.  This function reports no errors.
   */
  [[nodiscard]] bool prepareForAsmJS();

  size_t wasmMappedSize() const;

  wasm::Pages wasmPages() const;
  mozilla::Maybe<wasm::Pages> wasmMaxPages() const;

  [[nodiscard]] static bool wasmGrowToPagesInPlace(
      wasm::Pages newPages, Handle<ArrayBufferObject*> oldBuf,
      MutableHandle<ArrayBufferObject*> newBuf, JSContext* cx);
  [[nodiscard]] static bool wasmMovingGrowToPages(
      wasm::Pages newPages, Handle<ArrayBufferObject*> oldBuf,
      MutableHandle<ArrayBufferObject*> newBuf, JSContext* cx);

  static void finalize(JSFreeOp* fop, JSObject* obj);

  static BufferContents createMappedContents(int fd, size_t offset,
                                             size_t length);

 protected:
  void setDataPointer(BufferContents contents);
  void setByteLength(size_t length);

  size_t associatedBytes() const;

  uint32_t flags() const;
  void setFlags(uint32_t flags);

  void setIsDetached() { setFlags(flags() | DETACHED); }
  void setIsPreparedForAsmJS() {
    MOZ_ASSERT(!isWasm());
    MOZ_ASSERT(!hasUserOwnedData());
    MOZ_ASSERT(!isInlineData());
    MOZ_ASSERT(isMalloced() || isMapped() || isExternal());
    setFlags(flags() | FOR_ASMJS);
  }

  void initialize(size_t byteLength, BufferContents contents) {
    setByteLength(byteLength);
    setFlags(0);
    setFirstView(nullptr);
    setDataPointer(contents);
  }

  void* initializeToInlineData(size_t byteLength) {
    void* data = inlineDataPointer();
    initialize(byteLength, BufferContents::createInlineData(data));
    return data;
  }
};

using RootedArrayBufferObject = Rooted<ArrayBufferObject*>;
using HandleArrayBufferObject = Handle<ArrayBufferObject*>;
using MutableHandleArrayBufferObject = MutableHandle<ArrayBufferObject*>;

// Create a buffer for a 32-bit wasm memory.
bool CreateWasmBuffer32(JSContext* cx, const wasm::MemoryDesc& memory,
                        MutableHandleArrayBufferObjectMaybeShared buffer);

// Per-compartment table that manages the relationship between array buffers
// and the views that use their storage.
class InnerViewTable {
 public:
  typedef Vector<JSObject*, 1, ZoneAllocPolicy> ViewVector;

  friend class ArrayBufferObject;

 private:
  struct MapGCPolicy {
    static bool needsSweep(JSObject** key, ViewVector* value) {
      return InnerViewTable::sweepEntry(key, *value);
    }
  };

  // This key is a raw pointer and not a WeakHeapPtr because the post-barrier
  // would hold nursery-allocated entries live unconditionally. It is a very
  // common pattern in low-level and performance-oriented JavaScript to create
  // hundreds or thousands of very short lived temporary views on a larger
  // buffer; having to tenured all of these would be a catastrophic performance
  // regression. Thus, it is vital that nursery pointers in this map not be held
  // live. Special support is required in the minor GC, implemented in
  // sweepAfterMinorGC.
  using Map = GCHashMap<JSObject*, ViewVector, MovableCellHasher<JSObject*>,
                        ZoneAllocPolicy, MapGCPolicy>;

  // For all objects sharing their storage with some other view, this maps
  // the object to the list of such views. All entries in this map are weak.
  Map map;

  // List of keys from innerViews where either the source or at least one
  // target is in the nursery. The raw pointer to a JSObject is allowed here
  // because this vector is cleared after every minor collection. Users in
  // sweepAfterMinorCollection must be careful to use MaybeForwarded before
  // touching these pointers.
  Vector<JSObject*, 0, SystemAllocPolicy> nurseryKeys;

  // Whether nurseryKeys is a complete list.
  bool nurseryKeysValid;

  // Sweep an entry during GC, returning whether the entry should be removed.
  static bool sweepEntry(JSObject** pkey, ViewVector& views);

  bool addView(JSContext* cx, ArrayBufferObject* buffer, JSObject* view);
  ViewVector* maybeViewsUnbarriered(ArrayBufferObject* obj);
  void removeViews(ArrayBufferObject* obj);

 public:
  explicit InnerViewTable(Zone* zone) : map(zone), nurseryKeysValid(true) {}

  // Remove references to dead objects in the table and update table entries
  // to reflect moved objects.
  void sweep();
  void sweepAfterMinorGC();

  bool needsSweep() const { return map.needsSweep(); }

  bool needsSweepAfterMinorGC() const {
    return !nurseryKeys.empty() || !nurseryKeysValid;
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);
};

template <typename Wrapper>
class MutableWrappedPtrOperations<InnerViewTable, Wrapper>
    : public WrappedPtrOperations<InnerViewTable, Wrapper> {
  InnerViewTable& table() { return static_cast<Wrapper*>(this)->get(); }

 public:
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
    return table().sizeOfExcludingThis(mallocSizeOf);
  }
};

class WasmArrayRawBuffer {
  mozilla::Maybe<wasm::Pages> maxPages_;
  size_t mappedSize_;  // Not including the header page
  size_t length_;

 protected:
  WasmArrayRawBuffer(uint8_t* buffer,
                     const mozilla::Maybe<wasm::Pages>& maxPages,
                     size_t mappedSize, size_t length)
      : maxPages_(maxPages), mappedSize_(mappedSize), length_(length) {
    MOZ_ASSERT(buffer == dataPointer());
  }

 public:
  static WasmArrayRawBuffer* AllocateWasm(
      wasm::Pages initialPages, const mozilla::Maybe<wasm::Pages>& maxPages,
      const mozilla::Maybe<size_t>& mappedSize);
  static void Release(void* mem);

  uint8_t* dataPointer() {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(this);
    return ptr + sizeof(WasmArrayRawBuffer);
  }

  static const WasmArrayRawBuffer* fromDataPtr(const uint8_t* dataPtr) {
    return reinterpret_cast<const WasmArrayRawBuffer*>(
        dataPtr - sizeof(WasmArrayRawBuffer));
  }

  uint8_t* basePointer() { return dataPointer() - gc::SystemPageSize(); }

  size_t mappedSize() const { return mappedSize_; }

  size_t byteLength() const { return length_; }

  wasm::Pages pages() const {
    return wasm::Pages::fromByteLengthExact(length_);
  }

  mozilla::Maybe<wasm::Pages> maxPages() const { return maxPages_; }

  [[nodiscard]] bool growToPagesInPlace(wasm::Pages newPages);

  [[nodiscard]] bool extendMappedSize(wasm::Pages maxPages);

  // Try and grow the mapped region of memory. Does not change current size.
  // Does not move memory if no space to grow.
  void tryGrowMaxPagesInPlace(wasm::Pages deltaMaxPages);
};

}  // namespace js

template <>
bool JSObject::is<js::ArrayBufferObjectMaybeShared>() const;

#endif  // vm_ArrayBufferObject_h
