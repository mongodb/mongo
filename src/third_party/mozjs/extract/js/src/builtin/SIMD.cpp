/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS SIMD pseudo-module.
 * Specification matches polyfill:
 * https://github.com/johnmccutchan/ecmascript_simd/blob/master/src/ecmascript_simd.js
 * The objects float32x4 and int32x4 are installed on the SIMD pseudo-module.
 */

#include "builtin/SIMD.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TypeTraits.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsnum.h"

#include "builtin/TypedObject.h"
#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "js/Value.h"

#include "vm/JSObject-inl.h"

using namespace js;

using mozilla::IsNaN;
using mozilla::EnableIf;
using mozilla::IsIntegral;
using mozilla::IsFloatingPoint;
using mozilla::IsSigned;
using mozilla::MakeUnsigned;

///////////////////////////////////////////////////////////////////////////
// SIMD

static_assert(unsigned(SimdType::Count) == 12, "sync with TypedObjectConstants.h");

static bool ArgumentToLaneIndex(JSContext* cx, JS::HandleValue v, unsigned limit, unsigned* lane);

static bool
CheckVectorObject(HandleValue v, SimdType expectedType)
{
    if (!v.isObject())
        return false;

    JSObject& obj = v.toObject();
    if (!obj.is<TypedObject>())
        return false;

    TypeDescr& typeRepr = obj.as<TypedObject>().typeDescr();
    if (typeRepr.kind() != type::Simd)
        return false;

    return typeRepr.as<SimdTypeDescr>().type() == expectedType;
}

template<class V>
bool
js::IsVectorObject(HandleValue v)
{
    return CheckVectorObject(v, V::type);
}

#define FOR_EACH_SIMD(macro) \
  macro(Int8x16)             \
  macro(Int16x8)             \
  macro(Int32x4)             \
  macro(Uint8x16)            \
  macro(Uint16x8)            \
  macro(Uint32x4)            \
  macro(Float32x4)           \
  macro(Float64x2)           \
  macro(Bool8x16)            \
  macro(Bool16x8)            \
  macro(Bool32x4)            \
  macro(Bool64x2)

#define InstantiateIsVectorObject_(T) \
    template bool js::IsVectorObject<T>(HandleValue v);
FOR_EACH_SIMD(InstantiateIsVectorObject_)
#undef InstantiateIsVectorObject_

const char*
js::SimdTypeToString(SimdType type)
{
    switch (type) {
#define RETSTR_(TYPE) case SimdType::TYPE: return #TYPE;
      FOR_EACH_SIMD(RETSTR_)
#undef RETSTR_
      case SimdType::Count: break;
    }
    return "<bad SimdType>";
}

PropertyName*
js::SimdTypeToName(const JSAtomState& atoms, SimdType type)
{
    switch (type) {
#define CASE_(TypeName) case SimdType::TypeName: return atoms.TypeName;
      FOR_EACH_SIMD(CASE_)
#undef CASE_
      case SimdType::Count: break;
    }
    MOZ_CRASH("bad SIMD type");
}

bool
js::IsSimdTypeName(const JSAtomState& atoms, const PropertyName* name, SimdType* type)
{
#define CHECK_(TypeName) if (name == atoms.TypeName) {   \
                             *type = SimdType::TypeName; \
                             return true;                \
                         }
    FOR_EACH_SIMD(CHECK_)
#undef CHECK_
    return false;
}

static inline bool
ErrorBadArgs(JSContext* cx)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
    return false;
}

static inline bool
ErrorWrongTypeArg(JSContext* cx, unsigned argIndex, Handle<TypeDescr*> typeDescr)
{
    MOZ_ASSERT(argIndex < 10);
    char charArgIndex[2];
    SprintfLiteral(charArgIndex, "%u", argIndex);

    HeapSlot& typeNameSlot = typeDescr->getReservedSlotRef(JS_DESCR_SLOT_STRING_REPR);
    char* typeNameStr = JS_EncodeString(cx, typeNameSlot.toString());
    if (!typeNameStr)
        return false;

    JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_SIMD_NOT_A_VECTOR,
                               typeNameStr, charArgIndex);
    JS_free(cx, typeNameStr);
    return false;
}

static inline bool
ErrorBadIndex(JSContext* cx)
{
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
    return false;
}

/* Non-standard: convert and range check an index value for SIMD operations.
 *
 *   1. numericIndex = ToNumber(argument)            (may throw TypeError)
 *   2. intIndex = ToInteger(numericIndex)
 *   3. if intIndex != numericIndex throw RangeError
 *
 * This function additionally bounds the range to the non-negative contiguous
 * integers:
 *
 *   4. if intIndex < 0 or intIndex > 2^53 throw RangeError
 *
 * Return true and set |*index| to the integer value if |argument| is a valid
 * array index argument. Otherwise report an TypeError or RangeError and return
 * false.
 *
 * The returned index will always be in the range 0 <= *index <= 2^53.
 */
static bool
NonStandardToIndex(JSContext* cx, HandleValue v, uint64_t* index)
{
    // Fast common case.
    if (v.isInt32()) {
        int32_t i = v.toInt32();
        if (i >= 0) {
            *index = i;
            return true;
        }
    }

    // Slow case. Use ToNumber() to coerce. This may throw a TypeError.
    double d;
    if (!ToNumber(cx, v, &d))
        return false;

    // Check that |d| is an integer in the valid range.
    //
    // Not all floating point integers fit in the range of a uint64_t, so we
    // need a rough range check before the real range check in our caller. We
    // could limit indexes to UINT64_MAX, but this would mean that our callers
    // have to be very careful about integer overflow. The contiguous integer
    // floating point numbers end at 2^53, so make that our upper limit. If we
    // ever support arrays with more than 2^53 elements, this will need to
    // change.
    //
    // Reject infinities, NaNs, and numbers outside the contiguous integer range
    // with a RangeError.

    // Write relation so NaNs throw a RangeError.
    if (!(0 <= d && d <= (uint64_t(1) << 53))) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
        return false;
    }

    // Check that d is an integer, throw a RangeError if not.
    // Note that this conversion could invoke undefined behaviour without the
    // range check above.
    uint64_t i(d);
    if (d != double(i)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
        return false;
    }

    *index = i;
    return true;
}

template<typename T>
static SimdTypeDescr*
GetTypeDescr(JSContext* cx)
{
    RootedGlobalObject global(cx, cx->global());
    return GlobalObject::getOrCreateSimdTypeDescr(cx, global, T::type);
}

template<typename V>
bool
js::ToSimdConstant(JSContext* cx, HandleValue v, jit::SimdConstant* out)
{
    typedef typename V::Elem Elem;
    Rooted<TypeDescr*> typeDescr(cx, GetTypeDescr<V>(cx));
    if (!typeDescr)
        return false;
    if (!IsVectorObject<V>(v))
        return ErrorWrongTypeArg(cx, 1, typeDescr);

    JS::AutoCheckCannotGC nogc(cx);
    Elem* mem = reinterpret_cast<Elem*>(v.toObject().as<TypedObject>().typedMem(nogc));
    *out = jit::SimdConstant::CreateSimd128(mem);
    return true;
}

template bool js::ToSimdConstant<Int8x16>(JSContext* cx, HandleValue v, jit::SimdConstant* out);
template bool js::ToSimdConstant<Int16x8>(JSContext* cx, HandleValue v, jit::SimdConstant* out);
template bool js::ToSimdConstant<Int32x4>(JSContext* cx, HandleValue v, jit::SimdConstant* out);
template bool js::ToSimdConstant<Float32x4>(JSContext* cx, HandleValue v, jit::SimdConstant* out);
template bool js::ToSimdConstant<Bool8x16>(JSContext* cx, HandleValue v, jit::SimdConstant* out);
template bool js::ToSimdConstant<Bool16x8>(JSContext* cx, HandleValue v, jit::SimdConstant* out);
template bool js::ToSimdConstant<Bool32x4>(JSContext* cx, HandleValue v, jit::SimdConstant* out);

template<typename Elem>
static Elem
TypedObjectMemory(HandleValue v, const JS::AutoRequireNoGC& nogc)
{
    TypedObject& obj = v.toObject().as<TypedObject>();
    return reinterpret_cast<Elem>(obj.typedMem(nogc));
}

static const ClassOps SimdTypeDescrClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    TypeDescr::finalize,
    SimdTypeDescr::call
};

const Class SimdTypeDescr::class_ = {
    "SIMD",
    JSCLASS_HAS_RESERVED_SLOTS(JS_DESCR_SLOTS) | JSCLASS_BACKGROUND_FINALIZE,
    &SimdTypeDescrClassOps
};

namespace {

// Define classes (Int8x16Defn, Int16x8Defn, etc.) to group together various
// properties and so on.
#define DEFINE_DEFN_(TypeName)                                       \
class TypeName##Defn {                                               \
  public:                                                            \
    static const JSFunctionSpec Methods[];                           \
};

FOR_EACH_SIMD(DEFINE_DEFN_)
#undef DEFINE_DEFN_

} // namespace

// Shared type descriptor methods for all SIMD types.
static const JSFunctionSpec TypeDescriptorMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    JS_SELF_HOSTED_FN("array", "ArrayShorthand", 1, 0),
    JS_SELF_HOSTED_FN("equivalent", "TypeDescrEquivalent", 1, 0),
    JS_FS_END
};

// Shared TypedObject methods for all SIMD types.
static const JSFunctionSpec SimdTypedObjectMethods[] = {
    JS_SELF_HOSTED_FN("toString", "SimdToString", 0, 0),
    JS_SELF_HOSTED_FN("valueOf", "SimdValueOf", 0, 0),
    JS_SELF_HOSTED_FN("toSource", "SimdToSource", 0, 0),
    JS_FS_END
};

// Provide JSJitInfo structs for those types that are supported by Ion.
// The controlling SIMD type is encoded as the InlinableNative primary opcode.
// The SimdOperation within the type is encoded in the .depth field.
//
// The JS_INLINABLE_FN macro refers to js::JitInfo_##native which we provide as
// Simd##Type##_##Operation
//
// /!\ Don't forget to keep this list in sync with the SIMD instrinics used in
// SelfHosting.cpp.

namespace js {
namespace jit {

static_assert(uint64_t(SimdOperation::Last) <= UINT16_MAX, "SimdOperation must fit in uint16_t");

// See also JitInfo_* in MCallOptimize.cpp. We provide a JSJitInfo for all the
// named functions here. The default JitInfo_SimdInt32x4 etc structs represent the
// SimdOperation::Constructor.
#define DEFN(TYPE, OP) const JSJitInfo JitInfo_Simd##TYPE##_##OP = {                             \
     /* .getter, unused for inlinable natives. */                                                \
    { nullptr },                                                                                 \
    /* .inlinableNative, but we have to init first union member: .protoID. */                    \
    { uint16_t(InlinableNative::Simd##TYPE) },                                                   \
    /* .nativeOp. Actually initializing first union member .depth. */                            \
    { uint16_t(SimdOperation::Fn_##OP) },                                                        \
    /* .type_ bitfield says this in an inlinable native function. */                             \
    JSJitInfo::InlinableNative                                                                   \
    /* Remaining fields are not used for inlinable natives. They are zero-initialized. */        \
};

// This list of inlinable types should match the one in jit/InlinableNatives.h.
#define TDEFN(Name, Func, Operands) DEFN(Float32x4, Name)
FLOAT32X4_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Int8x16, Name)
INT8X16_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Uint8x16, Name)
UINT8X16_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Int16x8, Name)
INT16X8_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Uint16x8, Name)
UINT16X8_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Int32x4, Name)
INT32X4_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Uint32x4, Name)
UINT32X4_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Bool8x16, Name)
BOOL8X16_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Bool16x8, Name)
BOOL16X8_FUNCTION_LIST(TDEFN)
#undef TDEFN

#define TDEFN(Name, Func, Operands) DEFN(Bool32x4, Name)
BOOL32X4_FUNCTION_LIST(TDEFN)
#undef TDEFN

} // namespace jit
} // namespace js

const JSFunctionSpec Float32x4Defn::Methods[] = {
#define SIMD_FLOAT32X4_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_float32x4_##Name, Operands, 0, SimdFloat32x4_##Name),
    FLOAT32X4_FUNCTION_LIST(SIMD_FLOAT32X4_FUNCTION_ITEM)
#undef SIMD_FLOAT32x4_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Float64x2Defn::Methods[]  = {
#define SIMD_FLOAT64X2_FUNCTION_ITEM(Name, Func, Operands) \
    JS_FN(#Name, js::simd_float64x2_##Name, Operands, 0),
    FLOAT64X2_FUNCTION_LIST(SIMD_FLOAT64X2_FUNCTION_ITEM)
#undef SIMD_FLOAT64X2_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Int8x16Defn::Methods[] = {
#define SIMD_INT8X16_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_int8x16_##Name, Operands, 0, SimdInt8x16_##Name),
    INT8X16_FUNCTION_LIST(SIMD_INT8X16_FUNCTION_ITEM)
#undef SIMD_INT8X16_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Int16x8Defn::Methods[] = {
#define SIMD_INT16X8_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_int16x8_##Name, Operands, 0, SimdInt16x8_##Name),
    INT16X8_FUNCTION_LIST(SIMD_INT16X8_FUNCTION_ITEM)
#undef SIMD_INT16X8_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Int32x4Defn::Methods[] = {
#define SIMD_INT32X4_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_int32x4_##Name, Operands, 0, SimdInt32x4_##Name),
    INT32X4_FUNCTION_LIST(SIMD_INT32X4_FUNCTION_ITEM)
#undef SIMD_INT32X4_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Uint8x16Defn::Methods[] = {
#define SIMD_UINT8X16_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_uint8x16_##Name, Operands, 0, SimdUint8x16_##Name),
    UINT8X16_FUNCTION_LIST(SIMD_UINT8X16_FUNCTION_ITEM)
#undef SIMD_UINT8X16_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Uint16x8Defn::Methods[] = {
#define SIMD_UINT16X8_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_uint16x8_##Name, Operands, 0, SimdUint16x8_##Name),
    UINT16X8_FUNCTION_LIST(SIMD_UINT16X8_FUNCTION_ITEM)
#undef SIMD_UINT16X8_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Uint32x4Defn::Methods[] = {
#define SIMD_UINT32X4_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_uint32x4_##Name, Operands, 0, SimdUint32x4_##Name),
    UINT32X4_FUNCTION_LIST(SIMD_UINT32X4_FUNCTION_ITEM)
#undef SIMD_UINT32X4_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Bool8x16Defn::Methods[] = {
#define SIMD_BOOL8X16_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_bool8x16_##Name, Operands, 0, SimdBool8x16_##Name),
    BOOL8X16_FUNCTION_LIST(SIMD_BOOL8X16_FUNCTION_ITEM)
#undef SIMD_BOOL8X16_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Bool16x8Defn::Methods[] = {
#define SIMD_BOOL16X8_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_bool16x8_##Name, Operands, 0, SimdBool16x8_##Name),
    BOOL16X8_FUNCTION_LIST(SIMD_BOOL16X8_FUNCTION_ITEM)
#undef SIMD_BOOL16X8_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Bool32x4Defn::Methods[] = {
#define SIMD_BOOL32X4_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_bool32x4_##Name, Operands, 0, SimdBool32x4_##Name),
    BOOL32X4_FUNCTION_LIST(SIMD_BOOL32X4_FUNCTION_ITEM)
#undef SIMD_BOOL32X4_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Bool64x2Defn::Methods[] = {
#define SIMD_BOOL64X2_FUNCTION_ITEM(Name, Func, Operands) \
    JS_FN(#Name, js::simd_bool64x2_##Name, Operands, 0),
    BOOL64X2_FUNCTION_LIST(SIMD_BOOL64X2_FUNCTION_ITEM)
#undef SIMD_BOOL64x2_FUNCTION_ITEM
    JS_FS_END
};

template <typename T>
static bool
FillLanes(JSContext* cx, Handle<TypedObject*> result, const CallArgs& args)
{
    typedef typename T::Elem Elem;
    Elem tmp;
    for (unsigned i = 0; i < T::lanes; i++) {
        if (!T::Cast(cx, args.get(i), &tmp))
            return false;
        // Reassure typedMem() that we won't GC while holding onto the returned
        // pointer, even though we could GC on every iteration of this loop
        // (but it is safe because we re-fetch each time.)
        JS::AutoCheckCannotGC nogc(cx);
        reinterpret_cast<Elem*>(result->typedMem(nogc))[i] = tmp;
    }
    args.rval().setObject(*result);
    return true;
}

bool
SimdTypeDescr::call(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    Rooted<SimdTypeDescr*> descr(cx, &args.callee().as<SimdTypeDescr>());
    Rooted<TypedObject*> result(cx, TypedObject::createZeroed(cx, descr, 0));
    if (!result)
        return false;

#define CASE_CALL_(Type) \
      case SimdType::Type:   return FillLanes< ::Type>(cx, result, args);

    switch (descr->type()) {
      FOR_EACH_SIMD(CASE_CALL_)
      case SimdType::Count: break;
    }

#undef CASE_CALL_
    MOZ_CRASH("unexpected SIMD descriptor");
    return false;
}

///////////////////////////////////////////////////////////////////////////
// SIMD class

static const ClassOps SimdObjectClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    SimdObject::resolve
};

const Class SimdObject::class_ = {
    "SIMD",
    JSCLASS_HAS_RESERVED_SLOTS(uint32_t(SimdType::Count)),
    &SimdObjectClassOps
};

/* static */ bool
GlobalObject::initSimdObject(JSContext* cx, Handle<GlobalObject*> global)
{
    // SIMD relies on the TypedObject module being initialized.
    // In particular, the self-hosted code for array() wants
    // to be able to call GetTypedObjectModule(). It is NOT necessary
    // to install the TypedObjectModule global, but at the moment
    // those two things are not separable.
    if (!GlobalObject::getOrCreateTypedObjectModule(cx, global))
        return false;

    RootedObject globalSimdObject(cx);
    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!objProto)
        return false;

    globalSimdObject = NewObjectWithGivenProto(cx, &SimdObject::class_, objProto, SingletonObject);
    if (!globalSimdObject)
        return false;

    RootedValue globalSimdValue(cx, ObjectValue(*globalSimdObject));
    if (!DefineDataProperty(cx, global, cx->names().SIMD, globalSimdValue, JSPROP_RESOLVING))
        return false;

    global->setConstructor(JSProto_SIMD, globalSimdValue);
    return true;
}

static bool
CreateSimdType(JSContext* cx, Handle<GlobalObject*> global, HandlePropertyName stringRepr,
               SimdType simdType, const JSFunctionSpec* methods)
{
    RootedObject funcProto(cx, GlobalObject::getOrCreateFunctionPrototype(cx, global));
    if (!funcProto)
        return false;

    // Create type constructor itself and initialize its reserved slots.
    Rooted<SimdTypeDescr*> typeDescr(cx);
    typeDescr = NewObjectWithGivenProto<SimdTypeDescr>(cx, funcProto, SingletonObject);
    if (!typeDescr)
        return false;

    typeDescr->initReservedSlot(JS_DESCR_SLOT_KIND, Int32Value(type::Simd));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_STRING_REPR, StringValue(stringRepr));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_ALIGNMENT, Int32Value(SimdTypeDescr::alignment(simdType)));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_SIZE, Int32Value(SimdTypeDescr::size(simdType)));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_OPAQUE, BooleanValue(false));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_TYPE, Int32Value(uint8_t(simdType)));

    if (!CreateUserSizeAndAlignmentProperties(cx, typeDescr))
        return false;

    // Create prototype property, which inherits from Object.prototype.
    RootedObject objProto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!objProto)
        return false;
    Rooted<TypedProto*> proto(cx);
    proto = NewObjectWithGivenProto<TypedProto>(cx, objProto, SingletonObject);
    if (!proto)
        return false;
    typeDescr->initReservedSlot(JS_DESCR_SLOT_TYPROTO, ObjectValue(*proto));

    // Link constructor to prototype and install properties.
    if (!JS_DefineFunctions(cx, typeDescr, TypeDescriptorMethods))
        return false;

    if (!LinkConstructorAndPrototype(cx, typeDescr, proto) ||
        !JS_DefineFunctions(cx, proto, SimdTypedObjectMethods))
    {
        return false;
    }

    // Bind type descriptor to the global SIMD object
    RootedObject globalSimdObject(cx, GlobalObject::getOrCreateSimdGlobalObject(cx, global));
    MOZ_ASSERT(globalSimdObject);

    RootedValue typeValue(cx, ObjectValue(*typeDescr));
    if (!JS_DefineFunctions(cx, typeDescr, methods) ||
        !DefineDataProperty(cx, globalSimdObject, stringRepr, typeValue,
                            JSPROP_READONLY | JSPROP_PERMANENT | JSPROP_RESOLVING))
    {
        return false;
    }

    uint32_t slot = uint32_t(typeDescr->type());
    MOZ_ASSERT(globalSimdObject->as<NativeObject>().getReservedSlot(slot).isUndefined());
    globalSimdObject->as<NativeObject>().setReservedSlot(slot, ObjectValue(*typeDescr));
    return !!typeDescr;
}

/* static */ bool
GlobalObject::initSimdType(JSContext* cx, Handle<GlobalObject*> global, SimdType simdType)
{
#define CREATE_(Type) \
    case SimdType::Type: \
      return CreateSimdType(cx, global, cx->names().Type, simdType, Type##Defn::Methods);

    switch (simdType) {
      FOR_EACH_SIMD(CREATE_)
      case SimdType::Count: break;
    }
    MOZ_CRASH("unexpected simd type");

#undef CREATE_
}

/* static */ SimdTypeDescr*
GlobalObject::getOrCreateSimdTypeDescr(JSContext* cx, Handle<GlobalObject*> global,
                                       SimdType simdType)
{
    MOZ_ASSERT(unsigned(simdType) < unsigned(SimdType::Count), "Invalid SIMD type");

    RootedObject globalSimdObject(cx, GlobalObject::getOrCreateSimdGlobalObject(cx, global));
    if (!globalSimdObject)
       return nullptr;

    uint32_t typeSlotIndex = uint32_t(simdType);
    if (globalSimdObject->as<NativeObject>().getReservedSlot(typeSlotIndex).isUndefined() &&
        !GlobalObject::initSimdType(cx, global, simdType))
    {
        return nullptr;
    }

    const Value& slot = globalSimdObject->as<NativeObject>().getReservedSlot(typeSlotIndex);
    MOZ_ASSERT(slot.isObject());
    return &slot.toObject().as<SimdTypeDescr>();
}

bool
SimdObject::resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolved)
{
    *resolved = false;
    if (!JSID_IS_ATOM(id))
        return true;
    JSAtom* str = JSID_TO_ATOM(id);
    Rooted<GlobalObject*> global(cx, cx->global());
#define TRY_RESOLVE_(Type)                                                    \
    if (str == cx->names().Type) {                                            \
        *resolved = CreateSimdType(cx, global, cx->names().Type,              \
                                   SimdType::Type, Type##Defn::Methods);      \
        return *resolved;                                                     \
    }
    FOR_EACH_SIMD(TRY_RESOLVE_)
#undef TRY_RESOLVE_
    return true;
}

JSObject*
js::InitSimdClass(JSContext* cx, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    return GlobalObject::getOrCreateSimdGlobalObject(cx, global);
}

template<typename V>
JSObject*
js::CreateSimd(JSContext* cx, const typename V::Elem* data)
{
    typedef typename V::Elem Elem;
    Rooted<TypeDescr*> typeDescr(cx, GetTypeDescr<V>(cx));
    if (!typeDescr)
        return nullptr;

    Rooted<TypedObject*> result(cx, TypedObject::createZeroed(cx, typeDescr, 0));
    if (!result)
        return nullptr;

    JS::AutoCheckCannotGC nogc(cx);
    Elem* resultMem = reinterpret_cast<Elem*>(result->typedMem(nogc));
    memcpy(resultMem, data, sizeof(Elem) * V::lanes);
    return result;
}

#define InstantiateCreateSimd_(Type) \
    template JSObject* js::CreateSimd<Type>(JSContext* cx, const Type::Elem* data);

FOR_EACH_SIMD(InstantiateCreateSimd_)

#undef InstantiateCreateSimd_

#undef FOR_EACH_SIMD

namespace js {

namespace detail {

template<typename T, typename Enable = void>
struct MaybeMakeUnsigned {
    using Type = T;
};

template<typename T>
struct MaybeMakeUnsigned<T, typename EnableIf<IsIntegral<T>::value && IsSigned<T>::value>::Type> {
    using Type = typename MakeUnsigned<T>::Type;
};

} // namespace detail

// Unary SIMD operators
template<typename T>
struct Identity {
    static T apply(T x) { return x; }
};
template<typename T>
struct Abs {
    static T apply(T x) { return mozilla::Abs(x); }
};
template<typename T>
struct Neg {
    using MaybeUnsignedT = typename detail::MaybeMakeUnsigned<T>::Type;
    static T apply(T x) {
        // Prepend |1U| to force integral promotion through *unsigned* types.
        // Otherwise when |T = uint16_t| and |int| is 32-bit, we could have
        // |uint16_t(-1) * uint16_t(65535)| which would really be
        // |int(65535) * int(65535)|, but as |4294836225 > 2147483647| would
        // perform signed integer overflow.
        // https://stackoverflow.com/questions/24795651/whats-the-best-c-way-to-multiply-unsigned-integers-modularly-safely
        return static_cast<MaybeUnsignedT>(1U * MaybeUnsignedT(-1) * MaybeUnsignedT(x));
    }
};
template<typename T>
struct Not {
    static T apply(T x) { return ~x; }
};
template<typename T>
struct LogicalNot {
    static T apply(T x) { return !x; }
};
template<typename T>
struct RecApprox {
    static_assert(IsFloatingPoint<T>::value, "RecApprox only supported for floating points");
    static T apply(T x) { return 1 / x; }
};
template<typename T>
struct RecSqrtApprox {
    static_assert(IsFloatingPoint<T>::value, "RecSqrtApprox only supported for floating points");
    static T apply(T x) { return 1 / sqrt(x); }
};
template<typename T>
struct Sqrt {
    static_assert(IsFloatingPoint<T>::value, "Sqrt only supported for floating points");
    static T apply(T x) { return sqrt(x); }
};

// Binary SIMD operators
template<typename T>
struct Add {
    using MaybeUnsignedT = typename detail::MaybeMakeUnsigned<T>::Type;
    static T apply(T l, T r) { return MaybeUnsignedT(l) + MaybeUnsignedT(r); }
};
template<typename T>
struct Sub {
    using MaybeUnsignedT = typename detail::MaybeMakeUnsigned<T>::Type;
    static T apply(T l, T r) { return MaybeUnsignedT(l) - MaybeUnsignedT(r); }
};
template<typename T>
struct Div {
    static_assert(IsFloatingPoint<T>::value, "Div only supported for floating points");
    static T apply(T l, T r) { return l / r; }
};
template<typename T>
struct Mul {
    using MaybeUnsignedT = typename detail::MaybeMakeUnsigned<T>::Type;
    static T apply(T l, T r) { return MaybeUnsignedT(l) * MaybeUnsignedT(r); }
};
template<typename T>
struct Minimum {
    static T apply(T l, T r) { return math_min_impl(l, r); }
};
template<typename T>
struct MinNum {
    static T apply(T l, T r) { return IsNaN(l) ? r : (IsNaN(r) ? l : math_min_impl(l, r)); }
};
template<typename T>
struct Maximum {
    static T apply(T l, T r) { return math_max_impl(l, r); }
};
template<typename T>
struct MaxNum {
    static T apply(T l, T r) { return IsNaN(l) ? r : (IsNaN(r) ? l : math_max_impl(l, r)); }
};
template<typename T>
struct LessThan {
    static bool apply(T l, T r) { return l < r; }
};
template<typename T>
struct LessThanOrEqual {
    static bool apply(T l, T r) { return l <= r; }
};
template<typename T>
struct GreaterThan {
    static bool apply(T l, T r) { return l > r; }
};
template<typename T>
struct GreaterThanOrEqual {
    static bool apply(T l, T r) { return l >= r; }
};
template<typename T>
struct Equal {
    static bool apply(T l, T r) { return l == r; }
};
template<typename T>
struct NotEqual {
    static bool apply(T l, T r) { return l != r; }
};
template<typename T>
struct Xor {
    static T apply(T l, T r) { return l ^ r; }
};
template<typename T>
struct And {
    static T apply(T l, T r) { return l & r; }
};
template<typename T>
struct Or {
    static T apply(T l, T r) { return l | r; }
};

// For the following three operators, if the value v we're trying to shift is
// such that v << bits can't fit in the int32 range, then we have undefined
// behavior, according to C++11 [expr.shift]p2. However, left-shifting an
// unsigned type is well-defined.
//
// In C++, shifting by an amount outside the range [0;N-1] is undefined
// behavior. SIMD.js reduces the shift amount modulo the number of bits in a
// lane and has defined behavior for all shift amounts.
template<typename T>
struct ShiftLeft {
    static T apply(T v, int32_t bits) {
        typedef typename mozilla::MakeUnsigned<T>::Type UnsignedT;
        uint32_t maskedBits = uint32_t(bits) % (sizeof(T) * 8);
        return UnsignedT(v) << maskedBits;
    }
};
template<typename T>
struct ShiftRightArithmetic {
    static T apply(T v, int32_t bits) {
        typedef typename mozilla::MakeSigned<T>::Type SignedT;
        uint32_t maskedBits = uint32_t(bits) % (sizeof(T) * 8);
        return SignedT(v) >> maskedBits;
    }
};
template<typename T>
struct ShiftRightLogical {
    static T apply(T v, int32_t bits) {
        typedef typename mozilla::MakeUnsigned<T>::Type UnsignedT;
        uint32_t maskedBits = uint32_t(bits) % (sizeof(T) * 8);
        return UnsignedT(v) >> maskedBits;
    }
};

// Saturating arithmetic is only defined on types smaller than int.
// Clamp `x` into the range supported by the integral type T.
template<typename T>
static T
Saturate(int x)
{
    static_assert(mozilla::IsIntegral<T>::value, "Only integer saturation supported");
    static_assert(sizeof(T) < sizeof(int), "Saturating int-sized arithmetic is not safe");
    const T lower = mozilla::MinValue<T>::value;
    const T upper = mozilla::MaxValue<T>::value;
    if (x > int(upper))
        return upper;
    if (x < int(lower))
        return lower;
    return T(x);
}

// Since signed integer overflow is undefined behavior in C++, it would be
// wildly irresponsible to attempt something as dangerous as adding two numbers
// coming from user code. However, in this case we know that T is smaller than
// int, so there is no way these operations can cause overflow. The
// static_assert in Saturate() enforces this for us.
template<typename T>
struct AddSaturate {
    static T apply(T l, T r) { return Saturate<T>(l + r); }
};
template<typename T>
struct SubSaturate {
    static T apply(T l, T r) { return Saturate<T>(l - r); }
};

} // namespace js

template<typename Out>
static bool
StoreResult(JSContext* cx, CallArgs& args, typename Out::Elem* result)
{
    RootedObject obj(cx, CreateSimd<Out>(cx, result));
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

// StoreResult can GC, and it is commonly used after pulling something out of a
// TypedObject:
//
//   Elem result = op(TypedObjectMemory<Elem>(args[0]));
//   StoreResult<Out>(..., result);
//
// The pointer extracted from the typed object in args[0] in the above example
// could be an interior pointer, and therefore be invalidated by GC.
// TypedObjectMemory() requires an assertion token to be passed in to prove
// that we won't GC, but the scope of eg an AutoCheckCannotGC RAII object
// extends to the end of its containing scope -- which would include the call
// to StoreResult, resulting in a rooting hazard.
//
// TypedObjectElemArray fixes this by wrapping the problematic pointer in a
// type, and the analysis is able to see that it is dead before calling
// StoreResult. (But if another GC called is made before the pointer is dead,
// it will correctly report a hazard.)
//
template <typename Elem>
class TypedObjectElemArray {
    Elem* elements;
  public:
    explicit TypedObjectElemArray(HandleValue objVal) {
        JS::AutoCheckCannotGC nogc;
        elements = TypedObjectMemory<Elem*>(objVal, nogc);
    }
    Elem& operator[](int i) { return elements[i]; }
} JS_HAZ_GC_POINTER;

// Coerces the inputs of type In to the type Coercion, apply the operator Op
// and converts the result to the type Out.
template<typename In, typename Coercion, template<typename C> class Op, typename Out>
static bool
CoercedUnaryFunc(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename Coercion::Elem CoercionElem;
    typedef typename Out::Elem RetElem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1 || !IsVectorObject<In>(args[0]))
        return ErrorBadArgs(cx);

    CoercionElem result[Coercion::lanes];
    TypedObjectElemArray<CoercionElem> val(args[0]);
    for (unsigned i = 0; i < Coercion::lanes; i++)
        result[i] = Op<CoercionElem>::apply(val[i]);
    return StoreResult<Out>(cx, args, (RetElem*) result);
}

// Coerces the inputs of type In to the type Coercion, apply the operator Op
// and converts the result to the type Out.
template<typename In, typename Coercion, template<typename C> class Op, typename Out>
static bool
CoercedBinaryFunc(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename Coercion::Elem CoercionElem;
    typedef typename Out::Elem RetElem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2 || !IsVectorObject<In>(args[0]) || !IsVectorObject<In>(args[1]))
        return ErrorBadArgs(cx);

    CoercionElem result[Coercion::lanes];
    TypedObjectElemArray<CoercionElem> left(args[0]);
    TypedObjectElemArray<CoercionElem> right(args[1]);
    for (unsigned i = 0; i < Coercion::lanes; i++)
        result[i] = Op<CoercionElem>::apply(left[i], right[i]);
    return StoreResult<Out>(cx, args, (RetElem*) result);
}

// Same as above, with no coercion, i.e. Coercion == In.
template<typename In, template<typename C> class Op, typename Out>
static bool
UnaryFunc(JSContext* cx, unsigned argc, Value* vp)
{
    return CoercedUnaryFunc<In, Out, Op, Out>(cx, argc, vp);
}

template<typename In, template<typename C> class Op, typename Out>
static bool
BinaryFunc(JSContext* cx, unsigned argc, Value* vp)
{
    return CoercedBinaryFunc<In, Out, Op, Out>(cx, argc, vp);
}

template<typename V>
static bool
ExtractLane(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 2 || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    unsigned lane;
    if (!ArgumentToLaneIndex(cx, args[1], V::lanes, &lane))
        return false;

    JS::AutoCheckCannotGC nogc(cx);
    Elem* vec = TypedObjectMemory<Elem*>(args[0], nogc);
    Elem val = vec[lane];
    args.rval().set(V::ToValue(val));
    return true;
}

template<typename V>
static bool
AllTrue(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1 || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    JS::AutoCheckCannotGC nogc(cx);
    Elem* vec = TypedObjectMemory<Elem*>(args[0], nogc);
    bool allTrue = true;
    for (unsigned i = 0; allTrue && i < V::lanes; i++)
        allTrue = vec[i];

    args.rval().setBoolean(allTrue);
    return true;
}

template<typename V>
static bool
AnyTrue(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1 || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    JS::AutoCheckCannotGC nogc(cx);
    Elem* vec = TypedObjectMemory<Elem*>(args[0], nogc);
    bool anyTrue = false;
    for (unsigned i = 0; !anyTrue && i < V::lanes; i++)
        anyTrue = vec[i];

    args.rval().setBoolean(anyTrue);
    return true;
}

template<typename V>
static bool
ReplaceLane(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    // Only the first and second arguments are mandatory
    if (args.length() < 2 || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    unsigned lane;
    if (!ArgumentToLaneIndex(cx, args[1], V::lanes, &lane))
        return false;

    Elem value;
    if (!V::Cast(cx, args.get(2), &value))
        return false;

    TypedObjectElemArray<Elem> vec(args[0]);
    Elem result[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++)
        result[i] = i == lane ? value : vec[i];

    return StoreResult<V>(cx, args, result);
}

template<typename V>
static bool
Swizzle(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != (V::lanes + 1) || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    unsigned lanes[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++) {
        if (!ArgumentToLaneIndex(cx, args[i + 1], V::lanes, &lanes[i]))
            return false;
    }

    TypedObjectElemArray<Elem> val(args[0]);
    Elem result[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++)
        result[i] = val[lanes[i]];

    return StoreResult<V>(cx, args, result);
}

template<typename V>
static bool
Shuffle(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != (V::lanes + 2) || !IsVectorObject<V>(args[0]) || !IsVectorObject<V>(args[1]))
        return ErrorBadArgs(cx);

    unsigned lanes[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++) {
        if (!ArgumentToLaneIndex(cx, args[i + 2], 2 * V::lanes, &lanes[i]))
            return false;
    }

    Elem result[V::lanes];
    {
        JS::AutoCheckCannotGC nogc(cx);
        Elem* lhs = TypedObjectMemory<Elem*>(args[0], nogc);
        Elem* rhs = TypedObjectMemory<Elem*>(args[1], nogc);

        for (unsigned i = 0; i < V::lanes; i++) {
            Elem* selectedInput = lanes[i] < V::lanes ? lhs : rhs;
            result[i] = selectedInput[lanes[i] % V::lanes];
        }
    }

    return StoreResult<V>(cx, args, result);
}

template<typename V, template<typename T> class Op>
static bool
BinaryScalar(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2)
        return ErrorBadArgs(cx);

    if (!IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    int32_t bits;
    if (!ToInt32(cx, args[1], &bits))
        return false;

    TypedObjectElemArray<Elem> val(args[0]);
    Elem result[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++)
        result[i] = Op<Elem>::apply(val[i], bits);

    return StoreResult<V>(cx, args, result);
}

template<typename In, template<typename C> class Op, typename Out>
static bool
CompareFunc(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename In::Elem InElem;
    typedef typename Out::Elem OutElem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2 || !IsVectorObject<In>(args[0]) || !IsVectorObject<In>(args[1]))
        return ErrorBadArgs(cx);

    OutElem result[Out::lanes];
    TypedObjectElemArray<InElem> left(args[0]);
    TypedObjectElemArray<InElem> right(args[1]);
    for (unsigned i = 0; i < Out::lanes; i++) {
        unsigned j = (i * In::lanes) / Out::lanes;
        result[i] = Op<InElem>::apply(left[j], right[j]) ? -1 : 0;
    }

    return StoreResult<Out>(cx, args, result);
}

// This struct defines whether we should throw during a conversion attempt,
// when trying to convert a value of type from From to the type To.  This
// happens whenever a C++ conversion would have undefined behavior (and perhaps
// be platform-dependent).
template<typename From, typename To>
struct ThrowOnConvert;

struct NeverThrow
{
    static bool value(int32_t v) {
        return false;
    }
};

// While int32 to float conversions can be lossy, these conversions have
// defined behavior in C++, so we don't need to care about them here. In practice,
// this means round to nearest, tie with even (zero bit in significand).
template<>
struct ThrowOnConvert<int32_t, float> : public NeverThrow {};

template<>
struct ThrowOnConvert<uint32_t, float> : public NeverThrow {};

// All int32 can be safely converted to doubles.
template<>
struct ThrowOnConvert<int32_t, double> : public NeverThrow {};

template<>
struct ThrowOnConvert<uint32_t, double> : public NeverThrow {};

// All floats can be safely converted to doubles.
template<>
struct ThrowOnConvert<float, double> : public NeverThrow {};

// Double to float conversion for inputs which aren't in the float range are
// undefined behavior in C++, but they're defined in IEEE754.
template<>
struct ThrowOnConvert<double, float> : public NeverThrow {};

// Float to integer conversions have undefined behavior if the float value
// is out of the representable integer range (on x86, will yield the undefined
// value pattern, namely 0x80000000; on arm, will clamp the input value), so
// check this here.
template<typename From, typename IntegerType>
struct ThrowIfNotInRange
{
    static_assert(mozilla::IsIntegral<IntegerType>::value, "bad destination type");

    static bool value(From v) {
        // Truncate to integer value before the range check.
        double d = trunc(double(v));
        // Arrange relations so NaN returns true (i.e., it throws a RangeError).
        return !(d >= double(mozilla::MinValue<IntegerType>::value) &&
                 d <= double(mozilla::MaxValue<IntegerType>::value));
    }
};

template<>
struct ThrowOnConvert<double, int32_t> : public ThrowIfNotInRange<double, int32_t> {};

template<>
struct ThrowOnConvert<double, uint32_t> : public ThrowIfNotInRange<double, uint32_t> {};

template<>
struct ThrowOnConvert<float, int32_t> : public ThrowIfNotInRange<float, int32_t> {};

template<>
struct ThrowOnConvert<float, uint32_t> : public ThrowIfNotInRange<float, uint32_t> {};

template<typename V, typename Vret>
static bool
FuncConvert(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;
    typedef typename Vret::Elem RetElem;

    static_assert(!mozilla::IsSame<V,Vret>::value, "Can't convert SIMD type to itself");
    static_assert(V::lanes == Vret::lanes, "Can only convert from same number of lanes");
    static_assert(!mozilla::IsIntegral<Elem>::value || !mozilla::IsIntegral<RetElem>::value,
                  "Cannot convert between integer SIMD types");

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1 || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    TypedObjectElemArray<Elem> val(args[0]);
    RetElem result[Vret::lanes];
    for (unsigned i = 0; i < V::lanes; i++) {
        if (ThrowOnConvert<Elem, RetElem>::value(val[i])) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_SIMD_FAILED_CONVERSION);
            return false;
        }
        result[i] = ConvertScalar<RetElem>(val[i]);
    }

    return StoreResult<Vret>(cx, args, result);
}

template<typename V, typename Vret>
static bool
FuncConvertBits(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;
    typedef typename Vret::Elem RetElem;

    static_assert(!mozilla::IsSame<V, Vret>::value, "Can't convert SIMD type to itself");
    static_assert(V::lanes * sizeof(Elem) == Vret::lanes * sizeof(RetElem),
                  "Can only bitcast from the same number of bits");

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1 || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    // While we could just pass the typedMem of args[0] as StoreResults' last
    // argument, a GC could move the pointer to its memory in the meanwhile.
    // For consistency with other SIMD functions, simply copy the input in a
    // temporary array.
    RetElem copy[Vret::lanes];
    {
        JS::AutoCheckCannotGC nogc(cx);
        memcpy(copy, TypedObjectMemory<RetElem*>(args[0], nogc), Vret::lanes * sizeof(RetElem));
    }
    return StoreResult<Vret>(cx, args, copy);
}

template<typename Vret>
static bool
FuncSplat(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename Vret::Elem RetElem;

    CallArgs args = CallArgsFromVp(argc, vp);
    RetElem arg;
    if (!Vret::Cast(cx, args.get(0), &arg))
        return false;

    RetElem result[Vret::lanes];
    for (unsigned i = 0; i < Vret::lanes; i++)
        result[i] = arg;
    return StoreResult<Vret>(cx, args, result);
}

template<typename V>
static bool
Bool(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);

    Elem result[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++)
        result[i] = ToBoolean(args.get(i)) ? -1 : 0;
    return StoreResult<V>(cx, args, result);
}

template<typename V, typename MaskType>
static bool
SelectBits(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;
    typedef typename MaskType::Elem MaskTypeElem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 3 || !IsVectorObject<MaskType>(args[0]) ||
        !IsVectorObject<V>(args[1]) || !IsVectorObject<V>(args[2]))
    {
        return ErrorBadArgs(cx);
    }

    TypedObjectElemArray<MaskTypeElem> val(args[0]);
    TypedObjectElemArray<MaskTypeElem> tv(args[1]);
    TypedObjectElemArray<MaskTypeElem> fv(args[2]);

    MaskTypeElem tr[MaskType::lanes];
    for (unsigned i = 0; i < MaskType::lanes; i++)
        tr[i] = And<MaskTypeElem>::apply(val[i], tv[i]);

    MaskTypeElem fr[MaskType::lanes];
    for (unsigned i = 0; i < MaskType::lanes; i++)
        fr[i] = And<MaskTypeElem>::apply(Not<MaskTypeElem>::apply(val[i]), fv[i]);

    MaskTypeElem orInt[MaskType::lanes];
    for (unsigned i = 0; i < MaskType::lanes; i++)
        orInt[i] = Or<MaskTypeElem>::apply(tr[i], fr[i]);

    Elem* result = reinterpret_cast<Elem*>(orInt);
    return StoreResult<V>(cx, args, result);
}

template<typename V, typename MaskType>
static bool
Select(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;
    typedef typename MaskType::Elem MaskTypeElem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 3 || !IsVectorObject<MaskType>(args[0]) ||
        !IsVectorObject<V>(args[1]) || !IsVectorObject<V>(args[2]))
    {
        return ErrorBadArgs(cx);
    }

    TypedObjectElemArray<MaskTypeElem> mask(args[0]);
    TypedObjectElemArray<Elem> tv(args[1]);
    TypedObjectElemArray<Elem> fv(args[2]);

    Elem result[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++)
        result[i] = mask[i] ? tv[i] : fv[i];

    return StoreResult<V>(cx, args, result);
}

// Extract an integer lane index from a function argument.
//
// Register an exception and return false if the argument is not suitable.
static bool
ArgumentToLaneIndex(JSContext* cx, JS::HandleValue v, unsigned limit, unsigned* lane)
{
    uint64_t arg;
    if (!NonStandardToIndex(cx, v, &arg))
        return false;
    if (arg >= limit)
        return ErrorBadIndex(cx);

    *lane = unsigned(arg);
    return true;
}

// Look for arguments (ta, idx) where ta is a TypedArray and idx is a
// non-negative integer.
// Check that accessBytes can be accessed starting from index idx in the array.
// Return the array handle in typedArray and idx converted to a byte offset in byteStart.
static bool
TypedArrayFromArgs(JSContext* cx, const CallArgs& args, uint32_t accessBytes,
                   MutableHandleObject typedArray, size_t* byteStart)
{
    if (!args[0].isObject())
        return ErrorBadArgs(cx);

    JSObject& argobj = args[0].toObject();
    if (!argobj.is<TypedArrayObject>())
        return ErrorBadArgs(cx);

    typedArray.set(&argobj);

    uint64_t index;
    if (!NonStandardToIndex(cx, args[1], &index))
        return false;

    // Do the range check in 64 bits even when size_t is 32 bits.
    // This can't overflow because index <= 2^53.
    uint64_t bytes = index * typedArray->as<TypedArrayObject>().bytesPerElement();
    // Keep in sync with AsmJS OnOutOfBounds function.
    if ((bytes + accessBytes) > typedArray->as<TypedArrayObject>().byteLength())
        return ErrorBadIndex(cx);

    *byteStart = bytes;

    return true;
}

template<class V, unsigned NumElem>
static bool
Load(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2)
        return ErrorBadArgs(cx);

    size_t byteStart;
    RootedObject typedArray(cx);
    if (!TypedArrayFromArgs(cx, args, sizeof(Elem) * NumElem, &typedArray, &byteStart))
        return false;

    Rooted<TypeDescr*> typeDescr(cx, GetTypeDescr<V>(cx));
    if (!typeDescr)
        return false;

    Rooted<TypedObject*> result(cx, TypedObject::createZeroed(cx, typeDescr, 0));
    if (!result)
        return false;

    JS::AutoCheckCannotGC nogc(cx);
    SharedMem<Elem*> src =
        typedArray->as<TypedArrayObject>().viewDataEither().addBytes(byteStart).cast<Elem*>();
    Elem* dst = reinterpret_cast<Elem*>(result->typedMem(nogc));
    jit::AtomicOperations::podCopySafeWhenRacy(SharedMem<Elem*>::unshared(dst), src, NumElem);

    args.rval().setObject(*result);
    return true;
}

template<class V, unsigned NumElem>
static bool
Store(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 3)
        return ErrorBadArgs(cx);

    size_t byteStart;
    RootedObject typedArray(cx);
    if (!TypedArrayFromArgs(cx, args, sizeof(Elem) * NumElem, &typedArray, &byteStart))
        return false;

    if (!IsVectorObject<V>(args[2]))
        return ErrorBadArgs(cx);

    JS::AutoCheckCannotGC nogc(cx);
    Elem* src = TypedObjectMemory<Elem*>(args[2], nogc);
    SharedMem<Elem*> dst =
        typedArray->as<TypedArrayObject>().viewDataEither().addBytes(byteStart).cast<Elem*>();
    js::jit::AtomicOperations::podCopySafeWhenRacy(dst, SharedMem<Elem*>::unshared(src), NumElem);

    args.rval().setObject(args[2].toObject());
    return true;
}

#define DEFINE_SIMD_FLOAT32X4_FUNCTION(Name, Func, Operands)       \
bool                                                               \
js::simd_float32x4_##Name(JSContext* cx, unsigned argc, Value* vp) \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
FLOAT32X4_FUNCTION_LIST(DEFINE_SIMD_FLOAT32X4_FUNCTION)
#undef DEFINE_SIMD_FLOAT32X4_FUNCTION

#define DEFINE_SIMD_FLOAT64X2_FUNCTION(Name, Func, Operands)       \
bool                                                               \
js::simd_float64x2_##Name(JSContext* cx, unsigned argc, Value* vp) \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
FLOAT64X2_FUNCTION_LIST(DEFINE_SIMD_FLOAT64X2_FUNCTION)
#undef DEFINE_SIMD_FLOAT64X2_FUNCTION

#define DEFINE_SIMD_INT8X16_FUNCTION(Name, Func, Operands)         \
bool                                                               \
js::simd_int8x16_##Name(JSContext* cx, unsigned argc, Value* vp)   \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
INT8X16_FUNCTION_LIST(DEFINE_SIMD_INT8X16_FUNCTION)
#undef DEFINE_SIMD_INT8X16_FUNCTION

#define DEFINE_SIMD_INT16X8_FUNCTION(Name, Func, Operands)         \
bool                                                               \
js::simd_int16x8_##Name(JSContext* cx, unsigned argc, Value* vp)   \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
INT16X8_FUNCTION_LIST(DEFINE_SIMD_INT16X8_FUNCTION)
#undef DEFINE_SIMD_INT16X8_FUNCTION

#define DEFINE_SIMD_INT32X4_FUNCTION(Name, Func, Operands)         \
bool                                                               \
js::simd_int32x4_##Name(JSContext* cx, unsigned argc, Value* vp)   \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
INT32X4_FUNCTION_LIST(DEFINE_SIMD_INT32X4_FUNCTION)
#undef DEFINE_SIMD_INT32X4_FUNCTION

#define DEFINE_SIMD_UINT8X16_FUNCTION(Name, Func, Operands)        \
bool                                                               \
js::simd_uint8x16_##Name(JSContext* cx, unsigned argc, Value* vp)  \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
UINT8X16_FUNCTION_LIST(DEFINE_SIMD_UINT8X16_FUNCTION)
#undef DEFINE_SIMD_UINT8X16_FUNCTION

#define DEFINE_SIMD_UINT16X8_FUNCTION(Name, Func, Operands)        \
bool                                                               \
js::simd_uint16x8_##Name(JSContext* cx, unsigned argc, Value* vp)  \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
UINT16X8_FUNCTION_LIST(DEFINE_SIMD_UINT16X8_FUNCTION)
#undef DEFINE_SIMD_UINT16X8_FUNCTION

#define DEFINE_SIMD_UINT32X4_FUNCTION(Name, Func, Operands)        \
bool                                                               \
js::simd_uint32x4_##Name(JSContext* cx, unsigned argc, Value* vp)  \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
UINT32X4_FUNCTION_LIST(DEFINE_SIMD_UINT32X4_FUNCTION)
#undef DEFINE_SIMD_UINT32X4_FUNCTION

#define DEFINE_SIMD_BOOL8X16_FUNCTION(Name, Func, Operands)        \
bool                                                               \
js::simd_bool8x16_##Name(JSContext* cx, unsigned argc, Value* vp)  \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}

BOOL8X16_FUNCTION_LIST(DEFINE_SIMD_BOOL8X16_FUNCTION)
#undef DEFINE_SIMD_BOOL8X16_FUNCTION

#define DEFINE_SIMD_BOOL16X8_FUNCTION(Name, Func, Operands)        \
bool                                                               \
js::simd_bool16x8_##Name(JSContext* cx, unsigned argc, Value* vp)  \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
BOOL16X8_FUNCTION_LIST(DEFINE_SIMD_BOOL16X8_FUNCTION)
#undef DEFINE_SIMD_BOOL16X8_FUNCTION

#define DEFINE_SIMD_BOOL32X4_FUNCTION(Name, Func, Operands)        \
bool                                                               \
js::simd_bool32x4_##Name(JSContext* cx, unsigned argc, Value* vp)  \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
BOOL32X4_FUNCTION_LIST(DEFINE_SIMD_BOOL32X4_FUNCTION)
#undef DEFINE_SIMD_BOOL32X4_FUNCTION

#define DEFINE_SIMD_BOOL64X2_FUNCTION(Name, Func, Operands)        \
bool                                                               \
js::simd_bool64x2_##Name(JSContext* cx, unsigned argc, Value* vp)  \
{                                                                  \
    return Func(cx, argc, vp);                                     \
}
BOOL64X2_FUNCTION_LIST(DEFINE_SIMD_BOOL64X2_FUNCTION)
#undef DEFINE_SIMD_BOOL64X2_FUNCTION
