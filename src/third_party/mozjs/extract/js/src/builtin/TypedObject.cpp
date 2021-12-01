/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/TypedObject-inl.h"

#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"

#include "jsutil.h"

#include "builtin/SIMD.h"
#include "gc/Marking.h"
#include "js/Vector.h"
#include "util/StringBuffer.h"
#include "vm/GlobalObject.h"
#include "vm/JSCompartment.h"
#include "vm/JSFunction.h"
#include "vm/StringType.h"
#include "vm/TypedArrayObject.h"

#include "gc/Nursery-inl.h"
#include "gc/StoreBuffer-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using mozilla::AssertedCast;
using mozilla::CheckedInt32;
using mozilla::IsPowerOfTwo;
using mozilla::PodCopy;
using mozilla::PointerRangeSize;

using namespace js;

const Class js::TypedObjectModuleObject::class_ = {
    "TypedObject",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_TypedObject)
};

static const JSFunctionSpec TypedObjectMethods[] = {
    JS_SELF_HOSTED_FN("objectType", "TypeOfTypedObject", 1, 0),
    JS_SELF_HOSTED_FN("storage", "StorageOfTypedObject", 1, 0),
    JS_FS_END
};

static void
ReportCannotConvertTo(JSContext* cx, HandleValue fromValue, const char* toType)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                              InformalValueTypeName(fromValue), toType);
}

template<class T>
static inline T*
ToObjectIf(HandleValue value)
{
    if (!value.isObject())
        return nullptr;

    if (!value.toObject().is<T>())
        return nullptr;

    return &value.toObject().as<T>();
}

static inline CheckedInt32
RoundUpToAlignment(CheckedInt32 address, uint32_t align)
{
    MOZ_ASSERT(IsPowerOfTwo(align));

    // Note: Be careful to order operators such that we first make the
    // value smaller and then larger, so that we don't get false
    // overflow errors due to (e.g.) adding `align` and then
    // subtracting `1` afterwards when merely adding `align-1` would
    // not have overflowed. Note that due to the nature of two's
    // complement representation, if `address` is already aligned,
    // then adding `align-1` cannot itself cause an overflow.

    return ((address + (align - 1)) / align) * align;
}

/*
 * Overwrites the contents of `typedObj` at offset `offset` with `val`
 * converted to the type `typeObj`. This is done by delegating to
 * self-hosted code. This is used for assignments and initializations.
 *
 * For example, consider the final assignment in this snippet:
 *
 *    var Point = new StructType({x: float32, y: float32});
 *    var Line = new StructType({from: Point, to: Point});
 *    var line = new Line();
 *    line.to = {x: 22, y: 44};
 *
 * This would result in a call to `ConvertAndCopyTo`
 * where:
 * - typeObj = Point
 * - typedObj = line
 * - offset = sizeof(Point) == 8
 * - val = {x: 22, y: 44}
 * This would result in loading the value of `x`, converting
 * it to a float32, and hen storing it at the appropriate offset,
 * and then doing the same for `y`.
 *
 * Note that the type of `typeObj` may not be the
 * type of `typedObj` but rather some subcomponent of `typedObj`.
 */
static bool
ConvertAndCopyTo(JSContext* cx,
                 HandleTypeDescr typeObj,
                 HandleTypedObject typedObj,
                 int32_t offset,
                 HandleAtom name,
                 HandleValue val)
{
    RootedFunction func(cx, SelfHostedFunction(cx, cx->names().ConvertAndCopyTo));
    if (!func)
        return false;

    FixedInvokeArgs<5> args(cx);

    args[0].setObject(*typeObj);
    args[1].setObject(*typedObj);
    args[2].setInt32(offset);
    if (name)
        args[3].setString(name);
    else
        args[3].setNull();
    args[4].set(val);

    RootedValue fval(cx, ObjectValue(*func));
    RootedValue dummy(cx); // ignored by ConvertAndCopyTo
    return js::Call(cx, fval, dummy, args, &dummy);
}

static bool
ConvertAndCopyTo(JSContext* cx, HandleTypedObject typedObj, HandleValue val)
{
    Rooted<TypeDescr*> type(cx, &typedObj->typeDescr());
    return ConvertAndCopyTo(cx, type, typedObj, 0, nullptr, val);
}

/*
 * Overwrites the contents of `typedObj` at offset `offset` with `val`
 * converted to the type `typeObj`
 */
static bool
Reify(JSContext* cx,
      HandleTypeDescr type,
      HandleTypedObject typedObj,
      size_t offset,
      MutableHandleValue to)
{
    RootedFunction func(cx, SelfHostedFunction(cx, cx->names().Reify));
    if (!func)
        return false;

    FixedInvokeArgs<3> args(cx);

    args[0].setObject(*type);
    args[1].setObject(*typedObj);
    args[2].setInt32(offset);

    RootedValue fval(cx, ObjectValue(*func));
    return js::Call(cx, fval, UndefinedHandleValue, args, to);
}

// Extracts the `prototype` property from `obj`, throwing if it is
// missing or not an object.
static JSObject*
GetPrototype(JSContext* cx, HandleObject obj)
{
    RootedValue prototypeVal(cx);
    if (!GetProperty(cx, obj, obj, cx->names().prototype,
                               &prototypeVal))
    {
        return nullptr;
    }
    if (!prototypeVal.isObject()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INVALID_PROTOTYPE);
        return nullptr;
    }
    return &prototypeVal.toObject();
}

/***************************************************************************
 * Typed Prototypes
 *
 * Every type descriptor has an associated prototype. Instances of
 * that type descriptor use this as their prototype. Per the spec,
 * typed object prototypes cannot be mutated.
 */

const Class js::TypedProto::class_ = {
    "TypedProto",
    JSCLASS_HAS_RESERVED_SLOTS(JS_TYPROTO_SLOTS)
};

/***************************************************************************
 * Scalar type objects
 *
 * Scalar type objects like `uint8`, `uint16`, are all instances of
 * the ScalarTypeDescr class. Like all type objects, they have a reserved
 * slot pointing to a TypeRepresentation object, which is used to
 * distinguish which scalar type object this actually is.
 */

static const ClassOps ScalarTypeDescrClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    TypeDescr::finalize,
    ScalarTypeDescr::call
};

const Class js::ScalarTypeDescr::class_ = {
    "Scalar",
    JSCLASS_HAS_RESERVED_SLOTS(JS_DESCR_SLOTS) | JSCLASS_BACKGROUND_FINALIZE,
    &ScalarTypeDescrClassOps
};

const JSFunctionSpec js::ScalarTypeDescr::typeObjectMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    JS_SELF_HOSTED_FN("array", "ArrayShorthand", 1, 0),
    JS_SELF_HOSTED_FN("equivalent", "TypeDescrEquivalent", 1, 0),
    JS_FS_END
};

uint32_t
ScalarTypeDescr::size(Type t)
{
    return AssertedCast<uint32_t>(Scalar::byteSize(t));
}

uint32_t
ScalarTypeDescr::alignment(Type t)
{
    return AssertedCast<uint32_t>(Scalar::byteSize(t));
}

/*static*/ const char*
ScalarTypeDescr::typeName(Type type)
{
    switch (type) {
#define NUMERIC_TYPE_TO_STRING(constant_, type_, name_) \
        case constant_: return #name_;
        JS_FOR_EACH_SCALAR_TYPE_REPR(NUMERIC_TYPE_TO_STRING)
#undef NUMERIC_TYPE_TO_STRING
      case Scalar::Int64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Int32x4:
      case Scalar::MaxTypedArrayViewType:
        break;
    }
    MOZ_CRASH("Invalid type");
}

bool
ScalarTypeDescr::call(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                                  args.callee().getClass()->name, "0", "s");
        return false;
    }

    Rooted<ScalarTypeDescr*> descr(cx, &args.callee().as<ScalarTypeDescr>());
    ScalarTypeDescr::Type type = descr->type();

    double number;
    if (!ToNumber(cx, args[0], &number))
        return false;

    if (type == Scalar::Uint8Clamped)
        number = ClampDoubleToUint8(number);

    switch (type) {
#define SCALARTYPE_CALL(constant_, type_, name_)                             \
      case constant_: {                                                       \
          type_ converted = ConvertScalar<type_>(number);                     \
          args.rval().setNumber((double) converted);                          \
          return true;                                                        \
      }

        JS_FOR_EACH_SCALAR_TYPE_REPR(SCALARTYPE_CALL)
#undef SCALARTYPE_CALL
      case Scalar::Int64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Int32x4:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH();
    }
    return true;
}

/***************************************************************************
 * Reference type objects
 *
 * Reference type objects like `Any` or `Object` basically work the
 * same way that the scalar type objects do. There is one class with
 * many instances, and each instance has a reserved slot with a
 * TypeRepresentation object, which is used to distinguish which
 * reference type object this actually is.
 */

static const ClassOps ReferenceTypeDescrClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    TypeDescr::finalize,
    ReferenceTypeDescr::call
};

const Class js::ReferenceTypeDescr::class_ = {
    "Reference",
    JSCLASS_HAS_RESERVED_SLOTS(JS_DESCR_SLOTS) | JSCLASS_BACKGROUND_FINALIZE,
    &ReferenceTypeDescrClassOps
};

const JSFunctionSpec js::ReferenceTypeDescr::typeObjectMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    {"array", {nullptr, nullptr}, 1, 0, "ArrayShorthand"},
    {"equivalent", {nullptr, nullptr}, 1, 0, "TypeDescrEquivalent"},
    JS_FS_END
};

static const uint32_t ReferenceSizes[] = {
#define REFERENCE_SIZE(_kind, _type, _name)                        \
    sizeof(_type),
    JS_FOR_EACH_REFERENCE_TYPE_REPR(REFERENCE_SIZE) 0
#undef REFERENCE_SIZE
};

uint32_t
ReferenceTypeDescr::size(Type t)
{
    return ReferenceSizes[t];
}

uint32_t
ReferenceTypeDescr::alignment(Type t)
{
    return ReferenceSizes[t];
}

/*static*/ const char*
ReferenceTypeDescr::typeName(Type type)
{
    switch (type) {
#define NUMERIC_TYPE_TO_STRING(constant_, type_, name_) \
        case constant_: return #name_;
        JS_FOR_EACH_REFERENCE_TYPE_REPR(NUMERIC_TYPE_TO_STRING)
#undef NUMERIC_TYPE_TO_STRING
    }
    MOZ_CRASH("Invalid type");
}

bool
js::ReferenceTypeDescr::call(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    MOZ_ASSERT(args.callee().is<ReferenceTypeDescr>());
    Rooted<ReferenceTypeDescr*> descr(cx, &args.callee().as<ReferenceTypeDescr>());

    if (args.length() < 1) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                                  descr->typeName(), "0", "s");
        return false;
    }

    switch (descr->type()) {
      case ReferenceTypeDescr::TYPE_ANY:
        args.rval().set(args[0]);
        return true;

      case ReferenceTypeDescr::TYPE_OBJECT:
      {
        RootedObject obj(cx, ToObject(cx, args[0]));
        if (!obj)
            return false;
        args.rval().setObject(*obj);
        return true;
      }

      case ReferenceTypeDescr::TYPE_STRING:
      {
        RootedString obj(cx, ToString<CanGC>(cx, args[0]));
        if (!obj)
            return false;
        args.rval().setString(&*obj);
        return true;
      }
    }

    MOZ_CRASH("Unhandled Reference type");
}

/***************************************************************************
 * SIMD type objects
 *
 * Note: these are partially defined in SIMD.cpp
 */

SimdType
SimdTypeDescr::type() const {
    uint32_t t = uint32_t(getReservedSlot(JS_DESCR_SLOT_TYPE).toInt32());
    MOZ_ASSERT(t < uint32_t(SimdType::Count));
    return SimdType(t);
}

uint32_t
SimdTypeDescr::size(SimdType t)
{
    MOZ_ASSERT(unsigned(t) < unsigned(SimdType::Count));
    switch (t) {
      case SimdType::Int8x16:
      case SimdType::Int16x8:
      case SimdType::Int32x4:
      case SimdType::Uint8x16:
      case SimdType::Uint16x8:
      case SimdType::Uint32x4:
      case SimdType::Float32x4:
      case SimdType::Float64x2:
      case SimdType::Bool8x16:
      case SimdType::Bool16x8:
      case SimdType::Bool32x4:
      case SimdType::Bool64x2:
        return 16;
      case SimdType::Count:
        break;
    }
    MOZ_CRASH("unexpected SIMD type");
}

uint32_t
SimdTypeDescr::alignment(SimdType t)
{
    MOZ_ASSERT(unsigned(t) < unsigned(SimdType::Count));
    return size(t);
}

/***************************************************************************
 * ArrayMetaTypeDescr class
 */

/*
 * For code like:
 *
 *   var A = new TypedObject.ArrayType(uint8, 10);
 *   var S = new TypedObject.StructType({...});
 *
 * As usual, the [[Prototype]] of A is
 * TypedObject.ArrayType.prototype.  This permits adding methods to
 * all ArrayType types, by setting
 * TypedObject.ArrayType.prototype.methodName = function() { ... }.
 * The same holds for S with respect to TypedObject.StructType.
 *
 * We may also want to add methods to *instances* of an ArrayType:
 *
 *   var a = new A();
 *   var s = new S();
 *
 * As usual, the [[Prototype]] of a is A.prototype.  What's
 * A.prototype?  It's an empty object, and you can set
 * A.prototype.methodName = function() { ... } to add a method to all
 * A instances.  (And the same with respect to s and S.)
 *
 * But what if you want to add a method to all ArrayType instances,
 * not just all A instances?  (Or to all StructType instances.)  The
 * [[Prototype]] of the A.prototype empty object is
 * TypedObject.ArrayType.prototype.prototype (two .prototype levels!).
 * So just set TypedObject.ArrayType.prototype.prototype.methodName =
 * function() { ... } to add a method to all ArrayType instances.
 * (And, again, same with respect to s and S.)
 *
 * This function creates the A.prototype/S.prototype object. It returns an
 * empty object with the .prototype.prototype object as its [[Prototype]].
 */
static TypedProto*
CreatePrototypeObjectForComplexTypeInstance(JSContext* cx, HandleObject ctorPrototype)
{
    RootedObject ctorPrototypePrototype(cx, GetPrototype(cx, ctorPrototype));
    if (!ctorPrototypePrototype)
        return nullptr;

    return NewObjectWithGivenProto<TypedProto>(cx, ctorPrototypePrototype, SingletonObject);
}

static const ClassOps ArrayTypeDescrClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    TypeDescr::finalize,
    nullptr, /* call */
    nullptr, /* hasInstance */
    TypedObject::construct
};

const Class ArrayTypeDescr::class_ = {
    "ArrayType",
    JSCLASS_HAS_RESERVED_SLOTS(JS_DESCR_SLOTS) | JSCLASS_BACKGROUND_FINALIZE,
    &ArrayTypeDescrClassOps
};

const JSPropertySpec ArrayMetaTypeDescr::typeObjectProperties[] = {
    JS_PS_END
};

const JSFunctionSpec ArrayMetaTypeDescr::typeObjectMethods[] = {
    {"array", {nullptr, nullptr}, 1, 0, "ArrayShorthand"},
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    {"equivalent", {nullptr, nullptr}, 1, 0, "TypeDescrEquivalent"},
    JS_SELF_HOSTED_FN("build",    "TypedObjectArrayTypeBuild", 3, 0),
    JS_SELF_HOSTED_FN("from",     "TypedObjectArrayTypeFrom", 3, 0),
    JS_FS_END
};

const JSPropertySpec ArrayMetaTypeDescr::typedObjectProperties[] = {
    JS_PS_END
};

const JSFunctionSpec ArrayMetaTypeDescr::typedObjectMethods[] = {
    {"forEach", {nullptr, nullptr}, 1, 0, "ArrayForEach"},
    {"redimension", {nullptr, nullptr}, 1, 0, "TypedObjectArrayRedimension"},
    JS_SELF_HOSTED_FN("map",        "TypedObjectArrayMap",        2, 0),
    JS_SELF_HOSTED_FN("reduce",     "TypedObjectArrayReduce",     2, 0),
    JS_SELF_HOSTED_FN("filter",     "TypedObjectArrayFilter",     1, 0),
    JS_FS_END
};

bool
js::CreateUserSizeAndAlignmentProperties(JSContext* cx, HandleTypeDescr descr)
{
    // If data is transparent, also store the public slots.
    if (descr->transparent()) {
        // byteLength
        RootedValue typeByteLength(cx, Int32Value(AssertedCast<int32_t>(descr->size())));
        if (!DefineDataProperty(cx, descr, cx->names().byteLength, typeByteLength,
                                JSPROP_READONLY | JSPROP_PERMANENT))
        {
            return false;
        }

        // byteAlignment
        RootedValue typeByteAlignment(cx, Int32Value(descr->alignment()));
        if (!DefineDataProperty(cx, descr, cx->names().byteAlignment, typeByteAlignment,
                                JSPROP_READONLY | JSPROP_PERMANENT))
        {
            return false;
        }
    } else {
        // byteLength
        if (!DefineDataProperty(cx, descr, cx->names().byteLength, UndefinedHandleValue,
                                JSPROP_READONLY | JSPROP_PERMANENT))
        {
            return false;
        }

        // byteAlignment
        if (!DefineDataProperty(cx, descr, cx->names().byteAlignment, UndefinedHandleValue,
                                JSPROP_READONLY | JSPROP_PERMANENT))
        {
            return false;
        }
    }

    return true;
}

static bool
CreateTraceList(JSContext* cx, HandleTypeDescr descr);

ArrayTypeDescr*
ArrayMetaTypeDescr::create(JSContext* cx,
                           HandleObject arrayTypePrototype,
                           HandleTypeDescr elementType,
                           HandleAtom stringRepr,
                           int32_t size,
                           int32_t length)
{
    MOZ_ASSERT(arrayTypePrototype);
    Rooted<ArrayTypeDescr*> obj(cx);
    obj = NewObjectWithGivenProto<ArrayTypeDescr>(cx, arrayTypePrototype, SingletonObject);
    if (!obj)
        return nullptr;

    obj->initReservedSlot(JS_DESCR_SLOT_KIND, Int32Value(ArrayTypeDescr::Kind));
    obj->initReservedSlot(JS_DESCR_SLOT_STRING_REPR, StringValue(stringRepr));
    obj->initReservedSlot(JS_DESCR_SLOT_ALIGNMENT, Int32Value(elementType->alignment()));
    obj->initReservedSlot(JS_DESCR_SLOT_SIZE, Int32Value(size));
    obj->initReservedSlot(JS_DESCR_SLOT_OPAQUE, BooleanValue(elementType->opaque()));
    obj->initReservedSlot(JS_DESCR_SLOT_ARRAY_ELEM_TYPE, ObjectValue(*elementType));
    obj->initReservedSlot(JS_DESCR_SLOT_ARRAY_LENGTH, Int32Value(length));

    RootedValue elementTypeVal(cx, ObjectValue(*elementType));
    if (!DefineDataProperty(cx, obj, cx->names().elementType, elementTypeVal,
                            JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return nullptr;
    }

    RootedValue lengthValue(cx, NumberValue(length));
    if (!DefineDataProperty(cx, obj, cx->names().length, lengthValue,
                            JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return nullptr;
    }

    if (!CreateUserSizeAndAlignmentProperties(cx, obj))
        return nullptr;

    // All arrays with the same element type have the same prototype. This
    // prototype is created lazily and stored in the element type descriptor.
    Rooted<TypedProto*> prototypeObj(cx);
    if (elementType->getReservedSlot(JS_DESCR_SLOT_ARRAYPROTO).isObject()) {
        prototypeObj = &elementType->getReservedSlot(JS_DESCR_SLOT_ARRAYPROTO).toObject().as<TypedProto>();
    } else {
        prototypeObj = CreatePrototypeObjectForComplexTypeInstance(cx, arrayTypePrototype);
        if (!prototypeObj)
            return nullptr;
        elementType->setReservedSlot(JS_DESCR_SLOT_ARRAYPROTO, ObjectValue(*prototypeObj));
    }

    obj->initReservedSlot(JS_DESCR_SLOT_TYPROTO, ObjectValue(*prototypeObj));

    if (!LinkConstructorAndPrototype(cx, obj, prototypeObj))
        return nullptr;

    if (!CreateTraceList(cx, obj))
        return nullptr;

    if (!cx->zone()->addTypeDescrObject(cx, obj)) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    return obj;
}

bool
ArrayMetaTypeDescr::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "ArrayType"))
        return false;

    RootedObject arrayTypeGlobal(cx, &args.callee());

    // Expect two arguments. The first is a type object, the second is a length.
    if (args.length() < 2) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                                  "ArrayType", "1", "");
        return false;
    }

    if (!args[0].isObject() || !args[0].toObject().is<TypeDescr>()) {
        ReportCannotConvertTo(cx, args[0], "ArrayType element specifier");
        return false;
    }

    if (!args[1].isInt32() || args[1].toInt32() < 0) {
        ReportCannotConvertTo(cx, args[1], "ArrayType length specifier");
        return false;
    }

    Rooted<TypeDescr*> elementType(cx, &args[0].toObject().as<TypeDescr>());

    int32_t length = args[1].toInt32();

    // Compute the byte size.
    CheckedInt32 size = CheckedInt32(elementType->size()) * length;
    if (!size.isValid()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_TOO_BIG);
        return false;
    }

    // Construct a canonical string `new ArrayType(<elementType>, N)`:
    StringBuffer contents(cx);
    if (!contents.append("new ArrayType("))
        return false;
    if (!contents.append(&elementType->stringRepr()))
        return false;
    if (!contents.append(", "))
        return false;
    if (!NumberValueToStringBuffer(cx, NumberValue(length), contents))
        return false;
    if (!contents.append(")"))
        return false;
    RootedAtom stringRepr(cx, contents.finishAtom());
    if (!stringRepr)
        return false;

    // Extract ArrayType.prototype
    RootedObject arrayTypePrototype(cx, GetPrototype(cx, arrayTypeGlobal));
    if (!arrayTypePrototype)
        return false;

    // Create the instance of ArrayType
    Rooted<ArrayTypeDescr*> obj(cx);
    obj = create(cx, arrayTypePrototype, elementType, stringRepr, size.value(), length);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

bool
js::IsTypedObjectArray(JSObject& obj)
{
    if (!obj.is<TypedObject>())
        return false;
    TypeDescr& d = obj.as<TypedObject>().typeDescr();
    return d.is<ArrayTypeDescr>();
}

/*********************************
 * StructType class
 */

static const ClassOps StructTypeDescrClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    TypeDescr::finalize,
    nullptr, /* call */
    nullptr, /* hasInstance */
    TypedObject::construct
};

const Class StructTypeDescr::class_ = {
    "StructType",
    JSCLASS_HAS_RESERVED_SLOTS(JS_DESCR_SLOTS) | JSCLASS_BACKGROUND_FINALIZE,
    &StructTypeDescrClassOps
};

const JSPropertySpec StructMetaTypeDescr::typeObjectProperties[] = {
    JS_PS_END
};

const JSFunctionSpec StructMetaTypeDescr::typeObjectMethods[] = {
    {"array", {nullptr, nullptr}, 1, 0, "ArrayShorthand"},
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    {"equivalent", {nullptr, nullptr}, 1, 0, "TypeDescrEquivalent"},
    JS_FS_END
};

const JSPropertySpec StructMetaTypeDescr::typedObjectProperties[] = {
    JS_PS_END
};

const JSFunctionSpec StructMetaTypeDescr::typedObjectMethods[] = {
    JS_FS_END
};

JSObject*
StructMetaTypeDescr::create(JSContext* cx,
                            HandleObject metaTypeDescr,
                            HandleObject fields)
{
    // Obtain names of fields, which are the own properties of `fields`
    AutoIdVector ids(cx);
    if (!GetPropertyKeys(cx, fields, JSITER_OWNONLY | JSITER_SYMBOLS, &ids))
        return nullptr;

    // Iterate through each field. Collect values for the various
    // vectors below and also track total size and alignment. Be wary
    // of overflow!
    StringBuffer stringBuffer(cx);     // Canonical string repr
    AutoValueVector fieldNames(cx);    // Name of each field.
    AutoValueVector fieldTypeObjs(cx); // Type descriptor of each field.
    AutoValueVector fieldOffsets(cx);  // Offset of each field field.
    RootedObject userFieldOffsets(cx); // User-exposed {f:offset} object
    RootedObject userFieldTypes(cx);   // User-exposed {f:descr} object.
    CheckedInt32 sizeSoFar(0);         // Size of struct thus far.
    uint32_t alignment = 1;            // Alignment of struct.
    bool opaque = false;               // Opacity of struct.

    userFieldOffsets = NewBuiltinClassInstance<PlainObject>(cx, TenuredObject);
    if (!userFieldOffsets)
        return nullptr;

    userFieldTypes = NewBuiltinClassInstance<PlainObject>(cx, TenuredObject);
    if (!userFieldTypes)
        return nullptr;

    if (!stringBuffer.append("new StructType({"))
        return nullptr;

    RootedValue fieldTypeVal(cx);
    RootedId id(cx);
    Rooted<TypeDescr*> fieldType(cx);
    for (unsigned int i = 0; i < ids.length(); i++) {
        id = ids[i];

        // Check that all the property names are non-numeric strings.
        uint32_t unused;
        if (!JSID_IS_ATOM(id) || JSID_TO_ATOM(id)->isIndex(&unused)) {
            RootedValue idValue(cx, IdToValue(id));
            ReportCannotConvertTo(cx, idValue, "StructType field name");
            return nullptr;
        }

        // Load the value for the current field from the `fields` object.
        // The value should be a type descriptor.
        if (!GetProperty(cx, fields, fields, id, &fieldTypeVal))
            return nullptr;
        fieldType = ToObjectIf<TypeDescr>(fieldTypeVal);
        if (!fieldType) {
            ReportCannotConvertTo(cx, fieldTypeVal, "StructType field specifier");
            return nullptr;
        }

        // Collect field name and type object
        RootedValue fieldName(cx, IdToValue(id));
        if (!fieldNames.append(fieldName))
            return nullptr;
        if (!fieldTypeObjs.append(ObjectValue(*fieldType)))
            return nullptr;

        // userFieldTypes[id] = typeObj
        if (!DefineDataProperty(cx, userFieldTypes, id, fieldTypeObjs[i],
                                JSPROP_READONLY | JSPROP_PERMANENT))
        {
            return nullptr;
        }

        // Append "f:Type" to the string repr
        if (i > 0 && !stringBuffer.append(", "))
            return nullptr;
        if (!stringBuffer.append(JSID_TO_ATOM(id)))
            return nullptr;
        if (!stringBuffer.append(": "))
            return nullptr;
        if (!stringBuffer.append(&fieldType->stringRepr()))
            return nullptr;

        // Offset of this field is the current total size adjusted for
        // the field's alignment.
        CheckedInt32 offset = RoundUpToAlignment(sizeSoFar, fieldType->alignment());
        if (!offset.isValid()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_TOO_BIG);
            return nullptr;
        }
        MOZ_ASSERT(offset.value() >= 0);
        if (!fieldOffsets.append(Int32Value(offset.value())))
            return nullptr;

        // userFieldOffsets[id] = offset
        RootedValue offsetValue(cx, Int32Value(offset.value()));
        if (!DefineDataProperty(cx, userFieldOffsets, id, offsetValue,
                                JSPROP_READONLY | JSPROP_PERMANENT))
        {
            return nullptr;
        }

        // Add space for this field to the total struct size.
        sizeSoFar = offset + fieldType->size();
        if (!sizeSoFar.isValid()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_TOO_BIG);
            return nullptr;
        }

        // Struct is opaque if any field is opaque
        if (fieldType->opaque())
            opaque = true;

        // Alignment of the struct is the max of the alignment of its fields.
        alignment = js::Max(alignment, fieldType->alignment());
    }

    // Complete string representation.
    if (!stringBuffer.append("})"))
        return nullptr;

    RootedAtom stringRepr(cx, stringBuffer.finishAtom());
    if (!stringRepr)
        return nullptr;

    // Adjust the total size to be a multiple of the final alignment.
    CheckedInt32 totalSize = RoundUpToAlignment(sizeSoFar, alignment);
    if (!totalSize.isValid()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_TOO_BIG);
        return nullptr;
    }

    // Now create the resulting type descriptor.
    RootedObject structTypePrototype(cx, GetPrototype(cx, metaTypeDescr));
    if (!structTypePrototype)
        return nullptr;

    Rooted<StructTypeDescr*> descr(cx);
    descr = NewObjectWithGivenProto<StructTypeDescr>(cx, structTypePrototype, SingletonObject);
    if (!descr)
        return nullptr;

    descr->initReservedSlot(JS_DESCR_SLOT_KIND, Int32Value(type::Struct));
    descr->initReservedSlot(JS_DESCR_SLOT_STRING_REPR, StringValue(stringRepr));
    descr->initReservedSlot(JS_DESCR_SLOT_ALIGNMENT, Int32Value(AssertedCast<int32_t>(alignment)));
    descr->initReservedSlot(JS_DESCR_SLOT_SIZE, Int32Value(totalSize.value()));
    descr->initReservedSlot(JS_DESCR_SLOT_OPAQUE, BooleanValue(opaque));

    // Construct for internal use an array with the name for each field.
    {
        RootedObject fieldNamesVec(cx);
        fieldNamesVec = NewDenseCopiedArray(cx, fieldNames.length(),
                                            fieldNames.begin(), nullptr,
                                            TenuredObject);
        if (!fieldNamesVec)
            return nullptr;
        descr->initReservedSlot(JS_DESCR_SLOT_STRUCT_FIELD_NAMES, ObjectValue(*fieldNamesVec));
    }

    // Construct for internal use an array with the type object for each field.
    RootedObject fieldTypeVec(cx);
    fieldTypeVec = NewDenseCopiedArray(cx, fieldTypeObjs.length(),
                                       fieldTypeObjs.begin(), nullptr,
                                       TenuredObject);
    if (!fieldTypeVec)
        return nullptr;
    descr->initReservedSlot(JS_DESCR_SLOT_STRUCT_FIELD_TYPES, ObjectValue(*fieldTypeVec));

    // Construct for internal use an array with the offset for each field.
    {
        RootedObject fieldOffsetsVec(cx);
        fieldOffsetsVec = NewDenseCopiedArray(cx, fieldOffsets.length(),
                                              fieldOffsets.begin(), nullptr,
                                              TenuredObject);
        if (!fieldOffsetsVec)
            return nullptr;
        descr->initReservedSlot(JS_DESCR_SLOT_STRUCT_FIELD_OFFSETS, ObjectValue(*fieldOffsetsVec));
    }

    // Create data properties fieldOffsets and fieldTypes
    if (!FreezeObject(cx, userFieldOffsets))
        return nullptr;
    if (!FreezeObject(cx, userFieldTypes))
        return nullptr;
    RootedValue userFieldOffsetsValue(cx, ObjectValue(*userFieldOffsets));
    if (!DefineDataProperty(cx, descr, cx->names().fieldOffsets, userFieldOffsetsValue,
                            JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return nullptr;
    }
    RootedValue userFieldTypesValue(cx, ObjectValue(*userFieldTypes));
    if (!DefineDataProperty(cx, descr, cx->names().fieldTypes, userFieldTypesValue,
                            JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return nullptr;
    }

    if (!CreateUserSizeAndAlignmentProperties(cx, descr))
        return nullptr;

    Rooted<TypedProto*> prototypeObj(cx);
    prototypeObj = CreatePrototypeObjectForComplexTypeInstance(cx, structTypePrototype);
    if (!prototypeObj)
        return nullptr;

    descr->initReservedSlot(JS_DESCR_SLOT_TYPROTO, ObjectValue(*prototypeObj));

    if (!LinkConstructorAndPrototype(cx, descr, prototypeObj))
        return nullptr;

    if (!CreateTraceList(cx, descr))
        return nullptr;

    if (!cx->zone()->addTypeDescrObject(cx, descr)) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    return descr;
}

bool
StructMetaTypeDescr::construct(JSContext* cx, unsigned int argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "StructType"))
        return false;

    if (args.length() >= 1 && args[0].isObject()) {
        RootedObject metaTypeDescr(cx, &args.callee());
        RootedObject fields(cx, &args[0].toObject());
        RootedObject obj(cx, create(cx, metaTypeDescr, fields));
        if (!obj)
            return false;
        args.rval().setObject(*obj);
        return true;
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_STRUCTTYPE_BAD_ARGS);
    return false;
}

size_t
StructTypeDescr::fieldCount() const
{
    return fieldInfoObject(JS_DESCR_SLOT_STRUCT_FIELD_NAMES).getDenseInitializedLength();
}

bool
StructTypeDescr::fieldIndex(jsid id, size_t* out) const
{
    ArrayObject& fieldNames = fieldInfoObject(JS_DESCR_SLOT_STRUCT_FIELD_NAMES);
    size_t l = fieldNames.getDenseInitializedLength();
    for (size_t i = 0; i < l; i++) {
        JSAtom& a = fieldNames.getDenseElement(i).toString()->asAtom();
        if (JSID_IS_ATOM(id, &a)) {
            *out = i;
            return true;
        }
    }
    return false;
}

JSAtom&
StructTypeDescr::fieldName(size_t index) const
{
    return fieldInfoObject(JS_DESCR_SLOT_STRUCT_FIELD_NAMES).getDenseElement(index).toString()->asAtom();
}

size_t
StructTypeDescr::fieldOffset(size_t index) const
{
    ArrayObject& fieldOffsets = fieldInfoObject(JS_DESCR_SLOT_STRUCT_FIELD_OFFSETS);
    MOZ_ASSERT(index < fieldOffsets.getDenseInitializedLength());
    return AssertedCast<size_t>(fieldOffsets.getDenseElement(index).toInt32());
}

TypeDescr&
StructTypeDescr::fieldDescr(size_t index) const
{
    ArrayObject& fieldDescrs = fieldInfoObject(JS_DESCR_SLOT_STRUCT_FIELD_TYPES);
    MOZ_ASSERT(index < fieldDescrs.getDenseInitializedLength());
    return fieldDescrs.getDenseElement(index).toObject().as<TypeDescr>();
}

/******************************************************************************
 * Creating the TypedObject "module"
 *
 * We create one global, `TypedObject`, which contains the following
 * members:
 *
 * 1. uint8, uint16, etc
 * 2. ArrayType
 * 3. StructType
 *
 * Each of these is a function and hence their prototype is
 * `Function.__proto__` (in terms of the JS Engine, they are not
 * JSFunctions but rather instances of their own respective JSClasses
 * which override the call and construct operations).
 *
 * Each type object also has its own `prototype` field. Therefore,
 * using `StructType` as an example, the basic setup is:
 *
 *   StructType --__proto__--> Function.__proto__
 *        |
 *    prototype -- prototype --> { }
 *        |
 *        v
 *       { } -----__proto__--> Function.__proto__
 *
 * When a new type object (e.g., an instance of StructType) is created,
 * it will look as follows:
 *
 *   MyStruct -__proto__-> StructType.prototype -__proto__-> Function.__proto__
 *        |                          |
 *        |                     prototype
 *        |                          |
 *        |                          v
 *    prototype -----__proto__----> { }
 *        |
 *        v
 *       { } --__proto__-> Object.prototype
 *
 * Finally, when an instance of `MyStruct` is created, its
 * structure is as follows:
 *
 *    object -__proto__->
 *      MyStruct.prototype -__proto__->
 *        StructType.prototype.prototype -__proto__->
 *          Object.prototype
 */

// Here `T` is either `ScalarTypeDescr` or `ReferenceTypeDescr`
template<typename T>
static bool
DefineSimpleTypeDescr(JSContext* cx,
                      Handle<GlobalObject*> global,
                      HandleObject module,
                      typename T::Type type,
                      HandlePropertyName className)
{
    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!objProto)
        return false;

    RootedObject funcProto(cx, GlobalObject::getOrCreateFunctionPrototype(cx, global));
    if (!funcProto)
        return false;

    Rooted<T*> descr(cx);
    descr = NewObjectWithGivenProto<T>(cx, funcProto, SingletonObject);
    if (!descr)
        return false;

    descr->initReservedSlot(JS_DESCR_SLOT_KIND, Int32Value(T::Kind));
    descr->initReservedSlot(JS_DESCR_SLOT_STRING_REPR, StringValue(className));
    descr->initReservedSlot(JS_DESCR_SLOT_ALIGNMENT, Int32Value(T::alignment(type)));
    descr->initReservedSlot(JS_DESCR_SLOT_SIZE, Int32Value(AssertedCast<int32_t>(T::size(type))));
    descr->initReservedSlot(JS_DESCR_SLOT_OPAQUE, BooleanValue(T::Opaque));
    descr->initReservedSlot(JS_DESCR_SLOT_TYPE, Int32Value(type));

    if (!CreateUserSizeAndAlignmentProperties(cx, descr))
        return false;

    if (!JS_DefineFunctions(cx, descr, T::typeObjectMethods))
        return false;

    // Create the typed prototype for the scalar type. This winds up
    // not being user accessible, but we still create one for consistency.
    Rooted<TypedProto*> proto(cx);
    proto = NewObjectWithGivenProto<TypedProto>(cx, objProto, TenuredObject);
    if (!proto)
        return false;
    descr->initReservedSlot(JS_DESCR_SLOT_TYPROTO, ObjectValue(*proto));

    RootedValue descrValue(cx, ObjectValue(*descr));
    if (!DefineDataProperty(cx, module, className, descrValue, 0))
        return false;

    if (!CreateTraceList(cx, descr))
        return false;

    if (!cx->zone()->addTypeDescrObject(cx, descr))
        return false;

    return true;
}

///////////////////////////////////////////////////////////////////////////

template<typename T>
static JSObject*
DefineMetaTypeDescr(JSContext* cx,
                    const char* name,
                    Handle<GlobalObject*> global,
                    Handle<TypedObjectModuleObject*> module,
                    TypedObjectModuleObject::Slot protoSlot)
{
    RootedAtom className(cx, Atomize(cx, name, strlen(name)));
    if (!className)
        return nullptr;

    RootedObject funcProto(cx, GlobalObject::getOrCreateFunctionPrototype(cx, global));
    if (!funcProto)
        return nullptr;

    // Create ctor.prototype, which inherits from Function.__proto__

    RootedObject proto(cx, NewObjectWithGivenProto<PlainObject>(cx, funcProto, SingletonObject));
    if (!proto)
        return nullptr;

    // Create ctor.prototype.prototype, which inherits from Object.__proto__

    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!objProto)
        return nullptr;
    RootedObject protoProto(cx);
    protoProto = NewObjectWithGivenProto<PlainObject>(cx, objProto, SingletonObject);
    if (!protoProto)
        return nullptr;

    RootedValue protoProtoValue(cx, ObjectValue(*protoProto));
    if (!DefineDataProperty(cx, proto, cx->names().prototype, protoProtoValue,
                            JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return nullptr;
    }

    // Create ctor itself

    const int constructorLength = 2;
    RootedFunction ctor(cx);
    ctor = GlobalObject::createConstructor(cx, T::construct, className, constructorLength);
    if (!ctor ||
        !LinkConstructorAndPrototype(cx, ctor, proto) ||
        !DefinePropertiesAndFunctions(cx, proto,
                                      T::typeObjectProperties,
                                      T::typeObjectMethods) ||
        !DefinePropertiesAndFunctions(cx, protoProto,
                                      T::typedObjectProperties,
                                      T::typedObjectMethods))
    {
        return nullptr;
    }

    module->initReservedSlot(protoSlot, ObjectValue(*proto));

    return ctor;
}

/*  The initialization strategy for TypedObjects is mildly unusual
 * compared to other classes. Because all of the types are members
 * of a single global, `TypedObject`, we basically make the
 * initializer for the `TypedObject` class populate the
 * `TypedObject` global (which is referred to as "module" herein).
 */
/* static */ bool
GlobalObject::initTypedObjectModule(JSContext* cx, Handle<GlobalObject*> global)
{
    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!objProto)
        return false;

    Rooted<TypedObjectModuleObject*> module(cx);
    module = NewObjectWithGivenProto<TypedObjectModuleObject>(cx, objProto, SingletonObject);
    if (!module)
        return false;

    if (!JS_DefineFunctions(cx, module, TypedObjectMethods))
        return false;

    // uint8, uint16, any, etc

#define BINARYDATA_SCALAR_DEFINE(constant_, type_, name_)                       \
    if (!DefineSimpleTypeDescr<ScalarTypeDescr>(cx, global, module, constant_,      \
                                            cx->names().name_))                 \
        return false;
    JS_FOR_EACH_SCALAR_TYPE_REPR(BINARYDATA_SCALAR_DEFINE)
#undef BINARYDATA_SCALAR_DEFINE

#define BINARYDATA_REFERENCE_DEFINE(constant_, type_, name_)                    \
    if (!DefineSimpleTypeDescr<ReferenceTypeDescr>(cx, global, module, constant_,   \
                                               cx->names().name_))              \
        return false;
    JS_FOR_EACH_REFERENCE_TYPE_REPR(BINARYDATA_REFERENCE_DEFINE)
#undef BINARYDATA_REFERENCE_DEFINE

    // ArrayType.

    RootedObject arrayType(cx);
    arrayType = DefineMetaTypeDescr<ArrayMetaTypeDescr>(
        cx, "ArrayType", global, module, TypedObjectModuleObject::ArrayTypePrototype);
    if (!arrayType)
        return false;

    RootedValue arrayTypeValue(cx, ObjectValue(*arrayType));
    if (!DefineDataProperty(cx, module, cx->names().ArrayType, arrayTypeValue,
                            JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return false;
    }

    // StructType.

    RootedObject structType(cx);
    structType = DefineMetaTypeDescr<StructMetaTypeDescr>(
        cx, "StructType", global, module, TypedObjectModuleObject::StructTypePrototype);
    if (!structType)
        return false;

    RootedValue structTypeValue(cx, ObjectValue(*structType));
    if (!DefineDataProperty(cx, module, cx->names().StructType, structTypeValue,
                            JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return false;
    }

    // Everything is setup, install module on the global object:
    RootedValue moduleValue(cx, ObjectValue(*module));
    if (!DefineDataProperty(cx, global, cx->names().TypedObject, moduleValue,
                            JSPROP_RESOLVING))
    {
        return false;
    }

    global->setConstructor(JSProto_TypedObject, moduleValue);

    return module;
}

JSObject*
js::InitTypedObjectModuleObject(JSContext* cx, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    return GlobalObject::getOrCreateTypedObjectModule(cx, global);
}

/******************************************************************************
 * Typed objects
 */

uint32_t
TypedObject::offset() const
{
    if (is<InlineTypedObject>())
        return 0;
    return PointerRangeSize(typedMemBase(), typedMem());
}

uint32_t
TypedObject::length() const
{
    return typeDescr().as<ArrayTypeDescr>().length();
}

uint8_t*
TypedObject::typedMem() const
{
    MOZ_ASSERT(isAttached());

    if (is<InlineTypedObject>())
        return as<InlineTypedObject>().inlineTypedMem();
    return as<OutlineTypedObject>().outOfLineTypedMem();
}

uint8_t*
TypedObject::typedMemBase() const
{
    MOZ_ASSERT(isAttached());
    MOZ_ASSERT(is<OutlineTypedObject>());

    JSObject& owner = as<OutlineTypedObject>().owner();
    if (owner.is<ArrayBufferObject>())
        return owner.as<ArrayBufferObject>().dataPointer();
    return owner.as<InlineTypedObject>().inlineTypedMem();
}

bool
TypedObject::isAttached() const
{
    if (is<InlineTransparentTypedObject>()) {
        ObjectWeakMap* table = compartment()->lazyArrayBuffers;
        if (table) {
            JSObject* buffer = table->lookup(this);
            if (buffer)
                return !buffer->as<ArrayBufferObject>().isDetached();
        }
        return true;
    }
    if (is<InlineOpaqueTypedObject>())
        return true;
    if (!as<OutlineTypedObject>().outOfLineTypedMem())
        return false;
    JSObject& owner = as<OutlineTypedObject>().owner();
    if (owner.is<ArrayBufferObject>() && owner.as<ArrayBufferObject>().isDetached())
        return false;
    return true;
}

/* static */ bool
TypedObject::GetBuffer(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JSObject& obj = args[0].toObject();
    ArrayBufferObject* buffer;
    if (obj.is<OutlineTransparentTypedObject>())
        buffer = obj.as<OutlineTransparentTypedObject>().getOrCreateBuffer(cx);
    else
        buffer = obj.as<InlineTransparentTypedObject>().getOrCreateBuffer(cx);
    if (!buffer)
        return false;
    args.rval().setObject(*buffer);
    return true;
}

/* static */ bool
TypedObject::GetByteOffset(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setInt32(AssertedCast<int32_t>(args[0].toObject().as<TypedObject>().offset()));
    return true;
}

/******************************************************************************
 * Outline typed objects
 */

/*static*/ OutlineTypedObject*
OutlineTypedObject::createUnattached(JSContext* cx,
                                     HandleTypeDescr descr,
                                     int32_t length,
                                     gc::InitialHeap heap)
{
    if (descr->opaque())
        return createUnattachedWithClass(cx, &OutlineOpaqueTypedObject::class_, descr, length, heap);
    else
        return createUnattachedWithClass(cx, &OutlineTransparentTypedObject::class_, descr, length, heap);
}

void
OutlineTypedObject::setOwnerAndData(JSObject* owner, uint8_t* data)
{
    // Make sure we don't associate with array buffers whose data is from an
    // inline typed object, see obj_trace.
    MOZ_ASSERT_IF(owner && owner->is<ArrayBufferObject>(),
                  !owner->as<ArrayBufferObject>().forInlineTypedObject());

    // Typed objects cannot move from one owner to another, so don't worry
    // about pre barriers during this initialization.
    owner_ = owner;
    data_ = data;

    // Trigger a post barrier when attaching an object outside the nursery to
    // one that is inside it.
    if (owner && !IsInsideNursery(this) && IsInsideNursery(owner))
        zone()->group()->storeBuffer().putWholeCell(this);
}

/*static*/ OutlineTypedObject*
OutlineTypedObject::createUnattachedWithClass(JSContext* cx,
                                              const Class* clasp,
                                              HandleTypeDescr descr,
                                              int32_t length,
                                              gc::InitialHeap heap)
{
    MOZ_ASSERT(clasp == &OutlineTransparentTypedObject::class_ ||
               clasp == &OutlineOpaqueTypedObject::class_);

    AutoSetNewObjectMetadata metadata(cx);

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, clasp,
                                                             TaggedProto(&descr->typedProto()),
                                                             descr));
    if (!group)
        return nullptr;

    NewObjectKind newKind = (heap == gc::TenuredHeap) ? TenuredObject : GenericObject;
    OutlineTypedObject* obj = NewObjectWithGroup<OutlineTypedObject>(cx, group,
                                                                     gc::AllocKind::OBJECT0,
                                                                     newKind);
    if (!obj)
        return nullptr;

    obj->setOwnerAndData(nullptr, nullptr);
    return obj;
}

void
OutlineTypedObject::attach(JSContext* cx, ArrayBufferObject& buffer, uint32_t offset)
{
    MOZ_ASSERT(!isAttached());
    MOZ_ASSERT(offset <= buffer.byteLength());
    MOZ_ASSERT(size() <= buffer.byteLength() - offset);

    // If the owner's data is from an inline typed object, associate this with
    // the inline typed object instead, to simplify tracing.
    if (buffer.forInlineTypedObject()) {
        InlineTypedObject& realOwner = buffer.firstView()->as<InlineTypedObject>();
        attach(cx, realOwner, offset);
        return;
    }

    buffer.setHasTypedObjectViews();

    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!buffer.addView(cx, this))
            oomUnsafe.crash("TypedObject::attach");
    }

    setOwnerAndData(&buffer, buffer.dataPointer() + offset);
}

void
OutlineTypedObject::attach(JSContext* cx, TypedObject& typedObj, uint32_t offset)
{
    MOZ_ASSERT(!isAttached());
    MOZ_ASSERT(typedObj.isAttached());

    JSObject* owner = &typedObj;
    if (typedObj.is<OutlineTypedObject>()) {
        owner = &typedObj.as<OutlineTypedObject>().owner();
        MOZ_ASSERT(typedObj.offset() <= UINT32_MAX - offset);
        offset += typedObj.offset();
    }

    if (owner->is<ArrayBufferObject>()) {
        attach(cx, owner->as<ArrayBufferObject>(), offset);
    } else {
        MOZ_ASSERT(owner->is<InlineTypedObject>());
        JS::AutoCheckCannotGC nogc(cx);
        setOwnerAndData(owner, owner->as<InlineTypedObject>().inlineTypedMem(nogc) + offset);
    }
}

// Returns a suitable JS_TYPEDOBJ_SLOT_LENGTH value for an instance of
// the type `type`.
static uint32_t
TypedObjLengthFromType(TypeDescr& descr)
{
    switch (descr.kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Struct:
      case type::Simd:
        return 0;

      case type::Array:
        return descr.as<ArrayTypeDescr>().length();
    }
    MOZ_CRASH("Invalid kind");
}

/*static*/ OutlineTypedObject*
OutlineTypedObject::createDerived(JSContext* cx, HandleTypeDescr type,
                                  HandleTypedObject typedObj, uint32_t offset)
{
    MOZ_ASSERT(offset <= typedObj->size());
    MOZ_ASSERT(offset + type->size() <= typedObj->size());

    int32_t length = TypedObjLengthFromType(*type);

    const js::Class* clasp = typedObj->opaque()
                             ? &OutlineOpaqueTypedObject::class_
                             : &OutlineTransparentTypedObject::class_;
    Rooted<OutlineTypedObject*> obj(cx);
    obj = createUnattachedWithClass(cx, clasp, type, length);
    if (!obj)
        return nullptr;

    obj->attach(cx, *typedObj, offset);
    return obj;
}

/*static*/ TypedObject*
TypedObject::createZeroed(JSContext* cx, HandleTypeDescr descr, int32_t length, gc::InitialHeap heap)
{
    // If possible, create an object with inline data.
    if (descr->size() <= InlineTypedObject::MaximumSize) {
        AutoSetNewObjectMetadata metadata(cx);

        InlineTypedObject* obj = InlineTypedObject::create(cx, descr, heap);
        if (!obj)
            return nullptr;
        JS::AutoCheckCannotGC nogc(cx);
        descr->initInstances(cx->runtime(), obj->inlineTypedMem(nogc), 1);
        return obj;
    }

    // Create unattached wrapper object.
    Rooted<OutlineTypedObject*> obj(cx, OutlineTypedObject::createUnattached(cx, descr, length, heap));
    if (!obj)
        return nullptr;

    // Allocate and initialize the memory for this instance.
    size_t totalSize = descr->size();
    Rooted<ArrayBufferObject*> buffer(cx);
    buffer = ArrayBufferObject::create(cx, totalSize);
    if (!buffer)
        return nullptr;
    descr->initInstances(cx->runtime(), buffer->dataPointer(), 1);
    obj->attach(cx, *buffer, 0);
    return obj;
}

static bool
ReportTypedObjTypeError(JSContext* cx,
                        const unsigned errorNumber,
                        HandleTypedObject obj)
{
    // Serialize type string of obj
    RootedAtom typeReprAtom(cx, &obj->typeDescr().stringRepr());
    UniqueChars typeReprStr(JS_EncodeStringToUTF8(cx, typeReprAtom));
    if (!typeReprStr)
        return false;

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber, typeReprStr.get());
    return false;
}

/* static */ void
OutlineTypedObject::obj_trace(JSTracer* trc, JSObject* object)
{
    OutlineTypedObject& typedObj = object->as<OutlineTypedObject>();

    TraceEdge(trc, typedObj.shapePtr(), "OutlineTypedObject_shape");

    if (!typedObj.owner_)
        return;

    TypeDescr& descr = typedObj.typeDescr();

    // Mark the owner, watching in case it is moved by the tracer.
    JSObject* oldOwner = typedObj.owner_;
    TraceManuallyBarrieredEdge(trc, &typedObj.owner_, "typed object owner");
    JSObject* owner = typedObj.owner_;

    uint8_t* oldData = typedObj.outOfLineTypedMem();
    uint8_t* newData = oldData;

    // Update the data pointer if the owner moved and the owner's data is
    // inline with it. Note that an array buffer pointing to data in an inline
    // typed object will never be used as an owner for another outline typed
    // object. In such cases, the owner will be the inline typed object itself.
    MakeAccessibleAfterMovingGC(owner);
    MOZ_ASSERT_IF(owner->is<ArrayBufferObject>(),
                  !owner->as<ArrayBufferObject>().forInlineTypedObject());
    if (owner != oldOwner &&
        (owner->is<InlineTypedObject>() ||
         owner->as<ArrayBufferObject>().hasInlineData()))
    {
        newData += reinterpret_cast<uint8_t*>(owner) - reinterpret_cast<uint8_t*>(oldOwner);
        typedObj.setData(newData);

        if (trc->isTenuringTracer()) {
            Nursery& nursery = typedObj.zoneFromAnyThread()->group()->nursery();
            nursery.maybeSetForwardingPointer(trc, oldData, newData, /* direct = */ false);
        }
    }

    if (!descr.opaque() || !typedObj.isAttached())
        return;

    descr.traceInstances(trc, newData, 1);
}

bool
TypeDescr::hasProperty(const JSAtomState& names, jsid id)
{
    switch (kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Simd:
        return false;

      case type::Array:
      {
        uint32_t index;
        return IdIsIndex(id, &index) || JSID_IS_ATOM(id, names.length);
      }

      case type::Struct:
      {
        size_t index;
        return as<StructTypeDescr>().fieldIndex(id, &index);
      }
    }

    MOZ_CRASH("Unexpected kind");
}

/* static */ bool
TypedObject::obj_lookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                                MutableHandleObject objp, MutableHandle<PropertyResult> propp)
{
    if (obj->as<TypedObject>().typeDescr().hasProperty(cx->names(), id)) {
        propp.setNonNativeProperty();
        objp.set(obj);
        return true;
    }

    RootedObject proto(cx, obj->staticPrototype());
    if (!proto) {
        objp.set(nullptr);
        propp.setNotFound();
        return true;
    }

    return LookupProperty(cx, proto, id, objp, propp);
}

static bool
ReportPropertyError(JSContext* cx,
                    const unsigned errorNumber,
                    HandleId id)
{
    RootedValue idVal(cx, IdToValue(id));
    RootedString str(cx, ValueToSource(cx, idVal));
    if (!str)
        return false;

    UniqueChars propName(JS_EncodeStringToUTF8(cx, str));
    if (!propName)
        return false;

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber, propName.get());
    return false;
}

bool
TypedObject::obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result)
{
    Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());
    return ReportTypedObjTypeError(cx, JSMSG_OBJECT_NOT_EXTENSIBLE, typedObj);
}

bool
TypedObject::obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp)
{
    Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());
    switch (typedObj->typeDescr().kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Simd:
        break;

      case type::Array: {
        if (JSID_IS_ATOM(id, cx->names().length)) {
            *foundp = true;
            return true;
        }
        uint32_t index;
        // Elements are not inherited from the prototype.
        if (IdIsIndex(id, &index)) {
            *foundp = (index < uint32_t(typedObj->length()));
            return true;
        }
        break;
      }

      case type::Struct:
        size_t index;
        if (typedObj->typeDescr().as<StructTypeDescr>().fieldIndex(id, &index)) {
            *foundp = true;
            return true;
        }
    }

    RootedObject proto(cx, obj->staticPrototype());
    if (!proto) {
        *foundp = false;
        return true;
    }

    return HasProperty(cx, proto, id, foundp);
}

bool
TypedObject::obj_getProperty(JSContext* cx, HandleObject obj, HandleValue receiver,
                             HandleId id, MutableHandleValue vp)
{
    Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());

    // Dispatch elements to obj_getElement:
    uint32_t index;
    if (IdIsIndex(id, &index))
        return obj_getElement(cx, obj, receiver, index, vp);

    // Handle everything else here:

    switch (typedObj->typeDescr().kind()) {
      case type::Scalar:
      case type::Reference:
        break;

      case type::Simd:
        break;

      case type::Array:
        if (JSID_IS_ATOM(id, cx->names().length)) {
            if (!typedObj->isAttached()) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_TYPEDOBJECT_HANDLE_UNATTACHED);
                return false;
            }

            vp.setInt32(typedObj->length());
            return true;
        }
        break;

      case type::Struct: {
        Rooted<StructTypeDescr*> descr(cx, &typedObj->typeDescr().as<StructTypeDescr>());

        size_t fieldIndex;
        if (!descr->fieldIndex(id, &fieldIndex))
            break;

        size_t offset = descr->fieldOffset(fieldIndex);
        Rooted<TypeDescr*> fieldType(cx, &descr->fieldDescr(fieldIndex));
        return Reify(cx, fieldType, typedObj, offset, vp);
      }
    }

    RootedObject proto(cx, obj->staticPrototype());
    if (!proto) {
        vp.setUndefined();
        return true;
    }

    return GetProperty(cx, proto, receiver, id, vp);
}

bool
TypedObject::obj_getElement(JSContext* cx, HandleObject obj, HandleValue receiver,
                            uint32_t index, MutableHandleValue vp)
{
    MOZ_ASSERT(obj->is<TypedObject>());
    Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());
    Rooted<TypeDescr*> descr(cx, &typedObj->typeDescr());

    switch (descr->kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Simd:
      case type::Struct:
        break;

      case type::Array:
        return obj_getArrayElement(cx, typedObj, descr, index, vp);
    }

    RootedObject proto(cx, obj->staticPrototype());
    if (!proto) {
        vp.setUndefined();
        return true;
    }

    return GetElement(cx, proto, receiver, index, vp);
}

/*static*/ bool
TypedObject::obj_getArrayElement(JSContext* cx,
                                 Handle<TypedObject*> typedObj,
                                 Handle<TypeDescr*> typeDescr,
                                 uint32_t index,
                                 MutableHandleValue vp)
{
    // Elements are not inherited from the prototype.
    if (index >= (size_t) typedObj->length()) {
        vp.setUndefined();
        return true;
    }

    Rooted<TypeDescr*> elementType(cx, &typeDescr->as<ArrayTypeDescr>().elementType());
    size_t offset = elementType->size() * index;
    return Reify(cx, elementType, typedObj, offset, vp);
}

bool
TypedObject::obj_setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                             HandleValue receiver, ObjectOpResult& result)
{
    Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());

    switch (typedObj->typeDescr().kind()) {
      case type::Scalar:
      case type::Reference:
        break;

      case type::Simd:
        break;

      case type::Array: {
        if (JSID_IS_ATOM(id, cx->names().length)) {
            if (receiver.isObject() && obj == &receiver.toObject()) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_CANT_REDEFINE_ARRAY_LENGTH);
                return false;
            }
            return result.failReadOnly();
        }

        uint32_t index;
        if (IdIsIndex(id, &index)) {
            if (!receiver.isObject() || obj != &receiver.toObject())
                return SetPropertyByDefining(cx, id, v, receiver, result);

            if (index >= uint32_t(typedObj->length())) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                          JSMSG_TYPEDOBJECT_BINARYARRAY_BAD_INDEX);
                return false;
            }

            Rooted<TypeDescr*> elementType(cx);
            elementType = &typedObj->typeDescr().as<ArrayTypeDescr>().elementType();
            size_t offset = elementType->size() * index;
            if (!ConvertAndCopyTo(cx, elementType, typedObj, offset, nullptr, v))
                return false;
            return result.succeed();
        }
        break;
      }

      case type::Struct: {
        Rooted<StructTypeDescr*> descr(cx, &typedObj->typeDescr().as<StructTypeDescr>());

        size_t fieldIndex;
        if (!descr->fieldIndex(id, &fieldIndex))
            break;

        if (!receiver.isObject() || obj != &receiver.toObject())
            return SetPropertyByDefining(cx, id, v, receiver, result);

        size_t offset = descr->fieldOffset(fieldIndex);
        Rooted<TypeDescr*> fieldType(cx, &descr->fieldDescr(fieldIndex));
        RootedAtom fieldName(cx, &descr->fieldName(fieldIndex));
        if (!ConvertAndCopyTo(cx, fieldType, typedObj, offset, fieldName, v))
            return false;
        return result.succeed();
      }
    }

    return SetPropertyOnProto(cx, obj, id, v, receiver, result);
}

bool
TypedObject::obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc)
{
    Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());
    if (!typedObj->isAttached()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_HANDLE_UNATTACHED);
        return false;
    }

    Rooted<TypeDescr*> descr(cx, &typedObj->typeDescr());
    switch (descr->kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Simd:
        break;

      case type::Array:
      {
        uint32_t index;
        if (IdIsIndex(id, &index)) {
            if (!obj_getArrayElement(cx, typedObj, descr, index, desc.value()))
                return false;
            desc.setAttributes(JSPROP_ENUMERATE | JSPROP_PERMANENT);
            desc.object().set(obj);
            return true;
        }

        if (JSID_IS_ATOM(id, cx->names().length)) {
            desc.value().setInt32(typedObj->length());
            desc.setAttributes(JSPROP_READONLY | JSPROP_PERMANENT);
            desc.object().set(obj);
            return true;
        }
        break;
      }

      case type::Struct:
      {
        Rooted<StructTypeDescr*> descr(cx, &typedObj->typeDescr().as<StructTypeDescr>());

        size_t fieldIndex;
        if (!descr->fieldIndex(id, &fieldIndex))
            break;

        size_t offset = descr->fieldOffset(fieldIndex);
        Rooted<TypeDescr*> fieldType(cx, &descr->fieldDescr(fieldIndex));
        if (!Reify(cx, fieldType, typedObj, offset, desc.value()))
            return false;

        desc.setAttributes(JSPROP_ENUMERATE | JSPROP_PERMANENT);
        desc.object().set(obj);
        return true;
      }
    }

    desc.object().set(nullptr);
    return true;
}

static bool
IsOwnId(JSContext* cx, HandleObject obj, HandleId id)
{
    uint32_t index;
    Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());
    switch (typedObj->typeDescr().kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Simd:
        return false;

      case type::Array:
        return IdIsIndex(id, &index) || JSID_IS_ATOM(id, cx->names().length);

      case type::Struct:
        size_t index;
        if (typedObj->typeDescr().as<StructTypeDescr>().fieldIndex(id, &index))
            return true;
    }

    return false;
}

bool
TypedObject::obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id, ObjectOpResult& result)
{
    if (IsOwnId(cx, obj, id))
        return ReportPropertyError(cx, JSMSG_CANT_DELETE, id);

    RootedObject proto(cx, obj->staticPrototype());
    if (!proto)
        return result.succeed();

    return DeleteProperty(cx, proto, id, result);
}

bool
TypedObject::obj_newEnumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                              bool enumerableOnly)
{
    MOZ_ASSERT(obj->is<TypedObject>());
    Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());
    Rooted<TypeDescr*> descr(cx, &typedObj->typeDescr());

    RootedId id(cx);
    switch (descr->kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Simd: {
        // Nothing to enumerate.
        break;
      }

      case type::Array: {
        if (!properties.reserve(typedObj->length()))
            return false;

        for (uint32_t index = 0; index < typedObj->length(); index++) {
            id = INT_TO_JSID(index);
            properties.infallibleAppend(id);
        }
        break;
      }

      case type::Struct: {
        size_t fieldCount = descr->as<StructTypeDescr>().fieldCount();
        if (!properties.reserve(fieldCount))
            return false;

        for (size_t index = 0; index < fieldCount; index++) {
            id = AtomToId(&descr->as<StructTypeDescr>().fieldName(index));
            properties.infallibleAppend(id);
        }
        break;
      }
    }

    return true;
}

void
OutlineTypedObject::notifyBufferDetached(void* newData)
{
    setData(reinterpret_cast<uint8_t*>(newData));
}

/******************************************************************************
 * Inline typed objects
 */

/* static */ InlineTypedObject*
InlineTypedObject::create(JSContext* cx, HandleTypeDescr descr, gc::InitialHeap heap)
{
    gc::AllocKind allocKind = allocKindForTypeDescriptor(descr);

    const Class* clasp = descr->opaque()
                         ? &InlineOpaqueTypedObject::class_
                         : &InlineTransparentTypedObject::class_;

    RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(cx, clasp,
                                                             TaggedProto(&descr->typedProto()),
                                                             descr));
    if (!group)
        return nullptr;

    NewObjectKind newKind = (heap == gc::TenuredHeap) ? TenuredObject : GenericObject;
    return NewObjectWithGroup<InlineTypedObject>(cx, group, allocKind, newKind);
}

/* static */ InlineTypedObject*
InlineTypedObject::createCopy(JSContext* cx, Handle<InlineTypedObject*> templateObject,
                              gc::InitialHeap heap)
{
    AutoSetNewObjectMetadata metadata(cx);

    Rooted<TypeDescr*> descr(cx, &templateObject->typeDescr());
    InlineTypedObject* res = create(cx, descr, heap);
    if (!res)
        return nullptr;

    memcpy(res->inlineTypedMem(), templateObject->inlineTypedMem(), templateObject->size());
    return res;
}

/* static */ void
InlineTypedObject::obj_trace(JSTracer* trc, JSObject* object)
{
    InlineTypedObject& typedObj = object->as<InlineTypedObject>();

    TraceEdge(trc, typedObj.shapePtr(), "InlineTypedObject_shape");

    // Inline transparent objects do not have references and do not need more
    // tracing. If there is an entry in the compartment's LazyArrayBufferTable,
    // tracing that reference will be taken care of by the table itself.
    if (typedObj.is<InlineTransparentTypedObject>())
        return;

    typedObj.typeDescr().traceInstances(trc, typedObj.inlineTypedMem(), 1);
}

/* static */ size_t
InlineTypedObject::obj_moved(JSObject* dst, JSObject* src)
{
    if (!IsInsideNursery(src))
        return 0;

    // Inline typed object element arrays can be preserved on the stack by Ion
    // and need forwarding pointers created during a minor GC. We can't do this
    // in the trace hook because we don't have any stale data to determine
    // whether this object moved and where it was moved from.
    TypeDescr& descr = dst->as<InlineTypedObject>().typeDescr();
    if (descr.kind() == type::Array) {
        // The forwarding pointer can be direct as long as there is enough
        // space for it. Other objects might point into the object's buffer,
        // but they will not set any direct forwarding pointers.
        uint8_t* oldData = reinterpret_cast<uint8_t*>(src) + offsetOfDataStart();
        uint8_t* newData = dst->as<InlineTypedObject>().inlineTypedMem();
        auto& nursery = dst->zone()->group()->nursery();
        bool direct = descr.size() >= sizeof(uintptr_t);
        nursery.setForwardingPointerWhileTenuring(oldData, newData, direct);
    }

    return 0;
}

ArrayBufferObject*
InlineTransparentTypedObject::getOrCreateBuffer(JSContext* cx)
{
    ObjectWeakMap*& table = cx->compartment()->lazyArrayBuffers;
    if (!table) {
        table = cx->new_<ObjectWeakMap>(cx);
        if (!table || !table->init())
            return nullptr;
    }

    JSObject* obj = table->lookup(this);
    if (obj)
        return &obj->as<ArrayBufferObject>();

    ArrayBufferObject::BufferContents contents =
        ArrayBufferObject::BufferContents::createPlain(inlineTypedMem());
    size_t nbytes = typeDescr().size();

    // Prevent GC under ArrayBufferObject::create, which might move this object
    // and its contents.
    gc::AutoSuppressGC suppress(cx);

    ArrayBufferObject* buffer =
        ArrayBufferObject::create(cx, nbytes, contents, ArrayBufferObject::DoesntOwnData);
    if (!buffer)
        return nullptr;

    // The owning object must always be the array buffer's first view. This
    // both prevents the memory from disappearing out from under the buffer
    // (the first view is held strongly by the buffer) and is used by the
    // buffer marking code to detect whether its data pointer needs to be
    // relocated.
    JS_ALWAYS_TRUE(buffer->addView(cx, this));

    buffer->setForInlineTypedObject();
    buffer->setHasTypedObjectViews();

    if (!table->add(cx, this, buffer))
        return nullptr;

    if (IsInsideNursery(this)) {
        // Make sure the buffer is traced by the next generational collection,
        // so that its data pointer is updated after this typed object moves.
        zone()->group()->storeBuffer().putWholeCell(buffer);
    }

    return buffer;
}

ArrayBufferObject*
OutlineTransparentTypedObject::getOrCreateBuffer(JSContext* cx)
{
    if (owner().is<ArrayBufferObject>())
        return &owner().as<ArrayBufferObject>();
    return owner().as<InlineTransparentTypedObject>().getOrCreateBuffer(cx);
}

/******************************************************************************
 * Typed object classes
 */

const ObjectOps TypedObject::objectOps_ = {
    TypedObject::obj_lookupProperty,
    TypedObject::obj_defineProperty,
    TypedObject::obj_hasProperty,
    TypedObject::obj_getProperty,
    TypedObject::obj_setProperty,
    TypedObject::obj_getOwnPropertyDescriptor,
    TypedObject::obj_deleteProperty,
    nullptr,   /* getElements */
    nullptr, /* thisValue */
};

#define DEFINE_TYPEDOBJ_CLASS(Name, Trace, Moved)        \
    static const ClassOps Name##ClassOps = {             \
        nullptr,        /* addProperty */                \
        nullptr,        /* delProperty */                \
        nullptr,        /* enumerate   */                \
        TypedObject::obj_newEnumerate,                   \
        nullptr,        /* resolve     */                \
        nullptr,        /* mayResolve  */                \
        nullptr,        /* finalize    */                \
        nullptr,        /* call        */                \
        nullptr,        /* hasInstance */                \
        nullptr,        /* construct   */                \
        Trace,                                           \
    };                                                   \
    static const ClassExtension Name##ClassExt = {       \
        nullptr,        /* weakmapKeyDelegateOp */       \
        Moved           /* objectMovedOp */              \
    };                                                   \
    const Class Name::class_ = {                         \
        # Name,                                          \
        Class::NON_NATIVE |                              \
            JSCLASS_DELAY_METADATA_BUILDER,              \
        &Name##ClassOps,                                 \
        JS_NULL_CLASS_SPEC,                              \
        &Name##ClassExt,                                 \
        &TypedObject::objectOps_                         \
    }

DEFINE_TYPEDOBJ_CLASS(OutlineTransparentTypedObject,
                      OutlineTypedObject::obj_trace,
                      nullptr);
DEFINE_TYPEDOBJ_CLASS(OutlineOpaqueTypedObject,
                      OutlineTypedObject::obj_trace,
                      nullptr);
DEFINE_TYPEDOBJ_CLASS(InlineTransparentTypedObject,
                      InlineTypedObject::obj_trace,
                      InlineTypedObject::obj_moved);
DEFINE_TYPEDOBJ_CLASS(InlineOpaqueTypedObject,
                      InlineTypedObject::obj_trace,
                      InlineTypedObject::obj_moved);

static int32_t
LengthForType(TypeDescr& descr)
{
    switch (descr.kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Struct:
      case type::Simd:
        return 0;

      case type::Array:
        return descr.as<ArrayTypeDescr>().length();
    }

    MOZ_CRASH("Invalid kind");
}

static bool
CheckOffset(uint32_t offset, uint32_t size, uint32_t alignment, uint32_t bufferLength)
{
    // Offset (plus size) must be fully contained within the buffer.
    if (offset > bufferLength)
        return false;
    if (offset + size < offset)
        return false;
    if (offset + size > bufferLength)
        return false;

    // Offset must be aligned.
    if ((offset % alignment) != 0)
        return false;

    return true;
}

template<typename T, typename U, typename V, typename W>
inline bool CheckOffset(T, U, V, W) = delete;

/*static*/ bool
TypedObject::construct(JSContext* cx, unsigned int argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    MOZ_ASSERT(args.callee().is<TypeDescr>());
    Rooted<TypeDescr*> callee(cx, &args.callee().as<TypeDescr>());

    // Typed object constructors are overloaded in three ways, in order of
    // precedence:
    //
    //   new TypeObj()
    //   new TypeObj(buffer, [offset])
    //   new TypeObj(data)

    // Zero argument constructor:
    if (args.length() == 0) {
        int32_t length = LengthForType(*callee);
        Rooted<TypedObject*> obj(cx, createZeroed(cx, callee, length));
        if (!obj)
            return false;
        args.rval().setObject(*obj);
        return true;
    }

    // Buffer constructor.
    if (args[0].isObject() && args[0].toObject().is<ArrayBufferObject>()) {
        Rooted<ArrayBufferObject*> buffer(cx);
        buffer = &args[0].toObject().as<ArrayBufferObject>();

        if (callee->opaque() || buffer->isDetached()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_BAD_ARGS);
            return false;
        }

        uint32_t offset;
        if (args.length() >= 2 && !args[1].isUndefined()) {
            if (!args[1].isInt32() || args[1].toInt32() < 0) {
                JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_BAD_ARGS);
                return false;
            }

            offset = args[1].toInt32();
        } else {
            offset = 0;
        }

        if (args.length() >= 3 && !args[2].isUndefined()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_BAD_ARGS);
            return false;
        }

        if (!CheckOffset(offset, callee->size(), callee->alignment(),
                         buffer->byteLength()))
        {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_BAD_ARGS);
            return false;
        }

        Rooted<OutlineTypedObject*> obj(cx);
        obj = OutlineTypedObject::createUnattached(cx, callee, LengthForType(*callee));
        if (!obj)
            return false;

        obj->attach(cx, *buffer, offset);
        args.rval().setObject(*obj);
        return true;
    }

    // Data constructor.
    if (args[0].isObject()) {
        // Create the typed object.
        int32_t length = LengthForType(*callee);
        Rooted<TypedObject*> obj(cx, createZeroed(cx, callee, length));
        if (!obj)
            return false;

        // Initialize from `arg`.
        if (!ConvertAndCopyTo(cx, obj, args[0]))
            return false;
        args.rval().setObject(*obj);
        return true;
    }

    // Something bogus.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPEDOBJECT_BAD_ARGS);
    return false;
}

/* static */ JS::Result<TypedObject*, JS::OOM&>
TypedObject::create(JSContext* cx, js::gc::AllocKind kind, js::gc::InitialHeap heap,
                    js::HandleShape shape, js::HandleObjectGroup group)
{
    debugCheckNewObject(group, shape, kind, heap);

    const js::Class* clasp = group->clasp();
    MOZ_ASSERT(::IsTypedObjectClass(clasp));

    JSObject* obj = js::Allocate<JSObject>(cx, kind, /* nDynamicSlots = */ 0, heap, clasp);
    if (!obj)
        return cx->alreadyReportedOOM();

    TypedObject* tobj = static_cast<TypedObject*>(obj);
    tobj->initGroup(group);
    tobj->initShape(shape);

    MOZ_ASSERT(clasp->shouldDelayMetadataBuilder());
    cx->compartment()->setObjectPendingMetadata(cx, tobj);

    js::gc::TraceCreateObject(tobj);

    return tobj;
}

/******************************************************************************
 * Intrinsics
 */

bool
js::NewOpaqueTypedObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject() && args[0].toObject().is<TypeDescr>());

    Rooted<TypeDescr*> descr(cx, &args[0].toObject().as<TypeDescr>());
    int32_t length = TypedObjLengthFromType(*descr);
    Rooted<OutlineTypedObject*> obj(cx);
    obj = OutlineTypedObject::createUnattachedWithClass(cx, &OutlineOpaqueTypedObject::class_, descr, length);
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

bool
js::NewDerivedTypedObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);
    MOZ_ASSERT(args[0].isObject() && args[0].toObject().is<TypeDescr>());
    MOZ_ASSERT(args[1].isObject() && args[1].toObject().is<TypedObject>());
    MOZ_ASSERT(args[2].isInt32());

    Rooted<TypeDescr*> descr(cx, &args[0].toObject().as<TypeDescr>());
    Rooted<TypedObject*> typedObj(cx, &args[1].toObject().as<TypedObject>());
    uint32_t offset = AssertedCast<uint32_t>(args[2].toInt32());

    Rooted<TypedObject*> obj(cx);
    obj = OutlineTypedObject::createDerived(cx, descr, typedObj, offset);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

bool
js::AttachTypedObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);
    MOZ_ASSERT(args[2].isInt32());

    OutlineTypedObject& handle = args[0].toObject().as<OutlineTypedObject>();
    TypedObject& target = args[1].toObject().as<TypedObject>();
    MOZ_ASSERT(!handle.isAttached());
    uint32_t offset = AssertedCast<uint32_t>(args[2].toInt32());

    handle.attach(cx, target, offset);

    return true;
}

bool
js::SetTypedObjectOffset(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(args[0].isObject() && args[0].toObject().is<TypedObject>());
    MOZ_ASSERT(args[1].isInt32());

    OutlineTypedObject& typedObj = args[0].toObject().as<OutlineTypedObject>();
    int32_t offset = args[1].toInt32();

    MOZ_ASSERT(typedObj.isAttached());
    typedObj.resetOffset(offset);
    args.rval().setUndefined();
    return true;
}

bool
js::ObjectIsTypeDescr(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());
    args.rval().setBoolean(args[0].toObject().is<TypeDescr>());
    return true;
}

bool
js::ObjectIsTypedObject(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());
    args.rval().setBoolean(args[0].toObject().is<TypedObject>());
    return true;
}

bool
js::ObjectIsOpaqueTypedObject(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    JSObject& obj = args[0].toObject();
    args.rval().setBoolean(obj.is<TypedObject>() && obj.as<TypedObject>().opaque());
    return true;
}

bool
js::ObjectIsTransparentTypedObject(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    JSObject& obj = args[0].toObject();
    args.rval().setBoolean(obj.is<TypedObject>() && !obj.as<TypedObject>().opaque());
    return true;
}

bool
js::TypeDescrIsSimpleType(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[0].toObject().is<js::TypeDescr>());
    args.rval().setBoolean(args[0].toObject().is<js::SimpleTypeDescr>());
    return true;
}

bool
js::TypeDescrIsArrayType(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[0].toObject().is<js::TypeDescr>());
    JSObject& obj = args[0].toObject();
    args.rval().setBoolean(obj.is<js::ArrayTypeDescr>());
    return true;
}

bool
js::TypedObjectIsAttached(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    TypedObject& typedObj = args[0].toObject().as<TypedObject>();
    args.rval().setBoolean(typedObj.isAttached());
    return true;
}

bool
js::TypedObjectTypeDescr(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    TypedObject& typedObj = args[0].toObject().as<TypedObject>();
    args.rval().setObject(typedObj.typeDescr());
    return true;
}

bool
js::ClampToUint8(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isNumber());
    args.rval().setNumber(ClampDoubleToUint8(args[0].toNumber()));
    return true;
}

bool
js::GetTypedObjectModule(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    Rooted<GlobalObject*> global(cx, cx->global());
    MOZ_ASSERT(global);
    args.rval().setObject(global->getTypedObjectModule());
    return true;
}

bool
js::GetSimdTypeDescr(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isInt32());
    // One of the JS_SIMDTYPEREPR_* constants / a SimdType enum value.
    // getOrCreateSimdTypeDescr() will do the range check.
    int32_t simdTypeRepr = args[0].toInt32();
    Rooted<GlobalObject*> global(cx, cx->global());
    MOZ_ASSERT(global);
    auto* obj = GlobalObject::getOrCreateSimdTypeDescr(cx, global, SimdType(simdTypeRepr));
    args.rval().setObject(*obj);
    return true;
}

#define JS_STORE_SCALAR_CLASS_IMPL(_constant, T, _name)                         \
bool                                                                            \
js::StoreScalar##T::Func(JSContext* cx, unsigned argc, Value* vp)               \
{                                                                               \
    CallArgs args = CallArgsFromVp(argc, vp);                                   \
    MOZ_ASSERT(args.length() == 3);                                             \
    MOZ_ASSERT(args[0].isObject() && args[0].toObject().is<TypedObject>());     \
    MOZ_ASSERT(args[1].isInt32());                                              \
    MOZ_ASSERT(args[2].isNumber());                                             \
                                                                                \
    TypedObject& typedObj = args[0].toObject().as<TypedObject>();               \
    int32_t offset = args[1].toInt32();                                         \
                                                                                \
    /* Should be guaranteed by the typed objects API: */                        \
    MOZ_ASSERT(offset % MOZ_ALIGNOF(T) == 0);                                   \
                                                                                \
    JS::AutoCheckCannotGC nogc(cx);                                             \
    T* target = reinterpret_cast<T*>(typedObj.typedMem(offset, nogc));          \
    double d = args[2].toNumber();                                              \
    *target = ConvertScalar<T>(d);                                              \
    args.rval().setUndefined();                                                 \
    return true;                                                                \
}

#define JS_STORE_REFERENCE_CLASS_IMPL(_constant, T, _name)                      \
bool                                                                            \
js::StoreReference##_name::Func(JSContext* cx, unsigned argc, Value* vp)        \
{                                                                               \
    CallArgs args = CallArgsFromVp(argc, vp);                                   \
    MOZ_ASSERT(args.length() == 4);                                             \
    MOZ_ASSERT(args[0].isObject() && args[0].toObject().is<TypedObject>());     \
    MOZ_ASSERT(args[1].isInt32());                                              \
    MOZ_ASSERT(args[2].isString() || args[2].isNull());                         \
                                                                                \
    TypedObject& typedObj = args[0].toObject().as<TypedObject>();               \
    int32_t offset = args[1].toInt32();                                         \
                                                                                \
    jsid id = args[2].isString()                                                \
              ? IdToTypeId(AtomToId(&args[2].toString()->asAtom()))             \
              : JSID_VOID;                                                      \
                                                                                \
    /* Should be guaranteed by the typed objects API: */                        \
    MOZ_ASSERT(offset % MOZ_ALIGNOF(T) == 0);                                   \
                                                                                \
    JS::AutoCheckCannotGC nogc(cx);                                             \
    T* target = reinterpret_cast<T*>(typedObj.typedMem(offset, nogc));          \
    if (!store(cx, target, args[3], &typedObj, id))                             \
        return false;                                                           \
    args.rval().setUndefined();                                                 \
    return true;                                                                \
}

#define JS_LOAD_SCALAR_CLASS_IMPL(_constant, T, _name)                                  \
bool                                                                                    \
js::LoadScalar##T::Func(JSContext* cx, unsigned argc, Value* vp)                        \
{                                                                                       \
    CallArgs args = CallArgsFromVp(argc, vp);                                           \
    MOZ_ASSERT(args.length() == 2);                                                     \
    MOZ_ASSERT(args[0].isObject() && args[0].toObject().is<TypedObject>());             \
    MOZ_ASSERT(args[1].isInt32());                                                      \
                                                                                        \
    TypedObject& typedObj = args[0].toObject().as<TypedObject>();                       \
    int32_t offset = args[1].toInt32();                                                 \
                                                                                        \
    /* Should be guaranteed by the typed objects API: */                                \
    MOZ_ASSERT(offset % MOZ_ALIGNOF(T) == 0);                                           \
                                                                                        \
    JS::AutoCheckCannotGC nogc(cx);                                                     \
    T* target = reinterpret_cast<T*>(typedObj.typedMem(offset, nogc));                  \
    args.rval().setNumber(JS::CanonicalizeNaN((double) *target));                       \
    return true;                                                                        \
}

#define JS_LOAD_REFERENCE_CLASS_IMPL(_constant, T, _name)                       \
bool                                                                            \
js::LoadReference##_name::Func(JSContext* cx, unsigned argc, Value* vp)         \
{                                                                               \
    CallArgs args = CallArgsFromVp(argc, vp);                                   \
    MOZ_ASSERT(args.length() == 2);                                             \
    MOZ_ASSERT(args[0].isObject() && args[0].toObject().is<TypedObject>());     \
    MOZ_ASSERT(args[1].isInt32());                                              \
                                                                                \
    TypedObject& typedObj = args[0].toObject().as<TypedObject>();               \
    int32_t offset = args[1].toInt32();                                         \
                                                                                \
    /* Should be guaranteed by the typed objects API: */                        \
    MOZ_ASSERT(offset % MOZ_ALIGNOF(T) == 0);                                   \
                                                                                \
    JS::AutoCheckCannotGC nogc(cx);                                             \
    T* target = reinterpret_cast<T*>(typedObj.typedMem(offset, nogc));          \
    load(target, args.rval());                                                  \
    return true;                                                                \
}

// Because the precise syntax for storing values/objects/strings
// differs, we abstract it away using specialized variants of the
// private methods `store()` and `load()`.

bool
StoreReferenceAny::store(JSContext* cx, GCPtrValue* heap, const Value& v,
                         TypedObject* obj, jsid id)
{
    // Undefined values are not included in type inference information for
    // value properties of typed objects, as these properties are always
    // considered to contain undefined.
    if (!v.isUndefined()) {
        if (!cx->helperThread())
            AddTypePropertyId(cx, obj, id, v);
        else if (!HasTypePropertyId(obj, id, v))
            return false;
    }

    *heap = v;
    return true;
}

bool
StoreReferenceObject::store(JSContext* cx, GCPtrObject* heap, const Value& v,
                            TypedObject* obj, jsid id)
{
    MOZ_ASSERT(v.isObjectOrNull()); // or else Store_object is being misused

    // Null pointers are not included in type inference information for
    // object properties of typed objects, as these properties are always
    // considered to contain null.
    if (v.isObject()) {
        if (!cx->helperThread())
            AddTypePropertyId(cx, obj, id, v);
        else if (!HasTypePropertyId(obj, id, v))
            return false;
    }

    *heap = v.toObjectOrNull();
    return true;
}

bool
StoreReferencestring::store(JSContext* cx, GCPtrString* heap, const Value& v,
                            TypedObject* obj, jsid id)
{
    MOZ_ASSERT(v.isString()); // or else Store_string is being misused

    // Note: string references are not reflected in type information for the object.
    *heap = v.toString();

    return true;
}

void
LoadReferenceAny::load(GCPtrValue* heap, MutableHandleValue v)
{
    v.set(*heap);
}

void
LoadReferenceObject::load(GCPtrObject* heap, MutableHandleValue v)
{
    if (*heap)
        v.setObject(**heap);
    else
        v.setNull();
}

void
LoadReferencestring::load(GCPtrString* heap, MutableHandleValue v)
{
    v.setString(*heap);
}

// I was using templates for this stuff instead of macros, but ran
// into problems with the Unagi compiler.
JS_FOR_EACH_UNIQUE_SCALAR_TYPE_REPR_CTYPE(JS_STORE_SCALAR_CLASS_IMPL)
JS_FOR_EACH_UNIQUE_SCALAR_TYPE_REPR_CTYPE(JS_LOAD_SCALAR_CLASS_IMPL)
JS_FOR_EACH_REFERENCE_TYPE_REPR(JS_STORE_REFERENCE_CLASS_IMPL)
JS_FOR_EACH_REFERENCE_TYPE_REPR(JS_LOAD_REFERENCE_CLASS_IMPL)

///////////////////////////////////////////////////////////////////////////
// Walking memory

template<typename V>
static void
visitReferences(TypeDescr& descr,
                uint8_t* mem,
                V& visitor)
{
    if (descr.transparent())
        return;

    switch (descr.kind()) {
      case type::Scalar:
      case type::Simd:
        return;

      case type::Reference:
        visitor.visitReference(descr.as<ReferenceTypeDescr>(), mem);
        return;

      case type::Array:
      {
        ArrayTypeDescr& arrayDescr = descr.as<ArrayTypeDescr>();
        TypeDescr& elementDescr = arrayDescr.elementType();
        for (uint32_t i = 0; i < arrayDescr.length(); i++) {
            visitReferences(elementDescr, mem, visitor);
            mem += elementDescr.size();
        }
        return;
      }

      case type::Struct:
      {
        StructTypeDescr& structDescr = descr.as<StructTypeDescr>();
        for (size_t i = 0; i < structDescr.fieldCount(); i++) {
            TypeDescr& descr = structDescr.fieldDescr(i);
            size_t offset = structDescr.fieldOffset(i);
            visitReferences(descr, mem + offset, visitor);
        }
        return;
      }
    }

    MOZ_CRASH("Invalid type repr kind");
}

///////////////////////////////////////////////////////////////////////////
// Initializing instances

namespace {

class MemoryInitVisitor {
    const JSRuntime* rt_;

  public:
    explicit MemoryInitVisitor(const JSRuntime* rt)
      : rt_(rt)
    {}

    void visitReference(ReferenceTypeDescr& descr, uint8_t* mem);
};

} // namespace

void
MemoryInitVisitor::visitReference(ReferenceTypeDescr& descr, uint8_t* mem)
{
    switch (descr.type()) {
      case ReferenceTypeDescr::TYPE_ANY:
      {
        js::GCPtrValue* heapValue = reinterpret_cast<js::GCPtrValue*>(mem);
        heapValue->init(UndefinedValue());
        return;
      }

      case ReferenceTypeDescr::TYPE_OBJECT:
      {
        js::GCPtrObject* objectPtr =
            reinterpret_cast<js::GCPtrObject*>(mem);
        objectPtr->init(nullptr);
        return;
      }

      case ReferenceTypeDescr::TYPE_STRING:
      {
        js::GCPtrString* stringPtr =
            reinterpret_cast<js::GCPtrString*>(mem);
        stringPtr->init(rt_->emptyString);
        return;
      }
    }

    MOZ_CRASH("Invalid kind");
}

void
TypeDescr::initInstances(const JSRuntime* rt, uint8_t* mem, size_t length)
{
    MOZ_ASSERT(length >= 1);

    MemoryInitVisitor visitor(rt);

    // Initialize the 0th instance
    memset(mem, 0, size());
    if (opaque())
        visitReferences(*this, mem, visitor);

    // Stamp out N copies of later instances
    uint8_t* target = mem;
    for (size_t i = 1; i < length; i++) {
        target += size();
        memcpy(target, mem, size());
    }
}

///////////////////////////////////////////////////////////////////////////
// Tracing instances

namespace {

class MemoryTracingVisitor {
    JSTracer* trace_;

  public:

    explicit MemoryTracingVisitor(JSTracer* trace)
      : trace_(trace)
    {}

    void visitReference(ReferenceTypeDescr& descr, uint8_t* mem);
};

} // namespace

void
MemoryTracingVisitor::visitReference(ReferenceTypeDescr& descr, uint8_t* mem)
{
    switch (descr.type()) {
      case ReferenceTypeDescr::TYPE_ANY:
      {
        GCPtrValue* heapValue = reinterpret_cast<js::GCPtrValue*>(mem);
        TraceEdge(trace_, heapValue, "reference-val");
        return;
      }

      case ReferenceTypeDescr::TYPE_OBJECT:
      {
        GCPtrObject* objectPtr = reinterpret_cast<js::GCPtrObject*>(mem);
        TraceNullableEdge(trace_, objectPtr, "reference-obj");
        return;
      }

      case ReferenceTypeDescr::TYPE_STRING:
      {
        GCPtrString* stringPtr = reinterpret_cast<js::GCPtrString*>(mem);
        TraceNullableEdge(trace_, stringPtr, "reference-str");
        return;
      }
    }

    MOZ_CRASH("Invalid kind");
}

void
TypeDescr::traceInstances(JSTracer* trace, uint8_t* mem, size_t length)
{
    MemoryTracingVisitor visitor(trace);

    for (size_t i = 0; i < length; i++) {
        visitReferences(*this, mem, visitor);
        mem += size();
    }
}

namespace {

struct TraceListVisitor {
    typedef Vector<int32_t, 0, SystemAllocPolicy> VectorType;
    VectorType stringOffsets, objectOffsets, valueOffsets;

    void visitReference(ReferenceTypeDescr& descr, uint8_t* mem);

    bool fillList(Vector<int32_t>& entries);
};

} // namespace

void
TraceListVisitor::visitReference(ReferenceTypeDescr& descr, uint8_t* mem)
{
    VectorType* offsets;
    switch (descr.type()) {
      case ReferenceTypeDescr::TYPE_ANY: offsets = &valueOffsets; break;
      case ReferenceTypeDescr::TYPE_OBJECT: offsets = &objectOffsets; break;
      case ReferenceTypeDescr::TYPE_STRING: offsets = &stringOffsets; break;
      default: MOZ_CRASH("Invalid kind");
    }

    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!offsets->append((uintptr_t) mem))
        oomUnsafe.crash("TraceListVisitor::visitReference");
}

bool
TraceListVisitor::fillList(Vector<int32_t>& entries)
{
    return entries.appendAll(stringOffsets) &&
           entries.append(-1) &&
           entries.appendAll(objectOffsets) &&
           entries.append(-1) &&
           entries.appendAll(valueOffsets) &&
           entries.append(-1);
}

static bool
CreateTraceList(JSContext* cx, HandleTypeDescr descr)
{
    // Trace lists are only used for inline typed objects. We don't use them
    // for larger objects, both to limit the size of the trace lists and
    // because tracing outline typed objects is considerably more complicated
    // than inline ones.
    if (descr->size() > InlineTypedObject::MaximumSize || descr->transparent())
        return true;

    TraceListVisitor visitor;
    visitReferences(*descr, nullptr, visitor);

    Vector<int32_t> entries(cx);
    if (!visitor.fillList(entries))
        return false;

    // Trace lists aren't necessary for descriptors with no references.
    MOZ_ASSERT(entries.length() >= 3);
    if (entries.length() == 3)
        return true;

    int32_t* list = cx->pod_malloc<int32_t>(entries.length());
    if (!list)
        return false;

    PodCopy(list, entries.begin(), entries.length());

    descr->initReservedSlot(JS_DESCR_SLOT_TRACE_LIST, PrivateValue(list));
    return true;
}

/* static */ void
TypeDescr::finalize(FreeOp* fop, JSObject* obj)
{
    TypeDescr& descr = obj->as<TypeDescr>();
    if (descr.hasTraceList())
        js_free(const_cast<int32_t*>(descr.traceList()));
}
