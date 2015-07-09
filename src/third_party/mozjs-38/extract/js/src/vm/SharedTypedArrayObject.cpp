/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SharedTypedArrayObject.h"

#include "mozilla/Alignment.h"
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

#include "asmjs/AsmJSModule.h"
#include "asmjs/AsmJSValidate.h"
#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "js/Conversions.h"
#include "vm/ArrayBufferObject.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/SharedArrayObject.h"
#include "vm/TypedArrayCommon.h"
#include "vm/WrapperObject.h"

#include "jsatominlines.h"
#include "jsobjinlines.h"

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

TypedArrayLayout SharedTypedArrayObject::layout_(true, // shared
                                                 false, // neuterable
                                                 &SharedTypedArrayObject::classes[0],
                                                 &SharedTypedArrayObject::classes[Scalar::MaxTypedArrayViewType]);

inline void
InitSharedArrayBufferViewDataPointer(SharedTypedArrayObject* obj, SharedArrayBufferObject* buffer, size_t byteOffset)
{
    /*
     * N.B. The base of the array's data is stored in the object's
     * private data rather than a slot to avoid the restriction that
     * private Values that are pointers must have the low bits clear.
     */
    MOZ_ASSERT(buffer->dataPointer() != nullptr);
    obj->initPrivate(buffer->dataPointer() + byteOffset);
}

// See note in TypedArrayObject.cpp about how we can probably merge
// the below type with the one in that file, once TypedArrayObject is
// less dissimilar from SharedTypedArrayObject (ie, when it is closer
// to ES6).

template<typename NativeType>
class SharedTypedArrayObjectTemplate : public SharedTypedArrayObject
{
    // A value that signifies that we should use the buffer up to the end.
    static const uint32_t LENGTH_NOT_PROVIDED = (uint32_t)-1;

    // This is the max implementation value of 'length': 2^31-1.
    // The reason it is not 2^32-2 is due to Bug 1068458: most of the
    // TypedArray code limits the length to INT32_MAX.
    static const uint32_t MAX_LENGTH = INT32_MAX;

    // This is the max value of 'byteOffset': one below the length.
    static const uint32_t MAX_BYTEOFFSET = MAX_LENGTH - 1;

  public:
    typedef NativeType ElementType;
    typedef SharedTypedArrayObjectTemplate<NativeType> ThisTypedArrayObject;
    typedef SharedArrayBufferObject BufferType;

    static Scalar::Type ArrayTypeID() { return TypeIDOfType<NativeType>(); }
    static bool ArrayTypeIsUnsigned() { return TypeIsUnsigned<NativeType>(); }
    static bool ArrayTypeIsFloatingPoint() { return TypeIsFloatingPoint<NativeType>(); }

    static const size_t BYTES_PER_ELEMENT = sizeof(ElementType);

    static inline const Class* protoClass()
    {
        return &SharedTypedArrayObject::protoClasses[ArrayTypeID()];
    }

    static JSObject* CreatePrototype(JSContext* cx, JSProtoKey key)
    {
        return cx->global()->createBlankPrototype(cx, protoClass());
    }

    static bool FinishClassInit(JSContext* cx, HandleObject ctor, HandleObject proto);

    static inline const Class* instanceClass()
    {
        return &SharedTypedArrayObject::classes[ArrayTypeID()];
    }

    static bool is(HandleValue v) {
        return v.isObject() && v.toObject().hasClass(instanceClass());
    }

    static SharedTypedArrayObject*
    makeProtoInstance(JSContext* cx, HandleObject proto, AllocKind allocKind)
    {
        MOZ_ASSERT(proto);

        RootedObject obj(cx, NewBuiltinClassInstance(cx, instanceClass(), allocKind));
        if (!obj)
            return nullptr;

        ObjectGroup* group = ObjectGroup::defaultNewGroup(cx, obj->getClass(), TaggedProto(proto.get()));
        if (!group)
            return nullptr;
        obj->setGroup(group);

        return &obj->as<SharedTypedArrayObject>();
    }

    static SharedTypedArrayObject*
    makeTypedInstance(JSContext* cx, uint32_t len, AllocKind allocKind)
    {
        MOZ_ASSERT(len <= MAX_LENGTH / sizeof(NativeType));

        // Multiplication is safe due to preconditions for makeInstance().
        if (len * sizeof(NativeType) >= SharedTypedArrayObject::SINGLETON_TYPE_BYTE_LENGTH) {
            return &NewBuiltinClassInstance(cx, instanceClass(), allocKind,
                                            SingletonObject)->as<SharedTypedArrayObject>();
        }

        jsbytecode* pc;
        RootedScript script(cx, cx->currentScript(&pc));
        NewObjectKind newKind = GenericObject;
        if (script && ObjectGroup::useSingletonForAllocationSite(script, pc, instanceClass()))
            newKind = SingletonObject;
        RootedObject obj(cx, NewBuiltinClassInstance(cx, instanceClass(), allocKind, newKind));
        if (!obj)
            return nullptr;

        if (script && !ObjectGroup::setAllocationSiteObjectGroup(cx, script, pc, obj,
                                                                 newKind == SingletonObject))
        {
            return nullptr;
        }

        return &obj->as<SharedTypedArrayObject>();
    }

    static JSObject*
    makeInstance(JSContext* cx, Handle<SharedArrayBufferObject*> buffer, uint32_t byteOffset, uint32_t len,
                 HandleObject proto)
    {
        MOZ_ASSERT(buffer);
        MOZ_ASSERT(byteOffset <= MAX_BYTEOFFSET);
        MOZ_ASSERT(len <= MAX_LENGTH / sizeof(NativeType));

        gc::AllocKind allocKind = GetGCObjectKind(instanceClass());

        Rooted<SharedTypedArrayObject*> obj(cx);
        if (proto)
            obj = makeProtoInstance(cx, proto, allocKind);
        else
            obj = makeTypedInstance(cx, len, allocKind);
        if (!obj)
            return nullptr;

        obj->setSlot(BUFFER_SLOT, ObjectOrNullValue(buffer));

        InitSharedArrayBufferViewDataPointer(obj, buffer, byteOffset);

        obj->setSlot(LENGTH_SLOT, Int32Value(len));
        obj->setSlot(BYTEOFFSET_SLOT, Int32Value(byteOffset));

#ifdef DEBUG
        if (buffer) {
            uint32_t arrayByteLength = obj->byteLength();
            uint32_t arrayByteOffset = obj->byteOffset();
            uint32_t bufferByteLength = buffer->byteLength();
            MOZ_ASSERT(bufferByteLength - arrayByteOffset >= arrayByteLength);
            MOZ_ASSERT(arrayByteOffset <= bufferByteLength);
        }

        // Verify that the private slot is at the expected place
        MOZ_ASSERT(obj->numFixedSlots() == DATA_SLOT);
#endif

        return obj;
    }

    static JSObject*
    makeInstance(JSContext* cx, Handle<SharedArrayBufferObject*> bufobj, uint32_t byteOffset, uint32_t len)
    {
        RootedObject nullproto(cx, nullptr);
        return makeInstance(cx, bufobj, byteOffset, len, nullproto);
    }

    /*
     * Shared{Type}Array(object)
     *
     * new Shared{Type}Array(length)
     * new Shared{Type}Array(SharedArrayBuffer, [optional] byteOffset, [optional] length)
     */
    static bool
    class_constructor(JSContext* cx, unsigned argc, Value* vp)
    {
        CallArgs args = CallArgsFromVp(argc, vp);

        if (!args.isConstructing()) {
            if (args.hasDefined(0) &&
                args[0].isObject() &&
                args[0].toObject().is<SharedTypedArrayObject>() &&
                AnyTypedArrayType(&args[0].toObject()) == ArrayTypeID())
            {
                args.rval().set(args[0]);
                return true;
            }
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_SHARED_TYPED_ARRAY_BAD_LENGTH);
            return false;
        }

        JSObject* obj = create(cx, args);
        if (!obj)
            return false;
        args.rval().setObject(*obj);
        return true;
    }

    static JSObject*
    create(JSContext* cx, const CallArgs& args)
    {
        if (args.length() == 0)
            return fromLength(cx, 0);

        // Case 1: (length)
        if (!args[0].isObject()) {
            // If not an object then it must be a length.
            uint32_t length;
            bool overflow;
            if (!ToLengthClamped(cx, args[0], &length, &overflow)) {
                // Bug 1068458: Limit length to 2^31-1.
                if (overflow || length > INT32_MAX)
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
                return nullptr;
            }
            return fromLength(cx, length);
        }

        // Case 2: (SharedArrayBuffer, [byteOffset, [length]])
        RootedObject dataObj(cx, &args.get(0).toObject());

        if (!UncheckedUnwrap(dataObj)->is<SharedArrayBufferObject>()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_SHARED_TYPED_ARRAY_BAD_OBJECT);
            return nullptr;
        }

        uint32_t byteOffset = 0;
        uint32_t length = LENGTH_NOT_PROVIDED;
        if (args.length() > 1) {
            double numByteOffset;
            if (!ToInteger(cx, args[1], &numByteOffset))
                return nullptr;

            if (numByteOffset < 0 || numByteOffset > MAX_BYTEOFFSET) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                                     JSMSG_SHARED_TYPED_ARRAY_ARG_RANGE, "'byteOffset'");
                return nullptr;
            }
            byteOffset = (uint32_t)numByteOffset;

            if (args.length() > 2) {
                bool overflow;
                if (!ToLengthClamped(cx, args[2], &length, &overflow)) {
                    // Bug 1068458: Limit length to 2^31-1.
                    if (overflow || length > INT32_MAX)
                        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                                             JSMSG_SHARED_TYPED_ARRAY_ARG_RANGE, "'length'");
                    return nullptr;
                }
            }
        }

        return fromBuffer(cx, dataObj, byteOffset, length);
    }

    template<Value ValueGetter(SharedTypedArrayObject* tarr)>
    static bool
    GetterImpl(JSContext* cx, CallArgs args)
    {
        MOZ_ASSERT(is(args.thisv()));
        args.rval().set(ValueGetter(&args.thisv().toObject().as<SharedTypedArrayObject>()));
        return true;
    }

    // ValueGetter is a function that takes an unwrapped typed array object and
    // returns a Value. Given such a function, Getter<> is a native that
    // retrieves a given Value, probably from a slot on the object.
    template<Value ValueGetter(SharedTypedArrayObject* tarr)>
    static bool
    Getter(JSContext* cx, unsigned argc, Value* vp)
    {
        CallArgs args = CallArgsFromVp(argc, vp);
        return CallNonGenericMethod(cx, is, GetterImpl<ValueGetter>, args);
    }

    static bool
    BufferGetterImpl(JSContext* cx, CallArgs args)
    {
        MOZ_ASSERT(is(args.thisv()));
        Rooted<SharedTypedArrayObject*> tarray(cx, &args.thisv().toObject().as<SharedTypedArrayObject>());
        args.rval().set(bufferValue(tarray));
        return true;
    }

    static bool
    BufferGetter(JSContext* cx, unsigned argc, Value* vp)
    {
        CallArgs args = CallArgsFromVp(argc, vp);
        return CallNonGenericMethod(cx, is, BufferGetterImpl, args);
    }

    // Define an accessor for a read-only property that invokes a native getter
    static bool
    DefineGetter(JSContext* cx, HandleNativeObject proto, PropertyName* name, Native native)
    {
        RootedId id(cx, NameToId(name));
        unsigned attrs = JSPROP_SHARED | JSPROP_GETTER;

        Rooted<GlobalObject*> global(cx, cx->compartment()->maybeGlobal());
        JSObject* getter = NewFunction(cx, NullPtr(), native, 0,
                                       JSFunction::NATIVE_FUN, global, NullPtr());
        if (!getter)
            return false;

        return NativeDefineProperty(cx, proto, id, UndefinedHandleValue,
                                    JS_DATA_TO_FUNC_PTR(PropertyOp, getter), nullptr,
                                    attrs);
    }

    static const NativeType
    getIndex(JSObject* obj, uint32_t index)
    {
        SharedTypedArrayObject& tarray = obj->as<SharedTypedArrayObject>();
        MOZ_ASSERT(index < tarray.length());
        return static_cast<const NativeType*>(tarray.viewData())[index];
    }

    static void
    setIndexValue(SharedTypedArrayObject& tarray, uint32_t index, double d)
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

    static void
    setIndex(SharedTypedArrayObject& tarray, uint32_t index, NativeType val)
    {
        MOZ_ASSERT(index < tarray.length());
        static_cast<NativeType*>(tarray.viewData())[index] = val;
    }

    static Value getIndexValue(JSObject* tarray, uint32_t index);

    static bool fun_subarray(JSContext* cx, unsigned argc, Value* vp);
    static bool fun_copyWithin(JSContext* cx, unsigned argc, Value* vp);
    static bool fun_set(JSContext* cx, unsigned argc, Value* vp);

  public:
    static JSObject*
    fromBufferWithProto(JSContext* cx, HandleObject bufobj, uint32_t byteOffset, uint32_t length,
                        HandleObject proto)
    {
        if (!ObjectClassIs(bufobj, ESClass_SharedArrayBuffer, cx)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_SHARED_TYPED_ARRAY_BAD_OBJECT);
            return nullptr; // must be SharedArrayBuffer
        }

        if (bufobj->is<ProxyObject>()) {
            // Complicated, see TypedArrayObject.cpp for code.  For now, punt.
            JS_ReportError(cx, "Permission denied to access object");
            return nullptr;
        }

        Rooted<SharedArrayBufferObject*> buffer(cx, &AsSharedArrayBuffer(bufobj));

        if (byteOffset > buffer->byteLength() || byteOffset % sizeof(NativeType) != 0) {
            // Invalid byteOffset.
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_SHARED_TYPED_ARRAY_BAD_ARGS);
            return nullptr;
        }

        uint32_t bytesAvailable = buffer->byteLength() - byteOffset;

        if (length == LENGTH_NOT_PROVIDED) {
            if (bytesAvailable % sizeof(NativeType) != 0) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_SHARED_TYPED_ARRAY_BAD_ARGS);
                return nullptr;
            }
            length = bytesAvailable / sizeof(NativeType);
        }

        if (length > MAX_LENGTH / sizeof(NativeType) || length * sizeof(NativeType) > bytesAvailable) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return nullptr;
        }

        return makeInstance(cx, buffer, byteOffset, length, proto);
    }

    static JSObject*
    fromBuffer(JSContext* cx, HandleObject bufobj, uint32_t byteOffset, uint32_t length)
    {
        RootedObject proto(cx, nullptr);
        return fromBufferWithProto(cx, bufobj, byteOffset, length, proto);
    }

    static JSObject*
    fromLength(JSContext* cx, uint32_t nelements)
    {
        if (nelements > MAX_LENGTH / sizeof(NativeType)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_ARRAY_LENGTH);
            return nullptr;
        }
        Rooted<SharedArrayBufferObject*> buffer(
            cx, SharedArrayBufferObject::New(cx, nelements * sizeof(NativeType)));
        if (!buffer)
            return nullptr;
        return makeInstance(cx, buffer, 0, nelements);
    }
};

class SharedInt8ArrayObject : public SharedTypedArrayObjectTemplate<int8_t> {
  public:
    enum { ACTUAL_TYPE = Scalar::Int8 };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};
class SharedUint8ArrayObject : public SharedTypedArrayObjectTemplate<uint8_t> {
  public:
    enum { ACTUAL_TYPE = Scalar::Uint8 };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};
class SharedInt16ArrayObject : public SharedTypedArrayObjectTemplate<int16_t> {
  public:
    enum { ACTUAL_TYPE = Scalar::Int16 };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};
class SharedUint16ArrayObject : public SharedTypedArrayObjectTemplate<uint16_t> {
  public:
    enum { ACTUAL_TYPE = Scalar::Uint16 };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};
class SharedInt32ArrayObject : public SharedTypedArrayObjectTemplate<int32_t> {
  public:
    enum { ACTUAL_TYPE = Scalar::Int32 };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};
class SharedUint32ArrayObject : public SharedTypedArrayObjectTemplate<uint32_t> {
  public:
    enum { ACTUAL_TYPE = Scalar::Uint32 };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};
class SharedFloat32ArrayObject : public SharedTypedArrayObjectTemplate<float> {
  public:
    enum { ACTUAL_TYPE = Scalar::Float32 };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};
class SharedFloat64ArrayObject : public SharedTypedArrayObjectTemplate<double> {
  public:
    enum { ACTUAL_TYPE = Scalar::Float64 };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};
class SharedUint8ClampedArrayObject : public SharedTypedArrayObjectTemplate<uint8_clamped> {
  public:
    enum { ACTUAL_TYPE = Scalar::Uint8Clamped };
    static const JSFunctionSpec jsfuncs[];
    static const JSPropertySpec jsprops[];
};

/* static */ bool
SharedTypedArrayObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().is<SharedTypedArrayObject>();
}

template<typename T>
struct SharedTypedArrayObject::OfType
{
    typedef SharedTypedArrayObjectTemplate<T> Type;
};

// The different typed arrays each have their own prototypes, but those
// prototypes are mostly empty.  Typed array methods instead are defined a
// single time on a single %TypedArray%.prototype object.  These functions
// behave identically on any kind of typed array.  (It's irrelevant right here,
// but each typed array constructor inherits from the %TypedArray% function
// to similarly share functions that live on the constructor.)
//
// In contrast, shared typed array methods are duplicated on each different
// shared typed array prototype (likewise for functions on the constructors),
// and each function only works on the corresponding kind of shared typed array
// (enforced by SharedTypedArrayObjectTemplate<T>::is in these methods).
//
// It's probably worth changing the shared typed array spec to use a similar
// %SharedTypedArray% structure to avoid duplication at some point.

/*
 * SharedTypedArrayObject boilerplate
 */

#define IMPL_SHARED_TYPED_ARRAY_STATICS(_typedArray)                               \
bool                                                                               \
Shared##_typedArray##Object_subarray(JSContext* cx, unsigned argc, Value* vp)      \
{                                                                                  \
    CallArgs args = CallArgsFromVp(argc, vp);                                      \
    return CallNonGenericMethod<Shared##_typedArray##Object::is,                   \
                                TypedArrayMethods<SharedTypedArrayObject>::subarray>(cx, args);\
}                                                                                  \
                                                                                   \
bool                                                                               \
Shared##_typedArray##Object_copyWithin(JSContext* cx, unsigned argc, Value* vp)    \
{                                                                                  \
    CallArgs args = CallArgsFromVp(argc, vp);                                      \
    return CallNonGenericMethod<Shared##_typedArray##Object::is,                   \
                                TypedArrayMethods<SharedTypedArrayObject>::copyWithin>(cx, args);\
}                                                                                  \
                                                                                   \
bool                                                                               \
Shared##_typedArray##Object_set(JSContext* cx, unsigned argc, Value* vp)           \
{                                                                                  \
    CallArgs args = CallArgsFromVp(argc, vp);                                      \
    return CallNonGenericMethod<Shared##_typedArray##Object::is,                   \
                                TypedArrayMethods<SharedTypedArrayObject>::set>(cx, args);\
}                                                                                  \
                                                                                   \
const JSFunctionSpec Shared##_typedArray##Object::jsfuncs[] = {                    \
    JS_FN("subarray", Shared##_typedArray##Object_subarray, 2, 0),                 \
    JS_FN("set", Shared##_typedArray##Object_set, 2, 0),                           \
    JS_FN("copyWithin", Shared##_typedArray##Object_copyWithin, 2, 0),             \
    JS_FS_END                                                                      \
};                                                                                 \
/* These next 3 functions are brought to you by the buggy GCC we use to build      \
   B2G ICS. Older GCC versions have a bug in which they fail to compile            \
   reinterpret_casts of templated functions with the message: "insufficient        \
   contextual information to determine type". JS_PSG needs to                      \
   reinterpret_cast<JSPropertyOp>, so this causes problems for us here.            \
                                                                                   \
   We could restructure all this code to make this nicer, but since ICS isn't      \
   going to be around forever (and since this bug is fixed with the newer GCC      \
   versions we use on JB and KK), the workaround here is designed for ease of      \
   removal. When you stop seeing ICS Emulator builds on TBPL, remove these 3       \
   JSNatives and insert the templated callee directly into the JS_PSG below. */    \
bool Shared##_typedArray##_lengthGetter(JSContext* cx, unsigned argc, Value* vp) {         \
    return Shared##_typedArray##Object::Getter<Shared##_typedArray##Object::lengthValue>(cx, argc, vp); \
}                                                                                  \
bool Shared##_typedArray##_byteLengthGetter(JSContext* cx, unsigned argc, Value* vp) {     \
    return Shared##_typedArray##Object::Getter<Shared##_typedArray##Object::byteLengthValue>(cx, argc, vp); \
}                                                                                  \
bool Shared##_typedArray##_byteOffsetGetter(JSContext* cx, unsigned argc, Value* vp) {     \
    return Shared##_typedArray##Object::Getter<Shared##_typedArray##Object::byteOffsetValue>(cx, argc, vp); \
}                                                                                  \
const JSPropertySpec Shared##_typedArray##Object::jsprops[] = {                            \
    JS_PSG("length", Shared##_typedArray##_lengthGetter, 0),                               \
    JS_PSG("buffer", Shared##_typedArray##Object::BufferGetter, 0),                        \
    JS_PSG("byteLength", Shared##_typedArray##_byteLengthGetter, 0),                       \
    JS_PSG("byteOffset", Shared##_typedArray##_byteOffsetGetter, 0),                       \
    JS_PS_END                                                                      \
};

#define IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Name,NativeType)                             \
  JS_FRIEND_API(JSObject*) JS_NewShared ## Name ## Array(JSContext* cx, uint32_t nelements)    \
  {                                                                                             \
      return SharedTypedArrayObjectTemplate<NativeType>::fromLength(cx, nelements);             \
  }                                                                                             \
  JS_FRIEND_API(JSObject*) JS_NewShared ## Name ## ArrayWithBuffer(JSContext* cx,              \
                               HandleObject arrayBuffer, uint32_t byteOffset, uint32_t length)  \
  {                                                                                             \
      return SharedTypedArrayObjectTemplate<NativeType>::fromBuffer(cx, arrayBuffer, byteOffset,\
                                                                    length);                    \
  }                                                                                             \
  JS_FRIEND_API(bool) JS_IsShared ## Name ## Array(JSObject* obj)                               \
  {                                                                                             \
      if (!(obj = CheckedUnwrap(obj)))                                                          \
          return false;                                                                         \
      const Class* clasp = obj->getClass();                                                     \
      const Scalar::Type id = SharedTypedArrayObjectTemplate<NativeType>::ArrayTypeID();        \
      return clasp == &SharedTypedArrayObject::classes[id];                                     \
  } \
  JS_FRIEND_API(JSObject*) js::UnwrapShared ## Name ## Array(JSObject* obj)                    \
  {                                                                                             \
      obj = CheckedUnwrap(obj);                                                                 \
      if (!obj)                                                                                 \
          return nullptr;                                                                       \
      const Class* clasp = obj->getClass();                                                     \
      const Scalar::Type id = SharedTypedArrayObjectTemplate<NativeType>::ArrayTypeID();        \
      if (clasp == &SharedTypedArrayObject::classes[id])                                        \
          return obj;                                                                           \
      return nullptr;                                                                           \
  } \
  const js::Class* const js::detail::Shared ## Name ## ArrayClassPtr =                          \
      &js::SharedTypedArrayObject::classes[SharedTypedArrayObjectTemplate<NativeType>::ArrayTypeID()];

IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Int8, int8_t)
IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Uint8, uint8_t)
IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Uint8Clamped, uint8_clamped)
IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Int16, int16_t)
IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Uint16, uint16_t)
IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Int32, int32_t)
IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Uint32, uint32_t)
IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Float32, float)
IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS(Float64, double)

#define IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Name, ExternalType, InternalType)       \
  JS_FRIEND_API(JSObject*) JS_GetObjectAsShared ## Name ## Array(JSObject* obj,            \
                                                                  uint32_t* length,         \
                                                                  ExternalType** data)      \
  {                                                                                         \
      if (!(obj = CheckedUnwrap(obj)))                                                      \
          return nullptr;                                                                   \
                                                                                            \
      const Class* clasp = obj->getClass();                                                 \
      const Scalar::Type id = SharedTypedArrayObjectTemplate<InternalType>::ArrayTypeID();  \
      if (clasp != &SharedTypedArrayObject::classes[id])                                    \
          return nullptr;                                                                   \
                                                                                            \
      SharedTypedArrayObject* tarr = &obj->as<SharedTypedArrayObject>();                    \
      *length = tarr->length();                                                             \
      *data = static_cast<ExternalType*>(tarr->viewData());                                \
                                                                                            \
      return obj;                                                                           \
  }

IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Int8, int8_t, int8_t)
IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Uint8, uint8_t, uint8_t)
IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Uint8Clamped, uint8_t, uint8_clamped)
IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Int16, int16_t, int16_t)
IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Uint16, uint16_t, uint16_t)
IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Int32, int32_t, int32_t)
IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Uint32, uint32_t, uint32_t)
IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Float32, float, float)
IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS(Float64, double, double)

#define SHARED_TYPED_ARRAY_CLASS_SPEC(_typedArray)                             \
{                                                                              \
    GenericCreateConstructor<Shared##_typedArray##Object::class_constructor, 3, \
                             JSFunction::FinalizeKind>,                        \
    Shared##_typedArray##Object::CreatePrototype,                              \
    nullptr,                                                                   \
    Shared##_typedArray##Object::jsfuncs,                                      \
    Shared##_typedArray##Object::jsprops,                                      \
    Shared##_typedArray##Object::FinishClassInit                               \
}

#define IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(_typedArray)                       \
{                                                                              \
    "Shared" #_typedArray "Prototype",                                         \
    JSCLASS_HAS_RESERVED_SLOTS(SharedTypedArrayObject::RESERVED_SLOTS) |       \
    JSCLASS_HAS_PRIVATE |                                                      \
    JSCLASS_HAS_CACHED_PROTO(JSProto_Shared##_typedArray),                     \
    nullptr,                 /* addProperty */                                 \
    nullptr,                 /* delProperty */                                 \
    nullptr,                 /* getProperty */                                 \
    nullptr,                 /* setProperty */                                 \
    nullptr,                 /* enumerate   */                                 \
    nullptr,                 /* resolve     */                                 \
    nullptr,                 /* convert     */                                 \
    nullptr,                 /* finalize    */                                 \
    nullptr,                 /* call        */                                 \
    nullptr,                 /* hasInstance */                                 \
    nullptr,                 /* construct   */                                 \
    nullptr,                 /* trace  */                                      \
    SHARED_TYPED_ARRAY_CLASS_SPEC(_typedArray)                                 \
}

#define IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(_typedArray)                        \
{                                                                              \
    "Shared" #_typedArray,                                                     \
    JSCLASS_HAS_RESERVED_SLOTS(SharedTypedArrayObject::RESERVED_SLOTS) |       \
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |                        \
    JSCLASS_HAS_CACHED_PROTO(JSProto_Shared##_typedArray),                     \
    nullptr,                 /* addProperty */                                 \
    nullptr,                 /* delProperty */                                 \
    nullptr,                 /* getProperty */                                 \
    nullptr,                 /* setProperty */                                 \
    nullptr,                 /* enumerate   */                                 \
    nullptr,                 /* resolve     */                                 \
    nullptr,                 /* convert     */                                 \
    nullptr,                 /* finalize    */                                 \
    nullptr,                 /* call        */                                 \
    nullptr,                 /* hasInstance */                                 \
    nullptr,                 /* construct   */                                 \
    nullptr,                 /* trace  */                                      \
    SHARED_TYPED_ARRAY_CLASS_SPEC(_typedArray)                                 \
}

template<typename NativeType>
bool
SharedTypedArrayObjectTemplate<NativeType>::FinishClassInit(JSContext* cx,
                                                            HandleObject ctor,
                                                            HandleObject proto)
{
    RootedValue bytesValue(cx, Int32Value(BYTES_PER_ELEMENT));

    if (!DefineProperty(cx, ctor, cx->names().BYTES_PER_ELEMENT, bytesValue,
                        nullptr, nullptr, JSPROP_PERMANENT | JSPROP_READONLY) ||
        !DefineProperty(cx, proto, cx->names().BYTES_PER_ELEMENT, bytesValue,
                        nullptr, nullptr, JSPROP_PERMANENT | JSPROP_READONLY))
    {
        return false;
    }

    return true;
};

IMPL_SHARED_TYPED_ARRAY_STATICS(Int8Array)
IMPL_SHARED_TYPED_ARRAY_STATICS(Uint8Array)
IMPL_SHARED_TYPED_ARRAY_STATICS(Int16Array)
IMPL_SHARED_TYPED_ARRAY_STATICS(Uint16Array)
IMPL_SHARED_TYPED_ARRAY_STATICS(Int32Array)
IMPL_SHARED_TYPED_ARRAY_STATICS(Uint32Array)
IMPL_SHARED_TYPED_ARRAY_STATICS(Float32Array)
IMPL_SHARED_TYPED_ARRAY_STATICS(Float64Array)
IMPL_SHARED_TYPED_ARRAY_STATICS(Uint8ClampedArray)

const Class SharedTypedArrayObject::classes[Scalar::MaxTypedArrayViewType] = {
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Int8Array),
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Uint8Array),
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Int16Array),
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Uint16Array),
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Int32Array),
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Uint32Array),
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Float32Array),
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Float64Array),
    IMPL_SHARED_TYPED_ARRAY_FAST_CLASS(Uint8ClampedArray)
};

const Class SharedTypedArrayObject::protoClasses[Scalar::MaxTypedArrayViewType] = {
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Int8Array),
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Uint8Array),
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Int16Array),
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Uint16Array),
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Int32Array),
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Uint32Array),
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Float32Array),
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Float64Array),
    IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS(Uint8ClampedArray)
};

// this default implementation is only valid for integer types
// less than 32-bits in size.
template<typename NativeType>
Value
SharedTypedArrayObjectTemplate<NativeType>::getIndexValue(JSObject* tarray, uint32_t index)
{
    JS_STATIC_ASSERT(sizeof(NativeType) < 4);

    return Int32Value(getIndex(tarray, index));
}

// and we need to specialize for 32-bit integers and floats
template<>
Value
SharedTypedArrayObjectTemplate<int32_t>::getIndexValue(JSObject* tarray, uint32_t index)
{
    return Int32Value(getIndex(tarray, index));
}

template<>
Value
SharedTypedArrayObjectTemplate<uint32_t>::getIndexValue(JSObject* tarray, uint32_t index)
{
    uint32_t val = getIndex(tarray, index);
    return NumberValue(val);
}

template<>
Value
SharedTypedArrayObjectTemplate<float>::getIndexValue(JSObject* tarray, uint32_t index)
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
SharedTypedArrayObjectTemplate<double>::getIndexValue(JSObject* tarray, uint32_t index)
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

/* static */ bool
SharedTypedArrayObject::isOriginalLengthGetter(Scalar::Type type, Native native)
{
    switch (type) {
      case Scalar::Int8:
        return native == SharedInt8Array_lengthGetter;
      case Scalar::Uint8:
        return native == SharedUint8Array_lengthGetter;
      case Scalar::Uint8Clamped:
        return native == SharedUint8ClampedArray_lengthGetter;
      case Scalar::Int16:
        return native == SharedInt16Array_lengthGetter;
      case Scalar::Uint16:
        return native == SharedUint16Array_lengthGetter;
      case Scalar::Int32:
        return native == SharedInt32Array_lengthGetter;
      case Scalar::Uint32:
        return native == SharedUint32Array_lengthGetter;
      case Scalar::Float32:
        return native == SharedFloat32Array_lengthGetter;
      case Scalar::Float64:
        return native == SharedFloat64Array_lengthGetter;
      default:
        MOZ_CRASH("Unknown TypedArray type");
    }
}

bool
js::IsSharedTypedArrayConstructor(HandleValue v, uint32_t type)
{
    switch (type) {
      case Scalar::Int8:
        return IsNativeFunction(v, SharedInt8ArrayObject::class_constructor);
      case Scalar::Uint8:
        return IsNativeFunction(v, SharedUint8ArrayObject::class_constructor);
      case Scalar::Int16:
        return IsNativeFunction(v, SharedInt16ArrayObject::class_constructor);
      case Scalar::Uint16:
        return IsNativeFunction(v, SharedUint16ArrayObject::class_constructor);
      case Scalar::Int32:
        return IsNativeFunction(v, SharedInt32ArrayObject::class_constructor);
      case Scalar::Uint32:
        return IsNativeFunction(v, SharedUint32ArrayObject::class_constructor);
      case Scalar::Float32:
        return IsNativeFunction(v, SharedFloat32ArrayObject::class_constructor);
      case Scalar::Float64:
        return IsNativeFunction(v, SharedFloat64ArrayObject::class_constructor);
      case Scalar::Uint8Clamped:
        return IsNativeFunction(v, SharedUint8ClampedArrayObject::class_constructor);
    }
    MOZ_CRASH("unexpected shared typed array type");
}

JS_FRIEND_API(bool)
JS_IsSharedTypedArrayObject(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    return obj && obj->is<SharedTypedArrayObject>();
}

SharedArrayBufferObject*
SharedTypedArrayObject::buffer() const
{
    return &bufferValue(const_cast<SharedTypedArrayObject*>(this)).toObject().as<SharedArrayBufferObject>();
}

Value
SharedTypedArrayObject::getElement(uint32_t index)
{
    switch (type()) {
      case Scalar::Int8:
        return SharedTypedArrayObjectTemplate<int8_t>::getIndexValue(this, index);
        break;
      case Scalar::Uint8:
        return SharedTypedArrayObjectTemplate<uint8_t>::getIndexValue(this, index);
        break;
      case Scalar::Uint8Clamped:
        return SharedTypedArrayObjectTemplate<uint8_clamped>::getIndexValue(this, index);
        break;
      case Scalar::Int16:
        return SharedTypedArrayObjectTemplate<int16_t>::getIndexValue(this, index);
        break;
      case Scalar::Uint16:
        return SharedTypedArrayObjectTemplate<uint16_t>::getIndexValue(this, index);
        break;
      case Scalar::Int32:
        return SharedTypedArrayObjectTemplate<int32_t>::getIndexValue(this, index);
        break;
      case Scalar::Uint32:
        return SharedTypedArrayObjectTemplate<uint32_t>::getIndexValue(this, index);
        break;
      case Scalar::Float32:
        return SharedTypedArrayObjectTemplate<float>::getIndexValue(this, index);
        break;
      case Scalar::Float64:
        return SharedTypedArrayObjectTemplate<double>::getIndexValue(this, index);
        break;
      default:
        MOZ_CRASH("Unknown SharedTypedArray type");
    }
}

void
SharedTypedArrayObject::setElement(SharedTypedArrayObject& obj, uint32_t index, double d)
{
    MOZ_ASSERT(index < obj.length());

    switch (obj.type()) {
      case Scalar::Int8:
        SharedTypedArrayObjectTemplate<int8_t>::setIndexValue(obj, index, d);
        break;
      case Scalar::Uint8:
        SharedTypedArrayObjectTemplate<uint8_t>::setIndexValue(obj, index, d);
        break;
      case Scalar::Uint8Clamped:
        SharedTypedArrayObjectTemplate<uint8_clamped>::setIndexValue(obj, index, d);
        break;
      case Scalar::Int16:
        SharedTypedArrayObjectTemplate<int16_t>::setIndexValue(obj, index, d);
        break;
      case Scalar::Uint16:
        SharedTypedArrayObjectTemplate<uint16_t>::setIndexValue(obj, index, d);
        break;
      case Scalar::Int32:
        SharedTypedArrayObjectTemplate<int32_t>::setIndexValue(obj, index, d);
        break;
      case Scalar::Uint32:
        SharedTypedArrayObjectTemplate<uint32_t>::setIndexValue(obj, index, d);
        break;
      case Scalar::Float32:
        SharedTypedArrayObjectTemplate<float>::setIndexValue(obj, index, d);
        break;
      case Scalar::Float64:
        SharedTypedArrayObjectTemplate<double>::setIndexValue(obj, index, d);
        break;
      default:
        MOZ_CRASH("Unknown SharedTypedArray type");
    }
}

#undef IMPL_SHARED_TYPED_ARRAY_STATICS
#undef IMPL_SHARED_TYPED_ARRAY_JSAPI_CONSTRUCTORS
#undef IMPL_SHARED_TYPED_ARRAY_COMBINED_UNWRAPPERS
#undef SHARED_TYPED_ARRAY_CLASS_SPEC
#undef IMPL_SHARED_TYPED_ARRAY_PROTO_CLASS
#undef IMPL_SHARED_TYPED_ARRAY_FAST_CLASS
