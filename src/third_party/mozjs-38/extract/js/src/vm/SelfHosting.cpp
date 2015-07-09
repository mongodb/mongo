/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SelfHosting.h"

#include "mozilla/DebugOnly.h"

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsdate.h"
#include "jsfriendapi.h"
#include "jshashutil.h"
#include "jsweakmap.h"
#include "jswrapper.h"
#include "selfhosted.out.h"

#include "builtin/Intl.h"
#include "builtin/Object.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/TypedObject.h"
#include "builtin/WeakSetObject.h"
#include "gc/Marking.h"
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

static void
selfHosting_ErrorReporter(JSContext* cx, const char* message, JSErrorReport* report)
{
    PrintError(cx, stderr, message, report, true);
}

bool
js::intrinsic_ToObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedValue val(cx, args[0]);
    RootedObject obj(cx, ToObject(cx, val));
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

bool
js::intrinsic_IsObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    Value val = args[0];
    bool isObject = val.isObject();
    args.rval().setBoolean(isObject);
    return true;
}

bool
js::intrinsic_ToInteger(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    double result;
    if (!ToInteger(cx, args[0], &result))
        return false;
    args.rval().setNumber(result);
    return true;
}

bool
js::intrinsic_ToString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedString str(cx);
    str = ToString<CanGC>(cx, args[0]);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

bool
intrinsic_ToPropertyKey(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args[0], &id))
        return false;

    args.rval().set(IdToValue(id));
    return true;
}

bool
js::intrinsic_IsCallable(JSContext* cx, unsigned argc, Value* vp)
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

bool
js::intrinsic_SubstringKernel(JSContext* cx, unsigned argc, Value* vp)
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
    return GetOwnPropertyKeys(cx, args,
                              JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS);
}

bool
js::intrinsic_ThrowError(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() >= 1);
    uint32_t errorNumber = args[0].toInt32();

#ifdef DEBUG
    const JSErrorFormatString* efs = js_GetErrorMessage(nullptr, errorNumber);
    MOZ_ASSERT(efs->argCount == args.length() - 1);
#endif

    JSAutoByteString errorArgs[3];
    for (unsigned i = 1; i < 4 && i < args.length(); i++) {
        RootedValue val(cx, args[i]);
        if (val.isInt32()) {
            JSString* str = ToString<CanGC>(cx, val);
            if (!str)
                return false;
            errorArgs[i - 1].encodeLatin1(cx, str);
        } else if (val.isString()) {
            errorArgs[i - 1].encodeLatin1(cx, val.toString());
        } else {
            errorArgs[i - 1].initBytes(DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, val, NullPtr()));
        }
        if (!errorArgs[i - 1])
            return false;
    }

    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, errorNumber,
                         errorArgs[0].ptr(), errorArgs[1].ptr(), errorArgs[2].ptr());
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

    ctor->as<JSFunction>().setIsSelfHostedConstructor();
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

/*
 * NewDenseArray(length): Allocates and returns a new dense array with
 * the given length where all values are initialized to holes.
 */
bool
js::intrinsic_NewDenseArray(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Check that index is an int32
    if (!args[0].isInt32()) {
        JS_ReportError(cx, "Expected int32 as second argument");
        return false;
    }
    uint32_t length = args[0].toInt32();

    // Make a new buffer and initialize it up to length.
    RootedArrayObject buffer(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!buffer)
        return false;

    ObjectGroup* newgroup = ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array);
    if (!newgroup)
        return false;
    buffer->setGroup(newgroup);

    NativeObject::EnsureDenseResult edr = buffer->ensureDenseElements(cx, length, 0);
    switch (edr) {
      case NativeObject::ED_OK:
        args.rval().setObject(*buffer);
        return true;

      case NativeObject::ED_SPARSE: // shouldn't happen!
        MOZ_ASSERT(!"%EnsureDenseArrayElements() would yield sparse array");
        JS_ReportError(cx, "%EnsureDenseArrayElements() would yield sparse array");
        break;

      case NativeObject::ED_FAILED:
        break;
    }
    return false;
}

/*
 * UnsafePutElements(arr0, idx0, elem0, ..., arrN, idxN, elemN): For each set of
 * (arr, idx, elem) arguments that are passed, performs the assignment
 * |arr[idx] = elem|. |arr| must be either a dense array or a typed array.
 *
 * If |arr| is a dense array, the index must be an int32 less than the
 * initialized length of |arr|. Use |%EnsureDenseResultArrayElements|
 * to ensure that the initialized length is long enough.
 *
 * If |arr| is a typed array, the index must be an int32 less than the
 * length of |arr|.
 */
bool
js::intrinsic_UnsafePutElements(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if ((args.length() % 3) != 0) {
        JS_ReportError(cx, "Incorrect number of arguments, not divisible by 3");
        return false;
    }

    for (uint32_t base = 0; base < args.length(); base += 3) {
        uint32_t arri = base;
        uint32_t idxi = base+1;
        uint32_t elemi = base+2;

        MOZ_ASSERT(args[arri].isObject());
        MOZ_ASSERT(args[arri].toObject().isNative() || IsTypedObjectArray(args[arri].toObject()));
        MOZ_ASSERT(args[idxi].isInt32());

        RootedObject arrobj(cx, &args[arri].toObject());
        uint32_t idx = args[idxi].toInt32();

        if (IsAnyTypedArray(arrobj.get()) || arrobj->is<TypedObject>()) {
            MOZ_ASSERT_IF(IsAnyTypedArray(arrobj.get()), idx < AnyTypedArrayLength(arrobj.get()));
            MOZ_ASSERT_IF(arrobj->is<TypedObject>(), idx < uint32_t(arrobj->as<TypedObject>().length()));
            RootedValue tmp(cx, args[elemi]);
            // XXX: Always non-strict.
            if (!SetElement(cx, arrobj, arrobj, idx, &tmp, false))
                return false;
        } else {
            MOZ_ASSERT(idx < arrobj->as<ArrayObject>().getDenseInitializedLength());
            arrobj->as<ArrayObject>().setDenseElementWithType(cx, idx, args[elemi]);
        }
    }

    args.rval().setUndefined();
    return true;
}

bool
js::intrinsic_DefineDataProperty(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    MOZ_ASSERT(args.length() == 4);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[3].isInt32());

    RootedObject obj(cx, &args[0].toObject());
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args[1], &id))
        return false;
    RootedValue value(cx, args[2]);
    unsigned attributes = args[3].toInt32();

    Rooted<PropDesc> desc(cx);

    MOZ_ASSERT(bool(attributes & ATTR_ENUMERABLE) != bool(attributes & ATTR_NONENUMERABLE),
               "_DefineDataProperty must receive either ATTR_ENUMERABLE xor ATTR_NONENUMERABLE");
    PropDesc::Enumerability enumerable =
        PropDesc::Enumerability(bool(attributes & ATTR_ENUMERABLE));

    MOZ_ASSERT(bool(attributes & ATTR_CONFIGURABLE) != bool(attributes & ATTR_NONCONFIGURABLE),
               "_DefineDataProperty must receive either ATTR_CONFIGURABLE xor "
               "ATTR_NONCONFIGURABLE");
    PropDesc::Configurability configurable =
        PropDesc::Configurability(bool(attributes & ATTR_CONFIGURABLE));

    MOZ_ASSERT(bool(attributes & ATTR_WRITABLE) != bool(attributes & ATTR_NONWRITABLE),
               "_DefineDataProperty must receive either ATTR_WRITABLE xor ATTR_NONWRITABLE");
    PropDesc::Writability writable =
        PropDesc::Writability(bool(attributes & ATTR_WRITABLE));

    desc = PropDesc(value, writable, enumerable, configurable);

    bool result;
    return StandardDefineProperty(cx, obj, id, desc, true, &result);
}

bool
js::intrinsic_UnsafeSetReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[1].isInt32());

    args[0].toObject().as<NativeObject>().setReservedSlot(args[1].toPrivateUint32(), args[2]);
    args.rval().setUndefined();
    return true;
}

bool
js::intrinsic_UnsafeGetReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[1].isInt32());

    args.rval().set(args[0].toObject().as<NativeObject>().getReservedSlot(args[1].toPrivateUint32()));
    return true;
}

bool
js::intrinsic_UnsafeGetObjectFromReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp))
        return false;
    MOZ_ASSERT(vp->isObject());
    return true;
}

bool
js::intrinsic_UnsafeGetInt32FromReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp))
        return false;
    MOZ_ASSERT(vp->isInt32());
    return true;
}

bool
js::intrinsic_UnsafeGetStringFromReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp))
        return false;
    MOZ_ASSERT(vp->isString());
    return true;
}

bool
js::intrinsic_UnsafeGetBooleanFromReservedSlot(JSContext* cx, unsigned argc, Value* vp)
{
    if (!intrinsic_UnsafeGetReservedSlot(cx, argc, vp))
        return false;
    MOZ_ASSERT(vp->isBoolean());
    return true;
}

bool
js::intrinsic_IsPackedArray(JSContext* cx, unsigned argc, Value* vp)
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

    JSObject* obj = NewObjectWithGivenProto(cx, &ArrayIteratorObject::class_, proto, cx->global());
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

bool
js::intrinsic_IsArrayIterator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    args.rval().setBoolean(args[0].toObject().is<ArrayIteratorObject>());
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

    JSObject* obj = NewObjectWithGivenProto(cx, &StringIteratorObject::class_, proto, cx->global());
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

bool
js::intrinsic_IsStringIterator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    args.rval().setBoolean(args[0].toObject().is<StringIteratorObject>());
    return true;
}

static bool
intrinsic_IsStarGeneratorObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    args.rval().setBoolean(args[0].toObject().is<StarGeneratorObject>());
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
intrinsic_IsLegacyGeneratorObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    args.rval().setBoolean(args[0].toObject().is<LegacyGeneratorObject>());
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

bool
js::intrinsic_IsTypedArray(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    RootedObject obj(cx, &args[0].toObject());
    args.rval().setBoolean(obj->is<TypedArrayObject>());
    return true;
}

// Return the value of [[ArrayLength]] internal slot of the TypedArray
bool
js::intrinsic_TypedArrayLength(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    RootedObject obj(cx, &args[0].toObject());
    MOZ_ASSERT(obj->is<TypedArrayObject>());
    args.rval().setInt32(obj->as<TypedArrayObject>().length());
    return true;
}

bool
CallSelfHostedNonGenericMethod(JSContext* cx, CallArgs args)
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

static bool
intrinsic_IsWeakSet(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isObject());

    args.rval().setBoolean(args[0].toObject().is<WeakSetObject>());
    return true;
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
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEFAULT_LOCALE_ERROR);
        return false;
    }

    RootedString jslocale(cx, JS_NewStringCopyZ(cx, locale));
    if (!jslocale)
        return false;

    args.rval().setString(jslocale);
    return true;
}

bool
js::intrinsic_IsConstructing(JSContext* cx, unsigned argc, Value* vp)
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
    RootedValue ctor(cx, cx->global()->getConstructor(protoKey));
    MOZ_ASSERT(ctor.isObject());

    args.rval().set(ctor);
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
    JS_FN("std_Array_join",                      array_join,                   1,0),
    JS_FN("std_Array_push",                      array_push,                   1,0),
    JS_FN("std_Array_pop",                       array_pop,                    0,0),
    JS_FN("std_Array_shift",                     array_shift,                  0,0),
    JS_FN("std_Array_unshift",                   array_unshift,                1,0),
    JS_FN("std_Array_slice",                     array_slice,                  2,0),
    JS_FN("std_Array_sort",                      array_sort,                   1,0),

    JS_FN("std_Date_now",                        date_now,                     0,0),
    JS_FN("std_Date_valueOf",                    date_valueOf,                 0,0),

    JS_FN("std_Function_bind",                   fun_bind,                     1,0),
    JS_FN("std_Function_apply",                  js_fun_apply,                 2,0),

    JS_FN("std_Math_floor",                      math_floor,                   1,0),
    JS_FN("std_Math_max",                        math_max,                     2,0),
    JS_FN("std_Math_min",                        math_min,                     2,0),
    JS_FN("std_Math_abs",                        math_abs,                     1,0),
    JS_FN("std_Math_imul",                       math_imul,                    2,0),
    JS_FN("std_Math_log2",                       math_log2,                    1,0),

    JS_FN("std_Map_has",                         MapObject::has,               1,0),
    JS_FN("std_Map_iterator",                    MapObject::entries,           0,0),

    JS_FN("std_Number_valueOf",                  js_num_valueOf,               0,0),

    JS_FN("std_Object_create",                   obj_create,                   2,0),
    JS_FN("std_Object_defineProperty",           obj_defineProperty,           3,0),
    JS_FN("std_Object_getPrototypeOf",           obj_getPrototypeOf,           1,0),
    JS_FN("std_Object_getOwnPropertyNames",      obj_getOwnPropertyNames,      1,0),
    JS_FN("std_Object_getOwnPropertyDescriptor", obj_getOwnPropertyDescriptor, 2,0),
    JS_FN("std_Object_hasOwnProperty",           obj_hasOwnProperty,           1,0),

    JS_FN("std_Set_has",                         SetObject::has,               1,0),
    JS_FN("std_Set_iterator",                    SetObject::values,            0,0),

    JS_FN("std_String_fromCharCode",             str_fromCharCode,             1,0),
    JS_FN("std_String_charCodeAt",               js_str_charCodeAt,            1,0),
    JS_FN("std_String_indexOf",                  str_indexOf,                  1,0),
    JS_FN("std_String_lastIndexOf",              str_lastIndexOf,              1,0),
    JS_FN("std_String_match",                    str_match,                    1,0),
    JS_FN("std_String_replace",                  str_replace,                  2,0),
    JS_FN("std_String_split",                    str_split,                    2,0),
    JS_FN("std_String_startsWith",               str_startsWith,               1,0),
    JS_FN("std_String_toLowerCase",              str_toLowerCase,              0,0),
    JS_FN("std_String_toUpperCase",              str_toUpperCase,              0,0),

    JS_FN("std_WeakMap_has",                     WeakMap_has,                  1,0),
    JS_FN("std_WeakMap_get",                     WeakMap_get,                  2,0),
    JS_FN("std_WeakMap_set",                     WeakMap_set,                  2,0),
    JS_FN("std_WeakMap_delete",                  WeakMap_delete,               1,0),
    JS_FN("std_WeakMap_clear",                   WeakMap_clear,                0,0),

    // Helper funtions after this point.
    JS_FN("ToObject",                intrinsic_ToObject,                1,0),
    JS_FN("IsObject",                intrinsic_IsObject,                1,0),
    JS_FN("ToInteger",               intrinsic_ToInteger,               1,0),
    JS_FN("ToString",                intrinsic_ToString,                1,0),
    JS_FN("ToPropertyKey",           intrinsic_ToPropertyKey,           1,0),
    JS_FN("IsCallable",              intrinsic_IsCallable,              1,0),
    JS_FN("IsConstructor",           intrinsic_IsConstructor,           1,0),
    JS_FN("OwnPropertyKeys",         intrinsic_OwnPropertyKeys,         1,0),
    JS_FN("ThrowError",              intrinsic_ThrowError,              4,0),
    JS_FN("AssertionFailed",         intrinsic_AssertionFailed,         1,0),
    JS_FN("MakeConstructible",       intrinsic_MakeConstructible,       2,0),
    JS_FN("_IsConstructing",         intrinsic_IsConstructing,          0,0),
    JS_FN("_ConstructorForTypedArray", intrinsic_ConstructorForTypedArray, 1,0),
    JS_FN("DecompileArg",            intrinsic_DecompileArg,            2,0),
    JS_FN("RuntimeDefaultLocale",    intrinsic_RuntimeDefaultLocale,    0,0),
    JS_FN("SubstringKernel",         intrinsic_SubstringKernel,         3,0),

    JS_FN("UnsafePutElements",       intrinsic_UnsafePutElements,       3,0),
    JS_FN("_DefineDataProperty",     intrinsic_DefineDataProperty,      4,0),
    JS_FN("UnsafeSetReservedSlot",   intrinsic_UnsafeSetReservedSlot,   3,0),
    JS_FN("UnsafeGetReservedSlot",   intrinsic_UnsafeGetReservedSlot,   2,0),
    JS_FN("UnsafeGetObjectFromReservedSlot",
          intrinsic_UnsafeGetObjectFromReservedSlot, 2, 0),
    JS_FN("UnsafeGetInt32FromReservedSlot",
          intrinsic_UnsafeGetInt32FromReservedSlot, 2, 0),
    JS_FN("UnsafeGetStringFromReservedSlot",
          intrinsic_UnsafeGetStringFromReservedSlot, 2, 0),
    JS_FN("UnsafeGetBooleanFromReservedSlot",
          intrinsic_UnsafeGetBooleanFromReservedSlot, 2, 0),
    JS_FN("IsPackedArray",           intrinsic_IsPackedArray,           1,0),

    JS_FN("GetIteratorPrototype",    intrinsic_GetIteratorPrototype,    0,0),

    JS_FN("NewArrayIterator",        intrinsic_NewArrayIterator,        0,0),
    JS_FN("IsArrayIterator",         intrinsic_IsArrayIterator,         1,0),
    JS_FN("CallArrayIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<ArrayIteratorObject>>,      2,0),


    JS_FN("NewStringIterator",       intrinsic_NewStringIterator,       0,0),
    JS_FN("IsStringIterator",        intrinsic_IsStringIterator,        1,0),
    JS_FN("CallStringIteratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<StringIteratorObject>>,     2,0),

    JS_FN("IsStarGeneratorObject",   intrinsic_IsStarGeneratorObject,   1,0),
    JS_FN("StarGeneratorObjectIsClosed", intrinsic_StarGeneratorObjectIsClosed, 1,0),
    JS_FN("IsSuspendedStarGenerator",intrinsic_IsSuspendedStarGenerator,1,0),

    JS_FN("IsLegacyGeneratorObject", intrinsic_IsLegacyGeneratorObject, 1,0),
    JS_FN("LegacyGeneratorObjectIsClosed", intrinsic_LegacyGeneratorObjectIsClosed, 1,0),
    JS_FN("CloseClosingLegacyGeneratorObject", intrinsic_CloseClosingLegacyGeneratorObject, 1,0),
    JS_FN("ThrowStopIteration",      intrinsic_ThrowStopIteration,      0,0),

    JS_FN("GeneratorIsRunning",      intrinsic_GeneratorIsRunning,      1,0),
    JS_FN("GeneratorSetClosed",      intrinsic_GeneratorSetClosed,      1,0),

    JS_FN("IsTypedArray",            intrinsic_IsTypedArray,            1,0),
    JS_FN("TypedArrayLength",        intrinsic_TypedArrayLength,        1,0),

    JS_FN("CallTypedArrayMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<TypedArrayObject>>, 2, 0),

    JS_FN("CallLegacyGeneratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<LegacyGeneratorObject>>, 2, 0),
    JS_FN("CallStarGeneratorMethodIfWrapped",
          CallNonGenericSelfhostedMethod<Is<StarGeneratorObject>>, 2, 0),

    JS_FN("IsWeakSet",               intrinsic_IsWeakSet,               1,0),

    JS_FN("NewDenseArray",           intrinsic_NewDenseArray,           1,0),

    // See builtin/TypedObject.h for descriptors of the typedobj functions.
    JS_FN("NewOpaqueTypedObject",           js::NewOpaqueTypedObject, 1, 0),
    JS_FN("NewDerivedTypedObject",          js::NewDerivedTypedObject, 3, 0),
    JS_FN("TypedObjectBuffer",              TypedObject::GetBuffer, 1, 0),
    JS_FN("TypedObjectByteOffset",          TypedObject::GetByteOffset, 1, 0),
    JS_FN("AttachTypedObject",              js::AttachTypedObject, 3, 0),
    JS_FN("SetTypedObjectOffset",           js::SetTypedObjectOffset, 2, 0),
    JS_FN("ObjectIsTypeDescr"    ,          js::ObjectIsTypeDescr, 1, 0),
    JS_FN("ObjectIsTypedObject",            js::ObjectIsTypedObject, 1, 0),
    JS_FN("ObjectIsTransparentTypedObject", js::ObjectIsTransparentTypedObject, 1, 0),
    JS_FN("TypedObjectIsAttached",          js::TypedObjectIsAttached, 1, 0),
    JS_FN("TypedObjectTypeDescr",           js::TypedObjectTypeDescr, 1, 0),
    JS_FN("ObjectIsOpaqueTypedObject",      js::ObjectIsOpaqueTypedObject, 1, 0),
    JS_FN("TypeDescrIsArrayType",           js::TypeDescrIsArrayType, 1, 0),
    JS_FN("TypeDescrIsSimpleType",          js::TypeDescrIsSimpleType, 1, 0),
    JS_FN("ClampToUint8",                   js::ClampToUint8, 1, 0),
    JS_FN("GetTypedObjectModule",           js::GetTypedObjectModule, 0, 0),
    JS_FN("GetFloat32x4TypeDescr",          js::GetFloat32x4TypeDescr, 0, 0),
    JS_FN("GetFloat64x2TypeDescr",          js::GetFloat64x2TypeDescr, 0, 0),
    JS_FN("GetInt32x4TypeDescr",            js::GetInt32x4TypeDescr, 0, 0),

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
    compartment->isSystem = true;

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
        if (Compile(cx, shg, options, filename, &script))
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

        ok = ok && Evaluate(cx, shg, options, src, srcLen, &rv);
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
        MarkObjectRoot(trc, &selfHostingGlobal_, "self-hosting global");
}

bool
JSRuntime::isSelfHostingCompartment(JSCompartment* comp)
{
    return selfHostingGlobal_->compartment() == comp;
}

bool
JSRuntime::isSelfHostingZone(JS::Zone* zone)
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
        return js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_NO_SUCH_SELF_HOSTED_PROP,
                                        JSDVG_IGNORE_STACK, value, NullPtr(), nullptr, nullptr);
    }

    RootedShape shape(cx, selfHostedObject->lookupPure(id));
    if (!shape) {
        RootedValue value(cx, IdToValue(id));
        return js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_NO_SUCH_SELF_HOSTED_PROP,
                                        JSDVG_IGNORE_STACK, value, NullPtr(), nullptr, nullptr);
    }

    MOZ_ASSERT(shape->hasSlot() && shape->hasDefaultGetter());
    vp.set(selfHostedObject->getSlot(shape->slot()));
    return true;
}

static bool
CloneProperties(JSContext* cx, HandleNativeObject selfHostedObject, HandleObject clone)
{
    AutoIdVector ids(cx);

    for (size_t i = 0; i < selfHostedObject->getDenseInitializedLength(); i++) {
        if (!selfHostedObject->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE)) {
            if (!ids.append(INT_TO_JSID(i)))
                return false;
        }
    }

    for (Shape::Range<NoGC> range(selfHostedObject->lastProperty()); !range.empty(); range.popFront()) {
        Shape& shape = range.front();
        if (shape.enumerable() && !ids.append(shape.propid()))
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
            !JS_DefinePropertyById(cx, clone, id, val, 0))
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
    AutoCycleDetector detect(cx, selfHostedObject);
    if (!detect.init())
        return nullptr;
    if (detect.foundCycle()) {
        JS_ReportError(cx, "SelfHosted cloning cannot handle cyclic object graphs.");
        return nullptr;
    }

    RootedObject clone(cx);
    if (selfHostedObject->is<JSFunction>()) {
        RootedFunction selfHostedFunction(cx, &selfHostedObject->as<JSFunction>());
        bool hasName = selfHostedFunction->atom() != nullptr;
        // Arrow functions use the first extended slot for their lexical |this| value.
        MOZ_ASSERT(!selfHostedFunction->isArrow());
        js::gc::AllocKind kind = hasName
                                 ? JSFunction::ExtendedFinalizeKind
                                 : selfHostedFunction->getAllocKind();
        clone = CloneFunctionObject(cx, selfHostedFunction, cx->global(), kind, TenuredObject);
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
        clone = JS_NewDateObjectMsec(cx, selfHostedObject->as<DateObject>().UTCTime().toNumber());
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
        clone = NewDenseEmptyArray(cx, NullPtr(), TenuredObject);
    } else {
        MOZ_ASSERT(selfHostedObject->isNative());
        clone = NewObjectWithGivenProto(cx, selfHostedObject->getClass(), NullPtr(), cx->global(),
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
    RootedScript sourceScript(cx, sourceFun->getOrCreateScript(cx));
    if (!sourceScript)
        return false;
    MOZ_ASSERT(!sourceScript->enclosingStaticScope());
    JSScript* cscript = CloneScript(cx, NullPtr(), targetFun, sourceScript);
    if (!cscript)
        return false;
    cscript->setFunction(targetFun);

    MOZ_ASSERT(sourceFun->nargs() == targetFun->nargs());
    // The target function might have been relazified after it's flags changed.
    targetFun->setFlags((targetFun->flags() & ~JSFunction::INTERPRETED_LAZY) |
                        sourceFun->flags() | JSFunction::EXTENDED);
    targetFun->setScript(cscript);
    MOZ_ASSERT(targetFun->isExtended());
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
    return fun->isSelfHostedBuiltin() && fun->getExtendedSlot(0).toString() == name;
}

static_assert(JSString::MAX_LENGTH <= INT32_MAX,
              "StringIteratorNext in builtin/String.js assumes the stored index "
              "into the string is an Int32Value");
