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

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsprf.h"

#include "builtin/TypedObject.h"
#include "jit/InlinableNatives.h"
#include "js/Value.h"

#include "jsobjinlines.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::IsFinite;
using mozilla::IsNaN;
using mozilla::FloorLog2;
using mozilla::NumberIsInt32;

///////////////////////////////////////////////////////////////////////////
// SIMD

static bool
CheckVectorObject(HandleValue v, SimdTypeDescr::Type expectedType)
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

template bool js::IsVectorObject<Int8x16>(HandleValue v);
template bool js::IsVectorObject<Int16x8>(HandleValue v);
template bool js::IsVectorObject<Int32x4>(HandleValue v);
template bool js::IsVectorObject<Float32x4>(HandleValue v);
template bool js::IsVectorObject<Float64x2>(HandleValue v);

static inline bool
ErrorBadArgs(JSContext* cx)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
    return false;
}

static inline bool
ErrorWrongTypeArg(JSContext* cx, size_t argIndex, Handle<TypeDescr*> typeDescr)
{
    MOZ_ASSERT(argIndex < 10);
    char charArgIndex[2];
    JS_snprintf(charArgIndex, sizeof charArgIndex, "%d", argIndex);

    HeapSlot& typeNameSlot = typeDescr->getReservedSlotRef(JS_DESCR_SLOT_STRING_REPR);
    char* typeNameStr = JS_EncodeString(cx, typeNameSlot.toString());
    if (!typeNameStr)
        return false;

    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_SIMD_NOT_A_VECTOR,
                         typeNameStr, charArgIndex);
    JS_free(cx, typeNameStr);
    return false;
}

template<typename T>
static SimdTypeDescr*
GetTypeDescr(JSContext* cx)
{
    return cx->global()->getOrCreateSimdTypeDescr<T>(cx);
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

    Elem* mem = reinterpret_cast<Elem*>(v.toObject().as<TypedObject>().typedMem());
    *out = jit::SimdConstant::CreateX4(mem);
    return true;
}

template bool js::ToSimdConstant<Int32x4>(JSContext* cx, HandleValue v, jit::SimdConstant* out);
template bool js::ToSimdConstant<Float32x4>(JSContext* cx, HandleValue v, jit::SimdConstant* out);

template<typename Elem>
static Elem
TypedObjectMemory(HandleValue v)
{
    TypedObject& obj = v.toObject().as<TypedObject>();
    return reinterpret_cast<Elem>(obj.typedMem());
}

template<typename SimdType>
static bool SignMask(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename SimdType::Elem Elem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.thisv().isObject() || !args.thisv().toObject().is<TypedObject>()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             SimdTypeDescr::class_.name, "signMask",
                             InformalValueTypeName(args.thisv()));
        return false;
    }

    TypedObject& typedObj = args.thisv().toObject().as<TypedObject>();
    TypeDescr& descr = typedObj.typeDescr();
    if (descr.kind() != type::Simd || descr.as<SimdTypeDescr>().type() != SimdType::type) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                             SimdTypeDescr::class_.name, "signMask",
                             InformalValueTypeName(args.thisv()));
        return false;
    }

    // Load the data as integer so that we treat the sign bit consistently,
    // since -0.0 is not less than zero, but it still has the sign bit set.
    typedef typename mozilla::SignedStdintTypeForSize<sizeof(Elem)>::Type Int;
    static_assert(SimdType::lanes * sizeof(Int) <= jit::Simd128DataSize,
                  "signMask access should respect the bounds of the type");
    const Elem* elems = reinterpret_cast<const Elem*>(typedObj.typedMem());
    int32_t result = 0;
    for (unsigned i = 0; i < SimdType::lanes; ++i) {
        Int x = mozilla::BitwiseCast<Int>(elems[i]);
        result |= (x < 0) << i;
    }
    args.rval().setInt32(result);
    return true;
}

#define SIGN_MASK(type) \
static bool type##SignMask(JSContext* cx, unsigned argc, Value* vp) { \
    return SignMask<type>(cx, argc, vp); \
}
    SIGN_MASK(Float32x4);
    SIGN_MASK(Float64x2);
    SIGN_MASK(Int8x16);
    SIGN_MASK(Int16x8);
    SIGN_MASK(Int32x4);
#undef SIGN_MASK

const Class SimdTypeDescr::class_ = {
    "SIMD",
    JSCLASS_HAS_RESERVED_SLOTS(JS_DESCR_SLOTS) | JSCLASS_BACKGROUND_FINALIZE,
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    TypeDescr::finalize,
    call
};

namespace {

// These classes just exist to group together various properties and so on.
class Int8x16Defn {
  public:
    static const SimdTypeDescr::Type type = SimdTypeDescr::Int8x16;
    static const JSFunctionSpec TypeDescriptorMethods[];
    static const JSPropertySpec TypedObjectProperties[];
    static const JSFunctionSpec TypedObjectMethods[];
    static const JSFunctionSpec Methods[];
};
class Int16x8Defn {
  public:
    static const SimdTypeDescr::Type type = SimdTypeDescr::Int16x8;
    static const JSFunctionSpec TypeDescriptorMethods[];
    static const JSPropertySpec TypedObjectProperties[];
    static const JSFunctionSpec TypedObjectMethods[];
    static const JSFunctionSpec Methods[];
};
class Int32x4Defn {
  public:
    static const SimdTypeDescr::Type type = SimdTypeDescr::Int32x4;
    static const JSFunctionSpec TypeDescriptorMethods[];
    static const JSPropertySpec TypedObjectProperties[];
    static const JSFunctionSpec TypedObjectMethods[];
    static const JSFunctionSpec Methods[];
};
class Float32x4Defn {
  public:
    static const SimdTypeDescr::Type type = SimdTypeDescr::Float32x4;
    static const JSFunctionSpec TypeDescriptorMethods[];
    static const JSPropertySpec TypedObjectProperties[];
    static const JSFunctionSpec TypedObjectMethods[];
    static const JSFunctionSpec Methods[];
};
class Float64x2Defn {
  public:
    static const SimdTypeDescr::Type type = SimdTypeDescr::Float64x2;
    static const JSFunctionSpec TypeDescriptorMethods[];
    static const JSPropertySpec TypedObjectProperties[];
    static const JSFunctionSpec TypedObjectMethods[];
    static const JSFunctionSpec Methods[];
};

} // namespace

const JSFunctionSpec Float32x4Defn::TypeDescriptorMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    JS_SELF_HOSTED_FN("array", "ArrayShorthand", 1, 0),
    JS_SELF_HOSTED_FN("equivalent", "TypeDescrEquivalent", 1, 0),
    JS_FS_END
};

const JSPropertySpec Float32x4Defn::TypedObjectProperties[] = {
    JS_PSG("signMask", Float32x4SignMask, JSPROP_PERMANENT),
    JS_PS_END
};

const JSFunctionSpec Float32x4Defn::TypedObjectMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "SimdToSource", 0, 0),
    JS_FS_END
};

const JSFunctionSpec Float32x4Defn::Methods[] = {
#define SIMD_FLOAT32X4_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_float32x4_##Name, Operands, 0, SimdFloat32x4),
    FLOAT32X4_FUNCTION_LIST(SIMD_FLOAT32X4_FUNCTION_ITEM)
#undef SIMD_FLOAT32x4_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Float64x2Defn::TypeDescriptorMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    JS_SELF_HOSTED_FN("array", "ArrayShorthand", 1, 0),
    JS_SELF_HOSTED_FN("equivalent", "TypeDescrEquivalent", 1, 0),
    JS_FS_END
};

const JSPropertySpec Float64x2Defn::TypedObjectProperties[] = {
    JS_PSG("signMask", Float64x2SignMask, JSPROP_PERMANENT),
    JS_PS_END
};

const JSFunctionSpec Float64x2Defn::TypedObjectMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "SimdToSource", 0, 0),
    JS_FS_END
};

const JSFunctionSpec Float64x2Defn::Methods[]  = {
#define SIMD_FLOAT64X2_FUNCTION_ITEM(Name, Func, Operands) \
    JS_FN(#Name, js::simd_float64x2_##Name, Operands, 0),
    FLOAT64X2_FUNCTION_LIST(SIMD_FLOAT64X2_FUNCTION_ITEM)
#undef SIMD_FLOAT64X2_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Int8x16Defn::TypeDescriptorMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    JS_SELF_HOSTED_FN("array", "ArrayShorthand", 1, 0),
    JS_SELF_HOSTED_FN("equivalent", "TypeDescrEquivalent", 1, 0),
    JS_FS_END,
};

const JSPropertySpec Int8x16Defn::TypedObjectProperties[] = {
    JS_PSG("signMask", Int8x16SignMask, JSPROP_PERMANENT),
    JS_PS_END
};

const JSFunctionSpec Int8x16Defn::TypedObjectMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "SimdToSource", 0, 0),
    JS_FS_END
};

const JSFunctionSpec Int8x16Defn::Methods[] = {
#define SIMD_INT8X16_FUNCTION_ITEM(Name, Func, Operands) \
    JS_FN(#Name, js::simd_int8x16_##Name, Operands, 0),
    INT8X16_FUNCTION_LIST(SIMD_INT8X16_FUNCTION_ITEM)
#undef SIMD_INT8X16_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Int16x8Defn::TypeDescriptorMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    JS_SELF_HOSTED_FN("array", "ArrayShorthand", 1, 0),
    JS_SELF_HOSTED_FN("equivalent", "TypeDescrEquivalent", 1, 0),
    JS_FS_END,
};

const JSPropertySpec Int16x8Defn::TypedObjectProperties[] = {
    JS_PSG("signMask", Int16x8SignMask, JSPROP_PERMANENT),
    JS_PS_END
};

const JSFunctionSpec Int16x8Defn::TypedObjectMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "SimdToSource", 0, 0),
    JS_FS_END
};

const JSFunctionSpec Int16x8Defn::Methods[] = {
#define SIMD_INT16X8_FUNCTION_ITEM(Name, Func, Operands) \
    JS_FN(#Name, js::simd_int16x8_##Name, Operands, 0),
    INT16X8_FUNCTION_LIST(SIMD_INT16X8_FUNCTION_ITEM)
#undef SIMD_INT16X8_FUNCTION_ITEM
    JS_FS_END
};

const JSFunctionSpec Int32x4Defn::TypeDescriptorMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "DescrToSource", 0, 0),
    JS_SELF_HOSTED_FN("array", "ArrayShorthand", 1, 0),
    JS_SELF_HOSTED_FN("equivalent", "TypeDescrEquivalent", 1, 0),
    JS_FS_END,
};

const JSPropertySpec Int32x4Defn::TypedObjectProperties[] = {
    JS_PSG("signMask", Int32x4SignMask, JSPROP_PERMANENT),
    JS_PS_END
};

const JSFunctionSpec Int32x4Defn::TypedObjectMethods[] = {
    JS_SELF_HOSTED_FN("toSource", "SimdToSource", 0, 0),
    JS_FS_END
};

const JSFunctionSpec Int32x4Defn::Methods[] = {
#define SIMD_INT32X4_FUNCTION_ITEM(Name, Func, Operands) \
    JS_INLINABLE_FN(#Name, js::simd_int32x4_##Name, Operands, 0, SimdInt32x4),
    INT32X4_FUNCTION_LIST(SIMD_INT32X4_FUNCTION_ITEM)
#undef SIMD_INT32X4_FUNCTION_ITEM
    JS_FS_END
};

template<typename T>
static JSObject*
CreateAndBindSimdClass(JSContext* cx, Handle<GlobalObject*> global, HandleObject globalSimdObject,
                       HandlePropertyName stringRepr)
{
    const SimdTypeDescr::Type type = T::type;

    RootedObject funcProto(cx, global->getOrCreateFunctionPrototype(cx));
    if (!funcProto)
        return nullptr;

    // Create type constructor itself and initialize its reserved slots.
    Rooted<SimdTypeDescr*> typeDescr(cx);
    typeDescr = NewObjectWithGivenProto<SimdTypeDescr>(cx, funcProto, SingletonObject);
    if (!typeDescr)
        return nullptr;

    typeDescr->initReservedSlot(JS_DESCR_SLOT_KIND, Int32Value(type::Simd));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_STRING_REPR, StringValue(stringRepr));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_ALIGNMENT, Int32Value(SimdTypeDescr::alignment(type)));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_SIZE, Int32Value(SimdTypeDescr::size(type)));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_OPAQUE, BooleanValue(false));
    typeDescr->initReservedSlot(JS_DESCR_SLOT_TYPE, Int32Value(T::type));

    if (!CreateUserSizeAndAlignmentProperties(cx, typeDescr))
        return nullptr;

    // Create prototype property, which inherits from Object.prototype.
    RootedObject objProto(cx, global->getOrCreateObjectPrototype(cx));
    if (!objProto)
        return nullptr;
    Rooted<TypedProto*> proto(cx);
    proto = NewObjectWithGivenProto<TypedProto>(cx, objProto, SingletonObject);
    if (!proto)
        return nullptr;
    typeDescr->initReservedSlot(JS_DESCR_SLOT_TYPROTO, ObjectValue(*proto));

    // Link constructor to prototype and install properties.
    if (!JS_DefineFunctions(cx, typeDescr, T::TypeDescriptorMethods))
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, typeDescr, proto) ||
        !DefinePropertiesAndFunctions(cx, proto, T::TypedObjectProperties,
                                      T::TypedObjectMethods))
    {
        return nullptr;
    }

    // Bind type descriptor to the global SIMD object
    RootedValue typeValue(cx, ObjectValue(*typeDescr));
    if (!JS_DefineFunctions(cx, typeDescr, T::Methods) ||
        !DefineProperty(cx, globalSimdObject, stringRepr, typeValue, nullptr, nullptr,
                        JSPROP_READONLY | JSPROP_PERMANENT))
    {
        return nullptr;
    }

    uint32_t slot = uint32_t(typeDescr->type());
    globalSimdObject->as<NativeObject>().setReservedSlot(slot, ObjectValue(*typeDescr));

    return typeDescr;
}

template <typename T>
static bool
FillLanes(JSContext* cx, Handle<TypedObject*> result, const CallArgs& args)
{
    typedef typename T::Elem Elem;
    Elem tmp;
    for (unsigned i = 0; i < T::lanes; i++) {
        if (!T::Cast(cx, args.get(i), &tmp))
            return false;
        reinterpret_cast<Elem*>(result->typedMem())[i] = tmp;
    }
    args.rval().setObject(*result);
    return true;
}

bool
SimdTypeDescr::call(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    Rooted<SimdTypeDescr*> descr(cx, &args.callee().as<SimdTypeDescr>());
    MOZ_ASSERT(size_t(static_cast<TypeDescr*>(descr)->size()) <= InlineTypedObject::MaximumSize,
               "inline storage is needed for using InternalHandle belows");

    Rooted<TypedObject*> result(cx, TypedObject::createZeroed(cx, descr, 0));
    if (!result)
        return false;

    switch (descr->type()) {
      case SimdTypeDescr::Int8x16:   return FillLanes< ::Int8x16>(cx, result, args);
      case SimdTypeDescr::Int16x8:   return FillLanes< ::Int16x8>(cx, result, args);
      case SimdTypeDescr::Int32x4:   return FillLanes< ::Int32x4>(cx, result, args);
      case SimdTypeDescr::Float32x4: return FillLanes< ::Float32x4>(cx, result, args);
      case SimdTypeDescr::Float64x2: return FillLanes< ::Float64x2>(cx, result, args);
    }

    MOZ_CRASH("unexpected SIMD descriptor");
    return false;
}

///////////////////////////////////////////////////////////////////////////
// SIMD class

static const uint32_t SIMD_SLOTS_COUNT = SimdTypeDescr::LAST_TYPE + 1;

const Class SIMDObject::class_ = {
    "SIMD",
    JSCLASS_HAS_RESERVED_SLOTS(SIMD_SLOTS_COUNT)
};

bool
GlobalObject::initSimdObject(JSContext* cx, Handle<GlobalObject*> global)
{
    // SIMD relies on the TypedObject module being initialized.
    // In particular, the self-hosted code for array() wants
    // to be able to call GetTypedObjectModule(). It is NOT necessary
    // to install the TypedObjectModule global, but at the moment
    // those two things are not separable.
    if (!global->getOrCreateTypedObjectModule(cx))
        return false;

    RootedObject globalSimdObject(cx);
    RootedObject objProto(cx, global->getOrCreateObjectPrototype(cx));
    if (!objProto)
        return false;

    globalSimdObject = NewObjectWithGivenProto(cx, &SIMDObject::class_, objProto, SingletonObject);
    if (!globalSimdObject)
        return false;

    RootedValue globalSimdValue(cx, ObjectValue(*globalSimdObject));
    if (!DefineProperty(cx, global, cx->names().SIMD, globalSimdValue, nullptr, nullptr,
                        JSPROP_RESOLVING))
    {
        return false;
    }

    global->setConstructor(JSProto_SIMD, globalSimdValue);
    return true;
}

JSObject*
js::InitSIMDClass(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->is<GlobalObject>());
    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());

    RootedObject globalSimdObject(cx, global->getOrCreateSimdGlobalObject(cx));
    if (!globalSimdObject)
        return nullptr;

    RootedObject i8x16(cx);
    i8x16 = CreateAndBindSimdClass<Int8x16Defn>(cx, global, globalSimdObject, cx->names().int8x16);
    if (!i8x16)
        return nullptr;

    RootedObject i16x8(cx);
    i16x8 = CreateAndBindSimdClass<Int16x8Defn>(cx, global, globalSimdObject, cx->names().int16x8);
    if (!i16x8)
        return nullptr;

    RootedObject f32x4(cx);
    f32x4 = CreateAndBindSimdClass<Float32x4Defn>(cx, global, globalSimdObject, cx->names().float32x4);
    if (!f32x4)
        return nullptr;

    RootedObject i32x4(cx);
    i32x4 = CreateAndBindSimdClass<Int32x4Defn>(cx, global, globalSimdObject, cx->names().int32x4);
    if (!i32x4)
        return nullptr;

    RootedObject f64x2(cx);
    f64x2 = CreateAndBindSimdClass<Float64x2Defn>(cx, global, globalSimdObject, cx->names().float64x2);
    if (!f64x2)
        return nullptr;

    return globalSimdObject;
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

    Elem* resultMem = reinterpret_cast<Elem*>(result->typedMem());
    memcpy(resultMem, data, sizeof(Elem) * V::lanes);
    return result;
}

template JSObject* js::CreateSimd<Float32x4>(JSContext* cx, const Float32x4::Elem* data);
template JSObject* js::CreateSimd<Float64x2>(JSContext* cx, const Float64x2::Elem* data);
template JSObject* js::CreateSimd<Int8x16>(JSContext* cx, const Int8x16::Elem* data);
template JSObject* js::CreateSimd<Int16x8>(JSContext* cx, const Int16x8::Elem* data);
template JSObject* js::CreateSimd<Int32x4>(JSContext* cx, const Int32x4::Elem* data);

namespace js {
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
    static T apply(T x) { return -1 * x; }
};
template<typename T>
struct Not {
    static T apply(T x) { return ~x; }
};
template<typename T>
struct RecApprox {
    static T apply(T x) { return 1 / x; }
};
template<typename T>
struct RecSqrtApprox {
    static T apply(T x) { return 1 / sqrt(x); }
};
template<typename T>
struct Sqrt {
    static T apply(T x) { return sqrt(x); }
};

// Binary SIMD operators
template<typename T>
struct Add {
    static T apply(T l, T r) { return l + r; }
};
template<typename T>
struct Sub {
    static T apply(T l, T r) { return l - r; }
};
template<typename T>
struct Div {
    static T apply(T l, T r) { return l / r; }
};
template<typename T>
struct Mul {
    static T apply(T l, T r) { return l * r; }
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
// behavior, according to C++11 [expr.shift]p2.
template<typename T>
struct ShiftLeft {
    static T apply(T v, int32_t bits) {
        return uint32_t(bits) >= sizeof(T) * 8 ? 0 : v << bits;
    }
};
template<typename T>
struct ShiftRightArithmetic {
    static T apply(T v, int32_t bits) {
        uint32_t maxBits = sizeof(T) * 8;
        return v >> (uint32_t(bits) >= maxBits ? maxBits - 1 : bits);
    }
};
template<typename T>
struct ShiftRightLogical {
    static T apply(T v, int32_t bits) {
        return uint32_t(bits) >= sizeof(T) * 8 ? 0 : uint32_t(v) >> bits;
    }
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
    CoercionElem* val = TypedObjectMemory<CoercionElem*>(args[0]);
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
    CoercionElem* left = TypedObjectMemory<CoercionElem*>(args[0]);
    CoercionElem* right = TypedObjectMemory<CoercionElem*>(args[1]);
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
    if (args.length() < 2 || !IsVectorObject<V>(args[0]) || !args[1].isNumber())
        return ErrorBadArgs(cx);

    int32_t lane;
    if (!NumberIsInt32(args[1].toNumber(), &lane))
        return ErrorBadArgs(cx);
    if (lane < 0 || uint32_t(lane) >= V::lanes)
        return ErrorBadArgs(cx);

    Elem* vec = TypedObjectMemory<Elem*>(args[0]);
    Elem val = vec[lane];
    args.rval().set(V::ToValue(val));
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

    Elem* vec = TypedObjectMemory<Elem*>(args[0]);
    Elem result[V::lanes];

    int32_t lanearg;
    if (!args[1].isNumber() || !NumberIsInt32(args[1].toNumber(), &lanearg))
        return ErrorBadArgs(cx);
    if (lanearg < 0 || uint32_t(lanearg) >= V::lanes)
        return ErrorBadArgs(cx);
    uint32_t lane = uint32_t(lanearg);

    Elem value;
    if (!V::Cast(cx, args.get(2), &value))
        return false;

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

    uint32_t lanes[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++) {
        int32_t lane;
        if (!args[i + 1].isNumber() || !NumberIsInt32(args[i + 1].toNumber(), &lane))
            return ErrorBadArgs(cx);
        if (lane < 0 || uint32_t(lane) >= V::lanes)
            return ErrorBadArgs(cx);
        lanes[i] = uint32_t(lane);
    }

    Elem* val = TypedObjectMemory<Elem*>(args[0]);

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

    uint32_t lanes[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++) {
        int32_t lane;
        if (!args[i + 2].isNumber() || !NumberIsInt32(args[i + 2].toNumber(), &lane))
            return ErrorBadArgs(cx);
        if (lane < 0 || uint32_t(lane) >= (2 * V::lanes))
            return ErrorBadArgs(cx);
        lanes[i] = uint32_t(lane);
    }

    Elem* lhs = TypedObjectMemory<Elem*>(args[0]);
    Elem* rhs = TypedObjectMemory<Elem*>(args[1]);

    Elem result[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++) {
        Elem* selectedInput = lanes[i] < V::lanes ? lhs : rhs;
        result[i] = selectedInput[lanes[i] % V::lanes];
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

    Elem result[V::lanes];
    if (!IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    Elem* val = TypedObjectMemory<Elem*>(args[0]);
    int32_t bits;
    if (!ToInt32(cx, args[1], &bits))
        return false;

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
    InElem* left = TypedObjectMemory<InElem*>(args[0]);
    InElem* right = TypedObjectMemory<InElem*>(args[1]);
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

// All int32 can be safely converted to doubles.
template<>
struct ThrowOnConvert<int32_t, double> : public NeverThrow {};

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
        double d(v);
        return mozilla::IsNaN(d) ||
               d < double(mozilla::MinValue<IntegerType>::value) ||
               d > double(mozilla::MaxValue<IntegerType>::value);
    }
};

template<>
struct ThrowOnConvert<double, int32_t> : public ThrowIfNotInRange<double, int32_t> {};

template<>
struct ThrowOnConvert<float, int32_t> : public ThrowIfNotInRange<float, int32_t> {};

template<typename V, typename Vret>
static bool
FuncConvert(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename V::Elem Elem;
    typedef typename Vret::Elem RetElem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1 || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    Elem* val = TypedObjectMemory<Elem*>(args[0]);

    RetElem result[Vret::lanes];
    for (unsigned i = 0; i < Min(V::lanes, Vret::lanes); i++) {
        if (ThrowOnConvert<Elem, RetElem>::value(val[i])) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                 JSMSG_SIMD_FAILED_CONVERSION);
            return false;
        }
        result[i] = ConvertScalar<RetElem>(val[i]);
    }

    // Fill remaining lanes with 0
    for (unsigned i = V::lanes; i < Vret::lanes; i++)
        result[i] = 0;

    return StoreResult<Vret>(cx, args, result);
}

template<typename V, typename Vret>
static bool
FuncConvertBits(JSContext* cx, unsigned argc, Value* vp)
{
    typedef typename Vret::Elem RetElem;

    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1 || !IsVectorObject<V>(args[0]))
        return ErrorBadArgs(cx);

    // While we could just pass the typedMem of args[0] as StoreResults' last
    // argument, a GC could move the pointer to its memory in the meanwhile.
    // For consistency with other SIMD functions, simply copy the input in a
    // temporary array.
    RetElem copy[Vret::lanes];
    memcpy(copy, TypedObjectMemory<RetElem*>(args[0]), Vret::lanes * sizeof(RetElem));
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

    MaskTypeElem* val = TypedObjectMemory<MaskTypeElem*>(args[0]);
    MaskTypeElem* tv = TypedObjectMemory<MaskTypeElem*>(args[1]);
    MaskTypeElem* fv = TypedObjectMemory<MaskTypeElem*>(args[2]);

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

    MaskTypeElem* mask = TypedObjectMemory<MaskTypeElem*>(args[0]);
    Elem* tv = TypedObjectMemory<Elem*>(args[1]);
    Elem* fv = TypedObjectMemory<Elem*>(args[2]);

    Elem result[V::lanes];
    for (unsigned i = 0; i < V::lanes; i++)
        result[i] = mask[i] < 0 ? tv[i] : fv[i];

    return StoreResult<V>(cx, args, result);
}

template<class VElem, unsigned NumElem>
static bool
TypedArrayFromArgs(JSContext* cx, const CallArgs& args,
                   MutableHandleObject typedArray, int32_t* byteStart)
{
    if (!args[0].isObject())
        return ErrorBadArgs(cx);

    JSObject& argobj = args[0].toObject();
    if (!IsAnyTypedArray(&argobj))
        return ErrorBadArgs(cx);

    typedArray.set(&argobj);

    int32_t index;
    if (!ToInt32(cx, args[1], &index))
        return false;

    *byteStart = index * AnyTypedArrayBytesPerElement(typedArray);
    if (*byteStart < 0 ||
        (uint32_t(*byteStart) + NumElem * sizeof(VElem)) > AnyTypedArrayByteLength(typedArray))
    {
        // Keep in sync with AsmJS OnOutOfBounds function.
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
        return false;
    }

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

    int32_t byteStart;
    RootedObject typedArray(cx);
    if (!TypedArrayFromArgs<Elem, NumElem>(cx, args, &typedArray, &byteStart))
        return false;

    Rooted<TypeDescr*> typeDescr(cx, GetTypeDescr<V>(cx));
    if (!typeDescr)
        return false;

    Rooted<TypedObject*> result(cx, TypedObject::createZeroed(cx, typeDescr, 0));
    if (!result)
        return false;

    SharedMem<Elem*> src = AnyTypedArrayViewData(typedArray).addBytes(byteStart).cast<Elem*>();
    Elem* dst = reinterpret_cast<Elem*>(result->typedMem());
    jit::AtomicOperations::memcpySafeWhenRacy(dst, src, sizeof(Elem) * NumElem);

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

    int32_t byteStart;
    RootedObject typedArray(cx);
    if (!TypedArrayFromArgs<Elem, NumElem>(cx, args, &typedArray, &byteStart))
        return false;

    if (!IsVectorObject<V>(args[2]))
        return ErrorBadArgs(cx);

    Elem* src = TypedObjectMemory<Elem*>(args[2]);
    SharedMem<Elem*> dst = AnyTypedArrayViewData(typedArray).addBytes(byteStart).cast<Elem*>();
    js::jit::AtomicOperations::memcpySafeWhenRacy(dst, src, sizeof(Elem) * NumElem);

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
