/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedTypedArrayObject_h
#define vm_SharedTypedArrayObject_h

#include "jsobj.h"

#include "builtin/TypedObjectConstants.h"
#include "gc/Barrier.h"
#include "js/Class.h"
#include "vm/ArrayBufferObject.h"
#include "vm/SharedArrayObject.h"
#include "vm/TypedArrayObject.h"

typedef struct JSProperty JSProperty;

namespace js {

// Note that the representation of a SharedTypedArrayObject is the
// same as the representation of a TypedArrayObject, see comments in
// TypedArrayObject.h.

class SharedTypedArrayObject : public NativeObject
{
  protected:
    static const size_t BUFFER_SLOT      = TypedArrayLayout::BUFFER_SLOT;
    static const size_t BYTEOFFSET_SLOT  = TypedArrayLayout::BYTEOFFSET_SLOT;
    static const size_t LENGTH_SLOT      = TypedArrayLayout::LENGTH_SLOT;
    static const size_t RESERVED_SLOTS   = TypedArrayLayout::RESERVED_SLOTS;
    static const size_t DATA_SLOT        = TypedArrayLayout::DATA_SLOT;

  public:
    typedef SharedTypedArrayObject SomeTypedArray;
    typedef SharedArrayBufferObject BufferType;

    template<typename T> struct OfType;

    static bool ensureHasBuffer(JSContext* cx, Handle<SharedTypedArrayObject*> tarray) {
        return true;
    }

    static bool sameBuffer(Handle<SharedTypedArrayObject*> a, Handle<SharedTypedArrayObject*> b) {
        // Object equality isn't good enough for shared typed arrays.
        return a->buffer()->globalID() == b->buffer()->globalID();
    }

    static bool is(HandleValue v);

    static const Class classes[Scalar::MaxTypedArrayViewType];
    static const Class protoClasses[Scalar::MaxTypedArrayViewType];

    static SharedArrayBufferObject* bufferObject(JSContext* cx, Handle<SharedTypedArrayObject*> obj);

    static Value bufferValue(SharedTypedArrayObject* tarr) {
        return tarr->getFixedSlot(BUFFER_SLOT);
    }
    static Value byteOffsetValue(SharedTypedArrayObject* tarr) {
        return tarr->getFixedSlot(BYTEOFFSET_SLOT);
    }
    static inline Value byteLengthValue(SharedTypedArrayObject* tarr);
    static Value lengthValue(SharedTypedArrayObject* tarr) {
        return tarr->getFixedSlot(LENGTH_SLOT);
    }

    static void setElement(SharedTypedArrayObject& obj, uint32_t index, double d);

    static bool isOriginalLengthGetter(Scalar::Type type, Native native);

    SharedArrayBufferObject* buffer() const;

    inline Scalar::Type type() const;

    inline size_t bytesPerElement() const;

    void* viewData() const {
        return getPrivate(DATA_SLOT);
    }
    uint32_t byteOffset() const {
        return byteOffsetValue(const_cast<SharedTypedArrayObject*>(this)).toInt32();
    }
    uint32_t byteLength() const {
        return byteLengthValue(const_cast<SharedTypedArrayObject*>(this)).toInt32();
    }
    uint32_t length() const {
        return lengthValue(const_cast<SharedTypedArrayObject*>(this)).toInt32();
    }

    Value getElement(uint32_t index);

    /*
     * Byte length above which created typed arrays and data views will have
     * singleton types regardless of the context in which they are created.
     */
    static const uint32_t SINGLETON_TYPE_BYTE_LENGTH = 1024 * 1024 * 10;

  private:
    static TypedArrayLayout layout_;

  public:
    static const TypedArrayLayout& layout() {
        return layout_;
    }
};

inline bool
IsSharedTypedArrayClass(const Class* clasp)
{
    return &SharedTypedArrayObject::classes[0] <= clasp &&
           clasp < &SharedTypedArrayObject::classes[Scalar::MaxTypedArrayViewType];
}

inline bool
IsSharedTypedArrayProtoClass(const Class* clasp)
{
    return &SharedTypedArrayObject::protoClasses[0] <= clasp &&
           clasp < &SharedTypedArrayObject::protoClasses[Scalar::MaxTypedArrayViewType];
}

bool
IsSharedTypedArrayConstructor(HandleValue v, uint32_t type);

inline Scalar::Type
SharedTypedArrayObject::type() const
{
    MOZ_ASSERT(IsSharedTypedArrayClass(getClass()));
    return static_cast<Scalar::Type>(getClass() - &classes[0]);
}

inline size_t
SharedTypedArrayObject::bytesPerElement() const
{
    return Scalar::byteSize(type());
}

/* static */ inline Value
SharedTypedArrayObject::byteLengthValue(SharedTypedArrayObject* tarr)
{
    size_t size = tarr->bytesPerElement();
    return Int32Value(tarr->getFixedSlot(LENGTH_SLOT).toInt32() * size);
}

}  // namespace js

template <>
inline bool
JSObject::is<js::SharedTypedArrayObject>() const
{
    return js::IsSharedTypedArrayClass(getClass());
}

#endif // vm_SharedTypedArrayObject_h
