/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedArrayObject_h
#define vm_SharedArrayObject_h

#include "mozilla/Atomics.h"

#include "jsapi.h"
#include "jstypes.h"

#include "gc/Barrier.h"
#include "vm/ArrayBufferObject.h"
#include "vm/JSObject.h"

typedef struct JSProperty JSProperty;

namespace js {

class FutexWaiter;

/*
 * SharedArrayRawBuffer
 *
 * A bookkeeping object always stored immediately before the raw buffer.
 * The buffer itself is mmap()'d and refcounted.
 * SharedArrayBufferObjects and AsmJS code may hold references.
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
 * If preparedForAsmJS_ is true then length_ never changes.
 *
 * If preparedForWasm_ is true then length_ can change following initialization;
 * it may grow toward maxSize_.  See extensive comments above WasmArrayRawBuffer
 * in ArrayBufferObject.cpp.
 *
 * length_ only grows when the lock is held.
 */
class SharedArrayRawBuffer
{
  private:
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> refcount_;
    Mutex    lock_;
    uint32_t length_;
    uint32_t maxSize_;
    size_t   mappedSize_;         // Does not include the page for the header
    bool     preparedForAsmJS_;
    bool     preparedForWasm_;

    // A list of structures representing tasks waiting on some
    // location within this buffer.
    FutexWaiter* waiters_;

    uint8_t* basePointer() {
        SharedMem<uint8_t*> p = dataPointerShared() - gc::SystemPageSize();
        MOZ_ASSERT(p.asValue() % gc::SystemPageSize() == 0);
        return p.unwrap(/* we trust you won't abuse it */);
    }

  protected:
    SharedArrayRawBuffer(uint8_t* buffer, uint32_t length, uint32_t maxSize, size_t mappedSize,
                         bool preparedForAsmJS, bool preparedForWasm)
      : refcount_(1),
        lock_(mutexid::SharedArrayGrow),
        length_(length),
        maxSize_(maxSize),
        mappedSize_(mappedSize),
        preparedForAsmJS_(preparedForAsmJS),
        preparedForWasm_(preparedForWasm),
        waiters_(nullptr)
    {
        MOZ_ASSERT(buffer == dataPointerShared());
    }

  public:
    class Lock;
    friend class Lock;

    class MOZ_STACK_CLASS Lock {
        SharedArrayRawBuffer* buf;
      public:
        explicit Lock(SharedArrayRawBuffer* buf) : buf(buf) {
            buf->lock_.lock();
        }
        ~Lock() {
            buf->lock_.unlock();
        }
    };

    // max must be Something for wasm, Nothing for other uses
    static SharedArrayRawBuffer* Allocate(uint32_t initial, const mozilla::Maybe<uint32_t>& max);

    // This may be called from multiple threads.  The caller must take
    // care of mutual exclusion.
    FutexWaiter* waiters() const {
        return waiters_;
    }

    // This may be called from multiple threads.  The caller must take
    // care of mutual exclusion.
    void setWaiters(FutexWaiter* waiters) {
        waiters_ = waiters;
    }

    SharedMem<uint8_t*> dataPointerShared() const {
        uint8_t* ptr = reinterpret_cast<uint8_t*>(const_cast<SharedArrayRawBuffer*>(this));
        return SharedMem<uint8_t*>::shared(ptr + sizeof(SharedArrayRawBuffer));
    }

    uint32_t byteLength(const Lock&) const {
        return length_;
    }

    uint32_t maxSize() const {
        return maxSize_;
    }

    size_t mappedSize() const {
        return mappedSize_;
    }

#ifndef WASM_HUGE_MEMORY
    uint32_t boundsCheckLimit() const {
        return mappedSize_ - wasm::GuardSize;
    }
#endif

    bool isPreparedForAsmJS() const {
        return preparedForAsmJS_;
    }

    bool isWasm() const {
        return preparedForWasm_;
    }

#ifndef WASM_HUGE_MEMORY
    void tryGrowMaxSizeInPlace(uint32_t deltaMaxSize);
#endif

    bool wasmGrowToSizeInPlace(const Lock&, uint32_t newLength);

    uint32_t refcount() const { return refcount_; }

    MOZ_MUST_USE bool addReference();
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
class SharedArrayBufferObject : public ArrayBufferObjectMaybeShared
{
    static bool byteLengthGetterImpl(JSContext* cx, const CallArgs& args);

  public:
    // RAWBUF_SLOT holds a pointer (as "private" data) to the
    // SharedArrayRawBuffer object, which is manually managed storage.
    static const uint8_t RAWBUF_SLOT = 0;

    // LENGTH_SLOT holds the length of the underlying buffer as it was when this
    // object was created.  For JS and AsmJS use cases this is the same length
    // as the buffer, but for Wasm the buffer can grow, and the buffer's length
    // may be greater than the object's length.
    static const uint8_t LENGTH_SLOT = 1;

    static const uint8_t RESERVED_SLOTS = 2;

    static const Class class_;
    static const Class protoClass_;

    static bool byteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

    static bool class_constructor(JSContext* cx, unsigned argc, Value* vp);

    // Create a SharedArrayBufferObject with a new SharedArrayRawBuffer.
    static SharedArrayBufferObject* New(JSContext* cx,
                                        uint32_t length,
                                        HandleObject proto = nullptr);

    // Create a SharedArrayBufferObject using an existing SharedArrayRawBuffer,
    // recording the given length in the SharedArrayBufferObject.
    static SharedArrayBufferObject* New(JSContext* cx,
                                        SharedArrayRawBuffer* buffer,
                                        uint32_t length,
                                        HandleObject proto = nullptr);

    static void Finalize(FreeOp* fop, JSObject* obj);

    static void addSizeOfExcludingThis(JSObject* obj, mozilla::MallocSizeOf mallocSizeOf,
                                       JS::ClassInfo* info);

    static void copyData(Handle<SharedArrayBufferObject*> toBuffer, uint32_t toIndex,
                         Handle<SharedArrayBufferObject*> fromBuffer, uint32_t fromIndex,
                         uint32_t count);

    SharedArrayRawBuffer* rawBufferObject() const;

    // Invariant: This method does not cause GC and can be called
    // without anchoring the object it is called on.
    uintptr_t globalID() const {
        // The buffer address is good enough as an ID provided the memory is not shared
        // between processes or, if it is, it is mapped to the same address in every
        // process.  (At the moment, shared memory cannot be shared between processes.)
        return dataPointerShared().asValue();
    }

    uint32_t byteLength() const {
        return getReservedSlot(LENGTH_SLOT).toPrivateUint32();
    }

    bool isPreparedForAsmJS() const {
        return rawBufferObject()->isPreparedForAsmJS();
    }
    bool isWasm() const {
        return rawBufferObject()->isWasm();
    }
    SharedMem<uint8_t*> dataPointerShared() const {
        return rawBufferObject()->dataPointerShared();
    }

    // WebAssembly support:

    // Create a SharedArrayBufferObject using the provided buffer and size.
    // Assumes ownership of a reference to |buffer| even in case of failure,
    // i.e. on failure |buffer->dropReference()| is performed.
    static SharedArrayBufferObject*
    createFromNewRawBuffer(JSContext* cx, SharedArrayRawBuffer* buffer, uint32_t initialSize);

    mozilla::Maybe<uint32_t> wasmMaxSize() const {
        return mozilla::Some(rawBufferObject()->maxSize());
    }

    size_t wasmMappedSize() const {
        return rawBufferObject()->mappedSize();
    }

#ifndef WASM_HUGE_MEMORY
    uint32_t wasmBoundsCheckLimit() const;
#endif

private:
    void acceptRawBuffer(SharedArrayRawBuffer* buffer, uint32_t length);
    void dropRawBuffer();
};

bool IsSharedArrayBuffer(HandleValue v);
bool IsSharedArrayBuffer(HandleObject o);
bool IsSharedArrayBuffer(JSObject* o);

SharedArrayBufferObject& AsSharedArrayBuffer(HandleObject o);

typedef Rooted<SharedArrayBufferObject*> RootedSharedArrayBufferObject;
typedef Handle<SharedArrayBufferObject*> HandleSharedArrayBufferObject;
typedef MutableHandle<SharedArrayBufferObject*> MutableHandleSharedArrayBufferObject;

} // namespace js

#endif // vm_SharedArrayObject_h
