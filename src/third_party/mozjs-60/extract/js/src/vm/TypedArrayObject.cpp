/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/TypedArrayObject-inl.h"
#include "vm/TypedArrayObject.h"

#include "mozilla/Alignment.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/PodOperations.h"

#include <string.h>
#ifndef XP_WIN
# include <sys/mman.h>
#endif

#include "jsapi.h"
#include "jsarray.h"
#include "jsnum.h"
#include "jstypes.h"
#include "jsutil.h"

#include "builtin/DataViewObject.h"
#include "builtin/TypedObjectConstants.h"
#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "jit/InlinableNatives.h"
#include "js/Conversions.h"
#include "js/Wrapper.h"
#include "util/Windows.h"
#include "vm/ArrayBufferObject.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PIC.h"
#include "vm/SelfHosting.h"
#include "vm/SharedMem.h"
#include "vm/WrapperObject.h"

#include "gc/Nursery-inl.h"
#include "gc/StoreBuffer-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::AssertedCast;
using JS::CanonicalizeNaN;
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
TypedArrayObject::notifyBufferDetached(JSContext* cx, void* newData)
{
    MOZ_ASSERT(!isSharedMemory());
    setFixedSlot(TypedArrayObject::LENGTH_SLOT, Int32Value(0));
    setFixedSlot(TypedArrayObject::BYTEOFFSET_SLOT, Int32Value(0));

    // If the object is in the nursery, the buffer will be freed by the next
    // nursery GC. Free the data slot pointer if the object has no inline data.
    Nursery& nursery = cx->nursery();
    if (isTenured() && !hasBuffer() && !hasInlineElements() &&
        !nursery.isInside(elements()))
    {
        js_free(elements());
    }

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

    // If the object is in the nursery, the buffer will be freed by the next
    // nursery GC. Free the data slot pointer if the object has no inline data.
    Nursery& nursery = cx->nursery();
    if (tarray->isTenured() && !tarray->hasInlineElements() &&
        !nursery.isInside(tarray->elements()))
    {
        js_free(tarray->elements());
    }

    tarray->setPrivate(buffer->dataPointer());

    tarray->setFixedSlot(TypedArrayObject::BUFFER_SLOT, ObjectValue(*buffer));

    // Notify compiled jit code that the base pointer has moved.
    MarkObjectStateChange(cx, tarray);

    return true;
}

#ifdef DEBUG
void
TypedArrayObject::assertZeroLengthArrayData() const
{
    if (length() == 0 && !hasBuffer()) {
        uint8_t* end = fixedData(TypedArrayObject::FIXED_DATA_START);
        MOZ_ASSERT(end[0] == ZeroLengthArrayData);
    }
}
#endif

/* static */ void
TypedArrayObject::trace(JSTracer* trc, JSObject* objArg)
{
    // Handle all tracing required when the object has a buffer.
    ArrayBufferViewObject::trace(trc, objArg);
}

void
TypedArrayObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(!IsInsideNursery(obj));
    TypedArrayObject* curObj = &obj->as<TypedArrayObject>();

    // Template objects or discarded objects (which didn't have enough room
    // for inner elements). Don't have anything to free.
    if (!curObj->elementsRaw())
        return;

    curObj->assertZeroLengthArrayData();

    // Typed arrays with a buffer object do not need to be free'd
    if (curObj->hasBuffer())
        return;

    // Free the data slot pointer if it does not point into the old JSObject.
    if (!curObj->hasInlineElements())
        js_free(curObj->elements());
}

/* static */ size_t
TypedArrayObject::objectMoved(JSObject* obj, JSObject* old)
{
    TypedArrayObject* newObj = &obj->as<TypedArrayObject>();
    TypedArrayObject* oldObj = &old->as<TypedArrayObject>();
    MOZ_ASSERT(newObj->elementsRaw() == oldObj->elementsRaw());
    MOZ_ASSERT(obj->isTenured());

    // Typed arrays with a buffer object do not need an update.
    if (oldObj->hasBuffer())
        return 0;

    if (!IsInsideNursery(old)) {
        // Update the data slot pointer if it points to the old JSObject.
        if (oldObj->hasInlineElements())
            newObj->setInlineElements();

        return 0;
    }

    Nursery& nursery = obj->zone()->group()->nursery();
    void* buf = oldObj->elements();

    if (!nursery.isInside(buf)) {
        nursery.removeMallocedBuffer(buf);
        return 0;
    }

    // Determine if we can use inline data for the target array. If this is
    // possible, the nursery will have picked an allocation size that is large
    // enough.
    size_t nbytes = 0;
    switch (oldObj->type()) {
#define OBJECT_MOVED_TYPED_ARRAY(T, N) \
      case Scalar::N: \
        nbytes = oldObj->length() * sizeof(T); \
        break;
JS_FOR_EACH_TYPED_ARRAY(OBJECT_MOVED_TYPED_ARRAY)
#undef OBJECT_MOVED_TYPED_ARRAY
      default:
        MOZ_CRASH("Unsupported TypedArray type");
    }

    size_t headerSize = dataOffset() + sizeof(HeapSlot);

    // See AllocKindForLazyBuffer.
    AllocKind newAllocKind = obj->asTenured().getAllocKind();
    MOZ_ASSERT_IF(nbytes == 0, headerSize + sizeof(uint8_t) <= GetGCKindBytes(newAllocKind));

    if (headerSize + nbytes <= GetGCKindBytes(newAllocKind)) {
        MOZ_ASSERT(oldObj->hasInlineElements());
#ifdef DEBUG
        if (nbytes == 0) {
            uint8_t* output = newObj->fixedData(TypedArrayObject::FIXED_DATA_START);
            output[0] = ZeroLengthArrayData;
        }
#endif
        newObj->setInlineElements();
    } else {
        MOZ_ASSERT(!oldObj->hasInlineElements());
        AutoEnterOOMUnsafeRegion oomUnsafe;
        nbytes = JS_ROUNDUP(nbytes, sizeof(Value));
        void* data = newObj->zone()->pod_malloc<uint8_t>(nbytes);
        if (!data)
            oomUnsafe.crash("Failed to allocate typed array elements while tenuring.");
        MOZ_ASSERT(!nursery.isInside(data));
        newObj->initPrivate(data);
    }

    mozilla::PodCopy(newObj->elements(), oldObj->elements(), nbytes);

    // Set a forwarding pointer for the element buffers in case they were
    // preserved on the stack by Ion.
    nursery.setForwardingPointerWhileTenuring(oldObj->elements(), newObj->elements(),
                                              /* direct = */nbytes >= sizeof(uintptr_t));

    return newObj->hasInlineElements() ? 0 : nbytes;
}

bool
TypedArrayObject::hasInlineElements() const
{
    return elements() == this->fixedData(TypedArrayObject::FIXED_DATA_START) &&
        byteLength() <= TypedArrayObject::INLINE_BUFFER_LIMIT;
}

void
TypedArrayObject::setInlineElements()
{
    char* dataSlot = reinterpret_cast<char*>(this) + this->dataOffset();
    *reinterpret_cast<void**>(dataSlot) = this->fixedData(TypedArrayObject::FIXED_DATA_START);
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

enum class SpeciesConstructorOverride {
    None,
    ArrayBuffer
};

enum class CreateSingleton {
    Yes,
    No
};

template<typename NativeType>
class TypedArrayObjectTemplate : public TypedArrayObject
{
    friend class TypedArrayObject;

  public:
    static constexpr Scalar::Type ArrayTypeID() { return TypeIDOfType<NativeType>::id; }
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
        return GlobalObject::createBlankPrototypeInheriting(cx, global, clasp, typedArrayProto);
    }

    static JSObject*
    createConstructor(JSContext* cx, JSProtoKey key)
    {
        Handle<GlobalObject*> global = cx->global();
        RootedFunction ctorProto(cx, GlobalObject::getOrCreateTypedArrayConstructor(cx, global));
        if (!ctorProto)
            return nullptr;

        JSFunction* fun = NewFunctionWithProto(cx, class_constructor, 3,
                                               JSFunction::NATIVE_CTOR, nullptr,
                                               ClassName(key, cx),
                                               ctorProto, gc::AllocKind::FUNCTION,
                                               SingletonObject);

        if (fun)
            fun->setJitInfo(&jit::JitInfo_TypedArrayConstructor);

        return fun;
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
    makeTypedInstance(JSContext* cx, CreateSingleton createSingleton, gc::AllocKind allocKind)
    {
        const Class* clasp = instanceClass();
        if (createSingleton == CreateSingleton::Yes) {
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
    makeInstance(JSContext* cx, Handle<ArrayBufferObjectMaybeShared*> buffer,
                 CreateSingleton createSingleton, uint32_t byteOffset, uint32_t len,
                 HandleObject proto)
    {
        MOZ_ASSERT_IF(!buffer, byteOffset == 0);
        MOZ_ASSERT_IF(buffer, !buffer->isDetached());
        MOZ_ASSERT(len < INT32_MAX / sizeof(NativeType));

        gc::AllocKind allocKind = buffer
                                  ? GetGCObjectKind(instanceClass())
                                  : AllocKindForLazyBuffer(len * sizeof(NativeType));

        // Subclassing mandates that we hand in the proto every time. Most of
        // the time, though, that [[Prototype]] will not be interesting. If
        // it isn't, we can do some more TI optimizations.
        RootedObject checkProto(cx);
        if (proto) {
            checkProto =
                GlobalObject::getOrCreatePrototype(cx, JSCLASS_CACHED_PROTO_KEY(instanceClass()));
            if (!checkProto)
                return nullptr;
        }

        AutoSetNewObjectMetadata metadata(cx);
        Rooted<TypedArrayObject*> obj(cx);
        if (proto && proto != checkProto)
            obj = makeProtoInstance(cx, proto, allocKind);
        else
            obj = makeTypedInstance(cx, createSingleton, allocKind);
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
            auto ptr = buffer->dataPointerEither();
            if (!IsInsideNursery(obj) && cx->nursery().isInside(ptr)) {
                // Shared buffer data should never be nursery-allocated, so we
                // need to fail here if isSharedMemory.  However, mmap() can
                // place a SharedArrayRawBuffer up against the bottom end of a
                // nursery chunk, and a zero-length buffer will erroneously be
                // perceived as being inside the nursery; sidestep that.
                if (isSharedMemory) {
                    MOZ_ASSERT(buffer->byteLength() == 0 &&
                               (uintptr_t(ptr.unwrapValue()) & gc::ChunkMask) == 0);
                } else {
                    cx->zone()->group()->storeBuffer().putWholeCell(obj);
                }
            }
        } else {
            void* data = obj->fixedData(FIXED_DATA_START);
            obj->initPrivate(data);
            memset(data, 0, len * sizeof(NativeType));
#ifdef DEBUG
            if (len == 0) {
                uint8_t* elements = static_cast<uint8_t*>(data);
                elements[0] = ZeroLengthArrayData;
            }
#endif
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
                MOZ_ASSERT_IF(!AsArrayBuffer(buffer.get()).isDetached(),
                              buffer->dataPointerEither().unwrap(/*safe*/) <= obj->viewDataEither().unwrap(/*safe*/));
            }
            MOZ_ASSERT(bufferByteLength - arrayByteOffset >= arrayByteLength);
            MOZ_ASSERT(arrayByteOffset <= bufferByteLength);
        }

        // Verify that the private slot is at the expected place
        MOZ_ASSERT(obj->numFixedSlots() == TypedArrayObject::DATA_SLOT);
#endif

        // ArrayBufferObjects track their views to support detaching.
        if (buffer && buffer->is<ArrayBufferObject>()) {
            if (!buffer->as<ArrayBufferObject>().addView(cx, obj))
                return nullptr;
        }

        return obj;
    }

    static TypedArrayObject*
    makeTemplateObject(JSContext* cx, int32_t len)
    {
        MOZ_ASSERT(len >= 0);
        size_t nbytes;
        MOZ_ALWAYS_TRUE(CalculateAllocSize<NativeType>(len, &nbytes));
        MOZ_ASSERT(nbytes < TypedArrayObject::SINGLETON_BYTE_LENGTH);
        NewObjectKind newKind = TenuredObject;
        bool fitsInline = nbytes <= INLINE_BUFFER_LIMIT;
        const Class* clasp = instanceClass();
        gc::AllocKind allocKind = !fitsInline
                                  ? GetGCObjectKind(clasp)
                                  : AllocKindForLazyBuffer(nbytes);
        MOZ_ASSERT(CanBeFinalizedInBackground(allocKind, clasp));
        allocKind = GetBackgroundAllocKind(allocKind);

        AutoSetNewObjectMetadata metadata(cx);
        jsbytecode* pc;
        RootedScript script(cx, cx->currentScript(&pc));
        if (script && ObjectGroup::useSingletonForAllocationSite(script, pc, clasp))
            newKind = SingletonObject;
        JSObject* tmp = NewBuiltinClassInstance(cx, clasp, allocKind, newKind);
        if (!tmp)
            return nullptr;

        Rooted<TypedArrayObject*> tarray(cx, &tmp->as<TypedArrayObject>());
        initTypedArraySlots(tarray, len);

        // Template objects do not need memory for its elements, since there
        // won't be any elements to store. Therefore, we set the pointer to
        // nullptr and avoid allocating memory that will never be used.
        tarray->initPrivate(nullptr);

        if (script && !ObjectGroup::setAllocationSiteObjectGroup(cx, script, pc, tarray,
                                                                 newKind == SingletonObject))
        {
            return nullptr;
        }

        return tarray;
    }

    static void
    initTypedArraySlots(TypedArrayObject* tarray, int32_t len)
    {
        MOZ_ASSERT(len >= 0);
        tarray->setFixedSlot(TypedArrayObject::BUFFER_SLOT, NullValue());
        tarray->setFixedSlot(TypedArrayObject::LENGTH_SLOT, Int32Value(AssertedCast<int32_t>(len)));
        tarray->setFixedSlot(TypedArrayObject::BYTEOFFSET_SLOT, Int32Value(0));

        // Verify that the private slot is at the expected place.
        MOZ_ASSERT(tarray->numFixedSlots() == TypedArrayObject::DATA_SLOT);

#ifdef DEBUG
        if (len == 0) {
            uint8_t* output = tarray->fixedData(TypedArrayObject::FIXED_DATA_START);
            output[0] = TypedArrayObject::ZeroLengthArrayData;
        }
#endif
    }

    static void
    initTypedArrayData(JSContext* cx, TypedArrayObject* tarray, int32_t len,
                       void* buf, AllocKind allocKind)
    {
        if (buf) {
#ifdef DEBUG
            Nursery& nursery = cx->nursery();
            MOZ_ASSERT_IF(!nursery.isInside(buf) && !tarray->hasInlineElements(),
                          tarray->isTenured());
#endif
            tarray->initPrivate(buf);
        } else {
            size_t nbytes = len * sizeof(NativeType);
#ifdef DEBUG
            size_t dataOffset = TypedArrayObject::dataOffset();
            size_t offset = dataOffset + sizeof(HeapSlot);
            MOZ_ASSERT(offset + nbytes <= GetGCKindBytes(allocKind));
#endif

            void* data = tarray->fixedData(FIXED_DATA_START);
            tarray->initPrivate(data);
            memset(data, 0, nbytes);
        }
    }

    static TypedArrayObject*
    makeTypedArrayWithTemplate(JSContext* cx, TypedArrayObject* templateObj, int32_t len)
    {
        if (len < 0 || uint32_t(len) >= INT32_MAX / sizeof(NativeType)) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return nullptr;
        }

        size_t nbytes;
        MOZ_ALWAYS_TRUE(js::CalculateAllocSize<NativeType>(len, &nbytes));

        bool fitsInline = nbytes <= INLINE_BUFFER_LIMIT;

        AutoSetNewObjectMetadata metadata(cx);

        const Class* clasp = templateObj->group()->clasp();
        gc::AllocKind allocKind = !fitsInline
                                  ? GetGCObjectKind(clasp)
                                  : AllocKindForLazyBuffer(nbytes);
        MOZ_ASSERT(CanBeFinalizedInBackground(allocKind, clasp));
        allocKind = GetBackgroundAllocKind(allocKind);
        RootedObjectGroup group(cx, templateObj->group());

        NewObjectKind newKind = TenuredObject;

        ScopedJSFreePtr<void> buf;
        if (!fitsInline && len > 0) {
            buf = cx->zone()->pod_malloc<uint8_t>(nbytes);
            if (!buf) {
                ReportOutOfMemory(cx);
                return nullptr;
            }

            memset(buf, 0, nbytes);
        }

        TypedArrayObject* obj = NewObjectWithGroup<TypedArrayObject>(cx, group, allocKind, newKind);
        if (!obj)
            return nullptr;

        initTypedArraySlots(obj, len);
        initTypedArrayData(cx, obj, len, buf.forget(), allocKind);

        return obj;
    }

    // ES2018 draft rev 8340bf9a8427ea81bb0d1459471afbcc91d18add
    // 22.2.4.1 TypedArray ( )
    // 22.2.4.2 TypedArray ( length )
    // 22.2.4.3 TypedArray ( typedArray )
    // 22.2.4.4 TypedArray ( object )
    // 22.2.4.5 TypedArray ( buffer [ , byteOffset [ , length ] ] )
    static bool
    class_constructor(JSContext* cx, unsigned argc, Value* vp)
    {
        CallArgs args = CallArgsFromVp(argc, vp);

        // Step 1 (22.2.4.1) or 2 (22.2.4.2-5).
        if (!ThrowIfNotConstructing(cx, args, "typed array"))
            return false;

        JSObject* obj = create(cx, args);
        if (!obj)
            return false;
        args.rval().setObject(*obj);
        return true;
    }

  private:
    static JSObject*
    create(JSContext* cx, const CallArgs& args)
    {
        MOZ_ASSERT(args.isConstructing());

        // 22.2.4.1 TypedArray ( )
        // 22.2.4.2 TypedArray ( length )
        if (args.length() == 0 || !args[0].isObject()) {
            // 22.2.4.2, step 3.
            uint64_t len;
            if (!ToIndex(cx, args.get(0), JSMSG_BAD_ARRAY_LENGTH, &len))
                return nullptr;

            // 22.2.4.1, step 3 and 22.2.4.2, step 5.
            // 22.2.4.2.1 AllocateTypedArray, step 1.
            RootedObject proto(cx);
            if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
                return nullptr;

            return fromLength(cx, len, proto);
        }

        RootedObject dataObj(cx, &args[0].toObject());

        // 22.2.4.{3,4,5}, step 4.
        // 22.2.4.2.1 AllocateTypedArray, step 1.
        RootedObject proto(cx);
        if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
            return nullptr;

        // 22.2.4.3 TypedArray ( typedArray )
        // 22.2.4.4 TypedArray ( object )
        if (!UncheckedUnwrap(dataObj)->is<ArrayBufferObjectMaybeShared>())
            return fromArray(cx, dataObj, proto);

        // 22.2.4.5 TypedArray ( buffer [ , byteOffset [ , length ] ] )

        uint64_t byteOffset = 0;
        if (args.hasDefined(1)) {
            // Step 6.
            if (!ToIndex(cx, args[1], &byteOffset))
                return nullptr;

            // Step 7.
            if (byteOffset % sizeof(NativeType) != 0) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_TYPED_ARRAY_CONSTRUCT_BOUNDS);
                return nullptr;
            }
        }

        uint64_t length = UINT64_MAX;
        if (args.hasDefined(2)) {
            // Step 8.a.
            if (!ToIndex(cx, args[2], &length))
                return nullptr;
        }

        // Steps 9-17.
        if (dataObj->is<ArrayBufferObjectMaybeShared>()) {
            HandleArrayBufferObjectMaybeShared buffer = dataObj.as<ArrayBufferObjectMaybeShared>();
            return fromBufferSameCompartment(cx, buffer, byteOffset, length, proto);
        }
        return fromBufferWrapped(cx, dataObj, byteOffset, length, proto);
    }

    // ES2018 draft rev 8340bf9a8427ea81bb0d1459471afbcc91d18add
    // 22.2.4.5 TypedArray ( buffer [ , byteOffset [ , length ] ] )
    // Steps 9-12.
    static bool
    computeAndCheckLength(JSContext* cx, HandleArrayBufferObjectMaybeShared bufferMaybeUnwrapped,
                          uint64_t byteOffset, uint64_t lengthIndex, uint32_t* length)
    {
        MOZ_ASSERT(byteOffset % sizeof(NativeType) == 0);
        MOZ_ASSERT(byteOffset < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));
        MOZ_ASSERT_IF(lengthIndex != UINT64_MAX,
                      lengthIndex < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT));

        // Step 9.
        if (bufferMaybeUnwrapped->isDetached()) {
           JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
           return false;
        }

        // Step 10.
        uint32_t bufferByteLength = bufferMaybeUnwrapped->byteLength();

        uint32_t len;
        if (lengthIndex == UINT64_MAX) {
            // Steps 11.a, 11.c.
            if (bufferByteLength % sizeof(NativeType) != 0 || byteOffset > bufferByteLength) {
                // The given byte array doesn't map exactly to
                // |sizeof(NativeType) * N| or |byteOffset| is invalid.
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_TYPED_ARRAY_CONSTRUCT_BOUNDS);
                return false;
            }

            // Step 11.b.
            uint32_t newByteLength = bufferByteLength - uint32_t(byteOffset);
            len = newByteLength / sizeof(NativeType);
        } else {
            // Step 12.a.
            uint64_t newByteLength = lengthIndex * sizeof(NativeType);

            // Step 12.b.
            if (byteOffset + newByteLength > bufferByteLength) {
                // |byteOffset + newByteLength| is too big for the arraybuffer
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_TYPED_ARRAY_CONSTRUCT_BOUNDS);
                return false;
            }

            len = uint32_t(lengthIndex);
        }

        // ArrayBuffer is too large for TypedArrays:
        // Standalone ArrayBuffers can hold up to INT32_MAX bytes, whereas
        // buffers in TypedArrays must have less than or equal to
        // |INT32_MAX - sizeof(NativeType) - INT32_MAX % sizeof(NativeType)|
        // bytes.
        if (len >= INT32_MAX / sizeof(NativeType)) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_TYPED_ARRAY_CONSTRUCT_BOUNDS);
            return false;
        }
        MOZ_ASSERT(byteOffset <= UINT32_MAX);

        *length = len;
        return true;
    }

    // ES2018 draft rev 8340bf9a8427ea81bb0d1459471afbcc91d18add
    // 22.2.4.5 TypedArray ( buffer [ , byteOffset [ , length ] ] )
    // Steps 9-17.
    static JSObject*
    fromBufferSameCompartment(JSContext* cx, HandleArrayBufferObjectMaybeShared buffer,
                              uint64_t byteOffset, uint64_t lengthIndex, HandleObject proto)
    {
        // Steps 9-12.
        uint32_t length;
        if (!computeAndCheckLength(cx, buffer, byteOffset, lengthIndex, &length))
            return nullptr;

        CreateSingleton createSingleton = CreateSingleton::No;
        if (length * sizeof(NativeType) >= TypedArrayObject::SINGLETON_BYTE_LENGTH)
            createSingleton = CreateSingleton::Yes;

        // Steps 13-17.
        return makeInstance(cx, buffer, createSingleton, uint32_t(byteOffset), length, proto);
    }

    // Create a TypedArray object in another compartment.
    //
    // ES6 supports creating a TypedArray in global A (using global A's
    // TypedArray constructor) backed by an ArrayBuffer created in global B.
    //
    // Our TypedArrayObject implementation doesn't support a TypedArray in
    // compartment A backed by an ArrayBuffer in compartment B. So in this
    // case, we create the TypedArray in B (!) and return a cross-compartment
    // wrapper.
    //
    // Extra twist: the spec says the new TypedArray's [[Prototype]] must be
    // A's TypedArray.prototype. So even though we're creating the TypedArray
    // in B, its [[Prototype]] must be (a cross-compartment wrapper for) the
    // TypedArray.prototype in A.
    static JSObject*
    fromBufferWrapped(JSContext* cx, HandleObject bufobj, uint64_t byteOffset,
                      uint64_t lengthIndex, HandleObject proto)
    {
        JSObject* unwrapped = CheckedUnwrap(bufobj);
        if (!unwrapped) {
            ReportAccessDenied(cx);
            return nullptr;
        }

        if (!unwrapped->is<ArrayBufferObjectMaybeShared>()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
            return nullptr;
        }

        RootedArrayBufferObjectMaybeShared unwrappedBuffer(cx);
        unwrappedBuffer = &unwrapped->as<ArrayBufferObjectMaybeShared>();

        uint32_t length;
        if (!computeAndCheckLength(cx, unwrappedBuffer, byteOffset, lengthIndex, &length))
            return nullptr;

        // Make sure to get the [[Prototype]] for the created typed array from
        // this compartment.
        RootedObject protoRoot(cx, proto);
        if (!protoRoot) {
            protoRoot =
                GlobalObject::getOrCreatePrototype(cx, JSCLASS_CACHED_PROTO_KEY(instanceClass()));
            if (!protoRoot)
                return nullptr;
        }

        RootedObject typedArray(cx);
        {
            JSAutoCompartment ac(cx, unwrappedBuffer);

            RootedObject wrappedProto(cx, protoRoot);
            if (!cx->compartment()->wrap(cx, &wrappedProto))
                return nullptr;

            typedArray =
                makeInstance(cx, unwrappedBuffer, CreateSingleton::No, uint32_t(byteOffset),
                             length, wrappedProto);
            if (!typedArray)
                return nullptr;
        }

        if (!cx->compartment()->wrap(cx, &typedArray))
            return nullptr;

        return typedArray;
    }

  public:
    static JSObject*
    fromBuffer(JSContext* cx, HandleObject bufobj, uint32_t byteOffset, int32_t lengthInt)
    {
        if (byteOffset % sizeof(NativeType) != 0) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_TYPED_ARRAY_CONSTRUCT_BOUNDS);
            return nullptr; // invalid byteOffset
        }

        uint64_t lengthIndex = lengthInt >= 0 ? uint64_t(lengthInt) : UINT64_MAX;
        if (bufobj->is<ArrayBufferObjectMaybeShared>()) {
            HandleArrayBufferObjectMaybeShared buffer = bufobj.as<ArrayBufferObjectMaybeShared>();
            return fromBufferSameCompartment(cx, buffer, byteOffset, lengthIndex, nullptr);
        }
        return fromBufferWrapped(cx, bufobj, byteOffset, lengthIndex, nullptr);
    }

    static bool
    maybeCreateArrayBuffer(JSContext* cx, uint32_t count, uint32_t unit,
                           HandleObject nonDefaultProto,
                           MutableHandle<ArrayBufferObject*> buffer)
    {
        if (count >= INT32_MAX / unit) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }
        uint32_t byteLength = count * unit;

        MOZ_ASSERT(byteLength < INT32_MAX);
        static_assert(INLINE_BUFFER_LIMIT % sizeof(NativeType) == 0,
                      "ArrayBuffer inline storage shouldn't waste any space");

        if (!nonDefaultProto && byteLength <= INLINE_BUFFER_LIMIT) {
            // The array's data can be inline, and the buffer created lazily.
            return true;
        }

        ArrayBufferObject* buf = ArrayBufferObject::create(cx, byteLength, nonDefaultProto);
        if (!buf)
            return false;

        buffer.set(buf);
        return true;
    }

    // 22.2.4.1 TypedArray ( )
    // 22.2.4.2 TypedArray ( length )
    static JSObject*
    fromLength(JSContext* cx, uint64_t nelements, HandleObject proto = nullptr)
    {
        // 22.2.4.1, step 1 and 22.2.4.2, steps 1-3 (performed in caller).
        // 22.2.4.1, step 2 and 22.2.4.2, step 4 (implicit).
        // 22.2.4.1, step 3 and 22.2.4.2, step 5 (call AllocateTypedArray).

        if (nelements > UINT32_MAX) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return nullptr;
        }

        Rooted<ArrayBufferObject*> buffer(cx);
        if (!maybeCreateArrayBuffer(cx, uint32_t(nelements), BYTES_PER_ELEMENT, nullptr, &buffer))
            return nullptr;

        return makeInstance(cx, buffer, CreateSingleton::No, 0, uint32_t(nelements), proto);
    }

    static bool
    AllocateArrayBuffer(JSContext* cx, HandleObject ctor,
                        uint32_t count, uint32_t unit,
                        MutableHandle<ArrayBufferObject*> buffer);

    static JSObject*
    fromArray(JSContext* cx, HandleObject other, HandleObject proto = nullptr);

    static JSObject*
    fromTypedArray(JSContext* cx, HandleObject other, bool isWrapped, HandleObject proto);

    static JSObject*
    fromObject(JSContext* cx, HandleObject other, HandleObject proto);

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

#define CREATE_TYPE_FOR_TYPED_ARRAY(T, N) \
    typedef TypedArrayObjectTemplate<T> N##Array;
JS_FOR_EACH_TYPED_ARRAY(CREATE_TYPE_FOR_TYPED_ARRAY)
#undef CREATE_TYPE_FOR_TYPED_ARRAY

} /* anonymous namespace */

TypedArrayObject*
js::TypedArrayCreateWithTemplate(JSContext* cx, HandleObject templateObj, int32_t len)
{
    MOZ_ASSERT(templateObj->is<TypedArrayObject>());
    TypedArrayObject* tobj = &templateObj->as<TypedArrayObject>();

    switch (tobj->type()) {
#define CREATE_TYPED_ARRAY(T, N) \
      case Scalar::N: \
        return TypedArrayObjectTemplate<T>::makeTypedArrayWithTemplate(cx, tobj, len);
JS_FOR_EACH_TYPED_ARRAY(CREATE_TYPED_ARRAY)
#undef CREATE_TYPED_ARRAY
      default:
        MOZ_CRASH("Unsupported TypedArray type");
    }
}

// ES2018 draft rev 2aea8f3e617b49df06414eb062ab44fad87661d3
// 24.1.1.1 AllocateArrayBuffer ( constructor, byteLength )
// byteLength = count * unit
template<typename T>
/* static */ bool
TypedArrayObjectTemplate<T>::AllocateArrayBuffer(JSContext* cx, HandleObject ctor,
                                                 uint32_t count, uint32_t unit,
                                                 MutableHandle<ArrayBufferObject*> buffer)
{
    // 24.1.1.1 step 1 (partially).
    RootedObject proto(cx);

    JSFunction* arrayBufferCtor = GlobalObject::getOrCreateArrayBufferConstructor(cx, cx->global());
    if (!arrayBufferCtor)
        return false;

    // As an optimization, skip the "prototype" lookup for %ArrayBuffer%.
    if (ctor != arrayBufferCtor) {
        // 9.1.13 OrdinaryCreateFromConstructor, steps 1-2.
        if (!GetPrototypeFromConstructor(cx, ctor, &proto))
            return false;

        JSObject* arrayBufferProto = GlobalObject::getOrCreateArrayBufferPrototype(cx, cx->global());
        if (!arrayBufferProto)
            return false;

        // Reset |proto| if it's the default %ArrayBufferPrototype%.
        if (proto == arrayBufferProto)
            proto = nullptr;
    }

    // 24.1.1.1 steps 1 (remaining part), 2-6.
    if (!maybeCreateArrayBuffer(cx, count, unit, proto, buffer))
        return false;

    return true;
}

static bool
IsArrayBufferSpecies(JSContext* cx, JSFunction* species)
{
    return IsSelfHostedFunctionWithName(species, cx->names().ArrayBufferSpecies);
}

static JSObject*
GetBufferSpeciesConstructor(JSContext* cx, Handle<TypedArrayObject*> typedArray,
                            bool isWrapped, SpeciesConstructorOverride override)
{
    RootedObject defaultCtor(cx, GlobalObject::getOrCreateArrayBufferConstructor(cx, cx->global()));
    if (!defaultCtor)
        return nullptr;

    // Use the current global's ArrayBuffer if the override is set.
    if (override == SpeciesConstructorOverride::ArrayBuffer)
        return defaultCtor;

    RootedObject obj(cx, typedArray->bufferObject());
    if (!obj) {
        MOZ_ASSERT(!isWrapped);

        // The buffer was never exposed to content code, so if
        // 1. %ArrayBufferPrototype%.constructor == %ArrayBuffer%, and
        // 2. %ArrayBuffer%[@@species] == ArrayBufferSpecies
        // we don't have to reify the buffer object and can simply return the
        // default arrray buffer constructor.

        JSObject* proto = GlobalObject::getOrCreateArrayBufferPrototype(cx, cx->global());
        if (!proto)
            return nullptr;

        Value ctor;
        if (GetOwnPropertyPure(cx, proto, NameToId(cx->names().constructor), &ctor) &&
            ctor.isObject() && &ctor.toObject() == defaultCtor)
        {
            jsid speciesId = SYMBOL_TO_JSID(cx->wellKnownSymbols().species);
            JSFunction* getter;
            if (GetOwnGetterPure(cx, defaultCtor, speciesId, &getter) && getter &&
                IsArrayBufferSpecies(cx, getter))
            {
                return defaultCtor;
            }
        }

        if (!TypedArrayObject::ensureHasBuffer(cx, typedArray))
            return nullptr;

        obj.set(typedArray->bufferObject());
    } else {
        if (isWrapped && !cx->compartment()->wrap(cx, &obj))
            return nullptr;
    }

    return SpeciesConstructor(cx, obj, defaultCtor, IsArrayBufferSpecies);
}

template<typename T>
/* static */ JSObject*
TypedArrayObjectTemplate<T>::fromArray(JSContext* cx, HandleObject other,
                                       HandleObject proto /* = nullptr */)
{
    // Allow nullptr proto for FriendAPI methods, which don't care about
    // subclassing.
    if (other->is<TypedArrayObject>())
        return fromTypedArray(cx, other, /* wrapped= */ false, proto);

    if (other->is<WrapperObject>() && UncheckedUnwrap(other)->is<TypedArrayObject>())
        return fromTypedArray(cx, other, /* wrapped= */ true, proto);

    return fromObject(cx, other, proto);
}

// ES2018 draft rev 272beb67bc5cd9fd18a220665198384108208ee1
// 22.2.4.3 TypedArray ( typedArray )
template<typename T>
/* static */ JSObject*
TypedArrayObjectTemplate<T>::fromTypedArray(JSContext* cx, HandleObject other, bool isWrapped,
                                            HandleObject proto)
{
    // Step 1.
    MOZ_ASSERT_IF(!isWrapped, other->is<TypedArrayObject>());
    MOZ_ASSERT_IF(isWrapped,
                  other->is<WrapperObject>() &&
                  UncheckedUnwrap(other)->is<TypedArrayObject>());

    // Step 2 (Already performed in caller).

    // Steps 3-4 (Allocation deferred until later).

    // Step 5.
    Rooted<TypedArrayObject*> srcArray(cx);
    if (!isWrapped) {
        srcArray = &other->as<TypedArrayObject>();
    } else {
        RootedObject unwrapped(cx, CheckedUnwrap(other));
        if (!unwrapped) {
            ReportAccessDenied(cx);
            return nullptr;
        }

        JSAutoCompartment ac(cx, unwrapped);

        srcArray = &unwrapped->as<TypedArrayObject>();

        // To keep things simpler, we always reify the array buffer for
        // wrapped typed arrays.
        if (!TypedArrayObject::ensureHasBuffer(cx, srcArray))
            return nullptr;
    }

    // Step 6 (skipped).

    // Step 7.
    if (srcArray->hasDetachedBuffer()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
        return nullptr;
    }

    // Step 9.
    uint32_t elementLength = srcArray->length();

    // Steps 10-11.
    Scalar::Type srcType = srcArray->type();

    // Steps 12-13 (skipped).

    // Steps 16-17.
    bool isShared = srcArray->isSharedMemory();
    SpeciesConstructorOverride override = isShared ? SpeciesConstructorOverride::ArrayBuffer
                                                   : SpeciesConstructorOverride::None;

    RootedObject bufferCtor(cx, GetBufferSpeciesConstructor(cx, srcArray, isWrapped, override));
    if (!bufferCtor)
        return nullptr;

    // Steps 8, 18-19.
    Rooted<ArrayBufferObject*> buffer(cx);
    if (ArrayTypeID() == srcType) {
        // Step 15.
        uint32_t byteLength = srcArray->byteLength();

        // Step 18.a.
        // 24.1.1.4 CloneArrayBuffer(...), steps 1-3.
        if (!AllocateArrayBuffer(cx, bufferCtor, byteLength, 1, &buffer))
            return nullptr;
    } else {
        // Steps 14-15, 19.a.
        if (!AllocateArrayBuffer(cx, bufferCtor, elementLength, BYTES_PER_ELEMENT, &buffer))
            return nullptr;
    }

    // Step 19.b or 24.1.1.4 step 4.
    if (srcArray->hasDetachedBuffer()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
        return nullptr;
    }

    // Steps 3-4 (remaining part), 20-23.
    Rooted<TypedArrayObject*> obj(cx, makeInstance(cx, buffer, CreateSingleton::No, 0,
                                                   elementLength, proto));
    if (!obj)
        return nullptr;

    // Steps 19.c-f or 24.1.1.4 steps 5-7.
    MOZ_ASSERT(!obj->isSharedMemory());
    if (isShared) {
        if (!ElementSpecific<T, SharedOps>::setFromTypedArray(obj, srcArray, 0))
            return nullptr;
    } else {
        if (!ElementSpecific<T, UnsharedOps>::setFromTypedArray(obj, srcArray, 0))
            return nullptr;
    }

    // Step 24.
    return obj;
}

static MOZ_ALWAYS_INLINE bool
IsOptimizableInit(JSContext* cx, HandleObject iterable, bool* optimized)
{
    MOZ_ASSERT(!*optimized);

    if (!IsPackedArray(iterable))
        return true;

    ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
    if (!stubChain)
        return false;

    return stubChain->tryOptimizeArray(cx, iterable.as<ArrayObject>(), optimized);
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 22.2.4.4 TypedArray ( object )
template<typename T>
/* static */ JSObject*
TypedArrayObjectTemplate<T>::fromObject(JSContext* cx, HandleObject other, HandleObject proto)
{
    // Steps 1-2 (Already performed in caller).

    // Steps 3-4 (Allocation deferred until later).

    bool optimized = false;
    if (!IsOptimizableInit(cx, other, &optimized))
        return nullptr;

    // Fast path when iterable is a packed array using the default iterator.
    if (optimized) {
        // Step 6.a (We don't need to call IterableToList for the fast path).
        HandleArrayObject array = other.as<ArrayObject>();

        // Step 6.b.
        uint32_t len = array->getDenseInitializedLength();

        // Step 6.c.
        Rooted<ArrayBufferObject*> buffer(cx);
        if (!maybeCreateArrayBuffer(cx, len, BYTES_PER_ELEMENT, nullptr, &buffer))
            return nullptr;

        Rooted<TypedArrayObject*> obj(cx, makeInstance(cx, buffer, CreateSingleton::No, 0,
                                                       len, proto));
        if (!obj)
            return nullptr;

        // Steps 6.d-e.
        MOZ_ASSERT(!obj->isSharedMemory());
        if (!ElementSpecific<T, UnsharedOps>::initFromIterablePackedArray(cx, obj, array))
            return nullptr;

        // Step 6.f (The assertion isn't applicable for the fast path).

        // Step 6.g.
        return obj;
    }

    // Step 5.
    RootedValue callee(cx);
    RootedId iteratorId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
    if (!GetProperty(cx, other, other, iteratorId, &callee))
        return nullptr;

    // Steps 6-8.
    RootedObject arrayLike(cx);
    if (!callee.isNullOrUndefined()) {
        // Throw if other[Symbol.iterator] isn't callable.
        if (!callee.isObject() || !callee.toObject().isCallable()) {
            RootedValue otherVal(cx, ObjectValue(*other));
            UniqueChars bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, otherVal, nullptr);
            if (!bytes)
                return nullptr;
            JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_NOT_ITERABLE,
                                       bytes.get());
            return nullptr;
        }

        FixedInvokeArgs<2> args2(cx);
        args2[0].setObject(*other);
        args2[1].set(callee);

        // Step 6.a.
        RootedValue rval(cx);
        if (!CallSelfHostedFunction(cx, cx->names().IterableToList, UndefinedHandleValue, args2,
                                    &rval))
        {
            return nullptr;
        }

        // Steps 6.b-g (Implemented in steps 9-13 below).
        arrayLike = &rval.toObject();
    } else {
        // Step 7 is an assertion: object is not an Iterator. Testing this is
        // literally the very last thing we did, so we don't assert here.

        // Step 8.
        arrayLike = other;
    }

    // Step 9.
    uint32_t len;
    if (!GetLengthProperty(cx, arrayLike, &len))
        return nullptr;

    // Step 10.
    Rooted<ArrayBufferObject*> buffer(cx);
    if (!maybeCreateArrayBuffer(cx, len, BYTES_PER_ELEMENT, nullptr, &buffer))
        return nullptr;

    Rooted<TypedArrayObject*> obj(cx, makeInstance(cx, buffer, CreateSingleton::No, 0, len,
                                                   proto));
    if (!obj)
        return nullptr;

    // Steps 11-12.
    MOZ_ASSERT(!obj->isSharedMemory());
    if (!ElementSpecific<T, UnsharedOps>::setFromNonTypedArray(cx, obj, arrayLike, len))
        return nullptr;

    // Step 13.
    return obj;
}

bool
TypedArrayConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_CALL_OR_CONSTRUCT,
                              args.isConstructing() ? "construct" : "call");
    return false;
}

/* static */ bool
TypedArrayObject::GetTemplateObjectForNative(JSContext* cx, Native native, uint32_t len,
                                             MutableHandleObject res)
{
#define CHECK_TYPED_ARRAY_CONSTRUCTOR(T, N) \
    if (native == &TypedArrayObjectTemplate<T>::class_constructor) { \
        size_t nbytes; \
        if (!js::CalculateAllocSize<T>(len, &nbytes)) \
            return true; \
        \
        if (nbytes < TypedArrayObject::SINGLETON_BYTE_LENGTH) { \
            res.set(TypedArrayObjectTemplate<T>::makeTemplateObject(cx, len)); \
            return !!res; \
        } \
    }
JS_FOR_EACH_TYPED_ARRAY(CHECK_TYPED_ARRAY_CONSTRUCTOR)
#undef CHECK_TYPED_ARRAY_CONSTRUCTOR
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
    return TypedArrayObject::Getter<TypedArrayObject::lengthValue>(cx, argc, vp);
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

/*static*/ bool
js::TypedArray_bufferGetter(JSContext* cx, unsigned argc, Value* vp)
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
    JS_SELF_HOSTED_SYM_GET(toStringTag, "TypedArrayToStringTag", 0),
    JS_PS_END
};

template<typename T>
static inline bool
SetFromTypedArray(Handle<TypedArrayObject*> target, Handle<TypedArrayObject*> source,
                  uint32_t offset)
{
    // WARNING: |source| may be an unwrapped typed array from a different
    // compartment. Proceed with caution!

    if (target->isSharedMemory() || source->isSharedMemory())
        return ElementSpecific<T, SharedOps>::setFromTypedArray(target, source, offset);
    return ElementSpecific<T, UnsharedOps>::setFromTypedArray(target, source, offset);
}

template<typename T>
static inline bool
SetFromNonTypedArray(JSContext* cx, Handle<TypedArrayObject*> target, HandleObject source,
                     uint32_t len, uint32_t offset)
{
    MOZ_ASSERT(!source->is<TypedArrayObject>(), "use SetFromTypedArray");

    if (target->isSharedMemory())
        return ElementSpecific<T, SharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
    return ElementSpecific<T, UnsharedOps>::setFromNonTypedArray(cx, target, source, len, offset);
}

// ES2017 draft rev c57ef95c45a371f9c9485bb1c3881dbdc04524a2
// 22.2.3.23 %TypedArray%.prototype.set ( overloaded [ , offset ] )
// 22.2.3.23.1 %TypedArray%.prototype.set ( array [ , offset ] )
// 22.2.3.23.2 %TypedArray%.prototype.set( typedArray [ , offset ] )
/* static */ bool
TypedArrayObject::set_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(TypedArrayObject::is(args.thisv()));

    // Steps 1-5 (Validation performed as part of CallNonGenericMethod).
    Rooted<TypedArrayObject*> target(cx, &args.thisv().toObject().as<TypedArrayObject>());

    // Steps 6-7.
    double targetOffset = 0;
    if (args.length() > 1) {
        // Step 6.
        if (!ToInteger(cx, args[1], &targetOffset))
            return false;

        // Step 7.
        if (targetOffset < 0) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
            return false;
        }
    }

    // Steps 8-9.
    if (target->hasDetachedBuffer()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
        return false;
    }

    // 22.2.3.23.1, step 15. (22.2.3.23.2 only applies if args[0] is a typed
    // array, so it doesn't make a difference there to apply ToObject here.)
    RootedObject src(cx, ToObject(cx, args.get(0)));
    if (!src)
        return false;

    Rooted<TypedArrayObject*> srcTypedArray(cx);
    {
        JSObject* obj = CheckedUnwrap(src);
        if (!obj) {
            ReportAccessDenied(cx);
            return false;
        }

        if (obj->is<TypedArrayObject>())
            srcTypedArray = &obj->as<TypedArrayObject>();
    }

    if (srcTypedArray) {
        // Remaining steps of 22.2.3.23.2.

        // WARNING: |srcTypedArray| may be an unwrapped typed array from a
        // different compartment. Proceed with caution!

        // Steps 11-12.
        if (srcTypedArray->hasDetachedBuffer()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
            return false;
        }

        // Step 10 (Reordered).
        uint32_t targetLength = target->length();

        // Step 22 (Split into two checks to provide better error messages).
        if (targetOffset > targetLength) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
            return false;
        }

        // Step 22 (Cont'd).
        uint32_t offset = uint32_t(targetOffset);
        if (srcTypedArray->length() > targetLength - offset) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }

        // Steps 13-21, 23-28.
        switch (target->type()) {
#define SET_FROM_TYPED_ARRAY(T, N) \
          case Scalar::N: \
            if (!SetFromTypedArray<T>(target, srcTypedArray, offset)) \
                return false; \
            break;
JS_FOR_EACH_TYPED_ARRAY(SET_FROM_TYPED_ARRAY)
#undef SET_FROM_TYPED_ARRAY
          default:
            MOZ_CRASH("Unsupported TypedArray type");
        }
    } else {
        // Remaining steps of 22.2.3.23.1.

        // Step 10.
        // We can't reorder this step because side-effects in step 16 can
        // detach the underlying array buffer from the typed array.
        uint32_t targetLength = target->length();

        // Step 16.
        uint32_t srcLength;
        if (!GetLengthProperty(cx, src, &srcLength))
            return false;

        // Step 17 (Split into two checks to provide better error messages).
        if (targetOffset > targetLength) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
            return false;
        }

        // Step 17 (Cont'd).
        uint32_t offset = uint32_t(targetOffset);
        if (srcLength > targetLength - offset) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return false;
        }

        // Steps 11-14, 18-21.
        if (srcLength > 0) {
            // GetLengthProperty in step 16 can lead to the execution of user
            // code which may detach the buffer. Handle this case here to
            // ensure SetFromNonTypedArray is never called with a detached
            // buffer. We still need to execute steps 21.a-b for their
            // possible side-effects.
            if (target->hasDetachedBuffer()) {
                // Steps 21.a-b.
                RootedValue v(cx);
                if (!GetElement(cx, src, src, 0, &v))
                   return false;

                double unused;
                if (!ToNumber(cx, v, &unused))
                    return false;

                // Step 21.c.
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_TYPED_ARRAY_DETACHED);
                return false;
            }

            switch (target->type()) {
#define SET_FROM_NON_TYPED_ARRAY(T, N) \
              case Scalar::N: \
                if (!SetFromNonTypedArray<T>(cx, target, src, srcLength, offset)) \
                    return false; \
                break;
JS_FOR_EACH_TYPED_ARRAY(SET_FROM_NON_TYPED_ARRAY)
#undef SET_FROM_NON_TYPED_ARRAY
              default:
                MOZ_CRASH("Unsupported TypedArray type");
            }

            // Step 21.c.
            // SetFromNonTypedArray doesn't throw when the array buffer gets
            // detached.
            if (target->hasDetachedBuffer()) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_TYPED_ARRAY_DETACHED);
                return false;
            }
        }
    }

    // Step 29/22.
    args.rval().setUndefined();
    return true;
}

/* static */ bool
TypedArrayObject::set(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<TypedArrayObject::is, TypedArrayObject::set_impl>(cx, args);
}

/* static */ const JSFunctionSpec
TypedArrayObject::protoFunctions[] = {
    JS_SELF_HOSTED_FN("subarray", "TypedArraySubarray", 2, 0),
#if 0 /* disabled until perf-testing is completed */
    JS_SELF_HOSTED_FN("set", "TypedArraySet", 2, 0),
#else
    JS_FN("set", TypedArrayObject::set, 1, 0),
#endif
    JS_SELF_HOSTED_FN("copyWithin", "TypedArrayCopyWithin", 3, 0),
    JS_SELF_HOSTED_FN("every", "TypedArrayEvery", 1, 0),
    JS_SELF_HOSTED_FN("fill", "TypedArrayFill", 3, 0),
    JS_SELF_HOSTED_FN("filter", "TypedArrayFilter", 1, 0),
    JS_SELF_HOSTED_FN("find", "TypedArrayFind", 1, 0),
    JS_SELF_HOSTED_FN("findIndex", "TypedArrayFindIndex", 1, 0),
    JS_SELF_HOSTED_FN("forEach", "TypedArrayForEach", 1, 0),
    JS_SELF_HOSTED_FN("indexOf", "TypedArrayIndexOf", 2, 0),
    JS_SELF_HOSTED_FN("join", "TypedArrayJoin", 1, 0),
    JS_SELF_HOSTED_FN("lastIndexOf", "TypedArrayLastIndexOf", 1, 0),
    JS_SELF_HOSTED_FN("map", "TypedArrayMap", 1, 0),
    JS_SELF_HOSTED_FN("reduce", "TypedArrayReduce", 1, 0),
    JS_SELF_HOSTED_FN("reduceRight", "TypedArrayReduceRight", 1, 0),
    JS_SELF_HOSTED_FN("reverse", "TypedArrayReverse", 0, 0),
    JS_SELF_HOSTED_FN("slice", "TypedArraySlice", 2, 0),
    JS_SELF_HOSTED_FN("some", "TypedArraySome", 1, 0),
    JS_SELF_HOSTED_FN("sort", "TypedArraySort", 1, 0),
    JS_SELF_HOSTED_FN("entries", "TypedArrayEntries", 0, 0),
    JS_SELF_HOSTED_FN("keys", "TypedArrayKeys", 0, 0),
    JS_SELF_HOSTED_FN("values", "TypedArrayValues", 0, 0),
    JS_SELF_HOSTED_SYM_FN(iterator, "TypedArrayValues", 0, 0),
    JS_SELF_HOSTED_FN("includes", "TypedArrayIncludes", 2, 0),
    JS_SELF_HOSTED_FN("toString", "ArrayToString", 0, 0),
    JS_SELF_HOSTED_FN("toLocaleString", "TypedArrayToLocaleString", 2, 0),
    JS_FS_END
};

/* static */ const JSFunctionSpec
TypedArrayObject::staticFunctions[] = {
    JS_SELF_HOSTED_FN("from", "TypedArrayStaticFrom", 3, 0),
    JS_SELF_HOSTED_FN("of", "TypedArrayStaticOf", 0, 0),
    JS_FS_END
};

/* static */ const JSPropertySpec
TypedArrayObject::staticProperties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "TypedArraySpecies", 0),
    JS_PS_END
};

static const ClassSpec
TypedArrayObjectSharedTypedArrayPrototypeClassSpec = {
    GenericCreateConstructor<TypedArrayConstructor, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype,
    TypedArrayObject::staticFunctions,
    TypedArrayObject::staticProperties,
    TypedArrayObject::protoFunctions,
    TypedArrayObject::protoAccessors,
    nullptr,
    ClassSpec::DontDefineConstructor
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
    JS_NULL_CLASS_OPS,
    &TypedArrayObjectSharedTypedArrayPrototypeClassSpec
};

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
      case Scalar::Int64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
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

#ifdef JS_MORE_DETERMINISTIC
    // See the comment in ElementSpecific::doubleToNative.
    d = JS::CanonicalizeNaN(d);
#endif

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
      case Scalar::Int64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Int32x4:
      case Scalar::MaxTypedArrayViewType:
        break;
    }

    MOZ_CRASH("Unknown TypedArray type");
}

void
TypedArrayObject::getElements(Value* vp)
{
    uint32_t length = this->length();
    MOZ_ASSERT_IF(length > 0, !hasDetachedBuffer());

    switch (type()) {
#define GET_ELEMENTS(T, N) \
      case Scalar::N: \
        for (uint32_t i = 0; i < length; ++i, ++vp) \
            *vp = N##Array::getIndexValue(this, i); \
        break;
JS_FOR_EACH_TYPED_ARRAY(GET_ELEMENTS)
#undef GET_ELEMENTS
      default:
        MOZ_CRASH("Unknown TypedArray type");
    }
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

static const ClassOps TypedArrayClassOps = {
    nullptr,                 /* addProperty */
    nullptr,                 /* delProperty */
    nullptr,                 /* enumerate   */
    nullptr,                 /* newEnumerate */
    nullptr,                 /* resolve     */
    nullptr,                 /* mayResolve  */
    TypedArrayObject::finalize, /* finalize    */
    nullptr,                 /* call        */
    nullptr,                 /* hasInstance */
    nullptr,                 /* construct   */
    TypedArrayObject::trace, /* trace  */
};

static const ClassExtension TypedArrayClassExtension = {
    nullptr,
    TypedArrayObject::objectMoved,
};

#define IMPL_TYPED_ARRAY_PROPERTIES(_type)                                     \
{                                                                              \
JS_INT32_PS("BYTES_PER_ELEMENT", _type##Array::BYTES_PER_ELEMENT,              \
            JSPROP_READONLY | JSPROP_PERMANENT),                               \
JS_PS_END                                                                      \
}

static const JSPropertySpec static_prototype_properties[Scalar::MaxTypedArrayViewType][2] = {
    IMPL_TYPED_ARRAY_PROPERTIES(Int8),
    IMPL_TYPED_ARRAY_PROPERTIES(Uint8),
    IMPL_TYPED_ARRAY_PROPERTIES(Int16),
    IMPL_TYPED_ARRAY_PROPERTIES(Uint16),
    IMPL_TYPED_ARRAY_PROPERTIES(Int32),
    IMPL_TYPED_ARRAY_PROPERTIES(Uint32),
    IMPL_TYPED_ARRAY_PROPERTIES(Float32),
    IMPL_TYPED_ARRAY_PROPERTIES(Float64),
    IMPL_TYPED_ARRAY_PROPERTIES(Uint8Clamped)
};

#define IMPL_TYPED_ARRAY_CLASS_SPEC(_type)                                     \
{                                                                              \
    _type##Array::createConstructor,                                           \
    _type##Array::createPrototype,                                             \
    nullptr,                                                                   \
    static_prototype_properties[Scalar::Type::_type],                          \
    nullptr,                                                                   \
    static_prototype_properties[Scalar::Type::_type],                          \
    nullptr,                                                                   \
    JSProto_TypedArray                                                         \
}

static const ClassSpec TypedArrayObjectClassSpecs[Scalar::MaxTypedArrayViewType] = {
    IMPL_TYPED_ARRAY_CLASS_SPEC(Int8),
    IMPL_TYPED_ARRAY_CLASS_SPEC(Uint8),
    IMPL_TYPED_ARRAY_CLASS_SPEC(Int16),
    IMPL_TYPED_ARRAY_CLASS_SPEC(Uint16),
    IMPL_TYPED_ARRAY_CLASS_SPEC(Int32),
    IMPL_TYPED_ARRAY_CLASS_SPEC(Uint32),
    IMPL_TYPED_ARRAY_CLASS_SPEC(Float32),
    IMPL_TYPED_ARRAY_CLASS_SPEC(Float64),
    IMPL_TYPED_ARRAY_CLASS_SPEC(Uint8Clamped)
};

#define IMPL_TYPED_ARRAY_CLASS(_type)                                          \
{                                                                              \
    #_type "Array",                                                            \
    JSCLASS_HAS_RESERVED_SLOTS(TypedArrayObject::RESERVED_SLOTS) |             \
    JSCLASS_HAS_PRIVATE |                                                      \
    JSCLASS_HAS_CACHED_PROTO(JSProto_##_type##Array) |                         \
    JSCLASS_DELAY_METADATA_BUILDER |                                           \
    JSCLASS_SKIP_NURSERY_FINALIZE |                                            \
    JSCLASS_BACKGROUND_FINALIZE,                                               \
    &TypedArrayClassOps,                                                       \
    &TypedArrayObjectClassSpecs[Scalar::Type::_type],                          \
    &TypedArrayClassExtension                                                  \
}

const Class TypedArrayObject::classes[Scalar::MaxTypedArrayViewType] = {
    IMPL_TYPED_ARRAY_CLASS(Int8),
    IMPL_TYPED_ARRAY_CLASS(Uint8),
    IMPL_TYPED_ARRAY_CLASS(Int16),
    IMPL_TYPED_ARRAY_CLASS(Uint16),
    IMPL_TYPED_ARRAY_CLASS(Int32),
    IMPL_TYPED_ARRAY_CLASS(Uint32),
    IMPL_TYPED_ARRAY_CLASS(Float32),
    IMPL_TYPED_ARRAY_CLASS(Float64),
    IMPL_TYPED_ARRAY_CLASS(Uint8Clamped)
};

// The various typed array prototypes are supposed to 1) be normal objects,
// 2) stringify to "[object <name of constructor>]", and 3) (Gecko-specific)
// be xrayable.  The first and second requirements mandate (in the absence of
// @@toStringTag) a custom class.  The third requirement mandates that each
// prototype's class have the relevant typed array's cached JSProtoKey in them.
// Thus we need one class with cached prototype per kind of typed array, with a
// delegated ClassSpec.
#define IMPL_TYPED_ARRAY_PROTO_CLASS(_type) \
{ \
    /*
     * Actually ({}).toString.call(Uint8Array.prototype) should throw, because
     * Uint8Array.prototype lacks the the typed array internal slots.  (Same as
     * with %TypedArray%.prototype.)  It's not clear this is desirable (see
     * above), but it's what we've always done, so keep doing it till we
     * implement @@toStringTag or ES6 changes.
     */ \
    #_type "ArrayPrototype", \
    JSCLASS_HAS_CACHED_PROTO(JSProto_##_type##Array), \
    JS_NULL_CLASS_OPS, \
    &TypedArrayObjectClassSpecs[Scalar::Type::_type] \
}

const Class TypedArrayObject::protoClasses[Scalar::MaxTypedArrayViewType] = {
    IMPL_TYPED_ARRAY_PROTO_CLASS(Int8),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Uint8),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Int16),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Uint16),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Int32),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Uint32),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Float32),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Float64),
    IMPL_TYPED_ARRAY_PROTO_CLASS(Uint8Clamped)
};

/* static */ bool
TypedArrayObject::isOriginalLengthGetter(Native native)
{
    return native == TypedArray_lengthGetter;
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

bool
js::IsBufferSource(JSObject* object, SharedMem<uint8_t*>* dataPointer, size_t* byteLength)
{
    if (object->is<TypedArrayObject>()) {
        TypedArrayObject& view = object->as<TypedArrayObject>();
        *dataPointer = view.viewDataEither().cast<uint8_t*>();
        *byteLength = view.byteLength();
        return true;
    }

    if (object->is<DataViewObject>()) {
        DataViewObject& view = object->as<DataViewObject>();
        *dataPointer = view.dataPointerEither().cast<uint8_t*>();
        *byteLength = view.byteLength();
    }

    if (object->is<ArrayBufferObject>()) {
        ArrayBufferObject& buffer = object->as<ArrayBufferObject>();
        *dataPointer = buffer.dataPointerShared();
        *byteLength = buffer.byteLength();
        return true;
    }

    if (object->is<SharedArrayBufferObject>()) {
        SharedArrayBufferObject& buffer = object->as<SharedArrayBufferObject>();
        *dataPointer = buffer.dataPointerShared();
        *byteLength = buffer.byteLength();
        return true;
    }

    return false;
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
    MOZ_ASSERT(obj->is<TypedArrayObject>());

    // These are all substeps of 3.b.

    // Steps i-iii are handled by the caller.

    // Steps iv-v.
    // We (wrongly) ignore out of range defines with a value.
    uint32_t length = obj->as<TypedArrayObject>().length();
    if (index >= length)
        return result.succeed();

    // Step vi.
    if (desc.isAccessorDescriptor())
        return result.fail(JSMSG_CANT_REDEFINE_PROP);

    // Step vii.
    if (desc.hasConfigurable() && desc.configurable())
        return result.fail(JSMSG_CANT_REDEFINE_PROP);

    // Step viii.
    if (desc.hasEnumerable() && !desc.enumerable())
        return result.fail(JSMSG_CANT_REDEFINE_PROP);

    // Step ix.
    if (desc.hasWritable() && !desc.writable())
        return result.fail(JSMSG_CANT_REDEFINE_PROP);

    // Step x.
    if (desc.hasValue()) {
        // The following step numbers refer to 9.4.5.9
        // IntegerIndexedElementSet.

        // Steps 1-2 are enforced by the caller.

        // Step 3.
        double numValue;
        if (!ToNumber(cx, desc.value(), &numValue))
            return false;

        // Steps 4-5, 8-9.
        if (obj->as<TypedArrayObject>().hasDetachedBuffer())
            return result.fail(JSMSG_TYPED_ARRAY_DETACHED);

        // Steps 10-16.
        TypedArrayObject::setElement(obj->as<TypedArrayObject>(), index, numValue);
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
    if (obj->is<DataViewObject>())
        return Scalar::MaxTypedArrayViewType;
    MOZ_CRASH("invalid ArrayBufferView type");
}

JS_FRIEND_API(int8_t*)
JS_GetInt8ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
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
JS_GetUint8ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
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
JS_GetUint8ClampedArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
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
JS_GetInt16ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
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
JS_GetUint16ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
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
JS_GetInt32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
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
JS_GetUint32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
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
JS_GetFloat32ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
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
JS_GetFloat64ArrayData(JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    TypedArrayObject* tarr = &obj->as<TypedArrayObject>();
    MOZ_ASSERT((int32_t) tarr->type() == Scalar::Float64);
    *isSharedMemory = tarr->isSharedMemory();
    return static_cast<double*>(tarr->viewDataEither().unwrap(/*safe - caller sees isSharedMemory*/));
}
