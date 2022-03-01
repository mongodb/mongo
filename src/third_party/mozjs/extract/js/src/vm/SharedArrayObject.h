/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedArrayObject_h
#define vm_SharedArrayObject_h

#include "mozilla/Atomics.h"

#include "jsapi.h"
#include "jstypes.h"

#include "gc/Barrier.h"
#include "gc/Memory.h"
#include "vm/ArrayBufferObject.h"
#include "vm/JSObject.h"
#include "wasm/WasmPages.h"

namespace js {

class FutexWaiter;

/*
 * SharedArrayRawBuffer
 *
 * A bookkeeping object always stored immediately before the raw buffer.
 * The buffer itself is mmap()'d and refcounted.
 * SharedArrayBufferObjects and structured clone objects may hold references.
 *
 *           |<------ sizeof ------>|<- length ->|
 *
 *   | waste | SharedArrayRawBuffer | data array | waste |
 *
 * Observe that if we want to map the data array on a specific address, such
 * as absolute zero (bug 1056027), then the SharedArrayRawBuffer cannot be
 * prefixed to the data array, it has to be a separate object, also in
 * shared memory.  (That would get rid of ~4KB of waste, as well.)  Very little
 * else would have to change throughout the engine, the SARB would point to
 * the data array using a constant pointer, instead of computing its
 * address.
 *
 * If preparedForWasm_ is true then length_ can change following initialization;
 * it may grow toward maxSize_.  See extensive comments above WasmArrayRawBuffer
 * in ArrayBufferObject.cpp.
 *
 * length_ only grows when the lock is held.
 */
class SharedArrayRawBuffer {
 private:
  mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> refcount_;
  mozilla::Atomic<size_t, mozilla::SequentiallyConsistent> length_;
  Mutex growLock_;
  // The maximum size of this buffer in wasm pages. If this buffer was not
  // prepared for wasm, then this is zero.
  wasm::Pages wasmMaxPages_;
  size_t mappedSize_;  // Does not include the page for the header
  bool preparedForWasm_;

  // A list of structures representing tasks waiting on some
  // location within this buffer.
  FutexWaiter* waiters_;

  uint8_t* basePointer() {
    SharedMem<uint8_t*> p = dataPointerShared() - gc::SystemPageSize();
    MOZ_ASSERT(p.asValue() % gc::SystemPageSize() == 0);
    return p.unwrap(/* we trust you won't abuse it */);
  }

 protected:
  SharedArrayRawBuffer(uint8_t* buffer, size_t length, wasm::Pages wasmMaxPages,
                       size_t mappedSize, bool preparedForWasm)
      : refcount_(1),
        length_(length),
        growLock_(mutexid::SharedArrayGrow),
        wasmMaxPages_(wasmMaxPages),
        mappedSize_(mappedSize),
        preparedForWasm_(preparedForWasm),
        waiters_(nullptr) {
    MOZ_ASSERT(buffer == dataPointerShared());
  }

  // Allocate a SharedArrayRawBuffer for either Wasm or other users.
  // `wasmMaxPages` must always be something for wasm and nothing for other
  // users.
  static SharedArrayRawBuffer* AllocateInternal(
      size_t length, const mozilla::Maybe<wasm::Pages>& wasmMaxPages,
      const mozilla::Maybe<size_t>& wasmMappedSize);

 public:
  class Lock;
  friend class Lock;

  class MOZ_STACK_CLASS Lock {
    SharedArrayRawBuffer* buf;

   public:
    explicit Lock(SharedArrayRawBuffer* buf) : buf(buf) {
      buf->growLock_.lock();
    }
    ~Lock() { buf->growLock_.unlock(); }
  };

  static SharedArrayRawBuffer* Allocate(size_t length);
  static SharedArrayRawBuffer* AllocateWasm(
      wasm::Pages initialPages, const mozilla::Maybe<wasm::Pages>& maxPages,
      const mozilla::Maybe<size_t>& mappedSize);

  // This may be called from multiple threads.  The caller must take
  // care of mutual exclusion.
  FutexWaiter* waiters() const { return waiters_; }

  // This may be called from multiple threads.  The caller must take
  // care of mutual exclusion.
  void setWaiters(FutexWaiter* waiters) { waiters_ = waiters; }

  SharedMem<uint8_t*> dataPointerShared() const {
    uint8_t* ptr =
        reinterpret_cast<uint8_t*>(const_cast<SharedArrayRawBuffer*>(this));
    return SharedMem<uint8_t*>::shared(ptr + sizeof(SharedArrayRawBuffer));
  }

  static const SharedArrayRawBuffer* fromDataPtr(const uint8_t* dataPtr) {
    return reinterpret_cast<const SharedArrayRawBuffer*>(
        dataPtr - sizeof(SharedArrayRawBuffer));
  }

  size_t volatileByteLength() const { return length_; }

  wasm::Pages volatileWasmPages() const {
    return wasm::Pages::fromByteLengthExact(length_);
  }

  wasm::Pages wasmMaxPages() const { return wasmMaxPages_; }

  size_t mappedSize() const { return mappedSize_; }

  bool isWasm() const { return preparedForWasm_; }

  void tryGrowMaxPagesInPlace(wasm::Pages deltaMaxPages);

  bool wasmGrowToPagesInPlace(const Lock&, wasm::Pages newPages);

  uint32_t refcount() const { return refcount_; }

  [[nodiscard]] bool addReference();
  void dropReference();

  static int32_t liveBuffers();
};

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
 */
class SharedArrayBufferObject : public ArrayBufferObjectMaybeShared {
  static bool byteLengthGetterImpl(JSContext* cx, const CallArgs& args);

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

  static const JSClass class_;
  static const JSClass protoClass_;

  static bool byteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

  static bool class_constructor(JSContext* cx, unsigned argc, Value* vp);

  static bool isOriginalByteLengthGetter(Native native) {
    return native == byteLengthGetter;
  }

  // Create a SharedArrayBufferObject with a new SharedArrayRawBuffer.
  static SharedArrayBufferObject* New(JSContext* cx, size_t length,
                                      HandleObject proto = nullptr);

  // Create a SharedArrayBufferObject using an existing SharedArrayRawBuffer,
  // recording the given length in the SharedArrayBufferObject.
  static SharedArrayBufferObject* New(JSContext* cx,
                                      SharedArrayRawBuffer* buffer,
                                      size_t length,
                                      HandleObject proto = nullptr);

  static void Finalize(JSFreeOp* fop, JSObject* obj);

  static void addSizeOfExcludingThis(JSObject* obj,
                                     mozilla::MallocSizeOf mallocSizeOf,
                                     JS::ClassInfo* info);

  static void copyData(Handle<SharedArrayBufferObject*> toBuffer,
                       size_t toIndex,
                       Handle<SharedArrayBufferObject*> fromBuffer,
                       size_t fromIndex, size_t count);

  SharedArrayRawBuffer* rawBufferObject() const;

  // Invariant: This method does not cause GC and can be called
  // without anchoring the object it is called on.
  uintptr_t globalID() const {
    // The buffer address is good enough as an ID provided the memory is not
    // shared between processes or, if it is, it is mapped to the same address
    // in every process.  (At the moment, shared memory cannot be shared between
    // processes.)
    return dataPointerShared().asValue();
  }

  size_t byteLength() const {
    return size_t(getFixedSlot(LENGTH_SLOT).toPrivate());
  }

  bool isWasm() const { return rawBufferObject()->isWasm(); }
  SharedMem<uint8_t*> dataPointerShared() const {
    return rawBufferObject()->dataPointerShared();
  }

  // WebAssembly support:

  // Create a SharedArrayBufferObject using the provided buffer and size.
  // Assumes ownership of a reference to |buffer| even in case of failure,
  // i.e. on failure |buffer->dropReference()| is performed.
  static SharedArrayBufferObject* createFromNewRawBuffer(
      JSContext* cx, SharedArrayRawBuffer* buffer, size_t initialSize);

  wasm::Pages volatileWasmPages() const {
    return rawBufferObject()->volatileWasmPages();
  }
  wasm::Pages wasmMaxPages() const { return rawBufferObject()->wasmMaxPages(); }

  size_t wasmMappedSize() const { return rawBufferObject()->mappedSize(); }

 private:
  [[nodiscard]] bool acceptRawBuffer(SharedArrayRawBuffer* buffer,
                                     size_t length);
  void dropRawBuffer();
};

using RootedSharedArrayBufferObject = Rooted<SharedArrayBufferObject*>;
using HandleSharedArrayBufferObject = Handle<SharedArrayBufferObject*>;
using MutableHandleSharedArrayBufferObject =
    MutableHandle<SharedArrayBufferObject*>;

}  // namespace js

#endif  // vm_SharedArrayObject_h
