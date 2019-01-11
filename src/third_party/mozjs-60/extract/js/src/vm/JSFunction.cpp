/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS function support.
 */

#include "vm/JSFunction-inl.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"

#include <string.h>

#include "jsapi.h"
#include "jsarray.h"
#include "jstypes.h"

#include "builtin/Eval.h"
#include "builtin/Object.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/String.h"
#include "frontend/BytecodeCompiler.h"
#include "frontend/TokenStream.h"
#include "gc/Marking.h"
#include "gc/Policy.h"
#include "jit/InlinableNatives.h"
#include "jit/Ion.h"
#include "js/CallNonGenericMethod.h"
#include "js/Proxy.h"
#include "js/Wrapper.h"
#include "util/StringBuffer.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/Debugger.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/SharedImmutableStringsCache.h"
#include "vm/WrapperObject.h"
#include "vm/Xdr.h"
#include "wasm/AsmJS.h"

#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::gc;
using namespace js::frontend;

using mozilla::ArrayLength;
using mozilla::Maybe;
using mozilla::Some;

static bool
fun_enumerate(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->is<JSFunction>());

    RootedId id(cx);
    bool found;

    if (!obj->isBoundFunction() && !obj->as<JSFunction>().isArrow()) {
        id = NameToId(cx->names().prototype);
        if (!HasOwnProperty(cx, obj, id, &found))
            return false;
    }

    if (!obj->as<JSFunction>().hasResolvedLength()) {
        id = NameToId(cx->names().length);
        if (!HasOwnProperty(cx, obj, id, &found))
            return false;
    }

    if (!obj->as<JSFunction>().hasResolvedName()) {
        id = NameToId(cx->names().name);
        if (!HasOwnProperty(cx, obj, id, &found))
            return false;
    }

    return true;
}

bool
IsFunction(HandleValue v)
{
    return v.isObject() && v.toObject().is<JSFunction>();
}

static bool
AdvanceToActiveCallLinear(JSContext* cx, NonBuiltinScriptFrameIter& iter, HandleFunction fun)
{
    MOZ_ASSERT(!fun->isBuiltin());

    for (; !iter.done(); ++iter) {
        if (!iter.isFunctionFrame())
            continue;
        if (iter.matchCallee(cx, fun))
            return true;
    }
    return false;
}

void
js::ThrowTypeErrorBehavior(JSContext* cx)
{
    JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
                                     JSMSG_THROW_TYPE_ERROR);
}

static bool
IsSloppyNormalFunction(JSFunction* fun)
{
    // FunctionDeclaration or FunctionExpression in sloppy mode.
    if (fun->kind() == JSFunction::NormalFunction) {
        if (fun->isBuiltin() || fun->isBoundFunction())
            return false;

        if (fun->isGenerator() || fun->isAsync())
            return false;

        MOZ_ASSERT(fun->isInterpreted());
        return !fun->strict();
    }

    // Or asm.js function in sloppy mode.
    if (fun->kind() == JSFunction::AsmJS)
        return !IsAsmJSStrictModeModuleOrFunction(fun);

    return false;
}

// Beware: this function can be invoked on *any* function! That includes
// natives, strict mode functions, bound functions, arrow functions,
// self-hosted functions and constructors, asm.js functions, functions with
// destructuring arguments and/or a rest argument, and probably a few more I
// forgot. Turn back and save yourself while you still can. It's too late for
// me.
static bool
ArgumentsRestrictions(JSContext* cx, HandleFunction fun)
{
    // Throw unless the function is a sloppy, normal function.
    // TODO (bug 1057208): ensure semantics are correct for all possible
    // pairings of callee/caller.
    if (!IsSloppyNormalFunction(fun)) {
        ThrowTypeErrorBehavior(cx);
        return false;
    }

    // Otherwise emit a strict warning about |f.arguments| to discourage use of
    // this non-standard, performance-harmful feature.
    if (!JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_WARNING | JSREPORT_STRICT, GetErrorMessage,
                                           nullptr, JSMSG_DEPRECATED_USAGE, js_arguments_str))
    {
        return false;
    }

    return true;
}

bool
ArgumentsGetterImpl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsFunction(args.thisv()));

    RootedFunction fun(cx, &args.thisv().toObject().as<JSFunction>());
    if (!ArgumentsRestrictions(cx, fun))
        return false;

    // Return null if this function wasn't found on the stack.
    NonBuiltinScriptFrameIter iter(cx);
    if (!AdvanceToActiveCallLinear(cx, iter, fun)) {
        args.rval().setNull();
        return true;
    }

    Rooted<ArgumentsObject*> argsobj(cx, ArgumentsObject::createUnexpected(cx, iter));
    if (!argsobj)
        return false;

#ifndef JS_CODEGEN_NONE
    // Disabling compiling of this script in IonMonkey.  IonMonkey doesn't
    // guarantee |f.arguments| can be fully recovered, so we try to mitigate
    // observing this behavior by detecting its use early.
    JSScript* script = iter.script();
    jit::ForbidCompilation(cx, script);
#endif

    args.rval().setObject(*argsobj);
    return true;
}

static bool
ArgumentsGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsFunction, ArgumentsGetterImpl>(cx, args);
}

bool
ArgumentsSetterImpl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsFunction(args.thisv()));

    RootedFunction fun(cx, &args.thisv().toObject().as<JSFunction>());
    if (!ArgumentsRestrictions(cx, fun))
        return false;

    // If the function passes the gauntlet, return |undefined|.
    args.rval().setUndefined();
    return true;
}

static bool
ArgumentsSetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsFunction, ArgumentsSetterImpl>(cx, args);
}

// Beware: this function can be invoked on *any* function! That includes
// natives, strict mode functions, bound functions, arrow functions,
// self-hosted functions and constructors, asm.js functions, functions with
// destructuring arguments and/or a rest argument, and probably a few more I
// forgot. Turn back and save yourself while you still can. It's too late for
// me.
static bool
CallerRestrictions(JSContext* cx, HandleFunction fun)
{
    // Throw unless the function is a sloppy, normal function.
    // TODO (bug 1057208): ensure semantics are correct for all possible
    // pairings of callee/caller.
    if (!IsSloppyNormalFunction(fun)) {
        ThrowTypeErrorBehavior(cx);
        return false;
    }

    // Otherwise emit a strict warning about |f.caller| to discourage use of
    // this non-standard, performance-harmful feature.
    if (!JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_WARNING | JSREPORT_STRICT, GetErrorMessage,
                                           nullptr, JSMSG_DEPRECATED_USAGE, js_caller_str))
    {
        return false;
    }

    return true;
}

bool
CallerGetterImpl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsFunction(args.thisv()));

    // Beware!  This function can be invoked on *any* function!  It can't
    // assume it'll never be invoked on natives, strict mode functions, bound
    // functions, or anything else that ordinarily has immutable .caller
    // defined with [[ThrowTypeError]].
    RootedFunction fun(cx, &args.thisv().toObject().as<JSFunction>());
    if (!CallerRestrictions(cx, fun))
        return false;

    // Also return null if this function wasn't found on the stack.
    NonBuiltinScriptFrameIter iter(cx);
    if (!AdvanceToActiveCallLinear(cx, iter, fun)) {
        args.rval().setNull();
        return true;
    }

    ++iter;
    while (!iter.done() && iter.isEvalFrame())
        ++iter;

    if (iter.done() || !iter.isFunctionFrame()) {
        args.rval().setNull();
        return true;
    }

    RootedObject caller(cx, iter.callee(cx));
    if (caller->is<JSFunction>() && caller->as<JSFunction>().isAsync())
        caller = GetWrappedAsyncFunction(&caller->as<JSFunction>());
    if (!cx->compartment()->wrap(cx, &caller))
        return false;

    // Censor the caller if we don't have full access to it.  If we do, but the
    // caller is a function with strict mode code, throw a TypeError per ES5.
    // If we pass these checks, we can return the computed caller.
    {
        JSObject* callerObj = CheckedUnwrap(caller);
        if (!callerObj) {
            args.rval().setNull();
            return true;
        }

        JSFunction* callerFun = &callerObj->as<JSFunction>();
        if (IsWrappedAsyncFunction(callerFun))
            callerFun = GetUnwrappedAsyncFunction(callerFun);
        else if (IsWrappedAsyncGenerator(callerFun))
            callerFun = GetUnwrappedAsyncGenerator(callerFun);
        MOZ_ASSERT(!callerFun->isBuiltin(), "non-builtin iterator returned a builtin?");

        if (callerFun->strict()) {
            JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
                                              JSMSG_CALLER_IS_STRICT);
            return false;
        }
    }

    args.rval().setObject(*caller);
    return true;
}

static bool
CallerGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsFunction, CallerGetterImpl>(cx, args);
}

bool
CallerSetterImpl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsFunction(args.thisv()));

    // Beware!  This function can be invoked on *any* function!  It can't
    // assume it'll never be invoked on natives, strict mode functions, bound
    // functions, or anything else that ordinarily has immutable .caller
    // defined with [[ThrowTypeError]].
    RootedFunction fun(cx, &args.thisv().toObject().as<JSFunction>());
    if (!CallerRestrictions(cx, fun))
        return false;

    // Return |undefined| unless an error must be thrown.
    args.rval().setUndefined();

    // We can almost just return |undefined| here -- but if the caller function
    // was strict mode code, we still have to throw a TypeError.  This requires
    // computing the caller, checking that no security boundaries are crossed,
    // and throwing a TypeError if the resulting caller is strict.

    NonBuiltinScriptFrameIter iter(cx);
    if (!AdvanceToActiveCallLinear(cx, iter, fun))
        return true;

    ++iter;
    while (!iter.done() && iter.isEvalFrame())
        ++iter;

    if (iter.done() || !iter.isFunctionFrame())
        return true;

    RootedObject caller(cx, iter.callee(cx));
    // |caller| is only used for security access-checking and for its
    // strictness.  An unwrapped async function has its wrapped async
    // function's security access and strictness, so don't bother calling
    // |GetUnwrappedAsyncFunction|.
    if (!cx->compartment()->wrap(cx, &caller)) {
        cx->clearPendingException();
        return true;
    }

    // If we don't have full access to the caller, or the caller is not strict,
    // return undefined.  Otherwise throw a TypeError.
    JSObject* callerObj = CheckedUnwrap(caller);
    if (!callerObj)
        return true;

    JSFunction* callerFun = &callerObj->as<JSFunction>();
    MOZ_ASSERT(!callerFun->isBuiltin(), "non-builtin iterator returned a builtin?");

    if (callerFun->strict()) {
        JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
                                          JSMSG_CALLER_IS_STRICT);
        return false;
    }

    return true;
}

static bool
CallerSetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsFunction, CallerSetterImpl>(cx, args);
}

static const JSPropertySpec function_properties[] = {
    JS_PSGS("arguments", ArgumentsGetter, ArgumentsSetter, 0),
    JS_PSGS("caller", CallerGetter, CallerSetter, 0),
    JS_PS_END
};

static bool
ResolveInterpretedFunctionPrototype(JSContext* cx, HandleFunction fun, HandleId id)
{
    bool isAsyncGenerator = IsWrappedAsyncGenerator(fun);

    MOZ_ASSERT_IF(!isAsyncGenerator, fun->isInterpreted() || fun->isAsmJSNative());
    MOZ_ASSERT(id == NameToId(cx->names().prototype));

    // Assert that fun is not a compiler-created function object, which
    // must never leak to script or embedding code and then be mutated.
    // Also assert that fun is not bound, per the ES5 15.3.4.5 ref above.
    MOZ_ASSERT_IF(!isAsyncGenerator, !IsInternalFunctionObject(*fun));
    MOZ_ASSERT(!fun->isBoundFunction());

    // Make the prototype object an instance of Object with the same parent as
    // the function object itself, unless the function is an ES6 generator.  In
    // that case, per the 15 July 2013 ES6 draft, section 15.19.3, its parent is
    // the GeneratorObjectPrototype singleton.
    bool isGenerator = fun->isGenerator();
    Rooted<GlobalObject*> global(cx, &fun->global());
    RootedObject objProto(cx);
    if (isAsyncGenerator)
        objProto = GlobalObject::getOrCreateAsyncGeneratorPrototype(cx, global);
    else if (isGenerator)
        objProto = GlobalObject::getOrCreateGeneratorObjectPrototype(cx, global);
    else
        objProto = GlobalObject::getOrCreateObjectPrototype(cx, global);
    if (!objProto)
        return false;

    RootedPlainObject proto(cx, NewObjectWithGivenProto<PlainObject>(cx, objProto,
                                                                     SingletonObject));
    if (!proto)
        return false;

    // Per ES5 13.2 the prototype's .constructor property is configurable,
    // non-enumerable, and writable.  However, per the 15 July 2013 ES6 draft,
    // section 15.19.3, the .prototype of a generator function does not link
    // back with a .constructor.
    if (!isGenerator && !isAsyncGenerator) {
        RootedValue objVal(cx, ObjectValue(*fun));
        if (!DefineDataProperty(cx, proto, cx->names().constructor, objVal, 0))
            return false;
    }

    // Per ES5 15.3.5.2 a user-defined function's .prototype property is
    // initially non-configurable, non-enumerable, and writable.
    RootedValue protoVal(cx, ObjectValue(*proto));
    return DefineDataProperty(cx, fun, id, protoVal, JSPROP_PERMANENT | JSPROP_RESOLVING);
}

bool
JSFunction::needsPrototypeProperty()
{
    /*
     * Built-in functions do not have a .prototype property per ECMA-262,
     * or (Object.prototype, Function.prototype, etc.) have that property
     * created eagerly.
     *
     * ES5 15.3.4.5: bound functions don't have a prototype property. The
     * isBuiltin() test covers this case because bound functions are native
     * (and thus built-in) functions by definition/construction.
     *
     * ES6 9.2.8 MakeConstructor defines the .prototype property on constructors.
     * Generators are not constructors, but they have a .prototype property anyway,
     * according to errata to ES6. See bug 1191486.
     *
     * Thus all of the following don't get a .prototype property:
     * - Methods (that are not class-constructors or generators)
     * - Arrow functions
     * - Function.prototype
     */
    if (isBuiltin())
        return IsWrappedAsyncGenerator(this);

    return isConstructor() || isGenerator() || isAsync();
}

static bool
fun_mayResolve(const JSAtomState& names, jsid id, JSObject*)
{
    if (!JSID_IS_ATOM(id))
        return false;

    JSAtom* atom = JSID_TO_ATOM(id);
    return atom == names.prototype || atom == names.length || atom == names.name;
}

static bool
fun_resolve(JSContext* cx, HandleObject obj, HandleId id, bool* resolvedp)
{
    if (!JSID_IS_ATOM(id))
        return true;

    RootedFunction fun(cx, &obj->as<JSFunction>());

    if (JSID_IS_ATOM(id, cx->names().prototype)) {
        if (!fun->needsPrototypeProperty())
            return true;

        if (!ResolveInterpretedFunctionPrototype(cx, fun, id))
            return false;

        *resolvedp = true;
        return true;
    }

    bool isLength = JSID_IS_ATOM(id, cx->names().length);
    if (isLength || JSID_IS_ATOM(id, cx->names().name)) {
        MOZ_ASSERT(!IsInternalFunctionObject(*obj));

        RootedValue v(cx);

        // Since f.length and f.name are configurable, they could be resolved
        // and then deleted:
        //     function f(x) {}
        //     assertEq(f.length, 1);
        //     delete f.length;
        //     assertEq(f.name, "f");
        //     delete f.name;
        // Afterwards, asking for f.length or f.name again will cause this
        // resolve hook to run again. Defining the property again the second
        // time through would be a bug.
        //     assertEq(f.length, 0);  // gets Function.prototype.length!
        //     assertEq(f.name, "");  // gets Function.prototype.name!
        // We use the RESOLVED_LENGTH and RESOLVED_NAME flags as a hack to prevent this
        // bug.
        if (isLength) {
            if (fun->hasResolvedLength())
                return true;

            if (!JSFunction::getUnresolvedLength(cx, fun, &v))
                return false;
        } else {
            if (fun->hasResolvedName())
                return true;

            RootedAtom name(cx);
            if (!JSFunction::getUnresolvedName(cx, fun, &name))
                return false;

            // Don't define an own .name property for unnamed functions.
            if (!name)
                return true;

            v.setString(name);
        }

        if (!NativeDefineDataProperty(cx, fun, id, v, JSPROP_READONLY | JSPROP_RESOLVING))
            return false;

        if (isLength)
            fun->setResolvedLength();
        else
            fun->setResolvedName();

        *resolvedp = true;
        return true;
    }

    return true;
}

template<XDRMode mode>
bool
js::XDRInterpretedFunction(XDRState<mode>* xdr, HandleScope enclosingScope,
                           HandleScriptSource sourceObject, MutableHandleFunction objp)
{
    enum FirstWordFlag {
        HasAtom             = 0x1,
        HasGeneratorProto   = 0x2,
        IsLazy              = 0x4,
        HasSingletonType    = 0x8
    };

    /* NB: Keep this in sync with CloneInnerInterpretedFunction. */
    RootedAtom atom(xdr->cx());
    uint32_t firstword = 0;        /* bitmask of FirstWordFlag */
    uint32_t flagsword = 0;        /* word for argument count and fun->flags */

    JSContext* cx = xdr->cx();
    RootedFunction fun(cx);
    RootedScript script(cx);
    Rooted<LazyScript*> lazy(cx);

    if (mode == XDR_ENCODE) {
        fun = objp;
        if (!fun->isInterpreted())
            return xdr->fail(JS::TranscodeResult_Failure_NotInterpretedFun);

        if (fun->explicitName() || fun->hasCompileTimeName() || fun->hasGuessedAtom())
            firstword |= HasAtom;

        if (fun->isGenerator() || fun->isAsync())
            firstword |= HasGeneratorProto;

        if (fun->isInterpretedLazy()) {
            // Encode a lazy script.
            firstword |= IsLazy;
            lazy = fun->lazyScript();
        } else {
            // Encode the script.
            script = fun->nonLazyScript();
        }

        if (fun->isSingleton())
            firstword |= HasSingletonType;

        atom = fun->displayAtom();
        flagsword = (fun->nargs() << 16) |
                    (fun->flags() & ~JSFunction::NO_XDR_FLAGS);

        // The environment of any function which is not reused will always be
        // null, it is later defined when a function is cloned or reused to
        // mirror the scope chain.
        MOZ_ASSERT_IF(fun->isSingleton() &&
                      !((lazy && lazy->hasBeenCloned()) || (script && script->hasBeenCloned())),
                      fun->environment() == nullptr);
    }

    // Everything added below can substituted by the non-lazy-script version of
    // this function later.
    js::AutoXDRTree funTree(xdr, xdr->getTreeKey(fun));

    if (!xdr->codeUint32(&firstword))
        return false;

    if ((firstword & HasAtom) && !XDRAtom(xdr, &atom))
        return false;
    if (!xdr->codeUint32(&flagsword))
        return false;

    if (mode == XDR_DECODE) {
        RootedObject proto(cx);
        if (firstword & HasGeneratorProto) {
            proto = GlobalObject::getOrCreateGeneratorFunctionPrototype(cx, cx->global());
            if (!proto)
                return false;
        }

        gc::AllocKind allocKind = gc::AllocKind::FUNCTION;
        if (uint16_t(flagsword) & JSFunction::EXTENDED)
            allocKind = gc::AllocKind::FUNCTION_EXTENDED;
        fun = NewFunctionWithProto(cx, nullptr, 0, JSFunction::INTERPRETED,
                                   /* enclosingDynamicScope = */ nullptr, nullptr, proto,
                                   allocKind, TenuredObject);
        if (!fun)
            return false;
        script = nullptr;
    }

    if (firstword & IsLazy) {
        if (!XDRLazyScript(xdr, enclosingScope, sourceObject, fun, &lazy))
            return false;
    } else {
        if (!XDRScript(xdr, enclosingScope, sourceObject, fun, &script))
            return false;
    }

    if (mode == XDR_DECODE) {
        fun->setArgCount(flagsword >> 16);
        fun->setFlags(uint16_t(flagsword));
        fun->initAtom(atom);
        if (firstword & IsLazy) {
            MOZ_ASSERT(fun->lazyScript() == lazy);
        } else {
            MOZ_ASSERT(fun->nonLazyScript() == script);
            MOZ_ASSERT(fun->nargs() == script->numArgs());
        }

        bool singleton = firstword & HasSingletonType;
        if (!JSFunction::setTypeForScriptedFunction(cx, fun, singleton))
            return false;
        objp.set(fun);
    }

    // Verify marker at end of function to detect buffer trunction.
    if (!xdr->codeMarker(0x9E35CA1F))
        return false;

    return true;
}

template bool
js::XDRInterpretedFunction(XDRState<XDR_ENCODE>*, HandleScope, HandleScriptSource,
                           MutableHandleFunction);

template bool
js::XDRInterpretedFunction(XDRState<XDR_DECODE>*, HandleScope, HandleScriptSource,
                           MutableHandleFunction);

/* ES6 (04-25-16) 19.2.3.6 Function.prototype [ @@hasInstance ] */
bool
js::fun_symbolHasInstance(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() < 1) {
        args.rval().setBoolean(false);
        return true;
    }

    /* Step 1. */
    HandleValue func = args.thisv();

    // Primitives are non-callable and will always return false from
    // OrdinaryHasInstance.
    if (!func.isObject()) {
        args.rval().setBoolean(false);
        return true;
    }

    RootedObject obj(cx, &func.toObject());

    /* Step 2. */
    bool result;
    if (!OrdinaryHasInstance(cx, obj, args[0], &result))
        return false;

    args.rval().setBoolean(result);
    return true;
}

/*
 * ES6 (4-25-16) 7.3.19 OrdinaryHasInstance
 */
bool
JS::OrdinaryHasInstance(JSContext* cx, HandleObject objArg, HandleValue v, bool* bp)
{
    AssertHeapIsIdle();
    assertSameCompartment(cx, objArg, v);

    RootedObject obj(cx, objArg);

    /* Step 1. */
    if (!obj->isCallable()) {
        *bp = false;
        return true;
    }

    /* Step 2. */
    if (obj->is<JSFunction>() && obj->isBoundFunction()) {
        /* Steps 2a-b. */
        obj = obj->as<JSFunction>().getBoundFunctionTarget();
        return InstanceOfOperator(cx, obj, v, bp);
    }

    /* Step 3. */
    if (!v.isObject()) {
        *bp = false;
        return true;
    }

    /* Step 4. */
    RootedValue pval(cx);
    if (!GetProperty(cx, obj, obj, cx->names().prototype, &pval))
        return false;

    /* Step 5. */
    if (pval.isPrimitive()) {
        /*
         * Throw a runtime error if instanceof is called on a function that
         * has a non-object as its .prototype value.
         */
        RootedValue val(cx, ObjectValue(*obj));
        ReportValueError(cx, JSMSG_BAD_PROTOTYPE, -1, val, nullptr);
        return false;
    }

    /* Step 6. */
    RootedObject pobj(cx, &pval.toObject());
    bool isDelegate;
    if (!IsDelegate(cx, pobj, v, &isDelegate))
        return false;
    *bp = isDelegate;
    return true;
}

inline void
JSFunction::trace(JSTracer* trc)
{
    if (isExtended()) {
        TraceRange(trc, ArrayLength(toExtended()->extendedSlots),
                   (GCPtrValue*)toExtended()->extendedSlots, "nativeReserved");
    }

    TraceNullableEdge(trc, &atom_, "atom");

    if (isInterpreted()) {
        // Functions can be be marked as interpreted despite having no script
        // yet at some points when parsing, and can be lazy with no lazy script
        // for self-hosted code.
        if (hasScript() && !hasUncompiledScript())
            TraceManuallyBarrieredEdge(trc, &u.scripted.s.script_, "script");
        else if (isInterpretedLazy() && u.scripted.s.lazy_)
            TraceManuallyBarrieredEdge(trc, &u.scripted.s.lazy_, "lazyScript");

        if (u.scripted.env_)
            TraceManuallyBarrieredEdge(trc, &u.scripted.env_, "fun_environment");
    }
}

static void
fun_trace(JSTracer* trc, JSObject* obj)
{
    obj->as<JSFunction>().trace(trc);
}

static JSObject*
CreateFunctionConstructor(JSContext* cx, JSProtoKey key)
{
    Rooted<GlobalObject*> global(cx, cx->global());
    RootedObject functionProto(cx, &global->getPrototype(JSProto_Function).toObject());

    RootedObject functionCtor(cx,
      NewFunctionWithProto(cx, Function, 1, JSFunction::NATIVE_CTOR,
                           nullptr, HandlePropertyName(cx->names().Function),
                           functionProto, AllocKind::FUNCTION, SingletonObject));
    if (!functionCtor)
        return nullptr;

    return functionCtor;
}

static JSObject*
CreateFunctionPrototype(JSContext* cx, JSProtoKey key)
{
    Rooted<GlobalObject*> self(cx, cx->global());

    RootedObject objectProto(cx, &self->getPrototype(JSProto_Object).toObject());
    /*
     * Bizarrely, |Function.prototype| must be an interpreted function, so
     * give it the guts to be one.
     */
    RootedObject enclosingEnv(cx, &self->lexicalEnvironment());
    JSObject* functionProto_ =
        NewFunctionWithProto(cx, nullptr, 0, JSFunction::INTERPRETED,
                             enclosingEnv, nullptr, objectProto, AllocKind::FUNCTION,
                             SingletonObject);
    if (!functionProto_)
        return nullptr;

    RootedFunction functionProto(cx, &functionProto_->as<JSFunction>());

    const char* rawSource = "function () {\n}";
    size_t sourceLen = strlen(rawSource);
    size_t begin = 9;
    MOZ_ASSERT(rawSource[begin] == '(');
    mozilla::UniquePtr<char16_t[], JS::FreePolicy> source(InflateString(cx, rawSource, sourceLen));
    if (!source)
        return nullptr;

    ScriptSource* ss = cx->new_<ScriptSource>();
    if (!ss)
        return nullptr;
    ScriptSourceHolder ssHolder(ss);
    if (!ss->setSource(cx, mozilla::Move(source), sourceLen))
        return nullptr;

    CompileOptions options(cx);
    options.setIntroductionType("Function.prototype")
           .setNoScriptRval(true);
    if (!ss->initFromOptions(cx, options))
        return nullptr;
    RootedScriptSource sourceObject(cx, ScriptSourceObject::create(cx, ss));
    if (!sourceObject || !ScriptSourceObject::initFromOptions(cx, sourceObject, options))
        return nullptr;

    RootedScript script(cx, JSScript::Create(cx,
                                             options,
                                             sourceObject,
                                             begin,
                                             ss->length(),
                                             0,
                                             ss->length()));
    if (!script || !JSScript::initFunctionPrototype(cx, script, functionProto))
        return nullptr;

    functionProto->initScript(script);
    ObjectGroup* protoGroup = JSObject::getGroup(cx, functionProto);
    if (!protoGroup)
        return nullptr;

    protoGroup->setInterpretedFunction(functionProto);

    /*
     * The default 'new' group of Function.prototype is required by type
     * inference to have unknown properties, to simplify handling of e.g.
     * NewFunctionClone.
     */
    if (!JSObject::setNewGroupUnknown(cx, &JSFunction::class_, functionProto))
        return nullptr;

    return functionProto;
}

static const ClassOps JSFunctionClassOps = {
    nullptr,                 /* addProperty */
    nullptr,                 /* delProperty */
    fun_enumerate,
    nullptr,                 /* newEnumerate */
    fun_resolve,
    fun_mayResolve,
    nullptr,                 /* finalize    */
    nullptr,                 /* call        */
    nullptr,
    nullptr,                 /* construct   */
    fun_trace,
};

static const ClassSpec JSFunctionClassSpec = {
    CreateFunctionConstructor,
    CreateFunctionPrototype,
    nullptr,
    nullptr,
    function_methods,
    function_properties
};

const Class JSFunction::class_ = {
    js_Function_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Function),
    &JSFunctionClassOps,
    &JSFunctionClassSpec
};

const Class* const js::FunctionClassPtr = &JSFunction::class_;

JSString*
js::FunctionToStringCache::lookup(JSScript* script) const
{
    for (size_t i = 0; i < NumEntries; i++) {
        if (entries_[i].script == script)
            return entries_[i].string;
    }
    return nullptr;
}

void
js::FunctionToStringCache::put(JSScript* script, JSString* string)
{
    for (size_t i = NumEntries - 1; i > 0; i--)
        entries_[i] = entries_[i - 1];

    entries_[0].set(script, string);
}

JSString*
js::FunctionToString(JSContext* cx, HandleFunction fun, bool isToSource)
{
    if (fun->isInterpretedLazy() && !JSFunction::getOrCreateScript(cx, fun))
        return nullptr;

    if (IsAsmJSModule(fun))
        return AsmJSModuleToString(cx, fun, isToSource);
    if (IsAsmJSFunction(fun))
        return AsmJSFunctionToString(cx, fun);

    if (IsWrappedAsyncFunction(fun)) {
        RootedFunction unwrapped(cx, GetUnwrappedAsyncFunction(fun));
        return FunctionToString(cx, unwrapped, isToSource);
    }
    if (IsWrappedAsyncGenerator(fun)) {
        RootedFunction unwrapped(cx, GetUnwrappedAsyncGenerator(fun));
        return FunctionToString(cx, unwrapped, isToSource);
    }

    RootedScript script(cx);
    if (fun->hasScript())
        script = fun->nonLazyScript();

    // Default class constructors are self-hosted, but have their source
    // objects overridden to refer to the span of the class statement or
    // expression. Non-default class constructors are never self-hosted. So,
    // all class constructors always have source.
    bool haveSource = fun->isInterpreted() && (fun->isClassConstructor() ||
                                               !fun->isSelfHostedBuiltin());

    // If we're in toSource mode, put parentheses around lambda functions so
    // that eval returns lambda, not function statement.
    bool addParentheses = haveSource && isToSource && (fun->isLambda() && !fun->isArrow());

    if (haveSource && !script->scriptSource()->hasSourceData() &&
        !JSScript::loadSource(cx, script->scriptSource(), &haveSource))
    {
        return nullptr;
    }

    // Fast path for the common case, to avoid StringBuffer overhead.
    if (!addParentheses && haveSource) {
        FunctionToStringCache& cache = cx->zone()->functionToStringCache();
        if (JSString* str = cache.lookup(script))
            return str;

        size_t start = script->toStringStart(), end = script->toStringEnd();
        JSString* str = (end - start <= ScriptSource::SourceDeflateLimit)
            ? script->scriptSource()->substring(cx, start, end)
            : script->scriptSource()->substringDontDeflate(cx, start, end);
        if (!str)
            return nullptr;

        cache.put(script, str);
        return str;
    }

    StringBuffer out(cx);
    if (addParentheses) {
        if (!out.append('('))
            return nullptr;
    }

    if (haveSource) {
        if (!script->appendSourceDataForToString(cx, out))
            return nullptr;
    } else {
        if (fun->isAsync()) {
            if (!out.append("async "))
                return nullptr;
        }

        if (!fun->isArrow()) {
            if (!out.append("function"))
                return nullptr;

            if (fun->isGenerator()) {
                if (!out.append('*'))
                    return nullptr;
            }
        }

        if (fun->explicitName()) {
            if (!out.append(' '))
                return nullptr;
            if (fun->isBoundFunction() && !fun->hasBoundFunctionNamePrefix()) {
                if (!out.append(cx->names().boundWithSpace))
                    return nullptr;
            }
            if (!out.append(fun->explicitName()))
                return nullptr;
        }

        if (fun->isInterpreted() &&
            (!fun->isSelfHostedBuiltin() ||
             fun->infallibleIsDefaultClassConstructor(cx)))
        {
            // Default class constructors should always haveSource except;
            //
            // 1. Source has been discarded for the whole compartment.
            //
            // 2. The source is marked as "lazy", i.e., retrieved on demand, and
            // the embedding has not provided a hook to retrieve sources.
            MOZ_ASSERT_IF(fun->infallibleIsDefaultClassConstructor(cx),
                          !cx->runtime()->sourceHook.ref() ||
                          !script->scriptSource()->sourceRetrievable() ||
                          fun->compartment()->behaviors().discardSource());

            if (!out.append("() {\n    [sourceless code]\n}"))
                return nullptr;
        } else {
            if (!out.append("() {\n    [native code]\n}"))
                return nullptr;
        }
    }

    if (addParentheses) {
        if (!out.append(')'))
            return nullptr;
    }

    return out.finishString();
}

JSString*
fun_toStringHelper(JSContext* cx, HandleObject obj, bool isToSource)
{
    if (!obj->is<JSFunction>()) {
        if (JSFunToStringOp op = obj->getOpsFunToString())
            return op(cx, obj, isToSource);

        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_INCOMPATIBLE_PROTO,
                                  js_Function_str, js_toString_str, "object");
        return nullptr;
    }

    RootedFunction fun(cx, &obj->as<JSFunction>());
    return FunctionToString(cx, fun, isToSource);
}

bool
js::FunctionHasDefaultHasInstance(JSFunction* fun, const WellKnownSymbols& symbols)
{
    jsid id = SYMBOL_TO_JSID(symbols.hasInstance);
    Shape* shape = fun->lookupPure(id);
    if (shape) {
        if (!shape->isDataProperty())
            return false;
        const Value hasInstance = fun->as<NativeObject>().getSlot(shape->slot());
        return IsNativeFunction(hasInstance, js::fun_symbolHasInstance);
    }
    return true;
}

bool
js::fun_toString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(IsFunctionObject(args.calleev()));

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    JSString* str = fun_toStringHelper(cx, obj, /* isToSource = */ false);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

static bool
fun_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(IsFunctionObject(args.calleev()));

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    RootedString str(cx);
    if (obj->isCallable())
        str = fun_toStringHelper(cx, obj, /* isToSource = */ true);
    else
        str = ObjectToSource(cx, obj);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

bool
js::fun_call(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    HandleValue func = args.thisv();

    // We don't need to do this -- Call would do it for us -- but the error
    // message is *much* better if we do this here.  (Without this,
    // JSDVG_SEARCH_STACK tries to decompile |func| as if it were |this| in
    // the scripted caller's frame -- so for example
    //
    //   Function.prototype.call.call({});
    //
    // would identify |{}| as |this| as being the result of evaluating
    // |Function.prototype.call| and would conclude, "Function.prototype.call
    // is not a function".  Grotesque.)
    if (!IsCallable(func)) {
        ReportIncompatibleMethod(cx, args, &JSFunction::class_);
        return false;
    }

    size_t argCount = args.length();
    if (argCount > 0)
        argCount--; // strip off provided |this|

    InvokeArgs iargs(cx);
    if (!iargs.init(cx, argCount))
        return false;

    for (size_t i = 0; i < argCount; i++)
        iargs[i].set(args[i + 1]);

    return Call(cx, func, args.get(0), iargs, args.rval());
}

// ES5 15.3.4.3
bool
js::fun_apply(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    //
    // Note that we must check callability here, not at actual call time,
    // because extracting argument values from the provided arraylike might
    // have side effects or throw an exception.
    HandleValue fval = args.thisv();
    if (!IsCallable(fval)) {
        ReportIncompatibleMethod(cx, args, &JSFunction::class_);
        return false;
    }

    // Step 2.
    if (args.length() < 2 || args[1].isNullOrUndefined())
        return fun_call(cx, (args.length() > 0) ? 1 : 0, vp);

    InvokeArgs args2(cx);

    // A JS_OPTIMIZED_ARGUMENTS magic value means that 'arguments' flows into
    // this apply call from a scripted caller and, as an optimization, we've
    // avoided creating it since apply can simply pull the argument values from
    // the calling frame (which we must do now).
    if (args[1].isMagic(JS_OPTIMIZED_ARGUMENTS)) {
        // Step 3-6.
        ScriptFrameIter iter(cx);
        MOZ_ASSERT(iter.numActualArgs() <= ARGS_LENGTH_MAX);
        if (!args2.init(cx, iter.numActualArgs()))
            return false;

        // Steps 7-8.
        iter.unaliasedForEachActual(cx, CopyTo(args2.array()));
    } else {
        // Step 3.
        if (!args[1].isObject()) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_BAD_APPLY_ARGS, js_apply_str);
            return false;
        }

        // Steps 4-5 (note erratum removing steps originally numbered 5 and 7 in
        // original version of ES5).
        RootedObject aobj(cx, &args[1].toObject());
        uint32_t length;
        if (!GetLengthProperty(cx, aobj, &length))
            return false;

        // Step 6.
        if (!args2.init(cx, length))
            return false;

        MOZ_ASSERT(length <= ARGS_LENGTH_MAX);

        // Steps 7-8.
        if (!GetElements(cx, aobj, length, args2.array()))
            return false;
    }

    // Step 9.
    return Call(cx, fval, args[0], args2, args.rval());
}

bool
JSFunction::infallibleIsDefaultClassConstructor(JSContext* cx) const
{
    if (!isSelfHostedBuiltin())
        return false;

    bool isDefault = false;
    if (isInterpretedLazy()) {
        JSAtom* name = &getExtendedSlot(LAZY_FUNCTION_NAME_SLOT).toString()->asAtom();
        isDefault = name == cx->names().DefaultDerivedClassConstructor ||
                    name == cx->names().DefaultBaseClassConstructor;
    } else {
        isDefault = nonLazyScript()->isDefaultClassConstructor();
    }

    MOZ_ASSERT_IF(isDefault, isConstructor());
    MOZ_ASSERT_IF(isDefault, isClassConstructor());
    return isDefault;
}

bool
JSFunction::isDerivedClassConstructor()
{
    bool derived;
    if (isInterpretedLazy()) {
        // There is only one plausible lazy self-hosted derived
        // constructor.
        if (isSelfHostedBuiltin()) {
            JSAtom* name = &getExtendedSlot(LAZY_FUNCTION_NAME_SLOT).toString()->asAtom();

            // This function is called from places without access to a
            // JSContext. Trace some plumbing to get what we want.
            derived = name == compartment()->runtimeFromAnyThread()->
                              commonNames->DefaultDerivedClassConstructor;
        } else {
            derived = lazyScript()->isDerivedClassConstructor();
        }
    } else {
        derived = nonLazyScript()->isDerivedClassConstructor();
    }
    MOZ_ASSERT_IF(derived, isClassConstructor());
    return derived;
}

/* static */ bool
JSFunction::getLength(JSContext* cx, HandleFunction fun, uint16_t* length)
{
    MOZ_ASSERT(!fun->isBoundFunction());
    if (fun->isInterpretedLazy() && !getOrCreateScript(cx, fun))
        return false;

    *length = fun->isNative() ? fun->nargs() : fun->nonLazyScript()->funLength();
    return true;
}

/* static */ bool
JSFunction::getUnresolvedLength(JSContext* cx, HandleFunction fun, MutableHandleValue v)
{
    MOZ_ASSERT(!IsInternalFunctionObject(*fun));
    MOZ_ASSERT(!fun->hasResolvedLength());

    // Bound functions' length can have values up to MAX_SAFE_INTEGER, so
    // they're handled differently from other functions.
    if (fun->isBoundFunction()) {
        MOZ_ASSERT(fun->getExtendedSlot(BOUND_FUN_LENGTH_SLOT).isNumber());
        v.set(fun->getExtendedSlot(BOUND_FUN_LENGTH_SLOT));
        return true;
    }

    uint16_t length;
    if (!JSFunction::getLength(cx, fun, &length))
        return false;

    v.setInt32(length);
    return true;
}

/* static */ bool
JSFunction::getUnresolvedName(JSContext* cx, HandleFunction fun, MutableHandleAtom v)
{
    MOZ_ASSERT(!IsInternalFunctionObject(*fun));
    MOZ_ASSERT(!fun->hasResolvedName());

    JSAtom* name = fun->explicitOrCompileTimeName();
    if (fun->isClassConstructor()) {
        // It's impossible to have an empty named class expression. We use
        // empty as a sentinel when creating default class constructors.
        MOZ_ASSERT(name != cx->names().empty);

        // Unnamed class expressions should not get a .name property at all.
        if (name)
            v.set(name);
        return true;
    }

    if (fun->isBoundFunction() && !fun->hasBoundFunctionNamePrefix()) {
        // Bound functions are never unnamed.
        MOZ_ASSERT(name);

        if (name->length() > 0) {
            StringBuffer sb(cx);
            if (!sb.append(cx->names().boundWithSpace) || !sb.append(name))
                return false;

            name = sb.finishAtom();
            if (!name)
                return false;
        } else {
            name = cx->names().boundWithSpace;
        }

        fun->setPrefixedBoundFunctionName(name);
    }

    v.set(name != nullptr ? name : cx->names().empty);
    return true;
}

static const js::Value&
BoundFunctionEnvironmentSlotValue(const JSFunction* fun, uint32_t slotIndex)
{
    MOZ_ASSERT(fun->isBoundFunction());
    MOZ_ASSERT(fun->environment()->is<CallObject>());
    CallObject* callObject = &fun->environment()->as<CallObject>();
    return callObject->getSlot(slotIndex);
}

JSObject*
JSFunction::getBoundFunctionTarget() const
{
    js::Value targetVal = BoundFunctionEnvironmentSlotValue(this, JSSLOT_BOUND_FUNCTION_TARGET);
    MOZ_ASSERT(IsCallable(targetVal));
    return &targetVal.toObject();
}

const js::Value&
JSFunction::getBoundFunctionThis() const
{
    return BoundFunctionEnvironmentSlotValue(this, JSSLOT_BOUND_FUNCTION_THIS);
}

static ArrayObject*
GetBoundFunctionArguments(const JSFunction* boundFun)
{
    js::Value argsVal = BoundFunctionEnvironmentSlotValue(boundFun, JSSLOT_BOUND_FUNCTION_ARGS);
    return &argsVal.toObject().as<ArrayObject>();
}

const js::Value&
JSFunction::getBoundFunctionArgument(unsigned which) const
{
    MOZ_ASSERT(which < getBoundFunctionArgumentCount());
    return GetBoundFunctionArguments(this)->getDenseElement(which);
}

size_t
JSFunction::getBoundFunctionArgumentCount() const
{
    return GetBoundFunctionArguments(this)->length();
}

/* static */ bool
JSFunction::finishBoundFunctionInit(JSContext* cx, HandleFunction bound, HandleObject targetObj,
                                    int32_t argCount)
{
    bound->setIsBoundFunction();
    MOZ_ASSERT(bound->getBoundFunctionTarget() == targetObj);

    // 9.4.1.3 BoundFunctionCreate, steps 1, 3-5, 8-12 (Already performed).

    // 9.4.1.3 BoundFunctionCreate, step 6.
    if (targetObj->isConstructor())
        bound->setIsConstructor();

    // 9.4.1.3 BoundFunctionCreate, step 2.
    RootedObject proto(cx);
    if (!GetPrototype(cx, targetObj, &proto))
        return false;

    // 9.4.1.3 BoundFunctionCreate, step 7.
    if (bound->staticPrototype() != proto) {
        if (!SetPrototype(cx, bound, proto))
            return false;
    }

    double length = 0.0;

    // Try to avoid invoking the resolve hook.
    if (targetObj->is<JSFunction>() && !targetObj->as<JSFunction>().hasResolvedLength()) {
        RootedValue targetLength(cx);
        if (!JSFunction::getUnresolvedLength(cx, targetObj.as<JSFunction>(), &targetLength))
            return false;

        length = Max(0.0, targetLength.toNumber() - argCount);
    } else {
        // 19.2.3.2 Function.prototype.bind, step 5.
        bool hasLength;
        RootedId idRoot(cx, NameToId(cx->names().length));
        if (!HasOwnProperty(cx, targetObj, idRoot, &hasLength))
            return false;

        // 19.2.3.2 Function.prototype.bind, step 6.
        if (hasLength) {
            RootedValue targetLength(cx);
            if (!GetProperty(cx, targetObj, targetObj, idRoot, &targetLength))
                return false;

            if (targetLength.isNumber())
                length = Max(0.0, JS::ToInteger(targetLength.toNumber()) - argCount);
        }

        // 19.2.3.2 Function.prototype.bind, step 7 (implicit).
    }

    // 19.2.3.2 Function.prototype.bind, step 8.
    bound->setExtendedSlot(BOUND_FUN_LENGTH_SLOT, NumberValue(length));

    // Try to avoid invoking the resolve hook.
    RootedAtom name(cx);
    if (targetObj->is<JSFunction>() && !targetObj->as<JSFunction>().hasResolvedName()) {
        if (!JSFunction::getUnresolvedName(cx, targetObj.as<JSFunction>(), &name))
            return false;
    }

    // 19.2.3.2 Function.prototype.bind, steps 9-11.
    if (!name) {
        // 19.2.3.2 Function.prototype.bind, step 9.
        RootedValue targetName(cx);
        if (!GetProperty(cx, targetObj, targetObj, cx->names().name, &targetName))
            return false;

        // 19.2.3.2 Function.prototype.bind, step 10.
        if (targetName.isString() && !targetName.toString()->empty()) {
            name = AtomizeString(cx, targetName.toString());
            if (!name)
                return false;
        } else {
            name = cx->names().empty;
        }
    }

    MOZ_ASSERT(!bound->hasGuessedAtom());
    bound->setAtom(name);

    return true;
}

/* static */ bool
JSFunction::createScriptForLazilyInterpretedFunction(JSContext* cx, HandleFunction fun)
{
    MOZ_ASSERT(fun->isInterpretedLazy());

    Rooted<LazyScript*> lazy(cx, fun->lazyScriptOrNull());
    if (lazy) {
        RootedScript script(cx, lazy->maybeScript());

        // Only functions without inner functions or direct eval are
        // re-lazified. Functions with either of those are on the static scope
        // chain of their inner functions, or in the case of eval, possibly
        // eval'd inner functions. This prohibits re-lazification as
        // StaticScopeIter queries needsCallObject of those functions, which
        // requires a non-lazy script.  Note that if this ever changes,
        // XDRRelazificationInfo will have to be fixed.
        bool canRelazify = !lazy->numInnerFunctions() && !lazy->hasDirectEval();

        if (script) {
            fun->setUnlazifiedScript(script);
            // Remember the lazy script on the compiled script, so it can be
            // stored on the function again in case of re-lazification.
            if (canRelazify)
                script->setLazyScript(lazy);
            return true;
        }

        if (fun != lazy->functionNonDelazifying()) {
            if (!LazyScript::functionDelazifying(cx, lazy))
                return false;
            script = lazy->functionNonDelazifying()->nonLazyScript();
            if (!script)
                return false;

            fun->setUnlazifiedScript(script);
            return true;
        }

        MOZ_ASSERT(lazy->scriptSource()->hasSourceData());

        // Parse and compile the script from source.
        size_t lazyLength = lazy->end() - lazy->begin();
        UncompressedSourceCache::AutoHoldEntry holder;
        ScriptSource::PinnedChars chars(cx, lazy->scriptSource(), holder,
                                        lazy->begin(), lazyLength);
        if (!chars.get())
            return false;

        if (!frontend::CompileLazyFunction(cx, lazy, chars.get(), lazyLength)) {
            // The frontend may have linked the function and the non-lazy
            // script together during bytecode compilation. Reset it now on
            // error.
            fun->initLazyScript(lazy);
            if (lazy->hasScript())
                lazy->resetScript();
            return false;
        }

        script = fun->nonLazyScript();

        // Remember the compiled script on the lazy script itself, in case
        // there are clones of the function still pointing to the lazy script.
        if (!lazy->maybeScript())
            lazy->initScript(script);

        // Try to insert the newly compiled script into the lazy script cache.
        if (canRelazify) {
            // A script's starting column isn't set by the bytecode emitter, so
            // specify this from the lazy script so that if an identical lazy
            // script is encountered later a match can be determined.
            script->setColumn(lazy->column());

            // Remember the lazy script on the compiled script, so it can be
            // stored on the function again in case of re-lazification.
            // Only functions without inner functions are re-lazified.
            script->setLazyScript(lazy);
        }

        // XDR the newly delazified function.
        if (script->scriptSource()->hasEncoder()) {
            RootedScriptSource sourceObject(cx, lazy->sourceObject());
            if (!script->scriptSource()->xdrEncodeFunction(cx, fun, sourceObject))
                return false;
        }

        return true;
    }

    /* Lazily cloned self-hosted script. */
    MOZ_ASSERT(fun->isSelfHostedBuiltin());
    RootedAtom funAtom(cx, &fun->getExtendedSlot(LAZY_FUNCTION_NAME_SLOT).toString()->asAtom());
    if (!funAtom)
        return false;
    Rooted<PropertyName*> funName(cx, funAtom->asPropertyName());
    return cx->runtime()->cloneSelfHostedFunctionScript(cx, funName, fun);
}

void
JSFunction::maybeRelazify(JSRuntime* rt)
{
    // Try to relazify functions with a non-lazy script. Note: functions can be
    // marked as interpreted despite having no script yet at some points when
    // parsing.
    if (!hasScript() || !u.scripted.s.script_)
        return;

    // Don't relazify functions in compartments that are active.
    JSCompartment* comp = compartment();
    if (comp->hasBeenEntered() && !rt->allowRelazificationForTesting)
        return;

    // The caller should have checked we're not in the self-hosting zone (it's
    // shared with worker runtimes so relazifying functions in it will race).
    MOZ_ASSERT(!comp->isSelfHosting);

    // Don't relazify if the compartment is being debugged.
    if (comp->isDebuggee())
        return;

    // Don't relazify if the compartment and/or runtime is instrumented to
    // collect code coverage for analysis.
    if (comp->collectCoverageForDebug())
        return;

    // Don't relazify functions with JIT code.
    if (!u.scripted.s.script_->isRelazifiable())
        return;

    // To delazify self-hosted builtins we need the name of the function
    // to clone. This name is stored in the first extended slot. Since
    // that slot is sometimes also used for other purposes, make sure it
    // contains a string.
    if (isSelfHostedBuiltin() &&
        (!isExtended() || !getExtendedSlot(LAZY_FUNCTION_NAME_SLOT).isString()))
    {
        return;
    }

    JSScript* script = nonLazyScript();

    flags_ &= ~INTERPRETED;
    flags_ |= INTERPRETED_LAZY;
    LazyScript* lazy = script->maybeLazyScript();
    u.scripted.s.lazy_ = lazy;
    if (lazy) {
        MOZ_ASSERT(!isSelfHostedBuiltin());
    } else {
        MOZ_ASSERT(isSelfHostedBuiltin());
        MOZ_ASSERT(isExtended());
        MOZ_ASSERT(getExtendedSlot(LAZY_FUNCTION_NAME_SLOT).toString()->isAtom());
    }

    comp->scheduleDelazificationForDebugger();
}

const JSFunctionSpec js::function_methods[] = {
    JS_FN(js_toSource_str,   fun_toSource,   0,0),
    JS_FN(js_toString_str,   fun_toString,   0,0),
    JS_FN(js_apply_str,      fun_apply,      2,0),
    JS_FN(js_call_str,       fun_call,       1,0),
    JS_SELF_HOSTED_FN("bind", "FunctionBind", 2, 0),
    JS_SYM_FN(hasInstance, fun_symbolHasInstance, 1, JSPROP_READONLY | JSPROP_PERMANENT),
    JS_FS_END
};

// ES2018 draft rev 2aea8f3e617b49df06414eb062ab44fad87661d3
// 19.2.1.1.1 CreateDynamicFunction( constructor, newTarget, kind, args )
static bool
CreateDynamicFunction(JSContext* cx, const CallArgs& args, GeneratorKind generatorKind,
                      FunctionAsyncKind asyncKind)
{
    // Steps 1-5.
    // Block this call if security callbacks forbid it.
    Rooted<GlobalObject*> global(cx, &args.callee().global());
    if (!GlobalObject::isRuntimeCodeGenEnabled(cx, global)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CSP_BLOCKED_FUNCTION);
        return false;
    }

    bool isGenerator = generatorKind == GeneratorKind::Generator;
    bool isAsync = asyncKind == FunctionAsyncKind::AsyncFunction;

    RootedScript maybeScript(cx);
    const char* filename;
    unsigned lineno;
    bool mutedErrors;
    uint32_t pcOffset;
    DescribeScriptedCallerForCompilation(cx, &maybeScript, &filename, &lineno, &pcOffset,
                                         &mutedErrors);

    const char* introductionType = "Function";
    if (isAsync) {
        if (isGenerator)
            introductionType = "AsyncGenerator";
        else
            introductionType = "AsyncFunction";
    } else if (isGenerator) {
        introductionType = "GeneratorFunction";
    }

    const char* introducerFilename = filename;
    if (maybeScript && maybeScript->scriptSource()->introducerFilename())
        introducerFilename = maybeScript->scriptSource()->introducerFilename();

    CompileOptions options(cx);
    options.setMutedErrors(mutedErrors)
           .setFileAndLine(filename, 1)
           .setNoScriptRval(false)
           .setIntroductionInfo(introducerFilename, introductionType, lineno, maybeScript, pcOffset);

    StringBuffer sb(cx);

    if (isAsync) {
        if (!sb.append("async "))
            return false;
    }
    if (!sb.append("function"))
         return false;
    if (isGenerator) {
        if (!sb.append('*'))
            return false;
    }

    if (!sb.append(" anonymous("))
        return false;

    if (args.length() > 1) {
        RootedString str(cx);

        // Steps 10, 14.d.
        unsigned n = args.length() - 1;

        for (unsigned i = 0; i < n; i++) {
            // Steps 14.a-b, 14.d.i-ii.
            str = ToString<CanGC>(cx, args[i]);
            if (!str)
                return false;

            // Steps 14.b, 14.d.iii.
            if (!sb.append(str))
                 return false;

            if (i < args.length() - 2) {
                // Step 14.d.iii.
                if (!sb.append(','))
                    return false;
            }
        }
    }

    if (!sb.append('\n'))
        return false;

    // Remember the position of ")".
    Maybe<uint32_t> parameterListEnd = Some(uint32_t(sb.length()));
    MOZ_ASSERT(FunctionConstructorMedialSigils[0] == ')');

    if (!sb.append(FunctionConstructorMedialSigils))
        return false;

    if (args.length() > 0) {
        // Steps 13, 14.e, 15.
        RootedString body(cx, ToString<CanGC>(cx, args[args.length() - 1]));
        if (!body || !sb.append(body))
             return false;
     }

    if (!sb.append(FunctionConstructorFinalBrace))
        return false;

    // The parser only accepts two byte strings.
    if (!sb.ensureTwoByteChars())
        return false;

    RootedString functionText(cx, sb.finishString());
    if (!functionText)
        return false;

    /*
     * NB: (new Function) is not lexically closed by its caller, it's just an
     * anonymous function in the top-level scope that its constructor inhabits.
     * Thus 'var x = 42; f = new Function("return x"); print(f())' prints 42,
     * and so would a call to f from another top-level's script or function.
     */
    RootedAtom anonymousAtom(cx, cx->names().anonymous);

    // Initialize the function with the default prototype:
    // Leave as nullptr to get the default from clasp for normal functions.
    // Use %Generator% for generators and the unwrapped function of async
    // functions and async generators.
    RootedObject defaultProto(cx);
    if (isGenerator || isAsync) {
        defaultProto = GlobalObject::getOrCreateGeneratorFunctionPrototype(cx, global);
        if (!defaultProto)
            return false;
    }

    // Step 30-37 (reordered).
    RootedObject globalLexical(cx, &global->lexicalEnvironment());
    JSFunction::Flags flags = (isGenerator || isAsync)
                              ? JSFunction::INTERPRETED_LAMBDA_GENERATOR_OR_ASYNC
                              : JSFunction::INTERPRETED_LAMBDA;
    AllocKind allocKind = isAsync ? AllocKind::FUNCTION_EXTENDED : AllocKind::FUNCTION;
    RootedFunction fun(cx, NewFunctionWithProto(cx, nullptr, 0,
                                                flags, globalLexical,
                                                anonymousAtom, defaultProto,
                                                allocKind, TenuredObject));
    if (!fun)
        return false;

    if (!JSFunction::setTypeForScriptedFunction(cx, fun))
        return false;

    // Steps 7.a-b, 8.a-b, 9.a-b, 16-28.
    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, functionText))
        return false;

    mozilla::Range<const char16_t> chars = stableChars.twoByteRange();
    SourceBufferHolder::Ownership ownership = stableChars.maybeGiveOwnershipToCaller()
                                              ? SourceBufferHolder::GiveOwnership
                                              : SourceBufferHolder::NoOwnership;
    SourceBufferHolder srcBuf(chars.begin().get(), chars.length(), ownership);
    if (isAsync) {
        if (isGenerator) {
            if (!CompileStandaloneAsyncGenerator(cx, &fun, options, srcBuf, parameterListEnd))
                return false;
        } else {
            if (!CompileStandaloneAsyncFunction(cx, &fun, options, srcBuf, parameterListEnd))
                return false;
        }
    } else {
        if (isGenerator) {
            if (!CompileStandaloneGenerator(cx, &fun, options, srcBuf, parameterListEnd))
                return false;
        } else {
            if (!CompileStandaloneFunction(cx, &fun, options, srcBuf, parameterListEnd))
                return false;
        }
    }

    // Steps 6, 29.
    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
        return false;

    if (isAsync) {
        // Create the async function wrapper.
        JSObject* wrapped;
        if (isGenerator) {
            wrapped = proto
                      ? WrapAsyncGeneratorWithProto(cx, fun, proto)
                      : WrapAsyncGenerator(cx, fun);
        } else {
            // Step 9.d, use %AsyncFunctionPrototype% as the fallback prototype.
            wrapped = proto
                      ? WrapAsyncFunctionWithProto(cx, fun, proto)
                      : WrapAsyncFunction(cx, fun);
        }
        if (!wrapped)
            return false;

        fun = &wrapped->as<JSFunction>();
    } else {
        // Steps 7.d, 8.d (implicit).
        // Call SetPrototype if an explicit prototype was given.
        if (proto && !SetPrototype(cx, fun, proto))
            return false;
    }

    // Step 38.
    args.rval().setObject(*fun);
    return true;
}

bool
js::Function(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CreateDynamicFunction(cx, args, GeneratorKind::NotGenerator,
                                 FunctionAsyncKind::SyncFunction);
}

bool
js::Generator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CreateDynamicFunction(cx, args, GeneratorKind::Generator,
                                 FunctionAsyncKind::SyncFunction);
}

bool
js::AsyncFunctionConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CreateDynamicFunction(cx, args, GeneratorKind::NotGenerator,
                                 FunctionAsyncKind::AsyncFunction);
}

bool
js::AsyncGeneratorConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CreateDynamicFunction(cx, args, GeneratorKind::Generator,
                                 FunctionAsyncKind::AsyncFunction);
}

bool
JSFunction::isBuiltinFunctionConstructor()
{
    return maybeNative() == Function || maybeNative() == Generator;
}

bool
JSFunction::needsExtraBodyVarEnvironment() const
{
    MOZ_ASSERT(!isInterpretedLazy());

    if (isNative())
        return false;

    if (!nonLazyScript()->functionHasExtraBodyVarScope())
        return false;

    return nonLazyScript()->functionExtraBodyVarScope()->hasEnvironment();
}

bool
JSFunction::needsNamedLambdaEnvironment() const
{
    MOZ_ASSERT(!isInterpretedLazy());

    if (!isNamedLambda())
        return false;

    LexicalScope* scope = nonLazyScript()->maybeNamedLambdaScope();
    if (!scope)
        return false;

    return scope->hasEnvironment();
}

JSFunction*
js::NewScriptedFunction(JSContext* cx, unsigned nargs,
                        JSFunction::Flags flags, HandleAtom atom,
                        HandleObject proto /* = nullptr */,
                        gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
                        NewObjectKind newKind /* = GenericObject */,
                        HandleObject enclosingEnvArg /* = nullptr */)
{
    RootedObject enclosingEnv(cx, enclosingEnvArg);
    if (!enclosingEnv)
        enclosingEnv = &cx->global()->lexicalEnvironment();
    return NewFunctionWithProto(cx, nullptr, nargs, flags, enclosingEnv,
                                atom, proto, allocKind, newKind);
}

#ifdef DEBUG
static bool
NewFunctionEnvironmentIsWellFormed(JSContext* cx, HandleObject env)
{
    // Assert that the terminating environment is null, global, or a debug
    // scope proxy. All other cases of polluting global scope behavior are
    // handled by EnvironmentObjects (viz. non-syntactic DynamicWithObject and
    // NonSyntacticVariablesObject).
    RootedObject terminatingEnv(cx, SkipEnvironmentObjects(env));
    return !terminatingEnv || terminatingEnv == cx->global() ||
           terminatingEnv->is<DebugEnvironmentProxy>();
}
#endif

JSFunction*
js::NewFunctionWithProto(JSContext* cx, Native native,
                         unsigned nargs, JSFunction::Flags flags, HandleObject enclosingEnv,
                         HandleAtom atom, HandleObject proto,
                         gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
                         NewObjectKind newKind /* = GenericObject */)
{
    MOZ_ASSERT(allocKind == AllocKind::FUNCTION || allocKind == AllocKind::FUNCTION_EXTENDED);
    MOZ_ASSERT_IF(native, !enclosingEnv);
    MOZ_ASSERT(NewFunctionEnvironmentIsWellFormed(cx, enclosingEnv));

    JSFunction* fun = NewObjectWithClassProto<JSFunction>(cx, proto, allocKind, newKind);
    if (!fun)
        return nullptr;

    if (allocKind == AllocKind::FUNCTION_EXTENDED)
        flags = JSFunction::Flags(flags | JSFunction::EXTENDED);

    /* Initialize all function members. */
    fun->setArgCount(uint16_t(nargs));
    fun->setFlags(flags);
    if (fun->isInterpreted()) {
        MOZ_ASSERT(!native);
        if (fun->isInterpretedLazy())
            fun->initLazyScript(nullptr);
        else
            fun->initScript(nullptr);
        fun->initEnvironment(enclosingEnv);
    } else {
        MOZ_ASSERT(fun->isNative());
        MOZ_ASSERT(native);
        if (fun->isWasmOptimized())
            fun->initWasmNative(native);
        else
            fun->initNative(native, nullptr);
    }
    if (allocKind == AllocKind::FUNCTION_EXTENDED)
        fun->initializeExtended();
    fun->initAtom(atom);

    return fun;
}

bool
js::CanReuseScriptForClone(JSCompartment* compartment, HandleFunction fun,
                           HandleObject newParent)
{
    MOZ_ASSERT(fun->isInterpreted());

    if (compartment != fun->compartment() ||
        fun->isSingleton() ||
        ObjectGroup::useSingletonForClone(fun))
    {
        return false;
    }

    if (newParent->is<GlobalObject>())
        return true;

    // Don't need to clone the script if newParent is a syntactic scope, since
    // in that case we have some actual scope objects on our scope chain and
    // whatnot; whoever put them there should be responsible for setting our
    // script's flags appropriately.  We hit this case for JSOP_LAMBDA, for
    // example.
    if (IsSyntacticEnvironment(newParent))
        return true;

    // We need to clone the script if we're not already marked as having a
    // non-syntactic scope.
    return fun->hasScript()
        ? fun->nonLazyScript()->hasNonSyntacticScope()
        : fun->lazyScript()->enclosingScope()->hasOnChain(ScopeKind::NonSyntactic);
}

static inline JSFunction*
NewFunctionClone(JSContext* cx, HandleFunction fun, NewObjectKind newKind,
                 gc::AllocKind allocKind, HandleObject proto)
{
    RootedObject cloneProto(cx, proto);
    if (!proto && (fun->isGenerator() || fun->isAsync())) {
        cloneProto = GlobalObject::getOrCreateGeneratorFunctionPrototype(cx, cx->global());
        if (!cloneProto)
            return nullptr;
    }

    JSObject* cloneobj = NewObjectWithClassProto(cx, &JSFunction::class_, cloneProto,
                                                 allocKind, newKind);
    if (!cloneobj)
        return nullptr;
    RootedFunction clone(cx, &cloneobj->as<JSFunction>());

    uint16_t flags = fun->flags() & ~JSFunction::EXTENDED;
    if (allocKind == AllocKind::FUNCTION_EXTENDED)
        flags |= JSFunction::EXTENDED;

    clone->setArgCount(fun->nargs());
    clone->setFlags(flags);

    JSAtom* atom = fun->displayAtom();
    if (atom)
        cx->markAtom(atom);
    clone->initAtom(atom);

    if (allocKind == AllocKind::FUNCTION_EXTENDED) {
        if (fun->isExtended() && fun->compartment() == cx->compartment()) {
            for (unsigned i = 0; i < FunctionExtended::NUM_EXTENDED_SLOTS; i++)
                clone->initExtendedSlot(i, fun->getExtendedSlot(i));
        } else {
            clone->initializeExtended();
        }
    }

    return clone;
}

JSFunction*
js::CloneFunctionReuseScript(JSContext* cx, HandleFunction fun, HandleObject enclosingEnv,
                             gc::AllocKind allocKind /* = FUNCTION */ ,
                             NewObjectKind newKind /* = GenericObject */,
                             HandleObject proto /* = nullptr */)
{
    MOZ_ASSERT(NewFunctionEnvironmentIsWellFormed(cx, enclosingEnv));
    MOZ_ASSERT(fun->isInterpreted());
    MOZ_ASSERT(!fun->isBoundFunction());
    MOZ_ASSERT(CanReuseScriptForClone(cx->compartment(), fun, enclosingEnv));

    RootedFunction clone(cx, NewFunctionClone(cx, fun, newKind, allocKind, proto));
    if (!clone)
        return nullptr;

    if (fun->hasScript()) {
        clone->initScript(fun->nonLazyScript());
        clone->initEnvironment(enclosingEnv);
    } else {
        MOZ_ASSERT(fun->isInterpretedLazy());
        MOZ_ASSERT(fun->compartment() == clone->compartment());
        LazyScript* lazy = fun->lazyScriptOrNull();
        clone->initLazyScript(lazy);
        clone->initEnvironment(enclosingEnv);
    }

    /*
     * Clone the function, reusing its script. We can use the same group as
     * the original function provided that its prototype is correct.
     */
    if (fun->staticPrototype() == clone->staticPrototype())
        clone->setGroup(fun->group());
    return clone;
}

JSFunction*
js::CloneFunctionAndScript(JSContext* cx, HandleFunction fun, HandleObject enclosingEnv,
                           HandleScope newScope, gc::AllocKind allocKind /* = FUNCTION */,
                           HandleObject proto /* = nullptr */)
{
    MOZ_ASSERT(NewFunctionEnvironmentIsWellFormed(cx, enclosingEnv));
    MOZ_ASSERT(fun->isInterpreted());
    MOZ_ASSERT(!fun->isBoundFunction());

    JSScript::AutoDelazify funScript(cx, fun);
    if (!funScript)
        return nullptr;

    RootedFunction clone(cx, NewFunctionClone(cx, fun, SingletonObject, allocKind, proto));
    if (!clone)
        return nullptr;

    clone->initScript(nullptr);
    clone->initEnvironment(enclosingEnv);

    /*
     * Across compartments or if we have to introduce a non-syntactic scope we
     * have to clone the script for interpreted functions. Cross-compartment
     * cloning only happens via JSAPI (JS::CloneFunctionObject) which
     * dynamically ensures that 'script' has no enclosing lexical scope (only
     * the global scope or other non-lexical scope).
     */
#ifdef DEBUG
    RootedObject terminatingEnv(cx, enclosingEnv);
    while (IsSyntacticEnvironment(terminatingEnv))
        terminatingEnv = terminatingEnv->enclosingEnvironment();
    MOZ_ASSERT_IF(!terminatingEnv->is<GlobalObject>(),
                  newScope->hasOnChain(ScopeKind::NonSyntactic));
#endif

    RootedScript script(cx, fun->nonLazyScript());
    MOZ_ASSERT(script->compartment() == fun->compartment());
    MOZ_ASSERT(cx->compartment() == clone->compartment(),
               "Otherwise we could relazify clone below!");

    RootedScript clonedScript(cx, CloneScriptIntoFunction(cx, newScope, clone, script));
    if (!clonedScript)
        return nullptr;
    Debugger::onNewScript(cx, clonedScript);

    return clone;
}

JSFunction*
js::CloneAsmJSModuleFunction(JSContext* cx, HandleFunction fun)
{
    MOZ_ASSERT(fun->isNative());
    MOZ_ASSERT(IsAsmJSModule(fun));
    MOZ_ASSERT(fun->isExtended());
    MOZ_ASSERT(cx->compartment() == fun->compartment());

    JSFunction* clone = NewFunctionClone(cx, fun, GenericObject, AllocKind::FUNCTION_EXTENDED,
                                         /* proto = */ nullptr);
    if (!clone)
        return nullptr;

    MOZ_ASSERT(fun->native() == InstantiateAsmJS);
    MOZ_ASSERT(!fun->hasJitInfo());
    clone->initNative(InstantiateAsmJS, nullptr);

    clone->setGroup(fun->group());
    return clone;
}

JSFunction*
js::CloneSelfHostingIntrinsic(JSContext* cx, HandleFunction fun)
{
    MOZ_ASSERT(fun->isNative());
    MOZ_ASSERT(fun->compartment()->isSelfHosting);
    MOZ_ASSERT(!fun->isExtended());
    MOZ_ASSERT(cx->compartment() != fun->compartment());

    JSFunction* clone = NewFunctionClone(cx, fun, SingletonObject, AllocKind::FUNCTION,
                                         /* proto = */ nullptr);
    if (!clone)
        return nullptr;

    clone->initNative(fun->native(), fun->hasJitInfo() ? fun->jitInfo() : nullptr);
    return clone;
}

/*
 * Return an atom for use as the name of a builtin method with the given
 * property id.
 *
 * Function names are always strings. If id is the well-known @@iterator
 * symbol, this returns "[Symbol.iterator]".  If a prefix is supplied the final
 * name is |prefix + " " + name|. A prefix cannot be supplied if id is a
 * symbol value.
 *
 * Implements steps 3-5 of 9.2.11 SetFunctionName in ES2016.
 */
JSAtom*
js::IdToFunctionName(JSContext* cx, HandleId id,
                     FunctionPrefixKind prefixKind /* = FunctionPrefixKind::None */)
{
    // No prefix fastpath.
    if (JSID_IS_ATOM(id) && prefixKind == FunctionPrefixKind::None)
        return JSID_TO_ATOM(id);

    // Step 3 (implicit).

    // Step 4.
    if (JSID_IS_SYMBOL(id)) {
        // Step 4.a.
        RootedAtom desc(cx, JSID_TO_SYMBOL(id)->description());

        // Step 4.b, no prefix fastpath.
        if (!desc && prefixKind == FunctionPrefixKind::None)
            return cx->names().empty;

        // Step 5 (reordered).
        StringBuffer sb(cx);
        if (prefixKind == FunctionPrefixKind::Get) {
            if (!sb.append("get "))
                return nullptr;
        } else if (prefixKind == FunctionPrefixKind::Set) {
            if (!sb.append("set "))
                return nullptr;
        }

        // Step 4.b.
        if (desc) {
            // Step 4.c.
            if (!sb.append('[') || !sb.append(desc) || !sb.append(']'))
                return nullptr;
        }
        return sb.finishAtom();
    }

    RootedValue idv(cx, IdToValue(id));
    RootedAtom name(cx, ToAtom<CanGC>(cx, idv));
    if (!name)
        return nullptr;

    // Step 5.
    return NameToFunctionName(cx, name, prefixKind);
}

JSAtom*
js::NameToFunctionName(JSContext* cx, HandleAtom name,
                       FunctionPrefixKind prefixKind /* = FunctionPrefixKind::None */)
{
    if (prefixKind == FunctionPrefixKind::None)
        return name;

    StringBuffer sb(cx);
    if (prefixKind == FunctionPrefixKind::Get) {
        if (!sb.append("get "))
            return nullptr;
    } else {
        if (!sb.append("set "))
            return nullptr;
    }
    if (!sb.append(name))
        return nullptr;
    return sb.finishAtom();
}

bool
js::SetFunctionNameIfNoOwnName(JSContext* cx, HandleFunction fun, HandleValue name,
                               FunctionPrefixKind prefixKind)
{
    MOZ_ASSERT(name.isString() || name.isSymbol() || name.isNumber());

    if (fun->isClassConstructor()) {
        // A class may have static 'name' method or accessor.
        RootedId nameId(cx, NameToId(cx->names().name));
        bool result;
        if (!HasOwnProperty(cx, fun, nameId, &result))
            return false;

        if (result)
            return true;
    } else {
        // Anonymous function shouldn't have own 'name' property at this point.
        MOZ_ASSERT(!fun->containsPure(cx->names().name));
    }

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, name, &id))
        return false;

    RootedAtom funNameAtom(cx, IdToFunctionName(cx, id, prefixKind));
    if (!funNameAtom)
        return false;

    RootedValue funNameVal(cx, StringValue(funNameAtom));
    if (!NativeDefineDataProperty(cx, fun, cx->names().name, funNameVal, JSPROP_READONLY))
        return false;

    return true;
}

JSFunction*
js::DefineFunction(JSContext* cx, HandleObject obj, HandleId id, Native native,
                   unsigned nargs, unsigned flags, AllocKind allocKind /* = AllocKind::FUNCTION */)
{
    RootedAtom atom(cx, IdToFunctionName(cx, id));
    if (!atom)
        return nullptr;

    RootedFunction fun(cx);
    if (!native)
        fun = NewScriptedFunction(cx, nargs,
                                  JSFunction::INTERPRETED_LAZY, atom,
                                  /* proto = */ nullptr,
                                  allocKind, GenericObject, obj);
    else if (flags & JSFUN_CONSTRUCTOR)
        fun = NewNativeConstructor(cx, native, nargs, atom, allocKind);
    else
        fun = NewNativeFunction(cx, native, nargs, atom, allocKind);

    if (!fun)
        return nullptr;

    RootedValue funVal(cx, ObjectValue(*fun));
    if (!DefineDataProperty(cx, obj, id, funVal, flags & ~JSFUN_FLAGS_MASK))
        return nullptr;

    return fun;
}

void
js::ReportIncompatibleMethod(JSContext* cx, const CallArgs& args, const Class* clasp)
{
    RootedValue thisv(cx, args.thisv());

#ifdef DEBUG
    if (thisv.isObject()) {
        MOZ_ASSERT(thisv.toObject().getClass() != clasp ||
                   !thisv.toObject().isNative() ||
                   !thisv.toObject().staticPrototype() ||
                   thisv.toObject().staticPrototype()->getClass() != clasp);
    } else if (thisv.isString()) {
        MOZ_ASSERT(clasp != &StringObject::class_);
    } else if (thisv.isNumber()) {
        MOZ_ASSERT(clasp != &NumberObject::class_);
    } else if (thisv.isBoolean()) {
        MOZ_ASSERT(clasp != &BooleanObject::class_);
    } else if (thisv.isSymbol()) {
        MOZ_ASSERT(clasp != &SymbolObject::class_);
    } else {
        MOZ_ASSERT(thisv.isUndefined() || thisv.isNull());
    }
#endif

    if (JSFunction* fun = ReportIfNotFunction(cx, args.calleev())) {
        JSAutoByteString funNameBytes;
        if (const char* funName = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
            JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                       clasp->name, funName, InformalValueTypeName(thisv));
        }
    }
}

void
js::ReportIncompatible(JSContext* cx, const CallArgs& args)
{
    if (JSFunction* fun = ReportIfNotFunction(cx, args.calleev())) {
        JSAutoByteString funNameBytes;
        if (const char* funName = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
            JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_METHOD,
                                       funName, "method", InformalValueTypeName(args.thisv()));
        }
    }
}

namespace JS {
namespace detail {

JS_PUBLIC_API(void)
CheckIsValidConstructible(const Value& calleev)
{
    JSObject* callee = &calleev.toObject();
    if (callee->is<JSFunction>())
        MOZ_ASSERT(callee->as<JSFunction>().isConstructor());
    else
        MOZ_ASSERT(callee->constructHook() != nullptr);
}

} // namespace detail
} // namespace JS
