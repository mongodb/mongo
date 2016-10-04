/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SelfHosting.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsdate.h"
#include "jsfriendapi.h"
#include "jshashutil.h"
#include "jsweakmap.h"
#include "jswrapper.h"
#include "selfhosted.out.h"

#include "builtin/Intl.h"
#include "builtin/MapObject.h"
#include "builtin/ModuleObject.h"
#include "builtin/Object.h"
#include "builtin/Reflect.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/SIMD.h"
#include "builtin/TypedObject.h"
#include "builtin/WeakSetObject.h"
#include "gc/Marking.h"
#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "js/Date.h"
#include "vm/Compression.h"
#include "vm/GeneratorObject.h"
#include "vm/Interpreter.h"
#include "vm/String.h"
#include "vm/TypedArrayCommon.h"

#include "jsfuninlines.h"
#include "jsscriptinlines.h"

#include "vm/BooleanObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/NumberObject-inl.h"
#include "vm/StringObject-inl.h"

using namespace js;
using namespace js::selfhosted;

using JS::AutoCheckCannotGC;
using mozilla::IsInRange;
using mozilla::Maybe;
using mozilla::PodMove;
using mozilla::UniquePtr;
using mozilla::Maybe;

static void
selfHosting_ErrorReporter(JSContext* cx, const char* message, JSErrorReport* report)
{
    PrintError(cx, stderr, message, report, true);
}

static bool
intrinsic_ToObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedValue val(cx, args[0]);
    RootedObject obj(cx, ToObject(cx, val));
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

static bool
intrinsic_IsObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    Value val = args[0];
    bool isObject = val.isObject();
    args.rval().setBoolean(isObject);
    return true;
}

static bool
intrinsic_ToInteger(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    double result;
    if (!ToInteger(cx, args[0], &result))
        return false;
    args.rval().setNumber(result);
    return true;
}

static bool
intrinsic_ToString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedString str(cx);
    str = ToString<CanGC>(cx, args[0]);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
intrinsic_ToPropertyKey(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args[0], &id))
        return false;

    args.rval().set(IdToValue(id));
    return true;
}

static bool
intrinsic_IsCallable(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(IsCallable(args[0]));
    return true;
}

static bool
intrinsic_IsConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(IsConstructor(args[0]));
    return true;
}

template<typename T>
static bool
intrinsic_IsInstanceOfBuiltin(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    args.rval().setBoolean(args[0].toObject().is<T>());
    return true;
}

static bool
intrinsic_SubstringKernel(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args[0].isString());
    MOZ_ASSERT(args[1].isInt32());
    MOZ_ASSERT(args[2].isInt32());

    RootedString str(cx, args[0].toString());
    int32_t begin = args[1].toInt32();
    int32_t length = args[2].toInt32();

    JSString* substr = SubstringKernel(cx, str, begin, length);
    if (!substr)
        return false;

    args.rval().setString(substr);
    return true;
}

static bool
intrinsic_OwnPropertyKeys(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[1].isInt32());
    return GetOwnPropertyKeys(cx, args, args[1].toInt32());
}

static void
ThrowErrorWithType(JSContext* cx, JSExnType type, const CallArgs& args)
{
    uint32_t errorNumber = args[0].toInt32();

#ifdef DEBUG
    const JSErrorFormatString* efs = GetErrorMessage(nullptr, errorNumber);
    MOZ_ASSERT(efs->argCount == args.length() - 1);
    MOZ_ASSERT(efs->exnType == type, "error-throwing intrinsic and error number are inconsistent");
#endif

    JSAutoByteString errorArgs[3];
    for (unsigned i = 1; i < 4 && i < args.length(); i++) {
        RootedValue val(cx, args[i]);
        if (val.isInt32()) {
            JSString* str = ToString<CanGC>(cx, val);
            if (!str)
                return;
            errorArgs[i - 1].encodeLatin1(cx, str);
        } else if (val.isString()) {
            errorArgs[i - 1].encodeLatin1(cx, val.toString());
        } else {
            UniquePtr<char[], JS::FreePolicy> bytes =
                DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, val, nullptr);
            if (!bytes)
                return;
            errorArgs[i - 1].initBytes(bytes.release());
        }
        if (!errorArgs[i - 1])
            return;
    }

    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, errorNumber,
                         errorArgs[0].ptr(), errorArgs[1].ptr(), errorArgs[2].ptr());
}

static bool
intrinsic_ThrowRangeError(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() >= 1);

    ThrowErrorWithType(cx, JSEXN_RANGEERR, args);
    return false;
}

static bool
intrinsic_ThrowTypeError(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() >= 1);

    ThrowErrorWithType(cx, JSEXN_TYPEERR, args);
    return false;
}

static bool
intrinsic_ThrowSyntaxError(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() >= 1);

    ThrowErrorWithType(cx, JSEXN_SYNTAXERR, args);
    return false;
}

/**
 * Handles an assertion failure in self-hosted code just like an assertion
 * failure in C++ code. Information about the failure can be provided in args[0].
 */
static bool
intrinsic_AssertionFailed(JSContext* cx, unsigned argc, Value* vp)
{
#ifdef DEBUG
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() > 0) {
        // try to dump the informative string
        JSString* str = ToString<CanGC>(cx, args[0]);
        if (str) {
            fprintf(stderr, "Self-hosted JavaScript assertion info: ");
            str->dumpCharsNoNewline();
            fputc('\n', stderr);
        }
    }
#endif
    MOZ_ASSERT(false);
    return false;
}

static bool
intrinsic_MakeConstructible(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[0].toObject().is<JSFunction>());
    MOZ_ASSERT(args[0].toObject().as<JSFunction>().isSelfHostedBuiltin());
    MOZ_ASSERT(args[1].isObject());

    // Normal .prototype properties aren't enumerable.  But for this to clone
    // correctly, it must be enumerable.
    RootedObject ctor(cx, &args[0].toObject());
    if (!DefineProperty(cx, ctor, cx->names().prototype, args[1],
                        nullptr, nullptr,
                        JSPROP_READONLY | JSPROP_ENUMERATE | JSPROP_PERMANENT))
    {
        return false;
    }

    ctor->as<JSFunction>().setIsConstructor();
    args.rval().setUndefined();
    return true;
}

/*
 * Used to decompile values in the nearest non-builtin stack frame, falling
 * back to decompiling in the current frame. Helpful for printing higher-order
 * function arguments.
 *
 * The user must supply the argument number of the value in question; it
 * _cannot_ be automatically determined.
 */
static bool
intrinsic_DecompileArg(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);

    RootedValue value(cx, args[1]);
    ScopedJSFreePtr<char> str(DecompileArgument(cx, args[0].toInt32(), value));
    if (!str)
        return false;
    RootedAtom atom(cx, Atomize(cx, str, strlen(str)));
    if (!atom)
        return false;
    args.rval().setString(atom);
    return true;
}

static bool
intrinsic_DefineDataProperty(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    MOZ_ASSERT(args.length() >= 3);
    MOZ_ASSERT(args[0].isObject());

    RootedObject obj(cx, &args[0].toObject());
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args[1], &id))
        return false;
    RootedValue value(cx, args[2]);

    unsigned attrs = 0;
    if (args.length() >= 4) {
        unsigned attributes = args[3].toInt32();

        MOZ_ASSERT(bool(attributes & ATTR_ENUMERABLE) != bool(attributes & ATTR_NONENUMERABLE),
                   "_DefineDataProperty must receive either ATTR_ENUMERABLE xor ATTR_NONENUMERABLE");
        if (attributes & ATTR_ENUMERABLE)
            attrs |= JSPROP_ENUMERATE;

        MOZ_ASSERT(bool(attributes & ATTR_CONFIGURABLE) != bool(attributes & ATTR_NONCONFIGURABLE),
                   "_DefineDataProperty must receive either ATTR_CONFIGURABLE xor "
                   "ATTR_NONCONFIGURABLE");
        if (attributes & ATTR_NONCONFIGURABLE)
            attrs |= JSPROP_PERMANENT;

        MOZ_ASSERT(bool(attributes & ATTR_WRITABLE) != bool(attributes & ATTR_NONWRITABLE),
                   "_DefineDataProperty must receive either ATTR_WRITABLE xor ATTR_NONWRITABLE");
        if (attributes & ATTR_NONWRITABLE)
            attrs |= JSPROP_READONLY;
    } else {
        // If the fourth argument is unspecified, the attributes are for a
        // plain data property.
        attrs = JSPROP_ENUMERATE;
    }

    Rooted<PropertyDescriptor> desc(cx);
    desc.setDataDescriptor(value, attrs);
    if (!DefineProperty(cx, obj, id, desc))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
intrinsic_UnsafeSetReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[1].isInt32());

    args[0].toObject().as<NativeObject>().setReservedSlot(args[1].toPrivateUint32(), args[2]);
    args.rval().setUndefined();
    return true;
}

static bool
intrinsic_UnsafeGetReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[1].isInt32());

    args.rval().set(args[0].toObject().as<NativeObject>().getReservedSlot(args[1].toPrivateUint32()));
    return true;
}

static bool
intrinsic_UnsafeGetObjectFromReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp))
        return false;
    MOZ_ASSERT(vp->isObject());
    return true;
}

static bool
intrinsic_UnsafeGetInt32FromReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp))
        return false;
    MOZ_ASSERT(vp->isInt32());
    return true;
}

static bool
intrinsic_UnsafeGetStringFromReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp))
        return false;
    MOZ_ASSERT(vp->isString());
    return true;
}

static bool
intrinsic_UnsafeGetBooleanFromReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp))
        return false;
    MOZ_ASSERT(vp->isBoolean());
    return true;
}

static bool
intrinsic_IsPackedArray(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    JSObject* obj = &args[0].toObject();
    bool isPacked = obj->is<ArrayObject>() && !obj->hasLazyGroup() &&
                    !obj->group()->hasAllFlags(OBJECT_FLAG_NON_PACKED) &&
                    obj->as<ArrayObject>().getDenseInitializedLength() ==
                        obj->as<ArrayObject>().length();

    args.rval().setBoolean(isPacked);
    return true;
}

static bool
intrinsic_GetIteratorPrototype(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    JSObject* obj = GlobalObject::getOrCreateIteratorPrototype(cx, cx->global());
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
intrinsic_NewArrayIterator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    RootedObject proto(cx, GlobalObject::getOrCreateArrayIteratorPrototype(cx, cx->global()));
    if (!proto)
        return false;

    JSObject* obj = NewObjectWithGivenProto(cx, &ArrayIteratorObject::class_, proto);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
intrinsic_GetNextMapEntryForIterator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(args[0].toObject().is<MapIteratorObject>());
    MOZ_ASSERT(args[1].isObject());

    Rooted<MapIteratorObject*> mapIterator(cx, &args[0].toObject().as<MapIteratorObject>());
    RootedArrayObject result(cx, &args[1].toObject().as<ArrayObject>());

    args.rval().setBoolean(MapIteratorObject::next(cx, mapIterator, result));
    return true;
}

static bool
intrinsic_NewStringIterator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    RootedObject proto(cx, GlobalObject::getOrCreateStringIteratorPrototype(cx, cx->global()));
    if (!proto)
        return false;

    JSObject* obj = NewObjectWithGivenProto(cx, &StringIteratorObject::class_, proto);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
intrinsic_NewListIterator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    RootedObject proto(cx, GlobalObject::getOrCreateIteratorPrototype(cx, cx->global()));
    if (!proto)
        return false;

    RootedObject iterator(cx);
    iterator = NewObjectWithGivenProto(cx, &ListIteratorObject::class_, proto);
    if (!iterator)
        return false;

    args.rval().setObject(*iterator);
    return true;
}

static bool
intrinsic_ActiveFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    ScriptFrameIter iter(cx);
    MOZ_ASSERT(iter.isFunctionFrame());
    args.rval().setObject(*iter.callee(cx));
    return true;
}

static bool
intrinsic_StarGeneratorObjectIsClosed(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    StarGeneratorObject* genObj = &args[0].toObject().as<StarGeneratorObject>();
    args.rval().setBoolean(genObj->isClosed());
    return true;
}

bool
js::intrinsic_IsSuspendedStarGenerator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    if (!args[0].isObject() || !args[0].toObject().is<StarGeneratorObject>()) {
        args.rval().setBoolean(false);
        return true;
    }

    StarGeneratorObject& genObj = args[0].toObject().as<StarGeneratorObject>();
    args.rval().setBoolean(!genObj.isClosed() && genObj.isSuspended());
    return true;
}

static bool
intrinsic_LegacyGeneratorObjectIsClosed(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    LegacyGeneratorObject* genObj = &args[0].toObject().as<LegacyGeneratorObject>();
    args.rval().setBoolean(genObj->isClosed());
    return true;
}

static bool
intrinsic_CloseClosingLegacyGeneratorObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    LegacyGeneratorObject* genObj = &args[0].toObject().as<LegacyGeneratorObject>();
    MOZ_ASSERT(genObj->isClosing());
    genObj->setClosed();
    return true;
}

static bool
intrinsic_ThrowStopIteration(JSContext* cx, unsigned argc, Value* vp)
{
    MOZ_ASSERT(CallArgsFromVp(argc, vp).length() == 0);

    return ThrowStopIteration(cx);
}

static bool
intrinsic_GeneratorIsRunning(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    GeneratorObject* genObj = &args[0].toObject().as<GeneratorObject>();
    args.rval().setBoolean(genObj->isRunning() || genObj->isClosing());
    return true;
}

static bool
intrinsic_GeneratorSetClosed(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    GeneratorObject* genObj = &args[0].toObject().as<GeneratorObject>();
    genObj->setClosed();
    return true;
}

static bool
intrinsic_IsPossiblyWrappedTypedArray(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    bool isTypedArray = false;
    if (args[0].isObject()) {
        JSObject* obj = CheckedUnwrap(&args[0].toObject());
        if (!obj) {
            JS_ReportError(cx, "Permission denied to access object");
            return false;
        }

        isTypedArray = obj->is<TypedArrayObject>();
    }

    args.rval().setBoolean(isTypedArray);
    return true;
}

static bool
intrinsic_TypedArrayBuffer(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(TypedArrayObject::is(args[0]));

    Rooted<TypedArrayObject*> tarray(cx, &args[0].toObject().as<TypedArrayObject>());
    if (!TypedArrayObject::ensureHasBuffer(cx, tarray))
        return false;

    args.rval().set(TypedArrayObject::bufferValue(tarray));
    return true;
}

static bool
intrinsic_TypedArrayByteOffset(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(TypedArrayObject::is(args[0]));

    args.rval().set(TypedArrayObject::byteOffsetValue(&args[0].toObject().as<TypedArrayObject>()));
    return true;
}

static bool
intrinsic_TypedArrayElementShift(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(TypedArrayObject::is(args[0]));

    unsigned shift = TypedArrayShift(args[0].toObject().as<TypedArrayObject>().type());
    MOZ_ASSERT(shift == 0 || shift == 1 || shift == 2 || shift == 3);

    args.rval().setInt32(mozilla::AssertedCast<int32_t>(shift));
    return true;
}

// Return the value of [[ArrayLength]] internal slot of the TypedArray
static bool
intrinsic_TypedArrayLength(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    RootedObject obj(cx, &args[0].toObject());
    MOZ_ASSERT(obj->is<TypedArrayObject>());
    args.rval().setInt32(obj->as<TypedArrayObject>().length());
    return true;
}

static bool
intrinsic_MoveTypedArrayElements(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 4);

    Rooted<TypedArrayObject*> tarray(cx, &args[0].toObject().as<TypedArrayObject>());
    uint32_t to = uint32_t(args[1].toInt32());
    uint32_t from = uint32_t(args[2].toInt32());
    uint32_t count = uint32_t(args[3].toInt32());

    MOZ_ASSERT(count > 0,
               "don't call this method if copying no elements, because then "
               "the not-neutered requirement is wrong");

    if (tarray->isNeutered() && tarray->hasBuffer()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_BAD_ARGS);
        return false;
    }

    // Don't multiply by |tarray->bytesPerElement()| in case the compiler can't
    // strength-reduce multiplication by 1/2/4/8 into the equivalent shift.
    const size_t ElementShift = TypedArrayShift(tarray->type());

    MOZ_ASSERT((UINT32_MAX >> ElementShift) > to);
    uint32_t byteDest = to << ElementShift;

    MOZ_ASSERT((UINT32_MAX >> ElementShift) > from);
    uint32_t byteSrc = from << ElementShift;

    MOZ_ASSERT((UINT32_MAX >> ElementShift) >= count);
    uint32_t byteSize = count << ElementShift;

#ifdef DEBUG
    {
        uint32_t viewByteLength = tarray->byteLength();
        MOZ_ASSERT(byteSize <= viewByteLength);
        MOZ_ASSERT(byteDest < viewByteLength);
        MOZ_ASSERT(byteSrc < viewByteLength);
        MOZ_ASSERT(byteDest <= viewByteLength - byteSize);
        MOZ_ASSERT(byteSrc <= viewByteLength - byteSize);
    }
#endif

    SharedMem<uint8_t*> data = tarray->viewDataEither().cast<uint8_t*>();
    jit::AtomicOperations::memmoveSafeWhenRacy(data + byteDest, data + byteSrc, byteSize);

    args.rval().setUndefined();
    return true;
}

// Extract the TypedArrayObject* underlying |obj| and return it.  This method,
// in a TOTALLY UNSAFE manner, completely violates the normal compartment
// boundaries, returning an object not necessarily in the current compartment
// or in |obj|'s compartment.
//
// All callers of this method are expected to sigil this TypedArrayObject*, and
// all values and information derived from it, with an "unsafe" prefix, to
// indicate the extreme caution required when dealing with such values.
//
// If calling code discipline ever fails to be maintained, it's gonna have a
// bad time.
static TypedArrayObject*
DangerouslyUnwrapTypedArray(JSContext* cx, JSObject* obj)
{
    // An unwrapped pointer to an object potentially on the other side of a
    // compartment boundary!  Isn't this such fun?
    JSObject* unwrapped = CheckedUnwrap(obj);
    if (!unwrapped->is<TypedArrayObject>()) {
        // By *appearances* this can't happen, as self-hosted TypedArraySet
        // checked this.  But.  Who's to say a GC couldn't happen between
        // the check that this value was a typed array, and this extraction
        // occurring?  A GC might turn a cross-compartment wrapper |obj| into
        // |unwrapped == obj|, a dead object no longer connected its typed
        // array.
        //
        // Yeah, yeah, it's pretty unlikely.  Are you willing to stake a
        // sec-critical bug on that assessment, now and forever, against
        // all changes those pesky GC and JIT people might make?
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
        return nullptr;
    }

    // Be super-duper careful using this, as we've just punched through
    // the compartment boundary, and things like buffer() on this aren't
    // same-compartment with anything else in the calling method.
    return &unwrapped->as<TypedArrayObject>();
}

// ES6 draft 20150403 22.2.3.22.2, steps 12-24, 29.
static bool
intrinsic_SetFromTypedArrayApproach(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 4);

    Rooted<TypedArrayObject*> target(cx, &args[0].toObject().as<TypedArrayObject>());
    MOZ_ASSERT(!target->hasBuffer() || !target->isNeutered(),
               "something should have defended against a neutered target");

    // As directed by |DangerouslyUnwrapTypedArray|, sigil this pointer and all
    // variables derived from it to counsel extreme caution here.
    Rooted<TypedArrayObject*> unsafeTypedArrayCrossCompartment(cx);
    unsafeTypedArrayCrossCompartment = DangerouslyUnwrapTypedArray(cx, &args[1].toObject());
    if (!unsafeTypedArrayCrossCompartment)
        return false;

    double doubleTargetOffset = args[2].toNumber();
    MOZ_ASSERT(doubleTargetOffset >= 0, "caller failed to ensure |targetOffset >= 0|");

    uint32_t targetLength = uint32_t(args[3].toInt32());

    // Handle all checks preceding the actual element-setting.  A visual skim
    // of 22.2.3.22.2 should confirm these are the only steps after steps 1-11
    // that might abort processing (other than for reason of internal error.)

    // Steps 12-13.
    if (unsafeTypedArrayCrossCompartment->hasBuffer() &&
        unsafeTypedArrayCrossCompartment->isNeutered())
    {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TYPED_ARRAY_DETACHED);
        return false;
    }

    // Steps 21, 23.
    uint32_t unsafeSrcLengthCrossCompartment = unsafeTypedArrayCrossCompartment->length();
    if (unsafeSrcLengthCrossCompartment + doubleTargetOffset > targetLength) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
        return false;
    }

    // Now that that's confirmed, we can use |targetOffset| of a sane type.
    uint32_t targetOffset = uint32_t(doubleTargetOffset);

    // The remaining steps are unobservable *except* through their effect on
    // which elements are copied and how.

    Scalar::Type targetType = target->type();
    Scalar::Type unsafeSrcTypeCrossCompartment = unsafeTypedArrayCrossCompartment->type();

    size_t targetElementSize = TypedArrayElemSize(targetType);
    SharedMem<uint8_t*> targetData =
        target->viewDataEither().cast<uint8_t*>() + targetOffset * targetElementSize;

    SharedMem<uint8_t*> unsafeSrcDataCrossCompartment =
        unsafeTypedArrayCrossCompartment->viewDataEither().cast<uint8_t*>();

    uint32_t unsafeSrcElementSizeCrossCompartment =
        TypedArrayElemSize(unsafeSrcTypeCrossCompartment);
    uint32_t unsafeSrcByteLengthCrossCompartment =
        unsafeSrcLengthCrossCompartment * unsafeSrcElementSizeCrossCompartment;

    // Step 29.
    //
    // The same-type case requires exact copying preserving the bit-level
    // encoding of the source data, so move the values.  (We could PodCopy if
    // we knew the buffers differed, but it's doubtful the work to check
    // wouldn't swap any minor wins PodCopy would afford.  Because of the
    // TOTALLY UNSAFE CROSS-COMPARTMENT NONSENSE here, comparing buffer
    // pointers directly could give an incorrect answer.)  If this occurs,
    // the %TypedArray%.prototype.set operation is completely finished.
    if (targetType == unsafeSrcTypeCrossCompartment) {
        jit::AtomicOperations::memmoveSafeWhenRacy(targetData,
                                                   unsafeSrcDataCrossCompartment,
                                                   unsafeSrcByteLengthCrossCompartment);
        args.rval().setInt32(JS_SETTYPEDARRAY_SAME_TYPE);
        return true;
    }

    // Every other bit of element-copying is handled by step 28.  Indicate
    // whether such copying must take care not to overlap, so that self-hosted
    // code may correctly perform the copying.

    SharedMem<uint8_t*> unsafeSrcDataLimitCrossCompartment =
        unsafeSrcDataCrossCompartment + unsafeSrcByteLengthCrossCompartment;
    SharedMem<uint8_t*> targetDataLimit =
        target->viewDataEither().cast<uint8_t*>() + targetLength * targetElementSize;

    // Step 24 test (but not steps 24a-d -- the caller handles those).
    bool overlap =
        IsInRange(targetData.unwrap(/*safe - used for ptr value*/),
                  unsafeSrcDataCrossCompartment.unwrap(/*safe - ditto*/),
                  unsafeSrcDataLimitCrossCompartment.unwrap(/*safe - ditto*/)) ||
        IsInRange(unsafeSrcDataCrossCompartment.unwrap(/*safe - ditto*/),
                  targetData.unwrap(/*safe - ditto*/),
                  targetDataLimit.unwrap(/*safe - ditto*/));

    args.rval().setInt32(overlap ? JS_SETTYPEDARRAY_OVERLAPPING : JS_SETTYPEDARRAY_DISJOINT);
    return true;
}

template <typename From, typename To>
static void
CopyValues(SharedMem<To*> dest, SharedMem<From*> src, uint32_t count)
{
#ifdef DEBUG
    void* destVoid = dest.template cast<void*>().unwrap(/*safe - used for ptr value*/);
    void* destVoidEnd = (dest + count).template cast<void*>().unwrap(/*safe - ditto*/);
    const void* srcVoid = src.template cast<void*>().unwrap(/*safe - ditto*/);
    const void* srcVoidEnd = (src + count).template cast<void*>().unwrap(/*safe - ditto*/);
    MOZ_ASSERT(!IsInRange(destVoid, srcVoid, srcVoidEnd));
    MOZ_ASSERT(!IsInRange(srcVoid, destVoid, destVoidEnd));
#endif

    using namespace jit;

    for (; count > 0; count--) {
        AtomicOperations::storeSafeWhenRacy(dest++,
                                            To(AtomicOperations::loadSafeWhenRacy(src++)));
    }
}

struct DisjointElements
{
    template <typename To>
    static void
    copy(SharedMem<To*> dest, SharedMem<void*> src, Scalar::Type fromType, uint32_t count) {
        switch (fromType) {
          case Scalar::Int8:
            CopyValues(dest, src.cast<int8_t*>(), count);
            return;

          case Scalar::Uint8:
            CopyValues(dest, src.cast<uint8_t*>(), count);
            return;

          case Scalar::Int16:
            CopyValues(dest, src.cast<int16_t*>(), count);
            return;

          case Scalar::Uint16:
            CopyValues(dest, src.cast<uint16_t*>(), count);
            return;

          case Scalar::Int32:
            CopyValues(dest, src.cast<int32_t*>(), count);
            return;

          case Scalar::Uint32:
            CopyValues(dest, src.cast<uint32_t*>(), count);
            return;

          case Scalar::Float32:
            CopyValues(dest, src.cast<float*>(), count);
            return;

          case Scalar::Float64:
            CopyValues(dest, src.cast<double*>(), count);
            return;

          case Scalar::Uint8Clamped:
            CopyValues(dest, src.cast<uint8_clamped*>(), count);
            return;

          default:
            MOZ_CRASH("NonoverlappingSet with bogus from-type");
        }
    }
};

static void
CopyToDisjointArray(TypedArrayObject* target, uint32_t targetOffset, SharedMem<void*> src,
                    Scalar::Type srcType, uint32_t count)
{
    Scalar::Type destType = target->type();
    SharedMem<uint8_t*> dest = target->viewDataEither().cast<uint8_t*>() + targetOffset * TypedArrayElemSize(destType);

    switch (destType) {
      case Scalar::Int8: {
        DisjointElements::copy(dest.cast<int8_t*>(), src, srcType, count);
        break;
      }

      case Scalar::Uint8: {
        DisjointElements::copy(dest.cast<uint8_t*>(), src, srcType, count);
        break;
      }

      case Scalar::Int16: {
        DisjointElements::copy(dest.cast<int16_t*>(), src, srcType, count);
        break;
      }

      case Scalar::Uint16: {
        DisjointElements::copy(dest.cast<uint16_t*>(), src, srcType, count);
        break;
      }

      case Scalar::Int32: {
        DisjointElements::copy(dest.cast<int32_t*>(), src, srcType, count);
        break;
      }

      case Scalar::Uint32: {
        DisjointElements::copy(dest.cast<uint32_t*>(), src, srcType, count);
        break;
      }

      case Scalar::Float32: {
        DisjointElements::copy(dest.cast<float*>(), src, srcType, count);
        break;
      }

      case Scalar::Float64: {
        DisjointElements::copy(dest.cast<double*>(), src, srcType, count);
        break;
      }

      case Scalar::Uint8Clamped: {
        DisjointElements::copy(dest.cast<uint8_clamped*>(), src, srcType, count);
        break;
      }

      default:
        MOZ_CRASH("setFromAnyTypedArray with a typed array with bogus type");
    }
}

// |unsafeSrcCrossCompartment| is produced by |DangerouslyUnwrapTypedArray|,
// counseling extreme caution when using it.  As directed by
// |DangerouslyUnwrapTypedArray|, sigil this pointer and all variables derived
// from it to counsel extreme caution here.
void
js::SetDisjointTypedElements(TypedArrayObject* target, uint32_t targetOffset,
                             TypedArrayObject* unsafeSrcCrossCompartment)
{
    Scalar::Type unsafeSrcTypeCrossCompartment = unsafeSrcCrossCompartment->type();

    SharedMem<void*> unsafeSrcDataCrossCompartment = unsafeSrcCrossCompartment->viewDataEither();
    uint32_t count = unsafeSrcCrossCompartment->length();

    CopyToDisjointArray(target, targetOffset,
                        unsafeSrcDataCrossCompartment,
                        unsafeSrcTypeCrossCompartment, count);
}

static bool
intrinsic_SetDisjointTypedElements(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);

    Rooted<TypedArrayObject*> target(cx, &args[0].toObject().as<TypedArrayObject>());
    MOZ_ASSERT(!target->hasBuffer() || !target->isNeutered(),
               "a neutered typed array has no elements to set, so "
               "it's nonsensical to be setting them");

    uint32_t targetOffset = uint32_t(args[1].toInt32());

    // As directed by |DangerouslyUnwrapTypedArray|, sigil this pointer and all
    // variables derived from it to counsel extreme caution here.
    Rooted<TypedArrayObject*> unsafeSrcCrossCompartment(cx);
    unsafeSrcCrossCompartment = DangerouslyUnwrapTypedArray(cx, &args[2].toObject());
    if (!unsafeSrcCrossCompartment)
        return false;

    SetDisjointTypedElements(target, targetOffset, unsafeSrcCrossCompartment);

    args.rval().setUndefined();
    return true;
}

static bool
intrinsic_SetOverlappingTypedElements(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);

    Rooted<TypedArrayObject*> target(cx, &args[0].toObject().as<TypedArrayObject>());
    MOZ_ASSERT(!target->hasBuffer() || !target->isNeutered(),
               "shouldn't be setting elements if neutered");

    uint32_t targetOffset = uint32_t(args[1].toInt32());

    // As directed by |DangerouslyUnwrapTypedArray|, sigil this pointer and all
    // variables derived from it to counsel extreme caution here.
    Rooted<TypedArrayObject*> unsafeSrcCrossCompartment(cx);
    unsafeSrcCrossCompartment = DangerouslyUnwrapTypedArray(cx, &args[2].toObject());
    if (!unsafeSrcCrossCompartment)
        return false;

    // Smarter algorithms exist to perform overlapping transfers of the sort
    // this method performs (for example, v8's self-hosted implementation).
    // But it seems likely deliberate overlapping transfers are rare enough
    // that it's not worth the trouble to implement one (and worry about its
    // safety/correctness!).  Make a copy and do a disjoint set from that.
    uint32_t count = unsafeSrcCrossCompartment->length();
    Scalar::Type unsafeSrcTypeCrossCompartment = unsafeSrcCrossCompartment->type();
    size_t sourceByteLen = count * TypedArrayElemSize(unsafeSrcTypeCrossCompartment);

    auto copyOfSrcData = target->zone()->make_pod_array<uint8_t>(sourceByteLen);
    if (!copyOfSrcData)
        return false;

    jit::AtomicOperations::memcpySafeWhenRacy(copyOfSrcData.get(),
                                              unsafeSrcCrossCompartment->viewDataEither().cast<uint8_t*>(),
                                              sourceByteLen);

    CopyToDisjointArray(target, targetOffset, SharedMem<void*>::unshared(copyOfSrcData.get()),
                        unsafeSrcTypeCrossCompartment, count);

    args.rval().setUndefined();
    return true;
}

bool
CallSelfHostedNonGenericMethod(JSContext* cx, const CallArgs& args)
{
    // This function is called when a self-hosted method is invoked on a
    // wrapper object, like a CrossCompartmentWrapper. The last argument is
    // the name of the self-hosted function. The other arguments are the
    // arguments to pass to this function.

    MOZ_ASSERT(args.length() > 0);
    RootedPropertyName name(cx, args[args.length() - 1].toString()->asAtom().asPropertyName());

    RootedValue selfHostedFun(cx);
    if (!GlobalObject::getIntrinsicValue(cx, cx->global(), name, &selfHostedFun))
        return false;

    MOZ_ASSERT(selfHostedFun.toObject().is<JSFunction>());

    InvokeArgs args2(cx);
    if (!args2.init(args.length() - 1))
        return false;

    args2.setCallee(selfHostedFun);
    args2.setThis(args.thisv());

    for (size_t i = 0; i < args.length() - 1; i++)
        args2[i].set(args[i]);

    if (!Invoke(cx, args2))
        return false;

    args.rval().set(args2.rval());
    return true;
}

template<typename T>
bool
Is(HandleValue v)
{
    return v.isObject() && v.toObject().is<T>();
}

template<IsAcceptableThis Test>
static bool
CallNonGenericSelfhostedMethod(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<Test, CallSelfHostedNonGenericMethod>(cx, args);
}

/**
 * Returns the default locale as a well-formed, but not necessarily canonicalized,
 * BCP-47 language tag.
 */
static bool
intrinsic_RuntimeDefaultLocale(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    const char* locale = cx->runtime()->getDefaultLocale();
    if (!locale) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_DEFAULT_LOCALE_ERROR);
        return false;
    }

    RootedString jslocale(cx, JS_NewStringCopyZ(cx, locale));
    if (!jslocale)
        return false;

    args.rval().setString(jslocale);
    return true;
}

static bool
intrinsic_LocalTZA(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0, "the LocalTZA intrinsic takes no arguments");

    args.rval().setDouble(DateTimeInfo::localTZA());
    return true;
}

static bool
intrinsic_IsConstructing(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    ScriptFrameIter iter(cx);
    bool isConstructing = iter.isConstructing();
    args.rval().setBoolean(isConstructing);
    return true;
}

static bool
intrinsic_ConstructorForTypedArray(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(IsAnyTypedArray(&args[0].toObject()));

    RootedObject object(cx, &args[0].toObject());
    JSProtoKey protoKey = StandardProtoKeyOrNull(object);
    MOZ_ASSERT(protoKey);

    // While it may seem like an invariant that in any compartment,
    // seeing a typed array object implies that the TypedArray constructor
    // for that type is initialized on the compartment's global, this is not
    // the case. When we construct a typed array given a cross-compartment
    // ArrayBuffer, we put the constructed TypedArray in the same compartment
    // as the ArrayBuffer. Since we use the prototype from the initial
    // compartment, and never call the constructor in the ArrayBuffer's
    // compartment from script, we are not guaranteed to have initialized
    // the constructor.
    RootedObject ctor(cx);
    if (!GetBuiltinConstructor(cx, protoKey, &ctor))
        return false;

    args.rval().setObject(*ctor);
    return true;
}

static bool
intrinsic_HostResolveImportedModule(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(args[0].toObject().is<ModuleObject>());
    MOZ_ASSERT(args[1].isString());

    RootedFunction moduleResolveHook(cx, cx->global()->moduleResolveHook());
    if (!moduleResolveHook) {
        JS_ReportError(cx, "Module resolve hook not set");
        return false;
    }

    RootedValue result(cx);
    if (!JS_CallFunction(cx, nullptr, moduleResolveHook, args, &result))
        return false;

    if (!result.isObject() || !result.toObject().is<ModuleObject>()) {
        JS_ReportError(cx, "Module resolve hook did not return Module object");
        return false;
    }

    args.rval().set(result);
    return true;
}

static bool
intrinsic_GetModuleEnvironment(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
    RootedModuleEnvironmentObject env(cx, module->environment());
    args.rval().setUndefined();
    if (!env) {
        args.rval().setUndefined();
        return true;
    }

    args.rval().setObject(*env);
    return true;
}

static bool
intrinsic_CreateModuleEnvironment(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
    module->createEnvironment();
    args.rval().setUndefined();
    return true;
}

static bool
intrinsic_CreateImportBinding(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 4);
    RootedModuleEnvironmentObject environment(cx, &args[0].toObject().as<ModuleEnvironmentObject>());
    RootedAtom importedName(cx, &args[1].toString()->asAtom());
    RootedModuleObject module(cx, &args[2].toObject().as<ModuleObject>());
    RootedAtom localName(cx, &args[3].toString()->asAtom());
    if (!environment->createImportBinding(cx, importedName, module, localName))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
intrinsic_CreateNamespaceBinding(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);
    RootedModuleEnvironmentObject environment(cx, &args[0].toObject().as<ModuleEnvironmentObject>());
    RootedId name(cx, AtomToId(&args[1].toString()->asAtom()));
    MOZ_ASSERT(args[2].toObject().is<ModuleNamespaceObject>());
    // The property already exists in the evironment but is not writable, so set
    // the slot directly.
    RootedShape shape(cx, environment->lookup(cx, name));
    MOZ_ASSERT(shape);
    MOZ_ASSERT(environment->getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL));
    environment->setSlot(shape->slot(), args[2]);
    args.rval().setUndefined();
    return true;
}

static bool
intrinsic_InstantiateModuleFunctionDeclarations(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
    args.rval().setUndefined();
    return ModuleObject::instantiateFunctionDeclarations(cx, module);
}

static bool
intrinsic_SetModuleEvaluated(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
    module->setEvaluated();
    args.rval().setUndefined();
    return true;
}

static bool
intrinsic_EvaluateModule(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
    return ModuleObject::evaluate(cx, module, args.rval());
}

static bool
intrinsic_NewModuleNamespace(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    RootedModuleObject module(cx, &args[0].toObject().as<ModuleObject>());
    RootedArrayObject exports(cx, &args[1].toObject().as<ArrayObject>());
    RootedObject namespace_(cx, ModuleObject::createNamespace(cx, module, exports));
    if (!namespace_)
        return false;

    args.rval().setObject(*namespace_);
    return true;
}

static bool
intrinsic_AddModuleNamespaceBinding(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 4);
    RootedModuleNamespaceObject namespace_(cx, &args[0].toObject().as<ModuleNamespaceObject>());
    RootedAtom exportedName(cx, &args[1].toString()->asAtom());
    RootedModuleObject targetModule(cx, &args[2].toObject().as<ModuleObject>());
    RootedAtom localName(cx, &args[3].toString()->asAtom());
    if (!namespace_->addBinding(cx, exportedName, targetModule, localName))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
intrinsic_ModuleNamespaceExports(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    RootedModuleNamespaceObject namespace_(cx, &args[0].toObject().as<ModuleNamespaceObject>());
    args.rval().setObject(namespace_->exports());
    return true;
}

// The self-hosting global isn't initialized with the normal set of builtins.
// Instead, individual C++-implemented functions that're required by
// self-hosted code are defined as global functions. Accessing these
// functions via a content compartment's builtins would be unsafe, because
// content script might have changed the builtins' prototypes' members.
// Installing the whole set of builtins in the self-hosting compartment, OTOH,
// would be wasteful: it increases memory usage and initialization time for
// self-hosting compartment.
//
// Additionally, a set of C++-implemented helper functions is defined on the
// self-hosting global.
static const JSFunctionSpec intrinsic_functions[] = {
    JS_INLINABLE_FN("std_Array",                 ArrayConstructor,             1,0, Array),
    JS_FN("std_Array_join",                      array_join,                   1,0),
    JS_INLINABLE_FN("std_Array_push",            array_push,                   1,0, ArrayPush),
    JS_INLINABLE_FN("std_Array_pop",             array_pop,                    0,0, ArrayPop),
    JS_INLINABLE_FN("std_Array_shift",           array_shift,                  0,0, ArrayShift),
    JS_FN("std_Array_unshift",                   array_unshift,                1,0),
    JS_INLINABLE_FN("std_Array_slice",           array_slice,                  2,0, ArraySlice),
    JS_FN("std_Array_sort",                      array_sort,                   1,0),

    JS_FN("std_Date_now",                        date_now,                     0,0),
    JS_FN("std_Date_valueOf",                    date_valueOf,                 0,0),

    JS_FN("std_Function_bind",                   fun_bind,                     1,0),
    JS_FN("std_Function_apply",                  fun_apply,                    2,0),

    JS_INLINABLE_FN("std_Math_floor",            math_floor,                   1,0, MathFloor),
    JS_INLINABLE_FN("std_Math_max",              math_max,                     2,0, MathMax),
    JS_INLINABLE_FN("std_Math_min",              math_min,                     2,0, MathMin),
    JS_INLINABLE_FN("std_Math_abs",              math_abs,                     1,0, MathAbs),
    JS_INLINABLE_FN("std_Math_imul",             math_imul,                    2,0, MathImul),
    JS_INLINABLE_FN("std_Math_log2",             math_log2,                    1,0, MathLog2),

    JS_FN("std_Map_has",                         MapObject::has,               1,0),
    JS_FN("std_Map_iterator",                    MapObject::entries,           0,0),

    JS_FN("std_Number_valueOf",                  num_valueOf,                  0,0),

    JS_INLINABLE_FN("std_Object_create",         obj_create,                   2, 0, ObjectCreate),
    JS_FN("std_Object_propertyIsEnumerable",     obj_propertyIsEnumerable,     1,0),
    JS_FN("std_Object_defineProperty",           obj_defineProperty,           3,0),
    JS_FN("std_Object_getOwnPropertyNames",      obj_getOwnPropertyNames,      1,0),
    JS_FN("std_Object_getOwnPropertyDescriptor", obj_getOwnPropertyDescriptor, 2,0),
    JS_FN("std_Object_hasOwnProperty",           obj_hasOwnProperty,           1,0),
    JS_FN("std_Object_toString",                 obj_toString,                 0,0),

    JS_FN("std_Reflect_getPrototypeOf",          Reflect_getPrototypeOf,       1,0),
    JS_FN("std_Reflect_isExtensible",            Reflect_isExtensible,         1,0),

    JS_FN("std_Set_has",                         SetObject::has,               1,0),
    JS_FN("std_Set_iterator",                    SetObject::values,            0,0),

    JS_INLINABLE_FN("std_String_fromCharCode",   str_fromCharCode,             1,0, StringFromCharCode),
    JS_INLINABLE_FN("std_String_charCodeAt",     str_charCodeAt,               1,0, StringCharCodeAt),
    JS_FN("std_String_indexOf",                  str_indexOf,                  1,0),
    JS_FN("std_String_lastIndexOf",              str_lastIndexOf,              1,0),
    JS_FN("std_String_match",                    str_match,                    1,0),
    JS_INLINABLE_FN("std_String_replace",        str_replace,                  2,0, StringReplace),
    JS_INLINABLE_FN("std_String_split",          str_split,                    2,0, StringSplit),
    JS_FN("std_String_startsWith",               str_startsWith,               1,0),
    JS_FN("std_String_toLowerCase",              str_toLowerCase,              0,0),
    JS_FN("std_String_toUpperCase",              str_toUpperCase,              0,0),

    JS_FN("std_WeakMap_has",                     WeakMap_has,                  1,0),
    JS_FN("std_WeakMap_get",                     WeakMap_get,                  2,0),
    JS_FN("std_WeakMap_set",                     WeakMap_set,                  2,0),
    JS_FN("std_WeakMap_delete",                  WeakMap_delete,               1,0),
    JS_FN("std_WeakMap_clear",                   WeakMap_clear,                0,0),

    JS_INLINABLE_FN("std_SIMD_Int32x4_extractLane",   simd_int32x4_extractLane,  2,0, SimdInt32x4),
    JS_INLINABLE_FN("std_SIMD_Float32x4_extractLane", simd_float32x4_extractLane,2,0, SimdFloat32x4),
    JS_FN("std_SIMD_Float64x2_extractLane",      simd_float64x2_extractLane,   2,0),

    // Helper funtions after this point.
    JS_INLINABLE_FN("ToObject",      intrinsic_ToObject,                1,0, IntrinsicToObject),
    JS_INLINABLE_FN("IsObject",      intrinsic_IsObject,                1,0, IntrinsicIsObject),
    JS_INLINABLE_FN("ToInteger",     intrinsic_ToInteger,               1,0, IntrinsicToInteger),
    JS_INLINABLE_FN("ToString",      intrinsic_ToString,                1,0, IntrinsicToString),
    JS_FN("ToPropertyKey",           intrinsic_ToPropertyKey,           1,0),
    JS_INLINABLE_FN("IsCallable",    intrinsic_IsCallable,              1,0, IntrinsicIsCallable),
    JS_FN("IsConstructor",           intrinsic_IsConstructor,           1,0),
    JS_FN("OwnPropertyKeys",         intrinsic_OwnPropertyKeys,         1,0),
    JS_FN("ThrowRangeError",         intrinsic_ThrowRangeError,         4,0),
    JS_FN("ThrowTypeError",          intrinsic_ThrowTypeError,          4,0),
    JS_FN("ThrowSyntaxError",        intrinsic_ThrowSyntaxError,        4,0),
    JS_FN("AssertionFailed",         intrinsic_AssertionFailed,         1,0),
    JS_FN("MakeConstructible",       intrinsic_MakeConstructible,       2,0),
    JS_FN("_ConstructorForTypedArray", intrinsic_ConstructorForTypedArray, 1,0),
    JS_FN("DecompileArg",            intrinsic_DecompileArg,            2,0),
    JS_FN("RuntimeDefaultLocale",    intrinsic_RuntimeDefaultLocale,    0,0),
    JS_FN("LocalTZA",                intrinsic_LocalTZA,                0,0),

    JS_INLINABLE_FN("_IsConstructing", intrinsic_IsConstructing,        0,0,
                    IntrinsicIsConstructing),
    JS_INLINABLE_FN("SubstringKernel", intrinsic_SubstringKernel,       3,0,
                    IntrinsicSubstringKernel),
    JS_INLINABLE_FN("_DefineDataProperty",              intrinsic_DefineDataProperty,      4,0,
                    IntrinsicDefineDataProperty),
    JS_INLINABLE_FN("UnsafeSetReservedSlot",            intrinsic_UnsafeSetReservedSlot,   3,0,
                    IntrinsicUnsafeSetReservedSlot),
    JS_INLINABLE_FN("UnsafeGetReservedSlot",            intrinsic_UnsafeGetReservedSlot,   2,0,
                    IntrinsicUnsafeGetReservedSlot),
    JS_INLINABLE_FN("UnsafeGetObjectFromReservedSlot",  intrinsic_UnsafeGetObjectFromReservedSlot, 2,0,
                    IntrinsicUnsafeGetObjectFromReservedSlot),
    JS_INLINABLE_FN("UnsafeGetInt32FromReservedSlot",   intrinsic_UnsafeGetInt32FromReservedSlot,  2,0,
                    IntrinsicUnsafeGetInt32FromReservedSlot),
    JS_INLINABLE_FN("UnsafeGetStringFromReservedSlot",  intrinsic_UnsafeGetStringFromReservedSlot, 2,0,
                    IntrinsicUnsafeGetStringFromReservedSlot),
    JS_INLINABLE_FN("UnsafeGetBooleanFromReservedSlot", intrinsic_UnsafeGetBooleanFromReservedSlot,2,0,
                    IntrinsicUnsafeGetBooleanFromReservedSlot),

    JS_FN("IsPackedArray",           intrinsic_IsPackedArray,           1,0),

    JS_FN("GetIteratorPrototype",    intrinsic_GetIteratorPrototype,    0,0),

    JS_FN("NewArrayIterator",        intrinsic_NewArrayIterator,        0,0),
    JS_FN("CallArrayIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<ArrayIteratorObject>>,      2,0),

    JS_FN("NewListIterator",         intrinsic_NewListIterator,         0,0),
    JS_FN("CallListIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<ListIteratorObject>>,       2,0),
    JS_FN("ActiveFunction",          intrinsic_ActiveFunction,          0,0),

    JS_INLINABLE_FN("IsArrayIterator",
                    intrinsic_IsInstanceOfBuiltin<ArrayIteratorObject>, 1,0,
                    IntrinsicIsArrayIterator),
    JS_INLINABLE_FN("IsMapIterator",
                    intrinsic_IsInstanceOfBuiltin<MapIteratorObject>,   1,0,
                    IntrinsicIsMapIterator),
    JS_INLINABLE_FN("IsStringIterator",
                    intrinsic_IsInstanceOfBuiltin<StringIteratorObject>, 1,0,
                    IntrinsicIsStringIterator),
    JS_INLINABLE_FN("IsListIterator",
                    intrinsic_IsInstanceOfBuiltin<ListIteratorObject>,  1,0,
                    IntrinsicIsListIterator),

    JS_FN("_GetNextMapEntryForIterator", intrinsic_GetNextMapEntryForIterator, 3,0),
    JS_FN("CallMapIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<MapIteratorObject>>,        2,0),


    JS_FN("NewStringIterator",       intrinsic_NewStringIterator,       0,0),
    JS_FN("CallStringIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<StringIteratorObject>>,     2,0),

    JS_FN("IsStarGeneratorObject",
          intrinsic_IsInstanceOfBuiltin<StarGeneratorObject>,           1,0),
    JS_FN("StarGeneratorObjectIsClosed", intrinsic_StarGeneratorObjectIsClosed, 1,0),
    JS_FN("IsSuspendedStarGenerator",intrinsic_IsSuspendedStarGenerator,1,0),

    JS_FN("IsLegacyGeneratorObject",
          intrinsic_IsInstanceOfBuiltin<LegacyGeneratorObject>,         1,0),
    JS_FN("LegacyGeneratorObjectIsClosed", intrinsic_LegacyGeneratorObjectIsClosed, 1,0),
    JS_FN("CloseClosingLegacyGeneratorObject", intrinsic_CloseClosingLegacyGeneratorObject, 1,0),
    JS_FN("ThrowStopIteration",      intrinsic_ThrowStopIteration,      0,0),

    JS_FN("GeneratorIsRunning",      intrinsic_GeneratorIsRunning,      1,0),
    JS_FN("GeneratorSetClosed",      intrinsic_GeneratorSetClosed,      1,0),

    JS_FN("IsArrayBuffer",
          intrinsic_IsInstanceOfBuiltin<ArrayBufferObject>,             1,0),
    JS_FN("IsSharedArrayBuffer",
          intrinsic_IsInstanceOfBuiltin<SharedArrayBufferObject>,       1,0),

    JS_INLINABLE_FN("IsTypedArray",
                    intrinsic_IsInstanceOfBuiltin<TypedArrayObject>,    1,0,
                    IntrinsicIsTypedArray),
    JS_INLINABLE_FN("IsPossiblyWrappedTypedArray",intrinsic_IsPossiblyWrappedTypedArray,1,0,
                    IntrinsicIsPossiblyWrappedTypedArray),

    JS_FN("TypedArrayBuffer",        intrinsic_TypedArrayBuffer,        1,0),
    JS_FN("TypedArrayByteOffset",    intrinsic_TypedArrayByteOffset,    1,0),
    JS_FN("TypedArrayElementShift",  intrinsic_TypedArrayElementShift,  1,0),

    JS_INLINABLE_FN("TypedArrayLength", intrinsic_TypedArrayLength,     1,0,
                    IntrinsicTypedArrayLength),

    JS_FN("MoveTypedArrayElements",  intrinsic_MoveTypedArrayElements,  4,0),
    JS_FN("SetFromTypedArrayApproach",intrinsic_SetFromTypedArrayApproach, 4, 0),
    JS_FN("SetOverlappingTypedElements",intrinsic_SetOverlappingTypedElements,3,0),

    JS_INLINABLE_FN("SetDisjointTypedElements",intrinsic_SetDisjointTypedElements,3,0,
                    IntrinsicSetDisjointTypedElements),

    JS_FN("CallTypedArrayMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<TypedArrayObject>>, 2, 0),

    JS_FN("CallLegacyGeneratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<LegacyGeneratorObject>>, 2, 0),
    JS_FN("CallStarGeneratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<StarGeneratorObject>>, 2, 0),

    JS_FN("IsWeakSet", intrinsic_IsInstanceOfBuiltin<WeakSetObject>, 1,0),
    JS_FN("CallWeakSetMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<WeakSetObject>>, 2, 0),

    // See builtin/TypedObject.h for descriptors of the typedobj functions.
    JS_FN("NewOpaqueTypedObject",           js::NewOpaqueTypedObject, 1, 0),
    JS_FN("NewDerivedTypedObject",          js::NewDerivedTypedObject, 3, 0),
    JS_FN("TypedObjectBuffer",              TypedObject::GetBuffer, 1, 0),
    JS_FN("TypedObjectByteOffset",          TypedObject::GetByteOffset, 1, 0),
    JS_FN("AttachTypedObject",              js::AttachTypedObject, 3, 0),
    JS_FN("TypedObjectIsAttached",          js::TypedObjectIsAttached, 1, 0),
    JS_FN("TypedObjectTypeDescr",           js::TypedObjectTypeDescr, 1, 0),
    JS_FN("ClampToUint8",                   js::ClampToUint8, 1, 0),
    JS_FN("GetTypedObjectModule",           js::GetTypedObjectModule, 0, 0),
    JS_FN("GetFloat32x4TypeDescr",          js::GetFloat32x4TypeDescr, 0, 0),
    JS_FN("GetFloat64x2TypeDescr",          js::GetFloat64x2TypeDescr, 0, 0),
    JS_FN("GetInt8x16TypeDescr",            js::GetInt8x16TypeDescr, 0, 0),
    JS_FN("GetInt16x8TypeDescr",            js::GetInt16x8TypeDescr, 0, 0),
    JS_FN("GetInt32x4TypeDescr",            js::GetInt32x4TypeDescr, 0, 0),

    JS_INLINABLE_FN("ObjectIsTypeDescr"    ,          js::ObjectIsTypeDescr, 1, 0,
                    IntrinsicObjectIsTypeDescr),
    JS_INLINABLE_FN("ObjectIsTypedObject",            js::ObjectIsTypedObject, 1, 0,
                    IntrinsicObjectIsTypedObject),
    JS_INLINABLE_FN("ObjectIsOpaqueTypedObject",      js::ObjectIsOpaqueTypedObject, 1, 0,
                    IntrinsicObjectIsOpaqueTypedObject),
    JS_INLINABLE_FN("ObjectIsTransparentTypedObject", js::ObjectIsTransparentTypedObject, 1, 0,
                    IntrinsicObjectIsTransparentTypedObject),
    JS_INLINABLE_FN("TypeDescrIsArrayType",           js::TypeDescrIsArrayType, 1, 0,
                    IntrinsicTypeDescrIsArrayType),
    JS_INLINABLE_FN("TypeDescrIsSimpleType",          js::TypeDescrIsSimpleType, 1, 0,
                    IntrinsicTypeDescrIsSimpleType),
    JS_INLINABLE_FN("SetTypedObjectOffset",           js::SetTypedObjectOffset, 2, 0,
                    IntrinsicSetTypedObjectOffset),

#define LOAD_AND_STORE_SCALAR_FN_DECLS(_constant, _type, _name)         \
    JS_FN("Store_" #_name, js::StoreScalar##_type::Func, 3, 0),         \
    JS_FN("Load_" #_name,  js::LoadScalar##_type::Func, 3, 0),
    JS_FOR_EACH_UNIQUE_SCALAR_TYPE_REPR_CTYPE(LOAD_AND_STORE_SCALAR_FN_DECLS)
#undef LOAD_AND_STORE_SCALAR_FN_DECLS

#define LOAD_AND_STORE_REFERENCE_FN_DECLS(_constant, _type, _name)      \
    JS_FN("Store_" #_name, js::StoreReference##_type::Func, 3, 0),      \
    JS_FN("Load_" #_name,  js::LoadReference##_type::Func, 3, 0),
    JS_FOR_EACH_REFERENCE_TYPE_REPR(LOAD_AND_STORE_REFERENCE_FN_DECLS)
#undef LOAD_AND_STORE_REFERENCE_FN_DECLS

    // See builtin/Intl.h for descriptions of the intl_* functions.
    JS_FN("intl_availableCalendars", intl_availableCalendars, 1,0),
    JS_FN("intl_availableCollations", intl_availableCollations, 1,0),
    JS_FN("intl_Collator", intl_Collator, 2,0),
    JS_FN("intl_Collator_availableLocales", intl_Collator_availableLocales, 0,0),
    JS_FN("intl_CompareStrings", intl_CompareStrings, 3,0),
    JS_FN("intl_DateTimeFormat", intl_DateTimeFormat, 2,0),
    JS_FN("intl_DateTimeFormat_availableLocales", intl_DateTimeFormat_availableLocales, 0,0),
    JS_FN("intl_FormatDateTime", intl_FormatDateTime, 2,0),
    JS_FN("intl_FormatNumber", intl_FormatNumber, 2,0),
    JS_FN("intl_NumberFormat", intl_NumberFormat, 2,0),
    JS_FN("intl_NumberFormat_availableLocales", intl_NumberFormat_availableLocales, 0,0),
    JS_FN("intl_numberingSystem", intl_numberingSystem, 1,0),
    JS_FN("intl_patternForSkeleton", intl_patternForSkeleton, 2,0),

    // See builtin/RegExp.h for descriptions of the regexp_* functions.
    JS_FN("regexp_exec_no_statics", regexp_exec_no_statics, 2,0),
    JS_FN("regexp_test_no_statics", regexp_test_no_statics, 2,0),
    JS_FN("regexp_construct_no_statics", regexp_construct_no_statics, 2,0),

    JS_FN("IsModule", intrinsic_IsInstanceOfBuiltin<ModuleObject>, 1, 0),
    JS_FN("CallModuleMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<ModuleObject>>, 2, 0),
    JS_FN("HostResolveImportedModule", intrinsic_HostResolveImportedModule, 2, 0),
    JS_FN("GetModuleEnvironment", intrinsic_GetModuleEnvironment, 1, 0),
    JS_FN("CreateModuleEnvironment", intrinsic_CreateModuleEnvironment, 1, 0),
    JS_FN("CreateImportBinding", intrinsic_CreateImportBinding, 4, 0),
    JS_FN("CreateNamespaceBinding", intrinsic_CreateNamespaceBinding, 3, 0),
    JS_FN("InstantiateModuleFunctionDeclarations",
          intrinsic_InstantiateModuleFunctionDeclarations, 1, 0),
    JS_FN("SetModuleEvaluated", intrinsic_SetModuleEvaluated, 1, 0),
    JS_FN("EvaluateModule", intrinsic_EvaluateModule, 1, 0),
    JS_FN("IsModuleNamespace", intrinsic_IsInstanceOfBuiltin<ModuleNamespaceObject>, 1, 0),
    JS_FN("NewModuleNamespace", intrinsic_NewModuleNamespace, 2, 0),
    JS_FN("AddModuleNamespaceBinding", intrinsic_AddModuleNamespaceBinding, 4, 0),
    JS_FN("ModuleNamespaceExports", intrinsic_ModuleNamespaceExports, 1, 0),

    JS_FS_END
};

void
js::FillSelfHostingCompileOptions(CompileOptions& options)
{
    /*
     * In self-hosting mode, scripts use JSOP_GETINTRINSIC instead of
     * JSOP_GETNAME or JSOP_GETGNAME to access unbound variables.
     * JSOP_GETINTRINSIC does a name lookup on a special object, whose
     * properties are filled in lazily upon first access for a given global.
     *
     * As that object is inaccessible to client code, the lookups are
     * guaranteed to return the original objects, ensuring safe implementation
     * of self-hosted builtins.
     *
     * Additionally, the special syntax callFunction(fun, receiver, ...args)
     * is supported, for which bytecode is emitted that invokes |fun| with
     * |receiver| as the this-object and ...args as the arguments.
     */
    options.setIntroductionType("self-hosted");
    options.setFileAndLine("self-hosted", 1);
    options.setSelfHostingMode(true);
    options.setCanLazilyParse(false);
    options.setVersion(JSVERSION_LATEST);
    options.werrorOption = true;
    options.strictOption = true;

#ifdef DEBUG
    options.extraWarningsOption = true;
#endif
}

GlobalObject*
JSRuntime::createSelfHostingGlobal(JSContext* cx)
{
    MOZ_ASSERT(!cx->isExceptionPending());
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(cx->compartment()));

    JS::CompartmentOptions options;
    options.setDiscardSource(true);
    options.setZone(JS::FreshZone);

    JSCompartment* compartment = NewCompartment(cx, nullptr, nullptr, options);
    if (!compartment)
        return nullptr;

    static const Class shgClass = {
        "self-hosting-global", JSCLASS_GLOBAL_FLAGS,
        nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        JS_GlobalObjectTraceHook
    };

    AutoCompartment ac(cx, compartment);
    Rooted<GlobalObject*> shg(cx, GlobalObject::createInternal(cx, &shgClass));
    if (!shg)
        return nullptr;

    cx->runtime()->selfHostingGlobal_ = shg;
    compartment->isSelfHosting = true;
    compartment->setIsSystem(true);

    if (!GlobalObject::initSelfHostingBuiltins(cx, shg, intrinsic_functions))
        return nullptr;

    JS_FireOnNewGlobalObject(cx, shg);

    return shg;
}

bool
JSRuntime::initSelfHosting(JSContext* cx)
{
    MOZ_ASSERT(!selfHostingGlobal_);

    if (cx->runtime()->parentRuntime) {
        selfHostingGlobal_ = cx->runtime()->parentRuntime->selfHostingGlobal_;
        return true;
    }

    /*
     * Self hosted state can be accessed from threads for other runtimes
     * parented to this one, so cannot include state in the nursery.
     */
    JS::AutoDisableGenerationalGC disable(cx->runtime());

    Rooted<GlobalObject*> shg(cx, JSRuntime::createSelfHostingGlobal(cx));
    if (!shg)
        return false;

    JSAutoCompartment ac(cx, shg);

    CompileOptions options(cx);
    FillSelfHostingCompileOptions(options);

    /*
     * Set a temporary error reporter printing to stderr because it is too
     * early in the startup process for any other reporter to be registered
     * and we don't want errors in self-hosted code to be silently swallowed.
     */
    JSErrorReporter oldReporter = JS_SetErrorReporter(cx->runtime(), selfHosting_ErrorReporter);
    RootedValue rv(cx);
    bool ok = true;

    char* filename = getenv("MOZ_SELFHOSTEDJS");
    if (filename) {
        RootedScript script(cx);
        if (Compile(cx, options, filename, &script))
            ok = Execute(cx, script, *shg.get(), rv.address());
    } else {
        uint32_t srcLen = GetRawScriptsSize();

        const unsigned char* compressed = compressedSources;
        uint32_t compressedLen = GetCompressedSize();
        ScopedJSFreePtr<char> src(selfHostingGlobal_->zone()->pod_malloc<char>(srcLen));
        if (!src || !DecompressString(compressed, compressedLen,
                                      reinterpret_cast<unsigned char*>(src.get()), srcLen))
        {
            ok = false;
        }

        ok = ok && Evaluate(cx, options, src, srcLen, &rv);
    }
    JS_SetErrorReporter(cx->runtime(), oldReporter);
    return ok;
}

void
JSRuntime::finishSelfHosting()
{
    selfHostingGlobal_ = nullptr;
}

void
JSRuntime::markSelfHostingGlobal(JSTracer* trc)
{
    if (selfHostingGlobal_ && !parentRuntime)
        TraceRoot(trc, &selfHostingGlobal_, "self-hosting global");
}

bool
JSRuntime::isSelfHostingCompartment(JSCompartment* comp) const
{
    return selfHostingGlobal_->compartment() == comp;
}

bool
JSRuntime::isSelfHostingZone(const JS::Zone* zone) const
{
    return selfHostingGlobal_ && selfHostingGlobal_->zoneFromAnyThread() == zone;
}

static bool
CloneValue(JSContext* cx, HandleValue selfHostedValue, MutableHandleValue vp);

static bool
GetUnclonedValue(JSContext* cx, HandleNativeObject selfHostedObject,
                 HandleId id, MutableHandleValue vp)
{
    vp.setUndefined();

    if (JSID_IS_INT(id)) {
        size_t index = JSID_TO_INT(id);
        if (index < selfHostedObject->getDenseInitializedLength() &&
            !selfHostedObject->getDenseElement(index).isMagic(JS_ELEMENTS_HOLE))
        {
            vp.set(selfHostedObject->getDenseElement(JSID_TO_INT(id)));
            return true;
        }
    }

    // Since all atoms used by self hosting are marked as permanent, any
    // attempt to look up a non-permanent atom will fail. We should only
    // see such atoms when code is looking for properties on the self
    // hosted global which aren't present.
    if (JSID_IS_STRING(id) && !JSID_TO_STRING(id)->isPermanentAtom()) {
        MOZ_ASSERT(selfHostedObject->is<GlobalObject>());
        RootedValue value(cx, IdToValue(id));
        return ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_NO_SUCH_SELF_HOSTED_PROP,
                                     JSDVG_IGNORE_STACK, value, nullptr, nullptr, nullptr);
    }

    RootedShape shape(cx, selfHostedObject->lookupPure(id));
    if (!shape) {
        RootedValue value(cx, IdToValue(id));
        return ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_NO_SUCH_SELF_HOSTED_PROP,
                                     JSDVG_IGNORE_STACK, value, nullptr, nullptr, nullptr);
    }

    MOZ_ASSERT(shape->hasSlot() && shape->hasDefaultGetter());
    vp.set(selfHostedObject->getSlot(shape->slot()));
    return true;
}

static bool
CloneProperties(JSContext* cx, HandleNativeObject selfHostedObject, HandleObject clone)
{
    AutoIdVector ids(cx);
    Vector<uint8_t, 16> attrs(cx);

    for (size_t i = 0; i < selfHostedObject->getDenseInitializedLength(); i++) {
        if (!selfHostedObject->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE)) {
            if (!ids.append(INT_TO_JSID(i)))
                return false;
            if (!attrs.append(JSPROP_ENUMERATE))
                return false;
        }
    }

    Rooted<ShapeVector> shapes(cx, ShapeVector(cx));
    for (Shape::Range<NoGC> range(selfHostedObject->lastProperty()); !range.empty(); range.popFront()) {
        Shape& shape = range.front();
        if (shape.enumerable() && !shapes.append(&shape))
            return false;
    }

    // Now our shapes are in last-to-first order, so....
    Reverse(shapes.begin(), shapes.end());
    for (size_t i = 0; i < shapes.length(); ++i) {
        MOZ_ASSERT(!shapes[i]->isAccessorShape(),
                   "Can't handle cloning accessors here yet.");
        if (!ids.append(shapes[i]->propid()))
            return false;
        uint8_t shapeAttrs =
            shapes[i]->attributes() & (JSPROP_ENUMERATE | JSPROP_PERMANENT | JSPROP_READONLY);
        if (!attrs.append(shapeAttrs))
            return false;
    }

    RootedId id(cx);
    RootedValue val(cx);
    RootedValue selfHostedValue(cx);
    for (uint32_t i = 0; i < ids.length(); i++) {
        id = ids[i];
        if (!GetUnclonedValue(cx, selfHostedObject, id, &selfHostedValue))
            return false;
        if (!CloneValue(cx, selfHostedValue, &val) ||
            !JS_DefinePropertyById(cx, clone, id, val, attrs[i]))
        {
            return false;
        }
    }

    return true;
}

static JSString*
CloneString(JSContext* cx, JSFlatString* selfHostedString)
{
    size_t len = selfHostedString->length();
    {
        JS::AutoCheckCannotGC nogc;
        JSString* clone;
        if (selfHostedString->hasLatin1Chars())
            clone = NewStringCopyN<NoGC>(cx, selfHostedString->latin1Chars(nogc), len);
        else
            clone = NewStringCopyNDontDeflate<NoGC>(cx, selfHostedString->twoByteChars(nogc), len);
        if (clone)
            return clone;
    }

    AutoStableStringChars chars(cx);
    if (!chars.init(cx, selfHostedString))
        return nullptr;

    return chars.isLatin1()
           ? NewStringCopyN<CanGC>(cx, chars.latin1Range().start().get(), len)
           : NewStringCopyNDontDeflate<CanGC>(cx, chars.twoByteRange().start().get(), len);
}

static JSObject*
CloneObject(JSContext* cx, HandleNativeObject selfHostedObject)
{
#ifdef DEBUG
    // Object hash identities are owned by the hashed object, which may be on a
    // different thread than the clone target. In theory, these objects are all
    // tenured and will not be compacted; however, we simply avoid the issue
    // altogether by skipping the cycle-detection when off the main thread.
    mozilla::Maybe<AutoCycleDetector> detect;
    if (js::CurrentThreadCanAccessZone(selfHostedObject->zoneFromAnyThread())) {
        detect.emplace(cx, selfHostedObject);
        if (!detect->init())
            return nullptr;
        if (detect->foundCycle())
            MOZ_CRASH("SelfHosted cloning cannot handle cyclic object graphs.");
    }
#endif

    RootedObject clone(cx);
    if (selfHostedObject->is<JSFunction>()) {
        RootedFunction selfHostedFunction(cx, &selfHostedObject->as<JSFunction>());
        bool hasName = selfHostedFunction->atom() != nullptr;
        // Arrow functions use the first extended slot for their lexical |this| value.
        MOZ_ASSERT(!selfHostedFunction->isArrow());
        js::gc::AllocKind kind = hasName
                                 ? gc::AllocKind::FUNCTION_EXTENDED
                                 : selfHostedFunction->getAllocKind();
        MOZ_ASSERT(!CanReuseScriptForClone(cx->compartment(), selfHostedFunction, cx->global()));
        Rooted<ClonedBlockObject*> globalLexical(cx, &cx->global()->lexicalScope());
        RootedObject staticGlobalLexical(cx, &globalLexical->staticBlock());
        clone = CloneFunctionAndScript(cx, selfHostedFunction, globalLexical,
                                       staticGlobalLexical, kind);
        // To be able to re-lazify the cloned function, its name in the
        // self-hosting compartment has to be stored on the clone.
        if (clone && hasName)
            clone->as<JSFunction>().setExtendedSlot(0, StringValue(selfHostedFunction->atom()));
    } else if (selfHostedObject->is<RegExpObject>()) {
        RegExpObject& reobj = selfHostedObject->as<RegExpObject>();
        RootedAtom source(cx, reobj.getSource());
        MOZ_ASSERT(source->isPermanentAtom());
        clone = RegExpObject::createNoStatics(cx, source, reobj.getFlags(), nullptr, cx->tempLifoAlloc());
    } else if (selfHostedObject->is<DateObject>()) {
        clone = JS::NewDateObject(cx, selfHostedObject->as<DateObject>().clippedTime());
    } else if (selfHostedObject->is<BooleanObject>()) {
        clone = BooleanObject::create(cx, selfHostedObject->as<BooleanObject>().unbox());
    } else if (selfHostedObject->is<NumberObject>()) {
        clone = NumberObject::create(cx, selfHostedObject->as<NumberObject>().unbox());
    } else if (selfHostedObject->is<StringObject>()) {
        JSString* selfHostedString = selfHostedObject->as<StringObject>().unbox();
        if (!selfHostedString->isFlat())
            MOZ_CRASH();
        RootedString str(cx, CloneString(cx, &selfHostedString->asFlat()));
        if (!str)
            return nullptr;
        clone = StringObject::create(cx, str);
    } else if (selfHostedObject->is<ArrayObject>()) {
        clone = NewDenseEmptyArray(cx, nullptr, TenuredObject);
    } else {
        MOZ_ASSERT(selfHostedObject->isNative());
        clone = NewObjectWithGivenProto(cx, selfHostedObject->getClass(), nullptr,
                                        selfHostedObject->asTenured().getAllocKind(),
                                        SingletonObject);
    }
    if (!clone)
        return nullptr;
    if (!CloneProperties(cx, selfHostedObject, clone))
        return nullptr;
    return clone;
}

static bool
CloneValue(JSContext* cx, HandleValue selfHostedValue, MutableHandleValue vp)
{
    if (selfHostedValue.isObject()) {
        RootedNativeObject selfHostedObject(cx, &selfHostedValue.toObject().as<NativeObject>());
        JSObject* clone = CloneObject(cx, selfHostedObject);
        if (!clone)
            return false;
        vp.setObject(*clone);
    } else if (selfHostedValue.isBoolean() || selfHostedValue.isNumber() || selfHostedValue.isNullOrUndefined()) {
        // Nothing to do here: these are represented inline in the value.
        vp.set(selfHostedValue);
    } else if (selfHostedValue.isString()) {
        if (!selfHostedValue.toString()->isFlat())
            MOZ_CRASH();
        JSFlatString* selfHostedString = &selfHostedValue.toString()->asFlat();
        JSString* clone = CloneString(cx, selfHostedString);
        if (!clone)
            return false;
        vp.setString(clone);
    } else if (selfHostedValue.isSymbol()) {
        // Well-known symbols are shared.
        mozilla::DebugOnly<JS::Symbol*> sym = selfHostedValue.toSymbol();
        MOZ_ASSERT(sym->isWellKnownSymbol());
        MOZ_ASSERT(cx->wellKnownSymbols().get(size_t(sym->code())) == sym);
        vp.set(selfHostedValue);
    } else {
        MOZ_CRASH("Self-hosting CloneValue can't clone given value.");
    }
    return true;
}

bool
JSRuntime::cloneSelfHostedFunctionScript(JSContext* cx, HandlePropertyName name,
                                         HandleFunction targetFun)
{
    RootedId id(cx, NameToId(name));
    RootedValue funVal(cx);
    if (!GetUnclonedValue(cx, HandleNativeObject::fromMarkedLocation(&selfHostingGlobal_), id, &funVal))
        return false;

    RootedFunction sourceFun(cx, &funVal.toObject().as<JSFunction>());
    // JSFunction::generatorKind can't handle lazy self-hosted functions, so we make sure there
    // aren't any.
    MOZ_ASSERT(!sourceFun->isGenerator());
    MOZ_ASSERT(sourceFun->nargs() == targetFun->nargs());
    MOZ_ASSERT(targetFun->isExtended());
    MOZ_ASSERT(targetFun->isInterpretedLazy());
    MOZ_ASSERT(targetFun->isSelfHostedBuiltin());

    RootedScript sourceScript(cx, sourceFun->getOrCreateScript(cx));
    if (!sourceScript)
        return false;

    // Assert that there are no intervening scopes between the global scope
    // and the self-hosted script. Toplevel lexicals are explicitly forbidden
    // by the parser when parsing self-hosted code. The fact they have the
    // global lexical scope on the scope chain is for uniformity and engine
    // invariants.
    MOZ_ASSERT(IsStaticGlobalLexicalScope(sourceScript->enclosingStaticScope()));
    Rooted<ScopeObject*> enclosingScope(cx, &cx->global()->lexicalScope().staticBlock());
    if (!CloneScriptIntoFunction(cx, enclosingScope, targetFun, sourceScript))
        return false;
    MOZ_ASSERT(!targetFun->isInterpretedLazy());

    // The target function might have been relazified after its flags changed.
    targetFun->setFlags(targetFun->flags() | sourceFun->flags());
    return true;
}

bool
JSRuntime::cloneSelfHostedValue(JSContext* cx, HandlePropertyName name, MutableHandleValue vp)
{
    RootedId id(cx, NameToId(name));
    RootedValue selfHostedValue(cx);
    if (!GetUnclonedValue(cx, HandleNativeObject::fromMarkedLocation(&selfHostingGlobal_), id, &selfHostedValue))
        return false;

    /*
     * We don't clone if we're operating in the self-hosting global, as that
     * means we're currently executing the self-hosting script while
     * initializing the runtime (see JSRuntime::initSelfHosting).
     */
    if (cx->global() == selfHostingGlobal_) {
        vp.set(selfHostedValue);
        return true;
    }

    return CloneValue(cx, selfHostedValue, vp);
}

JSFunction*
js::SelfHostedFunction(JSContext* cx, HandlePropertyName propName)
{
    RootedValue func(cx);
    if (!GlobalObject::getIntrinsicValue(cx, cx->global(), propName, &func))
        return nullptr;

    MOZ_ASSERT(func.isObject());
    MOZ_ASSERT(func.toObject().is<JSFunction>());
    return &func.toObject().as<JSFunction>();
}

bool
js::IsSelfHostedFunctionWithName(JSFunction* fun, JSAtom* name)
{
    return fun->isSelfHostedBuiltin() &&
           fun->getExtendedSlot(LAZY_FUNCTION_NAME_SLOT).toString() == name;
}

static_assert(JSString::MAX_LENGTH <= INT32_MAX,
              "StringIteratorNext in builtin/String.js assumes the stored index "
              "into the string is an Int32Value");
