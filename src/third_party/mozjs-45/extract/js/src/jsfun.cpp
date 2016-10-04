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
#include "mozilla/CheckedInt.h"
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
#include "jit/InlinableNatives.h"
#include "jit/Ion.h"
#include "jit/JitFrameIterator.h"
#include "js/CallNonGenericMethod.h"
#include "js/Proxy.h"
#include "vm/Debugger.h"
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
    JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
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

    // Otherwise emit a strict warning about |f.arguments| to discourage use of
    // this non-standard, performance-harmful feature.
    if (!JS_ReportErrorFlagsAndNumber(cx, JSREPORT_WARNING | JSREPORT_STRICT, GetErrorMessage,
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
    if (!JS_ReportErrorFlagsAndNumber(cx, JSREPORT_WARNING | JSREPORT_STRICT, GetErrorMessage,
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
            JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
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
        JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, GetErrorMessage, nullptr,
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
    MOZ_ASSERT(fun->isInterpreted() || fun->isAsmJSNative());
    MOZ_ASSERT(!fun->isFunctionPrototype());
    MOZ_ASSERT(id == NameToId(cx->names().prototype));

    // Assert that fun is not a compiler-created function object, which
    // must never leak to script or embedding code and then be mutated.
    // Also assert that fun is not bound, per the ES5 15.3.4.5 ref above.
    MOZ_ASSERT(!IsInternalFunctionObject(*fun));
    MOZ_ASSERT(!fun->isBoundFunction());

    // Make the prototype object an instance of Object with the same parent as
    // the function object itself, unless the function is an ES6 generator.  In
    // that case, per the 15 July 2013 ES6 draft, section 15.19.3, its parent is
    // the GeneratorObjectPrototype singleton.
    bool isStarGenerator = fun->isStarGenerator();
    Rooted<GlobalObject*> global(cx, &fun->global());
    RootedObject objProto(cx);
    if (isStarGenerator)
        objProto = GlobalObject::getOrCreateStarGeneratorObjectPrototype(cx, global);
    else
        objProto = fun->global().getOrCreateObjectPrototype(cx);
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
    if (!isStarGenerator) {
        RootedValue objVal(cx, ObjectValue(*fun));
        if (!DefineProperty(cx, proto, cx->names().constructor, objVal, nullptr, nullptr, 0))
            return false;
    }

    // Per ES5 15.3.5.2 a user-defined function's .prototype property is
    // initially non-configurable, non-enumerable, and writable.
    RootedValue protoVal(cx, ObjectValue(*proto));
    return DefineProperty(cx, fun, id, protoVal, nullptr, nullptr,
                          JSPROP_PERMANENT | JSPROP_RESOLVING);
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
        if (fun->isBuiltin() || (!fun->isConstructor() && !fun->isGenerator()))
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

            uint16_t length;
            if (!fun->getLength(cx, &length))
                return false;

            v.setInt32(length);
        } else {
            if (fun->hasResolvedName())
                return true;

            if (fun->isClassConstructor()) {
                // It's impossible to have an empty named class expression. We
                // use empty as a sentinel when creating default class
                // constructors.
                MOZ_ASSERT(fun->atom() != cx->names().empty);

                // Unnamed class expressions should not get a .name property
                // at all.
                if (fun->atom() == nullptr)
                    return true;
            }

            v.setString(fun->atom() == nullptr ? cx->runtime()->emptyString : fun->atom());
        }

        if (!NativeDefineProperty(cx, fun, id, v, nullptr, nullptr,
                                  JSPROP_READONLY | JSPROP_RESOLVING))
        {
            return false;
        }

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
        if (!fun->isInterpreted()) {
            JSAutoByteString funNameBytes;
            if (const char* name = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                     JSMSG_NOT_SCRIPTED_FUNCTION, name);
            }
            return false;
        }

        if (fun->atom() || fun->hasGuessedAtom())
            firstword |= HasAtom;

        if (fun->isStarGenerator())
            firstword |= IsStarGenerator;

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
            MOZ_ASSERT(fun->lazyScript() == lazy);
        } else {
            MOZ_ASSERT(fun->nonLazyScript() == script);
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
        ReportValueError(cx, JSMSG_BAD_PROTOTYPE, -1, val, nullptr);
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
        TraceRange(trc, ArrayLength(toExtended()->extendedSlots),
                   (HeapValue*)toExtended()->extendedSlots, "nativeReserved");
    }

    if (atom_)
        TraceEdge(trc, &atom_, "atom");

    if (isInterpreted()) {
        // Functions can be be marked as interpreted despite having no script
        // yet at some points when parsing, and can be lazy with no lazy script
        // for self-hosted code.
        if (hasScript() && !hasUncompiledScript())
            TraceManuallyBarrieredEdge(trc, &u.i.s.script_, "script");
        else if (isInterpretedLazy() && u.i.s.lazy_)
            TraceManuallyBarrieredEdge(trc, &u.i.s.lazy_, "lazyScript");

        if (!isBeingParsed() && u.i.env_)
            TraceManuallyBarrieredEdge(trc, &u.i.env_, "fun_environment");
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
    JSObject* functionProto_ =
        NewFunctionWithProto(cx, nullptr, 0, JSFunction::INTERPRETED,
                             self, nullptr, objectProto, AllocKind::FUNCTION,
                             SingletonObject);
    if (!functionProto_)
        return nullptr;

    RootedFunction functionProto(cx, &functionProto_->as<JSFunction>());
    functionProto->setIsFunctionPrototype();

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
                                             /* enclosingScope = */ nullptr,
                                             /* savedCallerFun = */ false,
                                             options,
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
     * NewFunctionClone.
     */
    if (!JSObject::setNewGroupUnknown(cx, &JSFunction::class_, functionProto))
        return nullptr;

    // Construct the unique [[%ThrowTypeError%]] function object, used only for
    // "callee" and "caller" accessors on strict mode arguments objects.  (The
    // spec also uses this for "arguments" and "caller" on various functions,
    // but we're experimenting with implementing them using accessors on
    // |Function.prototype| right now.)
    //
    // Note that we can't use NewFunction here, even though we want the normal
    // Function.prototype for our proto, because we're still in the middle of
    // creating that as far as the world is concerned, so things will get all
    // confused.
    RootedFunction throwTypeError(cx,
      NewFunctionWithProto(cx, ThrowTypeError, 0, JSFunction::NATIVE_FUN,
                           nullptr, nullptr, functionProto, AllocKind::FUNCTION,
                           SingletonObject));
    if (!throwTypeError || !PreventExtensions(cx, throwTypeError))
        return nullptr;

    self->setThrowTypeError(throwTypeError);

    return functionProto;
}

const Class JSFunction::class_ = {
    js_Function_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Function),
    nullptr,                 /* addProperty */
    nullptr,                 /* delProperty */
    nullptr,                 /* getProperty */
    nullptr,                 /* setProperty */
    fun_enumerate,
    fun_resolve,
    fun_mayResolve,
    nullptr,                 /* finalize    */
    nullptr,                 /* call        */
    fun_hasInstance,
    nullptr,                 /* construct   */
    fun_trace,
    {
        CreateFunctionConstructor,
        CreateFunctionPrototype,
        nullptr,
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
    MOZ_ASSERT_IF(fun->isExprBody(), !braced);
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
js::FunctionToString(JSContext* cx, HandleFunction fun, bool lambdaParen)
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
            if (!out.append("function genexp() {") ||
                !out.append("\n    [generator expression]\n") ||
                !out.append("}"))
            {
                return nullptr;
            }
            return out.finishString();
        }
    }

    bool funIsMethodOrNonArrowLambda = (fun->isLambda() && !fun->isArrow()) || fun->isMethod() ||
                                        fun->isGetter() || fun->isSetter();

    // If we're not in pretty mode, put parentheses around lambda functions and methods.
    if (fun->isInterpreted() && !lambdaParen && funIsMethodOrNonArrowLambda) {
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

        bool exprBody = fun->isExprBody();

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

        bool buildBody = funCon;
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
        if (addUseStrict) {
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

            // Output the body and possibly closing braces (for addUseStrict).
            if (!out.appendSubstring(src, bodyStart, src->length() - bodyStart))
                return nullptr;
        } else {
            if (!out.append(src))
                return nullptr;
        }
        if (buildBody) {
            if (!out.append("\n}"))
                return nullptr;
        }
        if (!lambdaParen && funIsMethodOrNonArrowLambda) {
            if (!out.append(")"))
                return nullptr;
        }
    } else if (fun->isInterpreted() && !fun->isSelfHostedBuiltin()) {
        if (!out.append("() {\n    ") ||
            !out.append("[sourceless code]") ||
            !out.append("\n}"))
        {
            return nullptr;
        }
        if (!lambdaParen && fun->isLambda() && !fun->isArrow() && !out.append(")"))
            return nullptr;
    } else {
        MOZ_ASSERT(!fun->isExprBody());

        if (fun->isNative() && fun->native() == js::DefaultDerivedClassConstructor) {
            if (!out.append("(...args) {\n    ") ||
                !out.append("super(...args);\n}"))
            {
                return nullptr;
            }
        } else {
            if (!out.append("() {\n    "))
                return nullptr;

            if (!fun->isNative() || fun->native() != js::DefaultClassConstructor) {
                if (!out.append("[native code]"))
                    return nullptr;
            }

            if (!out.append("\n}"))
                return nullptr;
        }
    }
    return out.finishString();
}

JSString*
fun_toStringHelper(JSContext* cx, HandleObject obj, unsigned indent)
{
    if (!obj->is<JSFunction>()) {
        if (JSFunToStringOp op = obj->getOps()->funToString)
            return op(cx, obj, indent);

        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                             JSMSG_INCOMPATIBLE_PROTO,
                             js_Function_str, js_toString_str,
                             "object");
        return nullptr;
    }

    RootedFunction fun(cx, &obj->as<JSFunction>());
    return FunctionToString(cx, fun, indent != JS_DONT_PRETTY_PRINT);
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
js::fun_call(JSContext* cx, unsigned argc, Value* vp)
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
js::fun_apply(JSContext* cx, unsigned argc, Value* vp)
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
        if (!args2.init(iter.numActualArgs()))
            return false;

        args2.setCallee(fval);
        args2.setThis(args[0]);

        // Steps 7-8.
        iter.unaliasedForEachActual(cx, CopyTo(args2.array()));
    } else {
        // Step 3.
        if (!args[1].isObject()) {
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
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
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_TOO_MANY_FUN_APPLY_ARGS);
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

    self->setJitInfo(&jit::JitInfo_CallBoundFunction);

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
            RootedScript clonedScript(cx, CloneScriptIntoFunction(cx, enclosingScope, fun, script));
            if (!clonedScript)
                return false;

            clonedScript->setSourceObject(lazy->sourceObject());

            fun->initAtom(script->functionNonDelazifying()->displayAtom());

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

        if (!frontend::CompileLazyFunction(cx, lazy, lazyStart, lazyLength)) {
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
    if (!hasScript() || !u.i.s.script_)
        return;

    // Don't relazify functions in compartments that are active.
    JSCompartment* comp = compartment();
    if (comp->hasBeenEntered() && !rt->allowRelazificationForTesting)
        return;

    // Don't relazify if the compartment is being debugged or is the
    // self-hosting compartment.
    if (comp->isDebuggee() || comp->isSelfHosting)
        return;

    // Don't relazify functions with JIT code.
    if (!u.i.s.script_->isRelazifiable())
        return;

    // To delazify self-hosted builtins we need the name of the function
    // to clone. This name is stored in the first extended slot.
    if (isSelfHostedBuiltin() && !isExtended())
        return;

    JSScript* script = nonLazyScript();

    flags_ &= ~INTERPRETED;
    flags_ |= INTERPRETED_LAZY;
    LazyScript* lazy = script->maybeLazyScript();
    u.i.s.lazy_ = lazy;
    if (lazy) {
        MOZ_ASSERT(!isSelfHostedBuiltin());
    } else {
        MOZ_ASSERT(isSelfHostedBuiltin());
        MOZ_ASSERT(isExtended());
        MOZ_ASSERT(getExtendedSlot(LAZY_FUNCTION_NAME_SLOT).toString()->isAtom());
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
    unsigned boundArgsLen = fun->getBoundFunctionArgumentCount();

    uint32_t argsLen = args.length();
    if (argsLen + boundArgsLen > ARGS_LENGTH_MAX) {
        ReportAllocationOverflow(cx);
        return false;
    }

    /* 15.3.4.5.1 step 3, 15.3.4.5.2 step 1. */
    RootedObject target(cx, fun->getBoundFunctionTarget());

    /* 15.3.4.5.1 step 2. */
    const Value& boundThis = fun->getBoundFunctionThis();

    if (args.isConstructing()) {
        ConstructArgs cargs(cx);
        if (!cargs.init(argsLen + boundArgsLen))
            return false;

        /* 15.3.4.5.1, 15.3.4.5.2 step 4. */
        for (uint32_t i = 0; i < boundArgsLen; i++)
            cargs[i].set(fun->getBoundFunctionArgument(i));
        for (uint32_t i = 0; i < argsLen; i++)
            cargs[boundArgsLen + i].set(args[i]);

        RootedValue targetv(cx, ObjectValue(*target));

        /* ES6 9.4.1.2 step 5 */
        RootedValue newTarget(cx);
        if (&args.newTarget().toObject() == fun)
            newTarget.set(targetv);
        else
            newTarget.set(args.newTarget());

        return Construct(cx, targetv, cargs, newTarget, args.rval());
    }

    InvokeArgs invokeArgs(cx);
    if (!invokeArgs.init(argsLen + boundArgsLen))
        return false;

    /* 15.3.4.5.1, 15.3.4.5.2 step 4. */
    for (uint32_t i = 0; i < boundArgsLen; i++)
        invokeArgs[i].set(fun->getBoundFunctionArgument(i));
    for (uint32_t i = 0; i < argsLen; i++)
        invokeArgs[boundArgsLen + i].set(args[i]);

    /* 15.3.4.5.1, 15.3.4.5.2 step 5. */
    invokeArgs.setCallee(ObjectValue(*target));
    invokeArgs.setThis(boundThis);

    if (!Invoke(cx, invokeArgs))
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

static JSFunction*
NewNativeFunctionWithGivenProto(JSContext* cx, Native native, unsigned nargs,
                                HandleAtom atom, HandleObject proto)
{
    return NewFunctionWithProto(cx, native, nargs, JSFunction::NATIVE_FUN, nullptr, atom, proto,
                                AllocKind::FUNCTION, GenericObject, NewFunctionGivenProto);
}

static JSFunction*
NewNativeConstructorWithGivenProto(JSContext* cx, Native native, unsigned nargs,
                                   HandleAtom atom, HandleObject proto)
{
    return NewFunctionWithProto(cx, native, nargs, JSFunction::NATIVE_CTOR, nullptr, atom, proto,
                                AllocKind::FUNCTION, GenericObject, NewFunctionGivenProto);
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

    RootedValue thisArg(cx, args.length() >= 1 ? args[0] : UndefinedValue());
    RootedObject target(cx, &thisv.toObject());

    // This is part of step 4, but we're delaying allocating the function object.
    RootedObject proto(cx);
    if (!GetPrototype(cx, target, &proto))
        return false;

    double length = 0.0;
    // Try to avoid invoking the resolve hook.
    if (target->is<JSFunction>() && !target->as<JSFunction>().hasResolvedLength()) {
        uint16_t len;
        if (!target->as<JSFunction>().getLength(cx, &len))
            return false;
        length = Max(0.0, double(len) - argslen);
    } else {
        // Steps 5-6.
        RootedId id(cx, NameToId(cx->names().length));
        bool hasLength;
        if (!HasOwnProperty(cx, target, id, &hasLength))
            return false;

        // Step 7-8.
        if (hasLength) {
            // a-b.
            RootedValue targetLen(cx);
            if (!GetProperty(cx, target, target, id, &targetLen))
                return false;
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
            return false;

        // Step 13.
        if (targetName.isString())
            name = targetName.toString();
    }

    // Step 14. Relevant bits from SetFunctionName.
    StringBuffer sb(cx);
    // Disabled for B2G failures.
    // if (!sb.append("bound ") || !sb.append(name))
    //   return false;
    if (!sb.append(name))
        return false;

    RootedAtom nameAtom(cx, sb.finishAtom());
    if (!nameAtom)
        return false;

    // Step 4.
    RootedFunction fun(cx, target->isConstructor() ?
      NewNativeConstructorWithGivenProto(cx, CallOrConstructBoundFunction, length, nameAtom, proto) :
      NewNativeFunctionWithGivenProto(cx, CallOrConstructBoundFunction, length, nameAtom, proto));
    if (!fun)
        return false;

    if (!fun->initBoundFunction(cx, target, thisArg, boundArgs, argslen))
        return false;

    // Steps 9-10. Set length again, because NewNativeFunction/NewNativeConstructor
    // sometimes truncates.
    if (length != fun->nargs()) {
        RootedValue lengthVal(cx, NumberValue(length));
        if (!DefineProperty(cx, fun, cx->names().length, lengthVal, nullptr, nullptr,
                            JSPROP_READONLY))
        {
            return false;
        }
    }

    // Step 15.
    args.rval().setObject(*fun);
    return true;
}

/*
 * Report "malformed formal parameter" iff no illegal char or similar scanner
 * error was already reported.
 */
static bool
OnBadFormal(JSContext* cx)
{
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_FORMAL);
    return false;
}

const JSFunctionSpec js::function_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,   fun_toSource,   0,0),
#endif
    JS_FN(js_toString_str,   fun_toString,   0,0),
    JS_FN(js_apply_str,      fun_apply,      2,0),
    JS_FN(js_call_str,       fun_call,       1,0),
    JS_FN("bind",            fun_bind,       1,0),
    JS_FN("isGenerator",     fun_isGenerator,0,0),
    JS_FS_END
};

static bool
FunctionConstructor(JSContext* cx, unsigned argc, Value* vp, GeneratorKind generatorKind)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Block this call if security callbacks forbid it. */
    Rooted<GlobalObject*> global(cx, &args.callee().global());
    if (!GlobalObject::isRuntimeCodeGenEnabled(cx, global)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CSP_BLOCKED_FUNCTION);
        return false;
    }

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
           .setIntroductionInfo(introducerFilename, introductionType, lineno, maybeScript, pcOffset);

    Vector<char16_t> paramStr(cx);
    RootedString bodyText(cx);

    if (args.length() == 0) {
        bodyText = cx->names().empty;
    } else {
        // Collect the function-argument arguments into one string, separated
        // by commas, then make a tokenstream from that string, and scan it to
        // get the arguments.  We need to throw the full scanner at the
        // problem because the argument string may contain comments, newlines,
        // destructuring arguments, and similar manner of insanities.  ("I have
        // a feeling we're not in simple-comma-separated-parameters land any
        // more, Toto....")
        //
        // XXX It'd be better if the parser provided utility methods to parse
        //     an argument list, and to parse a function body given a parameter
        //     list.  But our parser provides no such pleasant interface now.
        unsigned n = args.length() - 1;

        // Convert the parameters-related arguments to strings, and determine
        // the length of the string containing the overall parameter list.
        mozilla::CheckedInt<uint32_t> paramStrLen = 0;
        RootedString str(cx);
        for (unsigned i = 0; i < n; i++) {
            str = ToString<CanGC>(cx, args[i]);
            if (!str)
                return false;

            args[i].setString(str);
            paramStrLen += str->length();
        }

        // Tack in space for any combining commas.
        if (n > 0)
            paramStrLen += n - 1;

        // Check for integer and string-size overflow.
        if (!paramStrLen.isValid() || paramStrLen.value() > JSString::MAX_LENGTH) {
            ReportAllocationOverflow(cx);
            return false;
        }

        uint32_t paramsLen = paramStrLen.value();

        // Fill a vector with the comma-joined arguments.  Careful!  This
        // string is *not* null-terminated!
        MOZ_ASSERT(paramStr.length() == 0);
        if (!paramStr.growBy(paramsLen)) {
            ReportOutOfMemory(cx);
            return false;
        }

        char16_t* cp = paramStr.begin();
        for (unsigned i = 0; i < n; i++) {
            JSLinearString* argLinear = args[i].toString()->ensureLinear(cx);
            if (!argLinear)
                return false;

            CopyChars(cp, *argLinear);
            cp += argLinear->length();

            if (i + 1 < n)
                *cp++ = ',';
        }

        MOZ_ASSERT(cp == paramStr.end());

        bodyText = ToString(cx, args[n]);
        if (!bodyText)
            return false;
    }

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
    } else {
        if (!GetPrototypeFromCallableConstructor(cx, args, &proto))
            return false;
    }

    RootedObject globalLexical(cx, &global->lexicalScope());
    RootedFunction fun(cx, NewFunctionWithProto(cx, nullptr, 0,
                                                JSFunction::INTERPRETED_LAMBDA, globalLexical,
                                                anonymousAtom, proto,
                                                AllocKind::FUNCTION, TenuredObject));
    if (!fun)
        return false;

    if (!JSFunction::setTypeForScriptedFunction(cx, fun))
        return false;

    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, bodyText))
        return false;

    bool hasRest = false;

    Rooted<PropertyNameVector> formals(cx, PropertyNameVector(cx));
    if (args.length() > 1) {
        // Initialize a tokenstream to parse the new function's arguments.  No
        // StrictModeGetter is needed because this TokenStream won't report any
        // strict mode errors.  Strict mode errors that might be reported here
        // (duplicate argument names, etc.) will be detected when we compile
        // the function body.
        //
        // XXX Bug!  We have to parse the body first to determine strictness.
        //     We have to know strictness to parse arguments correctly, in case
        //     arguments contains a strict mode violation.  And we should be
        //     using full-fledged arguments parsing here, in order to handle
        //     destructuring and other exotic syntaxes.
        AutoKeepAtoms keepAtoms(cx->perThreadData);
        TokenStream ts(cx, options, paramStr.begin(), paramStr.length(),
                       /* strictModeGetter = */ nullptr);
        bool yieldIsValidName = ts.versionNumber() < JSVERSION_1_7 && !isStarGenerator;

        // The argument string may be empty or contain no tokens.
        TokenKind tt;
        if (!ts.getToken(&tt))
            return false;
        if (tt != TOK_EOF) {
            while (true) {
                // Check that it's a name.
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

                // Get the next token.  Stop on end of stream.  Otherwise
                // insist on a comma, get another name, and iterate.
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

    if (hasRest)
        fun->setHasRest();

    mozilla::Range<const char16_t> chars = stableChars.twoByteRange();
    SourceBufferHolder::Ownership ownership = stableChars.maybeGiveOwnershipToCaller()
                                              ? SourceBufferHolder::GiveOwnership
                                              : SourceBufferHolder::NoOwnership;
    bool ok;
    SourceBufferHolder srcBuf(chars.start().get(), chars.length(), ownership);
    if (isStarGenerator)
        ok = frontend::CompileStarGeneratorBody(cx, &fun, options, formals, srcBuf);
    else
        ok = frontend::CompileFunctionBody(cx, &fun, options, formals, srcBuf);
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
js::NewNativeFunction(ExclusiveContext* cx, Native native, unsigned nargs, HandleAtom atom,
                      gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
                      NewObjectKind newKind /* = GenericObject */)
{
    return NewFunctionWithProto(cx, native, nargs, JSFunction::NATIVE_FUN,
                                nullptr, atom, nullptr, allocKind, newKind);
}

JSFunction*
js::NewNativeConstructor(ExclusiveContext* cx, Native native, unsigned nargs, HandleAtom atom,
                         gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
                         NewObjectKind newKind /* = GenericObject */,
                         JSFunction::Flags flags /* = JSFunction::NATIVE_CTOR */)
{
    MOZ_ASSERT(flags & JSFunction::NATIVE_CTOR);
    return NewFunctionWithProto(cx, native, nargs, flags, nullptr, atom,
                                nullptr, allocKind, newKind);
}

JSFunction*
js::NewScriptedFunction(ExclusiveContext* cx, unsigned nargs,
                        JSFunction::Flags flags, HandleAtom atom,
                        gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
                        NewObjectKind newKind /* = GenericObject */,
                        HandleObject enclosingDynamicScopeArg /* = nullptr */)
{
    RootedObject enclosingDynamicScope(cx, enclosingDynamicScopeArg);
    if (!enclosingDynamicScope)
        enclosingDynamicScope = &cx->global()->lexicalScope();
    return NewFunctionWithProto(cx, nullptr, nargs, flags, enclosingDynamicScope,
                                atom, nullptr, allocKind, newKind);
}

#ifdef DEBUG
static bool
NewFunctionScopeIsWellFormed(ExclusiveContext* cx, HandleObject parent)
{
    // Assert that the parent is null, global, or a debug scope proxy. All
    // other cases of polluting global scope behavior are handled by
    // ScopeObjects (viz. non-syntactic DynamicWithObject and
    // NonSyntacticVariablesObject).
    RootedObject realParent(cx, SkipScopeParent(parent));
    return !realParent || realParent == cx->global() ||
           realParent->is<DebugScopeObject>();
}
#endif

JSFunction*
js::NewFunctionWithProto(ExclusiveContext* cx, Native native,
                         unsigned nargs, JSFunction::Flags flags, HandleObject enclosingDynamicScope,
                         HandleAtom atom, HandleObject proto,
                         gc::AllocKind allocKind /* = AllocKind::FUNCTION */,
                         NewObjectKind newKind /* = GenericObject */,
                         NewFunctionProtoHandling protoHandling /* = NewFunctionClassProto */)
{
    MOZ_ASSERT(allocKind == AllocKind::FUNCTION || allocKind == AllocKind::FUNCTION_EXTENDED);
    MOZ_ASSERT_IF(native, !enclosingDynamicScope);
    MOZ_ASSERT(NewFunctionScopeIsWellFormed(cx, enclosingDynamicScope));

    RootedObject funobj(cx);
    // Don't mark asm.js module functions as singleton since they are
    // cloned (via CloneFunctionObjectIfNotSingleton) which assumes that
    // isSingleton implies isInterpreted.
    if (native && !IsAsmJSModuleNative(native))
        newKind = SingletonObject;

    if (protoHandling == NewFunctionClassProto) {
        funobj = NewObjectWithClassProto(cx, &JSFunction::class_, proto, allocKind,
                                         newKind);
    } else {
        funobj = NewObjectWithGivenTaggedProto(cx, &JSFunction::class_, AsTaggedProto(proto),
                                               allocKind, newKind);
    }
    if (!funobj)
        return nullptr;

    RootedFunction fun(cx, &funobj->as<JSFunction>());

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
        fun->initEnvironment(enclosingDynamicScope);
    } else {
        MOZ_ASSERT(fun->isNative());
        MOZ_ASSERT(native);
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
    if (IsSyntacticScope(newParent))
        return true;

    // We need to clone the script if we're interpreted and not already marked
    // as having a non-syntactic scope. If we're lazy, go ahead and clone the
    // script; see the big comment at the end of CopyScriptInternal for the
    // explanation of what's going on there.
    return !fun->isInterpreted() ||
           (fun->hasScript() && fun->nonLazyScript()->hasNonSyntacticScope());
}

static inline JSFunction*
NewFunctionClone(JSContext* cx, HandleFunction fun, NewObjectKind newKind,
                 gc::AllocKind allocKind, HandleObject proto)
{
    RootedObject cloneProto(cx, proto);
    if (!proto && fun->isStarGenerator()) {
        cloneProto = GlobalObject::getOrCreateStarGeneratorFunctionPrototype(cx, cx->global());
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
    clone->initAtom(fun->displayAtom());

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
js::CloneFunctionReuseScript(JSContext* cx, HandleFunction fun, HandleObject parent,
                             gc::AllocKind allocKind /* = FUNCTION */ ,
                             NewObjectKind newKind /* = GenericObject */,
                             HandleObject proto /* = nullptr */)
{
    MOZ_ASSERT(NewFunctionScopeIsWellFormed(cx, parent));
    MOZ_ASSERT(!fun->isBoundFunction());
    MOZ_ASSERT(CanReuseScriptForClone(cx->compartment(), fun, parent));

    RootedFunction clone(cx, NewFunctionClone(cx, fun, newKind, allocKind, proto));
    if (!clone)
        return nullptr;

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

    /*
     * Clone the function, reusing its script. We can use the same group as
     * the original function provided that its prototype is correct.
     */
    if (fun->getProto() == clone->getProto())
        clone->setGroup(fun->group());
    return clone;
}

JSFunction*
js::CloneFunctionAndScript(JSContext* cx, HandleFunction fun, HandleObject parent,
                           HandleObject newStaticScope,
                           gc::AllocKind allocKind /* = FUNCTION */,
                           HandleObject proto /* = nullptr */)
{
    MOZ_ASSERT(NewFunctionScopeIsWellFormed(cx, parent));
    MOZ_ASSERT(!fun->isBoundFunction());

    JSScript::AutoDelazify funScript(cx);
    if (fun->isInterpreted()) {
        funScript = fun;
        if (!funScript)
            return nullptr;
    }

    RootedFunction clone(cx, NewFunctionClone(cx, fun, SingletonObject, allocKind, proto));
    if (!clone)
        return nullptr;

    if (fun->hasScript()) {
        clone->initScript(nullptr);
        clone->initEnvironment(parent);
    } else {
        clone->initNative(fun->native(), fun->jitInfo());
    }

    /*
     * Across compartments or if we have to introduce a non-syntactic scope we
     * have to clone the script for interpreted functions. Cross-compartment
     * cloning only happens via JSAPI (JS::CloneFunctionObject) which
     * dynamically ensures that 'script' has no enclosing lexical scope (only
     * the global scope or other non-lexical scope).
     */
#ifdef DEBUG
    RootedObject terminatingScope(cx, parent);
    while (IsSyntacticScope(terminatingScope))
        terminatingScope = terminatingScope->enclosingScope();
    MOZ_ASSERT_IF(!terminatingScope->is<GlobalObject>(),
                  HasNonSyntacticStaticScopeChain(newStaticScope));
#endif

    if (clone->isInterpreted()) {
        RootedScript script(cx, fun->nonLazyScript());
        MOZ_ASSERT(script->compartment() == fun->compartment());
        MOZ_ASSERT(cx->compartment() == clone->compartment(),
                   "Otherwise we could relazify clone below!");

        RootedScript clonedScript(cx, CloneScriptIntoFunction(cx, newStaticScope, clone, script));
        if (!clonedScript)
            return nullptr;
        Debugger::onNewScript(cx, clonedScript);
    }

    return clone;
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
                   unsigned nargs, unsigned flags, AllocKind allocKind /* = AllocKind::FUNCTION */)
{
    GetterOp gop;
    SetterOp sop;
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

    RootedAtom atom(cx, IdToFunctionName(cx, id));
    if (!atom)
        return nullptr;

    RootedFunction fun(cx);
    if (!native)
        fun = NewScriptedFunction(cx, nargs,
                                  JSFunction::INTERPRETED_LAZY, atom,
                                  allocKind, GenericObject, obj);
    else if (flags & JSFUN_CONSTRUCTOR)
        fun = NewNativeConstructor(cx, native, nargs, atom, allocKind);
    else
        fun = NewNativeFunction(cx, native, nargs, atom, allocKind);

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
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_PROTO,
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
            JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_INCOMPATIBLE_METHOD,
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
        MOZ_ASSERT(callee->as<JSFunction>().isConstructor());
    else
        MOZ_ASSERT(callee->constructHook() != nullptr);
}

} // namespace detail
} // namespace JS
