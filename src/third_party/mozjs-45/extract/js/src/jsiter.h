/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsiter_h
#define jsiter_h

/*
 * JavaScript iterators.
 */

#include "mozilla/MemoryReporting.h"

#include "jscntxt.h"

#include "gc/Barrier.h"
#include "vm/ReceiverGuard.h"
#include "vm/Stack.h"

/*
 * For cacheable native iterators, whether the iterator is currently active.
 * Not serialized by XDR.
 */
#define JSITER_ACTIVE       0x1000
#define JSITER_UNREUSABLE   0x2000

namespace js {

struct NativeIterator
{
    HeapPtrObject obj;                  // Object being iterated.
    JSObject* iterObj_;                 // Internal iterator object.
    HeapPtrFlatString* props_array;
    HeapPtrFlatString* props_cursor;
    HeapPtrFlatString* props_end;
    HeapReceiverGuard* guard_array;
    uint32_t guard_length;
    uint32_t guard_key;
    uint32_t flags;

  private:
    /* While in compartment->enumerators, these form a doubly linked list. */
    NativeIterator* next_;
    NativeIterator* prev_;

  public:
    bool isKeyIter() const {
        return (flags & JSITER_FOREACH) == 0;
    }

    inline HeapPtrFlatString* begin() const {
        return props_array;
    }

    inline HeapPtrFlatString* end() const {
        return props_end;
    }

    size_t numKeys() const {
        return end() - begin();
    }

    JSObject* iterObj() const {
        return iterObj_;
    }
    HeapPtrFlatString* current() const {
        MOZ_ASSERT(props_cursor < props_end);
        return props_cursor;
    }

    NativeIterator* next() {
        return next_;
    }

    static inline size_t offsetOfNext() {
        return offsetof(NativeIterator, next_);
    }
    static inline size_t offsetOfPrev() {
        return offsetof(NativeIterator, prev_);
    }

    void incCursor() {
        props_cursor = props_cursor + 1;
    }
    void link(NativeIterator* other) {
        /* A NativeIterator cannot appear in the enumerator list twice. */
        MOZ_ASSERT(!next_ && !prev_);
        MOZ_ASSERT(flags & JSITER_ENUMERATE);

        this->next_ = other;
        this->prev_ = other->prev_;
        other->prev_->next_ = this;
        other->prev_ = this;
    }
    void unlink() {
        MOZ_ASSERT(flags & JSITER_ENUMERATE);

        next_->prev_ = prev_;
        prev_->next_ = next_;
        next_ = nullptr;
        prev_ = nullptr;
    }

    static NativeIterator* allocateSentinel(JSContext* maybecx);
    static NativeIterator* allocateIterator(JSContext* cx, uint32_t slength,
                                            const js::AutoIdVector& props);
    void init(JSObject* obj, JSObject* iterObj, unsigned flags, uint32_t slength, uint32_t key);

    void mark(JSTracer* trc);

    static void destroy(NativeIterator* iter) {
        js_free(iter);
    }
};

class PropertyIteratorObject : public NativeObject
{
  public:
    static const Class class_;

    NativeIterator* getNativeIterator() const {
        return static_cast<js::NativeIterator*>(getPrivate());
    }
    void setNativeIterator(js::NativeIterator* ni) {
        setPrivate(ni);
    }

    size_t sizeOfMisc(mozilla::MallocSizeOf mallocSizeOf) const;

  private:
    static void trace(JSTracer* trc, JSObject* obj);
    static void finalize(FreeOp* fop, JSObject* obj);
};

class ArrayIteratorObject : public JSObject
{
  public:
    static const Class class_;
};

class StringIteratorObject : public JSObject
{
  public:
    static const Class class_;
};

class ListIteratorObject : public JSObject
{
  public:
    static const Class class_;
};

bool
GetIterator(JSContext* cx, HandleObject obj, unsigned flags, MutableHandleObject objp);

JSObject*
GetIteratorObject(JSContext* cx, HandleObject obj, unsigned flags);

/*
 * Creates either a key or value iterator, depending on flags. For a value
 * iterator, performs value-lookup to convert the given list of jsids.
 */
bool
EnumeratedIdVectorToIterator(JSContext* cx, HandleObject obj, unsigned flags, AutoIdVector& props,
                             MutableHandleObject objp);

bool
NewEmptyPropertyIterator(JSContext* cx, unsigned flags, MutableHandleObject objp);

/*
 * Convert the value stored in *vp to its iteration object. The flags should
 * contain JSITER_ENUMERATE if js::ValueToIterator is called when enumerating
 * for-in semantics are required, and when the caller can guarantee that the
 * iterator will never be exposed to scripts.
 */
bool
ValueToIterator(JSContext* cx, unsigned flags, MutableHandleValue vp);

bool
CloseIterator(JSContext* cx, HandleObject iterObj);

bool
UnwindIteratorForException(JSContext* cx, HandleObject obj);

void
UnwindIteratorForUncatchableException(JSContext* cx, JSObject* obj);

bool
IteratorConstructor(JSContext* cx, unsigned argc, Value* vp);

extern bool
SuppressDeletedProperty(JSContext* cx, HandleObject obj, jsid id);

extern bool
SuppressDeletedElement(JSContext* cx, HandleObject obj, uint32_t index);

/*
 * IteratorMore() returns the next iteration value. If no value is available,
 * MagicValue(JS_NO_ITER_VALUE) is returned.
 */
extern bool
IteratorMore(JSContext* cx, HandleObject iterobj, MutableHandleValue rval);

extern bool
ThrowStopIteration(JSContext* cx);

/*
 * Create an object of the form { value: VALUE, done: DONE }.
 * ES6 draft from 2013-09-05, section 25.4.3.4.
 */
extern JSObject*
CreateItrResultObject(JSContext* cx, HandleValue value, bool done);

extern JSObject*
InitLegacyIteratorClass(JSContext* cx, HandleObject obj);

extern JSObject*
InitStopIterationClass(JSContext* cx, HandleObject obj);

} /* namespace js */

#endif /* jsiter_h */
