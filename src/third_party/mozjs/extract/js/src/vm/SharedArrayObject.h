/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedArrayObject_h
#define vm_SharedArrayObject_h

#include "mozilla/Atomics.h"

#include "jstypes.h"

#include "gc/Memory.h"
#include "vm/ArrayBufferObject.h"
#include "wasm/WasmMemory.h"

namespace js {

class FutexWaiter;
class WasmSharedArrayRawBuffer;

/*
 * SharedArrayRawBuffer
 *
 * A bookkeeping object always stored before the raw buffer. The buffer itself
 * is refcounted. SharedArrayBufferObjects and structured clone objects may hold
 * references.
 *
 * WasmSharedArrayRawBuffer is a derived class that's used for Wasm buffers.
 *
 * - Non-Wasm buffers are allocated with a single calloc allocation, like this:
 *
 *    |<------ sizeof ------>|<- length ->|
 *    | SharedArrayRawBuffer | data array |
 *
 * - Wasm buffers are allocated with MapBufferMemory (mmap), like this:
 *
 *           |<-------- sizeof -------->|<- length ->|
 *   | waste | WasmSharedArrayRawBuffer | data array | waste |
 *
 * Observe that if we want to map the data array on a specific address, such
 * as absolute zero (bug 1056027), then the {Wasm}SharedArrayRawBuffer cannot be
 * prefixed to the data array, it has to be a separate object, also in
 * shared memory.  (That would get rid of ~4KB of waste, as well.)  Very little
 * else would have to change throughout the engine, the SARB would point to
 * the data array using a constant pointer, instead of computing its
 * address.
 *
 * For Wasm buffers, length_ can change following initialization; it may grow
 * toward sourceMaxPages_. See extensive comments above WasmArrayRawBuffer in
 * ArrayBufferObject.cpp. length_ only grows when the lock is held.
 */
class SharedArrayRawBuffer {
 protected:
  // Whether this is a WasmSharedArrayRawBuffer.
  bool isWasm_;

  // Whether this is a growable non-Wasm buffer.
  bool isGrowable_;

  mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> refcount_;
  mozilla::Atomic<size_t, mozilla::SequentiallyConsistent> length_;

  // A list of structures representing tasks waiting on some
  // location within this buffer.
  FutexWaiter* waiters_ = nullptr;

 protected:
  SharedArrayRawBuffer(bool isGrowable, uint8_t* buffer, size_t length)
      : isWasm_(false), isGrowable_(isGrowable), refcount_(1), length_(length) {
    MOZ_ASSERT(buffer == dataPointerShared());
  }

  enum class WasmBuffer {};

  SharedArrayRawBuffer(WasmBuffer, uint8_t* buffer, size_t length)
      : isWasm_(true), isGrowable_(false), refcount_(1), length_(length) {
    MOZ_ASSERT(buffer == dataPointerShared());
  }

 public:
  static SharedArrayRawBuffer* Allocate(bool isGrowable, size_t length,
                                        size_t maxLength);

  inline WasmSharedArrayRawBuffer* toWasmBuffer();

  // This may be called from multiple threads.  The caller must take
  // care of mutual exclusion.
  FutexWaiter* waiters() const { return waiters_; }

  // This may be called from multiple threads.  The caller must take
  // care of mutual exclusion.
  void setWaiters(FutexWaiter* waiters) { waiters_ = waiters; }

  inline SharedMem<uint8_t*> dataPointerShared() const;

  size_t volatileByteLength() const { return length_; }

  bool isWasm() const { return isWasm_; }

  bool isGrowable() const { return isGrowable_; }

  uint32_t refcount() const { return refcount_; }

  [[nodiscard]] bool addReference();
  void dropReference();

  // Try to grow this buffer to |newByteLength| bytes. Returns false when the
  // current byte length is larger than |newByteLength|. Otherwise atomically
  // changes the byte length to |newByteLength| and then returns true.
  //
  // This method DOES NOT perform any memory operations to allocate additional
  // space. The caller is responsible to ensure that the buffer has been
  // allocated with enough space to hold at least |newByteLength| bytes. IOW
  // this method merely sets the number of user accessible bytes of this buffer.
  bool grow(size_t newByteLength);

  static size_t offsetOfByteLength() {
    return offsetof(SharedArrayRawBuffer, length_);
  }
};

class WasmSharedArrayRawBuffer : public SharedArrayRawBuffer {
 private:
  Mutex growLock_ MOZ_UNANNOTATED;
  // The index type of this buffer.
  wasm::IndexType indexType_;
  // The maximum size of this buffer in wasm pages.
  wasm::Pages clampedMaxPages_;
  wasm::Pages sourceMaxPages_;
  size_t mappedSize_;  // Does not include the page for the header.

  uint8_t* basePointer() {
    SharedMem<uint8_t*> p = dataPointerShared() - gc::SystemPageSize();
    MOZ_ASSERT(p.asValue() % gc::SystemPageSize() == 0);
    return p.unwrap(/* we trust you won't abuse it */);
  }

 protected:
  WasmSharedArrayRawBuffer(uint8_t* buffer, size_t length,
                           wasm::IndexType indexType,
                           wasm::Pages clampedMaxPages,
                           wasm::Pages sourceMaxPages, size_t mappedSize)
      : SharedArrayRawBuffer(WasmBuffer{}, buffer, length),
        growLock_(mutexid::SharedArrayGrow),
        indexType_(indexType),
        clampedMaxPages_(clampedMaxPages),
        sourceMaxPages_(sourceMaxPages),
        mappedSize_(mappedSize) {}

 public:
  friend class SharedArrayRawBuffer;

  class Lock;
  friend class Lock;

  class MOZ_RAII Lock {
    WasmSharedArrayRawBuffer* buf;

   public:
    explicit Lock(WasmSharedArrayRawBuffer* buf) : buf(buf) {
      buf->growLock_.lock();
    }
    ~Lock() { buf->growLock_.unlock(); }
  };

  static WasmSharedArrayRawBuffer* AllocateWasm(
      wasm::IndexType indexType, wasm::Pages initialPages,
      wasm::Pages clampedMaxPages,
      const mozilla::Maybe<wasm::Pages>& sourceMaxPages,
      const mozilla::Maybe<size_t>& mappedSize);

  static const WasmSharedArrayRawBuffer* fromDataPtr(const uint8_t* dataPtr) {
    return reinterpret_cast<const WasmSharedArrayRawBuffer*>(
        dataPtr - sizeof(WasmSharedArrayRawBuffer));
  }

  static WasmSharedArrayRawBuffer* fromDataPtr(uint8_t* dataPtr) {
    return reinterpret_cast<WasmSharedArrayRawBuffer*>(
        dataPtr - sizeof(WasmSharedArrayRawBuffer));
  }

  wasm::IndexType wasmIndexType() const { return indexType_; }

  wasm::Pages volatileWasmPages() const {
    return wasm::Pages::fromByteLengthExact(length_);
  }

  wasm::Pages wasmClampedMaxPages() const { return clampedMaxPages_; }
  wasm::Pages wasmSourceMaxPages() const { return sourceMaxPages_; }

  size_t mappedSize() const { return mappedSize_; }

  void tryGrowMaxPagesInPlace(wasm::Pages deltaMaxPages);

  bool wasmGrowToPagesInPlace(const Lock&, wasm::IndexType t,
                              wasm::Pages newPages);

  // Discard a region of memory, zeroing the pages and releasing physical memory
  // back to the operating system. byteOffset and byteLen must be wasm page
  // aligned and in bounds. A discard of zero bytes will have no effect.
  void discard(size_t byteOffset, size_t byteLen);
};

inline WasmSharedArrayRawBuffer* SharedArrayRawBuffer::toWasmBuffer() {
  MOZ_ASSERT(isWasm());
  return static_cast<WasmSharedArrayRawBuffer*>(this);
}

inline SharedMem<uint8_t*> SharedArrayRawBuffer::dataPointerShared() const {
  uint8_t* ptr =
      reinterpret_cast<uint8_t*>(const_cast<SharedArrayRawBuffer*>(this));
  ptr += isWasm() ? sizeof(WasmSharedArrayRawBuffer)
                  : sizeof(SharedArrayRawBuffer);
  return SharedMem<uint8_t*>::shared(ptr);
}

class FixedLengthSharedArrayBufferObject;
class GrowableSharedArrayBufferObject;

/*
 * SharedArrayBufferObject
 *
 * When transferred to a WebWorker, the buffer is not detached on the
 * parent side, and both child and parent reference the same buffer.
 *
 * The underlying memory is memory-mapped and reference counted
 * (across workers and/or processes).  The SharedArrayBuffer object
 * has a finalizer that decrements the refcount, the last one to leave
 * (globally) unmaps the memory.  The sender ups the refcount before
 * transmitting the memory to another worker.
 *
 * SharedArrayBufferObject (or really the underlying memory) /is
 * racy/: more than one worker can access the memory at the same time.
 *
 * A TypedArrayObject (a view) references a SharedArrayBuffer
 * and keeps it alive.  The SharedArrayBuffer does /not/ reference its
 * views.
 *
 * SharedArrayBufferObject is an abstract base class and has exactly two
 * concrete subclasses, FixedLengthSharedArrayBufferObject and
 * GrowableSharedArrayBufferObject.
 */
class SharedArrayBufferObject : public ArrayBufferObjectMaybeShared {
  static bool byteLengthGetterImpl(JSContext* cx, const CallArgs& args);
  static bool maxByteLengthGetterImpl(JSContext* cx, const CallArgs& args);
  static bool growableGetterImpl(JSContext* cx, const CallArgs& args);
  static bool growImpl(JSContext* cx, const CallArgs& args);

 public:
  // RAWBUF_SLOT holds a pointer (as "private" data) to the
  // SharedArrayRawBuffer object, which is manually managed storage.
  static const uint8_t RAWBUF_SLOT = 0;

  // LENGTH_SLOT holds the length of the underlying buffer as it was when this
  // object was created.  For JS use cases this is the same length as the
  // buffer, but for Wasm the buffer can grow, and the buffer's length may be
  // greater than the object's length.
  static const uint8_t LENGTH_SLOT = 1;

  static_assert(LENGTH_SLOT == ArrayBufferObject::BYTE_LENGTH_SLOT,
                "JIT code assumes the same slot is used for the length");

  static const uint8_t RESERVED_SLOTS = 2;

  static const JSClass protoClass_;

  static bool byteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

  static bool maxByteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

  static bool growableGetter(JSContext* cx, unsigned argc, Value* vp);

  static bool class_constructor(JSContext* cx, unsigned argc, Value* vp);

  static bool grow(JSContext* cx, unsigned argc, Value* vp);

  static bool isOriginalByteLengthGetter(Native native) {
    return native == byteLengthGetter;
  }

 private:
  template <class SharedArrayBufferType>
  static SharedArrayBufferType* NewWith(JSContext* cx,
                                        SharedArrayRawBuffer* buffer,
                                        size_t length, HandleObject proto);

 public:
  // Create a SharedArrayBufferObject with a new SharedArrayRawBuffer.
  static FixedLengthSharedArrayBufferObject* New(JSContext* cx, size_t length,
                                                 HandleObject proto = nullptr);

  // Create a SharedArrayBufferObject using an existing SharedArrayRawBuffer,
  // recording the given length in the SharedArrayBufferObject.
  static FixedLengthSharedArrayBufferObject* New(JSContext* cx,
                                                 SharedArrayRawBuffer* buffer,
                                                 size_t length,
                                                 HandleObject proto = nullptr);

  // Create a growable SharedArrayBufferObject with a new SharedArrayRawBuffer.
  static GrowableSharedArrayBufferObject* NewGrowable(
      JSContext* cx, size_t length, size_t maxLength,
      HandleObject proto = nullptr);

  // Create a growable SharedArrayBufferObject using an existing
  // SharedArrayRawBuffer, recording the given length in the
  // SharedArrayBufferObject.
  static GrowableSharedArrayBufferObject* NewGrowable(
      JSContext* cx, SharedArrayRawBuffer* buffer, size_t maxLength,
      HandleObject proto = nullptr);

  static void Finalize(JS::GCContext* gcx, JSObject* obj);

  static void addSizeOfExcludingThis(JSObject* obj,
                                     mozilla::MallocSizeOf mallocSizeOf,
                                     JS::ClassInfo* info,
                                     JS::RuntimeSizes* runtimeSizes);

  static void copyData(Handle<ArrayBufferObjectMaybeShared*> toBuffer,
                       size_t toIndex,
                       Handle<ArrayBufferObjectMaybeShared*> fromBuffer,
                       size_t fromIndex, size_t count);

  SharedArrayRawBuffer* rawBufferObject() const;

  WasmSharedArrayRawBuffer* rawWasmBufferObject() const {
    return rawBufferObject()->toWasmBuffer();
  }

  // Invariant: This method does not cause GC and can be called
  // without anchoring the object it is called on.
  uintptr_t globalID() const {
    // The buffer address is good enough as an ID provided the memory is not
    // shared between processes or, if it is, it is mapped to the same address
    // in every process.  (At the moment, shared memory cannot be shared between
    // processes.)
    return dataPointerShared().asValue();
  }

 protected:
  size_t growableByteLength() const {
    MOZ_ASSERT(isGrowable());
    return rawBufferObject()->volatileByteLength();
  }

 private:
  bool isInitialized() const {
    bool initialized = getFixedSlot(RAWBUF_SLOT).isDouble();
    MOZ_ASSERT_IF(initialized, getFixedSlot(LENGTH_SLOT).isDouble());
    return initialized;
  }

 public:
  // Returns either the byte length for fixed-length shared arrays. Or the
  // maximum byte length for growable shared arrays.
  size_t byteLengthOrMaxByteLength() const {
    return size_t(getFixedSlot(LENGTH_SLOT).toPrivate());
  }

  size_t byteLength() const {
    if (isGrowable()) {
      return growableByteLength();
    }
    return byteLengthOrMaxByteLength();
  }

  bool isWasm() const { return rawBufferObject()->isWasm(); }

  bool isGrowable() const { return rawBufferObject()->isGrowable(); }

  SharedMem<uint8_t*> dataPointerShared() const {
    return rawBufferObject()->dataPointerShared();
  }

  static constexpr int rawBufferOffset() {
    return NativeObject::getFixedSlotOffset(RAWBUF_SLOT);
  }

  // WebAssembly support:

  // Create a SharedArrayBufferObject using the provided buffer and size.
  // Assumes ownership of a reference to |buffer| even in case of failure,
  // i.e. on failure |buffer->dropReference()| is performed.
  static SharedArrayBufferObject* createFromNewRawBuffer(
      JSContext* cx, WasmSharedArrayRawBuffer* buffer, size_t initialSize);

  wasm::Pages volatileWasmPages() const {
    return rawWasmBufferObject()->volatileWasmPages();
  }
  wasm::Pages wasmClampedMaxPages() const {
    return rawWasmBufferObject()->wasmClampedMaxPages();
  }
  wasm::Pages wasmSourceMaxPages() const {
    return rawWasmBufferObject()->wasmSourceMaxPages();
  }

  size_t wasmMappedSize() const { return rawWasmBufferObject()->mappedSize(); }

  static void wasmDiscard(Handle<SharedArrayBufferObject*> buf,
                          uint64_t byteOffset, uint64_t byteLength);

 private:
  [[nodiscard]] bool acceptRawBuffer(SharedArrayRawBuffer* buffer,
                                     size_t length);
  void dropRawBuffer();
};

/**
 * FixedLengthSharedArrayBufferObject
 *
 * SharedArrayBuffer object with a fixed length. The JS exposed length is
 * unmodifiable, but the underlying memory can still grow for WebAssembly.
 *
 * Fixed-length SharedArrayBuffers can be used for asm.js and WebAssembly.
 */
class FixedLengthSharedArrayBufferObject : public SharedArrayBufferObject {
 public:
  static const JSClass class_;

  size_t byteLength() const { return byteLengthOrMaxByteLength(); }
};

/**
 * GrowableSharedArrayBufferObject
 *
 * SharedArrayBuffer object which can grow in size. The maximum byte length it
 * can grow to is set when creating the object.
 *
 * Growable SharedArrayBuffers can neither be used for asm.js nor WebAssembly.
 */
class GrowableSharedArrayBufferObject : public SharedArrayBufferObject {
 public:
  static const JSClass class_;

  size_t byteLength() const { return growableByteLength(); }

  size_t maxByteLength() const { return byteLengthOrMaxByteLength(); }
};

}  // namespace js

template <>
inline bool JSObject::is<js::SharedArrayBufferObject>() const {
  return is<js::FixedLengthSharedArrayBufferObject>() ||
         is<js::GrowableSharedArrayBufferObject>();
}

#endif  // vm_SharedArrayObject_h
