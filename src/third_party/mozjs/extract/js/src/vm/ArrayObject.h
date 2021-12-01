/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayObject_h
#define vm_ArrayObject_h

#include "vm/NativeObject.h"

namespace js {

class AutoSetNewObjectMetadata;

class ArrayObject : public NativeObject
{
  public:
    // Array(x) eagerly allocates dense elements if x <= this value. Without
    // the subtraction the max would roll over to the next power-of-two (4096)
    // due to the way that growElements() and goodAllocated() work.
    static const uint32_t EagerAllocationMaxLength = 2048 - ObjectElements::VALUES_PER_HEADER;

    static const Class class_;

    bool lengthIsWritable() const {
        return !getElementsHeader()->hasNonwritableArrayLength();
    }

    uint32_t length() const {
        return getElementsHeader()->length;
    }

    void setNonWritableLength(JSContext* cx) {
        if (getElementsHeader()->numShiftedElements() > 0)
            moveShiftedElements();

        // When an array's length becomes non-writable, writes to indexes
        // greater than or equal to the length don't change the array.  We
        // handle this with a check for non-writable length in most places.
        // But in JIT code every check counts -- so we piggyback the check on
        // the already-required range check for |index < capacity| by making
        // capacity of arrays with non-writable length never exceed the length.
        ObjectElements* header = getElementsHeader();
        uint32_t len = header->initializedLength;
        if (header->capacity > len) {
            shrinkElements(cx, len);
            header = getElementsHeader();
            header->capacity = len;
        }
        header->setNonwritableArrayLength();
    }

    inline void setLength(JSContext* cx, uint32_t length);

    // Variant of setLength for use on arrays where the length cannot overflow int32_t.
    void setLengthInt32(uint32_t length) {
        MOZ_ASSERT(lengthIsWritable());
        MOZ_ASSERT(length <= INT32_MAX);
        getElementsHeader()->length = length;
    }

    // Make an array object with the specified initial state.
    static inline ArrayObject*
    createArray(JSContext* cx,
                gc::AllocKind kind,
                gc::InitialHeap heap,
                HandleShape shape,
                HandleObjectGroup group,
                uint32_t length,
                AutoSetNewObjectMetadata& metadata);

    // Make a copy-on-write array object which shares the elements of an
    // existing object.
    static inline ArrayObject*
    createCopyOnWriteArray(JSContext* cx,
                           gc::InitialHeap heap,
                           HandleArrayObject sharedElementsOwner);

  private:
    // Helper for the above methods.
    static inline ArrayObject*
    createArrayInternal(JSContext* cx,
                        gc::AllocKind kind,
                        gc::InitialHeap heap,
                        HandleShape shape,
                        HandleObjectGroup group,
                        AutoSetNewObjectMetadata&);

    static inline ArrayObject*
    finishCreateArray(ArrayObject* obj, HandleShape shape, AutoSetNewObjectMetadata& metadata);
};

} // namespace js

#endif // vm_ArrayObject_h

