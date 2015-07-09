/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/RegExp.h"

#include "mozilla/TypeTraits.h"

#include "jscntxt.h"

#include "irregexp/RegExpParser.h"
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
 * Compile a new |RegExpShared| for the |RegExpObject|.
 *
 * Per ECMAv5 15.10.4.1, we act on combinations of (pattern, flags) as
 * arguments:
 *
 *  RegExp, undefined => flags := pattern.flags
 *  RegExp, _ => throw TypeError
 *  _ => pattern := ToString(pattern) if defined(pattern) else ''
 *       flags := ToString(flags) if defined(flags) else ''
 */
static bool
CompileRegExpObject(JSContext* cx, RegExpObjectBuilder& builder, CallArgs args,
                    RegExpStaticsUse staticsUse)
{
    if (args.length() == 0) {
        MOZ_ASSERT(staticsUse == UseRegExpStatics);
        RegExpStatics* res = cx->global()->getRegExpStatics(cx);
        if (!res)
            return false;
        Rooted<JSAtom*> empty(cx, cx->names().emptyRegExp);
        RegExpObject* reobj = builder.build(empty, res->getFlags());
        if (!reobj)
            return false;
        args.rval().setObject(*reobj);
        return true;
    }

    RootedValue sourceValue(cx, args[0]);

    /*
     * If we get passed in an object whose internal [[Class]] property is
     * "RegExp", return a new object with the same source/flags.
     */
    if (IsObjectWithClass(sourceValue, ESClass_RegExp, cx)) {
        /*
         * Beware, sourceObj may be a (transparent) proxy to a RegExp, so only
         * use generic (proxyable) operations on sourceObj that do not assume
         * sourceObj.is<RegExpObject>().
         */
        RootedObject sourceObj(cx, &sourceValue.toObject());

        if (args.hasDefined(1)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_NEWREGEXP_FLAGGED);
            return false;
        }

        /*
         * Only extract the 'flags' out of sourceObj; do not reuse the
         * RegExpShared since it may be from a different compartment.
         */
        RegExpFlag flags;
        {
            RegExpGuard g(cx);
            if (!RegExpToShared(cx, sourceObj, &g))
                return false;

            flags = g->getFlags();
        }

        /*
         * 'toSource' is a permanent read-only property, so this is equivalent
         * to executing RegExpObject::getSource on the unwrapped object.
         */
        RootedValue v(cx);
        if (!GetProperty(cx, sourceObj, sourceObj, cx->names().source, &v))
            return false;

        // For proxies like CPOWs, we can't assume the result of a property get
        // for 'source' is atomized.
        Rooted<JSAtom*> sourceAtom(cx, AtomizeString(cx, v.toString()));
        RegExpObject* reobj = builder.build(sourceAtom, flags);
        if (!reobj)
            return false;

        args.rval().setObject(*reobj);
        return true;
    }

    RootedAtom source(cx);
    if (sourceValue.isUndefined()) {
        source = cx->runtime()->emptyString;
    } else {
        /* Coerce to string and compile. */
        source = ToAtom<CanGC>(cx, sourceValue);
        if (!source)
            return false;
    }

    RegExpFlag flags = RegExpFlag(0);
    if (args.hasDefined(1)) {
        RootedString flagStr(cx, ToString<CanGC>(cx, args[1]));
        if (!flagStr)
            return false;
        args[1].setString(flagStr);
        if (!ParseRegExpFlags(cx, flagStr, &flags))
            return false;
    }

    RootedAtom escapedSourceStr(cx, EscapeRegExpPattern(cx, source));
    if (!escapedSourceStr)
        return false;

    CompileOptions options(cx);
    frontend::TokenStream dummyTokenStream(cx, options, nullptr, 0, nullptr);

    if (!irregexp::ParsePatternSyntax(dummyTokenStream, cx->tempLifoAlloc(), escapedSourceStr))
        return false;

    if (staticsUse == UseRegExpStatics) {
        RegExpStatics* res = cx->global()->getRegExpStatics(cx);
        if (!res)
            return false;
        flags = RegExpFlag(flags | res->getFlags());
    }
    RegExpObject* reobj = builder.build(escapedSourceStr, flags);
    if (!reobj)
        return false;

    args.rval().setObject(*reobj);
    return true;
}

MOZ_ALWAYS_INLINE bool
IsRegExp(HandleValue v)
{
    return v.isObject() && v.toObject().is<RegExpObject>();
}

MOZ_ALWAYS_INLINE bool
regexp_compile_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(IsRegExp(args.thisv()));
    RegExpObjectBuilder builder(cx, &args.thisv().toObject().as<RegExpObject>());
    return CompileRegExpObject(cx, builder, args, UseRegExpStatics);
}

static bool
regexp_compile(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExp, regexp_compile_impl>(cx, args);
}

static bool
regexp_construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!args.isConstructing()) {
        /*
         * If first arg is regexp and no flags are given, just return the arg.
         * Otherwise, delegate to the standard constructor.
         * See ECMAv5 15.10.3.1.
         */
        if (args.hasDefined(0) &&
            IsObjectWithClass(args[0], ESClass_RegExp, cx) &&
            !args.hasDefined(1))
        {
            args.rval().set(args[0]);
            return true;
        }
    }

    RegExpObjectBuilder builder(cx);
    return CompileRegExpObject(cx, builder, args, UseRegExpStatics);
}

bool
js::regexp_construct_no_statics(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    MOZ_ASSERT(args.length() == 1 || args.length() == 2);
    MOZ_ASSERT(args[0].isString());
    MOZ_ASSERT_IF(args.length() == 2, args[1].isString());
    MOZ_ASSERT(!args.isConstructing());

    RegExpObjectBuilder builder(cx);
    return CompileRegExpObject(cx, builder, args, DontUseRegExpStatics);
}

MOZ_ALWAYS_INLINE bool
regexp_toString_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(IsRegExp(args.thisv()));

    JSString* str = args.thisv().toObject().as<RegExpObject>().toString(cx);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

static bool
regexp_toString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExp, regexp_toString_impl>(cx, args);
}

/* ES6 draft rev29 21.2.5.3 RegExp.prototype.flags */
bool
regexp_flags(JSContext* cx, unsigned argc, JS::Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Steps 1-2. */
    if (!args.thisv().isObject()) {
        char* bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, args.thisv(), NullPtr());
        if (!bytes)
            return false;
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                             bytes, "not an object");
        js_free(bytes);
        return false;
    }
    RootedObject thisObj(cx, &args.thisv().toObject());

    /* Step 3. */
    StringBuffer sb(cx);

    /* Steps 4-6. */
    RootedValue global(cx);
    if (!GetProperty(cx, thisObj, thisObj, cx->names().global, &global))
        return false;
    if (ToBoolean(global) && !sb.append('g'))
        return false;

    /* Steps 7-9. */
    RootedValue ignoreCase(cx);
    if (!GetProperty(cx, thisObj, thisObj, cx->names().ignoreCase, &ignoreCase))
        return false;
    if (ToBoolean(ignoreCase) && !sb.append('i'))
        return false;

    /* Steps 10-12. */
    RootedValue multiline(cx);
    if (!GetProperty(cx, thisObj, thisObj, cx->names().multiline, &multiline))
        return false;
    if (ToBoolean(multiline) && !sb.append('m'))
        return false;

    /* Steps 13-15. */
    RootedValue unicode(cx);
    if (!GetProperty(cx, thisObj, thisObj, cx->names().unicode, &unicode))
        return false;
    if (ToBoolean(unicode) && !sb.append('u'))
        return false;

    /* Steps 16-18. */
    RootedValue sticky(cx);
    if (!GetProperty(cx, thisObj, thisObj, cx->names().sticky, &sticky))
        return false;
    if (ToBoolean(sticky) && !sb.append('y'))
        return false;

    /* Step 19. */
    args.rval().setString(sb.finishString());
    return true;
}

/* ES6 draft rev32 21.2.5.4. */
MOZ_ALWAYS_INLINE bool
regexp_global_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(IsRegExp(args.thisv()));
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
    return CallNonGenericMethod<IsRegExp, regexp_global_impl>(cx, args);
}

/* ES6 draft rev32 21.2.5.5. */
MOZ_ALWAYS_INLINE bool
regexp_ignoreCase_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(IsRegExp(args.thisv()));
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
    return CallNonGenericMethod<IsRegExp, regexp_ignoreCase_impl>(cx, args);
}

/* ES6 draft rev32 21.2.5.7. */
MOZ_ALWAYS_INLINE bool
regexp_multiline_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(IsRegExp(args.thisv()));
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
    return CallNonGenericMethod<IsRegExp, regexp_multiline_impl>(cx, args);
}

/* ES6 draft rev32 21.2.5.12. */
MOZ_ALWAYS_INLINE bool
regexp_sticky_impl(JSContext* cx, CallArgs args)
{
    MOZ_ASSERT(IsRegExp(args.thisv()));
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
    return CallNonGenericMethod<IsRegExp, regexp_sticky_impl>(cx, args);
}

static const JSPropertySpec regexp_properties[] = {
    JS_PSG("flags", regexp_flags, 0),
    JS_PSG("global", regexp_global, 0),
    JS_PSG("ignoreCase", regexp_ignoreCase, 0),
    JS_PSG("multiline", regexp_multiline, 0),
    JS_PSG("sticky", regexp_sticky, 0),
    JS_PS_END
};

static const JSFunctionSpec regexp_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,  regexp_toString,    0,0),
#endif
    JS_FN(js_toString_str,  regexp_toString,    0,0),
    JS_FN("compile",        regexp_compile,     2,0),
    JS_FN("exec",           regexp_exec,        1,0),
    JS_FN("test",           regexp_test,        1,0),
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

static const JSPropertySpec regexp_static_props[] = {
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
js_InitRegExpClass(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->isNative());

    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());

    Rooted<RegExpObject*> proto(cx, global->createBlankPrototype<RegExpObject>(cx));
    if (!proto)
        return nullptr;
    proto->NativeObject::setPrivate(nullptr);

    HandlePropertyName empty = cx->names().emptyRegExp;
    RegExpObjectBuilder builder(cx, proto);
    if (!builder.build(empty, RegExpFlag(0)))
        return nullptr;

    if (!DefinePropertiesAndFunctions(cx, proto, regexp_properties, regexp_methods))
        return nullptr;

    RootedFunction ctor(cx);
    ctor = global->createConstructor(cx, regexp_construct, cx->names().RegExp, 2);
    if (!ctor)
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return nullptr;

    /* Add static properties to the RegExp constructor. */
    if (!JS_DefineProperties(cx, ctor, regexp_static_props))
        return nullptr;

    if (!GlobalObject::initBuiltinConstructor(cx, global, JSProto_RegExp, ctor, proto))
        return nullptr;

    return proto;
}

RegExpRunStatus
js::ExecuteRegExp(JSContext* cx, HandleObject regexp, HandleString string,
                  MatchPairs* matches, RegExpStaticsUpdate staticsUpdate)
{
    /* Step 1 (b) was performed by CallNonGenericMethod. */
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

    /* Step 3. */
    RootedLinearString input(cx, string->ensureLinear(cx));
    if (!input)
        return RegExpRunStatus_Error;

    /* Step 4. */
    RootedValue lastIndex(cx, reobj->getLastIndex());
    size_t length = input->length();

    /* Step 5. */
    int searchIndex;
    if (lastIndex.isInt32()) {
        /* Aggressively avoid doubles. */
        searchIndex = lastIndex.toInt32();
    } else {
        double d;
        if (!ToInteger(cx, lastIndex, &d))
            return RegExpRunStatus_Error;

        /* Inlined steps 6, 7, 9a with doubles to detect failure case. */
        if (reobj->needUpdateLastIndex() && (d < 0 || d > length)) {
            reobj->zeroLastIndex();
            return RegExpRunStatus_Success_NotFound;
        }

        searchIndex = int(d);
    }

    /*
     * Steps 6-7 (with sticky extension).
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

    /* Step 9a. */
    if (searchIndex < 0 || size_t(searchIndex) > length) {
        reobj->zeroLastIndex();
        return RegExpRunStatus_Success_NotFound;
    }

    /* Steps 8-21. */
    RegExpRunStatus status = ExecuteRegExpImpl(cx, res, *re, input, searchIndex, matches);
    if (status == RegExpRunStatus_Error)
        return RegExpRunStatus_Error;

    /* Steps 9a and 11 (with sticky extension). */
    if (status == RegExpRunStatus_Success_NotFound)
        reobj->zeroLastIndex();
    else if (reobj->needUpdateLastIndex())
        reobj->setLastIndex((*matches)[0].limit);

    return status;
}

/* ES5 15.10.6.2 (and 15.10.6.3, which calls 15.10.6.2). */
static RegExpRunStatus
ExecuteRegExp(JSContext* cx, CallArgs args, MatchPairs* matches)
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
regexp_exec_impl(JSContext* cx, CallArgs args)
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
    return CallNonGenericMethod(cx, IsRegExp, regexp_exec_impl, args);
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
    MOZ_ASSERT(IsRegExp(args[0]));
    MOZ_ASSERT(args[1].isString());

    RootedObject regexp(cx, &args[0].toObject());
    RootedString string(cx, args[1].toString());

    return regexp_exec_impl(cx, regexp, string, DontUpdateRegExpStatics, args.rval());
}

/* ES5 15.10.6.3. */
static bool
regexp_test_impl(JSContext* cx, CallArgs args)
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
    return CallNonGenericMethod(cx, IsRegExp, regexp_test_impl, args);
}

bool
js::regexp_test_no_statics(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(IsRegExp(args[0]));
    MOZ_ASSERT(args[1].isString());

    RootedObject regexp(cx, &args[0].toObject());
    RootedString string(cx, args[1].toString());

    RegExpRunStatus status = ExecuteRegExp(cx, regexp, string, nullptr, DontUpdateRegExpStatics);
    args.rval().setBoolean(status == RegExpRunStatus_Success);
    return status != RegExpRunStatus_Error;
}
