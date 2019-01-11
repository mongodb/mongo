/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_DataViewObject_h
#define vm_DataViewObject_h

#include "mozilla/Attributes.h"

#include "gc/Barrier.h"
#include "js/Class.h"
#include "vm/ArrayBufferObject.h"
#include "vm/JSObject.h"
#include "vm/SharedArrayObject.h"
#include "vm/TypedArrayObject.h"

namespace js {

// In the DataViewObject, the private slot contains a raw pointer into
// the buffer.  The buffer may be shared memory and the raw pointer
// should not be exposed without sharedness information accompanying
// it.

class DataViewObject : public NativeObject
{
  private:
    static const Class protoClass_;
    static const ClassSpec classSpec_;

    static JSObject* CreatePrototype(JSContext* cx, JSProtoKey key);

    static bool is(HandleValue v) {
        return v.isObject() && v.toObject().hasClass(&class_);
    }

    template <typename NativeType>
    static SharedMem<uint8_t*>
    getDataPointer(JSContext* cx, Handle<DataViewObject*> obj, uint64_t offset, bool* isSharedMemory);

    static bool bufferGetterImpl(JSContext* cx, const CallArgs& args);
    static bool bufferGetter(JSContext* cx, unsigned argc, Value* vp);

    static bool byteLengthGetterImpl(JSContext* cx, const CallArgs& args);
    static bool byteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

    static bool byteOffsetGetterImpl(JSContext* cx, const CallArgs& args);
    static bool byteOffsetGetter(JSContext* cx, unsigned argc, Value* vp);

    static bool
    getAndCheckConstructorArgs(JSContext* cx, HandleObject bufobj, const CallArgs& args,
                               uint32_t* byteOffset, uint32_t* byteLength);
    static bool constructSameCompartment(JSContext* cx, HandleObject bufobj, const CallArgs& args);
    static bool constructWrapped(JSContext* cx, HandleObject bufobj, const CallArgs& args);

    static DataViewObject*
    create(JSContext* cx, uint32_t byteOffset, uint32_t byteLength,
           Handle<ArrayBufferObjectMaybeShared*> arrayBuffer, HandleObject proto);

  public:
    static const Class class_;

    static Value byteOffsetValue(DataViewObject* view) {
        Value v = view->getFixedSlot(TypedArrayObject::BYTEOFFSET_SLOT);
        MOZ_ASSERT(v.toInt32() >= 0);
        return v;
    }

    static Value byteLengthValue(DataViewObject* view) {
        Value v = view->getFixedSlot(TypedArrayObject::LENGTH_SLOT);
        MOZ_ASSERT(v.toInt32() >= 0);
        return v;
    }

    static Value bufferValue(DataViewObject* view) {
        return view->getFixedSlot(TypedArrayObject::BUFFER_SLOT);
    }

    uint32_t byteOffset() const {
        return byteOffsetValue(const_cast<DataViewObject*>(this)).toInt32();
    }

    uint32_t byteLength() const {
        return byteLengthValue(const_cast<DataViewObject*>(this)).toInt32();
    }

    ArrayBufferObjectMaybeShared& arrayBufferEither() const {
        return bufferValue(
            const_cast<DataViewObject*>(this)).toObject().as<ArrayBufferObjectMaybeShared>();
    }

    SharedMem<void*> dataPointerEither() const {
        void *p = getPrivate();
        if (isSharedMemory())
            return SharedMem<void*>::shared(p);
        return SharedMem<void*>::unshared(p);
    }

    void* dataPointerUnshared() const {
        MOZ_ASSERT(!isSharedMemory());
        return getPrivate();
    }

    void* dataPointerShared() const {
        MOZ_ASSERT(isSharedMemory());
        return getPrivate();
    }

    static bool construct(JSContext* cx, unsigned argc, Value* vp);

    static bool getInt8Impl(JSContext* cx, const CallArgs& args);
    static bool fun_getInt8(JSContext* cx, unsigned argc, Value* vp);

    static bool getUint8Impl(JSContext* cx, const CallArgs& args);
    static bool fun_getUint8(JSContext* cx, unsigned argc, Value* vp);

    static bool getInt16Impl(JSContext* cx, const CallArgs& args);
    static bool fun_getInt16(JSContext* cx, unsigned argc, Value* vp);

    static bool getUint16Impl(JSContext* cx, const CallArgs& args);
    static bool fun_getUint16(JSContext* cx, unsigned argc, Value* vp);

    static bool getInt32Impl(JSContext* cx, const CallArgs& args);
    static bool fun_getInt32(JSContext* cx, unsigned argc, Value* vp);

    static bool getUint32Impl(JSContext* cx, const CallArgs& args);
    static bool fun_getUint32(JSContext* cx, unsigned argc, Value* vp);

    static bool getFloat32Impl(JSContext* cx, const CallArgs& args);
    static bool fun_getFloat32(JSContext* cx, unsigned argc, Value* vp);

    static bool getFloat64Impl(JSContext* cx, const CallArgs& args);
    static bool fun_getFloat64(JSContext* cx, unsigned argc, Value* vp);

    static bool setInt8Impl(JSContext* cx, const CallArgs& args);
    static bool fun_setInt8(JSContext* cx, unsigned argc, Value* vp);

    static bool setUint8Impl(JSContext* cx, const CallArgs& args);
    static bool fun_setUint8(JSContext* cx, unsigned argc, Value* vp);

    static bool setInt16Impl(JSContext* cx, const CallArgs& args);
    static bool fun_setInt16(JSContext* cx, unsigned argc, Value* vp);

    static bool setUint16Impl(JSContext* cx, const CallArgs& args);
    static bool fun_setUint16(JSContext* cx, unsigned argc, Value* vp);

    static bool setInt32Impl(JSContext* cx, const CallArgs& args);
    static bool fun_setInt32(JSContext* cx, unsigned argc, Value* vp);

    static bool setUint32Impl(JSContext* cx, const CallArgs& args);
    static bool fun_setUint32(JSContext* cx, unsigned argc, Value* vp);

    static bool setFloat32Impl(JSContext* cx, const CallArgs& args);
    static bool fun_setFloat32(JSContext* cx, unsigned argc, Value* vp);

    static bool setFloat64Impl(JSContext* cx, const CallArgs& args);
    static bool fun_setFloat64(JSContext* cx, unsigned argc, Value* vp);

    static bool initClass(JSContext* cx);
    template<typename NativeType>
    static bool read(JSContext* cx, Handle<DataViewObject*> obj, const CallArgs& args,
                     NativeType* val);
    template<typename NativeType>
    static bool write(JSContext* cx, Handle<DataViewObject*> obj, const CallArgs& args);

    void notifyBufferDetached(void* newData);

  private:
    static const JSFunctionSpec methods[];
    static const JSPropertySpec properties[];
};

} // namespace js

#endif /* vm_DataViewObject_h */
