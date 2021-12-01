/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/VMFunctions.h"

#include "builtin/TypedObject.h"
#include "frontend/BytecodeCompiler.h"
#include "jit/arm/Simulator-arm.h"
#include "jit/BaselineIC.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/mips32/Simulator-mips32.h"
#include "jit/mips64/Simulator-mips64.h"
#include "vm/ArrayObject.h"
#include "vm/Debugger.h"
#include "vm/Interpreter.h"
#include "vm/TraceLogging.h"

#include "jit/BaselineFrame-inl.h"
#include "jit/JitFrames-inl.h"
#include "vm/Debugger-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/TypeInference-inl.h"
#include "vm/UnboxedObject-inl.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

// Statics are initialized to null.
/* static */ VMFunction* VMFunction::functions;

AutoDetectInvalidation::AutoDetectInvalidation(JSContext* cx, MutableHandleValue rval)
  : cx_(cx),
    ionScript_(GetTopJitJSScript(cx)->ionScript()),
    rval_(rval),
    disabled_(false)
{ }

void
VMFunction::addToFunctions()
{
    this->next = functions;
    functions = this;
}

bool
InvokeFunction(JSContext* cx, HandleObject obj, bool constructing, bool ignoresReturnValue,
               uint32_t argc, Value* argv, MutableHandleValue rval)
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
    TraceLogStartEvent(logger, TraceLogger_Call);

    AutoArrayRooter argvRoot(cx, argc + 1 + constructing, argv);

    // Data in the argument vector is arranged for a JIT -> JIT call.
    RootedValue thisv(cx, argv[0]);
    Value* argvWithoutThis = argv + 1;

    RootedValue fval(cx, ObjectValue(*obj));
    if (constructing) {
        if (!IsConstructor(fval)) {
            ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, fval, nullptr);
            return false;
        }

        ConstructArgs cargs(cx);
        if (!cargs.init(cx, argc))
            return false;

        for (uint32_t i = 0; i < argc; i++)
            cargs[i].set(argvWithoutThis[i]);

        RootedValue newTarget(cx, argvWithoutThis[argc]);

        // If |this| hasn't been created, or is JS_UNINITIALIZED_LEXICAL,
        // we can use normal construction code without creating an extraneous
        // object.
        if (thisv.isMagic()) {
            MOZ_ASSERT(thisv.whyMagic() == JS_IS_CONSTRUCTING ||
                       thisv.whyMagic() == JS_UNINITIALIZED_LEXICAL);

            RootedObject obj(cx);
            if (!Construct(cx, fval, cargs, newTarget, &obj))
                return false;

            rval.setObject(*obj);
            return true;
        }

        // Otherwise the default |this| has already been created.  We could
        // almost perform a *call* at this point, but we'd break |new.target|
        // in the function.  So in this one weird case we call a one-off
        // construction path that *won't* set |this| to JS_IS_CONSTRUCTING.
        return InternalConstructWithProvidedThis(cx, fval, thisv, cargs, newTarget, rval);
    }

    InvokeArgsMaybeIgnoresReturnValue args(cx, ignoresReturnValue);
    if (!args.init(cx, argc))
        return false;

    for (size_t i = 0; i < argc; i++)
        args[i].set(argvWithoutThis[i]);

    return Call(cx, fval, thisv, args, rval);
}

bool
InvokeFunctionShuffleNewTarget(JSContext* cx, HandleObject obj, uint32_t numActualArgs,
                               uint32_t numFormalArgs, Value* argv, MutableHandleValue rval)
{
    MOZ_ASSERT(numFormalArgs > numActualArgs);
    argv[1 + numActualArgs] = argv[1 + numFormalArgs];
    return InvokeFunction(cx, obj, true, false, numActualArgs, argv, rval);
}

bool
InvokeFromInterpreterStub(JSContext* cx, InterpreterStubExitFrameLayout* frame)
{
    JitFrameLayout* jsFrame = frame->jsFrame();
    CalleeToken token = jsFrame->calleeToken();

    Value* argv = jsFrame->argv();
    uint32_t numActualArgs = jsFrame->numActualArgs();
    bool constructing = CalleeTokenIsConstructing(token);
    RootedFunction fun(cx, CalleeTokenToFunction(token));

    // Ensure new.target immediately follows the actual arguments (the arguments
    // rectifier added padding). See also InvokeFunctionShuffleNewTarget.
    if (constructing && numActualArgs < fun->nargs())
        argv[1 + numActualArgs] = argv[1 + fun->nargs()];

    RootedValue rval(cx);
    if (!InvokeFunction(cx, fun, constructing,
                        /* ignoresReturnValue = */ false,
                        numActualArgs, argv, &rval))
    {
        return false;
    }

    // Overwrite |this| with the return value.
    argv[0] = rval;
    return true;
}

#ifdef JS_SIMULATOR
static bool
CheckSimulatorRecursionLimitWithExtra(JSContext* cx, uint32_t extra)
{
    if (cx->simulator()->overRecursedWithExtra(extra)) {
        ReportOverRecursed(cx);
        return false;
    }
    return true;
}
#endif

bool
CheckOverRecursed(JSContext* cx)
{
    // We just failed the jitStackLimit check. There are two possible reasons:
    //  - jitStackLimit was the real stack limit and we're over-recursed
    //  - jitStackLimit was set to UINTPTR_MAX by JSRuntime::requestInterrupt
    //    and we need to call JSRuntime::handleInterrupt.
#ifdef JS_SIMULATOR
    if (!CheckSimulatorRecursionLimitWithExtra(cx, 0))
        return false;
#else
    if (!CheckRecursionLimit(cx))
        return false;
#endif
    gc::MaybeVerifyBarriers(cx);
    return cx->handleInterrupt();
}

// This function can get called in two contexts.  In the usual context, it's
// called with earlyCheck=false, after the env chain has been initialized on
// a baseline frame.  In this case, it's ok to throw an exception, so a failed
// stack check returns false, and a successful stack check promps a check for
// an interrupt from the runtime, which may also cause a false return.
//
// In the second case, it's called with earlyCheck=true, prior to frame
// initialization.  An exception cannot be thrown in this instance, so instead
// an error flag is set on the frame and true returned.
bool
CheckOverRecursedWithExtra(JSContext* cx, BaselineFrame* frame,
                           uint32_t extra, uint32_t earlyCheck)
{
    MOZ_ASSERT_IF(earlyCheck, !frame->overRecursed());

    // See |CheckOverRecursed| above.  This is a variant of that function which
    // accepts an argument holding the extra stack space needed for the Baseline
    // frame that's about to be pushed.
    uint8_t spDummy;
    uint8_t* checkSp = (&spDummy) - extra;
    if (earlyCheck) {
#ifdef JS_SIMULATOR
        (void)checkSp;
        if (!CheckSimulatorRecursionLimitWithExtra(cx, extra))
            frame->setOverRecursed();
#else
        if (!CheckRecursionLimitWithStackPointer(cx, checkSp))
            frame->setOverRecursed();
#endif
        return true;
    }

    // The OVERRECURSED flag may have already been set on the frame by an
    // early over-recursed check.  If so, throw immediately.
    if (frame->overRecursed())
        return false;

#ifdef JS_SIMULATOR
    if (!CheckSimulatorRecursionLimitWithExtra(cx, extra))
        return false;
#else
    if (!CheckRecursionLimitWithStackPointer(cx, checkSp))
        return false;
#endif

    gc::MaybeVerifyBarriers(cx);
    return cx->handleInterrupt();
}

JSObject*
BindVar(JSContext* cx, HandleObject envChain)
{
    JSObject* obj = envChain;
    while (!obj->isQualifiedVarObj())
        obj = obj->enclosingEnvironment();
    MOZ_ASSERT(obj);
    return obj;
}

bool
DefVar(JSContext* cx, HandlePropertyName dn, unsigned attrs, HandleObject envChain)
{
    // Given the ScopeChain, extract the VarObj.
    RootedObject obj(cx, BindVar(cx, envChain));
    return DefVarOperation(cx, obj, dn, attrs);
}

bool
DefLexical(JSContext* cx, HandlePropertyName dn, unsigned attrs, HandleObject envChain)
{
    // Find the extensible lexical scope.
    Rooted<LexicalEnvironmentObject*> lexicalEnv(cx,
        &NearestEnclosingExtensibleLexicalEnvironment(envChain));

    // Find the variables object.
    RootedObject varObj(cx, BindVar(cx, envChain));
    return DefLexicalOperation(cx, lexicalEnv, varObj, dn, attrs);
}

bool
DefGlobalLexical(JSContext* cx, HandlePropertyName dn, unsigned attrs)
{
    Rooted<LexicalEnvironmentObject*> globalLexical(cx, &cx->global()->lexicalEnvironment());
    return DefLexicalOperation(cx, globalLexical, cx->global(), dn, attrs);
}

bool
MutatePrototype(JSContext* cx, HandlePlainObject obj, HandleValue value)
{
    if (!value.isObjectOrNull())
        return true;

    RootedObject newProto(cx, value.toObjectOrNull());
    return SetPrototype(cx, obj, newProto);
}

template<bool Equal>
bool
LooselyEqual(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res)
{
    if (!js::LooselyEqual(cx, lhs, rhs, res))
        return false;
    if (!Equal)
        *res = !*res;
    return true;
}

template bool LooselyEqual<true>(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);
template bool LooselyEqual<false>(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);

template<bool Equal>
bool
StrictlyEqual(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res)
{
    if (!js::StrictlyEqual(cx, lhs, rhs, res))
        return false;
    if (!Equal)
        *res = !*res;
    return true;
}

template bool StrictlyEqual<true>(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);
template bool StrictlyEqual<false>(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res);

bool
LessThan(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res)
{
    return LessThanOperation(cx, lhs, rhs, res);
}

bool
LessThanOrEqual(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res)
{
    return LessThanOrEqualOperation(cx, lhs, rhs, res);
}

bool
GreaterThan(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res)
{
    return GreaterThanOperation(cx, lhs, rhs, res);
}

bool
GreaterThanOrEqual(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, bool* res)
{
    return GreaterThanOrEqualOperation(cx, lhs, rhs, res);
}

template<bool Equal>
bool
StringsEqual(JSContext* cx, HandleString lhs, HandleString rhs, bool* res)
{
    if (!js::EqualStrings(cx, lhs, rhs, res))
        return false;
    if (!Equal)
        *res = !*res;
    return true;
}

template bool StringsEqual<true>(JSContext* cx, HandleString lhs, HandleString rhs, bool* res);
template bool StringsEqual<false>(JSContext* cx, HandleString lhs, HandleString rhs, bool* res);

bool StringSplitHelper(JSContext* cx, HandleString str, HandleString sep,
                       HandleObjectGroup group, uint32_t limit,
                       MutableHandleValue result)
{
    JSObject* resultObj = str_split_string(cx, group, str, sep, limit);
    if (!resultObj)
        return false;

    result.setObject(*resultObj);
    return true;
}


bool
ArrayPopDense(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    MOZ_ASSERT(obj->is<ArrayObject>());

    AutoDetectInvalidation adi(cx, rval);

    JS::AutoValueArray<2> argv(cx);
    argv[0].setUndefined();
    argv[1].setObject(*obj);
    if (!js::array_pop(cx, 0, argv.begin()))
        return false;

    // If the result is |undefined|, the array was probably empty and we
    // have to monitor the return value.
    rval.set(argv[0]);
    if (rval.isUndefined())
        TypeScript::Monitor(cx, rval);
    return true;
}

bool
ArrayPushDense(JSContext* cx, HandleArrayObject arr, HandleValue v, uint32_t* length)
{
    *length = arr->length();
    DenseElementResult result = arr->setOrExtendDenseElements(cx, *length, v.address(), 1,
                                                              ShouldUpdateTypes::DontUpdate);
    if (result != DenseElementResult::Incomplete) {
        (*length)++;
        return result == DenseElementResult::Success;
    }

    // AutoDetectInvalidation uses GetTopJitJSScript(cx)->ionScript(), but it's
    // possible the setOrExtendDenseElements call already invalidated the
    // IonScript. JSJitFrameIter::ionScript works when the script is invalidated
    // so we use that instead.
    JSJitFrameIter frame(cx->activation()->asJit());
    MOZ_ASSERT(frame.type() == JitFrame_Exit);
    ++frame;
    IonScript* ionScript = frame.ionScript();

    JS::AutoValueArray<3> argv(cx);
    AutoDetectInvalidation adi(cx, argv[0], ionScript);
    argv[0].setUndefined();
    argv[1].setObject(*arr);
    argv[2].set(v);
    if (!js::array_push(cx, 1, argv.begin()))
        return false;

    if (argv[0].isInt32()) {
        *length = argv[0].toInt32();
        return true;
    }

    // array_push changed the length to be larger than INT32_MAX. In this case
    // OBJECT_FLAG_LENGTH_OVERFLOW was set, TI invalidated the script, and the
    // AutoDetectInvalidation instance on the stack will replace *length with
    // the actual return value during bailout.
    MOZ_ASSERT(adi.shouldSetReturnOverride());
    MOZ_ASSERT(argv[0].toDouble() == double(INT32_MAX) + 1);
    *length = 0;
    return true;
}

bool
ArrayShiftDense(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    MOZ_ASSERT(obj->is<ArrayObject>());

    AutoDetectInvalidation adi(cx, rval);

    JS::AutoValueArray<2> argv(cx);
    argv[0].setUndefined();
    argv[1].setObject(*obj);
    if (!js::array_shift(cx, 0, argv.begin()))
        return false;

    // If the result is |undefined|, the array was probably empty and we
    // have to monitor the return value.
    rval.set(argv[0]);
    if (rval.isUndefined())
        TypeScript::Monitor(cx, rval);
    return true;
}

JSString*
ArrayJoin(JSContext* cx, HandleObject array, HandleString sep)
{
    JS::AutoValueArray<3> argv(cx);
    argv[0].setUndefined();
    argv[1].setObject(*array);
    argv[2].setString(sep);
    if (!js::array_join(cx, 1, argv.begin()))
        return nullptr;
    return argv[0].toString();
}

bool
SetArrayLength(JSContext* cx, HandleObject obj, HandleValue value, bool strict)
{
    Handle<ArrayObject*> array = obj.as<ArrayObject>();

    RootedId id(cx, NameToId(cx->names().length));
    ObjectOpResult result;

    // SetArrayLength is called by IC stubs for SetProp and SetElem on arrays'
    // "length" property.
    //
    // ArraySetLength below coerces |value| before checking for length being
    // writable, and in the case of illegal values, will throw RangeError even
    // when "length" is not writable. This is incorrect observable behavior,
    // as a regular [[Set]] operation will check for "length" being
    // writable before attempting any assignment.
    //
    // So, perform ArraySetLength if and only if "length" is writable.
    if (array->lengthIsWritable()) {
        if (!ArraySetLength(cx, array, id, JSPROP_PERMANENT, value, result))
            return false;
    } else {
        MOZ_ALWAYS_TRUE(result.fail(JSMSG_READ_ONLY));
    }

    return result.checkStrictErrorOrWarning(cx, obj, id, strict);
}

bool
CharCodeAt(JSContext* cx, HandleString str, int32_t index, uint32_t* code)
{
    char16_t c;
    if (!str->getChar(cx, index, &c))
        return false;
    *code = c;
    return true;
}

JSFlatString*
StringFromCharCode(JSContext* cx, int32_t code)
{
    char16_t c = char16_t(code);

    if (StaticStrings::hasUnit(c))
        return cx->staticStrings().getUnit(c);

    return NewStringCopyNDontDeflate<CanGC>(cx, &c, 1);
}

JSString*
StringFromCodePoint(JSContext* cx, int32_t codePoint)
{
    RootedValue rval(cx, Int32Value(codePoint));
    if (!str_fromCodePoint_one_arg(cx, rval, &rval))
        return nullptr;

    return rval.toString();
}

bool
SetProperty(JSContext* cx, HandleObject obj, HandlePropertyName name, HandleValue value,
            bool strict, jsbytecode* pc)
{
    RootedId id(cx, NameToId(name));

    JSOp op = JSOp(*pc);

    if (op == JSOP_SETALIASEDVAR || op == JSOP_INITALIASEDLEXICAL) {
        // Aliased var assigns ignore readonly attributes on the property, as
        // required for initializing 'const' closure variables.
        Shape* shape = obj->as<NativeObject>().lookup(cx, name);
        MOZ_ASSERT(shape && shape->isDataProperty());
        obj->as<NativeObject>().setSlotWithType(cx, shape, value);
        return true;
    }

    RootedValue receiver(cx, ObjectValue(*obj));
    ObjectOpResult result;
    if (MOZ_LIKELY(!obj->getOpsSetProperty())) {
        if (op == JSOP_SETNAME || op == JSOP_STRICTSETNAME ||
            op == JSOP_SETGNAME || op == JSOP_STRICTSETGNAME)
        {
            if (!NativeSetProperty<Unqualified>(cx, obj.as<NativeObject>(), id, value, receiver,
                                                result))
            {
                return false;
            }
        } else {
            if (!NativeSetProperty<Qualified>(cx, obj.as<NativeObject>(), id, value, receiver,
                                              result))
            {
                return false;
            }
        }
    } else {
        if (!SetProperty(cx, obj, id, value, receiver, result))
            return false;
    }
    return result.checkStrictErrorOrWarning(cx, obj, id, strict);
}

bool
InterruptCheck(JSContext* cx)
{
    gc::MaybeVerifyBarriers(cx);

    {
        JSRuntime* rt = cx->runtime();
        JitRuntime::AutoPreventBackedgePatching apbp(rt);
        cx->zone()->group()->jitZoneGroup->patchIonBackedges(cx, JitZoneGroup::BackedgeLoopHeader);
    }

    return CheckForInterrupt(cx);
}

void*
MallocWrapper(JS::Zone* zone, size_t nbytes)
{
    AutoUnsafeCallWithABI unsafe;
    return zone->pod_malloc<uint8_t>(nbytes);
}

JSObject*
NewCallObject(JSContext* cx, HandleShape shape, HandleObjectGroup group)
{
    JSObject* obj = CallObject::create(cx, shape, group);
    if (!obj)
        return nullptr;

    // The JIT creates call objects in the nursery, so elides barriers for
    // the initializing writes. The interpreter, however, may have allocated
    // the call object tenured, so barrier as needed before re-entering.
    if (!IsInsideNursery(obj))
        cx->zone()->group()->storeBuffer().putWholeCell(obj);

    return obj;
}

JSObject*
NewSingletonCallObject(JSContext* cx, HandleShape shape)
{
    JSObject* obj = CallObject::createSingleton(cx, shape);
    if (!obj)
        return nullptr;

    // The JIT creates call objects in the nursery, so elides barriers for
    // the initializing writes. The interpreter, however, may have allocated
    // the call object tenured, so barrier as needed before re-entering.
    MOZ_ASSERT(!IsInsideNursery(obj),
               "singletons are created in the tenured heap");
    cx->zone()->group()->storeBuffer().putWholeCell(obj);

    return obj;
}

JSObject*
NewStringObject(JSContext* cx, HandleString str)
{
    return StringObject::create(cx, str);
}

bool
OperatorIn(JSContext* cx, HandleValue key, HandleObject obj, bool* out)
{
    RootedId id(cx);
    return ToPropertyKey(cx, key, &id) &&
           HasProperty(cx, obj, id, out);
}

bool
OperatorInI(JSContext* cx, uint32_t index, HandleObject obj, bool* out)
{
    RootedValue key(cx, Int32Value(index));
    return OperatorIn(cx, key, obj, out);
}

bool
GetIntrinsicValue(JSContext* cx, HandlePropertyName name, MutableHandleValue rval)
{
    if (!GlobalObject::getIntrinsicValue(cx, cx->global(), name, rval))
        return false;

    // This function is called when we try to compile a cold getintrinsic
    // op. MCallGetIntrinsicValue has an AliasSet of None for optimization
    // purposes, as its side effect is not observable from JS. We are
    // guaranteed to bail out after this function, but because of its AliasSet,
    // type info will not be reflowed. Manually monitor here.
    TypeScript::Monitor(cx, rval);

    return true;
}

bool
CreateThis(JSContext* cx, HandleObject callee, HandleObject newTarget, MutableHandleValue rval)
{
    rval.set(MagicValue(JS_IS_CONSTRUCTING));

    if (callee->is<JSFunction>()) {
        RootedFunction fun(cx, &callee->as<JSFunction>());
        if (fun->isInterpreted() && fun->isConstructor()) {
            JSScript* script = JSFunction::getOrCreateScript(cx, fun);
            AutoKeepTypeScripts keepTypes(cx);
            if (!script || !script->ensureHasTypes(cx, keepTypes))
                return false;
            if (!js::CreateThis(cx, fun, script, newTarget, GenericObject, rval))
                return false;
        }
    }

    return true;
}

void
GetDynamicName(JSContext* cx, JSObject* envChain, JSString* str, Value* vp)
{
    // Lookup a string on the env chain, returning either the value found or
    // undefined through rval. This function is infallible, and cannot GC or
    // invalidate.

    AutoUnsafeCallWithABI unsafe;

    JSAtom* atom;
    if (str->isAtom()) {
        atom = &str->asAtom();
    } else {
        atom = AtomizeString(cx, str);
        if (!atom) {
            vp->setUndefined();
            return;
        }
    }

    if (!frontend::IsIdentifier(atom) || frontend::IsKeyword(atom)) {
        vp->setUndefined();
        return;
    }

    PropertyResult prop;
    JSObject* scope = nullptr;
    JSObject* pobj = nullptr;
    if (LookupNameNoGC(cx, atom->asPropertyName(), envChain, &scope, &pobj, &prop)) {
        if (FetchNameNoGC(pobj, prop, MutableHandleValue::fromMarkedLocation(vp)))
            return;
    }

    vp->setUndefined();
}

void
PostWriteBarrier(JSRuntime* rt, JSObject* obj)
{
    AutoUnsafeCallWithABI unsafe;
    MOZ_ASSERT(!IsInsideNursery(obj));
    rt->gc.storeBuffer().putWholeCell(obj);
}

static const size_t MAX_WHOLE_CELL_BUFFER_SIZE = 4096;

template <IndexInBounds InBounds>
void
PostWriteElementBarrier(JSRuntime* rt, JSObject* obj, int32_t index)
{
    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(!IsInsideNursery(obj));

    if (InBounds == IndexInBounds::Yes) {
        MOZ_ASSERT(uint32_t(index) < obj->as<NativeObject>().getDenseInitializedLength());
    } else {
        if (MOZ_UNLIKELY(!obj->is<NativeObject>() ||
                         index < 0 ||
                         uint32_t(index) >= NativeObject::MAX_DENSE_ELEMENTS_COUNT))
        {
            rt->gc.storeBuffer().putWholeCell(obj);
            return;
        }
    }

    NativeObject* nobj = &obj->as<NativeObject>();
    if (nobj->isInWholeCellBuffer())
        return;

    if (nobj->getDenseInitializedLength() > MAX_WHOLE_CELL_BUFFER_SIZE
#ifdef JS_GC_ZEAL
         || rt->hasZealMode(gc::ZealMode::ElementsBarrier)
#endif
        )
    {
        rt->gc.storeBuffer().putSlot(nobj, HeapSlot::Element,
                                     nobj->unshiftedIndex(index),
                                     1);
        return;
    }

    rt->gc.storeBuffer().putWholeCell(obj);
}

template void
PostWriteElementBarrier<IndexInBounds::Yes>(JSRuntime* rt, JSObject* obj, int32_t index);

template void
PostWriteElementBarrier<IndexInBounds::Maybe>(JSRuntime* rt, JSObject* obj, int32_t index);

void
PostGlobalWriteBarrier(JSRuntime* rt, JSObject* obj)
{
    MOZ_ASSERT(obj->is<GlobalObject>());
    if (!obj->compartment()->globalWriteBarriered) {
        PostWriteBarrier(rt, obj);
        obj->compartment()->globalWriteBarriered = 1;
    }
}

int32_t
GetIndexFromString(JSString* str)
{
    // We shouldn't GC here as this is called directly from IC code.
    AutoUnsafeCallWithABI unsafe;

    if (!str->isFlat())
        return -1;

    uint32_t index;
    if (!str->asFlat().isIndex(&index) || index > INT32_MAX)
        return -1;

    return int32_t(index);
}

JSObject*
WrapObjectPure(JSContext* cx, JSObject* obj)
{
    // IC code calls this directly so we shouldn't GC.
    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(obj);
    MOZ_ASSERT(cx->compartment() != obj->compartment());

    // From: JSCompartment::getNonWrapperObjectForCurrentCompartment
    // Note that if the object is same-compartment, but has been wrapped into a
    // different compartment, we need to unwrap it and return the bare same-
    // compartment object. Note again that windows are always wrapped by a
    // WindowProxy even when same-compartment so take care not to strip this
    // particular wrapper.
    obj = UncheckedUnwrap(obj, /* stopAtWindowProxy = */ true);
    if (cx->compartment() == obj->compartment()) {
        MOZ_ASSERT(!IsWindow(obj));
        JS::ExposeObjectToActiveJS(obj);
        return obj;
    }

    // Try to Lookup an existing wrapper for this object. We assume that
    // if we can find such a wrapper, not calling preWrap is correct.
    if (WrapperMap::Ptr p = cx->compartment()->lookupWrapper(obj)) {
        JSObject* wrapped = &p->value().get().toObject();

        // Ensure the wrapper is still exposed.
        JS::ExposeObjectToActiveJS(wrapped);
        return wrapped;
    }

    return nullptr;
}

bool
DebugPrologue(JSContext* cx, BaselineFrame* frame, jsbytecode* pc, bool* mustReturn)
{
    *mustReturn = false;

    switch (Debugger::onEnterFrame(cx, frame)) {
      case JSTRAP_CONTINUE:
        return true;

      case JSTRAP_RETURN:
        // The script is going to return immediately, so we have to call the
        // debug epilogue handler as well.
        MOZ_ASSERT(frame->hasReturnValue());
        *mustReturn = true;
        return jit::DebugEpilogue(cx, frame, pc, true);

      case JSTRAP_THROW:
      case JSTRAP_ERROR:
        return false;

      default:
        MOZ_CRASH("bad Debugger::onEnterFrame status");
    }
}

bool
DebugEpilogueOnBaselineReturn(JSContext* cx, BaselineFrame* frame, jsbytecode* pc)
{
    if (!DebugEpilogue(cx, frame, pc, true)) {
        // DebugEpilogue popped the frame by updating packedExitFP, so run the
        // stop event here before we enter the exception handler.
        TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
        TraceLogStopEvent(logger, TraceLogger_Baseline);
        TraceLogStopEvent(logger, TraceLogger_Scripts);
        return false;
    }

    return true;
}

bool
DebugEpilogue(JSContext* cx, BaselineFrame* frame, jsbytecode* pc, bool ok)
{
    // If Debugger::onLeaveFrame returns |true| we have to return the frame's
    // return value. If it returns |false|, the debugger threw an exception.
    // In both cases we have to pop debug scopes.
    ok = Debugger::onLeaveFrame(cx, frame, pc, ok);

    // Unwind to the outermost environment and set pc to the end of the
    // script, regardless of error.
    EnvironmentIter ei(cx, frame, pc);
    UnwindAllEnvironmentsInFrame(cx, ei);
    JSScript* script = frame->script();
    frame->setOverridePc(script->lastPC());

    if (!ok) {
        // Pop this frame by updating packedExitFP, so that the exception
        // handling code will start at the previous frame.
        JitFrameLayout* prefix = frame->framePrefix();
        EnsureBareExitFrame(cx->activation()->asJit(), prefix);
        return false;
    }

    // Clear the override pc. This is not necessary for correctness: the frame
    // will return immediately, but this simplifies the check we emit in debug
    // builds after each callVM, to ensure this flag is not set.
    frame->clearOverridePc();
    return true;
}

void
FrameIsDebuggeeCheck(BaselineFrame* frame)
{
    AutoUnsafeCallWithABI unsafe;
    if (frame->script()->isDebuggee())
        frame->setIsDebuggee();
}

JSObject*
CreateGenerator(JSContext* cx, BaselineFrame* frame)
{
    return GeneratorObject::create(cx, frame);
}

bool
NormalSuspend(JSContext* cx, HandleObject obj, BaselineFrame* frame, jsbytecode* pc,
              uint32_t stackDepth)
{
    MOZ_ASSERT(*pc == JSOP_YIELD || *pc == JSOP_AWAIT);

    // Return value is still on the stack.
    MOZ_ASSERT(stackDepth >= 1);

    // The expression stack slots are stored on the stack in reverse order, so
    // we copy them to a Vector and pass a pointer to that instead. We use
    // stackDepth - 1 because we don't want to include the return value.
    AutoValueVector exprStack(cx);
    if (!exprStack.reserve(stackDepth - 1))
        return false;

    size_t firstSlot = frame->numValueSlots() - stackDepth;
    for (size_t i = 0; i < stackDepth - 1; i++)
        exprStack.infallibleAppend(*frame->valueSlot(firstSlot + i));

    MOZ_ASSERT(exprStack.length() == stackDepth - 1);

    return GeneratorObject::normalSuspend(cx, obj, frame, pc, exprStack.begin(), stackDepth - 1);
}

bool
FinalSuspend(JSContext* cx, HandleObject obj, jsbytecode* pc)
{
    MOZ_ASSERT(*pc == JSOP_FINALYIELDRVAL);
    GeneratorObject::finalSuspend(obj);
    return true;
}

bool
InterpretResume(JSContext* cx, HandleObject obj, HandleValue val, HandlePropertyName kind,
                MutableHandleValue rval)
{
    MOZ_ASSERT(obj->is<GeneratorObject>());

    RootedValue selfHostedFun(cx);
    if (!GlobalObject::getIntrinsicValue(cx, cx->global(), cx->names().InterpretGeneratorResume,
                                         &selfHostedFun))
    {
        return false;
    }

    MOZ_ASSERT(selfHostedFun.toObject().is<JSFunction>());

    FixedInvokeArgs<3> args(cx);

    args[0].setObject(*obj);
    args[1].set(val);
    args[2].setString(kind);

    return Call(cx, selfHostedFun, UndefinedHandleValue, args, rval);
}

bool
DebugAfterYield(JSContext* cx, BaselineFrame* frame)
{
    // The BaselineFrame has just been constructed by JSOP_RESUME in the
    // caller. We need to set its debuggee flag as necessary.
    if (frame->script()->isDebuggee())
        frame->setIsDebuggee();
    return true;
}

bool
GeneratorThrowOrReturn(JSContext* cx, BaselineFrame* frame, Handle<GeneratorObject*> genObj,
                       HandleValue arg, uint32_t resumeKind)
{
    // Set the frame's pc to the current resume pc, so that frame iterators
    // work. This function always returns false, so we're guaranteed to enter
    // the exception handler where we will clear the pc.
    JSScript* script = frame->script();
    uint32_t offset = script->yieldAndAwaitOffsets()[genObj->yieldAndAwaitIndex()];
    frame->setOverridePc(script->offsetToPC(offset));

    MOZ_ALWAYS_TRUE(DebugAfterYield(cx, frame));
    MOZ_ALWAYS_FALSE(js::GeneratorThrowOrReturn(cx, frame, genObj, arg, resumeKind));
    return false;
}

bool
CheckGlobalOrEvalDeclarationConflicts(JSContext* cx, BaselineFrame* frame)
{
    RootedScript script(cx, frame->script());
    RootedObject envChain(cx, frame->environmentChain());
    RootedObject varObj(cx, BindVar(cx, envChain));

    if (script->isForEval()) {
        // Strict eval and eval in parameter default expressions have their
        // own call objects.
        //
        // Non-strict eval may introduce 'var' bindings that conflict with
        // lexical bindings in an enclosing lexical scope.
        if (!script->bodyScope()->hasEnvironment()) {
            MOZ_ASSERT(!script->strict() &&
                       (!script->enclosingScope()->is<FunctionScope>() ||
                        !script->enclosingScope()->as<FunctionScope>().hasParameterExprs()));
            if (!CheckEvalDeclarationConflicts(cx, script, envChain, varObj))
                return false;
        }
    } else {
        Rooted<LexicalEnvironmentObject*> lexicalEnv(cx,
            &NearestEnclosingExtensibleLexicalEnvironment(envChain));
        if (!CheckGlobalDeclarationConflicts(cx, script, lexicalEnv, varObj))
            return false;
    }

    return true;
}

bool
GlobalNameConflictsCheckFromIon(JSContext* cx, HandleScript script)
{
    Rooted<LexicalEnvironmentObject*> globalLexical(cx, &cx->global()->lexicalEnvironment());
    return CheckGlobalDeclarationConflicts(cx, script, globalLexical, cx->global());
}

bool
InitFunctionEnvironmentObjects(JSContext* cx, BaselineFrame* frame)
{
    return frame->initFunctionEnvironmentObjects(cx);
}

bool
NewArgumentsObject(JSContext* cx, BaselineFrame* frame, MutableHandleValue res)
{
    ArgumentsObject* obj = ArgumentsObject::createExpected(cx, frame);
    if (!obj)
        return false;
    res.setObject(*obj);
    return true;
}

JSObject*
CopyLexicalEnvironmentObject(JSContext* cx, HandleObject env, bool copySlots)
{
    Handle<LexicalEnvironmentObject*> lexicalEnv = env.as<LexicalEnvironmentObject>();

    if (copySlots)
        return LexicalEnvironmentObject::clone(cx, lexicalEnv);

    return LexicalEnvironmentObject::recreate(cx, lexicalEnv);
}

JSObject*
InitRestParameter(JSContext* cx, uint32_t length, Value* rest, HandleObject templateObj,
                  HandleObject objRes)
{
    if (objRes) {
        Rooted<ArrayObject*> arrRes(cx, &objRes->as<ArrayObject>());

        MOZ_ASSERT(!arrRes->getDenseInitializedLength());
        MOZ_ASSERT(arrRes->group() == templateObj->group());

        // Fast path: we managed to allocate the array inline; initialize the
        // slots.
        if (length > 0) {
            if (!arrRes->ensureElements(cx, length))
                return nullptr;
            arrRes->initDenseElements(rest, length);
            arrRes->setLengthInt32(length);
        }
        return arrRes;
    }

    NewObjectKind newKind = templateObj->group()->shouldPreTenure()
                            ? TenuredObject
                            : GenericObject;
    ArrayObject* arrRes = NewDenseCopiedArray(cx, length, rest, nullptr, newKind);
    if (arrRes)
        arrRes->setGroup(templateObj->group());
    return arrRes;
}

bool
HandleDebugTrap(JSContext* cx, BaselineFrame* frame, uint8_t* retAddr, bool* mustReturn)
{
    *mustReturn = false;

    RootedScript script(cx, frame->script());
    jsbytecode* pc = script->baselineScript()->icEntryFromReturnAddress(retAddr).pc(script);

    if (*pc == JSOP_DEBUGAFTERYIELD) {
        // JSOP_DEBUGAFTERYIELD will set the frame's debuggee flag, but if we
        // set a breakpoint there we have to do it now.
        MOZ_ASSERT(!frame->isDebuggee());
        if (!DebugAfterYield(cx, frame))
            return false;
    }

    MOZ_ASSERT(frame->isDebuggee());
    MOZ_ASSERT(script->stepModeEnabled() || script->hasBreakpointsAt(pc));

    RootedValue rval(cx);
    JSTrapStatus status = JSTRAP_CONTINUE;

    if (script->stepModeEnabled())
        status = Debugger::onSingleStep(cx, &rval);

    if (status == JSTRAP_CONTINUE && script->hasBreakpointsAt(pc))
        status = Debugger::onTrap(cx, &rval);

    switch (status) {
      case JSTRAP_CONTINUE:
        break;

      case JSTRAP_ERROR:
        return false;

      case JSTRAP_RETURN:
        *mustReturn = true;
        frame->setReturnValue(rval);
        return jit::DebugEpilogue(cx, frame, pc, true);

      case JSTRAP_THROW:
        cx->setPendingException(rval);
        return false;

      default:
        MOZ_CRASH("Invalid trap status");
    }

    return true;
}

bool
OnDebuggerStatement(JSContext* cx, BaselineFrame* frame, jsbytecode* pc, bool* mustReturn)
{
    *mustReturn = false;

    switch (Debugger::onDebuggerStatement(cx, frame)) {
      case JSTRAP_ERROR:
        return false;

      case JSTRAP_CONTINUE:
        return true;

      case JSTRAP_RETURN:
        *mustReturn = true;
        return jit::DebugEpilogue(cx, frame, pc, true);

      case JSTRAP_THROW:
        return false;

      default:
        MOZ_CRASH("Invalid trap status");
    }
}

bool
GlobalHasLiveOnDebuggerStatement(JSContext* cx)
{
    AutoUnsafeCallWithABI unsafe;
    return cx->compartment()->isDebuggee() &&
           Debugger::hasLiveHook(cx->global(), Debugger::OnDebuggerStatement);
}

bool
PushLexicalEnv(JSContext* cx, BaselineFrame* frame, Handle<LexicalScope*> scope)
{
    return frame->pushLexicalEnvironment(cx, scope);
}

bool
PopLexicalEnv(JSContext* cx, BaselineFrame* frame)
{
    frame->popOffEnvironmentChain<LexicalEnvironmentObject>();
    return true;
}

bool
DebugLeaveThenPopLexicalEnv(JSContext* cx, BaselineFrame* frame, jsbytecode* pc)
{
    MOZ_ALWAYS_TRUE(DebugLeaveLexicalEnv(cx, frame, pc));
    frame->popOffEnvironmentChain<LexicalEnvironmentObject>();
    return true;
}

bool
FreshenLexicalEnv(JSContext* cx, BaselineFrame* frame)
{
    return frame->freshenLexicalEnvironment(cx);
}

bool
DebugLeaveThenFreshenLexicalEnv(JSContext* cx, BaselineFrame* frame, jsbytecode* pc)
{
    MOZ_ALWAYS_TRUE(DebugLeaveLexicalEnv(cx, frame, pc));
    return frame->freshenLexicalEnvironment(cx);
}

bool
RecreateLexicalEnv(JSContext* cx, BaselineFrame* frame)
{
    return frame->recreateLexicalEnvironment(cx);
}

bool
DebugLeaveThenRecreateLexicalEnv(JSContext* cx, BaselineFrame* frame, jsbytecode* pc)
{
    MOZ_ALWAYS_TRUE(DebugLeaveLexicalEnv(cx, frame, pc));
    return frame->recreateLexicalEnvironment(cx);
}

bool
DebugLeaveLexicalEnv(JSContext* cx, BaselineFrame* frame, jsbytecode* pc)
{
    MOZ_ASSERT(frame->script()->baselineScript()->hasDebugInstrumentation());
    if (cx->compartment()->isDebuggee())
        DebugEnvironments::onPopLexical(cx, frame, pc);
    return true;
}

bool
PushVarEnv(JSContext* cx, BaselineFrame* frame, HandleScope scope)
{
    return frame->pushVarEnvironment(cx, scope);
}

bool
PopVarEnv(JSContext* cx, BaselineFrame* frame)
{
    frame->popOffEnvironmentChain<VarEnvironmentObject>();
    return true;
}

bool
EnterWith(JSContext* cx, BaselineFrame* frame, HandleValue val, Handle<WithScope*> templ)
{
    return EnterWithOperation(cx, frame, val, templ);
}

bool
LeaveWith(JSContext* cx, BaselineFrame* frame)
{
    if (MOZ_UNLIKELY(frame->isDebuggee()))
        DebugEnvironments::onPopWith(frame);
    frame->popOffEnvironmentChain<WithEnvironmentObject>();
    return true;
}

bool
InitBaselineFrameForOsr(BaselineFrame* frame, InterpreterFrame* interpFrame,
                        uint32_t numStackValues)
{
    return frame->initForOsr(interpFrame, numStackValues);
}

JSObject*
CreateDerivedTypedObj(JSContext* cx, HandleObject descr,
                      HandleObject owner, int32_t offset)
{
    MOZ_ASSERT(descr->is<TypeDescr>());
    MOZ_ASSERT(owner->is<TypedObject>());
    Rooted<TypeDescr*> descr1(cx, &descr->as<TypeDescr>());
    Rooted<TypedObject*> owner1(cx, &owner->as<TypedObject>());
    return OutlineTypedObject::createDerived(cx, descr1, owner1, offset);
}

JSString*
StringReplace(JSContext* cx, HandleString string, HandleString pattern, HandleString repl)
{
    MOZ_ASSERT(string);
    MOZ_ASSERT(pattern);
    MOZ_ASSERT(repl);

    return str_replace_string_raw(cx, string, pattern, repl);
}

bool
RecompileImpl(JSContext* cx, bool force)
{
    MOZ_ASSERT(cx->currentlyRunningInJit());
    JitActivationIterator activations(cx);
    JSJitFrameIter frame(activations->asJit());

    MOZ_ASSERT(frame.type() == JitFrame_Exit);
    ++frame;

    RootedScript script(cx, frame.script());
    MOZ_ASSERT(script->hasIonScript());

    if (!IsIonEnabled(cx))
        return true;

    MethodStatus status = Recompile(cx, script, nullptr, nullptr, force);
    if (status == Method_Error)
        return false;

    return true;
}

bool
ForcedRecompile(JSContext* cx)
{
    return RecompileImpl(cx, /* force = */ true);
}

bool
Recompile(JSContext* cx)
{
    return RecompileImpl(cx, /* force = */ false);
}

bool
SetDenseElement(JSContext* cx, HandleNativeObject obj, int32_t index, HandleValue value,
                bool strict)
{
    // This function is called from Ion code for StoreElementHole's OOL path.
    // In this case we know the object is native and that no type changes are
    // needed.

    DenseElementResult result = obj->setOrExtendDenseElements(cx, index, value.address(), 1,
                                                              ShouldUpdateTypes::DontUpdate);
    if (result != DenseElementResult::Incomplete)
        return result == DenseElementResult::Success;

    RootedValue indexVal(cx, Int32Value(index));
    return SetObjectElement(cx, obj, indexVal, value, strict);
}

void
AutoDetectInvalidation::setReturnOverride()
{
    cx_->setIonReturnOverride(rval_.get());
}

void
AssertValidObjectPtr(JSContext* cx, JSObject* obj)
{
    AutoUnsafeCallWithABI unsafe;
#ifdef DEBUG
    // Check what we can, so that we'll hopefully assert/crash if we get a
    // bogus object (pointer).
    MOZ_ASSERT(obj->compartment() == cx->compartment());
    MOZ_ASSERT(obj->zoneFromAnyThread() == cx->zone());
    MOZ_ASSERT(obj->runtimeFromActiveCooperatingThread() == cx->runtime());

    MOZ_ASSERT_IF(!obj->hasLazyGroup() && obj->maybeShape(),
                  obj->group()->clasp() == obj->maybeShape()->getObjectClass());

    if (obj->isTenured()) {
        MOZ_ASSERT(obj->isAligned());
        gc::AllocKind kind = obj->asTenured().getAllocKind();
        MOZ_ASSERT(gc::IsObjectAllocKind(kind));
    }
#endif
}

void
AssertValidObjectOrNullPtr(JSContext* cx, JSObject* obj)
{
    AutoUnsafeCallWithABI unsafe;
    if (obj)
        AssertValidObjectPtr(cx, obj);
}

void
AssertValidStringPtr(JSContext* cx, JSString* str)
{
    AutoUnsafeCallWithABI unsafe;
#ifdef DEBUG
    // We can't closely inspect strings from another runtime.
    if (str->runtimeFromAnyThread() != cx->runtime()) {
        MOZ_ASSERT(str->isPermanentAtom());
        return;
    }

    if (str->isAtom())
        MOZ_ASSERT(str->zone()->isAtomsZone());
    else
        MOZ_ASSERT(str->zone() == cx->zone());

    MOZ_ASSERT(str->isAligned());
    MOZ_ASSERT(str->length() <= JSString::MAX_LENGTH);

    gc::AllocKind kind = str->getAllocKind();
    if (str->isFatInline()) {
        MOZ_ASSERT(kind == gc::AllocKind::FAT_INLINE_STRING ||
                   kind == gc::AllocKind::FAT_INLINE_ATOM);
    } else if (str->isExternal()) {
        MOZ_ASSERT(kind == gc::AllocKind::EXTERNAL_STRING);
    } else if (str->isAtom()) {
        MOZ_ASSERT(kind == gc::AllocKind::ATOM);
    } else if (str->isFlat()) {
        MOZ_ASSERT(kind == gc::AllocKind::STRING ||
                   kind == gc::AllocKind::FAT_INLINE_STRING ||
                   kind == gc::AllocKind::EXTERNAL_STRING);
    } else {
        MOZ_ASSERT(kind == gc::AllocKind::STRING);
    }
#endif
}

void
AssertValidSymbolPtr(JSContext* cx, JS::Symbol* sym)
{
    AutoUnsafeCallWithABI unsafe;

    // We can't closely inspect symbols from another runtime.
    if (sym->runtimeFromAnyThread() != cx->runtime()) {
        MOZ_ASSERT(sym->isWellKnownSymbol());
        return;
    }

    MOZ_ASSERT(sym->zone()->isAtomsZone());
    MOZ_ASSERT(sym->isAligned());
    if (JSString* desc = sym->description()) {
        MOZ_ASSERT(desc->isAtom());
        AssertValidStringPtr(cx, desc);
    }

    MOZ_ASSERT(sym->getAllocKind() == gc::AllocKind::SYMBOL);
}

void
AssertValidValue(JSContext* cx, Value* v)
{
    AutoUnsafeCallWithABI unsafe;
    if (v->isObject())
        AssertValidObjectPtr(cx, &v->toObject());
    else if (v->isString())
        AssertValidStringPtr(cx, v->toString());
    else if (v->isSymbol())
        AssertValidSymbolPtr(cx, v->toSymbol());
}

bool
ObjectIsCallable(JSObject* obj)
{
    AutoUnsafeCallWithABI unsafe;
    return obj->isCallable();
}

bool
ObjectIsConstructor(JSObject* obj)
{
    AutoUnsafeCallWithABI unsafe;
    return obj->isConstructor();
}

void
MarkValueFromJit(JSRuntime* rt, Value* vp)
{
    AutoUnsafeCallWithABI unsafe;
    TraceManuallyBarrieredEdge(&rt->gc.marker, vp, "write barrier");
}

void
MarkStringFromJit(JSRuntime* rt, JSString** stringp)
{
    AutoUnsafeCallWithABI unsafe;
    MOZ_ASSERT(*stringp);
    TraceManuallyBarrieredEdge(&rt->gc.marker, stringp, "write barrier");
}

void
MarkObjectFromJit(JSRuntime* rt, JSObject** objp)
{
    AutoUnsafeCallWithABI unsafe;
    MOZ_ASSERT(*objp);
    TraceManuallyBarrieredEdge(&rt->gc.marker, objp, "write barrier");
}

void
MarkShapeFromJit(JSRuntime* rt, Shape** shapep)
{
    AutoUnsafeCallWithABI unsafe;
    TraceManuallyBarrieredEdge(&rt->gc.marker, shapep, "write barrier");
}

void
MarkObjectGroupFromJit(JSRuntime* rt, ObjectGroup** groupp)
{
    AutoUnsafeCallWithABI unsafe;
    TraceManuallyBarrieredEdge(&rt->gc.marker, groupp, "write barrier");
}

bool
ThrowRuntimeLexicalError(JSContext* cx, unsigned errorNumber)
{
    ScriptFrameIter iter(cx);
    RootedScript script(cx, iter.script());
    ReportRuntimeLexicalError(cx, errorNumber, script, iter.pc());
    return false;
}

bool
ThrowReadOnlyError(JSContext* cx, HandleObject obj, int32_t index)
{
    // We have to throw different errors depending on whether |index| is past
    // the array length, etc. It's simpler to just call SetProperty to ensure
    // we match the interpreter.

    RootedValue objVal(cx, ObjectValue(*obj));
    RootedValue indexVal(cx, Int32Value(index));
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, indexVal, &id))
        return false;

    ObjectOpResult result;
    MOZ_ALWAYS_FALSE(SetProperty(cx, obj, id, UndefinedHandleValue, objVal, result) &&
                     result.checkStrictErrorOrWarning(cx, obj, id, /* strict = */ true));
    return false;
}

bool
ThrowBadDerivedReturn(JSContext* cx, HandleValue v)
{
    ReportValueError(cx, JSMSG_BAD_DERIVED_RETURN, JSDVG_IGNORE_STACK, v, nullptr);
    return false;
}

bool
BaselineThrowUninitializedThis(JSContext* cx, BaselineFrame* frame)
{
    return ThrowUninitializedThis(cx, frame);
}

bool
BaselineThrowInitializedThis(JSContext* cx)
{
    return ThrowInitializedThis(cx);
}


bool
ThrowObjectCoercible(JSContext* cx, HandleValue v)
{
    MOZ_ASSERT(v.isUndefined() || v.isNull());
    MOZ_ALWAYS_FALSE(ToObjectSlow(cx, v, true));
    return false;
}

bool
BaselineGetFunctionThis(JSContext* cx, BaselineFrame* frame, MutableHandleValue res)
{
    return GetFunctionThis(cx, frame, res);
}

bool
CallNativeGetter(JSContext* cx, HandleFunction callee, HandleObject obj,
                 MutableHandleValue result)
{
    MOZ_ASSERT(callee->isNative());
    JSNative natfun = callee->native();

    JS::AutoValueArray<2> vp(cx);
    vp[0].setObject(*callee.get());
    vp[1].setObject(*obj.get());

    if (!natfun(cx, 0, vp.begin()))
        return false;

    result.set(vp[0]);
    return true;
}

bool
CallNativeSetter(JSContext* cx, HandleFunction callee, HandleObject obj, HandleValue rhs)
{
    MOZ_ASSERT(callee->isNative());
    JSNative natfun = callee->native();

    JS::AutoValueArray<3> vp(cx);
    vp[0].setObject(*callee.get());
    vp[1].setObject(*obj.get());
    vp[2].set(rhs);

    return natfun(cx, 1, vp.begin());
}

bool
EqualStringsHelper(JSString* str1, JSString* str2)
{
    // IC code calls this directly so we shouldn't GC.
    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(str1->isAtom());
    MOZ_ASSERT(!str2->isAtom());
    MOZ_ASSERT(str1->length() == str2->length());

    JSLinearString* str2Linear = str2->ensureLinear(nullptr);
    if (!str2Linear)
        return false;

    return EqualChars(&str1->asLinear(), str2Linear);
}

bool
CheckIsCallable(JSContext* cx, HandleValue v, CheckIsCallableKind kind)
{
    if (!IsCallable(v))
        return ThrowCheckIsCallable(cx, kind);

    return true;
}

template <bool HandleMissing>
static MOZ_ALWAYS_INLINE bool
GetNativeDataProperty(JSContext* cx, NativeObject* obj, jsid id, Value* vp)
{
    // Fast path used by megamorphic IC stubs. Unlike our other property
    // lookup paths, this is optimized to be as fast as possible for simple
    // data property lookups.

    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(JSID_IS_ATOM(id) || JSID_IS_SYMBOL(id));

    while (true) {
        if (Shape* shape = obj->lastProperty()->search(cx, id)) {
            if (!shape->isDataProperty())
                return false;

            *vp = obj->getSlot(shape->slot());
            return true;
        }

        // Property not found. Watch out for Class hooks.
        if (MOZ_UNLIKELY(!obj->is<PlainObject>())) {
            if (ClassMayResolveId(cx->names(), obj->getClass(), id, obj))
                return false;
        }

        JSObject* proto = obj->staticPrototype();
        if (!proto) {
            if (HandleMissing) {
                vp->setUndefined();
                return true;
            }
            return false;
        }

        if (!proto->isNative())
            return false;
        obj = &proto->as<NativeObject>();
    }
}

template <bool HandleMissing>
bool
GetNativeDataProperty(JSContext* cx, JSObject* obj, PropertyName* name, Value* vp)
{
    // Condition checked by caller.
    MOZ_ASSERT(obj->isNative());
    return GetNativeDataProperty<HandleMissing>(cx, &obj->as<NativeObject>(), NameToId(name), vp);
}

template bool
GetNativeDataProperty<true>(JSContext* cx, JSObject* obj, PropertyName* name, Value* vp);

template bool
GetNativeDataProperty<false>(JSContext* cx, JSObject* obj, PropertyName* name, Value* vp);

static MOZ_ALWAYS_INLINE bool
ValueToAtomOrSymbol(JSContext* cx, Value& idVal, jsid* id)
{
    AutoUnsafeCallWithABI unsafe;

    if (MOZ_LIKELY(idVal.isString())) {
        JSString* s = idVal.toString();
        JSAtom* atom;
        if (s->isAtom()) {
            atom = &s->asAtom();
        } else {
            atom = AtomizeString(cx, s);
            if (!atom)
                return false;
        }
        *id = AtomToId(atom);
    } else if (idVal.isSymbol()) {
        *id = SYMBOL_TO_JSID(idVal.toSymbol());
    } else {
        if (!ValueToIdPure(idVal, id))
            return false;
    }

    // Watch out for ids that may be stored in dense elements.
    static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT < JSID_INT_MAX,
                  "All dense elements must have integer jsids");
    if (MOZ_UNLIKELY(JSID_IS_INT(*id)))
        return false;

    return true;
}

template <bool HandleMissing>
bool
GetNativeDataPropertyByValue(JSContext* cx, JSObject* obj, Value* vp)
{
    AutoUnsafeCallWithABI unsafe;

    // Condition checked by caller.
    MOZ_ASSERT(obj->isNative());

    // vp[0] contains the id, result will be stored in vp[1].
    Value idVal = vp[0];
    jsid id;
    if (!ValueToAtomOrSymbol(cx, idVal, &id))
        return false;

    Value* res = vp + 1;
    return GetNativeDataProperty<HandleMissing>(cx, &obj->as<NativeObject>(), id, res);
}

template bool
GetNativeDataPropertyByValue<true>(JSContext* cx, JSObject* obj, Value* vp);

template bool
GetNativeDataPropertyByValue<false>(JSContext* cx, JSObject* obj, Value* vp);

template <bool NeedsTypeBarrier>
bool
SetNativeDataProperty(JSContext* cx, JSObject* obj, PropertyName* name, Value* val)
{
    AutoUnsafeCallWithABI unsafe;

    if (MOZ_UNLIKELY(!obj->isNative()))
        return false;

    NativeObject* nobj = &obj->as<NativeObject>();
    Shape* shape = nobj->lastProperty()->search(cx, NameToId(name));
    if (!shape ||
        !shape->isDataProperty() ||
        !shape->writable())
    {
        return false;
    }

    if (NeedsTypeBarrier && !HasTypePropertyId(nobj, NameToId(name), *val))
        return false;

    nobj->setSlot(shape->slot(), *val);
    return true;
}

template bool
SetNativeDataProperty<true>(JSContext* cx, JSObject* obj, PropertyName* name, Value* val);

template bool
SetNativeDataProperty<false>(JSContext* cx, JSObject* obj, PropertyName* name, Value* val);

bool
ObjectHasGetterSetter(JSContext* cx, JSObject* objArg, Shape* propShape)
{
    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(propShape->hasGetterObject() || propShape->hasSetterObject());

    // Window objects may require outerizing (passing the WindowProxy to the
    // getter/setter), so we don't support them here.
    if (MOZ_UNLIKELY(!objArg->isNative() || IsWindow(objArg)))
        return false;

    NativeObject* nobj = &objArg->as<NativeObject>();
    jsid id = propShape->propid();

    while (true) {
        if (Shape* shape = nobj->lastProperty()->search(cx, id)) {
            if (shape == propShape)
                return true;
            if (shape->getterOrUndefined() == propShape->getterOrUndefined() &&
                shape->setterOrUndefined() == propShape->setterOrUndefined())
            {
                return true;
            }
            return false;
        }

        // Property not found. Watch out for Class hooks.
        if (!nobj->is<PlainObject>()) {
            if (ClassMayResolveId(cx->names(), nobj->getClass(), id, nobj))
                return false;
        }

        JSObject* proto = nobj->staticPrototype();
        if (!proto)
            return false;

        if (!proto->isNative())
            return false;
        nobj = &proto->as<NativeObject>();
    }
}

template <bool HasOwn>
bool
HasNativeDataProperty(JSContext* cx, JSObject* obj, Value* vp)
{
    AutoUnsafeCallWithABI unsafe;

    // vp[0] contains the id, result will be stored in vp[1].
    Value idVal = vp[0];
    jsid id;
    if (!ValueToAtomOrSymbol(cx, idVal, &id))
        return false;

    do {
        if (obj->isNative()) {
            if (obj->as<NativeObject>().lastProperty()->search(cx, id)) {
                vp[1].setBoolean(true);
                return true;
            }

            // Fail if there's a resolve hook, unless the mayResolve hook tells
            // us the resolve hook won't define a property with this id.
            if (MOZ_UNLIKELY(ClassMayResolveId(cx->names(), obj->getClass(), id, obj)))
                return false;
        } else if (obj->is<UnboxedPlainObject>()) {
            if (obj->as<UnboxedPlainObject>().containsUnboxedOrExpandoProperty(cx, id)) {
                vp[1].setBoolean(true);
                return true;
            }
        } else if (obj->is<TypedObject>()) {
            if (obj->as<TypedObject>().typeDescr().hasProperty(cx->names(), id)) {
                vp[1].setBoolean(true);
                return true;
            }
        } else {
            return false;
        }

        // If implementing Object.hasOwnProperty, don't follow protochain.
        if (HasOwn)
            break;

        // Get prototype. Objects that may allow dynamic prototypes are already
        // filtered out above.
        obj = obj->staticPrototype();
    } while (obj);

    // Missing property.
    vp[1].setBoolean(false);
    return true;
}

template bool
HasNativeDataProperty<true>(JSContext* cx, JSObject* obj, Value* vp);

template bool
HasNativeDataProperty<false>(JSContext* cx, JSObject* obj, Value* vp);


bool
HasNativeElement(JSContext* cx, NativeObject* obj, int32_t index, Value* vp)
{
    AutoUnsafeCallWithABI unsafe;

    MOZ_ASSERT(obj->getClass()->isNative());
    MOZ_ASSERT(!obj->getOpsHasProperty());
    MOZ_ASSERT(!obj->getOpsLookupProperty());
    MOZ_ASSERT(!obj->getOpsGetOwnPropertyDescriptor());

    if (MOZ_UNLIKELY(index < 0))
        return false;

    if (obj->containsDenseElement(index)) {
        vp[0].setBoolean(true);
        return true;
    }

    jsid id = INT_TO_JSID(index);
    if (obj->lastProperty()->search(cx, id)) {
        vp[0].setBoolean(true);
        return true;
    }

    // Fail if there's a resolve hook, unless the mayResolve hook tells
    // us the resolve hook won't define a property with this id.
    if (MOZ_UNLIKELY(ClassMayResolveId(cx->names(), obj->getClass(), id, obj)))
        return false;
    // TypedArrayObject are also native and contain indexed properties.
    if (MOZ_UNLIKELY(obj->is<TypedArrayObject>())) {
        vp[0].setBoolean(uint32_t(index) < obj->as<TypedArrayObject>().length());
        return true;
    }

    vp[0].setBoolean(false);
    return true;
}

JSString*
TypeOfObject(JSObject* obj, JSRuntime* rt)
{
    AutoUnsafeCallWithABI unsafe;
    JSType type = js::TypeOfObject(obj);
    return TypeName(type, *rt->commonNames);
}

bool
GetPrototypeOf(JSContext* cx, HandleObject target, MutableHandleValue rval)
{
    MOZ_ASSERT(target->hasDynamicPrototype());

    RootedObject proto(cx);
    if (!GetPrototype(cx, target, &proto))
        return false;
    rval.setObjectOrNull(proto);
    return true;
}

void
CloseIteratorFromIon(JSContext* cx, JSObject* obj)
{
    CloseIterator(obj);
}

typedef bool (*SetObjectElementFn)(JSContext*, HandleObject, HandleValue,
                                   HandleValue, HandleValue, bool);
const VMFunction SetObjectElementInfo =
    FunctionInfo<SetObjectElementFn>(js::SetObjectElement, "SetObjectElement");

} // namespace jit
} // namespace js
