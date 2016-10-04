/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/RegExp.h"

#include "mozilla/TypeTraits.h"

#include "jscntxt.h"

#include "irregexp/RegExpParser.h"
#include "jit/InlinableNatives.h"
#include "vm/RegExpStatics.h"
#include "vm/StringBuffer.h"

#include "jsobjinlines.h"

#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::Maybe;

bool
js::CreateRegExpMatchResult(JSContext* cx, HandleString input, const MatchPairs& matches,
                            MutableHandleValue rval)
{
    MOZ_ASSERT(input);

    /*
     * Create the (slow) result array for a match.
     *
     * Array contents:
     *  0:              matched string
     *  1..pairCount-1: paren matches
     *  input:          input string
     *  index:          start index for the match
     */

    /* Get the templateObject that defines the shape and type of the output object */
    JSObject* templateObject = cx->compartment()->regExps.getOrCreateMatchResultTemplateObject(cx);
    if (!templateObject)
        return false;

    size_t numPairs = matches.length();
    MOZ_ASSERT(numPairs > 0);

    RootedArrayObject arr(cx, NewDenseFullyAllocatedArrayWithTemplate(cx, numPairs, templateObject));
    if (!arr)
        return false;

    /* Store a Value for each pair. */
    for (size_t i = 0; i < numPairs; i++) {
        const MatchPair& pair = matches[i];

        if (pair.isUndefined()) {
            MOZ_ASSERT(i != 0); /* Since we had a match, first pair must be present. */
            arr->setDenseInitializedLength(i + 1);
            arr->initDenseElement(i, UndefinedValue());
        } else {
            JSLinearString* str = NewDependentString(cx, input, pair.start, pair.length());
            if (!str)
                return false;
            arr->setDenseInitializedLength(i + 1);
            arr->initDenseElement(i, StringValue(str));
        }
    }

    /* Set the |index| property. (TemplateObject positions it in slot 0) */
    arr->setSlot(0, Int32Value(matches[0].start));

    /* Set the |input| property. (TemplateObject positions it in slot 1) */
    arr->setSlot(1, StringValue(input));

#ifdef DEBUG
    RootedValue test(cx);
    RootedId id(cx, NameToId(cx->names().index));
    if (!NativeGetProperty(cx, arr, id, &test))
        return false;
    MOZ_ASSERT(test == arr->getSlot(0));
    id = NameToId(cx->names().input);
    if (!NativeGetProperty(cx, arr, id, &test))
        return false;
    MOZ_ASSERT(test == arr->getSlot(1));
#endif

    rval.setObject(*arr);
    return true;
}

static RegExpRunStatus
ExecuteRegExpImpl(JSContext* cx, RegExpStatics* res, RegExpShared& re, HandleLinearString input,
                  size_t searchIndex, MatchPairs* matches)
{
    RegExpRunStatus status = re.execute(cx, input, searchIndex, matches);
    if (status == RegExpRunStatus_Success && res) {
        if (matches) {
            if (!res->updateFromMatchPairs(cx, input, *matches))
                return RegExpRunStatus_Error;
        } else {
            res->updateLazily(cx, input, &re, searchIndex);
        }
    }
    return status;
}

/* Legacy ExecuteRegExp behavior is baked into the JSAPI. */
bool
js::ExecuteRegExpLegacy(JSContext* cx, RegExpStatics* res, RegExpObject& reobj,
                        HandleLinearString input, size_t* lastIndex, bool test,
                        MutableHandleValue rval)
{
    RegExpGuard shared(cx);
    if (!reobj.getShared(cx, &shared))
        return false;

    ScopedMatchPairs matches(&cx->tempLifoAlloc());

    RegExpRunStatus status = ExecuteRegExpImpl(cx, res, *shared, input, *lastIndex, &matches);
    if (status == RegExpRunStatus_Error)
        return false;

    if (status == RegExpRunStatus_Success_NotFound) {
        /* ExecuteRegExp() previously returned an array or null. */
        rval.setNull();
        return true;
    }

    *lastIndex = matches[0].limit;

    if (test) {
        /* Forbid an array, as an optimization. */
        rval.setBoolean(true);
        return true;
    }

    return CreateRegExpMatchResult(cx, input, matches, rval);
}

/*
 * ES6 21.2.3.2.2.  Because this function only ever returns |obj| in the spec,
 * provided by the user, we omit it and just return the usual success/failure.
 */
static bool
RegExpInitialize(JSContext* cx, Handle<RegExpObject*> obj, HandleValue patternValue,
                 HandleValue flagsValue, RegExpStaticsUse staticsUse)
{
    RootedAtom pattern(cx);
    if (patternValue.isUndefined()) {
        /* Step 1. */
        pattern = cx->runtime()->emptyString;
    } else {
        /* Steps 2-3. */
        pattern = ToAtom<CanGC>(cx, patternValue);
        if (!pattern)
            return false;
    }

    /* Step 4. */
    RegExpFlag flags = RegExpFlag(0);
    if (!flagsValue.isUndefined()) {
        /* Steps 5-6. */
        RootedString flagStr(cx, ToString<CanGC>(cx, flagsValue));
        if (!flagStr)
            return false;
        /* Step 7. */
        if (!ParseRegExpFlags(cx, flagStr, &flags))
            return false;
    }

    /* Steps 9-10. */
    CompileOptions options(cx);
    frontend::TokenStream dummyTokenStream(cx, options, nullptr, 0, nullptr);
    if (!irregexp::ParsePatternSyntax(dummyTokenStream, cx->tempLifoAlloc(), pattern))
        return false;

    if (staticsUse == UseRegExpStatics) {
        RegExpStatics* res = cx->global()->getRegExpStatics(cx);
        if (!res)
            return false;
        flags = RegExpFlag(flags | res->getFlags());
    }

    /* Steps 11-15. */
    if (!RegExpObject::initFromAtom(cx, obj, pattern, flags))
        return false;

    /* Step 16. */
    return true;
}

MOZ_ALWAYS_INLINE bool
IsRegExpObject(HandleValue v)
{
    return v.isObject() && v.toObject().is<RegExpObject>();
}

/* ES6 draft rc3 7.2.8. */
bool
js::IsRegExp(JSContext* cx, HandleValue value, bool* result)
{
    /* Step 1. */
    if (!value.isObject()) {
        *result = false;
        return true;
    }
    RootedObject obj(cx, &value.toObject());

    /* Steps 2-3. */
    RootedValue isRegExp(cx);
    RootedId matchId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().match));
    if (!GetProperty(cx, obj, obj, matchId, &isRegExp))
        return false;

    /* Step 4. */
    if (!isRegExp.isUndefined()) {
        *result = ToBoolean(isRegExp);
        return true;
    }

    /* Steps 5-6. */
    ESClassValue cls;
    if (!GetClassOfValue(cx, value, &cls))
        return false;

    *result = cls == ESClass_RegExp;
    return true;
}

/* ES6 B.2.5.1. */
MOZ_ALWAYS_INLINE bool
regexp_compile_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsRegExpObject(args.thisv()));

    Rooted<RegExpObject*> regexp(cx, &args.thisv().toObject().as<RegExpObject>());

    // Step 3.
    RootedValue patternValue(cx, args.get(0));
    ESClassValue cls;
    if (!GetClassOfValue(cx, patternValue, &cls))
        return false;
    if (cls == ESClass_RegExp) {
        // Step 3a.
        if (args.hasDefined(1)) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_NEWREGEXP_FLAGGED);
            return false;
        }

        // Beware!  |patternObj| might be a proxy into another compartment, so
        // don't assume |patternObj.is<RegExpObject>()|.  For the same reason,
        // don't reuse the RegExpShared below.
        RootedObject patternObj(cx, &patternValue.toObject());

        RootedAtom sourceAtom(cx);
        RegExpFlag flags;
        {
            // Step 3b.
            RegExpGuard g(cx);
            if (!RegExpToShared(cx, patternObj, &g))
                return false;

            sourceAtom = g->getSource();
            flags = g->getFlags();
        }

        // Step 5.
        if (!RegExpObject::initFromAtom(cx, regexp, sourceAtom, flags))
            return false;

        args.rval().setObject(*regexp);
        return true;
    }

    // Step 4.
    RootedValue P(cx, patternValue);
    RootedValue F(cx, args.get(1));

    // Step 5.
    if (!RegExpInitialize(cx, regexp, P, F, UseRegExpStatics))
        return false;

    args.rval().setObject(*regexp);
    return true;
}

static bool
regexp_compile(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Steps 1-2. */
    return CallNonGenericMethod<IsRegExpObject, regexp_compile_impl>(cx, args);
}

/* ES6 21.2.3.1. */
bool
js::regexp_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Steps 1-2.
    bool patternIsRegExp;
    if (!IsRegExp(cx, args.get(0), &patternIsRegExp))
        return false;


    // We can delay step 3 and step 4a until later, during
    // GetPrototypeFromCallableConstructor calls. Accessing the new.target
    // and the callee from the stack is unobservable.
    if (!args.isConstructing()) {
        // Step 4b.
        if (patternIsRegExp && !args.hasDefined(1)) {
            RootedObject patternObj(cx, &args[0].toObject());

            // Steps 4b.i-ii.
            RootedValue patternConstructor(cx);
            if (!GetProperty(cx, patternObj, patternObj, cx->names().constructor, &patternConstructor))
                return false;

            // Step 4b.iii.
            if (patternConstructor.isObject() && patternConstructor.toObject() == args.callee()) {
                args.rval().set(args[0]);
                return true;
            }
        }
    }

    RootedValue patternValue(cx, args.get(0));

    // Step 5.
    ESClassValue cls;
    if (!GetClassOfValue(cx, patternValue, &cls))
        return false;
    if (cls == ESClass_RegExp) {
        // Beware!  |patternObj| might be a proxy into another compartment, so
        // don't assume |patternObj.is<RegExpObject>()|.  For the same reason,
        // don't reuse the RegExpShared below.
        RootedObject patternObj(cx, &patternValue.toObject());

        // Step 5
        RootedAtom sourceAtom(cx);
        RegExpFlag flags;
        {
            // Step 5.a.
            RegExpGuard g(cx);
            if (!RegExpToShared(cx, patternObj, &g))
                return false;
            sourceAtom = g->getSource();

            if (!args.hasDefined(1)) {
                // Step 5b.
                flags = g->getFlags();
            }
        }

        // Steps 8-9.
        RootedObject proto(cx);
        if (!GetPrototypeFromCallableConstructor(cx, args, &proto))
            return false;

        Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, proto));
        if (!regexp)
            return false;

        // Step 10.
        if (args.hasDefined(1)) {
            // Step 5c / 21.2.3.2.2 RegExpInitialize step 5.
            flags = RegExpFlag(0);
            RootedString flagStr(cx, ToString<CanGC>(cx, args[1]));
            if (!flagStr)
                return false;
            if (!ParseRegExpFlags(cx, flagStr, &flags))
                return false;
        }

        if (!RegExpObject::initFromAtom(cx, regexp, sourceAtom, flags))
            return false;

        args.rval().setObject(*regexp);
        return true;
    }

    RootedValue P(cx);
    RootedValue F(cx);

    // Step 6.
    if (patternIsRegExp) {
        RootedObject patternObj(cx, &patternValue.toObject());

        // Steps 6a-b.
        if (!GetProperty(cx, patternObj, patternObj, cx->names().source, &P))
            return false;

        // Steps 6c-d.
        F = args.get(1);
        if (F.isUndefined()) {
            if (!GetProperty(cx, patternObj, patternObj, cx->names().flags, &F))
                return false;
        }
    } else {
        // Steps 7a-b.
        P = patternValue;
        F = args.get(1);
    }

    // Steps 8-9.
    RootedObject proto(cx);
    if (!GetPrototypeFromCallableConstructor(cx, args, &proto))
        return false;

    Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx, proto));
    if (!regexp)
        return false;

    // Step 10.
    if (!RegExpInitialize(cx, regexp, P, F, UseRegExpStatics))
        return false;

    args.rval().setObject(*regexp);
    return true;
}

bool
js::regexp_construct_no_statics(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    MOZ_ASSERT(args.length() == 1 || args.length() == 2);
    MOZ_ASSERT(args[0].isString());
    MOZ_ASSERT_IF(args.length() == 2, args[1].isString());
    MOZ_ASSERT(!args.isConstructing());

    /* Steps 1-6 are not required since pattern is always string. */

    /* Steps 7-10. */
    Rooted<RegExpObject*> regexp(cx, RegExpAlloc(cx));
    if (!regexp)
        return false;

    if (!RegExpInitialize(cx, regexp, args[0], args.get(1), DontUseRegExpStatics))
        return false;

    args.rval().setObject(*regexp);
    return true;
}

/* ES6 draft rev32 21.2.5.4. */
MOZ_ALWAYS_INLINE bool
regexp_global_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsRegExpObject(args.thisv()));
    Rooted<RegExpObject*> reObj(cx, &args.thisv().toObject().as<RegExpObject>());

    /* Steps 4-6. */
    args.rval().setBoolean(reObj->global());
    return true;
}

static bool
regexp_global(JSContext* cx, unsigned argc, JS::Value* vp)
{
    /* Steps 1-3. */
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExpObject, regexp_global_impl>(cx, args);
}

/* ES6 draft rev32 21.2.5.5. */
MOZ_ALWAYS_INLINE bool
regexp_ignoreCase_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsRegExpObject(args.thisv()));
    Rooted<RegExpObject*> reObj(cx, &args.thisv().toObject().as<RegExpObject>());

    /* Steps 4-6. */
    args.rval().setBoolean(reObj->ignoreCase());
    return true;
}

static bool
regexp_ignoreCase(JSContext* cx, unsigned argc, JS::Value* vp)
{
    /* Steps 1-3. */
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExpObject, regexp_ignoreCase_impl>(cx, args);
}

/* ES6 draft rev32 21.2.5.7. */
MOZ_ALWAYS_INLINE bool
regexp_multiline_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsRegExpObject(args.thisv()));
    Rooted<RegExpObject*> reObj(cx, &args.thisv().toObject().as<RegExpObject>());

    /* Steps 4-6. */
    args.rval().setBoolean(reObj->multiline());
    return true;
}

static bool
regexp_multiline(JSContext* cx, unsigned argc, JS::Value* vp)
{
    /* Steps 1-3. */
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExpObject, regexp_multiline_impl>(cx, args);
}

/* ES6 draft rev32 21.2.5.10. */
MOZ_ALWAYS_INLINE bool
regexp_source_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsRegExpObject(args.thisv()));
    Rooted<RegExpObject*> reObj(cx, &args.thisv().toObject().as<RegExpObject>());

    /* Step 5. */
    RootedAtom src(cx, reObj->getSource());
    if (!src)
        return false;

    /* Step 7. */
    RootedString str(cx, EscapeRegExpPattern(cx, src));
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

static bool
regexp_source(JSContext* cx, unsigned argc, JS::Value* vp)
{
    /* Steps 1-4. */
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExpObject, regexp_source_impl>(cx, args);
}

/* ES6 draft rev32 21.2.5.12. */
MOZ_ALWAYS_INLINE bool
regexp_sticky_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsRegExpObject(args.thisv()));
    Rooted<RegExpObject*> reObj(cx, &args.thisv().toObject().as<RegExpObject>());

    /* Steps 4-6. */
    args.rval().setBoolean(reObj->sticky());
    return true;
}

static bool
regexp_sticky(JSContext* cx, unsigned argc, JS::Value* vp)
{
    /* Steps 1-3. */
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExpObject, regexp_sticky_impl>(cx, args);
}

const JSPropertySpec js::regexp_properties[] = {
    JS_SELF_HOSTED_GET("flags", "RegExpFlagsGetter", 0),
    JS_PSG("global", regexp_global, 0),
    JS_PSG("ignoreCase", regexp_ignoreCase, 0),
    JS_PSG("multiline", regexp_multiline, 0),
    JS_PSG("source", regexp_source, 0),
    JS_PSG("sticky", regexp_sticky, 0),
    JS_PS_END
};

const JSFunctionSpec js::regexp_methods[] = {
#if JS_HAS_TOSOURCE
    JS_SELF_HOSTED_FN(js_toSource_str, "RegExpToString", 0, 0),
#endif
    JS_SELF_HOSTED_FN(js_toString_str, "RegExpToString", 0, 0),
    JS_FN("compile",        regexp_compile,     2,0),
    JS_INLINABLE_FN("exec", regexp_exec,        1,0, RegExpExec),
    JS_INLINABLE_FN("test", regexp_test,        1,0, RegExpTest),
    JS_FS_END
};

#define STATIC_PAREN_GETTER_CODE(parenNum)                                      \
    if (!res->createParen(cx, parenNum, args.rval()))                           \
        return false;                                                           \
    if (args.rval().isUndefined())                                              \
        args.rval().setString(cx->runtime()->emptyString);                      \
    return true

/*
 * RegExp static properties.
 *
 * RegExp class static properties and their Perl counterparts:
 *
 *  RegExp.input                $_
 *  RegExp.multiline            $*
 *  RegExp.lastMatch            $&
 *  RegExp.lastParen            $+
 *  RegExp.leftContext          $`
 *  RegExp.rightContext         $'
 */

#define DEFINE_STATIC_GETTER(name, code)                                        \
    static bool                                                                 \
    name(JSContext* cx, unsigned argc, Value* vp)                               \
    {                                                                           \
        CallArgs args = CallArgsFromVp(argc, vp);                               \
        RegExpStatics* res = cx->global()->getRegExpStatics(cx);                \
        if (!res)                                                               \
            return false;                                                       \
        code;                                                                   \
    }

DEFINE_STATIC_GETTER(static_input_getter,        return res->createPendingInput(cx, args.rval()))
DEFINE_STATIC_GETTER(static_multiline_getter,    args.rval().setBoolean(res->multiline());
                                                 return true)
DEFINE_STATIC_GETTER(static_lastMatch_getter,    return res->createLastMatch(cx, args.rval()))
DEFINE_STATIC_GETTER(static_lastParen_getter,    return res->createLastParen(cx, args.rval()))
DEFINE_STATIC_GETTER(static_leftContext_getter,  return res->createLeftContext(cx, args.rval()))
DEFINE_STATIC_GETTER(static_rightContext_getter, return res->createRightContext(cx, args.rval()))

DEFINE_STATIC_GETTER(static_paren1_getter,       STATIC_PAREN_GETTER_CODE(1))
DEFINE_STATIC_GETTER(static_paren2_getter,       STATIC_PAREN_GETTER_CODE(2))
DEFINE_STATIC_GETTER(static_paren3_getter,       STATIC_PAREN_GETTER_CODE(3))
DEFINE_STATIC_GETTER(static_paren4_getter,       STATIC_PAREN_GETTER_CODE(4))
DEFINE_STATIC_GETTER(static_paren5_getter,       STATIC_PAREN_GETTER_CODE(5))
DEFINE_STATIC_GETTER(static_paren6_getter,       STATIC_PAREN_GETTER_CODE(6))
DEFINE_STATIC_GETTER(static_paren7_getter,       STATIC_PAREN_GETTER_CODE(7))
DEFINE_STATIC_GETTER(static_paren8_getter,       STATIC_PAREN_GETTER_CODE(8))
DEFINE_STATIC_GETTER(static_paren9_getter,       STATIC_PAREN_GETTER_CODE(9))

#define DEFINE_STATIC_SETTER(name, code)                                        \
    static bool                                                                 \
    name(JSContext* cx, unsigned argc, Value* vp)                               \
    {                                                                           \
        RegExpStatics* res = cx->global()->getRegExpStatics(cx);                \
        if (!res)                                                               \
            return false;                                                       \
        code;                                                                   \
        return true;                                                            \
    }

static bool
static_input_setter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RegExpStatics* res = cx->global()->getRegExpStatics(cx);
    if (!res)
        return false;

    RootedString str(cx, ToString<CanGC>(cx, args.get(0)));
    if (!str)
        return false;

    res->setPendingInput(str);
    args.rval().setString(str);
    return true;
}

static bool
static_multiline_setter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RegExpStatics* res = cx->global()->getRegExpStatics(cx);
    if (!res)
        return false;

    bool b = ToBoolean(args.get(0));
    res->setMultiline(cx, b);
    args.rval().setBoolean(b);
    return true;
}

const JSPropertySpec js::regexp_static_props[] = {
    JS_PSGS("input", static_input_getter, static_input_setter,
            JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSGS("multiline", static_multiline_getter, static_multiline_setter,
            JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("lastMatch", static_lastMatch_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("lastParen", static_lastParen_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("leftContext",  static_leftContext_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("rightContext", static_rightContext_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$1", static_paren1_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$2", static_paren2_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$3", static_paren3_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$4", static_paren4_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$5", static_paren5_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$6", static_paren6_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$7", static_paren7_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$8", static_paren8_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSG("$9", static_paren9_getter, JSPROP_PERMANENT | JSPROP_ENUMERATE),
    JS_PSGS("$_", static_input_getter, static_input_setter, JSPROP_PERMANENT),
    JS_PSGS("$*", static_multiline_getter, static_multiline_setter, JSPROP_PERMANENT),
    JS_PSG("$&", static_lastMatch_getter, JSPROP_PERMANENT),
    JS_PSG("$+", static_lastParen_getter, JSPROP_PERMANENT),
    JS_PSG("$`", static_leftContext_getter, JSPROP_PERMANENT),
    JS_PSG("$'", static_rightContext_getter, JSPROP_PERMANENT),
    JS_PS_END
};

JSObject*
js::CreateRegExpPrototype(JSContext* cx, JSProtoKey key)
{
    MOZ_ASSERT(key == JSProto_RegExp);

    Rooted<RegExpObject*> proto(cx, cx->global()->createBlankPrototype<RegExpObject>(cx));
    if (!proto)
        return nullptr;
    proto->NativeObject::setPrivate(nullptr);

    RootedAtom source(cx, cx->names().empty);
    if (!RegExpObject::initFromAtom(cx, proto, source, RegExpFlag(0)))
        return nullptr;
    return proto;
}

static bool
ReportLastIndexNonwritable(JSContext* cx)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_READ_ONLY, "\"lastIndex\"");
    return false;
}

static bool
SetLastIndex(JSContext* cx, Handle<RegExpObject*> reobj, double lastIndex)
{
    if (!reobj->lookup(cx, cx->names().lastIndex)->writable())
        return ReportLastIndexNonwritable(cx);

    reobj->setLastIndex(lastIndex);
    return true;
}

/* ES6 final draft 21.2.5.2.2. */
RegExpRunStatus
js::ExecuteRegExp(JSContext* cx, HandleObject regexp, HandleString string,
                  MatchPairs* matches, RegExpStaticsUpdate staticsUpdate)
{
    /*
     * WARNING: Despite the presence of spec step comment numbers, this
     *          algorithm isn't consistent with any ES6 version, draft or
     *          otherwise.  YOU HAVE BEEN WARNED.
     */

    /* Steps 1-2 performed by the caller. */
    Rooted<RegExpObject*> reobj(cx, &regexp->as<RegExpObject>());

    RegExpGuard re(cx);
    if (!reobj->getShared(cx, &re))
        return RegExpRunStatus_Error;

    RegExpStatics* res;
    if (staticsUpdate == UpdateRegExpStatics) {
        res = cx->global()->getRegExpStatics(cx);
        if (!res)
            return RegExpRunStatus_Error;
    } else {
        res = nullptr;
    }

    RootedLinearString input(cx, string->ensureLinear(cx));
    if (!input)
        return RegExpRunStatus_Error;

    /* Step 3. */
    size_t length = input->length();

    /* Steps 4-5. */
    RootedValue lastIndex(cx, reobj->getLastIndex());
    int searchIndex;
    if (lastIndex.isInt32()) {
        /* Aggressively avoid doubles. */
        searchIndex = lastIndex.toInt32();
    } else {
        double d;
        if (!ToInteger(cx, lastIndex, &d))
            return RegExpRunStatus_Error;

        /* Inlined steps 6-10, 15.a with doubles to detect failure case. */
        if (reobj->needUpdateLastIndex() && (d < 0 || d > length)) {
            /* Steps 15.a.i-ii. */
            if (!SetLastIndex(cx, reobj, 0))
                return RegExpRunStatus_Error;

            /* Step 15.a.iii. */
            return RegExpRunStatus_Success_NotFound;
        }

        searchIndex = int(d);
    }

    /*
     * Steps 6-10.
     *
     * Also make sure that we have a MatchPairs for regexps which update their
     * last index, as we won't compute the last index otherwise.
     */
    Maybe<ScopedMatchPairs> alternateMatches;
    if (!reobj->needUpdateLastIndex()) {
        searchIndex = 0;
    } else if (!matches) {
        alternateMatches.emplace(&cx->tempLifoAlloc());
        matches = &alternateMatches.ref();
    }

    /* Step 15.a. */
    if (searchIndex < 0 || size_t(searchIndex) > length) {
        /* Steps 15.a.i-ii. */
        if (!SetLastIndex(cx, reobj, 0))
            return RegExpRunStatus_Error;

        /* Step 15.a.iii. */
        return RegExpRunStatus_Success_NotFound;
    }

    /* Step 14-29. */
    RegExpRunStatus status = ExecuteRegExpImpl(cx, res, *re, input, searchIndex, matches);
    if (status == RegExpRunStatus_Error)
        return RegExpRunStatus_Error;

    if (status == RegExpRunStatus_Success_NotFound) {
        /* Steps 15.a.i-ii. */
        if (!SetLastIndex(cx, reobj, 0))
            return RegExpRunStatus_Error;
    } else if (reobj->needUpdateLastIndex()) {
        /* Steps 18.a-b. */
        MOZ_ASSERT(matches && !matches->empty());
        if (!SetLastIndex(cx, reobj, (*matches)[0].limit))
            return RegExpRunStatus_Error;
    }

    return status;
}

/* ES5 15.10.6.2 (and 15.10.6.3, which calls 15.10.6.2). */
static RegExpRunStatus
ExecuteRegExp(JSContext* cx, const CallArgs& args, MatchPairs* matches)
{
    /* Step 1 (a) was performed by CallNonGenericMethod. */
    RootedObject regexp(cx, &args.thisv().toObject());

    /* Step 2. */
    RootedString string(cx, ToString<CanGC>(cx, args.get(0)));
    if (!string)
        return RegExpRunStatus_Error;

    return ExecuteRegExp(cx, regexp, string, matches, UpdateRegExpStatics);
}

/* ES5 15.10.6.2. */
static bool
regexp_exec_impl(JSContext* cx, HandleObject regexp, HandleString string,
                 RegExpStaticsUpdate staticsUpdate, MutableHandleValue rval)
{
    /* Execute regular expression and gather matches. */
    ScopedMatchPairs matches(&cx->tempLifoAlloc());

    RegExpRunStatus status = ExecuteRegExp(cx, regexp, string, &matches, staticsUpdate);
    if (status == RegExpRunStatus_Error)
        return false;

    if (status == RegExpRunStatus_Success_NotFound) {
        rval.setNull();
        return true;
    }

    return CreateRegExpMatchResult(cx, string, matches, rval);
}

static bool
regexp_exec_impl(JSContext* cx, const CallArgs& args)
{
    RootedObject regexp(cx, &args.thisv().toObject());
    RootedString string(cx, ToString<CanGC>(cx, args.get(0)));
    if (!string)
        return false;

    return regexp_exec_impl(cx, regexp, string, UpdateRegExpStatics, args.rval());
}

bool
js::regexp_exec(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, IsRegExpObject, regexp_exec_impl, args);
}

/* Separate interface for use by IonMonkey. */
bool
js::regexp_exec_raw(JSContext* cx, HandleObject regexp, HandleString input,
                    MatchPairs* maybeMatches, MutableHandleValue output)
{
    // The MatchPairs will always be passed in, but RegExp execution was
    // successful only if the pairs have actually been filled in.
    if (maybeMatches && maybeMatches->pairsRaw()[0] >= 0)
        return CreateRegExpMatchResult(cx, input, *maybeMatches, output);
    return regexp_exec_impl(cx, regexp, input, UpdateRegExpStatics, output);
}

bool
js::regexp_exec_no_statics(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(IsRegExpObject(args[0]));
    MOZ_ASSERT(args[1].isString());

    RootedObject regexp(cx, &args[0].toObject());
    RootedString string(cx, args[1].toString());

    return regexp_exec_impl(cx, regexp, string, DontUpdateRegExpStatics, args.rval());
}

/* ES5 15.10.6.3. */
static bool
regexp_test_impl(JSContext* cx, const CallArgs& args)
{
    RegExpRunStatus status = ExecuteRegExp(cx, args, nullptr);
    args.rval().setBoolean(status == RegExpRunStatus_Success);
    return status != RegExpRunStatus_Error;
}

/* Separate interface for use by IonMonkey. */
bool
js::regexp_test_raw(JSContext* cx, HandleObject regexp, HandleString input, bool* result)
{
    RegExpRunStatus status = ExecuteRegExp(cx, regexp, input, nullptr, UpdateRegExpStatics);
    *result = (status == RegExpRunStatus_Success);
    return status != RegExpRunStatus_Error;
}

bool
js::regexp_test(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, IsRegExpObject, regexp_test_impl, args);
}

bool
js::regexp_test_no_statics(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(IsRegExpObject(args[0]));
    MOZ_ASSERT(args[1].isString());

    RootedObject regexp(cx, &args[0].toObject());
    RootedString string(cx, args[1].toString());

    RegExpRunStatus status = ExecuteRegExp(cx, regexp, string, nullptr, DontUpdateRegExpStatics);
    args.rval().setBoolean(status == RegExpRunStatus_Success);
    return status != RegExpRunStatus_Error;
}
