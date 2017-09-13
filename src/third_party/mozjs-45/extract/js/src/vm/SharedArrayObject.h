/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedArrayObject_h
#define vm_SharedArrayObject_h

#include "mozilla/Atomics.h"

#include "jsapi.h"
#include "jsobj.h"
#include "jstypes.h"

#include "gc/Barrier.h"
#include "vm/ArrayBufferObject.h"

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
 */
class SharedArrayRawBuffer
{
  private:
    mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> refcount;
    uint32_t length;

    // A list of structures representing tasks waiting on some
    // location within this buffer.
    FutexWaiter* waiters_;

  protected:
    SharedArrayRawBuffer(uint8_t* buffer, uint32_t length)
      : refcount(1),
        length(length),
        waiters_(nullptr)
    {
        MOZ_ASSERT(buffer == dataPointerShared());
    }

  public:
    static SharedArrayRawBuffer* New(JSContext* cx, uint32_t length);

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

    uint32_t byteLength() const {
        return length;
    }

    void addReference();
    void dropReference();
};

/*
 * SharedArrayBufferObject
 *
 * When transferred to a WebWorker, the buffer is not neutered on the
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

    static const uint8_t RESERVED_SLOTS = 1;

    static const Class class_;
    static const Class protoClass;
    static const JSFunctionSpec jsfuncs[];
    static const JSFunctionSpec jsstaticfuncs[];

    static bool byteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

    static bool fun_isView(JSContext* cx, unsigned argc, Value* vp);

    static bool class_constructor(JSContext* cx, unsigned argc, Value* vp);

    // Create a SharedArrayBufferObject with a new SharedArrayRawBuffer.
    static SharedArrayBufferObject* New(JSContext* cx, uint32_t length);

    // Create a SharedArrayBufferObject using an existing SharedArrayRawBuffer.
    static SharedArrayBufferObject* New(JSContext* cx, SharedArrayRawBuffer* buffer);

    static void Finalize(FreeOp* fop, JSObject* obj);

    static void addSizeOfExcludingThis(JSObject* obj, mozilla::MallocSizeOf mallocSizeOf,
                                       JS::ClassInfo* info);

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
        return rawBufferObject()->byteLength();
    }

    SharedMem<uint8_t*> dataPointerShared() const {
        return rawBufferObject()->dataPointerShared();
    }

private:
    void acceptRawBuffer(SharedArrayRawBuffer* buffer);
    void dropRawBuffer();
};

bool IsSharedArrayBuffer(HandleValue v);
bool IsSharedArrayBuffer(HandleObject o);
bool IsSharedArrayBuffer(JSObject* o);

SharedArrayBufferObject& AsSharedArrayBuffer(HandleObject o);

} // namespace js

#endif // vm_SharedArrayObject_h
