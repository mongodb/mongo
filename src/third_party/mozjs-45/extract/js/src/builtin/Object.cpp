/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Object.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/UniquePtr.h"

#include "jscntxt.h"

#include "builtin/Eval.h"
#include "frontend/BytecodeCompiler.h"
#include "jit/InlinableNatives.h"
#include "vm/StringBuffer.h"

#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

using js::frontend::IsIdentifier;
using mozilla::ArrayLength;
using mozilla::UniquePtr;

bool
js::obj_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, nullptr);
    if (args.isConstructing() && (&args.newTarget().toObject() != &args.callee())) {
        RootedObject newTarget(cx, &args.newTarget().toObject());
        obj = CreateThis(cx, &PlainObject::class_, newTarget);
        if (!obj)
            return false;
    } else if (args.length() > 0 && !args[0].isNullOrUndefined()) {
        obj = ToObject(cx, args[0]);
        if (!obj)
            return false;
    } else {
        /* Make an object whether this was called with 'new' or not. */
        if (!NewObjectScriptedCall(cx, &obj))
            return false;
    }

    args.rval().setObject(*obj);
    return true;
}

/* ES5 15.2.4.7. */
bool
js::obj_propertyIsEnumerable(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    HandleValue idValue = args.get(0);

    // As an optimization, provide a fast path when rooting is not necessary and
    // we can safely retrieve the attributes from the object's shape.

    /* Steps 1-2. */
    jsid id;
    if (args.thisv().isObject() && ValueToId<NoGC>(cx, idValue, &id)) {
        JSObject* obj = &args.thisv().toObject();

        /* Step 3. */
        Shape* shape;
        if (obj->isNative() &&
            NativeLookupOwnProperty<NoGC>(cx, &obj->as<NativeObject>(), id, &shape))
        {
            /* Step 4. */
            if (!shape) {
                args.rval().setBoolean(false);
                return true;
            }

            /* Step 5. */
            unsigned attrs = GetShapeAttributes(obj, shape);
            args.rval().setBoolean((attrs & JSPROP_ENUMERATE) != 0);
            return true;
        }
    }

    /* Step 1. */
    RootedId idRoot(cx);
    if (!ToPropertyKey(cx, idValue, &idRoot))
        return false;

    /* Step 2. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Step 3. */
    Rooted<PropertyDescriptor> desc(cx);
    if (!GetOwnPropertyDescriptor(cx, obj, idRoot, &desc))
        return false;

    /* Steps 4-5. */
    args.rval().setBoolean(desc.object() && desc.enumerable());
    return true;
}

#if JS_HAS_TOSOURCE
static bool
obj_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JS_CHECK_RECURSION(cx, return false);

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    JSString* str = ObjectToSource(cx, obj);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

/*
 * Given a function source string, return the offset and length of the part
 * between '(function $name' and ')'.
 */
template <typename CharT>
static bool
ArgsAndBodySubstring(mozilla::Range<const CharT> chars, size_t* outOffset, size_t* outLen)
{
    const CharT* const start = chars.start().get();
    const CharT* const end = chars.end().get();
    const CharT* s = start;

    uint8_t parenChomp = 0;
    if (s[0] == '(') {
        s++;
        parenChomp = 1;
    }

    /* Try to jump "function" keyword. */
    s = js_strchr_limit(s, ' ', end);
    if (!s)
        return false;

    /*
     * Jump over the function's name: it can't be encoded as part
     * of an ECMA getter or setter.
     */
    s = js_strchr_limit(s, '(', end);
    if (!s)
        return false;

    if (*s == ' ')
        s++;

    *outOffset = s - start;
    *outLen = end - s - parenChomp;
    MOZ_ASSERT(*outOffset + *outLen <= chars.length());
    return true;
}

JSString*
js::ObjectToSource(JSContext* cx, HandleObject obj)
{
    /* If outermost, we need parentheses to be an expression, not a block. */
    bool outermost = (cx->cycleDetectorSet.count() == 0);

    AutoCycleDetector detector(cx, obj);
    if (!detector.init())
        return nullptr;
    if (detector.foundCycle())
        return NewStringCopyZ<CanGC>(cx, "{}");

    StringBuffer buf(cx);
    if (outermost && !buf.append('('))
        return nullptr;
    if (!buf.append('{'))
        return nullptr;

    RootedValue v0(cx), v1(cx);
    MutableHandleValue val[2] = {&v0, &v1};

    RootedString str0(cx), str1(cx);
    MutableHandleString gsop[2] = {&str0, &str1};

    AutoIdVector idv(cx);
    if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY | JSITER_SYMBOLS, &idv))
        return nullptr;

    bool comma = false;
    for (size_t i = 0; i < idv.length(); ++i) {
        RootedId id(cx, idv[i]);
        Rooted<PropertyDescriptor> desc(cx);
        if (!GetOwnPropertyDescriptor(cx, obj, id, &desc))
            return nullptr;

        int valcnt = 0;
        if (desc.object()) {
            if (desc.isAccessorDescriptor()) {
                if (desc.hasGetterObject() && desc.getterObject()) {
                    val[valcnt].setObject(*desc.getterObject());
                    gsop[valcnt].set(cx->names().get);
                    valcnt++;
                }
                if (desc.hasSetterObject() && desc.setterObject()) {
                    val[valcnt].setObject(*desc.setterObject());
                    gsop[valcnt].set(cx->names().set);
                    valcnt++;
                }
            } else {
                valcnt = 1;
                val[0].set(desc.value());
                gsop[0].set(nullptr);
            }
        }

        /* Convert id to a string. */
        RootedString idstr(cx);
        if (JSID_IS_SYMBOL(id)) {
            RootedValue v(cx, SymbolValue(JSID_TO_SYMBOL(id)));
            idstr = ValueToSource(cx, v);
            if (!idstr)
                return nullptr;
        } else {
            RootedValue idv(cx, IdToValue(id));
            idstr = ToString<CanGC>(cx, idv);
            if (!idstr)
                return nullptr;

            /*
             * If id is a string that's not an identifier, or if it's a negative
             * integer, then it must be quoted.
             */
            if (JSID_IS_ATOM(id)
                ? !IsIdentifier(JSID_TO_ATOM(id))
                : JSID_TO_INT(id) < 0)
            {
                idstr = QuoteString(cx, idstr, char16_t('\''));
                if (!idstr)
                    return nullptr;
            }
        }

        for (int j = 0; j < valcnt; j++) {
            /* Convert val[j] to its canonical source form. */
            JSString* valsource = ValueToSource(cx, val[j]);
            if (!valsource)
                return nullptr;

            RootedLinearString valstr(cx, valsource->ensureLinear(cx));
            if (!valstr)
                return nullptr;

            size_t voffset = 0;
            size_t vlength = valstr->length();

            /*
             * Remove '(function ' from the beginning of valstr and ')' from the
             * end so that we can put "get" in front of the function definition.
             */
            if (gsop[j] && IsFunctionObject(val[j])) {
                bool success;
                JS::AutoCheckCannotGC nogc;
                if (valstr->hasLatin1Chars())
                    success = ArgsAndBodySubstring(valstr->latin1Range(nogc), &voffset, &vlength);
                else
                    success = ArgsAndBodySubstring(valstr->twoByteRange(nogc), &voffset, &vlength);
                if (!success)
                    gsop[j].set(nullptr);
            }

            if (comma && !buf.append(", "))
                return nullptr;
            comma = true;

            if (gsop[j]) {
                if (!buf.append(gsop[j]) || !buf.append(' '))
                    return nullptr;
            }
            if (JSID_IS_SYMBOL(id) && !buf.append('['))
                return nullptr;
            if (!buf.append(idstr))
                return nullptr;
            if (JSID_IS_SYMBOL(id) && !buf.append(']'))
                return nullptr;
            if (!buf.append(gsop[j] ? ' ' : ':'))
                return nullptr;

            if (!buf.appendSubstring(valstr, voffset, vlength))
                return nullptr;
        }
    }

    if (!buf.append('}'))
        return nullptr;
    if (outermost && !buf.append(')'))
        return nullptr;

    return buf.finishString();
}
#endif /* JS_HAS_TOSOURCE */

JSString*
JS_BasicObjectToString(JSContext* cx, HandleObject obj)
{
    // Some classes are really common, don't allocate new strings for them.
    // The ordering below is based on the measurements in bug 966264.
    if (obj->is<PlainObject>())
        return cx->names().objectObject;
    if (obj->is<StringObject>())
        return cx->names().objectString;
    if (obj->is<ArrayObject>())
        return cx->names().objectArray;
    if (obj->is<JSFunction>())
        return cx->names().objectFunction;
    if (obj->is<NumberObject>())
        return cx->names().objectNumber;

    const char* className = GetObjectClassName(cx, obj);

    if (strcmp(className, "Window") == 0)
        return cx->names().objectWindow;

    StringBuffer sb(cx);
    if (!sb.append("[object ") || !sb.append(className, strlen(className)) ||
        !sb.append("]"))
    {
        return nullptr;
    }
    return sb.finishString();
}

/* ES5 15.2.4.2.  Note steps 1 and 2 are errata. */
bool
js::obj_toString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    if (args.thisv().isUndefined()) {
        args.rval().setString(cx->names().objectUndefined);
        return true;
    }

    /* Step 2. */
    if (args.thisv().isNull()) {
        args.rval().setString(cx->names().objectNull);
        return true;
    }

    /* Step 3. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Steps 4-5. */
    JSString* str = JS_BasicObjectToString(cx, obj);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}


bool
js::obj_valueOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

static bool
obj_setPrototypeOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject setPrototypeOf(cx, &args.callee());
    if (!GlobalObject::warnOnceAboutPrototypeMutation(cx, setPrototypeOf))
        return false;

    if (args.length() < 2) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             "Object.setPrototypeOf", "1", "");
        return false;
    }

    /* Step 1-2. */
    if (args[0].isNullOrUndefined()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                             args[0].isNull() ? "null" : "undefined", "object");
        return false;
    }

    /* Step 3. */
    if (!args[1].isObjectOrNull()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                             "Object.setPrototypeOf", "an object or null", InformalValueTypeName(args[1]));
        return false;
    }

    /* Step 4. */
    if (!args[0].isObject()) {
        args.rval().set(args[0]);
        return true;
    }

    /* Step 5-7. */
    RootedObject obj(cx, &args[0].toObject());
    RootedObject newProto(cx, args[1].toObjectOrNull());
    if (!SetPrototype(cx, obj, newProto))
        return false;

    /* Step 8. */
    args.rval().set(args[0]);
    return true;
}

#if JS_HAS_OBJ_WATCHPOINT

bool
js::WatchHandler(JSContext* cx, JSObject* obj_, jsid id_, JS::Value old,
                 JS::Value* nvp, void* closure)
{
    RootedObject obj(cx, obj_);
    RootedId id(cx, id_);

    /* Avoid recursion on (obj, id) already being watched on cx. */
    AutoResolving resolving(cx, obj, id, AutoResolving::WATCH);
    if (resolving.alreadyStarted())
        return true;

    JSObject* callable = (JSObject*)closure;
    Value argv[] = { IdToValue(id), old, *nvp };
    RootedValue rv(cx);
    if (!Invoke(cx, ObjectValue(*obj), ObjectOrNullValue(callable), ArrayLength(argv), argv, &rv))
        return false;

    *nvp = rv;
    return true;
}

static bool
obj_watch(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    if (!GlobalObject::warnOnceAboutWatch(cx, obj))
        return false;

    if (args.length() <= 1) {
        ReportMissingArg(cx, args.calleev(), 1);
        return false;
    }

    RootedObject callable(cx, ValueToCallable(cx, args[1], args.length() - 2));
    if (!callable)
        return false;

    RootedId propid(cx);
    if (!ValueToId<CanGC>(cx, args[0], &propid))
        return false;

    if (!WatchProperty(cx, obj, propid, callable))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
obj_unwatch(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    if (!GlobalObject::warnOnceAboutWatch(cx, obj))
        return false;

    RootedId id(cx);
    if (args.length() != 0) {
        if (!ValueToId<CanGC>(cx, args[0], &id))
            return false;
    } else {
        id = JSID_VOID;
    }

    if (!UnwatchProperty(cx, obj, id))
        return false;

    args.rval().setUndefined();
    return true;
}

#endif /* JS_HAS_OBJ_WATCHPOINT */

/* ECMA 15.2.4.5. */
bool
js::obj_hasOwnProperty(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    HandleValue idValue = args.get(0);

    // As an optimization, provide a fast path when rooting is not necessary and
    // we can safely retrieve the object's shape.

    /* Step 1, 2. */
    jsid id;
    if (args.thisv().isObject() && ValueToId<NoGC>(cx, idValue, &id)) {
        JSObject* obj = &args.thisv().toObject();
        Shape* prop;
        if (obj->isNative() &&
            NativeLookupOwnProperty<NoGC>(cx, &obj->as<NativeObject>(), id, &prop))
        {
            args.rval().setBoolean(!!prop);
            return true;
        }
    }

    /* Step 1. */
    RootedId idRoot(cx);
    if (!ToPropertyKey(cx, idValue, &idRoot))
        return false;

    /* Step 2. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Step 3. */
    bool found;
    if (!HasOwnProperty(cx, obj, idRoot, &found))
        return false;

    /* Step 4,5. */
    args.rval().setBoolean(found);
    return true;
}

/* ES5 15.2.4.6. */
static bool
obj_isPrototypeOf(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    if (args.length() < 1 || !args[0].isObject()) {
        args.rval().setBoolean(false);
        return true;
    }

    /* Step 2. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Step 3. */
    bool isDelegate;
    if (!IsDelegate(cx, obj, args[0], &isDelegate))
        return false;
    args.rval().setBoolean(isDelegate);
    return true;
}

PlainObject*
js::ObjectCreateImpl(JSContext* cx, HandleObject proto, NewObjectKind newKind,
                     HandleObjectGroup group)
{
    // Give the new object a small number of fixed slots, like we do for empty
    // object literals ({}).
    gc::AllocKind allocKind = GuessObjectGCKind(0);

    if (!proto) {
        // Object.create(null) is common, optimize it by using an allocation
        // site specific ObjectGroup. Because GetCallerInitGroup is pretty
        // slow, the caller can pass in the group if it's known and we use that
        // instead.
        RootedObjectGroup ngroup(cx, group);
        if (!ngroup) {
            ngroup = ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Null);
            if (!ngroup)
                return nullptr;
        }

        MOZ_ASSERT(!ngroup->proto().toObjectOrNull());

        return NewObjectWithGroup<PlainObject>(cx, ngroup, allocKind, newKind);
    }

    return NewObjectWithGivenProto<PlainObject>(cx, proto, allocKind, newKind);
}

PlainObject*
js::ObjectCreateWithTemplate(JSContext* cx, HandlePlainObject templateObj)
{
    RootedObject proto(cx, templateObj->getProto());
    RootedObjectGroup group(cx, templateObj->group());
    return ObjectCreateImpl(cx, proto, GenericObject, group);
}

// ES6 draft rev34 (2015/02/20) 19.1.2.2 Object.create(O [, Properties])
bool
js::obj_create(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    if (args.length() == 0) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             "Object.create", "0", "s");
        return false;
    }

    if (!args[0].isObjectOrNull()) {
        RootedValue v(cx, args[0]);
        UniquePtr<char[], JS::FreePolicy> bytes =
            DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, v, nullptr);
        if (!bytes)
            return false;
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                             bytes.get(), "not an object or null");
        return false;
    }

    // Step 2.
    RootedObject proto(cx, args[0].toObjectOrNull());
    RootedPlainObject obj(cx, ObjectCreateImpl(cx, proto));
    if (!obj)
        return false;

    // Step 3.
    if (args.hasDefined(1)) {
        RootedValue val(cx, args[1]);
        RootedObject props(cx, ToObject(cx, val));
        if (!props || !DefineProperties(cx, obj, props))
            return false;
    }

    // Step 4.
    args.rval().setObject(*obj);
    return true;
}

// ES6 draft rev27 (2014/08/24) 19.1.2.6 Object.getOwnPropertyDescriptor(O, P)
bool
js::obj_getOwnPropertyDescriptor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1-2.
    RootedObject obj(cx, ToObject(cx, args.get(0)));
    if (!obj)
        return false;

    // Steps 3-4.
    RootedId id(cx);
    if (!ToPropertyKey(cx, args.get(1), &id))
        return false;

    // Steps 5-7.
    Rooted<PropertyDescriptor> desc(cx);
    return GetOwnPropertyDescriptor(cx, obj, id, &desc) &&
           FromPropertyDescriptor(cx, desc, args.rval());
}

// ES6 draft rev27 (2014/08/24) 19.1.2.14 Object.keys(O)
static bool
obj_keys(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return GetOwnPropertyKeys(cx, args, JSITER_OWNONLY);
}

/* ES6 draft 15.2.3.16 */
static bool
obj_is(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool same;
    if (!SameValue(cx, args.get(0), args.get(1), &same))
        return false;

    args.rval().setBoolean(same);
    return true;
}

bool
js::IdToStringOrSymbol(JSContext* cx, HandleId id, MutableHandleValue result)
{
    if (JSID_IS_INT(id)) {
        JSString* str = Int32ToString<CanGC>(cx, JSID_TO_INT(id));
        if (!str)
            return false;
        result.setString(str);
    } else if (JSID_IS_ATOM(id)) {
        result.setString(JSID_TO_STRING(id));
    } else {
        result.setSymbol(JSID_TO_SYMBOL(id));
    }
    return true;
}

/* ES6 draft rev 25 (2014 May 22) 19.1.2.8.1 */
bool
js::GetOwnPropertyKeys(JSContext* cx, const JS::CallArgs& args, unsigned flags)
{
    // Steps 1-2.
    RootedObject obj(cx, ToObject(cx, args.get(0)));
    if (!obj)
        return false;

    // Steps 3-10.
    AutoIdVector keys(cx);
    if (!GetPropertyKeys(cx, obj, flags, &keys))
        return false;

    // Step 11.
    AutoValueVector vals(cx);
    if (!vals.resize(keys.length()))
        return false;

    for (size_t i = 0, len = keys.length(); i < len; i++) {
        MOZ_ASSERT_IF(JSID_IS_SYMBOL(keys[i]), flags & JSITER_SYMBOLS);
        MOZ_ASSERT_IF(!JSID_IS_SYMBOL(keys[i]), !(flags & JSITER_SYMBOLSONLY));
        if (!IdToStringOrSymbol(cx, keys[i], vals[i]))
            return false;
    }

    JSObject* aobj = NewDenseCopiedArray(cx, vals.length(), vals.begin());
    if (!aobj)
        return false;

    args.rval().setObject(*aobj);
    return true;
}

bool
js::obj_getOwnPropertyNames(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return GetOwnPropertyKeys(cx, args, JSITER_OWNONLY | JSITER_HIDDEN);
}

/* ES6 draft rev 25 (2014 May 22) 19.1.2.8 */
static bool
obj_getOwnPropertySymbols(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return GetOwnPropertyKeys(cx, args,
                              JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS | JSITER_SYMBOLSONLY);
}

/* ES6 draft rev 32 (2015 Feb 2) 19.1.2.4: Object.defineProperty(O, P, Attributes) */
bool
js::obj_defineProperty(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1-3.
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.defineProperty", &obj))
        return false;
    RootedId id(cx);
    if (!ToPropertyKey(cx, args.get(1), &id))
        return false;

    // Steps 4-5.
    Rooted<PropertyDescriptor> desc(cx);
    if (!ToPropertyDescriptor(cx, args.get(2), true, &desc))
        return false;

    // Steps 6-8.
    if (!DefineProperty(cx, obj, id, desc))
        return false;
    args.rval().setObject(*obj);
    return true;
}

/* ES5 15.2.3.7: Object.defineProperties(O, Properties) */
static bool
obj_defineProperties(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Steps 1 and 7. */
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.defineProperties", &obj))
        return false;
    args.rval().setObject(*obj);

    /* Step 2. */
    if (args.length() < 2) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             "Object.defineProperties", "0", "s");
        return false;
    }
    RootedValue val(cx, args[1]);
    RootedObject props(cx, ToObject(cx, val));
    if (!props)
        return false;

    /* Steps 3-6. */
    return DefineProperties(cx, obj, props);
}

// ES6 20141014 draft 19.1.2.15 Object.preventExtensions(O)
static bool
obj_preventExtensions(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().set(args.get(0));

    // Step 1.
    if (!args.get(0).isObject())
        return true;

    // Steps 2-5.
    RootedObject obj(cx, &args.get(0).toObject());
    return PreventExtensions(cx, obj);
}

// ES6 draft rev27 (2014/08/24) 19.1.2.5 Object.freeze(O)
static bool
obj_freeze(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().set(args.get(0));

    // Step 1.
    if (!args.get(0).isObject())
        return true;

    // Steps 2-5.
    RootedObject obj(cx, &args.get(0).toObject());
    return SetIntegrityLevel(cx, obj, IntegrityLevel::Frozen);
}

// ES6 draft rev27 (2014/08/24) 19.1.2.12 Object.isFrozen(O)
static bool
obj_isFrozen(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    bool frozen = true;

    // Step 2.
    if (args.get(0).isObject()) {
        RootedObject obj(cx, &args.get(0).toObject());
        if (!TestIntegrityLevel(cx, obj, IntegrityLevel::Frozen, &frozen))
            return false;
    }
    args.rval().setBoolean(frozen);
    return true;
}

// ES6 draft rev27 (2014/08/24) 19.1.2.17 Object.seal(O)
static bool
obj_seal(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().set(args.get(0));

    // Step 1.
    if (!args.get(0).isObject())
        return true;

    // Steps 2-5.
    RootedObject obj(cx, &args.get(0).toObject());
    return SetIntegrityLevel(cx, obj, IntegrityLevel::Sealed);
}

// ES6 draft rev27 (2014/08/24) 19.1.2.13 Object.isSealed(O)
static bool
obj_isSealed(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    bool sealed = true;

    // Step 2.
    if (args.get(0).isObject()) {
        RootedObject obj(cx, &args.get(0).toObject());
        if (!TestIntegrityLevel(cx, obj, IntegrityLevel::Sealed, &sealed))
            return false;
    }
    args.rval().setBoolean(sealed);
    return true;
}

static bool
ProtoGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    HandleValue thisv = args.thisv();
    if (thisv.isNullOrUndefined()) {
        ReportIncompatible(cx, args);
        return false;
    }
    if (thisv.isPrimitive() && !BoxNonStrictThis(cx, args))
        return false;

    RootedObject obj(cx, &args.thisv().toObject());
    RootedObject proto(cx);
    if (!GetPrototype(cx, obj, &proto))
        return false;
    args.rval().setObjectOrNull(proto);
    return true;
}

namespace js {
size_t sSetProtoCalled = 0;
} // namespace js

static bool
ProtoSetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Do this here, rather than after the this-check so even likely-buggy
    // use of the __proto__ setter on unacceptable values, where no subsequent
    // use occurs on an acceptable value, will trigger a warning.
    RootedObject callee(cx, &args.callee());
    if (!GlobalObject::warnOnceAboutPrototypeMutation(cx, callee))
       return false;

    HandleValue thisv = args.thisv();
    if (thisv.isNullOrUndefined()) {
        ReportIncompatible(cx, args);
        return false;
    }
    if (thisv.isPrimitive()) {
        // Mutating a boxed primitive's [[Prototype]] has no side effects.
        args.rval().setUndefined();
        return true;
    }

    if (!cx->runningWithTrustedPrincipals())
        ++sSetProtoCalled;

    Rooted<JSObject*> obj(cx, &args.thisv().toObject());

    /* Do nothing if __proto__ isn't being set to an object or null. */
    if (args.length() == 0 || !args[0].isObjectOrNull()) {
        args.rval().setUndefined();
        return true;
    }

    Rooted<JSObject*> newProto(cx, args[0].toObjectOrNull());
    if (!SetPrototype(cx, obj, newProto))
        return false;

    args.rval().setUndefined();
    return true;
}

static const JSFunctionSpec object_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,             obj_toSource,                0,0),
#endif
    JS_FN(js_toString_str,             obj_toString,                0,0),
    JS_SELF_HOSTED_FN(js_toLocaleString_str, "Object_toLocaleString", 0,JSPROP_DEFINE_LATE),
    JS_FN(js_valueOf_str,              obj_valueOf,                 0,0),
#if JS_HAS_OBJ_WATCHPOINT
    JS_FN(js_watch_str,                obj_watch,                   2,0),
    JS_FN(js_unwatch_str,              obj_unwatch,                 1,0),
#endif
    JS_FN(js_hasOwnProperty_str,       obj_hasOwnProperty,          1,0),
    JS_FN(js_isPrototypeOf_str,        obj_isPrototypeOf,           1,0),
    JS_FN(js_propertyIsEnumerable_str, obj_propertyIsEnumerable,    1,0),
#if JS_OLD_GETTER_SETTER_METHODS
    JS_SELF_HOSTED_FN(js_defineGetter_str, "ObjectDefineGetter",    2,JSPROP_DEFINE_LATE),
    JS_SELF_HOSTED_FN(js_defineSetter_str, "ObjectDefineSetter",    2,JSPROP_DEFINE_LATE),
    JS_SELF_HOSTED_FN(js_lookupGetter_str, "ObjectLookupGetter",    1,JSPROP_DEFINE_LATE),
    JS_SELF_HOSTED_FN(js_lookupSetter_str, "ObjectLookupSetter",    1,JSPROP_DEFINE_LATE),
#endif
    JS_FS_END
};

static const JSPropertySpec object_properties[] = {
#if JS_HAS_OBJ_PROTO_PROP
    JS_PSGS("__proto__", ProtoGetter, ProtoSetter, 0),
#endif
    JS_PS_END
};

static const JSFunctionSpec object_static_methods[] = {
    JS_SELF_HOSTED_FN("assign",        "ObjectStaticAssign",        2, JSPROP_DEFINE_LATE),
    JS_SELF_HOSTED_FN("getPrototypeOf", "ObjectGetPrototypeOf",     1, JSPROP_DEFINE_LATE),
    JS_FN("setPrototypeOf",            obj_setPrototypeOf,          2, 0),
    JS_FN("getOwnPropertyDescriptor",  obj_getOwnPropertyDescriptor,2, 0),
    JS_FN("keys",                      obj_keys,                    1, 0),
#ifndef RELEASE_BUILD
    JS_SELF_HOSTED_FN("values",        "ObjectValues",              1, JSPROP_DEFINE_LATE),
    JS_SELF_HOSTED_FN("entries",       "ObjectEntries",             1, JSPROP_DEFINE_LATE),
#endif
    JS_FN("is",                        obj_is,                      2, 0),
    JS_FN("defineProperty",            obj_defineProperty,          3, 0),
    JS_FN("defineProperties",          obj_defineProperties,        2, 0),
    JS_INLINABLE_FN("create",          obj_create,                  2, 0, ObjectCreate),
    JS_FN("getOwnPropertyNames",       obj_getOwnPropertyNames,     1, 0),
    JS_FN("getOwnPropertySymbols",     obj_getOwnPropertySymbols,   1, 0),
    JS_SELF_HOSTED_FN("isExtensible",  "ObjectIsExtensible",        1, JSPROP_DEFINE_LATE),
    JS_FN("preventExtensions",         obj_preventExtensions,       1, 0),
    JS_FN("freeze",                    obj_freeze,                  1, 0),
    JS_FN("isFrozen",                  obj_isFrozen,                1, 0),
    JS_FN("seal",                      obj_seal,                    1, 0),
    JS_FN("isSealed",                  obj_isSealed,                1, 0),
    JS_FS_END
};

static JSObject*
CreateObjectConstructor(JSContext* cx, JSProtoKey key)
{
    Rooted<GlobalObject*> self(cx, cx->global());
    if (!GlobalObject::ensureConstructor(cx, self, JSProto_Function))
        return nullptr;

    /* Create the Object function now that we have a [[Prototype]] for it. */
    return NewNativeConstructor(cx, obj_construct, 1, HandlePropertyName(cx->names().Object),
                                gc::AllocKind::FUNCTION, SingletonObject);
}

static JSObject*
CreateObjectPrototype(JSContext* cx, JSProtoKey key)
{
    MOZ_ASSERT(!cx->runtime()->isAtomsCompartment(cx->compartment()));
    MOZ_ASSERT(cx->global()->isNative());

    /*
     * Create |Object.prototype| first, mirroring CreateBlankProto but for the
     * prototype of the created object.
     */
    RootedPlainObject objectProto(cx, NewObjectWithGivenProto<PlainObject>(cx, nullptr,
                                                                           SingletonObject));
    if (!objectProto)
        return nullptr;

    bool succeeded;
    if (!SetImmutablePrototype(cx, objectProto, &succeeded))
        return nullptr;
    MOZ_ASSERT(succeeded,
               "should have been able to make a fresh Object.prototype's "
               "[[Prototype]] immutable");

    /*
     * The default 'new' type of Object.prototype is required by type inference
     * to have unknown properties, to simplify handling of e.g. heterogenous
     * objects in JSON and script literals.
     */
    if (!JSObject::setNewGroupUnknown(cx, &PlainObject::class_, objectProto))
        return nullptr;

    return objectProto;
}

static bool
FinishObjectClassInit(JSContext* cx, JS::HandleObject ctor, JS::HandleObject proto)
{
    Rooted<GlobalObject*> global(cx, cx->global());

    /* ES5 15.1.2.1. */
    RootedId evalId(cx, NameToId(cx->names().eval));
    JSObject* evalobj = DefineFunction(cx, global, evalId, IndirectEval, 1,
                                       JSFUN_STUB_GSOPS | JSPROP_RESOLVING);
    if (!evalobj)
        return false;
    global->setOriginalEval(evalobj);

    Rooted<NativeObject*> holder(cx, GlobalObject::getIntrinsicsHolder(cx, global));
    if (!holder)
        return false;

    /*
     * Define self-hosted functions after setting the intrinsics holder
     * (which is needed to define self-hosted functions)
     */
    if (!cx->runtime()->isSelfHostingGlobal(global)) {
        if (!JS_DefineFunctions(cx, ctor, object_static_methods, OnlyDefineLateProperties))
            return false;
        if (!JS_DefineFunctions(cx, proto, object_methods, OnlyDefineLateProperties))
            return false;
    }

    /*
     * The global object should have |Object.prototype| as its [[Prototype]].
     * Eventually we'd like to have standard classes be there from the start,
     * and thus we would know we were always setting what had previously been a
     * null [[Prototype]], but right now some code assumes it can set the
     * [[Prototype]] before standard classes have been initialized.  For now,
     * only set the [[Prototype]] if it hasn't already been set.
     */
    Rooted<TaggedProto> tagged(cx, TaggedProto(proto));
    if (global->shouldSplicePrototype(cx)) {
        if (!global->splicePrototype(cx, global->getClass(), tagged))
            return false;
    }
    return true;
}

const Class PlainObject::class_ = {
    js_Object_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Object),
    nullptr,  /* addProperty */
    nullptr,  /* delProperty */
    nullptr,  /* getProperty */
    nullptr,  /* setProperty */
    nullptr,  /* enumerate */
    nullptr,  /* resolve */
    nullptr,  /* mayResolve */
    nullptr,  /* finalize */
    nullptr,  /* call */
    nullptr,  /* hasInstance */
    nullptr,  /* construct */
    nullptr,  /* trace */
    {
        CreateObjectConstructor,
        CreateObjectPrototype,
        object_static_methods,
        nullptr,
        object_methods,
        object_properties,
        FinishObjectClassInit
    }
};

const Class* const js::ObjectClassPtr = &PlainObject::class_;
