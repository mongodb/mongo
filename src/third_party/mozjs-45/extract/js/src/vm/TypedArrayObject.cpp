/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/TypedArrayObject.h"

#include "mozilla/Alignment.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/PodOperations.h"

#include <string.h>
#ifndef XP_WIN
# include <sys/mman.h>
#endif

#include "jsapi.h"
#include "jsarray.h"
#include "jscntxt.h"
#include "jscpucfg.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jstypes.h"
#include "jsutil.h"
#ifdef XP_WIN
# include "jswin.h"
#endif
#include "jswrapper.h"

#include "builtin/TypedObjectConstants.h"
#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "js/Conversions.h"
#include "vm/ArrayBufferObject.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/TypedArrayCommon.h"
#include "vm/WrapperObject.h"

#include "jsatominlines.h"

#include "vm/ArrayBufferObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::IsNaN;
using mozilla::NegativeInfinity;
using mozilla::PodCopy;
using mozilla::PositiveInfinity;
using JS::CanonicalizeNaN;
using JS::GenericNaN;
using JS::ToInt32;
using JS::ToUint32;

/*
 * TypedArrayObject
 *
 * The non-templated base class for the specific typed implementations.
 * This class holds all the member variables that are used by
 * the subclasses.
 */

/* static */ int
TypedArrayObject::lengthOffset()
{
    return NativeObject::getFixedSlotOffset(LENGTH_SLOT);
}

/* static */ int
TypedArrayObject::dataOffset()
{
    return NativeObject::getPrivateDataOffset(DATA_SLOT);
}

void
TypedArrayObject::neuter(void* newData)
{
    MOZ_ASSERT(!isSharedMemory());
    setFixedSlot(TypedArrayObject::LENGTH_SLOT, Int32Value(0));
    setFixedSlot(TypedArrayObject::BYTEOFFSET_SLOT, Int32Value(0));
    setPrivate(newData);
}

/* static */ bool
TypedArrayObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().is<TypedArrayObject>();
}

/* static */ bool
TypedArrayObject::ensureHasBuffer(JSContext* cx, Handle<TypedArrayObject*> tarray)
{
    if (tarray->hasBuffer())
        return true;

    Rooted<ArrayBufferObject*> buffer(cx, ArrayBufferObject::create(cx, tarray->byteLength()));
    if (!buffer)
        return false;

    if (!buffer->addView(cx, tarray))
        return false;

    // tarray is not shared, because if it were it would have a buffer.
    memcpy(buffer->dataPointer(), tarray->viewDataUnshared(), tarray->byteLength());
    tarray->setPrivate(buffer->dataPointer());

    tarray->setFixedSlot(TypedArrayObject::BUFFER_SLOT, ObjectValue(*buffer));

    // Notify compiled jit code that the base pointer has moved.
    MarkObjectStateChange(cx, tarray);

    return true;
}

/* static */ void
TypedArrayObject::trace(JSTracer* trc, JSObject* objArg)
{
    // Handle all tracing required when the object has a buffer.
    ArrayBufferViewObject::trace(trc, objArg);

    // If the typed array doesn't have a buffer, it must have a lazy buffer and
    // its data pointer must point to its inline data. Watch for cases where
    // the GC moved this object and fix up its data pointer.
    TypedArrayObject& obj = objArg->as<TypedArrayObject>();
    if (!obj.hasBuffer() && obj.getPrivate() != obj.fixedData(FIXED_DATA_START)) {
        void* oldData = obj.getPrivate();
        void* newData = obj.fixedData(FIXED_DATA_START);

        obj.setPrivateUnbarriered(newData);

        // If this is a minor GC, set a forwarding pointer for the array data.
        // This can always be done inline, as AllocKindForLazyBuffer ensures
        // there is at least a pointer's worth of inline data.
        trc->runtime()->gc.nursery.maybeSetForwardingPointer(trc, oldData, newData, true);
    }
}

/* Helper clamped uint8_t type */

uint32_t JS_FASTCALL
js::ClampDoubleToUint8(const double x)
{
    // Not < so that NaN coerces to 0
    if (!(x >= 0))
        return 0;

    if (x > 255)
        return 255;

    double toTruncate = x + 0.5;
    uint8_t y = uint8_t(toTruncate);

    /*
     * now val is rounded to nearest, ties rounded up.  We want
     * rounded to nearest ties to even, so check whether we had a
     * tie.
     */
    if (y == toTruncate) {
        /*
         * It was a tie (since adding 0.5 gave us the exact integer
         * we want).  Since we rounded up, we either already have an
         * even number or we have an odd number but the number we
         * want is one less.  So just unconditionally masking out the
         * ones bit should do the trick to get us the value we
         * want.
         */
        return y & ~1;
    }

    return y;
}

template<typename ElementType>
static inline JSObject*
NewArray(JSContext* cx, uint32_t nelements);

namespace {

// We allow nullptr for newTarget for all the creation methods, to allow for
// JSFriendAPI functions that don't care about subclassing
static bool
GetPrototypeForInstance(JSContext* cx, HandleObject newTarget, MutableHandleObject proto)
{
    if (newTarget) {
        if (!GetPrototypeFromConstructor(cx, newTarget, proto))
            return false;
    } else {
        proto.set(nullptr);
    }
    return true;
}

template<typename NativeType>
class TypedArrayObjectTemplate : public TypedArrayObject
{
    friend class TypedArrayObject;

  public:
    typedef NativeType ElementType;

    static MOZ_CONSTEXPR Scalar::Type ArrayTypeID() { return TypeIDOfType<NativeType>::id; }
    static bool ArrayTypeIsUnsigned() { return TypeIsUnsigned<NativeType>(); }
    static bool ArrayTypeIsFloatingPoint() { return TypeIsFloatingPoint<NativeType>(); }

    static const size_t BYTES_PER_ELEMENT = sizeof(NativeType);

    static JSObject*
    createPrototype(JSContext* cx, JSProtoKey key)
    {
        Handle<GlobalObject*> global = cx->global();
        RootedObject typedArrayProto(cx, GlobalObject::getOrCreateTypedArrayPrototype(cx, global));
        if (!typedArrayProto)
            return nullptr;

        const Class* clasp = TypedArrayObject::protoClassForType(ArrayTypeID());
        return global->createBlankPrototypeInheriting(cx, clasp, typedArrayProto);
    }

    static JSObject*
    createConstructor(JSContext* cx, JSProtoKey key)
    {
        Handle<GlobalObject*> global = cx->global();
        RootedFunction ctorProto(cx, GlobalObject::getOrCreateTypedArrayConstructor(cx, global));
        if (!ctorProto)
            return nullptr;

        return NewFunctionWithProto(cx, class_constructor, 3,
                                    JSFunction::NATIVE_CTOR, nullptr,
                                    ClassName(key, cx),
                                    ctorProto, gc::AllocKind::FUNCTION,
                                    SingletonObject);
    }

    static bool
    finishClassInit(JSContext* cx, HandleObject ctor, HandleObject proto)
    {
        RootedValue bytesValue(cx, Int32Value(BYTES_PER_ELEMENT));
        if (!DefineProperty(cx, ctor, cx->names().BYTES_PER_ELEMENT, bytesValue,
                            nullptr, nullptr, JSPROP_PERMANENT | JSPROP_READONLY) ||
            !DefineProperty(cx, proto, cx->names().BYTES_PER_ELEMENT, bytesValue,
                            nullptr, nullptr, JSPROP_PERMANENT | JSPROP_READONLY))
        {
            return false;
        }

        RootedFunction fun(cx);
        fun = NewNativeFunction(cx, ArrayBufferObject::createTypedArrayFromBuffer<NativeType>,
                                0, nullptr);
        if (!fun)
            return false;

        cx->global()->setCreateArrayFromBuffer<NativeType>(fun);
        return true;
    }

    static inline const Class* instanceClass()
    {
        return TypedArrayObject::classForType(ArrayTypeID());
    }

    static bool is(HandleValue v) {
        return v.isObject() && v.toObject().hasClass(instanceClass());
    }

    static void
    setIndexValue(TypedArrayObject& tarray, uint32_t index, double d)
    {
        // If the array is an integer array, we only handle up to
        // 32-bit ints from this point on.  if we want to handle
        // 64-bit ints, we'll need some changes.

        // Assign based on characteristics of the destination type
        if (ArrayTypeIsFloatingPoint()) {
            setIndex(tarray, index, NativeType(d));
        } else if (ArrayTypeIsUnsigned()) {
            MOZ_ASSERT(sizeof(NativeType) <= 4);
            uint32_t n = ToUint32(d);
            setIndex(tarray, index, NativeType(n));
        } else if (ArrayTypeID() == Scalar::Uint8Clamped) {
            // The uint8_clamped type has a special rounding converter
            // for doubles.
            setIndex(tarray, index, NativeType(d));
        } else {
            MOZ_ASSERT(sizeof(NativeType) <= 4);
            int32_t n = ToInt32(d);
            setIndex(tarray, index, NativeType(n));
        }
    }

    static TypedArrayObject*
    makeProtoInstance(JSContext* cx, HandleObject proto, AllocKind allocKind)
    {
        MOZ_ASSERT(proto);

        JSObject* obj = NewObjectWithClassProto(cx, instanceClass(), proto, allocKind);
        return obj ? &obj->as<TypedArrayObject>() : nullptr;
    }

    static TypedArrayObject*
    makeTypedInstance(JSContext* cx, uint32_t len, gc::AllocKind allocKind)
    {
        const Class* clasp = instanceClass();
        if (len * sizeof(NativeType) >= TypedArrayObject::SINGLETON_BYTE_LENGTH) {
            JSObject* obj = NewBuiltinClassInstance(cx, clasp, allocKind, SingletonObject);
            if (!obj)
                return nullptr;
            return &obj->as<TypedArrayObject>();
        }

        jsbytecode* pc;
        RootedScript script(cx, cx->currentScript(&pc));
        NewObjectKind newKind = GenericObject;
        if (script && ObjectGroup::useSingletonForAllocationSite(script, pc, clasp))
            newKind = SingletonObject;
        RootedObject obj(cx, NewBuiltinClassInstance(cx, clasp, allocKind, newKind));
        if (!obj)
            return nullptr;

        if (script && !ObjectGroup::setAllocationSiteObjectGroup(cx, script, pc, obj,
                                                                 newKind == SingletonObject))
        {
            return nullptr;
        }

        return &obj->as<TypedArrayObject>();
    }

    static TypedArrayObject*
    makeInstance(JSContext* cx, Handle<ArrayBufferObjectMaybeShared*> buffer, uint32_t byteOffset, uint32_t len,
                 HandleObject proto)
    {
        MOZ_ASSERT_IF(!buffer, byteOffset == 0);

        gc::AllocKind allocKind = buffer
                                  ? GetGCObjectKind(instanceClass())
                                  : AllocKindForLazyBuffer(len * sizeof(NativeType));

        // Subclassing mandates that we hand in the proto every time. Most of
        // the time, though, that [[Prototype]] will not be interesting. If
        // it isn't, we can do some more TI optimizations.
        RootedObject checkProto(cx);
        if (!GetBuiltinPrototype(cx, JSCLASS_CACHED_PROTO_KEY(instanceClass()), &checkProto))
            return nullptr;

        AutoSetNewObjectMetadata metadata(cx);
        Rooted<TypedArrayObject*> obj(cx);
        if (proto && proto != checkProto)
            obj = makeProtoInstance(cx, proto, allocKind);
        else
            obj = makeTypedInstance(cx, len, allocKind);
        if (!obj)
            return nullptr;

        bool isSharedMemory = buffer && IsSharedArrayBuffer(buffer.get());

        obj->setFixedSlot(TypedArrayObject::BUFFER_SLOT, ObjectOrNullValue(buffer));
        // This is invariant.  Self-hosting code that sets BUFFER_SLOT
        // (if it does) must maintain it, should it need to.
        if (isSharedMemory)
            obj->setIsSharedMemory();

        if (buffer) {
            obj->initViewData(buffer->dataPointerEither() + byteOffset);

            // If the buffer is for an inline typed object, the data pointer
            // may be in the nursery, so include a barrier to make sure this
            // object is updated if that typed object moves.
            if (!IsInsideNursery(obj) && cx->runtime()->gc.nursery.isInside(buffer->dataPointerEither())) {
                // Shared buffer data should never be nursery-allocated, so
                // we need to fail here if isSharedMemory.  However, mmap()
                // can place a SharedArrayRawBuffer up against the bottom end
                // of the nursery, and a zero-length buffer will erroneously be
                // perceived as being inside the nursery; sidestep that.
                if (isSharedMemory) {
                    MOZ_ASSERT(buffer->byteLength() == 0 &&
                               cx->runtime()->gc.nursery.start() ==
                                   buffer->dataPointerEither().unwrapValue());
                } else {
                    cx->runtime()->gc.storeBuffer.putWholeCell(obj);
                }
            }
        } else {
            void* data = obj->fixedData(FIXED_DATA_START);
            obj->initPrivate(data);
            memset(data, 0, len * sizeof(NativeType));
        }

        obj->setFixedSlot(TypedArrayObject::LENGTH_SLOT, Int32Value(len));
        obj->setFixedSlot(TypedArrayObject::BYTEOFFSET_SLOT, Int32Value(byteOffset));

#ifdef DEBUG
        if (buffer) {
            uint32_t arrayByteLength = obj->byteLength();
            uint32_t arrayByteOffset = obj->byteOffset();
            uint32_t bufferByteLength = buffer->byteLength();
            // Unwraps are safe: both are for the pointer value.
            if (IsArrayBuffer(buffer.get())) {
                MOZ_ASSERT_IF(!AsArrayBuffer(buffer.get()).isNeutered(),
                              buffer->dataPointerEither().unwrap(/*safe*/) <= obj->viewDataEither().unwrap(/*safe*/));
            }
            MOZ_ASSERT(bufferByteLength - arrayByteOffset >= arrayByteLength);
            MOZ_ASSERT(arrayByteOffset <= bufferByteLength);
        }

        // Verify that the private slot is at the expected place
        MOZ_ASSERT(obj->numFixedSlots() == TypedArrayObject::DATA_SLOT);
#endif

        // ArrayBufferObjects track their views to support neutering.
        if (buffer && buffer->is<ArrayBufferObject>()) {
            if (!buffer->as<ArrayBufferObject>().addView(cx, obj))
                return nullptr;
        }

        return obj;
    }

    static TypedArrayObject*
    makeInstance(JSContext* cx, Handle<ArrayBufferObjectMaybeShared*> buffer,
                 uint32_t byteOffset, uint32_t len)
    {
        RootedObject proto(cx, nullptr);
        return makeInstance(cx, buffer, byteOffset, len, proto);
    }

    /*
     * new [Type]Array(length)
     * new [Type]Array(otherTypedArray)
     * new [Type]Array(JSArray)
     * new [Type]Array(ArrayBuffer, [optional] byteOffset, [optional] length)
     */
    static bool
    class_constructor(JSContext* cx, unsigned argc, Value* vp)
    {
        CallArgs args = CallArgsFromVp(argc, vp);

        if (!ThrowIfNotConstructing(cx, args, "typed array"))
            return false;

        JSObject* obj = create(cx, args);
        if (!obj)
            return false;
        args.rval().setObject(*obj);
        return true;
    }

    static JSObject*
    create(JSContext* cx, const CallArgs& args)
    {
        MOZ_ASSERT(args.isConstructing());
        RootedObject newTarget(cx, &args.newTarget().toObject());

        /* () or (number) */
        uint32_t len = 0;
        if (args.length() == 0 || ValueIsLength(args[0], &len))
            return fromLength(cx, len, newTarget);

        /* (not an object) */
        if (!args[0].isObject()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return nullptr;
        }

        RootedObject dataObj(cx, &args.get(0).toObject());

        /*
         * (typedArray)
         * (sharedTypedArray)
         * (type[] array)
         *
         * Otherwise create a new typed array and copy elements 0..len-1
         * properties from the object, treating it as some sort of array.
         * Note that offset and length will be ignored.  Note that a
         * shared array's values are copied here.
         */
        if (!UncheckedUnwrap(dataObj)->is<ArrayBufferObjectMaybeShared>())
            return fromArray(cx, dataObj, newTarget);

        /* (ArrayBuffer, [byteOffset, [length]]) */
        RootedObject proto(cx);
        if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
            return nullptr;

        int32_t byteOffset = 0;
        int32_t length = -1;

        if (args.length() > 1) {
            if (!ToInt32(cx, args[1], &byteOffset))
                return nullptr;
            if (byteOffset < 0) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                     JSMSG_TYPED_ARRAY_NEGATIVE_ARG, "1");
                return nullptr;
            }

            if (args.length() > 2) {
                if (!ToInt32(cx, args[2], &length))
                    return nullptr;
                if (length < 0) {
                    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                         JSMSG_TYPED_ARRAY_NEGATIVE_ARG, "2");
                    return nullptr;
                }
            }
        }

        return fromBufferWithProto(cx, dataObj, byteOffset, length, proto);
    }

  public:
    static JSObject*
    fromBuffer(JSContext* cx, HandleObject bufobj, uint32_t byteOffset, int32_t lengthInt) {
        return fromBufferWithProto(cx, bufobj, byteOffset, lengthInt, nullptr);
    }

    static JSObject*
    fromBufferWithProto(JSContext* cx, HandleObject bufobj, uint32_t byteOffset, int32_t lengthInt,
                        HandleObject proto)
    {
        ESClassValue cls;
        if (!GetBuiltinClass(cx, bufobj, &cls))
            return nullptr;
        if (cls != ESClass_ArrayBuffer && cls != ESClass_SharedArrayBuffer) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return nullptr;
        }

        MOZ_ASSERT(IsArrayBuffer(bufobj) || IsSharedArrayBuffer(bufobj) || bufobj->is<ProxyObject>());
        if (bufobj->is<ProxyObject>()) {
            /*
             * Normally, NonGenericMethodGuard handles the case of transparent
             * wrappers. However, we have a peculiar situation: we want to
             * construct the new typed array in the compartment of the buffer,
             * so that the typed array can point directly at their buffer's
             * data without crossing compartment boundaries. So we use the
             * machinery underlying NonGenericMethodGuard directly to proxy the
             * native call. We will end up with a wrapper in the origin
             * compartment for a view in the target compartment referencing the
             * ArrayBufferObject in that same compartment.
             */
            JSObject* wrapped = CheckedUnwrap(bufobj);
            if (!wrapped) {
                JS_ReportError(cx, "Permission denied to access object");
                return nullptr;
            }
            if (IsArrayBuffer(wrapped) || IsSharedArrayBuffer(wrapped)) {
                /*
                 * And for even more fun, the new view's prototype should be
                 * set to the origin compartment's prototype object, not the
                 * target's (specifically, the actual view in the target
                 * compartment will use as its prototype a wrapper around the
                 * origin compartment's view.prototype object).
                 *
                 * Rather than hack some crazy solution together, implement
                 * this all using a private helper function, created when
                 * ArrayBufferObject was initialized and cached in the global.
                 * This reuses all the existing cross-compartment crazy so we
                 * don't have to do anything *uniquely* crazy here.
                 */

                RootedObject protoRoot(cx, proto);
                if (!protoRoot) {
                    if (!GetBuiltinPrototype(cx, JSCLASS_CACHED_PROTO_KEY(instanceClass()), &protoRoot))
                        return nullptr;
                }

                InvokeArgs args(cx);
                if (!args.init(3))
                    return nullptr;

                args.setCallee(cx->compartment()->maybeGlobal()->createArrayFromBuffer<NativeType>());
                args.setThis(ObjectValue(*bufobj));
                args[0].setNumber(byteOffset);
                args[1].setInt32(lengthInt);
                args[2].setObject(*protoRoot);

                if (!Invoke(cx, args))
                    return nullptr;
                return &args.rval().toObject();
            }
        }

        if (!IsArrayBuffer(bufobj) && !IsSharedArrayBuffer(bufobj)) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return nullptr; // must be arrayBuffer
        }

        Rooted<ArrayBufferObjectMaybeShared*> buffer(cx);
        if (IsArrayBuffer(bufobj)) {
            ArrayBufferObject& buf = AsArrayBuffer(bufobj);
            if (buf.isNeutered()) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
                return nullptr;
            }

            buffer = static_cast<ArrayBufferObjectMaybeShared*>(&buf);
        } else {
            buffer = static_cast<ArrayBufferObjectMaybeShared*>(&AsSharedArrayBuffer(bufobj));
        }

        if (byteOffset > buffer->byteLength() || byteOffset % sizeof(NativeType) != 0) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return nullptr; // invalid byteOffset
        }

        uint32_t len;
        if (lengthInt == -1) {
            len = (buffer->byteLength() - byteOffset) / sizeof(NativeType);
            if (len * sizeof(NativeType) != buffer->byteLength() - byteOffset) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                     JSMSG_TYPED_ARRAY_BAD_ARGS);
                return nullptr; // given byte array doesn't map exactly to sizeof(NativeType) * N
            }
        } else {
            len = uint32_t(lengthInt);
        }

        // Go slowly and check for overflow.
        uint32_t arrayByteLength = len * sizeof(NativeType);
        if (len >= INT32_MAX / sizeof(NativeType) || byteOffset >= INT32_MAX - arrayByteLength) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return nullptr; // overflow when calculating byteOffset + len * sizeof(NativeType)
        }

        if (arrayByteLength + byteOffset > buffer->byteLength()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return nullptr; // byteOffset + len is too big for the arraybuffer
        }

        return makeInstance(cx, buffer, byteOffset, len, proto);
    }

    static bool
    maybeCreateArrayBuffer(JSContext* cx, uint32_t nelements, MutableHandle<ArrayBufferObject*> buffer)
    {
        static_assert(INLINE_BUFFER_LIMIT % sizeof(NativeType) == 0,
                      "ArrayBuffer inline storage shouldn't waste any space");

        if (nelements <= INLINE_BUFFER_LIMIT / sizeof(NativeType)) {
            // The array's data can be inline, and the buffer created lazily.
            return true;
        }

        if (nelements >= INT32_MAX / sizeof(NativeType)) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                 JSMSG_NEED_DIET, "size and count");
            return false;
        }

        ArrayBufferObject* buf = ArrayBufferObject::create(cx, nelements * sizeof(NativeType));
        if (!buf)
            return false;

        buffer.set(buf);
        return true;
    }

    static JSObject*
    fromLength(JSContext* cx, uint32_t nelements, HandleObject newTarget = nullptr)
    {
        RootedObject proto(cx);
        if (!GetPrototypeForInstance(cx, newTarget, &proto))
            return nullptr;

        Rooted<ArrayBufferObject*> buffer(cx);
        if (!maybeCreateArrayBuffer(cx, nelements, &buffer))
            return nullptr;

        return makeInstance(cx, buffer, 0, nelements, proto);
    }

    static JSObject*
    fromArray(JSContext* cx, HandleObject other, HandleObject newTarget = nullptr);

    static const NativeType
    getIndex(JSObject* obj, uint32_t index)
    {
        TypedArrayObject& tarray = obj->as<TypedArrayObject>();
        MOZ_ASSERT(index < tarray.length());
        return jit::AtomicOperations::loadSafeWhenRacy(tarray.viewDataEither().cast<NativeType*>() + index);
    }

    static void
    setIndex(TypedArrayObject& tarray, uint32_t index, NativeType val)
    {
        MOZ_ASSERT(index < tarray.length());
        jit::AtomicOperations::storeSafeWhenRacy(tarray.viewDataEither().cast<NativeType*>() + index, val);
    }

    static Value getIndexValue(JSObject* tarray, uint32_t index);
};

typedef TypedArrayObjectTemplate<int8_t> Int8Array;
typedef TypedArrayObjectTemplate<uint8_t> Uint8Array;
typedef TypedArrayObjectTemplate<int16_t> Int16Array;
typedef TypedArrayObjectTemplate<uint16_t> Uint16Array;
typedef TypedArrayObjectTemplate<int32_t> Int32Array;
typedef TypedArrayObjectTemplate<uint32_t> Uint32Array;
typedef TypedArrayObjectTemplate<float> Float32Array;
typedef TypedArrayObjectTemplate<double> Float64Array;
typedef TypedArrayObjectTemplate<uint8_clamped> Uint8ClampedArray;

} /* anonymous namespace */

template<typename T>
struct TypedArrayObject::OfType
{
    typedef TypedArrayObjectTemplate<T> Type;
};

template<typename T>
/* static */ JSObject*
TypedArrayObjectTemplate<T>::fromArray(JSContext* cx, HandleObject other,
                                       HandleObject newTarget /* = nullptr */)
{
    // Allow nullptr newTarget for FriendAPI methods, which don't care about
    // subclassing.
    RootedObject proto(cx);

    uint32_t len;
    if (IsAnyTypedArray(other)) {
        if (!GetPrototypeForInstance(cx, newTarget, &proto))
            return nullptr;

        if (AnyTypedArrayIsDetached(other)) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
            return nullptr;
        }
        len = AnyTypedArrayLength(other);
    } else {
        if (!GetLengthProperty(cx, other, &len))
            return nullptr;
        if (!GetPrototypeForInstance(cx, newTarget, &proto))
            return nullptr;
    }

    Rooted<ArrayBufferObject*> buffer(cx);
    if (!maybeCreateArrayBuffer(cx, len, &buffer))
        return nullptr;

    Rooted<TypedArrayObject*> obj(cx, makeInstance(cx, buffer, 0, len, proto));
    if (!obj || !TypedArrayMethods<TypedArrayObject>::setFromArrayLike(cx, obj, other, len))
        return nullptr;
    return obj;
}

bool
TypedArrayConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportError(cx, "%%TypedArray%% calling/constructing not implemented yet");
    return false;
}

static bool
FinishTypedArrayInit(JSContext* cx, HandleObject ctor, HandleObject proto)
{
    // Define `values` and `@@iterator` manually, because they are supposed to be the same object.
    RootedId name(cx, NameToId(cx->names().values));
    RootedFunction fun(cx, GetSelfHostedFunction(cx, "TypedArrayValues", name, 0));
    if (!fun)
        return false;

    RootedValue funValue(cx, ObjectValue(*fun));
    if (!DefineProperty(cx, proto, cx->names().values, funValue, nullptr, nullptr, 0))
        return false;

    RootedId iteratorId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
    if (!DefineProperty(cx, proto, iteratorId, funValue, nullptr, nullptr, 0))
        return false;

    return true;
}

/*
 * These next 3 functions are brought to you by the buggy GCC we use to build
 * B2G ICS. Older GCC versions have a bug in which they fail to compile
 * reinterpret_casts of templated functions with the message: "insufficient
 * contextual information to determine type". JS_PSG needs to
 * reinterpret_cast<JSGetterOp>, so this causes problems for us here.
 *
 * We could restructure all this code to make this nicer, but since ICS isn't
 * going to be around forever (and since this bug is fixed with the newer GCC
 * versions we use on JB and KK), the workaround here is designed for ease of
 * removal. When you stop seeing ICS Emulator builds on TBPL, remove these 3
 * JSNatives and insert the templated callee directly into the JS_PSG below.
 */
static bool
TypedArray_lengthGetter(JSContext* cx, unsigned argc, Value* vp)
{
    return TypedArrayObject::Getter<TypedArrayObject::lengthValue>(cx, argc, vp); \
}

static bool
TypedArray_byteLengthGetter(JSContext* cx, unsigned argc, Value* vp)
{
    return TypedArrayObject::Getter<TypedArrayObject::byteLengthValue>(cx, argc, vp);
}

static bool
TypedArray_byteOffsetGetter(JSContext* cx, unsigned argc, Value* vp)
{
    return TypedArrayObject::Getter<TypedArrayObject::byteOffsetValue>(cx, argc, vp);
}

bool
BufferGetterImpl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(TypedArrayObject::is(args.thisv()));
    Rooted<TypedArrayObject*> tarray(cx, &args.thisv().toObject().as<TypedArrayObject>());
    if (!TypedArrayObject::ensureHasBuffer(cx, tarray))
        return false;
    args.rval().set(TypedArrayObject::bufferValue(tarray));
    return true;
}

static bool
TypedArray_bufferGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<TypedArrayObject::is, BufferGetterImpl>(cx, args);
}

/* static */ const JSPropertySpec
TypedArrayObject::protoAccessors[] = {
    JS_PSG("length", TypedArray_lengthGetter, 0),
    JS_PSG("buffer", TypedArray_bufferGetter, 0),
    JS_PSG("byteLength", TypedArray_byteLengthGetter, 0),
    JS_PSG("byteOffset", TypedArray_byteOffsetGetter, 0),
    JS_PS_END
};

/* static */ bool
TypedArrayObject::set(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<TypedArrayObject::is,
                                TypedArrayMethods<TypedArrayObject>::set>(cx, args);
}

/* static */ const JSFunctionSpec
TypedArrayObject::protoFunctions[] = {
    JS_SELF_HOSTED_FN("subarray", "TypedArraySubarray", 2, 0),
#if 0 /* disabled until perf-testing is completed */
    JS_SELF_HOSTED_FN("set", "TypedArraySet", 2, 0),
#else
    JS_FN("set", TypedArrayObject::set, 2, 0),
#endif
    JS_SELF_HOSTED_FN("copyWithin", "TypedArrayCopyWithin", 3, 0),
    JS_SELF_HOSTED_FN("every", "TypedArrayEvery", 2, 0),
    JS_SELF_HOSTED_FN("fill", "TypedArrayFill", 3, 0),
    JS_SELF_HOSTED_FN("filter", "TypedArrayFilter", 2, 0),
    JS_SELF_HOSTED_FN("find", "TypedArrayFind", 2, 0),
    JS_SELF_HOSTED_FN("findIndex", "TypedArrayFindIndex", 2, 0),
    JS_SELF_HOSTED_FN("forEach", "TypedArrayForEach", 2, 0),
    JS_SELF_HOSTED_FN("indexOf", "TypedArrayIndexOf", 2, 0),
    JS_SELF_HOSTED_FN("join", "TypedArrayJoin", 1, 0),
    JS_SELF_HOSTED_FN("lastIndexOf", "TypedArrayLastIndexOf", 2, 0),
    JS_SELF_HOSTED_FN("map", "TypedArrayMap", 2, 0),
    JS_SELF_HOSTED_FN("reduce", "TypedArrayReduce", 1, 0),
    JS_SELF_HOSTED_FN("reduceRight", "TypedArrayReduceRight", 1, 0),
    JS_SELF_HOSTED_FN("reverse", "TypedArrayReverse", 0, 0),
    JS_SELF_HOSTED_FN("slice", "TypedArraySlice", 2, 0),
    JS_SELF_HOSTED_FN("some", "TypedArraySome", 2, 0),
    JS_SELF_HOSTED_FN("entries", "TypedArrayEntries", 0, 0),
    JS_SELF_HOSTED_FN("keys", "TypedArrayKeys", 0, 0),
    // Both of these are actually defined to the same object in FinishTypedArrayInit.
    JS_SELF_HOSTED_FN("values", "TypedArrayValues", 0, JSPROP_DEFINE_LATE),
    JS_SELF_HOSTED_SYM_FN(iterator, "TypedArrayValues", 0, JSPROP_DEFINE_LATE),
    JS_SELF_HOSTED_FN("includes", "TypedArrayIncludes", 2, 0),
    JS_FS_END
};

/* static */ const JSFunctionSpec
TypedArrayObject::staticFunctions[] = {
    JS_SELF_HOSTED_FN("from", "TypedArrayStaticFrom", 3, 0),
    JS_SELF_HOSTED_FN("of", "TypedArrayStaticOf", 0, 0),
    JS_FS_END
};

/* static */ const Class
TypedArrayObject::sharedTypedArrayPrototypeClass = {
    // Actually ({}).toString.call(%TypedArray%.prototype) should throw,
    // because %TypedArray%.prototype lacks the the typed array internal
    // slots.  (It's not clear this is desirable -- particularly applied to
    // the actual typed array prototypes, see below -- but it's what ES6
    // draft 20140824 requires.)  But this is about as much as we can do
    // until we implement @@toStringTag.
    "???",
    JSCLASS_HAS_CACHED_PROTO(JSProto_TypedArray),
    nullptr,                /* addProperty */
    nullptr,                /* delProperty */
    nullptr,                /* getProperty */
    nullptr,                /* setProperty */
    nullptr,                /* enumerate */
    nullptr,                /* resolve */
    nullptr,                /* mayResolve */
    nullptr,                /* finalize */
    nullptr,                /* call */
    nullptr,                /* hasInstance */
    nullptr,                /* construct */
    nullptr,                /* trace */
    {
        GenericCreateConstructor<TypedArrayConstructor, 3, gc::AllocKind::FUNCTION>,
        GenericCreatePrototype,
        TypedArrayObject::staticFunctions,
        nullptr,
        TypedArrayObject::protoFunctions,
        TypedArrayObject::protoAccessors,
        FinishTypedArrayInit,
        ClassSpec::DontDefineConstructor
    }
};

template<typename T>
bool
ArrayBufferObject::createTypedArrayFromBufferImpl(JSContext* cx, const CallArgs& args)
{
    typedef TypedArrayObjectTemplate<T> ArrayType;
    MOZ_ASSERT(IsArrayBuffer(args.thisv()));
    MOZ_ASSERT(args.length() == 3);

    Rooted<JSObject*> buffer(cx, &args.thisv().toObject());
    Rooted<JSObject*> proto(cx, &args[2].toObject());

    Rooted<JSObject*> obj(cx);
    double byteOffset = args[0].toNumber();
    MOZ_ASSERT(0 <= byteOffset);
    MOZ_ASSERT(byteOffset <= UINT32_MAX);
    MOZ_ASSERT(byteOffset == uint32_t(byteOffset));
    obj = ArrayType::fromBufferWithProto(cx, buffer, uint32_t(byteOffset), args[1].toInt32(),
                                         proto);
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

template<typename T>
bool
ArrayBufferObject::createTypedArrayFromBuffer(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsArrayBuffer, createTypedArrayFromBufferImpl<T> >(cx, args);
}

// this default implementation is only valid for integer types
// less than 32-bits in size.
template<typename NativeType>
Value
TypedArrayObjectTemplate<NativeType>::getIndexValue(JSObject* tarray, uint32_t index)
{
    static_assert(sizeof(NativeType) < 4,
                  "this method must only handle NativeType values that are "
                  "always exact int32_t values");

    return Int32Value(getIndex(tarray, index));
}

namespace {

// and we need to specialize for 32-bit integers and floats
template<>
Value
TypedArrayObjectTemplate<int32_t>::getIndexValue(JSObject* tarray, uint32_t index)
{
    return Int32Value(getIndex(tarray, index));
}

template<>
Value
TypedArrayObjectTemplate<uint32_t>::getIndexValue(JSObject* tarray, uint32_t index)
{
    uint32_t val = getIndex(tarray, index);
    return NumberValue(val);
}

template<>
Value
TypedArrayObjectTemplate<float>::getIndexValue(JSObject* tarray, uint32_t index)
{
    float val = getIndex(tarray, index);
    double dval = val;

    /*
     * Doubles in typed arrays could be typed-punned arrays of integers. This
     * could allow user code to break the engine-wide invariant that only
     * canonical nans are stored into jsvals, which means user code could
     * confuse the engine into interpreting a double-typed jsval as an
     * object-typed jsval.
     *
     * This could be removed for platforms/compilers known to convert a 32-bit
     * non-canonical nan to a 64-bit canonical nan.
     */
    return DoubleValue(CanonicalizeNaN(dval));
}

template<>
Value
TypedArrayObjectTemplate<double>::getIndexValue(JSObject* tarray, uint32_t index)
{
    double val = getIndex(tarray, index);

    /*
     * Doubles in typed arrays could be typed-punned arrays of integers. This
     * could allow user code to break the engine-wide invariant that only
     * canonical nans are stored into jsvals, which means user code could
     * confuse the engine into interpreting a double-typed jsval as an
     * object-typed jsval.
     */
    return DoubleValue(CanonicalizeNaN(val));
}

} /* anonymous namespace */

static NewObjectKind
DataViewNewObjectKind(JSContext* cx, uint32_t byteLength, JSObject* proto)
{
    if (!proto && byteLength >= TypedArrayObject::SINGLETON_BYTE_LENGTH)
        return SingletonObject;
    jsbytecode* pc;
    JSScript* script = cx->currentScript(&pc);
    if (script && ObjectGroup::useSingletonForAllocationSite(script, pc, &DataViewObject::class_))
        return SingletonObject;
    return GenericObject;
}

DataViewObject*
DataViewObject::create(JSContext* cx, uint32_t byteOffset, uint32_t byteLength,
                       Handle<ArrayBufferObject*> arrayBuffer, JSObject* protoArg)
{
    MOZ_ASSERT(byteOffset <= INT32_MAX);
    MOZ_ASSERT(byteLength <= INT32_MAX);
    MOZ_ASSERT(byteOffset + byteLength < UINT32_MAX);
    MOZ_ASSERT(!arrayBuffer || !arrayBuffer->is<SharedArrayBufferObject>());

    RootedObject proto(cx, protoArg);
    RootedObject obj(cx);

    NewObjectKind newKind = DataViewNewObjectKind(cx, byteLength, proto);
    obj = NewObjectWithClassProto(cx, &class_, proto, newKind);
    if (!obj)
        return nullptr;

    if (!proto) {
        if (byteLength >= TypedArrayObject::SINGLETON_BYTE_LENGTH) {
            MOZ_ASSERT(obj->isSingleton());
        } else {
            jsbytecode* pc;
            RootedScript script(cx, cx->currentScript(&pc));
            if (script && !ObjectGroup::setAllocationSiteObjectGroup(cx, script, pc, obj,
                                                                     newKind == SingletonObject))
            {
                return nullptr;
            }
        }
    }

    // Caller should have established these preconditions, and no
    // (non-self-hosted) JS code has had an opportunity to run so nothing can
    // have invalidated them.
    MOZ_ASSERT(byteOffset <= arrayBuffer->byteLength());
    MOZ_ASSERT(byteOffset + byteLength <= arrayBuffer->byteLength());

    DataViewObject& dvobj = obj->as<DataViewObject>();
    dvobj.setFixedSlot(TypedArrayObject::BYTEOFFSET_SLOT, Int32Value(byteOffset));
    dvobj.setFixedSlot(TypedArrayObject::LENGTH_SLOT, Int32Value(byteLength));
    dvobj.setFixedSlot(TypedArrayObject::BUFFER_SLOT, ObjectValue(*arrayBuffer));
    dvobj.initPrivate(arrayBuffer->dataPointer() + byteOffset);

    // Include a barrier if the data view's data pointer is in the nursery, as
    // is done for typed arrays.
    if (!IsInsideNursery(obj) && cx->runtime()->gc.nursery.isInside(arrayBuffer->dataPointer()))
        cx->runtime()->gc.storeBuffer.putWholeCell(obj);

    // Verify that the private slot is at the expected place
    MOZ_ASSERT(dvobj.numFixedSlots() == TypedArrayObject::DATA_SLOT);

    if (!arrayBuffer->addView(cx, &dvobj))
        return nullptr;

    return &dvobj;
}

bool
DataViewObject::getAndCheckConstructorArgs(JSContext* cx, JSObject* bufobj, const CallArgs& args,
                                           uint32_t* byteOffsetPtr, uint32_t* byteLengthPtr)
{
    if (!IsArrayBuffer(bufobj)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                             "DataView", "ArrayBuffer", bufobj->getClass()->name);
        return false;
    }

    Rooted<ArrayBufferObject*> buffer(cx, &AsArrayBuffer(bufobj));
    uint32_t byteOffset = 0;
    uint32_t byteLength = buffer->byteLength();

    if (args.length() > 1) {
        if (!ToUint32(cx, args[1], &byteOffset))
            return false;
        if (byteOffset > INT32_MAX) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_ARG_INDEX_OUT_OF_RANGE, "1");
            return false;
        }
    }

    if (buffer->isNeutered()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
        return false;
    }

    if (args.length() > 1) {
        if (byteOffset > byteLength) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_ARG_INDEX_OUT_OF_RANGE, "1");
            return false;
        }

        if (args.get(2).isUndefined()) {
            byteLength -= byteOffset;
        } else {
            if (!ToUint32(cx, args[2], &byteLength))
                return false;
            if (byteLength > INT32_MAX) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                        JSMSG_ARG_INDEX_OUT_OF_RANGE, "2");
                return false;
            }

            MOZ_ASSERT(byteOffset + byteLength >= byteOffset,
                       "can't overflow: both numbers are less than INT32_MAX");
            if (byteOffset + byteLength > buffer->byteLength()) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                     JSMSG_ARG_INDEX_OUT_OF_RANGE, "1");
                return false;
            }
        }
    }

    /* The sum of these cannot overflow a uint32_t */
    MOZ_ASSERT(byteOffset <= INT32_MAX);
    MOZ_ASSERT(byteLength <= INT32_MAX);


    *byteOffsetPtr = byteOffset;
    *byteLengthPtr = byteLength;

    return true;
}

bool
DataViewObject::constructSameCompartment(JSContext* cx, HandleObject bufobj, const CallArgs& args)
{
    MOZ_ASSERT(args.isConstructing());
    assertSameCompartment(cx, bufobj);

    uint32_t byteOffset, byteLength;
    if (!getAndCheckConstructorArgs(cx, bufobj, args, &byteOffset, &byteLength))
        return false;

    RootedObject proto(cx);
    RootedObject newTarget(cx, &args.newTarget().toObject());
    if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
        return false;

    Rooted<ArrayBufferObject*> buffer(cx, &AsArrayBuffer(bufobj));
    JSObject* obj = DataViewObject::create(cx, byteOffset, byteLength, buffer, proto);
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

// Create a DataView object in another compartment.
//
// ES6 supports creating a DataView in global A (using global A's DataView
// constructor) backed by an ArrayBuffer created in global B.
//
// Our DataViewObject implementation doesn't support a DataView in
// compartment A backed by an ArrayBuffer in compartment B. So in this case,
// we create the DataView in B (!) and return a cross-compartment wrapper.
//
// Extra twist: the spec says the new DataView's [[Prototype]] must be
// A's DataView.prototype. So even though we're creating the DataView in B,
// its [[Prototype]] must be (a cross-compartment wrapper for) the
// DataView.prototype in A.
//
// As if this were not confusing enough, the way we actually do this is also
// tricky. We call compartment A's createDataViewForThis method, passing it
// bufobj as `this`. That calls ArrayBufferObject::createDataViewForThis(),
// which uses CallNonGenericMethod to switch to compartment B so that
// the new DataView is created there.
bool
DataViewObject::constructWrapped(JSContext* cx, HandleObject bufobj, const CallArgs& args)
{
    MOZ_ASSERT(args.isConstructing());
    MOZ_ASSERT(bufobj->is<WrapperObject>());

    JSObject* unwrapped = CheckedUnwrap(bufobj);
    if (!unwrapped) {
        JS_ReportError(cx, "Permission denied to access object");
        return false;
    }

    // NB: This entails the IsArrayBuffer check
    uint32_t byteOffset, byteLength;
    if (!getAndCheckConstructorArgs(cx, unwrapped, args, &byteOffset, &byteLength))
        return false;

    // Make sure to get the [[Prototype]] for the created view from this
    // compartment.
    RootedObject proto(cx);
    RootedObject newTarget(cx, &args.newTarget().toObject());
    if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
        return false;

    Rooted<GlobalObject*> global(cx, cx->compartment()->maybeGlobal());
    if (!proto) {
        proto = global->getOrCreateDataViewPrototype(cx);
        if (!proto)
            return false;
    }

    InvokeArgs args2(cx);
    if (!args2.init(3))
        return false;
    args2.setCallee(global->createDataViewForThis());
    args2.setThis(ObjectValue(*bufobj));
    args2[0].set(PrivateUint32Value(byteOffset));
    args2[1].set(PrivateUint32Value(byteLength));
    args2[2].setObject(*proto);
    if (!Invoke(cx, args2))
        return false;
    args.rval().set(args2.rval());
    return true;
}

bool
DataViewObject::class_constructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "DataView"))
        return false;

    RootedObject bufobj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "DataView constructor", &bufobj))
        return false;

    if (bufobj->is<WrapperObject>())
        return constructWrapped(cx, bufobj, args);
    return constructSameCompartment(cx, bufobj, args);
}

template <typename NativeType>
/* static */ uint8_t*
DataViewObject::getDataPointer(JSContext* cx, Handle<DataViewObject*> obj, uint32_t offset)
{
    const size_t TypeSize = sizeof(NativeType);
    if (offset > UINT32_MAX - TypeSize || offset + TypeSize > obj->byteLength()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_ARG_INDEX_OUT_OF_RANGE, "1");
        return nullptr;
    }

    return static_cast<uint8_t*>(obj->dataPointer()) + offset;
}

static inline bool
needToSwapBytes(bool littleEndian)
{
#if IS_LITTLE_ENDIAN
    return !littleEndian;
#else
    return littleEndian;
#endif
}

static inline uint8_t
swapBytes(uint8_t x)
{
    return x;
}

static inline uint16_t
swapBytes(uint16_t x)
{
    return ((x & 0xff) << 8) | (x >> 8);
}

static inline uint32_t
swapBytes(uint32_t x)
{
    return ((x & 0xff) << 24) |
           ((x & 0xff00) << 8) |
           ((x & 0xff0000) >> 8) |
           ((x & 0xff000000) >> 24);
}

static inline uint64_t
swapBytes(uint64_t x)
{
    uint32_t a = x & UINT32_MAX;
    uint32_t b = x >> 32;
    return (uint64_t(swapBytes(a)) << 32) | swapBytes(b);
}

template <typename DataType> struct DataToRepType { typedef DataType result; };
template <> struct DataToRepType<int8_t>   { typedef uint8_t result; };
template <> struct DataToRepType<uint8_t>  { typedef uint8_t result; };
template <> struct DataToRepType<int16_t>  { typedef uint16_t result; };
template <> struct DataToRepType<uint16_t> { typedef uint16_t result; };
template <> struct DataToRepType<int32_t>  { typedef uint32_t result; };
template <> struct DataToRepType<uint32_t> { typedef uint32_t result; };
template <> struct DataToRepType<float>    { typedef uint32_t result; };
template <> struct DataToRepType<double>   { typedef uint64_t result; };

template <typename DataType>
struct DataViewIO
{
    typedef typename DataToRepType<DataType>::result ReadWriteType;

    static void fromBuffer(DataType* dest, const uint8_t* unalignedBuffer, bool wantSwap)
    {
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(dest) & (Min<size_t>(MOZ_ALIGNOF(void*), sizeof(DataType)) - 1)) == 0);
        memcpy((void*) dest, unalignedBuffer, sizeof(ReadWriteType));
        if (wantSwap) {
            ReadWriteType* rwDest = reinterpret_cast<ReadWriteType*>(dest);
            *rwDest = swapBytes(*rwDest);
        }
    }

    static void toBuffer(uint8_t* unalignedBuffer, const DataType* src, bool wantSwap)
    {
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(src) & (Min<size_t>(MOZ_ALIGNOF(void*), sizeof(DataType)) - 1)) == 0);
        ReadWriteType temp = *reinterpret_cast<const ReadWriteType*>(src);
        if (wantSwap)
            temp = swapBytes(temp);
        memcpy(unalignedBuffer, (void*) &temp, sizeof(ReadWriteType));
    }
};

template<typename NativeType>
/* static */ bool
DataViewObject::read(JSContext* cx, Handle<DataViewObject*> obj,
                     const CallArgs& args, NativeType* val, const char* method)
{
    if (args.length() < 1) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                             JSMSG_MORE_ARGS_NEEDED, method, "0", "s");
        return false;
    }

    uint32_t offset;
    if (!ToUint32(cx, args[0], &offset))
        return false;

    bool fromLittleEndian = args.length() >= 2 && ToBoolean(args[1]);

    if (obj->arrayBuffer().isNeutered()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
        return false;
    }

    uint8_t* data = DataViewObject::getDataPointer<NativeType>(cx, obj, offset);
    if (!data)
        return false;

    DataViewIO<NativeType>::fromBuffer(val, data, needToSwapBytes(fromLittleEndian));
    return true;
}

template <typename NativeType>
static inline bool
WebIDLCast(JSContext* cx, HandleValue value, NativeType* out)
{
    int32_t temp;
    if (!ToInt32(cx, value, &temp))
        return false;
    // Technically, the behavior of assigning an out of range value to a signed
    // variable is undefined. In practice, compilers seem to do what we want
    // without issuing any warnings.
    *out = static_cast<NativeType>(temp);
    return true;
}

template <>
inline bool
WebIDLCast<float>(JSContext* cx, HandleValue value, float* out)
{
    double temp;
    if (!ToNumber(cx, value, &temp))
        return false;
    *out = static_cast<float>(temp);
    return true;
}

template <>
inline bool
WebIDLCast<double>(JSContext* cx, HandleValue value, double* out)
{
    return ToNumber(cx, value, out);
}

template<typename NativeType>
/* static */ bool
DataViewObject::write(JSContext* cx, Handle<DataViewObject*> obj,
                      const CallArgs& args, const char* method)
{
    if (args.length() < 2) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                             JSMSG_MORE_ARGS_NEEDED, method, "1", "");
        return false;
    }

    uint32_t offset;
    if (!ToUint32(cx, args[0], &offset))
        return false;

    NativeType value;
    if (!WebIDLCast(cx, args[1], &value))
        return false;

    bool toLittleEndian = args.length() >= 3 && ToBoolean(args[2]);

    if (obj->arrayBuffer().isNeutered()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
        return false;
    }

    uint8_t* data = DataViewObject::getDataPointer<NativeType>(cx, obj, offset);
    if (!data)
        return false;

    DataViewIO<NativeType>::toBuffer(data, &value, needToSwapBytes(toLittleEndian));
    return true;
}

bool
DataViewObject::getInt8Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    int8_t val;
    if (!read(cx, thisView, args, &val, "getInt8"))
        return false;
    args.rval().setInt32(val);
    return true;
}

bool
DataViewObject::fun_getInt8(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getInt8Impl>(cx, args);
}

bool
DataViewObject::getUint8Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    uint8_t val;
    if (!read(cx, thisView, args, &val, "getUint8"))
        return false;
    args.rval().setInt32(val);
    return true;
}

bool
DataViewObject::fun_getUint8(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getUint8Impl>(cx, args);
}

bool
DataViewObject::getInt16Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    int16_t val;
    if (!read(cx, thisView, args, &val, "getInt16"))
        return false;
    args.rval().setInt32(val);
    return true;
}

bool
DataViewObject::fun_getInt16(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getInt16Impl>(cx, args);
}

bool
DataViewObject::getUint16Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    uint16_t val;
    if (!read(cx, thisView, args, &val, "getUint16"))
        return false;
    args.rval().setInt32(val);
    return true;
}

bool
DataViewObject::fun_getUint16(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getUint16Impl>(cx, args);
}

bool
DataViewObject::getInt32Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    int32_t val;
    if (!read(cx, thisView, args, &val, "getInt32"))
        return false;
    args.rval().setInt32(val);
    return true;
}

bool
DataViewObject::fun_getInt32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getInt32Impl>(cx, args);
}

bool
DataViewObject::getUint32Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    uint32_t val;
    if (!read(cx, thisView, args, &val, "getUint32"))
        return false;
    args.rval().setNumber(val);
    return true;
}

bool
DataViewObject::fun_getUint32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getUint32Impl>(cx, args);
}

bool
DataViewObject::getFloat32Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    float val;
    if (!read(cx, thisView, args, &val, "getFloat32"))
        return false;

    args.rval().setDouble(CanonicalizeNaN(val));
    return true;
}

bool
DataViewObject::fun_getFloat32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getFloat32Impl>(cx, args);
}

bool
DataViewObject::getFloat64Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    double val;
    if (!read(cx, thisView, args, &val, "getFloat64"))
        return false;

    args.rval().setDouble(CanonicalizeNaN(val));
    return true;
}

bool
DataViewObject::fun_getFloat64(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getFloat64Impl>(cx, args);
}

bool
DataViewObject::setInt8Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    if (!write<int8_t>(cx, thisView, args, "setInt8"))
        return false;
    args.rval().setUndefined();
    return true;
}

bool
DataViewObject::fun_setInt8(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setInt8Impl>(cx, args);
}

bool
DataViewObject::setUint8Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    if (!write<uint8_t>(cx, thisView, args, "setUint8"))
        return false;
    args.rval().setUndefined();
    return true;
}

bool
DataViewObject::fun_setUint8(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setUint8Impl>(cx, args);
}

bool
DataViewObject::setInt16Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    if (!write<int16_t>(cx, thisView, args, "setInt16"))
        return false;
    args.rval().setUndefined();
    return true;
}

bool
DataViewObject::fun_setInt16(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setInt16Impl>(cx, args);
}

bool
DataViewObject::setUint16Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    if (!write<uint16_t>(cx, thisView, args, "setUint16"))
        return false;
    args.rval().setUndefined();
    return true;
}

bool
DataViewObject::fun_setUint16(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setUint16Impl>(cx, args);
}

bool
DataViewObject::setInt32Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    if (!write<int32_t>(cx, thisView, args, "setInt32"))
        return false;
    args.rval().setUndefined();
    return true;
}

bool
DataViewObject::fun_setInt32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setInt32Impl>(cx, args);
}

bool
DataViewObject::setUint32Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    if (!write<uint32_t>(cx, thisView, args, "setUint32"))
        return false;
    args.rval().setUndefined();
    return true;
}

bool
DataViewObject::fun_setUint32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setUint32Impl>(cx, args);
}

bool
DataViewObject::setFloat32Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    if (!write<float>(cx, thisView, args, "setFloat32"))
        return false;
    args.rval().setUndefined();
    return true;
}

bool
DataViewObject::fun_setFloat32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setFloat32Impl>(cx, args);
}

bool
DataViewObject::setFloat64Impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    Rooted<DataViewObject*> thisView(cx, &args.thisv().toObject().as<DataViewObject>());

    if (!write<double>(cx, thisView, args, "setFloat64"))
        return false;
    args.rval().setUndefined();
    return true;
}

bool
DataViewObject::fun_setFloat64(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setFloat64Impl>(cx, args);
}

Value
TypedArrayObject::getElement(uint32_t index)
{
    switch (type()) {
      case Scalar::Int8:
        return Int8Array::getIndexValue(this, index);
      case Scalar::Uint8:
        return Uint8Array::getIndexValue(this, index);
      case Scalar::Int16:
        return Int16Array::getIndexValue(this, index);
      case Scalar::Uint16:
        return Uint16Array::getIndexValue(this, index);
      case Scalar::Int32:
        return Int32Array::getIndexValue(this, index);
      case Scalar::Uint32:
        return Uint32Array::getIndexValue(this, index);
      case Scalar::Float32:
        return Float32Array::getIndexValue(this, index);
      case Scalar::Float64:
        return Float64Array::getIndexValue(this, index);
      case Scalar::Uint8Clamped:
        return Uint8ClampedArray::getIndexValue(this, index);
      case Scalar::Float32x4:
      case Scalar::Int32x4:
      case Scalar::MaxTypedArrayViewType:
        break;
    }

    MOZ_CRASH("Unknown TypedArray type");
}

void
TypedArrayObject::setElement(TypedArrayObject& obj, uint32_t index, double d)
{
    MOZ_ASSERT(index < obj.length());

    switch (obj.type()) {
      case Scalar::Int8:
        Int8Array::setIndexValue(obj, index, d);
        return;
      case Scalar::Uint8:
        Uint8Array::setIndexValue(obj, index, d);
        return;
      case Scalar::Uint8Clamped:
        Uint8ClampedArray::setIndexValue(obj, index, d);
        return;
      case Scalar::Int16:
        Int16Array::setIndexValue(obj, index, d);
        return;
      case Scalar::Uint16:
        Uint16Array::setIndexValue(obj, index, d);
        return;
      case Scalar::Int32:
        Int32Array::setIndexValue(obj, index, d);
        return;
      case Scalar::Uint32:
        Uint32Array::setIndexValue(obj, index, d);
        return;
      case Scalar::Float32:
        Float32Array::setIndexValue(obj, index, d);
        return;
      case Scalar::Float64:
        Float64Array::setIndexValue(obj, index, d);
        return;
      case Scalar::Float32x4:
      case Scalar::Int32x4:
      case Scalar::MaxTypedArrayViewType:
        break;
    }

    MOZ_CRASH("Unknown TypedArray type");
}

/***
 *** JS impl
 ***/

/*
 * TypedArrayObject boilerplate
 */

#define IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Name,NativeType)                                    \
  JS_FRIEND_API(JSObject*) JS_New ## Name ## Array(JSContext* cx, uint32_t nelements)           \
  {                                                                                             \
      return TypedArrayObjectTemplate<NativeType>::fromLength(cx, nelements);                   \
  }                                                                                             \
  JS_FRIEND_API(JSObject*) JS_New ## Name ## ArrayFromArray(JSContext* cx, HandleObject other)  \
  {                                                                                             \
      return TypedArrayObjectTemplate<NativeType>::fromArray(cx, other);                        \
  }                                                                                             \
  JS_FRIEND_API(JSObject*) JS_New ## Name ## ArrayWithBuffer(JSContext* cx,                     \
                               HandleObject arrayBuffer, uint32_t byteOffset, int32_t length)   \
  {                                                                                             \
      return TypedArrayObjectTemplate<NativeType>::fromBuffer(cx, arrayBuffer, byteOffset,      \
                                                              length);                          \
  }                                                                                             \
  JS_FRIEND_API(bool) JS_Is ## Name ## Array(JSObject* obj)                                     \
  {                                                                                             \
      if (!(obj = CheckedUnwrap(obj)))                                                          \
          return false;                                                                         \
      const Class* clasp = obj->getClass();                                                     \
      return clasp == TypedArrayObjectTemplate<NativeType>::instanceClass();                    \
  }                                                                                             \
  JS_FRIEND_API(JSObject*) js::Unwrap ## Name ## Array(JSObject* obj)                           \
  {                                                                                             \
      obj = CheckedUnwrap(obj);                                                                 \
      if (!obj)                                                                                 \
          return nullptr;                                                                       \
      const Class* clasp = obj->getClass();                                                     \
      if (clasp == TypedArrayObjectTemplate<NativeType>::instanceClass())                       \
          return obj;                                                                           \
      return nullptr;                                                                           \
  }                                                                                             \
  const js::Class* const js::detail::Name ## ArrayClassPtr =                                    \
      &js::TypedArrayObject::classes[TypedArrayObjectTemplate<NativeType>::ArrayTypeID()];

IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Int8, int8_t)
IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Uint8, uint8_t)
IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Uint8Clamped, uint8_clamped)
IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Int16, int16_t)
IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Uint16, uint16_t)
IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Int32, int32_t)
IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Uint32, uint32_t)
IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Float32, float)
IMPL_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Float64, double)

#define IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Name, ExternalType, InternalType)              \
  JS_FRIEND_API(JSObject*) JS_GetObjectAs ## Name ## Array(JSObject* obj,                  \
                                                            uint32_t* length,               \
                                                            bool* isShared,                 \
                                                            ExternalType** data)            \
  {                                                                                         \
      if (!(obj = CheckedUnwrap(obj)))                                                      \
          return nullptr;                                                                   \
                                                                                            \
      const Class* clasp = obj->getClass();                                                 \
      if (clasp != TypedArrayObjectTemplate<InternalType>::instanceClass())                 \
          return nullptr;                                                                   \
                                                                                            \
      TypedArrayObject* tarr = &obj->as<TypedArrayObject>();                                \
      *length = tarr->length();                                                             \
      *isShared = tarr->isSharedMemory();                                                         \
      *data = static_cast<ExternalType*>(tarr->viewDataEither().unwrap(/*safe - caller sees isShared flag*/)); \
                                                                                            \
      return obj;                                                                           \
  }

IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Int8, int8_t, int8_t)
IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Uint8, uint8_t, uint8_t)
IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Uint8Clamped, uint8_t, uint8_clamped)
IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Int16, int16_t, int16_t)
IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Uint16, uint16_t, uint16_t)
IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Int32, int32_t, int32_t)
IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Uint32, uint32_t, uint32_t)
IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Float32, float, float)
IMPL_TYPED_ARRAY_COMBINED_UNWRAPPERS(Float64, double, double)

#define TYPED_ARRAY_CLASS_SPEC(_typedArray)                                    \
{                                                                              \
    _typedArray::createConstructor,                                            \
    _typedArray::createPrototype,                                              \
    nullptr,                                                                   \
    nullptr,                                                                   \
    nullptr,                                                                   \
    nullptr,                                                                   \
    _typedArray::finishClassInit,                                              \
    JSProto_TypedArray                                                         \
}

#define IMPL_TYPED_ARRAY_CLASS(_typedArray)                                    \
{                                                                              \
    #_typedArray,                                                              \
    JSCLASS_HAS_RESERVED_SLOTS(TypedArrayObject::RESERVED_SLOTS) |             \
    JSCLASS_HAS_PRIVATE |                                                      \
    JSCLASS_HAS_CACHED_PROTO(JSProto_##_typedArray) |                          \
    JSCLASS_DELAY_METADATA_CALLBACK,                                           \
    nullptr,                 /* addProperty */                                 \
    nullptr,                 /* delProperty */                                 \
    nullptr,                 /* getProperty */                                 \
    nullptr,                 /* setProperty */                                 \
    nullptr,                 /* enumerate   */                                 \
    nullptr,                 /* resolve     */                                 \
    nullptr,                 /* mayResolve  */                                 \
    nullptr,                 /* finalize    */                                 \
    nullptr,                 /* call        */                                 \
    nullptr,                 /* hasInstance */                                 \
    nullptr,                 /* construct   */                                 \
    TypedArrayObject::trace, /* trace  */                                      \
    TYPED_ARRAY_CLASS_SPEC(_typedArray)                                        \
}

const Class TypedArrayObject::classes[Scalar::MaxTypedArrayViewType] = {
    IMPL_TYPED_ARRAY_CLASS(Int8Array),
    IMPL_TYPED_ARRAY_CLASS(Uint8Array),
    IMPL_TYPED_ARRAY_CLASS(Int16Array),
    IMPL_TYPED_ARRAY_CLASS(Uint16Array),
    IMPL_TYPED_ARRAY_CLASS(Int32Array),
    IMPL_TYPED_ARRAY_CLASS(Uint32Array),
    IMPL_TYPED_ARRAY_CLASS(Float32Array),
    IMPL_TYPED_ARRAY_CLASS(Float64Array),
    IMPL_TYPED_ARRAY_CLASS(Uint8ClampedArray)
};

// The various typed array prototypes are supposed to 1) be normal objects,
// 2) stringify to "[object <name of constructor>]", and 3) (Gecko-specific)
// be xrayable.  The first and second requirements mandate (in the absence of
// @@toStringTag) a custom class.  The third requirement mandates that each
// prototype's class have the relevant typed array's cached JSProtoKey in them.
// Thus we need one class with cached prototype per kind of typed array, with a
// delegated ClassSpec.
#define IMPL_TYPED_ARRAY_PROTO_CLASS(typedArray, i) \
{ \
    /*
     * Actually ({}).toString.call(Uint8Array.prototype) should throw, because
     * Uint8Array.prototype lacks the the typed array internal slots.  (Same as
     * with %TypedArray%.prototype.)  It's not clear this is desirable (see
     * above), but it's what we've always done, so keep doing it till we
     * implement @@toStringTag or ES6 changes.
     */ \
    #typedArray "Prototype", \
    JSCLASS_HAS_CACHED_PROTO(JSProto_##typedArray), \
    nullptr, /* addProperty */ \
    nullptr, /* delProperty */ \
    nullptr, /* getProperty */ \
    nullptr, /* setProperty */ \
    nullptr, /* enumerate */ \
    nullptr, /* resolve */ \
    nullptr, /* mayResolve */ \
    nullptr, /* finalize */ \
    nullptr, /* call */ \
    nullptr, /* hasInstance */ \
    nullptr, /* construct */ \
    nullptr, /* trace  */ \
    { \
        DELEGATED_CLASSSPEC(&TypedArrayObject::classes[i].spec), \
        nullptr, \
        nullptr, \
        nullptr, \
        nullptr, \
        nullptr, \
        nullptr, \
        JSProto_TypedArray | ClassSpec::IsDelegated \
    } \
}

const Class TypedArrayObject::protoClasses[Scalar::MaxTypedArrayViewType] = {
    IMPL_TYPED_ARRAY_PROTO_CLASS(Int8Array, 0),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Uint8Array, 1),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Int16Array, 2),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Uint16Array, 3),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Int32Array, 4),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Uint32Array, 5),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Float32Array, 6),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Float64Array, 7),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Uint8ClampedArray, 8)
};

/* static */ bool
TypedArrayObject::isOriginalLengthGetter(Native native)
{
    return native == TypedArray_lengthGetter;
}

const Class DataViewObject::protoClass = {
    "DataViewPrototype",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(TypedArrayObject::RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_DataView)
};

const Class DataViewObject::class_ = {
    "DataView",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(TypedArrayObject::RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_DataView),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    nullptr, /* finalize */
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    ArrayBufferViewObject::trace
};

const JSFunctionSpec DataViewObject::jsfuncs[] = {
    JS_FN("getInt8",    DataViewObject::fun_getInt8,      1,0),
    JS_FN("getUint8",   DataViewObject::fun_getUint8,     1,0),
    JS_FN("getInt16",   DataViewObject::fun_getInt16,     2,0),
    JS_FN("getUint16",  DataViewObject::fun_getUint16,    2,0),
    JS_FN("getInt32",   DataViewObject::fun_getInt32,     2,0),
    JS_FN("getUint32",  DataViewObject::fun_getUint32,    2,0),
    JS_FN("getFloat32", DataViewObject::fun_getFloat32,   2,0),
    JS_FN("getFloat64", DataViewObject::fun_getFloat64,   2,0),
    JS_FN("setInt8",    DataViewObject::fun_setInt8,      2,0),
    JS_FN("setUint8",   DataViewObject::fun_setUint8,     2,0),
    JS_FN("setInt16",   DataViewObject::fun_setInt16,     3,0),
    JS_FN("setUint16",  DataViewObject::fun_setUint16,    3,0),
    JS_FN("setInt32",   DataViewObject::fun_setInt32,     3,0),
    JS_FN("setUint32",  DataViewObject::fun_setUint32,    3,0),
    JS_FN("setFloat32", DataViewObject::fun_setFloat32,   3,0),
    JS_FN("setFloat64", DataViewObject::fun_setFloat64,   3,0),
    JS_FS_END
};

template<Value ValueGetter(DataViewObject* view)>
bool
DataViewObject::getterImpl(JSContext* cx, const CallArgs& args)
{
    args.rval().set(ValueGetter(&args.thisv().toObject().as<DataViewObject>()));
    return true;
}

template<Value ValueGetter(DataViewObject* view)>
bool
DataViewObject::getter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getterImpl<ValueGetter> >(cx, args);
}

template<Value ValueGetter(DataViewObject* view)>
bool
DataViewObject::defineGetter(JSContext* cx, PropertyName* name, HandleNativeObject proto)
{
    RootedId id(cx, NameToId(name));
    unsigned attrs = JSPROP_SHARED | JSPROP_GETTER;

    Rooted<GlobalObject*> global(cx, cx->compartment()->maybeGlobal());
    JSObject* getter =
        NewNativeFunction(cx, DataViewObject::getter<ValueGetter>, 0, nullptr);
    if (!getter)
        return false;

    return NativeDefineProperty(cx, proto, id, UndefinedHandleValue,
                                JS_DATA_TO_FUNC_PTR(GetterOp, getter), nullptr, attrs);
}

/* static */ bool
DataViewObject::initClass(JSContext* cx)
{
    Rooted<GlobalObject*> global(cx, cx->compartment()->maybeGlobal());
    if (global->isStandardClassResolved(JSProto_DataView))
        return true;

    RootedNativeObject proto(cx, global->createBlankPrototype(cx, &DataViewObject::protoClass));
    if (!proto)
        return false;

    RootedFunction ctor(cx, global->createConstructor(cx, DataViewObject::class_constructor,
                                                      cx->names().DataView, 3));
    if (!ctor)
        return false;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return false;

    if (!defineGetter<bufferValue>(cx, cx->names().buffer, proto))
        return false;

    if (!defineGetter<byteLengthValue>(cx, cx->names().byteLength, proto))
        return false;

    if (!defineGetter<byteOffsetValue>(cx, cx->names().byteOffset, proto))
        return false;

    if (!JS_DefineFunctions(cx, proto, DataViewObject::jsfuncs))
        return false;

    /*
     * Create a helper function to implement the craziness of
     * |new DataView(new otherWindow.ArrayBuffer())|, and install it in the
     * global for use by the DataViewObject constructor.
     */
    RootedFunction fun(cx, NewNativeFunction(cx, ArrayBufferObject::createDataViewForThis,
                                             0, nullptr));
    if (!fun)
        return false;

    if (!GlobalObject::initBuiltinConstructor(cx, global, JSProto_DataView, ctor, proto))
        return false;

    global->setCreateDataViewForThis(fun);

    return true;
}

void
DataViewObject::neuter(void* newData)
{
    setFixedSlot(TypedArrayObject::LENGTH_SLOT, Int32Value(0));
    setFixedSlot(TypedArrayObject::BYTEOFFSET_SLOT, Int32Value(0));
    setPrivate(newData);
}

JSObject*
js::InitDataViewClass(JSContext* cx, HandleObject obj)
{
    if (!DataViewObject::initClass(cx))
        return nullptr;
    return &cx->global()->getPrototype(JSProto_DataView).toObject();
}

bool
js::IsTypedArrayConstructor(HandleValue v, uint32_t type)
{
    switch (type) {
      case Scalar::Int8:
        return IsNativeFunction(v, Int8Array::class_constructor);
      case Scalar::Uint8:
        return IsNativeFunction(v, Uint8Array::class_constructor);
      case Scalar::Int16:
        return IsNativeFunction(v, Int16Array::class_constructor);
      case Scalar::Uint16:
        return IsNativeFunction(v, Uint16Array::class_constructor);
      case Scalar::Int32:
        return IsNativeFunction(v, Int32Array::class_constructor);
      case Scalar::Uint32:
        return IsNativeFunction(v, Uint32Array::class_constructor);
      case Scalar::Float32:
        return IsNativeFunction(v, Float32Array::class_constructor);
      case Scalar::Float64:
        return IsNativeFunction(v, Float64Array::class_constructor);
      case Scalar::Uint8Clamped:
        return IsNativeFunction(v, Uint8ClampedArray::class_constructor);
      case Scalar::MaxTypedArrayViewType:
        break;
    }
    MOZ_CRASH("unexpected typed array type");
}

template <typename CharT>
bool
js::StringIsTypedArrayIndex(const CharT* s, size_t length, uint64_t* indexp)
{
    const CharT* end = s + length;

    if (s == end)
        return false;

    bool negative = false;
    if (*s == '-') {
        negative = true;
        if (++s == end)
            return false;
    }

    if (!JS7_ISDEC(*s))
        return false;

    uint64_t index = 0;
    uint32_t digit = JS7_UNDEC(*s++);

    /* Don't allow leading zeros. */
    if (digit == 0 && s != end)
        return false;

    index = digit;

    for (; s < end; s++) {
        if (!JS7_ISDEC(*s))
            return false;

        digit = JS7_UNDEC(*s);

        /* Watch for overflows. */
        if ((UINT64_MAX - digit) / 10 < index)
            index = UINT64_MAX;
        else
            index = 10 * index + digit;
    }

    if (negative)
        *indexp = UINT64_MAX;
    else
        *indexp = index;
    return true;
}

template bool
js::StringIsTypedArrayIndex(const char16_t* s, size_t length, uint64_t* indexp);

template bool
js::StringIsTypedArrayIndex(const Latin1Char* s, size_t length, uint64_t* indexp);

/* ES6 draft rev 34 (2015 Feb 20) 9.4.5.3 [[DefineOwnProperty]] step 3.c. */
bool
js::DefineTypedArrayElement(JSContext* cx, HandleObject obj, uint64_t index,
                            Handle<PropertyDescriptor> desc, ObjectOpResult& result)
{
    MOZ_ASSERT(IsAnyTypedArray(obj));

    // These are all substeps of 3.c.
    // Steps i-vi.
    // We (wrongly) ignore out of range defines with a value.
    if (index >= AnyTypedArrayLength(obj))
        return result.succeed();

    // Step vii.
    if (desc.isAccessorDescriptor())
        return result.fail(JSMSG_CANT_REDEFINE_PROP);

    // Step viii.
    if (desc.hasConfigurable() && desc.configurable())
        return result.fail(JSMSG_CANT_REDEFINE_PROP);

    // Step ix.
    if (desc.hasEnumerable() && !desc.enumerable())
        return result.fail(JSMSG_CANT_REDEFINE_PROP);

    // Step x.
    if (desc.hasWritable() && !desc.writable())
        return result.fail(JSMSG_CANT_REDEFINE_PROP);

    // Step xi.
    if (desc.hasValue()) {
        double d;
        if (!ToNumber(cx, desc.value(), &d))
            return false;

        if (obj->is<TypedArrayObject>())
            TypedArrayObject::setElement(obj->as<TypedArrayObject>(), index, d);
    }

    // Step xii.
    return result.succeed();
}

/* JS Friend API */

JS_FRIEND_API(bool)
JS_IsTypedArrayObject(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    return obj ? obj->is<TypedArrayObject>() : false;
}

JS_FRIEND_API(uint32_t)
JS_GetTypedArrayLength(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return 0;
    return obj->as<TypedArrayObject>().length();
}

JS_FRIEND_API(uint32_t)
JS_GetTypedArrayByteOffset(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return 0;
    return obj->as<TypedArrayObject>().byteOffset();
}

JS_FRIEND_API(uint32_t)
JS_GetTypedArrayByteLength(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return 0;
    return obj->as<TypedArrayObject>().byteLength();
}

JS_FRIEND_API(bool)
JS_GetTypedArraySharedness(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return false;
    return obj->as<TypedArrayObject>().isSharedMemory();
}

JS_FRIEND_API(js::Scalar::Type)
JS_GetArrayBufferViewType(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return Scalar::MaxTypedArrayViewType;

    if (obj->is<TypedArrayObject>())
        return obj->as<TypedArrayObject>().type();
    else if (obj->is<DataViewObject>())
        return Scalar::MaxTypedArrayViewType;
    MOZ_CRASH("invalid ArrayBufferView type");
}

JS_FRIEND_API(int8_t*)
JS_GetInt8ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Int8);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<int8_t*>(tarr->viewDataEither().unwrap(/*safe - caller sees isShared*/));
}

JS_FRIEND_API(uint8_t*)
JS_GetUint8ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Uint8);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<uint8_t*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}

JS_FRIEND_API(uint8_t*)
JS_GetUint8ClampedArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Uint8Clamped);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<uint8_t*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}

JS_FRIEND_API(int16_t*)
JS_GetInt16ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Int16);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<int16_t*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}

JS_FRIEND_API(uint16_t*)
JS_GetUint16ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Uint16);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<uint16_t*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}

JS_FRIEND_API(int32_t*)
JS_GetInt32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Int32);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<int32_t*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}

JS_FRIEND_API(uint32_t*)
JS_GetUint32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Uint32);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<uint32_t*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}

JS_FRIEND_API(float*)
JS_GetFloat32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Float32);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<float*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}

JS_FRIEND_API(double*)
JS_GetFloat64ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Float64);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<double*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}

JS_FRIEND_API(bool)
JS_IsDataViewObject(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    return obj ? obj->is<DataViewObject>() : false;
}

JS_FRIEND_API(uint32_t)
JS_GetDataViewByteOffset(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return 0;
    return obj->as<DataViewObject>().byteOffset();
}

JS_FRIEND_API(void*)
JS_GetDataViewData(JSObject* obj, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    return obj->as<DataViewObject>().dataPointer();
}

JS_FRIEND_API(uint32_t)
JS_GetDataViewByteLength(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return 0;
    return obj->as<DataViewObject>().byteLength();
}

JS_FRIEND_API(JSObject*)
JS_NewDataView(JSContext* cx, HandleObject arrayBuffer, uint32_t byteOffset, int32_t byteLength)
{
    ConstructArgs cargs(cx);
    if (!cargs.init(3))
        return nullptr;

    RootedObject constructor(cx);
    JSProtoKey key = JSCLASS_CACHED_PROTO_KEY(&DataViewObject::class_);
    if (!GetBuiltinConstructor(cx, key, &constructor))
        return nullptr;

    cargs[0].setObject(*arrayBuffer);
    cargs[1].setNumber(byteOffset);
    cargs[2].setInt32(byteLength);

    RootedValue fun(cx, ObjectValue(*constructor));
    RootedValue rval(cx);
    if (!Construct(cx, fun, cargs, fun, &rval))
        return nullptr;
    MOZ_ASSERT(rval.isObject());
    return &rval.toObject();
}
