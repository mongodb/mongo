/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS function support.
 */

#include "jsfuninlines.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Range.h"

#include <string.h>

#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsobj.h"
#include "jsscript.h"
#include "jsstr.h"
#include "jstypes.h"
#include "jswrapper.h"

#include "builtin/Eval.h"
#include "builtin/Object.h"
#include "frontend/BytecodeCompiler.h"
#include "frontend/TokenStream.h"
#include "gc/Marking.h"
#include "jit/Ion.h"
#include "jit/JitFrameIterator.h"
#include "js/CallNonGenericMethod.h"
#include "js/Proxy.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/Shape.h"
#include "vm/StringBuffer.h"
#include "vm/WrapperObject.h"
#include "vm/Xdr.h"

#include "jsscriptinlines.h"

#include "vm/Interpreter-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::gc;
using namespace js::frontend;

using mozilla::ArrayLength;
using mozilla::PodCopy;
using mozilla::RangedPtr;

static bool
fun_enumerate(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->is<JSFunction>());

    RootedId id(cx);
    bool found;

    if (!obj->isBoundFunction() && !obj->as<JSFunction>().isArrow()) {
        id = NameToId(cx->names().prototype);
        if (!HasProperty(cx, obj, id, &found))
            return false;
    }

    id = NameToId(cx->names().length);
    if (!HasProperty(cx, obj, id, &found))
        return false;

    id = NameToId(cx->names().name);
    if (!HasProperty(cx, obj, id, &found))
        return false;

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
    MOZ_ASSERT(!fun->isBoundFunction(), "all bound functions are currently native (ergo builtin)");

    for (; !iter.done(); ++iter) {
        if (!iter.isFunctionFrame() || iter.isEvalFrame())
            continue;
        if (iter.matchCallee(cx, fun))
            return true;
    }
    return false;
}

static void
ThrowTypeErrorBehavior(JSContext* cx)
{
    JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, js_GetErrorMessage, nullptr,
                                 JSMSG_THROW_TYPE_ERROR);
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
    // Throw if the function is a builtin (note: this doesn't include asm.js),
    // a strict mode function (FIXME: needs work handle strict asm.js functions
    // correctly, should fall out of bug 1057208), or a bound function.
    if (fun->isBuiltin() ||
        (fun->isInterpreted() && fun->strict()) ||
        fun->isBoundFunction())
    {
        ThrowTypeErrorBehavior(cx);
        return false;
    }

    // Functions with rest arguments don't include a local |arguments| binding.
    // Similarly, "arguments" shouldn't work on them.
    if (fun->hasRest()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                             JSMSG_FUNCTION_ARGUMENTS_AND_REST);
        return false;
    }

    // Otherwise emit a strict warning about |f.arguments| to discourage use of
    // this non-standard, performance-harmful feature.
    if (!JS_ReportErrorFlagsAndNumber(cx, JSREPORT_WARNING | JSREPORT_STRICT, js_GetErrorMessage,
                                      nullptr, JSMSG_DEPRECATED_USAGE, js_arguments_str))
    {
        return false;
    }

    return true;
}

bool
ArgumentsGetterImpl(JSContext* cx, CallArgs args)
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

    // Disabling compiling of this script in IonMonkey.  IonMonkey doesn't
    // guarantee |f.arguments| can be fully recovered, so we try to mitigate
    // observing this behavior by detecting its use early.
    JSScript* script = iter.script();
    jit::ForbidCompilation(cx, script);

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
ArgumentsSetterImpl(JSContext* cx, CallArgs args)
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
    // Throw if the function is a builtin (note: this doesn't include asm.js),
    // a strict mode function (FIXME: needs work handle strict asm.js functions
    // correctly, should fall out of bug 1057208), or a bound function.
    if (fun->isBuiltin() ||
        (fun->isInterpreted() && fun->strict()) ||
        fun->isBoundFunction())
    {
        ThrowTypeErrorBehavior(cx);
        return false;
    }

    // Otherwise emit a strict warning about |f.caller| to discourage use of
    // this non-standard, performance-harmful feature.
    if (!JS_ReportErrorFlagsAndNumber(cx, JSREPORT_WARNING | JSREPORT_STRICT, js_GetErrorMessage,
                                      nullptr, JSMSG_DEPRECATED_USAGE, js_caller_str))
    {
        return false;
    }

    return true;
}

bool
CallerGetterImpl(JSContext* cx, CallArgs args)
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
    if (iter.done() || !iter.isFunctionFrame()) {
        args.rval().setNull();
        return true;
    }

    RootedObject caller(cx, iter.callee(cx));
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
        MOZ_ASSERT(!callerFun->isBuiltin(), "non-builtin iterator returned a builtin?");

        if (callerFun->strict()) {
            JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, js_GetErrorMessage, nullptr,
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
CallerSetterImpl(JSContext* cx, CallArgs args)
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
    if (iter.done() || !iter.isFunctionFrame())
        return true;

    RootedObject caller(cx, iter.callee(cx));
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
        JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, js_GetErrorMessage, nullptr,
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

static JSObject*
ResolveInterpretedFunctionPrototype(JSContext* cx, HandleObject obj)
{
#ifdef DEBUG
    JSFunction* fun = &obj->as<JSFunction>();
    MOZ_ASSERT(fun->isInterpreted() || fun->isAsmJSNative());
    MOZ_ASSERT(!fun->isFunctionPrototype());
#endif

    // Assert that fun is not a compiler-created function object, which
    // must never leak to script or embedding code and then be mutated.
    // Also assert that obj is not bound, per the ES5 15.3.4.5 ref above.
    MOZ_ASSERT(!IsInternalFunctionObject(obj));
    MOZ_ASSERT(!obj->isBoundFunction());

    // Make the prototype object an instance of Object with the same parent as
    // the function object itself, unless the function is an ES6 generator.  In
    // that case, per the 15 July 2013 ES6 draft, section 15.19.3, its parent is
    // the GeneratorObjectPrototype singleton.
    bool isStarGenerator = obj->as<JSFunction>().isStarGenerator();
    Rooted<GlobalObject*> global(cx, &obj->global());
    RootedObject objProto(cx);
    if (isStarGenerator)
        objProto = GlobalObject::getOrCreateStarGeneratorObjectPrototype(cx, global);
    else
        objProto = obj->global().getOrCreateObjectPrototype(cx);
    if (!objProto)
        return nullptr;

    RootedPlainObject proto(cx, NewObjectWithGivenProto<PlainObject>(cx, objProto,
                                                                     NullPtr(), SingletonObject));
    if (!proto)
        return nullptr;

    // Per ES5 15.3.5.2 a user-defined function's .prototype property is
    // initially non-configurable, non-enumerable, and writable.
    RootedValue protoVal(cx, ObjectValue(*proto));
    if (!DefineProperty(cx, obj, cx->names().prototype, protoVal, nullptr, nullptr,
                        JSPROP_PERMANENT))
    {
        return nullptr;
    }

    // Per ES5 13.2 the prototype's .constructor property is configurable,
    // non-enumerable, and writable.  However, per the 15 July 2013 ES6 draft,
    // section 15.19.3, the .prototype of a generator function does not link
    // back with a .constructor.
    if (!isStarGenerator) {
        RootedValue objVal(cx, ObjectValue(*obj));
        if (!DefineProperty(cx, proto, cx->names().constructor, objVal, nullptr, nullptr, 0))
            return nullptr;
    }

    return proto;
}

bool
js::FunctionHasResolveHook(const JSAtomState& atomState, jsid id)
{
    if (!JSID_IS_ATOM(id))
        return false;

    JSAtom* atom = JSID_TO_ATOM(id);
    return atom == atomState.prototype || atom == atomState.length || atom == atomState.name;
}

bool
js::fun_resolve(JSContext* cx, HandleObject obj, HandleId id, bool* resolvedp)
{
    if (!JSID_IS_ATOM(id))
        return true;

    RootedFunction fun(cx, &obj->as<JSFunction>());

    if (JSID_IS_ATOM(id, cx->names().prototype)) {
        /*
         * Built-in functions do not have a .prototype property per ECMA-262,
         * or (Object.prototype, Function.prototype, etc.) have that property
         * created eagerly.
         *
         * ES5 15.3.4: the non-native function object named Function.prototype
         * does not have a .prototype property.
         *
         * ES5 15.3.4.5: bound functions don't have a prototype property. The
         * isBuiltin() test covers this case because bound functions are native
         * (and thus built-in) functions by definition/construction.
         *
         * ES6 19.2.4.3: arrow functions also don't have a prototype property.
         */
        if (fun->isBuiltin() || fun->isArrow() || fun->isFunctionPrototype())
            return true;

        if (!ResolveInterpretedFunctionPrototype(cx, fun))
            return false;

        *resolvedp = true;
        return true;
    }

    bool isLength = JSID_IS_ATOM(id, cx->names().length);
    if (isLength || JSID_IS_ATOM(id, cx->names().name)) {
        MOZ_ASSERT(!IsInternalFunctionObject(obj));

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

            uint16_t length;
            if (!fun->getLength(cx, &length))
                return false;

            v.setInt32(length);
        } else {
            if (fun->hasResolvedName())
                return true;

            v.setString(fun->atom() == nullptr ? cx->runtime()->emptyString : fun->atom());
        }

        if (!NativeDefineProperty(cx, fun, id, v, nullptr, nullptr, JSPROP_READONLY))
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
js::XDRInterpretedFunction(XDRState<mode>* xdr, HandleObject enclosingScope, HandleScript enclosingScript,
                           MutableHandleFunction objp)
{
    enum FirstWordFlag {
        HasAtom             = 0x1,
        IsStarGenerator     = 0x2,
        IsLazy              = 0x4,
        HasSingletonType    = 0x8
    };

    /* NB: Keep this in sync with CloneFunctionAndScript. */
    RootedAtom atom(xdr->cx());
    uint32_t firstword = 0;        /* bitmask of FirstWordFlag */
    uint32_t flagsword = 0;        /* word for argument count and fun->flags */

    JSContext* cx = xdr->cx();
    RootedFunction fun(cx);
    RootedScript script(cx);
    Rooted<LazyScript*> lazy(cx);

    if (mode == XDR_ENCODE) {
        fun = objp;
        if (!fun->isInterpreted()) {
            JSAutoByteString funNameBytes;
            if (const char* name = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                                     JSMSG_NOT_SCRIPTED_FUNCTION, name);
            }
            return false;
        }

        if (fun->atom() || fun->hasGuessedAtom())
            firstword |= HasAtom;

        if (fun->isStarGenerator())
            firstword |= IsStarGenerator;

        if (fun->isInterpretedLazy()) {
            // This can only happen for re-lazified cloned functions, so this
            // does not apply to any JSFunction produced by the parser, only to
            // JSFunction created by the runtime.
            MOZ_ASSERT(!fun->lazyScript()->maybeScript());

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
        flagsword = (fun->nargs() << 16) | fun->flags();

        // The environment of any function which is not reused will always be
        // null, it is later defined when a function is cloned or reused to
        // mirror the scope chain.
        MOZ_ASSERT_IF(fun->isSingleton() &&
                      !((lazy && lazy->hasBeenCloned()) || (script && script->hasBeenCloned())),
                      fun->environment() == nullptr);
    }

    if (!xdr->codeUint32(&firstword))
        return false;

    if ((firstword & HasAtom) && !XDRAtom(xdr, &atom))
        return false;
    if (!xdr->codeUint32(&flagsword))
        return false;

    if (mode == XDR_DECODE) {
        RootedObject proto(cx);
        if (firstword & IsStarGenerator) {
            proto = GlobalObject::getOrCreateStarGeneratorFunctionPrototype(cx, cx->global());
            if (!proto)
                return false;
        }

        gc::AllocKind allocKind = JSFunction::FinalizeKind;
        if (uint16_t(flagsword) & JSFunction::EXTENDED)
            allocKind = JSFunction::ExtendedFinalizeKind;
        fun = NewFunctionWithProto(cx, NullPtr(), nullptr, 0, JSFunction::INTERPRETED,
                                   /* parent = */ NullPtr(), NullPtr(), proto,
                                   allocKind, TenuredObject);
        if (!fun)
            return false;
        script = nullptr;
    }

    if (firstword & IsLazy) {
        if (!XDRLazyScript(xdr, enclosingScope, enclosingScript, fun, &lazy))
            return false;
    } else {
        if (!XDRScript(xdr, enclosingScope, enclosingScript, fun, &script))
            return false;
    }

    if (mode == XDR_DECODE) {
        fun->setArgCount(flagsword >> 16);
        fun->setFlags(uint16_t(flagsword));
        fun->initAtom(atom);
        if (firstword & IsLazy) {
            fun->initLazyScript(lazy);
        } else {
            fun->initScript(script);
            script->setFunction(fun);
            MOZ_ASSERT(fun->nargs() == script->bindings.numArgs());
        }

        bool singleton = firstword & HasSingletonType;
        if (!JSFunction::setTypeForScriptedFunction(cx, fun, singleton))
            return false;
        objp.set(fun);
    }

    return true;
}

template bool
js::XDRInterpretedFunction(XDRState<XDR_ENCODE>*, HandleObject, HandleScript, MutableHandleFunction);

template bool
js::XDRInterpretedFunction(XDRState<XDR_DECODE>*, HandleObject, HandleScript, MutableHandleFunction);

JSObject*
js::CloneFunctionAndScript(JSContext* cx, HandleObject enclosingScope, HandleFunction srcFun)
{
    /* NB: Keep this in sync with XDRInterpretedFunction. */
    RootedObject cloneProto(cx);
    if (srcFun->isStarGenerator()) {
        cloneProto = GlobalObject::getOrCreateStarGeneratorFunctionPrototype(cx, cx->global());
        if (!cloneProto)
            return nullptr;
    }

    gc::AllocKind allocKind = JSFunction::FinalizeKind;
    if (srcFun->isExtended())
        allocKind = JSFunction::ExtendedFinalizeKind;
    RootedFunction clone(cx, NewFunctionWithProto(cx, NullPtr(), nullptr, 0,
                                                  JSFunction::INTERPRETED, NullPtr(), NullPtr(),
                                                  cloneProto, allocKind, TenuredObject));
    if (!clone)
        return nullptr;

    RootedScript srcScript(cx, srcFun->getOrCreateScript(cx));
    if (!srcScript)
        return nullptr;
    RootedScript clonedScript(cx, CloneScript(cx, enclosingScope, clone, srcScript));
    if (!clonedScript)
        return nullptr;

    clone->setArgCount(srcFun->nargs());
    clone->setFlags(srcFun->flags());
    clone->initAtom(srcFun->displayAtom());
    clone->initScript(clonedScript);
    clonedScript->setFunction(clone);
    if (!JSFunction::setTypeForScriptedFunction(cx, clone))
        return nullptr;

    RootedScript cloneScript(cx, clone->nonLazyScript());
    return clone;
}

/*
 * [[HasInstance]] internal method for Function objects: fetch the .prototype
 * property of its 'this' parameter, and walks the prototype chain of v (only
 * if v is an object) returning true if .prototype is found.
 */
static bool
fun_hasInstance(JSContext* cx, HandleObject objArg, MutableHandleValue v, bool* bp)
{
    RootedObject obj(cx, objArg);

    while (obj->is<JSFunction>() && obj->isBoundFunction())
        obj = obj->as<JSFunction>().getBoundFunctionTarget();

    RootedValue pval(cx);
    if (!GetProperty(cx, obj, obj, cx->names().prototype, &pval))
        return false;

    if (pval.isPrimitive()) {
        /*
         * Throw a runtime error if instanceof is called on a function that
         * has a non-object as its .prototype value.
         */
        RootedValue val(cx, ObjectValue(*obj));
        js_ReportValueError(cx, JSMSG_BAD_PROTOTYPE, -1, val, js::NullPtr());
        return false;
    }

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
        MarkValueRange(trc, ArrayLength(toExtended()->extendedSlots),
                       toExtended()->extendedSlots, "nativeReserved");
    }

    if (atom_)
        MarkString(trc, &atom_, "atom");

    if (isInterpreted()) {
        // Functions can be be marked as interpreted despite having no script
        // yet at some points when parsing, and can be lazy with no lazy script
        // for self-hosted code.
        if (hasScript() && u.i.s.script_) {
            // Functions can be relazified under the following conditions:
            // - their compartment isn't currently executing scripts or being
            //   debugged
            // - they are not in the self-hosting compartment
            // - they aren't generators
            // - they don't have JIT code attached
            // - they don't have child functions
            // - they have information for un-lazifying them again later
            // This information can either be a LazyScript, or the name of a
            // self-hosted function which can be cloned over again. The latter
            // is stored in the first extended slot.
            JSRuntime* rt = trc->runtime();
            if (IS_GC_MARKING_TRACER(trc) &&
                (rt->allowRelazificationForTesting || !compartment()->hasBeenEntered()) &&
                !compartment()->isDebuggee() && !compartment()->isSelfHosting &&
                u.i.s.script_->isRelazifiable() && (!isSelfHostedBuiltin() || isExtended()))
            {
                relazify(trc);
            } else {
                MarkScriptUnbarriered(trc, &u.i.s.script_, "script");
            }
        } else if (isInterpretedLazy() && u.i.s.lazy_) {
            MarkLazyScriptUnbarriered(trc, &u.i.s.lazy_, "lazyScript");
        }
        if (u.i.env_)
            MarkObjectUnbarriered(trc, &u.i.env_, "fun_environment");
    }
}

static void
fun_trace(JSTracer* trc, JSObject* obj)
{
    obj->as<JSFunction>().trace(trc);
}

static bool
ThrowTypeError(JSContext* cx, unsigned argc, Value* vp)
{
    ThrowTypeErrorBehavior(cx);
    return false;
}

static JSObject*
CreateFunctionConstructor(JSContext* cx, JSProtoKey key)
{
    Rooted<GlobalObject*> self(cx, cx->global());
    RootedObject functionProto(cx, &self->getPrototype(JSProto_Function).toObject());

    RootedObject ctor(cx, NewObjectWithGivenProto(cx, &JSFunction::class_, functionProto,
                                                  self, SingletonObject));
    if (!ctor)
        return nullptr;
    RootedObject functionCtor(cx, NewFunction(cx, ctor, Function, 1, JSFunction::NATIVE_CTOR, self,
                                              HandlePropertyName(cx->names().Function)));
    if (!functionCtor)
        return nullptr;

    MOZ_ASSERT(ctor == functionCtor);
    return functionCtor;

}

static JSObject*
CreateFunctionPrototype(JSContext* cx, JSProtoKey key)
{
    Rooted<GlobalObject*> self(cx, cx->global());

    RootedObject objectProto(cx, &self->getPrototype(JSProto_Object).toObject());
    JSObject* functionProto_ = NewObjectWithGivenProto(cx, &JSFunction::class_,
                                                       objectProto, self, SingletonObject);
    if (!functionProto_)
        return nullptr;

    RootedFunction functionProto(cx, &functionProto_->as<JSFunction>());

    /*
     * Bizarrely, |Function.prototype| must be an interpreted function, so
     * give it the guts to be one.
     */
    {
        JSObject* proto = NewFunction(cx, functionProto, nullptr, 0, JSFunction::INTERPRETED,
                                      self, js::NullPtr());
        if (!proto)
            return nullptr;

        MOZ_ASSERT(proto == functionProto);
        functionProto->setIsFunctionPrototype();
    }

    const char* rawSource = "() {\n}";
    size_t sourceLen = strlen(rawSource);
    char16_t* source = InflateString(cx, rawSource, &sourceLen);
    if (!source)
        return nullptr;

    ScriptSource* ss =
        cx->new_<ScriptSource>();
    if (!ss) {
        js_free(source);
        return nullptr;
    }
    ScriptSourceHolder ssHolder(ss);
    ss->setSource(source, sourceLen);
    CompileOptions options(cx);
    options.setNoScriptRval(true)
           .setVersion(JSVERSION_DEFAULT);
    RootedScriptSource sourceObject(cx, ScriptSourceObject::create(cx, ss));
    if (!sourceObject || !ScriptSourceObject::initFromOptions(cx, sourceObject, options))
        return nullptr;

    RootedScript script(cx, JSScript::Create(cx,
                                             /* enclosingScope = */ js::NullPtr(),
                                             /* savedCallerFun = */ false,
                                             options,
                                             /* staticLevel = */ 0,
                                             sourceObject,
                                             0,
                                             ss->length()));
    if (!script || !JSScript::fullyInitTrivial(cx, script))
        return nullptr;

    functionProto->initScript(script);
    ObjectGroup* protoGroup = functionProto->getGroup(cx);
    if (!protoGroup)
        return nullptr;

    protoGroup->setInterpretedFunction(functionProto);
    script->setFunction(functionProto);

    /*
     * The default 'new' group of Function.prototype is required by type
     * inference to have unknown properties, to simplify handling of e.g.
     * CloneFunctionObject.
     */
    if (!JSObject::setNewGroupUnknown(cx, &JSFunction::class_, functionProto))
        return nullptr;

    // Construct the unique [[%ThrowTypeError%]] function object, used only for
    // "callee" and "caller" accessors on strict mode arguments objects.  (The
    // spec also uses this for "arguments" and "caller" on various functions,
    // but we're experimenting with implementing them using accessors on
    // |Function.prototype| right now.)
    RootedObject tte(cx, NewObjectWithGivenProto(cx, &JSFunction::class_, functionProto, self,
                                                 SingletonObject));
    if (!tte)
        return nullptr;

    bool succeeded;
    RootedFunction throwTypeError(cx, NewFunction(cx, tte, ThrowTypeError, 0,
                                                  JSFunction::NATIVE_FUN, self, js::NullPtr()));
    if (!throwTypeError || !PreventExtensions(cx, throwTypeError, &succeeded))
        return nullptr;
    if (!succeeded) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_CANT_CHANGE_EXTENSIBILITY);
        return nullptr;
    }

    self->setThrowTypeError(throwTypeError);

    return functionProto;
}

const Class JSFunction::class_ = {
    js_Function_str,
    JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Function),
    nullptr,                 /* addProperty */
    nullptr,                 /* delProperty */
    nullptr,                 /* getProperty */
    nullptr,                 /* setProperty */
    fun_enumerate,
    js::fun_resolve,
    nullptr,                 /* convert     */
    nullptr,                 /* finalize    */
    nullptr,                 /* call        */
    fun_hasInstance,
    nullptr,                 /* construct   */
    fun_trace,
    {
        CreateFunctionConstructor,
        CreateFunctionPrototype,
        nullptr,
        function_methods,
        function_properties
    }
};

const Class* const js::FunctionClassPtr = &JSFunction::class_;

/* Find the body of a function (not including braces). */
bool
js::FindBody(JSContext* cx, HandleFunction fun, HandleLinearString src, size_t* bodyStart,
             size_t* bodyEnd)
{
    // We don't need principals, since those are only used for error reporting.
    CompileOptions options(cx);
    options.setFileAndLine("internal-findBody", 0);

    // For asm.js modules, there's no script.
    if (fun->hasScript())
        options.setVersion(fun->nonLazyScript()->getVersion());

    AutoKeepAtoms keepAtoms(cx->perThreadData);

    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, src))
        return false;

    const mozilla::Range<const char16_t> srcChars = stableChars.twoByteRange();
    TokenStream ts(cx, options, srcChars.start().get(), srcChars.length(), nullptr);
    int nest = 0;
    bool onward = true;
    // Skip arguments list.
    do {
        TokenKind tt;
        if (!ts.getToken(&tt))
            return false;
        switch (tt) {
          case TOK_NAME:
          case TOK_YIELD:
            if (nest == 0)
                onward = false;
            break;
          case TOK_LP:
            nest++;
            break;
          case TOK_RP:
            if (--nest == 0)
                onward = false;
            break;
          default:
            break;
        }
    } while (onward);
    TokenKind tt;
    if (!ts.getToken(&tt))
        return false;
    if (tt == TOK_ARROW) {
        if (!ts.getToken(&tt))
            return false;
    }
    bool braced = tt == TOK_LC;
    MOZ_ASSERT_IF(fun->isExprClosure(), !braced);
    *bodyStart = ts.currentToken().pos.begin;
    if (braced)
        *bodyStart += 1;
    mozilla::RangedPtr<const char16_t> end = srcChars.end();
    if (end[-1] == '}') {
        end--;
    } else {
        MOZ_ASSERT(!braced);
        for (; unicode::IsSpaceOrBOM2(end[-1]); end--)
            ;
    }
    *bodyEnd = end - srcChars.start();
    MOZ_ASSERT(*bodyStart <= *bodyEnd);
    return true;
}

JSString*
js::FunctionToString(JSContext* cx, HandleFunction fun, bool bodyOnly, bool lambdaParen)
{
    if (fun->isInterpretedLazy() && !fun->getOrCreateScript(cx))
        return nullptr;

    if (IsAsmJSModule(fun))
        return AsmJSModuleToString(cx, fun, !lambdaParen);
    if (IsAsmJSFunction(fun))
        return AsmJSFunctionToString(cx, fun);

    StringBuffer out(cx);
    RootedScript script(cx);

    if (fun->hasScript()) {
        script = fun->nonLazyScript();
        if (script->isGeneratorExp()) {
            if ((!bodyOnly && !out.append("function genexp() {")) ||
                !out.append("\n    [generator expression]\n") ||
                (!bodyOnly && !out.append("}")))
            {
                return nullptr;
            }
            return out.finishString();
        }
    }
    if (!bodyOnly) {
        // If we're not in pretty mode, put parentheses around lambda functions.
        if (fun->isInterpreted() && !lambdaParen && fun->isLambda() && !fun->isArrow()) {
            if (!out.append("("))
                return nullptr;
        }
        if (!fun->isArrow()) {
            if (!(fun->isStarGenerator() ? out.append("function* ") : out.append("function ")))
                return nullptr;
        }
        if (fun->atom()) {
            if (!out.append(fun->atom()))
                return nullptr;
        }
    }
    bool haveSource = fun->isInterpreted() && !fun->isSelfHostedBuiltin();
    if (haveSource && !script->scriptSource()->hasSourceData() &&
        !JSScript::loadSource(cx, script->scriptSource(), &haveSource))
    {
        return nullptr;
    }
    if (haveSource) {
        Rooted<JSFlatString*> src(cx, script->sourceData(cx));
        if (!src)
            return nullptr;

        bool exprBody = fun->isExprClosure();

        // The source data for functions created by calling the Function
        // constructor is only the function's body.  This depends on the fact,
        // asserted below, that in Function("function f() {}"), the inner
        // function's sourceStart points to the '(', not the 'f'.
        bool funCon = !fun->isArrow() &&
                      script->sourceStart() == 0 &&
                      script->sourceEnd() == script->scriptSource()->length() &&
                      script->scriptSource()->argumentsNotIncluded();

        // Functions created with the constructor can't be arrow functions or
        // expression closures.
        MOZ_ASSERT_IF(funCon, !fun->isArrow());
        MOZ_ASSERT_IF(funCon, !exprBody);
        MOZ_ASSERT_IF(!funCon && !fun->isArrow(),
                      src->length() > 0 && src->latin1OrTwoByteChar(0) == '(');

        // If a function inherits strict mode by having scopes above it that
        // have "use strict", we insert "use strict" into the body of the
        // function. This ensures that if the result of toString is evaled, the
        // resulting function will have the same semantics.
        bool addUseStrict = script->strict() && !script->explicitUseStrict() && !fun->isArrow();

        bool buildBody = funCon && !bodyOnly;
        if (buildBody) {
            // This function was created with the Function constructor. We don't
            // have source for the arguments, so we have to generate that. Part
            // of bug 755821 should be cobbling the arguments passed into the
            // Function constructor into the source string.
            if (!out.append("("))
                return nullptr;

            // Fish out the argument names.
            MOZ_ASSERT(script->bindings.numArgs() == fun->nargs());

            BindingIter bi(script);
            for (unsigned i = 0; i < fun->nargs(); i++, bi++) {
                MOZ_ASSERT(bi.argIndex() == i);
                if (i && !out.append(", "))
                    return nullptr;
                if (i == unsigned(fun->nargs() - 1) && fun->hasRest() && !out.append("..."))
                    return nullptr;
                if (!out.append(bi->name()))
                    return nullptr;
            }
            if (!out.append(") {\n"))
                return nullptr;
        }
        if ((bodyOnly && !funCon) || addUseStrict) {
            // We need to get at the body either because we're only supposed to
            // return the body or we need to insert "use strict" into the body.
            size_t bodyStart = 0, bodyEnd;

            // If the function is defined in the Function constructor, we
            // already have a body.
            if (!funCon) {
                MOZ_ASSERT(!buildBody);
                if (!FindBody(cx, fun, src, &bodyStart, &bodyEnd))
                    return nullptr;
            } else {
                bodyEnd = src->length();
            }

            if (addUseStrict) {
                // Output source up to beginning of body.
                if (!out.appendSubstring(src, 0, bodyStart))
                    return nullptr;
                if (exprBody) {
                    // We can't insert a statement into a function with an
                    // expression body. Do what the decompiler did, and insert a
                    // comment.
                    if (!out.append("/* use strict */ "))
                        return nullptr;
                } else {
                    if (!out.append("\n\"use strict\";\n"))
                        return nullptr;
                }
            }

            // Output just the body (for bodyOnly) or the body and possibly
            // closing braces (for addUseStrict).
            size_t dependentEnd = bodyOnly ? bodyEnd : src->length();
            if (!out.appendSubstring(src, bodyStart, dependentEnd - bodyStart))
                return nullptr;
        } else {
            if (!out.append(src))
                return nullptr;
        }
        if (buildBody) {
            if (!out.append("\n}"))
                return nullptr;
        }
        if (bodyOnly) {
            // Slap a semicolon on the end of functions with an expression body.
            if (exprBody && !out.append(";"))
                return nullptr;
        } else if (!lambdaParen && fun->isLambda() && !fun->isArrow()) {
            if (!out.append(")"))
                return nullptr;
        }
    } else if (fun->isInterpreted() && !fun->isSelfHostedBuiltin()) {
        if ((!bodyOnly && !out.append("() {\n    ")) ||
            !out.append("[sourceless code]") ||
            (!bodyOnly && !out.append("\n}")))
            return nullptr;
        if (!lambdaParen && fun->isLambda() && !fun->isArrow() && !out.append(")"))
            return nullptr;
    } else {
        MOZ_ASSERT(!fun->isExprClosure());

        if ((!bodyOnly && !out.append("() {\n    "))
            || !out.append("[native code]")
            || (!bodyOnly && !out.append("\n}")))
        {
            return nullptr;
        }
    }
    return out.finishString();
}

JSString*
fun_toStringHelper(JSContext* cx, HandleObject obj, unsigned indent)
{
    if (!obj->is<JSFunction>()) {
        if (obj->is<ProxyObject>())
            return Proxy::fun_toString(cx, obj, indent);
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                             JSMSG_INCOMPATIBLE_PROTO,
                             js_Function_str, js_toString_str,
                             "object");
        return nullptr;
    }

    RootedFunction fun(cx, &obj->as<JSFunction>());
    return FunctionToString(cx, fun, false, indent != JS_DONT_PRETTY_PRINT);
}

bool
js::fun_toString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(IsFunctionObject(args.calleev()));

    uint32_t indent = 0;

    if (args.length() != 0 && !ToUint32(cx, args[0], &indent))
        return false;

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    RootedString str(cx, fun_toStringHelper(cx, obj, indent));
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

#if JS_HAS_TOSOURCE
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
        str = fun_toStringHelper(cx, obj, JS_DONT_PRETTY_PRINT);
    else
        str = ObjectToSource(cx, obj);

    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}
#endif

bool
js_fun_call(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    HandleValue fval = args.thisv();
    if (!IsCallable(fval)) {
        ReportIncompatibleMethod(cx, args, &JSFunction::class_);
        return false;
    }

    args.setCallee(fval);
    args.setThis(args.get(0));

    if (args.length() > 0) {
        for (size_t i = 0; i < args.length() - 1; i++)
            args[i].set(args[i + 1]);
        args = CallArgsFromVp(args.length() - 1, vp);
    }

    return Invoke(cx, args);
}

// ES5 15.3.4.3
bool
js_fun_apply(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    HandleValue fval = args.thisv();
    if (!IsCallable(fval)) {
        ReportIncompatibleMethod(cx, args, &JSFunction::class_);
        return false;
    }

    // Step 2.
    if (args.length() < 2 || args[1].isNullOrUndefined())
        return js_fun_call(cx, (args.length() > 0) ? 1 : 0, vp);

    InvokeArgs args2(cx);

    // A JS_OPTIMIZED_ARGUMENTS magic value means that 'arguments' flows into
    // this apply call from a scripted caller and, as an optimization, we've
    // avoided creating it since apply can simply pull the argument values from
    // the calling frame (which we must do now).
    if (args[1].isMagic(JS_OPTIMIZED_ARGUMENTS)) {
        // Step 3-6.
        ScriptFrameIter iter(cx);
        MOZ_ASSERT(iter.numActualArgs() <= ARGS_LENGTH_MAX);
        if (!args2.init(iter.numActualArgs()))
            return false;

        args2.setCallee(fval);
        args2.setThis(args[0]);

        // Steps 7-8.
        iter.unaliasedForEachActual(cx, CopyTo(args2.array()));
    } else {
        // Step 3.
        if (!args[1].isObject()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
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
        if (length > ARGS_LENGTH_MAX) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_TOO_MANY_FUN_APPLY_ARGS);
            return false;
        }

        if (!args2.init(length))
            return false;

        // Push fval, obj, and aobj's elements as args.
        args2.setCallee(fval);
        args2.setThis(args[0]);

        // Steps 7-8.
        if (!GetElements(cx, aobj, length, args2.array()))
            return false;
    }

    // Step 9.
    if (!Invoke(cx, args2))
        return false;

    args.rval().set(args2.rval());
    return true;
}

static const uint32_t JSSLOT_BOUND_FUNCTION_TARGET     = 0;
static const uint32_t JSSLOT_BOUND_FUNCTION_THIS       = 1;
static const uint32_t JSSLOT_BOUND_FUNCTION_ARGS_COUNT = 2;

static const uint32_t BOUND_FUNCTION_RESERVED_SLOTS = 3;

inline bool
JSFunction::initBoundFunction(JSContext* cx, HandleObject target, HandleValue thisArg,
                              const Value* args, unsigned argslen)
{
    RootedFunction self(cx, this);

    /*
     * Convert to a dictionary to set the BOUND_FUNCTION flag and increase
     * the slot span to cover the arguments and additional slots for the 'this'
     * value and arguments count.
     */
    if (!self->toDictionaryMode(cx))
        return false;

    if (!self->JSObject::setFlags(cx, BaseShape::BOUND_FUNCTION))
        return false;

    if (!self->setSlotSpan(cx, BOUND_FUNCTION_RESERVED_SLOTS + argslen))
        return false;

    self->setSlot(JSSLOT_BOUND_FUNCTION_TARGET, ObjectValue(*target));
    self->setSlot(JSSLOT_BOUND_FUNCTION_THIS, thisArg);
    self->setSlot(JSSLOT_BOUND_FUNCTION_ARGS_COUNT, PrivateUint32Value(argslen));

    self->initSlotRange(BOUND_FUNCTION_RESERVED_SLOTS, args, argslen);

    return true;
}

JSObject*
JSFunction::getBoundFunctionTarget() const
{
    MOZ_ASSERT(isBoundFunction());

    return &getSlot(JSSLOT_BOUND_FUNCTION_TARGET).toObject();
}

const js::Value&
JSFunction::getBoundFunctionThis() const
{
    MOZ_ASSERT(isBoundFunction());

    return getSlot(JSSLOT_BOUND_FUNCTION_THIS);
}

const js::Value&
JSFunction::getBoundFunctionArgument(unsigned which) const
{
    MOZ_ASSERT(isBoundFunction());
    MOZ_ASSERT(which < getBoundFunctionArgumentCount());

    return getSlot(BOUND_FUNCTION_RESERVED_SLOTS + which);
}

size_t
JSFunction::getBoundFunctionArgumentCount() const
{
    MOZ_ASSERT(isBoundFunction());

    return getSlot(JSSLOT_BOUND_FUNCTION_ARGS_COUNT).toPrivateUint32();
}

/* static */ bool
JSFunction::createScriptForLazilyInterpretedFunction(JSContext* cx, HandleFunction fun)
{
    MOZ_ASSERT(fun->isInterpretedLazy());

    Rooted<LazyScript*> lazy(cx, fun->lazyScriptOrNull());
    if (lazy) {
        // Trigger a pre barrier on the lazy script being overwritten.
        if (cx->zone()->needsIncrementalBarrier())
            LazyScript::writeBarrierPre(lazy);

        // Suppress GC for now although we should be able to remove this by
        // making 'lazy' a Rooted<LazyScript*> (which requires adding a
        // THING_ROOT_LAZY_SCRIPT).
        AutoSuppressGC suppressGC(cx);

        RootedScript script(cx, lazy->maybeScript());

        // Only functions without inner functions or direct eval are
        // re-lazified. Functions with either of those are on the static scope
        // chain of their inner functions, or in the case of eval, possibly
        // eval'd inner functions. This prohibits re-lazification as
        // StaticScopeIter queries isHeavyweight of those functions, which
        // requires a non-lazy script.
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
            if (!lazy->functionDelazifying(cx))
                return false;
            script = lazy->functionNonDelazifying()->nonLazyScript();
            if (!script)
                return false;

            fun->setUnlazifiedScript(script);
            return true;
        }

        // Lazy script caching is only supported for leaf functions. If a
        // script with inner functions was returned by the cache, those inner
        // functions would be delazified when deep cloning the script, even if
        // they have never executed.
        //
        // Additionally, the lazy script cache is not used during incremental
        // GCs, to avoid resurrecting dead scripts after incremental sweeping
        // has started.
        if (canRelazify && !JS::IsIncrementalGCInProgress(cx->runtime())) {
            LazyScriptCache::Lookup lookup(cx, lazy);
            cx->runtime()->lazyScriptCache.lookup(lookup, script.address());
        }

        if (script) {
            RootedObject enclosingScope(cx, lazy->enclosingScope());
            RootedScript clonedScript(cx, CloneScript(cx, enclosingScope, fun, script));
            if (!clonedScript)
                return false;

            clonedScript->setSourceObject(lazy->sourceObject());

            fun->initAtom(script->functionNonDelazifying()->displayAtom());
            clonedScript->setFunction(fun);

            fun->setUnlazifiedScript(clonedScript);

            if (!lazy->maybeScript())
                lazy->initScript(clonedScript);
            return true;
        }

        MOZ_ASSERT(lazy->scriptSource()->hasSourceData());

        // Parse and compile the script from source.
        UncompressedSourceCache::AutoHoldEntry holder;
        const char16_t* chars = lazy->scriptSource()->chars(cx, holder);
        if (!chars)
            return false;

        const char16_t* lazyStart = chars + lazy->begin();
        size_t lazyLength = lazy->end() - lazy->begin();

        if (!frontend::CompileLazyFunction(cx, lazy, lazyStart, lazyLength))
            return false;

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

            LazyScriptCache::Lookup lookup(cx, lazy);
            cx->runtime()->lazyScriptCache.insert(lookup, script);

            // Remember the lazy script on the compiled script, so it can be
            // stored on the function again in case of re-lazification.
            // Only functions without inner functions are re-lazified.
            script->setLazyScript(lazy);
        }
        return true;
    }

    /* Lazily cloned self-hosted script. */
    MOZ_ASSERT(fun->isSelfHostedBuiltin());
    RootedAtom funAtom(cx, &fun->getExtendedSlot(0).toString()->asAtom());
    if (!funAtom)
        return false;
    Rooted<PropertyName*> funName(cx, funAtom->asPropertyName());
    return cx->runtime()->cloneSelfHostedFunctionScript(cx, funName, fun);
}

void
JSFunction::relazify(JSTracer* trc)
{
    JSScript* script = nonLazyScript();
    MOZ_ASSERT(script->isRelazifiable());
    MOZ_ASSERT(trc->runtime()->allowRelazificationForTesting || !compartment()->hasBeenEntered());
    MOZ_ASSERT(!compartment()->isDebuggee());

    // If the script's canonical function isn't lazy, we have to mark the
    // script. Otherwise, the following scenario would leave it unmarked
    // and cause it to be swept while a function is still expecting it to be
    // valid:
    // 1. an incremental GC slice causes the canonical function to relazify
    // 2. a clone is used and delazifies the canonical function
    // 3. another GC slice causes the clone to relazify
    // The result is that no function marks the script, but the canonical
    // function expects it to be valid.
    if (script->functionNonDelazifying()->hasScript())
        MarkScriptUnbarriered(trc, &u.i.s.script_, "script");

    flags_ &= ~INTERPRETED;
    flags_ |= INTERPRETED_LAZY;
    LazyScript* lazy = script->maybeLazyScript();
    u.i.s.lazy_ = lazy;
    if (lazy) {
        MOZ_ASSERT(!isSelfHostedBuiltin());
        // If this is the script stored in the lazy script to be cloned
        // for un-lazifying other functions, reset it so the script can
        // be freed.
        if (lazy->maybeScript() == script)
            lazy->resetScript();
        MarkLazyScriptUnbarriered(trc, &u.i.s.lazy_, "lazyScript");
    } else {
        MOZ_ASSERT(isSelfHostedBuiltin());
        MOZ_ASSERT(isExtended());
        MOZ_ASSERT(getExtendedSlot(0).toString()->isAtom());
    }
}

/* ES5 15.3.4.5.1 and 15.3.4.5.2. */
bool
js::CallOrConstructBoundFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedFunction fun(cx, &args.callee().as<JSFunction>());
    MOZ_ASSERT(fun->isBoundFunction());

    /* 15.3.4.5.1 step 1, 15.3.4.5.2 step 3. */
    unsigned argslen = fun->getBoundFunctionArgumentCount();

    if (args.length() + argslen > ARGS_LENGTH_MAX) {
        js_ReportAllocationOverflow(cx);
        return false;
    }

    /* 15.3.4.5.1 step 3, 15.3.4.5.2 step 1. */
    RootedObject target(cx, fun->getBoundFunctionTarget());

    /* 15.3.4.5.1 step 2. */
    const Value& boundThis = fun->getBoundFunctionThis();

    InvokeArgs invokeArgs(cx);
    if (!invokeArgs.init(args.length() + argslen))
        return false;

    /* 15.3.4.5.1, 15.3.4.5.2 step 4. */
    for (unsigned i = 0; i < argslen; i++)
        invokeArgs[i].set(fun->getBoundFunctionArgument(i));
    PodCopy(invokeArgs.array() + argslen, vp + 2, args.length());

    /* 15.3.4.5.1, 15.3.4.5.2 step 5. */
    invokeArgs.setCallee(ObjectValue(*target));

    bool constructing = args.isConstructing();
    if (!constructing)
        invokeArgs.setThis(boundThis);

    if (constructing ? !InvokeConstructor(cx, invokeArgs) : !Invoke(cx, invokeArgs))
        return false;

    args.rval().set(invokeArgs.rval());
    return true;
}

static bool
fun_isGenerator(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JSFunction* fun;
    if (!IsFunctionObject(args.thisv(), &fun)) {
        args.rval().setBoolean(false);
        return true;
    }

    args.rval().setBoolean(fun->isGenerator());
    return true;
}

// ES6 draft rev32 19.2.3.2
bool
js::fun_bind(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Step 1.
    RootedValue thisv(cx, args.thisv());

    // Step 2.
    if (!IsCallable(thisv)) {
        ReportIncompatibleMethod(cx, args, &JSFunction::class_);
        return false;
    }

    // Step 3.
    Value* boundArgs = nullptr;
    unsigned argslen = 0;
    if (args.length() > 1) {
        boundArgs = args.array() + 1;
        argslen = args.length() - 1;
    }

    // Steps 4-14.
    RootedValue thisArg(cx, args.length() >= 1 ? args[0] : UndefinedValue());
    RootedObject target(cx, &thisv.toObject());
    JSObject* boundFunction = js_fun_bind(cx, target, thisArg, boundArgs, argslen);
    if (!boundFunction)
        return false;

    // Step 15.
    args.rval().setObject(*boundFunction);
    return true;
}

JSObject*
js_fun_bind(JSContext* cx, HandleObject target, HandleValue thisArg,
            Value* boundArgs, unsigned argslen)
{
    double length = 0.0;
    // Try to avoid invoking the resolve hook.
    if (target->is<JSFunction>() && !target->as<JSFunction>().hasResolvedLength()) {
        uint16_t len;
        if (!target->as<JSFunction>().getLength(cx, &len))
            return nullptr;
        length = Max(0.0, double(len) - argslen);
    } else {
        // Steps 5-6.
        RootedId id(cx, NameToId(cx->names().length));
        bool hasLength;
        if (!HasOwnProperty(cx, target, id, &hasLength))
            return nullptr;

        // Step 7-8.
        if (hasLength) {
            // a-b.
            RootedValue targetLen(cx);
            if (!GetProperty(cx, target, target, id, &targetLen))
                return nullptr;
            // d.
            if (targetLen.isNumber())
                length = Max(0.0, JS::ToInteger(targetLen.toNumber()) - argslen);
        }
    }

    RootedString name(cx, cx->names().empty);
    if (target->is<JSFunction>() && !target->as<JSFunction>().hasResolvedName()) {
        if (target->as<JSFunction>().atom())
            name = target->as<JSFunction>().atom();
    } else {
        // Steps 11-12.
        RootedValue targetName(cx);
        if (!GetProperty(cx, target, target, cx->names().name, &targetName))
            return nullptr;

        // Step 13.
        if (targetName.isString())
            name = targetName.toString();
    }

    // Step 14. Relevant bits from SetFunctionName.
    StringBuffer sb(cx);
    // Disabled for B2G failures.
    // if (!sb.append("bound ") || !sb.append(name))
    //   return nullptr;
    if (!sb.append(name))
        return nullptr;

    RootedAtom nameAtom(cx, sb.finishAtom());
    if (!nameAtom)
        return nullptr;

    //  Step 4.
    JSFunction::Flags flags = target->isConstructor() ? JSFunction::NATIVE_CTOR
                                                      : JSFunction::NATIVE_FUN;
    RootedFunction fun(cx, NewFunction(cx, js::NullPtr(), CallOrConstructBoundFunction, length,
                                       flags, cx->global(), nameAtom));
    if (!fun)
        return nullptr;

    if (!fun->initBoundFunction(cx, target, thisArg, boundArgs, argslen))
        return nullptr;

    // Steps 9-10. Set length again, because NewFunction sometimes truncates.
    if (length != fun->nargs()) {
        RootedValue lengthVal(cx, NumberValue(length));
        if (!DefineProperty(cx, fun, cx->names().length, lengthVal, nullptr, nullptr,
                            JSPROP_READONLY))
        {
            return nullptr;
        }
    }

    return fun;
}

/*
 * Report "malformed formal parameter" iff no illegal char or similar scanner
 * error was already reported.
 */
static bool
OnBadFormal(JSContext* cx)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_BAD_FORMAL);
    return false;
}

const JSFunctionSpec js::function_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,   fun_toSource,   0,0),
#endif
    JS_FN(js_toString_str,   fun_toString,   0,0),
    JS_FN(js_apply_str,      js_fun_apply,   2,0),
    JS_FN(js_call_str,       js_fun_call,    1,0),
    JS_FN("bind",            fun_bind,       1,0),
    JS_FN("isGenerator",     fun_isGenerator,0,0),
    JS_FS_END
};

static bool
FunctionConstructor(JSContext* cx, unsigned argc, Value* vp, GeneratorKind generatorKind)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedString arg(cx);   // used multiple times below

    /* Block this call if security callbacks forbid it. */
    Rooted<GlobalObject*> global(cx, &args.callee().global());
    if (!GlobalObject::isRuntimeCodeGenEnabled(cx, global)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_CSP_BLOCKED_FUNCTION);
        return false;
    }

    AutoKeepAtoms keepAtoms(cx->perThreadData);
    AutoNameVector formals(cx);

    bool hasRest = false;

    bool isStarGenerator = generatorKind == StarGenerator;
    MOZ_ASSERT(generatorKind != LegacyGenerator);

    RootedScript maybeScript(cx);
    const char* filename;
    unsigned lineno;
    bool mutedErrors;
    uint32_t pcOffset;
    DescribeScriptedCallerForCompilation(cx, &maybeScript, &filename, &lineno, &pcOffset,
                                         &mutedErrors);

    const char* introductionType = "Function";
    if (generatorKind != NotGenerator)
        introductionType = "GeneratorFunction";

    const char* introducerFilename = filename;
    if (maybeScript && maybeScript->scriptSource()->introducerFilename())
        introducerFilename = maybeScript->scriptSource()->introducerFilename();

    CompileOptions options(cx);
    options.setMutedErrors(mutedErrors)
           .setFileAndLine(filename, 1)
           .setNoScriptRval(false)
           .setCompileAndGo(true)
           .setIntroductionInfo(introducerFilename, introductionType, lineno, maybeScript, pcOffset);

    unsigned n = args.length() ? args.length() - 1 : 0;
    if (n > 0) {
        /*
         * Collect the function-argument arguments into one string, separated
         * by commas, then make a tokenstream from that string, and scan it to
         * get the arguments.  We need to throw the full scanner at the
         * problem, because the argument string can legitimately contain
         * comments and linefeeds.  XXX It might be better to concatenate
         * everything up into a function definition and pass it to the
         * compiler, but doing it this way is less of a delta from the old
         * code.  See ECMA 15.3.2.1.
         */
        size_t args_length = 0;
        for (unsigned i = 0; i < n; i++) {
            /* Collect the lengths for all the function-argument arguments. */
            arg = ToString<CanGC>(cx, args[i]);
            if (!arg)
                return false;
            args[i].setString(arg);

            /*
             * Check for overflow.  The < test works because the maximum
             * JSString length fits in 2 fewer bits than size_t has.
             */
            size_t old_args_length = args_length;
            args_length = old_args_length + arg->length();
            if (args_length < old_args_length) {
                js_ReportAllocationOverflow(cx);
                return false;
            }
        }

        /* Add 1 for each joining comma and check for overflow (two ways). */
        size_t old_args_length = args_length;
        args_length = old_args_length + n - 1;
        if (args_length < old_args_length ||
            args_length >= ~(size_t)0 / sizeof(char16_t)) {
            js_ReportAllocationOverflow(cx);
            return false;
        }

        /*
         * Allocate a string to hold the concatenated arguments, including room
         * for a terminating 0. Mark cx->tempLifeAlloc for later release, to
         * free collected_args and its tokenstream in one swoop.
         */
        LifoAllocScope las(&cx->tempLifoAlloc());
        char16_t* cp = cx->tempLifoAlloc().newArray<char16_t>(args_length + 1);
        if (!cp) {
            js_ReportOutOfMemory(cx);
            return false;
        }
        ConstTwoByteChars collected_args(cp, args_length + 1);

        /*
         * Concatenate the arguments into the new string, separated by commas.
         */
        for (unsigned i = 0; i < n; i++) {
            JSLinearString* argLinear = args[i].toString()->ensureLinear(cx);
            if (!argLinear)
                return false;

            CopyChars(cp, *argLinear);
            cp += argLinear->length();

            /* Add separating comma or terminating 0. */
            *cp++ = (i + 1 < n) ? ',' : 0;
        }

        /*
         * Initialize a tokenstream that reads from the given string.  No
         * StrictModeGetter is needed because this TokenStream won't report any
         * strict mode errors.  Any strict mode errors which might be reported
         * here (duplicate argument names, etc.) will be detected when we
         * compile the function body.
         */
        TokenStream ts(cx, options, collected_args.start().get(), args_length,
                       /* strictModeGetter = */ nullptr);
        bool yieldIsValidName = ts.versionNumber() < JSVERSION_1_7 && !isStarGenerator;

        /* The argument string may be empty or contain no tokens. */
        TokenKind tt;
        if (!ts.getToken(&tt))
            return false;
        if (tt != TOK_EOF) {
            for (;;) {
                /* Check that it's a name. */
                if (hasRest) {
                    ts.reportError(JSMSG_PARAMETER_AFTER_REST);
                    return false;
                }

                if (tt == TOK_YIELD && yieldIsValidName)
                    tt = TOK_NAME;

                if (tt != TOK_NAME) {
                    if (tt == TOK_TRIPLEDOT) {
                        hasRest = true;
                        if (!ts.getToken(&tt))
                            return false;
                        if (tt == TOK_YIELD && yieldIsValidName)
                            tt = TOK_NAME;
                        if (tt != TOK_NAME) {
                            ts.reportError(JSMSG_NO_REST_NAME);
                            return false;
                        }
                    } else {
                        return OnBadFormal(cx);
                    }
                }

                if (!formals.append(ts.currentName()))
                    return false;

                /*
                 * Get the next token.  Stop on end of stream.  Otherwise
                 * insist on a comma, get another name, and iterate.
                 */
                if (!ts.getToken(&tt))
                    return false;
                if (tt == TOK_EOF)
                    break;
                if (tt != TOK_COMMA)
                    return OnBadFormal(cx);
                if (!ts.getToken(&tt))
                    return false;
            }
        }
    }

#ifdef DEBUG
    for (unsigned i = 0; i < formals.length(); ++i) {
        JSString* str = formals[i];
        MOZ_ASSERT(str->asAtom().asPropertyName() == formals[i]);
    }
#endif

    RootedString str(cx);
    if (!args.length())
        str = cx->runtime()->emptyString;
    else
        str = ToString<CanGC>(cx, args[args.length() - 1]);
    if (!str)
        return false;

    /*
     * NB: (new Function) is not lexically closed by its caller, it's just an
     * anonymous function in the top-level scope that its constructor inhabits.
     * Thus 'var x = 42; f = new Function("return x"); print(f())' prints 42,
     * and so would a call to f from another top-level's script or function.
     */
    RootedAtom anonymousAtom(cx, cx->names().anonymous);
    RootedObject proto(cx);
    if (isStarGenerator) {
        proto = GlobalObject::getOrCreateStarGeneratorFunctionPrototype(cx, global);
        if (!proto)
            return false;
    }
    RootedFunction fun(cx, NewFunctionWithProto(cx, js::NullPtr(), nullptr, 0,
                                                JSFunction::INTERPRETED_LAMBDA, global,
                                                anonymousAtom, proto,
                                                JSFunction::FinalizeKind, TenuredObject));
    if (!fun)
        return false;

    if (!JSFunction::setTypeForScriptedFunction(cx, fun))
        return false;

    if (hasRest)
        fun->setHasRest();

    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, str))
        return false;

    mozilla::Range<const char16_t> chars = stableChars.twoByteRange();
    SourceBufferHolder::Ownership ownership = stableChars.maybeGiveOwnershipToCaller()
                                              ? SourceBufferHolder::GiveOwnership
                                              : SourceBufferHolder::NoOwnership;
    bool ok;
    SourceBufferHolder srcBuf(chars.start().get(), chars.length(), ownership);
    if (isStarGenerator)
        ok = frontend::CompileStarGeneratorBody(cx, &fun, options, formals, srcBuf);
    else
        ok = frontend::CompileFunctionBody(cx, &fun, options, formals, srcBuf,
                                           /* enclosingScope = */ js::NullPtr());
    args.rval().setObject(*fun);
    return ok;
}

bool
js::Function(JSContext* cx, unsigned argc, Value* vp)
{
    return FunctionConstructor(cx, argc, vp, NotGenerator);
}

bool
js::Generator(JSContext* cx, unsigned argc, Value* vp)
{
    return FunctionConstructor(cx, argc, vp, StarGenerator);
}

bool
JSFunction::isBuiltinFunctionConstructor()
{
    return maybeNative() == Function || maybeNative() == Generator;
}

JSFunction*
js::NewFunction(ExclusiveContext* cx, HandleObject funobjArg, Native native, unsigned nargs,
                JSFunction::Flags flags, HandleObject parent, HandleAtom atom,
                gc::AllocKind allocKind /* = JSFunction::FinalizeKind */,
                NewObjectKind newKind /* = GenericObject */)
{
    return NewFunctionWithProto(cx, funobjArg, native, nargs, flags, parent, atom, NullPtr(),
                                allocKind, newKind);
}

JSFunction*
js::NewFunctionWithProto(ExclusiveContext* cx, HandleObject funobjArg, Native native,
                         unsigned nargs, JSFunction::Flags flags, HandleObject parent,
                         HandleAtom atom, HandleObject proto,
                         gc::AllocKind allocKind /* = JSFunction::FinalizeKind */,
                         NewObjectKind newKind /* = GenericObject */)
{
    MOZ_ASSERT(allocKind == JSFunction::FinalizeKind || allocKind == JSFunction::ExtendedFinalizeKind);
    MOZ_ASSERT(sizeof(JSFunction) <= gc::Arena::thingSize(JSFunction::FinalizeKind));
    MOZ_ASSERT(sizeof(FunctionExtended) <= gc::Arena::thingSize(JSFunction::ExtendedFinalizeKind));

    RootedObject funobj(cx, funobjArg);
    if (funobj) {
        MOZ_ASSERT(funobj->is<JSFunction>());
        MOZ_ASSERT(funobj->getParent() == parent);
        MOZ_ASSERT_IF(native, funobj->isSingleton());
    } else {
        // Don't mark asm.js module functions as singleton since they are
        // cloned (via CloneFunctionObjectIfNotSingleton) which assumes that
        // isSingleton implies isInterpreted.
        if (native && !IsAsmJSModuleNative(native))
            newKind = SingletonObject;
        RootedObject realParent(cx, SkipScopeParent(parent));
        funobj = NewObjectWithClassProto(cx, &JSFunction::class_, proto, realParent, allocKind,
                                         newKind);
        if (!funobj)
            return nullptr;
    }
    RootedFunction fun(cx, &funobj->as<JSFunction>());

    if (allocKind == JSFunction::ExtendedFinalizeKind)
        flags = JSFunction::Flags(flags | JSFunction::EXTENDED);

    /* Initialize all function members. */
    fun->setArgCount(uint16_t(nargs));
    fun->setFlags(flags);
    if (fun->isInterpreted()) {
        MOZ_ASSERT(!native);
        fun->mutableScript().init(nullptr);
        fun->initEnvironment(parent);
    } else {
        MOZ_ASSERT(fun->isNative());
        MOZ_ASSERT(native);
        fun->initNative(native, nullptr);
    }
    if (allocKind == JSFunction::ExtendedFinalizeKind)
        fun->initializeExtended();
    fun->initAtom(atom);

    return fun;
}

bool
js::CloneFunctionObjectUseSameScript(JSCompartment* compartment, HandleFunction fun)
{
    return compartment == fun->compartment() &&
           !fun->isSingleton() &&
           !ObjectGroup::useSingletonForClone(fun);
}

JSFunction*
js::CloneFunctionObject(JSContext* cx, HandleFunction fun, HandleObject parent,
                        gc::AllocKind allocKind, NewObjectKind newKindArg /* = GenericObject */)
{
    MOZ_ASSERT(parent);
    MOZ_ASSERT(!fun->isBoundFunction());

    bool useSameScript = CloneFunctionObjectUseSameScript(cx->compartment(), fun);

    if (!useSameScript && fun->isInterpretedLazy()) {
        JSAutoCompartment ac(cx, fun);
        if (!fun->getOrCreateScript(cx))
            return nullptr;
    }

    NewObjectKind newKind = useSameScript ? newKindArg : SingletonObject;
    RootedObject cloneProto(cx);
    if (fun->isStarGenerator()) {
        cloneProto = GlobalObject::getOrCreateStarGeneratorFunctionPrototype(cx, cx->global());
        if (!cloneProto)
            return nullptr;
    }
    RootedObject realParent(cx, SkipScopeParent(parent));
    JSObject* cloneobj = NewObjectWithClassProto(cx, &JSFunction::class_, cloneProto, realParent,
                                                 allocKind, newKind);
    if (!cloneobj)
        return nullptr;
    RootedFunction clone(cx, &cloneobj->as<JSFunction>());

    // Make sure fun didn't get re-lazified during a GC while creating the
    // clone.
    if (!useSameScript && fun->isInterpretedLazy()) {
        JSAutoCompartment ac(cx, fun);
        if (!fun->getOrCreateScript(cx))
            return nullptr;
    }

    uint16_t flags = fun->flags() & ~JSFunction::EXTENDED;
    if (allocKind == JSFunction::ExtendedFinalizeKind)
        flags |= JSFunction::EXTENDED;

    clone->setArgCount(fun->nargs());
    clone->setFlags(flags);
    if (fun->hasScript()) {
        clone->initScript(fun->nonLazyScript());
        clone->initEnvironment(parent);
    } else if (fun->isInterpretedLazy()) {
        MOZ_ASSERT(fun->compartment() == clone->compartment());
        LazyScript* lazy = fun->lazyScriptOrNull();
        clone->initLazyScript(lazy);
        clone->initEnvironment(parent);
    } else {
        clone->initNative(fun->native(), fun->jitInfo());
    }
    clone->initAtom(fun->displayAtom());

    if (allocKind == JSFunction::ExtendedFinalizeKind) {
        if (fun->isExtended() && fun->compartment() == cx->compartment()) {
            for (unsigned i = 0; i < FunctionExtended::NUM_EXTENDED_SLOTS; i++)
                clone->initExtendedSlot(i, fun->getExtendedSlot(i));
        } else {
            clone->initializeExtended();
        }
    }

    if (useSameScript) {
        /*
         * Clone the function, reusing its script. We can use the same group as
         * the original function provided that its prototype is correct.
         */
        if (fun->getProto() == clone->getProto())
            clone->setGroup(fun->group());
        return clone;
    }

    RootedFunction cloneRoot(cx, clone);

    /*
     * Across compartments we have to clone the script for interpreted
     * functions. Cross-compartment cloning only happens via JSAPI
     * (JS::CloneFunctionObject) which dynamically ensures that 'script' has
     * no enclosing lexical scope (only the global scope).
     */
    if (cloneRoot->isInterpreted() && !CloneFunctionScript(cx, fun, cloneRoot, newKindArg))
        return nullptr;

    return cloneRoot;
}

/*
 * Return an atom for use as the name of a builtin method with the given
 * property id.
 *
 * Function names are always strings. If id is the well-known @@iterator
 * symbol, this returns "[Symbol.iterator]".
 *
 * Implements step 4 of SetFunctionName in ES6 draft rev 27 (24 Aug 2014).
 */
JSAtom*
js::IdToFunctionName(JSContext* cx, HandleId id)
{
    if (JSID_IS_ATOM(id))
        return JSID_TO_ATOM(id);

    if (JSID_IS_SYMBOL(id)) {
        RootedAtom desc(cx, JSID_TO_SYMBOL(id)->description());
        StringBuffer sb(cx);
        if (!sb.append('[') || !sb.append(desc) || !sb.append(']'))
            return nullptr;
        return sb.finishAtom();
    }

    RootedValue idv(cx, IdToValue(id));
    return ToAtom<CanGC>(cx, idv);
}

JSFunction*
js::DefineFunction(JSContext* cx, HandleObject obj, HandleId id, Native native,
                   unsigned nargs, unsigned flags, AllocKind allocKind /* = FinalizeKind */,
                   NewObjectKind newKind /* = GenericObject */)
{
    PropertyOp gop;
    StrictPropertyOp sop;
    if (flags & JSFUN_STUB_GSOPS) {
        /*
         * JSFUN_STUB_GSOPS is a request flag only, not stored in fun->flags or
         * the defined property's attributes. This allows us to encode another,
         * internal flag using the same bit, JSFUN_EXPR_CLOSURE -- see jsfun.h
         * for more on this.
         */
        flags &= ~JSFUN_STUB_GSOPS;
        gop = nullptr;
        sop = nullptr;
    } else {
        gop = obj->getClass()->getProperty;
        sop = obj->getClass()->setProperty;
        MOZ_ASSERT(gop != JS_PropertyStub);
        MOZ_ASSERT(sop != JS_StrictPropertyStub);
    }

    JSFunction::Flags funFlags;
    if (!native)
        funFlags = JSFunction::INTERPRETED_LAZY;
    else
        funFlags = JSAPIToJSFunctionFlags(flags);

    RootedAtom atom(cx, IdToFunctionName(cx, id));
    if (!atom)
        return nullptr;

    RootedFunction fun(cx, NewFunction(cx, NullPtr(), native, nargs, funFlags, obj, atom,
                                       allocKind, newKind));
    if (!fun)
        return nullptr;

    RootedValue funVal(cx, ObjectValue(*fun));
    if (!DefineProperty(cx, obj, id, funVal, gop, sop, flags & ~JSFUN_FLAGS_MASK))
        return nullptr;

    return fun;
}

void
js::ReportIncompatibleMethod(JSContext* cx, CallReceiver call, const Class* clasp)
{
    RootedValue thisv(cx, call.thisv());

#ifdef DEBUG
    if (thisv.isObject()) {
        MOZ_ASSERT(thisv.toObject().getClass() != clasp ||
                   !thisv.toObject().isNative() ||
                   !thisv.toObject().getProto() ||
                   thisv.toObject().getProto()->getClass() != clasp);
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

    if (JSFunction* fun = ReportIfNotFunction(cx, call.calleev())) {
        JSAutoByteString funNameBytes;
        if (const char* funName = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
                                 clasp->name, funName, InformalValueTypeName(thisv));
        }
    }
}

void
js::ReportIncompatible(JSContext* cx, CallReceiver call)
{
    if (JSFunction* fun = ReportIfNotFunction(cx, call.calleev())) {
        JSAutoByteString funNameBytes;
        if (const char* funName = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_METHOD,
                                 funName, "method", InformalValueTypeName(call.thisv()));
        }
    }
}

namespace JS {
namespace detail {

JS_PUBLIC_API(void)
CheckIsValidConstructible(Value calleev)
{
    JSObject* callee = &calleev.toObject();
    if (callee->is<JSFunction>())
        MOZ_ASSERT(callee->as<JSFunction>().isNativeConstructor());
    else
        MOZ_ASSERT(callee->constructHook() != nullptr);
}

} // namespace detail
} // namespace JS
