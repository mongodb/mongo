/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineIC.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/TemplateLib.h"

#include "jsfriendapi.h"
#include "jslibmath.h"
#include "jstypes.h"

#include "builtin/Eval.h"
#include "builtin/SIMD.h"
#include "gc/Policy.h"
#include "jit/BaselineCacheIRCompiler.h"
#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineJIT.h"
#include "jit/InlinableNatives.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/Lowering.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/SharedICHelpers.h"
#include "jit/VMFunctions.h"
#include "js/Conversions.h"
#include "js/GCVector.h"
#include "vm/JSFunction.h"
#include "vm/Opcodes.h"
#include "vm/SelfHosting.h"
#include "vm/TypedArrayObject.h"

#include "jsboolinlines.h"

#include "jit/JitFrames-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "jit/SharedICHelpers-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/UnboxedObject-inl.h"

using mozilla::DebugOnly;

namespace js {
namespace jit {

//
// WarmUpCounter_Fallback
//


//
// The following data is kept in a temporary heap-allocated buffer, stored in
// JitRuntime (high memory addresses at top, low at bottom):
//
//     +----->+=================================+  --      <---- High Address
//     |      |                                 |   |
//     |      |     ...BaselineFrame...         |   |-- Copy of BaselineFrame + stack values
//     |      |                                 |   |
//     |      +---------------------------------+   |
//     |      |                                 |   |
//     |      |     ...Locals/Stack...          |   |
//     |      |                                 |   |
//     |      +=================================+  --
//     |      |     Padding(Maybe Empty)        |
//     |      +=================================+  --
//     +------|-- baselineFrame                 |   |-- IonOsrTempData
//            |   jitcode                       |   |
//            +=================================+  --      <---- Low Address
//
// A pointer to the IonOsrTempData is returned.

struct IonOsrTempData
{
    void* jitcode;
    uint8_t* baselineFrame;
};

static IonOsrTempData*
PrepareOsrTempData(JSContext* cx, BaselineFrame* frame, void* jitcode)
{
    size_t numLocalsAndStackVals = frame->numValueSlots();

    // Calculate the amount of space to allocate:
    //      BaselineFrame space:
    //          (sizeof(Value) * (numLocals + numStackVals))
    //        + sizeof(BaselineFrame)
    //
    //      IonOsrTempData space:
    //          sizeof(IonOsrTempData)

    size_t frameSpace = sizeof(BaselineFrame) + sizeof(Value) * numLocalsAndStackVals;
    size_t ionOsrTempDataSpace = sizeof(IonOsrTempData);

    size_t totalSpace = AlignBytes(frameSpace, sizeof(Value)) +
                        AlignBytes(ionOsrTempDataSpace, sizeof(Value));

    IonOsrTempData* info = (IonOsrTempData*)cx->allocateOsrTempData(totalSpace);
    if (!info) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    memset(info, 0, totalSpace);

    info->jitcode = jitcode;

    // Copy the BaselineFrame + local/stack Values to the buffer. Arguments and
    // |this| are not copied but left on the stack: the Baseline and Ion frame
    // share the same frame prefix and Ion won't clobber these values. Note
    // that info->baselineFrame will point to the *end* of the frame data, like
    // the frame pointer register in baseline frames.
    uint8_t* frameStart = (uint8_t*)info + AlignBytes(ionOsrTempDataSpace, sizeof(Value));
    info->baselineFrame = frameStart + frameSpace;

    memcpy(frameStart, (uint8_t*)frame - numLocalsAndStackVals * sizeof(Value), frameSpace);

    JitSpew(JitSpew_BaselineOSR, "Allocated IonOsrTempData at %p", (void*) info);
    JitSpew(JitSpew_BaselineOSR, "Jitcode is %p", info->jitcode);

    // All done.
    return info;
}

static bool
DoWarmUpCounterFallbackOSR(JSContext* cx, BaselineFrame* frame, ICWarmUpCounter_Fallback* stub,
                           IonOsrTempData** infoPtr)
{
    MOZ_ASSERT(infoPtr);
    *infoPtr = nullptr;

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    MOZ_ASSERT(JSOp(*pc) == JSOP_LOOPENTRY);

    FallbackICSpew(cx, stub, "WarmUpCounter(%d)", int(script->pcToOffset(pc)));

    if (!IonCompileScriptForBaseline(cx, frame, pc))
        return false;

    if (!script->hasIonScript() || script->ionScript()->osrPc() != pc ||
        script->ionScript()->bailoutExpected() ||
        frame->isDebuggee())
    {
        return true;
    }

    IonScript* ion = script->ionScript();
    MOZ_ASSERT(cx->runtime()->geckoProfiler().enabled() == ion->hasProfilingInstrumentation());
    MOZ_ASSERT(ion->osrPc() == pc);

    JitSpew(JitSpew_BaselineOSR, "  OSR possible!");
    void* jitcode = ion->method()->raw() + ion->osrEntryOffset();

    // Prepare the temporary heap copy of the fake InterpreterFrame and actual args list.
    JitSpew(JitSpew_BaselineOSR, "Got jitcode.  Preparing for OSR into ion.");
    IonOsrTempData* info = PrepareOsrTempData(cx, frame, jitcode);
    if (!info)
        return false;
    *infoPtr = info;

    return true;
}

typedef bool (*DoWarmUpCounterFallbackOSRFn)(JSContext*, BaselineFrame*,
                                             ICWarmUpCounter_Fallback*, IonOsrTempData** infoPtr);
static const VMFunction DoWarmUpCounterFallbackOSRInfo =
    FunctionInfo<DoWarmUpCounterFallbackOSRFn>(DoWarmUpCounterFallbackOSR,
                                               "DoWarmUpCounterFallbackOSR");

bool
ICWarmUpCounter_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, R1.scratchReg());

    Label noCompiledCode;
    // Call DoWarmUpCounterFallbackOSR to compile/check-for Ion-compiled function
    {
        // Push IonOsrTempData pointer storage
        masm.subFromStackPtr(Imm32(sizeof(void*)));
        masm.push(masm.getStackPointer());

        // Push stub pointer.
        masm.push(ICStubReg);

        pushStubPayload(masm, R0.scratchReg());

        if (!callVM(DoWarmUpCounterFallbackOSRInfo, masm))
            return false;

        // Pop IonOsrTempData pointer.
        masm.pop(R0.scratchReg());

        leaveStubFrame(masm);

        // If no JitCode was found, then skip just exit the IC.
        masm.branchPtr(Assembler::Equal, R0.scratchReg(), ImmPtr(nullptr), &noCompiledCode);
    }

    // Get a scratch register.
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
    Register osrDataReg = R0.scratchReg();
    regs.take(osrDataReg);
    regs.takeUnchecked(OsrFrameReg);

    Register scratchReg = regs.takeAny();

    // At this point, stack looks like:
    //  +-> [...Calling-Frame...]
    //  |   [...Actual-Args/ThisV/ArgCount/Callee...]
    //  |   [Descriptor]
    //  |   [Return-Addr]
    //  +---[Saved-FramePtr]            <-- BaselineFrameReg points here.
    //      [...Baseline-Frame...]

    // Restore the stack pointer to point to the saved frame pointer.
    masm.moveToStackPtr(BaselineFrameReg);

    // Discard saved frame pointer, so that the return address is on top of
    // the stack.
    masm.pop(scratchReg);

#ifdef DEBUG
    // If profiler instrumentation is on, ensure that lastProfilingFrame is
    // the frame currently being OSR-ed
    {
        Label checkOk;
        AbsoluteAddress addressOfEnabled(cx->runtime()->geckoProfiler().addressOfEnabled());
        masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &checkOk);
        masm.loadPtr(AbsoluteAddress((void*)&cx->jitActivation), scratchReg);
        masm.loadPtr(Address(scratchReg, JitActivation::offsetOfLastProfilingFrame()), scratchReg);

        // It may be the case that we entered the baseline frame with
        // profiling turned off on, then in a call within a loop (i.e. a
        // callee frame), turn on profiling, then return to this frame,
        // and then OSR with profiling turned on.  In this case, allow for
        // lastProfilingFrame to be null.
        masm.branchPtr(Assembler::Equal, scratchReg, ImmWord(0), &checkOk);

        masm.branchStackPtr(Assembler::Equal, scratchReg, &checkOk);
        masm.assumeUnreachable("Baseline OSR lastProfilingFrame mismatch.");
        masm.bind(&checkOk);
    }
#endif

    // Jump into Ion.
    masm.loadPtr(Address(osrDataReg, offsetof(IonOsrTempData, jitcode)), scratchReg);
    masm.loadPtr(Address(osrDataReg, offsetof(IonOsrTempData, baselineFrame)), OsrFrameReg);
    masm.jump(scratchReg);

    // No jitcode available, do nothing.
    masm.bind(&noCompiledCode);
    EmitReturnFromIC(masm);
    return true;
}

//
// TypeUpdate_Fallback
//
static bool
DoTypeUpdateFallback(JSContext* cx, BaselineFrame* frame, ICUpdatedStub* stub, HandleValue objval,
                     HandleValue value)
{
    // This can get called from optimized stubs. Therefore it is not allowed to gc.
    JS::AutoCheckCannotGC nogc;

    FallbackICSpew(cx, stub->getChainFallback(), "TypeUpdate(%s)",
                   ICStub::KindString(stub->kind()));

    MOZ_ASSERT(stub->isCacheIR_Updated());

    RootedScript script(cx, frame->script());
    RootedObject obj(cx, &objval.toObject());

    RootedId id(cx, stub->toCacheIR_Updated()->updateStubId());
    MOZ_ASSERT(id != JSID_EMPTY);

    // The group should match the object's group, except when the object is
    // an unboxed expando object: in that case, the group is the group of
    // the unboxed object.
    RootedObjectGroup group(cx, stub->toCacheIR_Updated()->updateStubGroup());
#ifdef DEBUG
    if (obj->is<UnboxedExpandoObject>())
        MOZ_ASSERT(group->clasp() == &UnboxedPlainObject::class_);
    else
        MOZ_ASSERT(obj->group() == group);
#endif

    // If we're storing null/undefined to a typed object property, check if
    // we want to include it in this property's type information.
    bool addType = true;
    if (MOZ_UNLIKELY(obj->is<TypedObject>()) && value.isNullOrUndefined()) {
        StructTypeDescr* structDescr = &obj->as<TypedObject>().typeDescr().as<StructTypeDescr>();
        size_t fieldIndex;
        MOZ_ALWAYS_TRUE(structDescr->fieldIndex(id, &fieldIndex));

        TypeDescr* fieldDescr = &structDescr->fieldDescr(fieldIndex);
        ReferenceTypeDescr::Type type = fieldDescr->as<ReferenceTypeDescr>().type();
        if (type == ReferenceTypeDescr::TYPE_ANY) {
            // Ignore undefined values, which are included implicitly in type
            // information for this property.
            if (value.isUndefined())
                addType = false;
        } else {
            MOZ_ASSERT(type == ReferenceTypeDescr::TYPE_OBJECT);

            // Ignore null values being written here. Null is included
            // implicitly in type information for this property. Note that
            // non-object, non-null values are not possible here, these
            // should have been filtered out by the IR emitter.
            if (value.isNull())
                addType = false;
        }
    }

    if (MOZ_LIKELY(addType)) {
        JSObject* maybeSingleton = obj->isSingleton() ? obj.get() : nullptr;
        AddTypePropertyId(cx, group, maybeSingleton, id, value);
    }

    if (MOZ_UNLIKELY(!stub->addUpdateStubForValue(cx, script, obj, group, id, value))) {
        // The calling JIT code assumes this function is infallible (for
        // instance we may reallocate dynamic slots before calling this),
        // so ignore OOMs if we failed to attach a stub.
        cx->recoverFromOutOfMemory();
    }

    return true;
}

typedef bool (*DoTypeUpdateFallbackFn)(JSContext*, BaselineFrame*, ICUpdatedStub*, HandleValue,
                                       HandleValue);
const VMFunction DoTypeUpdateFallbackInfo =
    FunctionInfo<DoTypeUpdateFallbackFn>(DoTypeUpdateFallback, "DoTypeUpdateFallback", NonTailCall);

bool
ICTypeUpdate_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // Just store false into R1.scratchReg() and return.
    masm.move32(Imm32(0), R1.scratchReg());
    EmitReturnFromIC(masm);
    return true;
}

bool
ICTypeUpdate_PrimitiveSet::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label success;
    if ((flags_ & TypeToFlag(JSVAL_TYPE_INT32)) && !(flags_ & TypeToFlag(JSVAL_TYPE_DOUBLE)))
        masm.branchTestInt32(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_DOUBLE))
        masm.branchTestNumber(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_UNDEFINED))
        masm.branchTestUndefined(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_BOOLEAN))
        masm.branchTestBoolean(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_STRING))
        masm.branchTestString(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_SYMBOL))
        masm.branchTestSymbol(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_OBJECT))
        masm.branchTestObject(Assembler::Equal, R0, &success);

    if (flags_ & TypeToFlag(JSVAL_TYPE_NULL))
        masm.branchTestNull(Assembler::Equal, R0, &success);

    EmitStubGuardFailure(masm);

    // Type matches, load true into R1.scratchReg() and return.
    masm.bind(&success);
    masm.mov(ImmWord(1), R1.scratchReg());
    EmitReturnFromIC(masm);

    return true;
}

bool
ICTypeUpdate_SingleObject::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Guard on the object's identity.
    Register obj = masm.extractObject(R0, R1.scratchReg());
    Address expectedObject(ICStubReg, ICTypeUpdate_SingleObject::offsetOfObject());
    masm.branchPtr(Assembler::NotEqual, expectedObject, obj, &failure);

    // Identity matches, load true into R1.scratchReg() and return.
    masm.mov(ImmWord(1), R1.scratchReg());
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICTypeUpdate_ObjectGroup::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Guard on the object's ObjectGroup.
    Address expectedGroup(ICStubReg, ICTypeUpdate_ObjectGroup::offsetOfGroup());
    Register scratch1 = R1.scratchReg();
    masm.unboxObject(R0, scratch1);
    masm.branchTestObjGroup(Assembler::NotEqual, scratch1, expectedGroup, scratch1,
                            R0.payloadOrValueReg(), &failure);

    // Group matches, load true into R1.scratchReg() and return.
    masm.mov(ImmWord(1), R1.scratchReg());
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICTypeUpdate_AnyValue::Compiler::generateStubCode(MacroAssembler& masm)
{
    // AnyValue always matches so return true.
    masm.mov(ImmWord(1), R1.scratchReg());
    EmitReturnFromIC(masm);
    return true;
}

//
// ToBool_Fallback
//

static bool
DoToBoolFallback(JSContext* cx, BaselineFrame* frame, ICToBool_Fallback* stub, HandleValue arg,
                 MutableHandleValue ret)
{
    FallbackICSpew(cx, stub, "ToBool");

    MOZ_ASSERT(!arg.isBoolean());

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {

        RootedScript script(cx, frame->script());
        jsbytecode* pc = stub->icEntry()->pc(script);

        ICStubEngine engine = ICStubEngine::Baseline;
        ToBoolIRGenerator gen(cx, script, pc, stub->state().mode(),
                              arg);
        bool attached = false;
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        engine, script, stub, &attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached ToBool CacheIR stub, attached is now %d", attached);
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    bool cond = ToBoolean(arg);
    ret.setBoolean(cond);

    return true;
}

typedef bool (*pf)(JSContext*, BaselineFrame*, ICToBool_Fallback*, HandleValue,
                   MutableHandleValue);
static const VMFunction fun = FunctionInfo<pf>(DoToBoolFallback, "DoToBoolFallback", TailCall);

bool
ICToBool_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(fun, masm);
}


//
// ToNumber_Fallback
//

static bool
DoToNumberFallback(JSContext* cx, ICToNumber_Fallback* stub, HandleValue arg, MutableHandleValue ret)
{
    FallbackICSpew(cx, stub, "ToNumber");
    ret.set(arg);
    return ToNumber(cx, ret);
}

typedef bool (*DoToNumberFallbackFn)(JSContext*, ICToNumber_Fallback*, HandleValue, MutableHandleValue);
static const VMFunction DoToNumberFallbackInfo =
    FunctionInfo<DoToNumberFallbackFn>(DoToNumberFallback, "DoToNumberFallback", TailCall,
                                       PopValues(1));

bool
ICToNumber_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);

    // Push arguments.
    masm.pushValue(R0);
    masm.push(ICStubReg);

    return tailCallVM(DoToNumberFallbackInfo, masm);
}

//
// GetElem_Fallback
//

static bool
DoGetElemFallback(JSContext* cx, BaselineFrame* frame, ICGetElem_Fallback* stub_, HandleValue lhs,
                  HandleValue rhs, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetElem_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);

    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetElem(%s)", CodeName[op]);

    MOZ_ASSERT(op == JSOP_GETELEM || op == JSOP_CALLELEM);

    // Don't pass lhs directly, we need it when generating stubs.
    RootedValue lhsCopy(cx, lhs);

    bool isOptimizedArgs = false;
    if (lhs.isMagic(JS_OPTIMIZED_ARGUMENTS)) {
        // Handle optimized arguments[i] access.
        if (!GetElemOptimizedArguments(cx, frame, &lhsCopy, rhs, res, &isOptimizedArgs))
            return false;
        if (isOptimizedArgs)
            TypeScript::Monitor(cx, script, pc, types, res);
    }

    bool attached = false;
    bool isTemporarilyUnoptimizable = false;

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        ICStubEngine engine = ICStubEngine::Baseline;
        GetPropIRGenerator gen(cx, script, pc,
                               CacheKind::GetElem, stub->state().mode(),
                               &isTemporarilyUnoptimizable, lhs, rhs, lhs,
                               GetPropertyResultFlags::All);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Monitored,
                                                        engine, script, stub, &attached);
            if (newStub) {
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
                if (gen.shouldNotePreliminaryObjectStub())
                    newStub->toCacheIR_Monitored()->notePreliminaryObject();
                else if (gen.shouldUnlinkPreliminaryObjectStubs())
                    StripPreliminaryObjectStubs(cx, stub);
            }
        }
        if (!attached && !isTemporarilyUnoptimizable)
            stub->state().trackNotAttached();
    }

    if (!isOptimizedArgs) {
        if (!GetElementOperation(cx, op, lhsCopy, rhs, res))
            return false;
        TypeScript::Monitor(cx, script, pc, types, res);
    }

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame, types, res))
        return false;

    if (attached)
        return true;

    // GetElem operations which could access negative indexes generally can't
    // be optimized without the potential for bailouts, as we can't statically
    // determine that an object has no properties on such indexes.
    if (rhs.isNumber() && rhs.toNumber() < 0)
        stub->noteNegativeIndex();

    if (!attached && !isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

static bool
DoGetElemSuperFallback(JSContext* cx, BaselineFrame* frame, ICGetElem_Fallback* stub_,
                       HandleValue lhs, HandleValue receiver, HandleValue rhs,
                       MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetElem_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);

    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetElemSuper(%s)", CodeName[op]);

    MOZ_ASSERT(op == JSOP_GETELEM_SUPER);

    bool attached = false;
    bool isTemporarilyUnoptimizable = false;

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        ICStubEngine engine = ICStubEngine::Baseline;
        GetPropIRGenerator gen(cx, script, pc, CacheKind::GetElemSuper, stub->state().mode(),
                               &isTemporarilyUnoptimizable, lhs, rhs, receiver,
                               GetPropertyResultFlags::All);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Monitored,
                                                        engine, script, stub, &attached);
            if (newStub) {
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
                if (gen.shouldNotePreliminaryObjectStub())
                    newStub->toCacheIR_Monitored()->notePreliminaryObject();
                else if (gen.shouldUnlinkPreliminaryObjectStubs())
                    StripPreliminaryObjectStubs(cx, stub);
            }
        }
        if (!attached && !isTemporarilyUnoptimizable)
            stub->state().trackNotAttached();
    }

    // |lhs| is [[HomeObject]].[[Prototype]] which must be Object
    RootedObject lhsObj(cx, &lhs.toObject());
    if (!GetObjectElementOperation(cx, op, lhsObj, receiver, rhs, res))
        return false;
    TypeScript::Monitor(cx, script, pc, types, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame, types, res))
        return false;

    if (attached)
        return true;

    // GetElem operations which could access negative indexes generally can't
    // be optimized without the potential for bailouts, as we can't statically
    // determine that an object has no properties on such indexes.
    if (rhs.isNumber() && rhs.toNumber() < 0)
        stub->noteNegativeIndex();

    if (!attached && !isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoGetElemFallbackFn)(JSContext*, BaselineFrame*, ICGetElem_Fallback*,
                                    HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoGetElemFallbackInfo =
    FunctionInfo<DoGetElemFallbackFn>(DoGetElemFallback, "DoGetElemFallback", TailCall,
                                      PopValues(2));

typedef bool (*DoGetElemSuperFallbackFn)(JSContext*, BaselineFrame*, ICGetElem_Fallback*,
                                         HandleValue, HandleValue, HandleValue,
                                         MutableHandleValue);
static const VMFunction DoGetElemSuperFallbackInfo =
    FunctionInfo<DoGetElemSuperFallbackFn>(DoGetElemSuperFallback, "DoGetElemSuperFallback",
                                           TailCall, PopValues(3));

bool
ICGetElem_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Super property getters use a |this| that differs from base object
    if (hasReceiver_) {
        // State: index in R0, receiver in R1, obj on the stack

        // Ensure stack is fully synced for the expression decompiler.
        // We need: index, receiver, obj
        masm.pushValue(R0);
        masm.pushValue(R1);
        masm.pushValue(Address(masm.getStackPointer(), sizeof(Value) * 2));

        // Push arguments.
        masm.pushValue(R0); // Index
        masm.pushValue(R1); // Reciver
        masm.pushValue(Address(masm.getStackPointer(), sizeof(Value) * 5)); // Obj
        masm.push(ICStubReg);
        pushStubPayload(masm, R0.scratchReg());

        return tailCallVM(DoGetElemSuperFallbackInfo, masm);
    }

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoGetElemFallbackInfo, masm);
}

void
LoadTypedThingLength(MacroAssembler& masm, TypedThingLayout layout, Register obj, Register result)
{
    switch (layout) {
      case Layout_TypedArray:
        masm.unboxInt32(Address(obj, TypedArrayObject::lengthOffset()), result);
        break;
      case Layout_OutlineTypedObject:
      case Layout_InlineTypedObject:
        masm.loadTypedObjectLength(obj, result);
        break;
      default:
        MOZ_CRASH();
    }
}

static void
SetUpdateStubData(ICCacheIR_Updated* stub, const PropertyTypeCheckInfo* info)
{
    if (info->isSet()) {
        stub->updateStubGroup() = info->group();
        stub->updateStubId() = info->id();
    }
}

static bool
DoSetElemFallback(JSContext* cx, BaselineFrame* frame, ICSetElem_Fallback* stub_, Value* stack,
                  HandleValue objv, HandleValue index, HandleValue rhs)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICSetElem_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    RootedScript outerScript(cx, script);
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "SetElem(%s)", CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_SETELEM ||
               op == JSOP_STRICTSETELEM ||
               op == JSOP_INITELEM ||
               op == JSOP_INITHIDDENELEM ||
               op == JSOP_INITELEM_ARRAY ||
               op == JSOP_INITELEM_INC);

    RootedObject obj(cx, ToObjectFromStack(cx, objv));
    if (!obj)
        return false;

    RootedShape oldShape(cx, obj->maybeShape());
    RootedObjectGroup oldGroup(cx, JSObject::getGroup(cx, obj));
    if (!oldGroup)
        return false;

    if (obj->is<UnboxedPlainObject>()) {
        MOZ_ASSERT(!oldShape);
        if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando())
            oldShape = expando->lastProperty();
    }

    bool isTemporarilyUnoptimizable = false;
    bool attached = false;

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        SetPropIRGenerator gen(cx, script, pc, CacheKind::SetElem, stub->state().mode(),
                               &isTemporarilyUnoptimizable, objv, index, rhs);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Updated,
                                                        ICStubEngine::Baseline, frame->script(),
                                                        stub, &attached);
            if (newStub) {
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");

                SetUpdateStubData(newStub->toCacheIR_Updated(), gen.typeCheckInfo());

                if (gen.shouldNotePreliminaryObjectStub())
                    newStub->toCacheIR_Updated()->notePreliminaryObject();
                else if (gen.shouldUnlinkPreliminaryObjectStubs())
                    StripPreliminaryObjectStubs(cx, stub);

                if (gen.attachedTypedArrayOOBStub())
                    stub->noteHasTypedArrayOOB();
            }
        }
    }

    if (op == JSOP_INITELEM || op == JSOP_INITHIDDENELEM) {
        if (!InitElemOperation(cx, pc, obj, index, rhs))
            return false;
    } else if (op == JSOP_INITELEM_ARRAY) {
        MOZ_ASSERT(uint32_t(index.toInt32()) <= INT32_MAX,
                   "the bytecode emitter must fail to compile code that would "
                   "produce JSOP_INITELEM_ARRAY with an index exceeding "
                   "int32_t range");
        MOZ_ASSERT(uint32_t(index.toInt32()) == GET_UINT32(pc));
        if (!InitArrayElemOperation(cx, pc, obj, index.toInt32(), rhs))
            return false;
    } else if (op == JSOP_INITELEM_INC) {
        if (!InitArrayElemOperation(cx, pc, obj, index.toInt32(), rhs))
            return false;
    } else {
        if (!SetObjectElement(cx, obj, index, rhs, objv, JSOp(*pc) == JSOP_STRICTSETELEM, script, pc))
            return false;
    }

    // Don't try to attach stubs that wish to be hidden. We don't know how to
    // have different enumerability in the stubs for the moment.
    if (op == JSOP_INITHIDDENELEM)
        return true;

    // Overwrite the object on the stack (pushed for the decompiler) with the rhs.
    MOZ_ASSERT(stack[2] == objv);
    stack[2] = rhs;

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (attached)
        return true;

    // The SetObjectElement call might have entered this IC recursively, so try
    // to transition.
    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        SetPropIRGenerator gen(cx, script, pc, CacheKind::SetElem, stub->state().mode(),
                               &isTemporarilyUnoptimizable, objv, index, rhs);
        if (gen.tryAttachAddSlotStub(oldGroup, oldShape)) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Updated,
                                                        ICStubEngine::Baseline, frame->script(),
                                                        stub, &attached);
            if (newStub) {
                if (gen.shouldNotePreliminaryObjectStub())
                    newStub->toCacheIR_Updated()->notePreliminaryObject();
                else if (gen.shouldUnlinkPreliminaryObjectStubs())
                    StripPreliminaryObjectStubs(cx, stub);

                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
                SetUpdateStubData(newStub->toCacheIR_Updated(), gen.typeCheckInfo());
                return true;
            }
        } else {
            gen.trackAttached(IRGenerator::NotAttached);
        }
        if (!attached && !isTemporarilyUnoptimizable)
            stub->state().trackNotAttached();
    }

    return true;
}

typedef bool (*DoSetElemFallbackFn)(JSContext*, BaselineFrame*, ICSetElem_Fallback*, Value*,
                                    HandleValue, HandleValue, HandleValue);
static const VMFunction DoSetElemFallbackInfo =
    FunctionInfo<DoSetElemFallbackFn>(DoSetElemFallback, "DoSetElemFallback", TailCall,
                                      PopValues(2));

bool
ICSetElem_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    // State: R0: object, R1: index, stack: rhs.
    // For the decompiler, the stack has to be: object, index, rhs,
    // so we push the index, then overwrite the rhs Value with R0
    // and push the rhs value.
    masm.pushValue(R1);
    masm.loadValue(Address(masm.getStackPointer(), sizeof(Value)), R1);
    masm.storeValue(R0, Address(masm.getStackPointer(), sizeof(Value)));
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1); // RHS

    // Push index. On x86 and ARM two push instructions are emitted so use a
    // separate register to store the old stack pointer.
    masm.moveStackPtrTo(R1.scratchReg());
    masm.pushValue(Address(R1.scratchReg(), 2 * sizeof(Value)));
    masm.pushValue(R0); // Object.

    // Push pointer to stack values, so that the stub can overwrite the object
    // (pushed for the decompiler) with the rhs.
    masm.computeEffectiveAddress(Address(masm.getStackPointer(), 3 * sizeof(Value)), R0.scratchReg());
    masm.push(R0.scratchReg());

    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoSetElemFallbackInfo, masm);
}

void
BaselineScript::noteHasDenseAdd(uint32_t pcOffset)
{
    ICEntry& entry = icEntryFromPCOffset(pcOffset);
    ICFallbackStub* stub = entry.fallbackStub();

    if (stub->isSetElem_Fallback())
        stub->toSetElem_Fallback()->noteHasDenseAdd();
}

template <typename T>
void
EmitICUnboxedPreBarrier(MacroAssembler& masm, const T& address, JSValueType type)
{
    if (type == JSVAL_TYPE_OBJECT)
        EmitPreBarrier(masm, address, MIRType::Object);
    else if (type == JSVAL_TYPE_STRING)
        EmitPreBarrier(masm, address, MIRType::String);
    else
        MOZ_ASSERT(!UnboxedTypeNeedsPreBarrier(type));
}

template void
EmitICUnboxedPreBarrier(MacroAssembler& masm, const Address& address, JSValueType type);

template void
EmitICUnboxedPreBarrier(MacroAssembler& masm, const BaseIndex& address, JSValueType type);

template <typename T>
void
StoreToTypedArray(JSContext* cx, MacroAssembler& masm, Scalar::Type type,
                  const ValueOperand& value, const T& dest, Register scratch,
                  Label* failure)
{
    Label done;

    if (type == Scalar::Float32 || type == Scalar::Float64) {
        masm.ensureDouble(value, FloatReg0, failure);
        if (type == Scalar::Float32) {
            masm.convertDoubleToFloat32(FloatReg0, ScratchFloat32Reg);
            masm.storeToTypedFloatArray(type, ScratchFloat32Reg, dest);
        } else {
            masm.storeToTypedFloatArray(type, FloatReg0, dest);
        }
    } else if (type == Scalar::Uint8Clamped) {
        Label notInt32;
        masm.branchTestInt32(Assembler::NotEqual, value, &notInt32);
        masm.unboxInt32(value, scratch);
        masm.clampIntToUint8(scratch);

        Label clamped;
        masm.bind(&clamped);
        masm.storeToTypedIntArray(type, scratch, dest);
        masm.jump(&done);

        // If the value is a double, clamp to uint8 and jump back.
        // Else, jump to failure.
        masm.bind(&notInt32);
        if (cx->runtime()->jitSupportsFloatingPoint) {
            masm.branchTestDouble(Assembler::NotEqual, value, failure);
            masm.unboxDouble(value, FloatReg0);
            masm.clampDoubleToUint8(FloatReg0, scratch);
            masm.jump(&clamped);
        } else {
            masm.jump(failure);
        }
    } else {
        Label notInt32;
        masm.branchTestInt32(Assembler::NotEqual, value, &notInt32);
        masm.unboxInt32(value, scratch);

        Label isInt32;
        masm.bind(&isInt32);
        masm.storeToTypedIntArray(type, scratch, dest);
        masm.jump(&done);

        // If the value is a double, truncate and jump back.
        // Else, jump to failure.
        masm.bind(&notInt32);
        if (cx->runtime()->jitSupportsFloatingPoint) {
            masm.branchTestDouble(Assembler::NotEqual, value, failure);
            masm.unboxDouble(value, FloatReg0);
            masm.branchTruncateDoubleMaybeModUint32(FloatReg0, scratch, failure);
            masm.jump(&isInt32);
        } else {
            masm.jump(failure);
        }
    }

    masm.bind(&done);
}

template void
StoreToTypedArray(JSContext* cx, MacroAssembler& masm, Scalar::Type type,
                  const ValueOperand& value, const Address& dest, Register scratch,
                  Label* failure);

template void
StoreToTypedArray(JSContext* cx, MacroAssembler& masm, Scalar::Type type,
                  const ValueOperand& value, const BaseIndex& dest, Register scratch,
                  Label* failure);

//
// In_Fallback
//

static bool
DoInFallback(JSContext* cx, BaselineFrame* frame, ICIn_Fallback* stub_,
             HandleValue key, HandleValue objValue, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICIn_Fallback*> stub(frame, stub_);

    FallbackICSpew(cx, stub, "In");

    if (!objValue.isObject()) {
        ReportInNotObjectError(cx, key, -2, objValue, -1);
        return false;
    }

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        RootedScript script(cx, frame->script());
        jsbytecode* pc = stub->icEntry()->pc(script);

        ICStubEngine engine = ICStubEngine::Baseline;
        HasPropIRGenerator gen(cx, script, pc, CacheKind::In, stub->state().mode(), key, objValue);
        bool attached = false;
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        engine, script, stub, &attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    RootedObject obj(cx, &objValue.toObject());
    bool cond = false;
    if (!OperatorIn(cx, key, obj, &cond))
        return false;
    res.setBoolean(cond);

    return true;
}

typedef bool (*DoInFallbackFn)(JSContext*, BaselineFrame*, ICIn_Fallback*, HandleValue,
                               HandleValue, MutableHandleValue);
static const VMFunction DoInFallbackInfo =
    FunctionInfo<DoInFallbackFn>(DoInFallback, "DoInFallback", TailCall, PopValues(2));

bool
ICIn_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    // Sync for the decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoInFallbackInfo, masm);
}

//
// HasOwn_Fallback
//

static bool
DoHasOwnFallback(JSContext* cx, BaselineFrame* frame, ICHasOwn_Fallback* stub_,
                 HandleValue keyValue, HandleValue objValue, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICIn_Fallback*> stub(frame, stub_);

    FallbackICSpew(cx, stub, "HasOwn");

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        RootedScript script(cx, frame->script());
        jsbytecode* pc = stub->icEntry()->pc(script);

        ICStubEngine engine = ICStubEngine::Baseline;
        HasPropIRGenerator gen(cx, script, pc, CacheKind::HasOwn,
                               stub->state().mode(), keyValue, objValue);
        bool attached = false;
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        engine, script, stub, &attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    bool found;
    if (!HasOwnProperty(cx, objValue, keyValue, &found))
        return false;

    res.setBoolean(found);
    return true;
}

typedef bool (*DoHasOwnFallbackFn)(JSContext*, BaselineFrame*, ICHasOwn_Fallback*, HandleValue,
                               HandleValue, MutableHandleValue);
static const VMFunction DoHasOwnFallbackInfo =
    FunctionInfo<DoHasOwnFallbackFn>(DoHasOwnFallback, "DoHasOwnFallback", TailCall, PopValues(2));

bool
ICHasOwn_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    // Sync for the decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoHasOwnFallbackInfo, masm);
}


//
// GetName_Fallback
//

static bool
DoGetNameFallback(JSContext* cx, BaselineFrame* frame, ICGetName_Fallback* stub_,
                  HandleObject envChain, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetName_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    mozilla::DebugOnly<JSOp> op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetName(%s)", CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_GETNAME || op == JSOP_GETGNAME);

    RootedPropertyName name(cx, script->getName(pc));
    bool attached = false;

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        ICStubEngine engine = ICStubEngine::Baseline;
        GetNameIRGenerator gen(cx, script, pc, stub->state().mode(), envChain, name);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Monitored,
                                                        engine, script, stub, &attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    static_assert(JSOP_GETGNAME_LENGTH == JSOP_GETNAME_LENGTH,
                  "Otherwise our check for JSOP_TYPEOF isn't ok");
    if (JSOp(pc[JSOP_GETGNAME_LENGTH]) == JSOP_TYPEOF) {
        if (!GetEnvironmentName<GetNameMode::TypeOf>(cx, envChain, name, res))
            return false;
    } else {
        if (!GetEnvironmentName<GetNameMode::Normal>(cx, envChain, name, res))
            return false;
    }

    StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);
    TypeScript::Monitor(cx, script, pc, types, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame, types, res))
        return false;

    if (!attached)
        stub->noteUnoptimizableAccess();
    return true;
}

typedef bool (*DoGetNameFallbackFn)(JSContext*, BaselineFrame*, ICGetName_Fallback*,
                                    HandleObject, MutableHandleValue);
static const VMFunction DoGetNameFallbackInfo =
    FunctionInfo<DoGetNameFallbackFn>(DoGetNameFallback, "DoGetNameFallback", TailCall);

bool
ICGetName_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    masm.push(R0.scratchReg());
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoGetNameFallbackInfo, masm);
}

//
// BindName_Fallback
//

static bool
DoBindNameFallback(JSContext* cx, BaselineFrame* frame, ICBindName_Fallback* stub,
                   HandleObject envChain, MutableHandleValue res)
{
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    mozilla::DebugOnly<JSOp> op = JSOp(*pc);
    FallbackICSpew(cx, stub, "BindName(%s)", CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_BINDNAME || op == JSOP_BINDGNAME);

    RootedPropertyName name(cx, frame->script()->getName(pc));

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        bool attached = false;
        RootedScript script(cx, frame->script());
        BindNameIRGenerator gen(cx, script, pc, stub->state().mode(), envChain, name);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        ICStubEngine::Baseline, script, stub,
                                                        &attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    RootedObject scope(cx);
    if (!LookupNameUnqualified(cx, name, envChain, &scope))
        return false;

    res.setObject(*scope);
    return true;
}

typedef bool (*DoBindNameFallbackFn)(JSContext*, BaselineFrame*, ICBindName_Fallback*,
                                     HandleObject, MutableHandleValue);
static const VMFunction DoBindNameFallbackInfo =
    FunctionInfo<DoBindNameFallbackFn>(DoBindNameFallback, "DoBindNameFallback", TailCall);

bool
ICBindName_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    masm.push(R0.scratchReg());
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoBindNameFallbackInfo, masm);
}

//
// GetIntrinsic_Fallback
//

static bool
DoGetIntrinsicFallback(JSContext* cx, BaselineFrame* frame, ICGetIntrinsic_Fallback* stub_,
                       MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetIntrinsic_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    mozilla::DebugOnly<JSOp> op = JSOp(*pc);
    FallbackICSpew(cx, stub, "GetIntrinsic(%s)", CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_GETINTRINSIC);

    if (!GetIntrinsicOperation(cx, pc, res))
        return false;

    // An intrinsic operation will always produce the same result, so only
    // needs to be monitored once. Attach a stub to load the resulting constant
    // directly.

    TypeScript::Monitor(cx, script, pc, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        bool attached = false;
        RootedScript script(cx, frame->script());
        GetIntrinsicIRGenerator gen(cx, script, pc, stub->state().mode(), res);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        ICStubEngine::Baseline, script, stub,
                                                        &attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    return true;
}

typedef bool (*DoGetIntrinsicFallbackFn)(JSContext*, BaselineFrame*, ICGetIntrinsic_Fallback*,
                                         MutableHandleValue);
static const VMFunction DoGetIntrinsicFallbackInfo =
    FunctionInfo<DoGetIntrinsicFallbackFn>(DoGetIntrinsicFallback, "DoGetIntrinsicFallback",
                                           TailCall);

bool
ICGetIntrinsic_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoGetIntrinsicFallbackInfo, masm);
}

//
// SetProp_Fallback
//

static bool
DoSetPropFallback(JSContext* cx, BaselineFrame* frame, ICSetProp_Fallback* stub_, Value* stack,
                  HandleValue lhs, HandleValue rhs)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICSetProp_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "SetProp(%s)", CodeName[op]);

    MOZ_ASSERT(op == JSOP_SETPROP ||
               op == JSOP_STRICTSETPROP ||
               op == JSOP_SETNAME ||
               op == JSOP_STRICTSETNAME ||
               op == JSOP_SETGNAME ||
               op == JSOP_STRICTSETGNAME ||
               op == JSOP_INITPROP ||
               op == JSOP_INITLOCKEDPROP ||
               op == JSOP_INITHIDDENPROP ||
               op == JSOP_SETALIASEDVAR ||
               op == JSOP_INITALIASEDLEXICAL ||
               op == JSOP_INITGLEXICAL);

    RootedPropertyName name(cx);
    if (op == JSOP_SETALIASEDVAR || op == JSOP_INITALIASEDLEXICAL)
        name = EnvironmentCoordinateName(cx->caches().envCoordinateNameCache, script, pc);
    else
        name = script->getName(pc);
    RootedId id(cx, NameToId(name));

    RootedObject obj(cx, ToObjectFromStack(cx, lhs));
    if (!obj)
        return false;
    RootedShape oldShape(cx, obj->maybeShape());
    RootedObjectGroup oldGroup(cx, JSObject::getGroup(cx, obj));
    if (!oldGroup)
        return false;

    if (obj->is<UnboxedPlainObject>()) {
        MOZ_ASSERT(!oldShape);
        if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando())
            oldShape = expando->lastProperty();
    }

    // There are some reasons we can fail to attach a stub that are temporary.
    // We want to avoid calling noteUnoptimizableAccess() if the reason we
    // failed to attach a stub is one of those temporary reasons, since we might
    // end up attaching a stub for the exact same access later.
    bool isTemporarilyUnoptimizable = false;

    bool attached = false;
    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        RootedValue idVal(cx, StringValue(name));
        SetPropIRGenerator gen(cx, script, pc, CacheKind::SetProp, stub->state().mode(),
                               &isTemporarilyUnoptimizable, lhs, idVal, rhs);
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Updated,
                                                        ICStubEngine::Baseline, frame->script(),
                                                        stub, &attached);
            if (newStub) {
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");

                SetUpdateStubData(newStub->toCacheIR_Updated(), gen.typeCheckInfo());

                if (gen.shouldNotePreliminaryObjectStub())
                    newStub->toCacheIR_Updated()->notePreliminaryObject();
                else if (gen.shouldUnlinkPreliminaryObjectStubs())
                    StripPreliminaryObjectStubs(cx, stub);
            }
        }
    }

    if (op == JSOP_INITPROP ||
        op == JSOP_INITLOCKEDPROP ||
        op == JSOP_INITHIDDENPROP)
    {
        if (!InitPropertyOperation(cx, op, obj, name, rhs))
            return false;
    } else if (op == JSOP_SETNAME ||
               op == JSOP_STRICTSETNAME ||
               op == JSOP_SETGNAME ||
               op == JSOP_STRICTSETGNAME)
    {
        if (!SetNameOperation(cx, script, pc, obj, rhs))
            return false;
    } else if (op == JSOP_SETALIASEDVAR || op == JSOP_INITALIASEDLEXICAL) {
        obj->as<EnvironmentObject>().setAliasedBinding(cx, EnvironmentCoordinate(pc), name, rhs);
    } else if (op == JSOP_INITGLEXICAL) {
        RootedValue v(cx, rhs);
        LexicalEnvironmentObject* lexicalEnv;
        if (script->hasNonSyntacticScope())
            lexicalEnv = &NearestEnclosingExtensibleLexicalEnvironment(frame->environmentChain());
        else
            lexicalEnv = &cx->global()->lexicalEnvironment();
        InitGlobalLexicalOperation(cx, lexicalEnv, script, pc, v);
    } else {
        MOZ_ASSERT(op == JSOP_SETPROP || op == JSOP_STRICTSETPROP);

        ObjectOpResult result;
        if (!SetProperty(cx, obj, id, rhs, lhs, result) ||
            !result.checkStrictErrorOrWarning(cx, obj, id, op == JSOP_STRICTSETPROP))
        {
            return false;
        }
    }

    // Overwrite the LHS on the stack (pushed for the decompiler) with the RHS.
    MOZ_ASSERT(stack[1] == lhs);
    stack[1] = rhs;

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (attached)
        return true;

    // The SetProperty call might have entered this IC recursively, so try
    // to transition.
    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        RootedValue idVal(cx, StringValue(name));
        SetPropIRGenerator gen(cx, script, pc, CacheKind::SetProp, stub->state().mode(),
                               &isTemporarilyUnoptimizable, lhs, idVal, rhs);
        if (gen.tryAttachAddSlotStub(oldGroup, oldShape)) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Updated,
                                                        ICStubEngine::Baseline, frame->script(),
                                                        stub, &attached);
            if (newStub) {
                if (gen.shouldNotePreliminaryObjectStub())
                    newStub->toCacheIR_Updated()->notePreliminaryObject();
                else if (gen.shouldUnlinkPreliminaryObjectStubs())
                    StripPreliminaryObjectStubs(cx, stub);

                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
                SetUpdateStubData(newStub->toCacheIR_Updated(), gen.typeCheckInfo());
            }
        } else {
            gen.trackAttached(IRGenerator::NotAttached);
        }
        if (!attached && !isTemporarilyUnoptimizable)
            stub->state().trackNotAttached();
    }

    if (!attached && !isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoSetPropFallbackFn)(JSContext*, BaselineFrame*, ICSetProp_Fallback*, Value*,
                                    HandleValue, HandleValue);
static const VMFunction DoSetPropFallbackInfo =
    FunctionInfo<DoSetPropFallbackFn>(DoSetPropFallback, "DoSetPropFallback", TailCall,
                                      PopValues(1));

bool
ICSetProp_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    // Overwrite the RHS value on top of the stack with the object, then push
    // the RHS in R1 on top of that.
    masm.storeValue(R0, Address(masm.getStackPointer(), 0));
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);

    // Push pointer to stack values, so that the stub can overwrite the object
    // (pushed for the decompiler) with the RHS.
    masm.computeEffectiveAddress(Address(masm.getStackPointer(), 2 * sizeof(Value)),
                                 R0.scratchReg());
    masm.push(R0.scratchReg());

    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    if (!tailCallVM(DoSetPropFallbackInfo, masm))
        return false;

    // This is the resume point used when bailout rewrites call stack to undo
    // Ion inlined frames. The return address pushed onto reconstructed stack
    // will point here.
    assumeStubFrame();
    bailoutReturnOffset_.bind(masm.currentOffset());

    leaveStubFrame(masm, true);
    EmitReturnFromIC(masm);

    return true;
}

void
ICSetProp_Fallback::Compiler::postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code)
{
    BailoutReturnStub kind = BailoutReturnStub::SetProp;
    void* address = code->raw() + bailoutReturnOffset_.offset();
    cx->compartment()->jitCompartment()->initBailoutReturnAddr(address, getKey(), kind);
}

//
// Call_Fallback
//

static bool
TryAttachFunApplyStub(JSContext* cx, ICCall_Fallback* stub, HandleScript script, jsbytecode* pc,
                      HandleValue thisv, uint32_t argc, Value* argv,
                      ICTypeMonitor_Fallback* typeMonitorFallback, bool* attached)
{
    if (argc != 2)
        return true;

    if (!thisv.isObject() || !thisv.toObject().is<JSFunction>())
        return true;
    RootedFunction target(cx, &thisv.toObject().as<JSFunction>());

    // right now, only handle situation where second argument is |arguments|
    if (argv[1].isMagic(JS_OPTIMIZED_ARGUMENTS) && !script->needsArgsObj()) {
        if (target->hasJitEntry() && !stub->hasStub(ICStub::Call_ScriptedApplyArguments)) {
            JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedApplyArguments stub");

            ICCall_ScriptedApplyArguments::Compiler compiler(
                cx, typeMonitorFallback->firstMonitorStub(), script->pcToOffset(pc));
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            stub->addNewStub(newStub);
            *attached = true;
            return true;
        }

        // TODO: handle FUNAPPLY for native targets.
    }

    if (argv[1].isObject() && argv[1].toObject().is<ArrayObject>()) {
        if (target->hasJitEntry() && !stub->hasStub(ICStub::Call_ScriptedApplyArray)) {
            JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedApplyArray stub");

            ICCall_ScriptedApplyArray::Compiler compiler(
                cx, typeMonitorFallback->firstMonitorStub(), script->pcToOffset(pc));
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            stub->addNewStub(newStub);
            *attached = true;
            return true;
        }
    }
    return true;
}

static bool
TryAttachFunCallStub(JSContext* cx, ICCall_Fallback* stub, HandleScript script, jsbytecode* pc,
                     HandleValue thisv, ICTypeMonitor_Fallback* typeMonitorFallback,
                     bool* attached)
{
    // Try to attach a stub for Function.prototype.call with scripted |this|.

    *attached = false;
    if (!thisv.isObject() || !thisv.toObject().is<JSFunction>())
        return true;
    RootedFunction target(cx, &thisv.toObject().as<JSFunction>());

    // Attach a stub if the script can be Baseline-compiled. We do this also
    // if the script is not yet compiled to avoid attaching a CallNative stub
    // that handles everything, even after the callee becomes hot.
    if (((target->hasScript() && target->nonLazyScript()->canBaselineCompile()) ||
        (target->isNativeWithJitEntry())) &&
        !stub->hasStub(ICStub::Call_ScriptedFunCall))
    {
        JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedFunCall stub");

        ICCall_ScriptedFunCall::Compiler compiler(cx, typeMonitorFallback->firstMonitorStub(),
                                                  script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        *attached = true;
        stub->addNewStub(newStub);
        return true;
    }

    return true;
}

// Check if target is a native SIMD operation which returns a SIMD type.
// If so, set res to a template object matching the SIMD type produced and return true.
static bool
GetTemplateObjectForSimd(JSContext* cx, JSFunction* target, MutableHandleObject res)
{
    if (!target->hasJitInfo())
        return false;

    const JSJitInfo* jitInfo = target->jitInfo();
    if (jitInfo->type() != JSJitInfo::InlinableNative)
        return false;

    // Check if this is a native inlinable SIMD operation.
    SimdType ctrlType;
    switch (jitInfo->inlinableNative) {
      case InlinableNative::SimdInt8x16:   ctrlType = SimdType::Int8x16;   break;
      case InlinableNative::SimdUint8x16:  ctrlType = SimdType::Uint8x16;  break;
      case InlinableNative::SimdInt16x8:   ctrlType = SimdType::Int16x8;   break;
      case InlinableNative::SimdUint16x8:  ctrlType = SimdType::Uint16x8;  break;
      case InlinableNative::SimdInt32x4:   ctrlType = SimdType::Int32x4;   break;
      case InlinableNative::SimdUint32x4:  ctrlType = SimdType::Uint32x4;  break;
      case InlinableNative::SimdFloat32x4: ctrlType = SimdType::Float32x4; break;
      case InlinableNative::SimdBool8x16:  ctrlType = SimdType::Bool8x16;  break;
      case InlinableNative::SimdBool16x8:  ctrlType = SimdType::Bool16x8;  break;
      case InlinableNative::SimdBool32x4:  ctrlType = SimdType::Bool32x4;  break;
      // This is not an inlinable SIMD operation.
      default: return false;
    }

    // The controlling type is not necessarily the return type.
    // Check the actual operation.
    SimdOperation simdOp = SimdOperation(jitInfo->nativeOp);
    SimdType retType;

    switch(simdOp) {
      case SimdOperation::Fn_allTrue:
      case SimdOperation::Fn_anyTrue:
      case SimdOperation::Fn_extractLane:
        // These operations return a scalar. No template object needed.
        return false;

      case SimdOperation::Fn_lessThan:
      case SimdOperation::Fn_lessThanOrEqual:
      case SimdOperation::Fn_equal:
      case SimdOperation::Fn_notEqual:
      case SimdOperation::Fn_greaterThan:
      case SimdOperation::Fn_greaterThanOrEqual:
        // These operations return a boolean vector with the same shape as the
        // controlling type.
        retType = GetBooleanSimdType(ctrlType);
        break;

      default:
        // All other operations return the controlling type.
        retType = ctrlType;
        break;
    }

    // Create a template object based on retType.
    RootedGlobalObject global(cx, cx->global());
    Rooted<SimdTypeDescr*> descr(cx, GlobalObject::getOrCreateSimdTypeDescr(cx, global, retType));
    res.set(cx->compartment()->jitCompartment()->getSimdTemplateObjectFor(cx, descr));
    return true;
}

static bool
GetTemplateObjectForNative(JSContext* cx, HandleFunction target, const CallArgs& args,
                           MutableHandleObject res, bool* skipAttach)
{
    Native native = target->native();

    // Check for natives to which template objects can be attached. This is
    // done to provide templates to Ion for inlining these natives later on.

    if (native == ArrayConstructor || native == array_construct) {
        // Note: the template array won't be used if its length is inaccurately
        // computed here.  (We allocate here because compilation may occur on a
        // separate thread where allocation is impossible.)
        size_t count = 0;
        if (args.length() != 1)
            count = args.length();
        else if (args.length() == 1 && args[0].isInt32() && args[0].toInt32() >= 0)
            count = args[0].toInt32();

        if (count <= ArrayObject::EagerAllocationMaxLength) {
            ObjectGroup* group = ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array);
            if (!group)
                return false;
            if (group->maybePreliminaryObjects()) {
                *skipAttach = true;
                return true;
            }

            // With this and other array templates, analyze the group so that
            // we don't end up with a template whose structure might change later.
            res.set(NewFullyAllocatedArrayForCallingAllocationSite(cx, count, TenuredObject));
            return !!res;
        }
    }

    if (args.length() == 1) {
        size_t len = 0;

        if (args[0].isInt32() && args[0].toInt32() >= 0)
            len = args[0].toInt32();

        if (!TypedArrayObject::GetTemplateObjectForNative(cx, native, len, res))
            return false;
        if (res)
            return true;
    }

    if (native == js::array_slice) {
        if (args.thisv().isObject()) {
            RootedObject obj(cx, &args.thisv().toObject());
            if (!obj->isSingleton()) {
                if (obj->group()->maybePreliminaryObjects()) {
                    *skipAttach = true;
                    return true;
                }
                res.set(NewFullyAllocatedArrayTryReuseGroup(cx, obj, 0, TenuredObject));
                return !!res;
            }
        }
    }

    if (native == js::intrinsic_StringSplitString && args.length() == 2 && args[0].isString() &&
        args[1].isString())
    {
        ObjectGroup* group = ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array);
        if (!group)
            return false;
        if (group->maybePreliminaryObjects()) {
            *skipAttach = true;
            return true;
        }

        res.set(NewFullyAllocatedArrayForCallingAllocationSite(cx, 0, TenuredObject));
        return !!res;
    }

    if (native == StringConstructor) {
        RootedString emptyString(cx, cx->runtime()->emptyString);
        res.set(StringObject::create(cx, emptyString, /* proto = */ nullptr, TenuredObject));
        return !!res;
    }

    if (native == obj_create && args.length() == 1 && args[0].isObjectOrNull()) {
        RootedObject proto(cx, args[0].toObjectOrNull());
        res.set(ObjectCreateImpl(cx, proto, TenuredObject));
        return !!res;
    }

    if (native == js::intrinsic_NewArrayIterator) {
        res.set(NewArrayIteratorObject(cx, TenuredObject));
        return !!res;
    }

    if (native == js::intrinsic_NewStringIterator) {
        res.set(NewStringIteratorObject(cx, TenuredObject));
        return !!res;
    }

    if (JitSupportsSimd() && GetTemplateObjectForSimd(cx, target, res))
       return !!res;

    return true;
}

static bool
GetTemplateObjectForClassHook(JSContext* cx, JSNative hook, CallArgs& args,
                              MutableHandleObject templateObject)
{
    if (hook == TypedObject::construct) {
        Rooted<TypeDescr*> descr(cx, &args.callee().as<TypeDescr>());
        templateObject.set(TypedObject::createZeroed(cx, descr, 1, gc::TenuredHeap));
        return !!templateObject;
    }

    if (hook == SimdTypeDescr::call && JitSupportsSimd()) {
        Rooted<SimdTypeDescr*> descr(cx, &args.callee().as<SimdTypeDescr>());
        templateObject.set(cx->compartment()->jitCompartment()->getSimdTemplateObjectFor(cx, descr));
        return !!templateObject;
    }

    return true;
}

static bool
IsOptimizableConstStringSplit(const Value& callee, int argc, Value* args)
{
    if (argc != 2 || !args[0].isString() || !args[1].isString())
        return false;

    if (!args[0].toString()->isAtom() || !args[1].toString()->isAtom())
        return false;

    if (!callee.isObject() || !callee.toObject().is<JSFunction>())
        return false;

    JSFunction& calleeFun = callee.toObject().as<JSFunction>();
    if (!calleeFun.isNative() || calleeFun.native() != js::intrinsic_StringSplitString)
        return false;

    return true;
}

static bool
TryAttachCallStub(JSContext* cx, ICCall_Fallback* stub, HandleScript script, jsbytecode* pc,
                  JSOp op, uint32_t argc, Value* vp, bool constructing, bool isSpread,
                  bool createSingleton, bool* handled)
{
    bool isSuper = op == JSOP_SUPERCALL || op == JSOP_SPREADSUPERCALL;

    if (createSingleton || op == JSOP_EVAL || op == JSOP_STRICTEVAL)
        return true;

    if (stub->numOptimizedStubs() >= ICCall_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    RootedValue callee(cx, vp[0]);
    RootedValue thisv(cx, vp[1]);

    // Don't attach an optimized call stub if we could potentially attach an
    // optimized ConstStringSplit stub.
    if (stub->numOptimizedStubs() == 0 && IsOptimizableConstStringSplit(callee, argc, vp + 2))
        return true;

    stub->unlinkStubsWithKind(cx, ICStub::Call_ConstStringSplit);

    if (!callee.isObject())
        return true;

    ICTypeMonitor_Fallback* typeMonitorFallback = stub->getFallbackMonitorStub(cx, script);
    if (!typeMonitorFallback)
        return false;

    RootedObject obj(cx, &callee.toObject());
    if (!obj->is<JSFunction>()) {
        // Try to attach a stub for a call/construct hook on the object.
        // Ignore proxies, which are special cased by callHook/constructHook.
        if (obj->is<ProxyObject>())
            return true;
        if (JSNative hook = constructing ? obj->constructHook() : obj->callHook()) {
            if (op != JSOP_FUNAPPLY && !isSpread && !createSingleton) {
                RootedObject templateObject(cx);
                CallArgs args = CallArgsFromVp(argc, vp);
                if (!GetTemplateObjectForClassHook(cx, hook, args, &templateObject))
                    return false;

                JitSpew(JitSpew_BaselineIC, "  Generating Call_ClassHook stub");
                ICCall_ClassHook::Compiler compiler(cx, typeMonitorFallback->firstMonitorStub(),
                                                    obj->getClass(), hook, templateObject,
                                                    script->pcToOffset(pc), constructing);
                ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
                if (!newStub)
                    return false;

                stub->addNewStub(newStub);
                *handled = true;
                return true;
            }
        }
        return true;
    }

    RootedFunction fun(cx, &obj->as<JSFunction>());

    bool nativeWithJitEntry = fun->isNativeWithJitEntry();
    if (fun->isInterpreted() || nativeWithJitEntry) {
        // Never attach optimized scripted call stubs for JSOP_FUNAPPLY.
        // MagicArguments may escape the frame through them.
        if (op == JSOP_FUNAPPLY)
            return true;

        // If callee is not an interpreted constructor, we have to throw.
        if (constructing && !fun->isConstructor())
            return true;

        // Likewise, if the callee is a class constructor, we have to throw.
        if (!constructing && fun->isClassConstructor())
            return true;

        if (!fun->hasJitEntry()) {
            // Don't treat this as an unoptimizable case, as we'll add a stub
            // when the callee is delazified.
            *handled = true;
            return true;
        }

        // If we're constructing, require the callee to have JIT code. This
        // isn't required for correctness but avoids allocating a template
        // object below for constructors that aren't hot. See bug 1419758.
        if (constructing && !fun->hasJITCode()) {
            *handled = true;
            return true;
        }

        // Check if this stub chain has already generalized scripted calls.
        if (stub->scriptedStubsAreGeneralized()) {
            JitSpew(JitSpew_BaselineIC, "  Chain already has generalized scripted call stub!");
            return true;
        }

        if (stub->state().mode() == ICState::Mode::Megamorphic) {
            // Create a Call_AnyScripted stub.
            JitSpew(JitSpew_BaselineIC, "  Generating Call_AnyScripted stub (cons=%s, spread=%s)",
                    constructing ? "yes" : "no", isSpread ? "yes" : "no");
            ICCallScriptedCompiler compiler(cx, typeMonitorFallback->firstMonitorStub(),
                                            constructing, isSpread, script->pcToOffset(pc));
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            // Before adding new stub, unlink all previous Call_Scripted.
            stub->unlinkStubsWithKind(cx, ICStub::Call_Scripted);

            // Add new generalized stub.
            stub->addNewStub(newStub);
            *handled = true;
            return true;
        }

        // Keep track of the function's |prototype| property in type
        // information, for use during Ion compilation.
        if (IsIonEnabled(cx))
            EnsureTrackPropertyTypes(cx, fun, NameToId(cx->names().prototype));

        // Remember the template object associated with any script being called
        // as a constructor, for later use during Ion compilation. This is unsound
        // for super(), as a single callsite can have multiple possible prototype object
        // created (via different newTargets)
        RootedObject templateObject(cx);
        if (constructing && !isSuper) {
            // If we are calling a constructor for which the new script
            // properties analysis has not been performed yet, don't attach a
            // stub. After the analysis is performed, CreateThisForFunction may
            // start returning objects with a different type, and the Ion
            // compiler will get confused.

            // Only attach a stub if the function already has a prototype and
            // we can look it up without causing side effects.
            RootedObject newTarget(cx, &vp[2 + argc].toObject());
            RootedValue protov(cx);
            if (!GetPropertyPure(cx, newTarget, NameToId(cx->names().prototype), protov.address())) {
                JitSpew(JitSpew_BaselineIC, "  Can't purely lookup function prototype");
                return true;
            }

            if (protov.isObject()) {
                TaggedProto proto(&protov.toObject());
                ObjectGroup* group = ObjectGroup::defaultNewGroup(cx, nullptr, proto, newTarget);
                if (!group)
                    return false;

                if (group->newScript() && !group->newScript()->analyzed()) {
                    JitSpew(JitSpew_BaselineIC, "  Function newScript has not been analyzed");

                    // This is temporary until the analysis is perfomed, so
                    // don't treat this as unoptimizable.
                    *handled = true;
                    return true;
                }
            }

            JSObject* thisObject = CreateThisForFunction(cx, fun, newTarget, TenuredObject);
            if (!thisObject)
                return false;

            if (thisObject->is<PlainObject>() || thisObject->is<UnboxedPlainObject>())
                templateObject = thisObject;
        }

        if (nativeWithJitEntry) {
            JitSpew(JitSpew_BaselineIC,
                    "  Generating Call_Scripted stub (native=%p with jit entry, cons=%s, spread=%s)",
                    fun->native(), constructing ? "yes" : "no", isSpread ? "yes" : "no");
        } else {
            JitSpew(JitSpew_BaselineIC,
                    "  Generating Call_Scripted stub (fun=%p, %s:%zu, cons=%s, spread=%s)",
                    fun.get(), fun->nonLazyScript()->filename(), fun->nonLazyScript()->lineno(),
                    constructing ? "yes" : "no", isSpread ? "yes" : "no");
        }

        ICCallScriptedCompiler compiler(cx, typeMonitorFallback->firstMonitorStub(),
                                        fun, templateObject,
                                        constructing, isSpread, script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *handled = true;
        return true;
    }

    if (fun->isNative() && (!constructing || (constructing && fun->isConstructor()))) {
        // Generalized native call stubs are not here yet!
        MOZ_ASSERT(!stub->nativeStubsAreGeneralized());

        // Check for JSOP_FUNAPPLY
        if (op == JSOP_FUNAPPLY) {
            if (fun->native() == fun_apply) {
                return TryAttachFunApplyStub(cx, stub, script, pc, thisv, argc, vp + 2,
                                             typeMonitorFallback, handled);
            }

            // Don't try to attach a "regular" optimized call stubs for FUNAPPLY ops,
            // since MagicArguments may escape through them.
            return true;
        }

        if (op == JSOP_FUNCALL && fun->native() == fun_call) {
            if (!TryAttachFunCallStub(cx, stub, script, pc, thisv, typeMonitorFallback, handled))
                return false;
            if (*handled)
                return true;
        }

        if (stub->state().mode() == ICState::Mode::Megamorphic) {
            JitSpew(JitSpew_BaselineIC,
                    "  Megamorphic Call_Native stubs. TODO: add Call_AnyNative!");
            return true;
        }

        if (fun->native() == intrinsic_IsSuspendedGenerator) {
            // This intrinsic only appears in self-hosted code.
            MOZ_ASSERT(op != JSOP_NEW);
            MOZ_ASSERT(argc == 1);
            JitSpew(JitSpew_BaselineIC, "  Generating Call_IsSuspendedGenerator stub");

            ICCall_IsSuspendedGenerator::Compiler compiler(cx);
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            stub->addNewStub(newStub);
            *handled = true;
            return true;
        }

        RootedObject templateObject(cx);
        if (MOZ_LIKELY(!isSpread && !isSuper)) {
            bool skipAttach = false;
            CallArgs args = CallArgsFromVp(argc, vp);
            if (!GetTemplateObjectForNative(cx, fun, args, &templateObject, &skipAttach))
                return false;
            if (skipAttach) {
                *handled = true;
                return true;
            }
            MOZ_ASSERT_IF(templateObject, !templateObject->group()->maybePreliminaryObjects());
        }

        bool ignoresReturnValue = op == JSOP_CALL_IGNORES_RV &&
                                  fun->isNative() &&
                                  fun->hasJitInfo() &&
                                  fun->jitInfo()->type() == JSJitInfo::IgnoresReturnValueNative;

        JitSpew(JitSpew_BaselineIC, "  Generating Call_Native stub (fun=%p, cons=%s, spread=%s)",
                fun.get(), constructing ? "yes" : "no", isSpread ? "yes" : "no");
        ICCall_Native::Compiler compiler(cx, typeMonitorFallback->firstMonitorStub(),
                                         fun, templateObject, constructing, ignoresReturnValue,
                                         isSpread, script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *handled = true;
        return true;
    }

    return true;
}

static bool
CopyArray(JSContext* cx, HandleArrayObject arr, MutableHandleValue result)
{
    uint32_t length = arr->length();
    ArrayObject* nobj = NewFullyAllocatedArrayTryReuseGroup(cx, arr, length, TenuredObject);
    if (!nobj)
        return false;
    nobj->initDenseElements(arr, 0, length);

    result.setObject(*nobj);
    return true;
}

static bool
TryAttachConstStringSplit(JSContext* cx, ICCall_Fallback* stub, HandleScript script,
                          uint32_t argc, HandleValue callee, Value* vp, jsbytecode* pc,
                          HandleValue res, bool* attached)
{
    if (stub->numOptimizedStubs() != 0)
        return true;

    Value* args = vp + 2;

    // String.prototype.split will not yield a constructable.
    if (JSOp(*pc) == JSOP_NEW)
        return true;

    if (!IsOptimizableConstStringSplit(callee, argc, args))
        return true;

    MOZ_ASSERT(callee.isObject());
    MOZ_ASSERT(callee.toObject().is<JSFunction>());

    RootedString str(cx, args[0].toString());
    RootedString sep(cx, args[1].toString());
    RootedObject obj(cx, &res.toObject());
    RootedValue arr(cx);

    // Copy the array before storing in stub.
    if (!CopyArray(cx, obj.as<ArrayObject>(), &arr))
        return false;

    // Atomize all elements of the array.
    RootedArrayObject arrObj(cx, &arr.toObject().as<ArrayObject>());
    uint32_t initLength = arrObj->length();
    for (uint32_t i = 0; i < initLength; i++) {
        JSAtom* str = js::AtomizeString(cx, arrObj->getDenseElement(i).toString());
        if (!str)
            return false;

        arrObj->setDenseElementWithType(cx, i, StringValue(str));
    }

    ICTypeMonitor_Fallback* typeMonitorFallback = stub->getFallbackMonitorStub(cx, script);
    if (!typeMonitorFallback)
        return false;

    ICCall_ConstStringSplit::Compiler compiler(cx, typeMonitorFallback->firstMonitorStub(),
                                               script->pcToOffset(pc), str, sep, arrObj);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
DoCallFallback(JSContext* cx, BaselineFrame* frame, ICCall_Fallback* stub_, uint32_t argc,
               Value* vp, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICCall_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    FallbackICSpew(cx, stub, "Call(%s)", CodeName[op]);

    MOZ_ASSERT(argc == GET_ARGC(pc));
    bool constructing = (op == JSOP_NEW || op == JSOP_SUPERCALL);
    bool ignoresReturnValue = (op == JSOP_CALL_IGNORES_RV);

    // Ensure vp array is rooted - we may GC in here.
    size_t numValues = argc + 2 + constructing;
    AutoArrayRooter vpRoot(cx, numValues, vp);

    CallArgs callArgs = CallArgsFromSp(argc + constructing, vp + numValues, constructing,
                                       ignoresReturnValue);
    RootedValue callee(cx, vp[0]);

    // Handle funapply with JSOP_ARGUMENTS
    if (op == JSOP_FUNAPPLY && argc == 2 && callArgs[1].isMagic(JS_OPTIMIZED_ARGUMENTS)) {
        if (!GuardFunApplyArgumentsOptimization(cx, frame, callArgs))
            return false;
    }

    // Transition stub state to megamorphic or generic if warranted.
    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    bool canAttachStub = stub->state().canAttachStub();
    bool handled = false;

    // Only bother to try optimizing JSOP_CALL with CacheIR if the chain is still
    // allowed to attach stubs.
    if (canAttachStub) {
        CallIRGenerator gen(cx, script, pc, op, stub->state().mode(), argc,
                            callee, callArgs.thisv(),
                            HandleValueArray::fromMarkedLocation(argc, vp+2));
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        gen.cacheIRStubKind(),
                                                        ICStubEngine::Baseline,
                                                        script, stub, &handled);

            if (newStub) {
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");

                // If it's an updated stub, initialize it.
                if (gen.cacheIRStubKind() == BaselineCacheIRStubKind::Updated)
                    SetUpdateStubData(newStub->toCacheIR_Updated(), gen.typeCheckInfo());
            }
        }

        // Try attaching a regular call stub, but only if the CacheIR attempt didn't add
        // any stubs.
        if (!handled) {
            bool createSingleton = ObjectGroup::useSingletonForNewObject(cx, script, pc);
            if (!TryAttachCallStub(cx, stub, script, pc, op, argc, vp, constructing, false,
                                   createSingleton, &handled))
            {
                return false;
            }
        }
    }

    if (constructing) {
        if (!ConstructFromStack(cx, callArgs))
            return false;
        res.set(callArgs.rval());
    } else if ((op == JSOP_EVAL || op == JSOP_STRICTEVAL) &&
               frame->environmentChain()->global().valueIsEval(callee))
    {
        if (!DirectEval(cx, callArgs.get(0), res))
            return false;
    } else {
        MOZ_ASSERT(op == JSOP_CALL ||
                   op == JSOP_CALL_IGNORES_RV ||
                   op == JSOP_CALLITER ||
                   op == JSOP_FUNCALL ||
                   op == JSOP_FUNAPPLY ||
                   op == JSOP_EVAL ||
                   op == JSOP_STRICTEVAL);
        if (op == JSOP_CALLITER && callee.isPrimitive()) {
            MOZ_ASSERT(argc == 0, "thisv must be on top of the stack");
            ReportValueError(cx, JSMSG_NOT_ITERABLE, -1, callArgs.thisv(), nullptr);
            return false;
        }

        if (!CallFromStack(cx, callArgs))
            return false;

        res.set(callArgs.rval());
    }

    StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);
    TypeScript::Monitor(cx, script, pc, types, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame, types, res))
        return false;

    // Try to transition again in case we called this IC recursively.
    if (stub->state().maybeTransition())
        stub->discardStubs(cx);
    canAttachStub = stub->state().canAttachStub();

    if (!handled && canAttachStub) {
        // If 'callee' is a potential Call_ConstStringSplit, try to attach an
        // optimized ConstStringSplit stub. Note that vp[0] now holds the return value
        // instead of the callee, so we pass the callee as well.
        if (!TryAttachConstStringSplit(cx, stub, script, argc, callee, vp, pc, res, &handled))
            return false;
    }

    if (!handled) {
        stub->noteUnoptimizableCall();
        if (canAttachStub)
            stub->state().trackNotAttached();
    }
    return true;
}

static bool
DoSpreadCallFallback(JSContext* cx, BaselineFrame* frame, ICCall_Fallback* stub_, Value* vp,
                     MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICCall_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    JSOp op = JSOp(*pc);
    bool constructing = (op == JSOP_SPREADNEW || op == JSOP_SPREADSUPERCALL);
    FallbackICSpew(cx, stub, "SpreadCall(%s)", CodeName[op]);

    // Ensure vp array is rooted - we may GC in here.
    AutoArrayRooter vpRoot(cx, 3 + constructing, vp);

    RootedValue callee(cx, vp[0]);
    RootedValue thisv(cx, vp[1]);
    RootedValue arr(cx, vp[2]);
    RootedValue newTarget(cx, constructing ? vp[3] : NullValue());

    // Try attaching a call stub.
    bool handled = false;
    if (op != JSOP_SPREADEVAL && op != JSOP_STRICTSPREADEVAL &&
        !TryAttachCallStub(cx, stub, script, pc, op, 1, vp, constructing, true, false,
                           &handled))
    {
        return false;
    }

    if (!SpreadCallOperation(cx, script, pc, thisv, callee, arr, newTarget, res))
        return false;

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    StackTypeSet* types = TypeScript::BytecodeTypes(script, pc);
    if (!stub->addMonitorStubForValue(cx, frame, types, res))
        return false;

    if (!handled)
        stub->noteUnoptimizableCall();
    return true;
}

void
ICCallStubCompiler::pushCallArguments(MacroAssembler& masm, AllocatableGeneralRegisterSet regs,
                                      Register argcReg, bool isJitCall, bool isConstructing)
{
    MOZ_ASSERT(!regs.has(argcReg));

    // Account for new.target
    Register count = regs.takeAny();

    masm.move32(argcReg, count);

    // If we are setting up for a jitcall, we have to align the stack taking
    // into account the args and newTarget. We could also count callee and |this|,
    // but it's a waste of stack space. Because we want to keep argcReg unchanged,
    // just account for newTarget initially, and add the other 2 after assuring
    // allignment.
    if (isJitCall) {
        if (isConstructing)
            masm.add32(Imm32(1), count);
    } else {
        masm.add32(Imm32(2 + isConstructing), count);
    }

    // argPtr initially points to the last argument.
    Register argPtr = regs.takeAny();
    masm.moveStackPtrTo(argPtr);

    // Skip 4 pointers pushed on top of the arguments: the frame descriptor,
    // return address, old frame pointer and stub reg.
    masm.addPtr(Imm32(STUB_FRAME_SIZE), argPtr);

    // Align the stack such that the JitFrameLayout is aligned on the
    // JitStackAlignment.
    if (isJitCall) {
        masm.alignJitStackBasedOnNArgs(count);

        // Account for callee and |this|, skipped earlier
        masm.add32(Imm32(2), count);
    }

    // Push all values, starting at the last one.
    Label loop, done;
    masm.bind(&loop);
    masm.branchTest32(Assembler::Zero, count, count, &done);
    {
        masm.pushValue(Address(argPtr, 0));
        masm.addPtr(Imm32(sizeof(Value)), argPtr);

        masm.sub32(Imm32(1), count);
        masm.jump(&loop);
    }
    masm.bind(&done);
}

void
ICCallStubCompiler::guardSpreadCall(MacroAssembler& masm, Register argcReg, Label* failure,
                                    bool isConstructing)
{
    masm.unboxObject(Address(masm.getStackPointer(),
                     isConstructing * sizeof(Value) + ICStackValueOffset), argcReg);
    masm.loadPtr(Address(argcReg, NativeObject::offsetOfElements()), argcReg);
    masm.load32(Address(argcReg, ObjectElements::offsetOfLength()), argcReg);

    // Limit actual argc to something reasonable (huge number of arguments can
    // blow the stack limit).
    static_assert(ICCall_Scripted::MAX_ARGS_SPREAD_LENGTH <= ARGS_LENGTH_MAX,
                  "maximum arguments length for optimized stub should be <= ARGS_LENGTH_MAX");
    masm.branch32(Assembler::Above, argcReg, Imm32(ICCall_Scripted::MAX_ARGS_SPREAD_LENGTH),
                  failure);
}

void
ICCallStubCompiler::pushSpreadCallArguments(MacroAssembler& masm,
                                            AllocatableGeneralRegisterSet regs,
                                            Register argcReg, bool isJitCall,
                                            bool isConstructing)
{
    // Pull the array off the stack before aligning.
    Register startReg = regs.takeAny();
    masm.unboxObject(Address(masm.getStackPointer(),
                             (isConstructing * sizeof(Value)) + STUB_FRAME_SIZE), startReg);
    masm.loadPtr(Address(startReg, NativeObject::offsetOfElements()), startReg);

    // Align the stack such that the JitFrameLayout is aligned on the
    // JitStackAlignment.
    if (isJitCall) {
        Register alignReg = argcReg;
        if (isConstructing) {
            alignReg = regs.takeAny();
            masm.movePtr(argcReg, alignReg);
            masm.addPtr(Imm32(1), alignReg);
        }
        masm.alignJitStackBasedOnNArgs(alignReg);
        if (isConstructing) {
            MOZ_ASSERT(alignReg != argcReg);
            regs.add(alignReg);
        }
    }

    // Push newTarget, if necessary
    if (isConstructing)
        masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE));

    // Push arguments: set up endReg to point to &array[argc]
    Register endReg = regs.takeAny();
    masm.movePtr(argcReg, endReg);
    static_assert(sizeof(Value) == 8, "Value must be 8 bytes");
    masm.lshiftPtr(Imm32(3), endReg);
    masm.addPtr(startReg, endReg);

    // Copying pre-decrements endReg by 8 until startReg is reached
    Label copyDone;
    Label copyStart;
    masm.bind(&copyStart);
    masm.branchPtr(Assembler::Equal, endReg, startReg, &copyDone);
    masm.subPtr(Imm32(sizeof(Value)), endReg);
    masm.pushValue(Address(endReg, 0));
    masm.jump(&copyStart);
    masm.bind(&copyDone);

    regs.add(startReg);
    regs.add(endReg);

    // Push the callee and |this|.
    masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE + (1 + isConstructing) * sizeof(Value)));
    masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE + (2 + isConstructing) * sizeof(Value)));
}

Register
ICCallStubCompiler::guardFunApply(MacroAssembler& masm, AllocatableGeneralRegisterSet regs,
                                  Register argcReg, FunApplyThing applyThing,
                                  Label* failure)
{
    // Ensure argc == 2
    masm.branch32(Assembler::NotEqual, argcReg, Imm32(2), failure);

    // Stack looks like:
    //      [..., CalleeV, ThisV, Arg0V, Arg1V <MaybeReturnReg>]

    Address secondArgSlot(masm.getStackPointer(), ICStackValueOffset);
    if (applyThing == FunApply_MagicArgs) {
        // Ensure that the second arg is magic arguments.
        masm.branchTestMagic(Assembler::NotEqual, secondArgSlot, failure);

        // Ensure that this frame doesn't have an arguments object.
        masm.branchTest32(Assembler::NonZero,
                          Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags()),
                          Imm32(BaselineFrame::HAS_ARGS_OBJ),
                          failure);

        // Limit the length to something reasonable.
        masm.branch32(Assembler::Above,
                      Address(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs()),
                      Imm32(ICCall_ScriptedApplyArray::MAX_ARGS_ARRAY_LENGTH),
                      failure);
    } else {
        MOZ_ASSERT(applyThing == FunApply_Array);

        AllocatableGeneralRegisterSet regsx = regs;

        // Ensure that the second arg is an array.
        ValueOperand secondArgVal = regsx.takeAnyValue();
        masm.loadValue(secondArgSlot, secondArgVal);

        masm.branchTestObject(Assembler::NotEqual, secondArgVal, failure);
        Register secondArgObj = masm.extractObject(secondArgVal, ExtractTemp1);

        regsx.add(secondArgVal);
        regsx.takeUnchecked(secondArgObj);

        masm.branchTestObjClass(Assembler::NotEqual, secondArgObj, &ArrayObject::class_,
                                regsx.getAny(), secondArgObj, failure);

        // Get the array elements and ensure that initializedLength == length
        masm.loadPtr(Address(secondArgObj, NativeObject::offsetOfElements()), secondArgObj);

        Register lenReg = regsx.takeAny();
        masm.load32(Address(secondArgObj, ObjectElements::offsetOfLength()), lenReg);

        masm.branch32(Assembler::NotEqual,
                      Address(secondArgObj, ObjectElements::offsetOfInitializedLength()),
                      lenReg, failure);

        // Limit the length to something reasonable (huge number of arguments can
        // blow the stack limit).
        masm.branch32(Assembler::Above, lenReg,
                      Imm32(ICCall_ScriptedApplyArray::MAX_ARGS_ARRAY_LENGTH),
                      failure);

        // Ensure no holes.  Loop through values in array and make sure none are magic.
        // Start address is secondArgObj, end address is secondArgObj + (lenReg * sizeof(Value))
        static_assert(sizeof(Value) == 8, "shift by 3 below assumes Value is 8 bytes");
        masm.lshiftPtr(Imm32(3), lenReg);
        masm.addPtr(secondArgObj, lenReg);

        Register start = secondArgObj;
        Register end = lenReg;
        Label loop;
        Label endLoop;
        masm.bind(&loop);
        masm.branchPtr(Assembler::AboveOrEqual, start, end, &endLoop);
        masm.branchTestMagic(Assembler::Equal, Address(start, 0), failure);
        masm.addPtr(Imm32(sizeof(Value)), start);
        masm.jump(&loop);
        masm.bind(&endLoop);
    }

    // Stack now confirmed to be like:
    //      [..., CalleeV, ThisV, Arg0V, MagicValue(Arguments), <MaybeReturnAddr>]

    // Load the callee, ensure that it's fun_apply
    ValueOperand val = regs.takeAnyValue();
    Address calleeSlot(masm.getStackPointer(), ICStackValueOffset + (3 * sizeof(Value)));
    masm.loadValue(calleeSlot, val);

    masm.branchTestObject(Assembler::NotEqual, val, failure);
    Register callee = masm.extractObject(val, ExtractTemp1);

    masm.branchTestObjClass(Assembler::NotEqual, callee, &JSFunction::class_, regs.getAny(),
                            callee, failure);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrEnv()), callee);

    masm.branchPtr(Assembler::NotEqual, callee, ImmPtr(fun_apply), failure);

    // Load the |thisv|, ensure that it's a scripted function with a valid baseline or ion
    // script, or a native function.
    Address thisSlot(masm.getStackPointer(), ICStackValueOffset + (2 * sizeof(Value)));
    masm.loadValue(thisSlot, val);

    masm.branchTestObject(Assembler::NotEqual, val, failure);
    Register target = masm.extractObject(val, ExtractTemp1);
    regs.add(val);
    regs.takeUnchecked(target);

    masm.branchTestObjClass(Assembler::NotEqual, target, &JSFunction::class_, regs.getAny(),
                            target, failure);

    Register temp = regs.takeAny();
    masm.branchIfFunctionHasNoJitEntry(target, /* constructing */ false, failure);
    masm.branchFunctionKind(Assembler::Equal, JSFunction::ClassConstructor, callee, temp, failure);
    regs.add(temp);
    return target;
}

void
ICCallStubCompiler::pushCallerArguments(MacroAssembler& masm, AllocatableGeneralRegisterSet regs)
{
    // Initialize copyReg to point to start caller arguments vector.
    // Initialize argcReg to poitn to the end of it.
    Register startReg = regs.takeAny();
    Register endReg = regs.takeAny();
    masm.loadPtr(Address(BaselineFrameReg, 0), startReg);
    masm.loadPtr(Address(startReg, BaselineFrame::offsetOfNumActualArgs()), endReg);
    masm.addPtr(Imm32(BaselineFrame::offsetOfArg(0)), startReg);
    masm.alignJitStackBasedOnNArgs(endReg);
    masm.lshiftPtr(Imm32(ValueShift), endReg);
    masm.addPtr(startReg, endReg);

    // Copying pre-decrements endReg by 8 until startReg is reached
    Label copyDone;
    Label copyStart;
    masm.bind(&copyStart);
    masm.branchPtr(Assembler::Equal, endReg, startReg, &copyDone);
    masm.subPtr(Imm32(sizeof(Value)), endReg);
    masm.pushValue(Address(endReg, 0));
    masm.jump(&copyStart);
    masm.bind(&copyDone);
}

void
ICCallStubCompiler::pushArrayArguments(MacroAssembler& masm, Address arrayVal,
                                       AllocatableGeneralRegisterSet regs)
{
    // Load start and end address of values to copy.
    // guardFunApply has already gauranteed that the array is packed and contains
    // no holes.
    Register startReg = regs.takeAny();
    Register endReg = regs.takeAny();
    masm.extractObject(arrayVal, startReg);
    masm.loadPtr(Address(startReg, NativeObject::offsetOfElements()), startReg);
    masm.load32(Address(startReg, ObjectElements::offsetOfInitializedLength()), endReg);
    masm.alignJitStackBasedOnNArgs(endReg);
    masm.lshiftPtr(Imm32(ValueShift), endReg);
    masm.addPtr(startReg, endReg);

    // Copying pre-decrements endReg by 8 until startReg is reached
    Label copyDone;
    Label copyStart;
    masm.bind(&copyStart);
    masm.branchPtr(Assembler::Equal, endReg, startReg, &copyDone);
    masm.subPtr(Imm32(sizeof(Value)), endReg);
    masm.pushValue(Address(endReg, 0));
    masm.jump(&copyStart);
    masm.bind(&copyDone);
}

typedef bool (*DoCallFallbackFn)(JSContext*, BaselineFrame*, ICCall_Fallback*,
                                 uint32_t, Value*, MutableHandleValue);
static const VMFunction DoCallFallbackInfo =
    FunctionInfo<DoCallFallbackFn>(DoCallFallback, "DoCallFallback");

typedef bool (*DoSpreadCallFallbackFn)(JSContext*, BaselineFrame*, ICCall_Fallback*,
                                       Value*, MutableHandleValue);
static const VMFunction DoSpreadCallFallbackInfo =
    FunctionInfo<DoSpreadCallFallbackFn>(DoSpreadCallFallback, "DoSpreadCallFallback");

bool
ICCall_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    MOZ_ASSERT(R0 == JSReturnOperand);

    // Values are on the stack left-to-right. Calling convention wants them
    // right-to-left so duplicate them on the stack in reverse order.
    // |this| and callee are pushed last.

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));

    if (MOZ_UNLIKELY(isSpread_)) {
        // Push a stub frame so that we can perform a non-tail call.
        enterStubFrame(masm, R1.scratchReg());

        // Use BaselineFrameReg instead of BaselineStackReg, because
        // BaselineFrameReg and BaselineStackReg hold the same value just after
        // calling enterStubFrame.

        // newTarget
        if (isConstructing_)
            masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE));

        // array
        uint32_t valueOffset = isConstructing_;
        masm.pushValue(Address(BaselineFrameReg, valueOffset++ * sizeof(Value) + STUB_FRAME_SIZE));

        // this
        masm.pushValue(Address(BaselineFrameReg, valueOffset++ * sizeof(Value) + STUB_FRAME_SIZE));

        // callee
        masm.pushValue(Address(BaselineFrameReg, valueOffset++ * sizeof(Value) + STUB_FRAME_SIZE));

        masm.push(masm.getStackPointer());
        masm.push(ICStubReg);

        PushStubPayload(masm, R0.scratchReg());

        if (!callVM(DoSpreadCallFallbackInfo, masm))
            return false;

        leaveStubFrame(masm);
        EmitReturnFromIC(masm);

        // SPREADCALL is not yet supported in Ion, so do not generate asmcode for
        // bailout.
        return true;
    }

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, R1.scratchReg());

    regs.take(R0.scratchReg()); // argc.

    pushCallArguments(masm, regs, R0.scratchReg(), /* isJitCall = */ false, isConstructing_);

    masm.push(masm.getStackPointer());
    masm.push(R0.scratchReg());
    masm.push(ICStubReg);

    PushStubPayload(masm, R0.scratchReg());

    if (!callVM(DoCallFallbackInfo, masm))
        return false;

    leaveStubFrame(masm);
    EmitReturnFromIC(masm);

    // This is the resume point used when bailout rewrites call stack to undo
    // Ion inlined frames. The return address pushed onto reconstructed stack
    // will point here.
    assumeStubFrame();
    bailoutReturnOffset_.bind(masm.currentOffset());

    // Load passed-in ThisV into R1 just in case it's needed.  Need to do this before
    // we leave the stub frame since that info will be lost.
    // Current stack:  [...., ThisV, ActualArgc, CalleeToken, Descriptor ]
    masm.loadValue(Address(masm.getStackPointer(), 3 * sizeof(size_t)), R1);

    leaveStubFrame(masm, true);

    // If this is a |constructing| call, if the callee returns a non-object, we replace it with
    // the |this| object passed in.
    if (isConstructing_) {
        MOZ_ASSERT(JSReturnOperand == R0);
        Label skipThisReplace;

        masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
        masm.moveValue(R1, R0);
#ifdef DEBUG
        masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
        masm.assumeUnreachable("Failed to return object in constructing call.");
#endif
        masm.bind(&skipThisReplace);
    }

    // At this point, ICStubReg points to the ICCall_Fallback stub, which is NOT
    // a MonitoredStub, but rather a MonitoredFallbackStub.  To use EmitEnterTypeMonitorIC,
    // first load the ICTypeMonitor_Fallback stub into ICStubReg.  Then, use
    // EmitEnterTypeMonitorIC with a custom struct offset. Note that we must
    // have a non-null fallbackMonitorStub here because InitFromBailout
    // delazifies.
    masm.loadPtr(Address(ICStubReg, ICMonitoredFallbackStub::offsetOfFallbackMonitorStub()),
                 ICStubReg);
    EmitEnterTypeMonitorIC(masm, ICTypeMonitor_Fallback::offsetOfFirstMonitorStub());

    return true;
}

void
ICCall_Fallback::Compiler::postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code)
{
    if (MOZ_UNLIKELY(isSpread_))
        return;

    void* address = code->raw() + bailoutReturnOffset_.offset();
    BailoutReturnStub kind = isConstructing_ ? BailoutReturnStub::New
                                             : BailoutReturnStub::Call;
    cx->compartment()->jitCompartment()->initBailoutReturnAddr(address, getKey(), kind);
}

typedef bool (*CreateThisFn)(JSContext* cx, HandleObject callee, HandleObject newTarget,
                             MutableHandleValue rval);
static const VMFunction CreateThisInfoBaseline =
    FunctionInfo<CreateThisFn>(CreateThis, "CreateThis");

bool
ICCallScriptedCompiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
    bool canUseTailCallReg = regs.has(ICTailCallReg);

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(ICTailCallReg);

    if (isSpread_)
        guardSpreadCall(masm, argcReg, &failure, isConstructing_);

    // Load the callee in R1, accounting for newTarget, if necessary
    // Stack Layout: [ ..., CalleeVal, ThisVal, Arg0Val, ..., ArgNVal, [newTarget] +ICStackValueOffset+ ]
    if (isSpread_) {
        unsigned skipToCallee = (2 + isConstructing_) * sizeof(Value);
        masm.loadValue(Address(masm.getStackPointer(), skipToCallee + ICStackValueOffset), R1);
    } else {
        // Account for newTarget, if necessary
        unsigned nonArgsSkip = (1 + isConstructing_) * sizeof(Value);
        BaseValueIndex calleeSlot(masm.getStackPointer(), argcReg, ICStackValueOffset + nonArgsSkip);
        masm.loadValue(calleeSlot, R1);
    }
    regs.take(R1);

    // Ensure callee is an object.
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    // Ensure callee is a function.
    Register callee = masm.extractObject(R1, ExtractTemp0);

    // If calling a specific script, check if the script matches.  Otherwise, ensure that
    // callee function is scripted.  Leave calleeScript in |callee| reg.
    if (callee_) {
        MOZ_ASSERT(kind == ICStub::Call_Scripted);

        // Check if the object matches this callee.
        Address expectedCallee(ICStubReg, ICCall_Scripted::offsetOfCallee());
        masm.branchPtr(Assembler::NotEqual, expectedCallee, callee, &failure);

        // Guard against relazification.
        masm.branchIfFunctionHasNoJitEntry(callee, isConstructing_, &failure);
    } else {
        // Ensure the object is a function.
        masm.branchTestObjClass(Assembler::NotEqual, callee, &JSFunction::class_, regs.getAny(),
                                callee, &failure);
        if (isConstructing_) {
            masm.branchIfNotInterpretedConstructor(callee, regs.getAny(), &failure);
        } else {
            masm.branchIfFunctionHasNoJitEntry(callee, /* constructing */ false, &failure);
            masm.branchFunctionKind(Assembler::Equal, JSFunction::ClassConstructor, callee,
                                    regs.getAny(), &failure);
        }
    }

    // Load the start of the target JitCode.
    Register code;
    if (!isConstructing_) {
        code = regs.takeAny();
        masm.loadJitCodeRaw(callee, code);
    }

    // We no longer need R1.
    regs.add(R1);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());
    if (canUseTailCallReg)
        regs.add(ICTailCallReg);

    if (isConstructing_) {
        // Save argc before call.
        masm.push(argcReg);

        // Stack now looks like:
        //      [..., Callee, ThisV, Arg0V, ..., ArgNV, NewTarget, StubFrameHeader, ArgC ]
        masm.loadValue(Address(masm.getStackPointer(), STUB_FRAME_SIZE + sizeof(size_t)), R1);
        masm.push(masm.extractObject(R1, ExtractTemp0));

        if (isSpread_) {
            masm.loadValue(Address(masm.getStackPointer(),
                                   3 * sizeof(Value) + STUB_FRAME_SIZE + sizeof(size_t) +
                                   sizeof(JSObject*)),
                                   R1);
        } else {
            BaseValueIndex calleeSlot2(masm.getStackPointer(), argcReg,
                                       2 * sizeof(Value) + STUB_FRAME_SIZE + sizeof(size_t) +
                                       sizeof(JSObject*));
            masm.loadValue(calleeSlot2, R1);
        }
        masm.push(masm.extractObject(R1, ExtractTemp0));
        if (!callVM(CreateThisInfoBaseline, masm))
            return false;

        // Return of CreateThis must be an object or uninitialized.
#ifdef DEBUG
        Label createdThisOK;
        masm.branchTestObject(Assembler::Equal, JSReturnOperand, &createdThisOK);
        masm.branchTestMagic(Assembler::Equal, JSReturnOperand, &createdThisOK);
        masm.assumeUnreachable("The return of CreateThis must be an object or uninitialized.");
        masm.bind(&createdThisOK);
#endif

        // Reset the register set from here on in.
        static_assert(JSReturnOperand == R0, "The code below needs to be adapted.");
        regs = availableGeneralRegs(0);
        regs.take(R0);
        argcReg = regs.takeAny();

        // Restore saved argc so we can use it to calculate the address to save
        // the resulting this object to.
        masm.pop(argcReg);

        // Save "this" value back into pushed arguments on stack.  R0 can be clobbered after that.
        // Stack now looks like:
        //      [..., Callee, ThisV, Arg0V, ..., ArgNV, [NewTarget], StubFrameHeader ]
        if (isSpread_) {
            masm.storeValue(R0, Address(masm.getStackPointer(),
                                        (1 + isConstructing_) * sizeof(Value) + STUB_FRAME_SIZE));
        } else {
            BaseValueIndex thisSlot(masm.getStackPointer(), argcReg,
                                    STUB_FRAME_SIZE + isConstructing_ * sizeof(Value));
            masm.storeValue(R0, thisSlot);
        }

        // Restore the stub register from the baseline stub frame.
        masm.loadPtr(Address(masm.getStackPointer(), STUB_FRAME_SAVED_STUB_OFFSET), ICStubReg);

        // Reload callee script. Note that a GC triggered by CreateThis may
        // have destroyed the callee BaselineScript and IonScript. CreateThis
        // is safely repeatable though, so in this case we just leave the stub
        // frame and jump to the next stub.

        // Just need to load the script now.
        if (isSpread_) {
            unsigned skipForCallee = (2 + isConstructing_) * sizeof(Value);
            masm.loadValue(Address(masm.getStackPointer(), skipForCallee + STUB_FRAME_SIZE), R0);
        } else {
            // Account for newTarget, if necessary
            unsigned nonArgsSkip = (1 + isConstructing_) * sizeof(Value);
            BaseValueIndex calleeSlot3(masm.getStackPointer(), argcReg, nonArgsSkip + STUB_FRAME_SIZE);
            masm.loadValue(calleeSlot3, R0);
        }
        callee = masm.extractObject(R0, ExtractTemp0);
        regs.add(R0);
        regs.takeUnchecked(callee);

        code = regs.takeAny();
        masm.loadJitCodeRaw(callee, code);

        // Release callee register, but don't add ExtractTemp0 back into the pool
        // ExtractTemp0 is used later, and if it's allocated to some other register at that
        // point, it will get clobbered when used.
        if (callee != ExtractTemp0)
            regs.add(callee);

        if (canUseTailCallReg)
            regs.addUnchecked(ICTailCallReg);
    }
    Register scratch = regs.takeAny();

    // Values are on the stack left-to-right. Calling convention wants them
    // right-to-left so duplicate them on the stack in reverse order.
    // |this| and callee are pushed last.
    if (isSpread_)
        pushSpreadCallArguments(masm, regs, argcReg, /* isJitCall = */ true, isConstructing_);
    else
        pushCallArguments(masm, regs, argcReg, /* isJitCall = */ true, isConstructing_);

    // The callee is on top of the stack. Pop and unbox it.
    ValueOperand val = regs.takeAnyValue();
    masm.popValue(val);
    callee = masm.extractObject(val, ExtractTemp0);

    EmitBaselineCreateStubFrameDescriptor(masm, scratch, JitFrameLayout::Size());

    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.
    masm.Push(argcReg);
    masm.PushCalleeToken(callee, isConstructing_);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), callee);
    masm.branch32(Assembler::AboveOrEqual, argcReg, callee, &noUnderflow);
    {
        // Call the arguments rectifier.
        TrampolinePtr argumentsRectifier = cx->runtime()->jitRuntime()->getArgumentsRectifier();
        masm.movePtr(argumentsRectifier, code);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    // If this is a constructing call, and the callee returns a non-object, replace it with
    // the |this| object passed in.
    if (isConstructing_) {
        Label skipThisReplace;
        masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);

        // Current stack: [ Padding?, ARGVALS..., ThisVal, ActualArgc, Callee, Descriptor ]
        // However, we can't use this ThisVal, because it hasn't been traced.  We need to use
        // The ThisVal higher up the stack:
        // Current stack: [ ThisVal, ARGVALS..., ...STUB FRAME...,
        //                  Padding?, ARGVALS..., ThisVal, ActualArgc, Callee, Descriptor ]

        // Restore the BaselineFrameReg based on the frame descriptor.
        //
        // BaselineFrameReg = BaselineStackReg
        //                  + sizeof(Descriptor) + sizeof(Callee) + sizeof(ActualArgc)
        //                  + stubFrameSize(Descriptor)
        //                  - sizeof(ICStubReg) - sizeof(BaselineFrameReg)
        Address descriptorAddr(masm.getStackPointer(), 0);
        masm.loadPtr(descriptorAddr, BaselineFrameReg);
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), BaselineFrameReg);
        masm.addPtr(Imm32((3 - 2) * sizeof(size_t)), BaselineFrameReg);
        masm.addStackPtrTo(BaselineFrameReg);

        // Load the number of arguments present before the stub frame.
        Register argcReg = JSReturnOperand.scratchReg();
        if (isSpread_) {
            // Account for the Array object.
            masm.move32(Imm32(1), argcReg);
        } else {
            Address argcAddr(masm.getStackPointer(), 2 * sizeof(size_t));
            masm.loadPtr(argcAddr, argcReg);
        }

        // Current stack: [ ThisVal, ARGVALS..., ...STUB FRAME..., <-- BaselineFrameReg
        //                  Padding?, ARGVALS..., ThisVal, ActualArgc, Callee, Descriptor ]
        //
        // &ThisVal = BaselineFrameReg + argc * sizeof(Value) + STUB_FRAME_SIZE + sizeof(Value)
        // This last sizeof(Value) accounts for the newTarget on the end of the arguments vector
        // which is not reflected in actualArgc
        BaseValueIndex thisSlotAddr(BaselineFrameReg, argcReg, STUB_FRAME_SIZE + sizeof(Value));
        masm.loadValue(thisSlotAddr, JSReturnOperand);
#ifdef DEBUG
        masm.branchTestObject(Assembler::Equal, JSReturnOperand, &skipThisReplace);
        masm.assumeUnreachable("Return of constructing call should be an object.");
#endif
        masm.bind(&skipThisReplace);
    }

    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

typedef bool (*CopyArrayFn)(JSContext*, HandleArrayObject, MutableHandleValue);
static const VMFunction CopyArrayInfo = FunctionInfo<CopyArrayFn>(CopyArray, "CopyArray");

bool
ICCall_ConstStringSplit::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // Stack Layout: [ ..., CalleeVal, ThisVal, strVal, sepVal, +ICStackValueOffset+ ]
    static const size_t SEP_DEPTH = 0;
    static const size_t STR_DEPTH = sizeof(Value);
    static const size_t CALLEE_DEPTH = 3 * sizeof(Value);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
    Label failureRestoreArgc;
#ifdef DEBUG
    Label twoArg;
    Register argcReg = R0.scratchReg();
    masm.branch32(Assembler::Equal, argcReg, Imm32(2), &twoArg);
    masm.assumeUnreachable("Expected argc == 2");
    masm.bind(&twoArg);
#endif
    Register scratchReg = regs.takeAny();

    // Guard that callee is native function js::intrinsic_StringSplitString.
    {
        Address calleeAddr(masm.getStackPointer(), ICStackValueOffset + CALLEE_DEPTH);
        ValueOperand calleeVal = regs.takeAnyValue();

        // Ensure that callee is an object.
        masm.loadValue(calleeAddr, calleeVal);
        masm.branchTestObject(Assembler::NotEqual, calleeVal, &failureRestoreArgc);

        // Ensure that callee is a function.
        Register calleeObj = masm.extractObject(calleeVal, ExtractTemp0);
        masm.branchTestObjClass(Assembler::NotEqual, calleeObj, &JSFunction::class_, scratchReg,
                                calleeObj, &failureRestoreArgc);

        // Ensure that callee's function impl is the native intrinsic_StringSplitString.
        masm.loadPtr(Address(calleeObj, JSFunction::offsetOfNativeOrEnv()), scratchReg);
        masm.branchPtr(Assembler::NotEqual, scratchReg, ImmPtr(js::intrinsic_StringSplitString),
                       &failureRestoreArgc);

        regs.add(calleeVal);
    }

    // Guard sep.
    {
        // Ensure that sep is a string.
        Address sepAddr(masm.getStackPointer(), ICStackValueOffset + SEP_DEPTH);
        ValueOperand sepVal = regs.takeAnyValue();

        masm.loadValue(sepAddr, sepVal);
        masm.branchTestString(Assembler::NotEqual, sepVal, &failureRestoreArgc);

        Register sep = masm.extractString(sepVal, ExtractTemp0);
        masm.branchPtr(Assembler::NotEqual, Address(ICStubReg, offsetOfExpectedSep()),
                       sep, &failureRestoreArgc);
        regs.add(sepVal);
    }

    // Guard str.
    {
        // Ensure that str is a string.
        Address strAddr(masm.getStackPointer(), ICStackValueOffset + STR_DEPTH);
        ValueOperand strVal = regs.takeAnyValue();

        masm.loadValue(strAddr, strVal);
        masm.branchTestString(Assembler::NotEqual, strVal, &failureRestoreArgc);

        Register str = masm.extractString(strVal, ExtractTemp0);
        masm.branchPtr(Assembler::NotEqual, Address(ICStubReg, offsetOfExpectedStr()),
                       str, &failureRestoreArgc);
        regs.add(strVal);
    }

    // Main stub body.
    {
        Register paramReg = regs.takeAny();

        // Push arguments.
        enterStubFrame(masm, scratchReg);
        masm.loadPtr(Address(ICStubReg, offsetOfTemplateObject()), paramReg);
        masm.push(paramReg);

        if (!callVM(CopyArrayInfo, masm))
            return false;
        leaveStubFrame(masm);
        regs.add(paramReg);
    }

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Guard failure path.
    masm.bind(&failureRestoreArgc);
    masm.move32(Imm32(2), R0.scratchReg());
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_IsSuspendedGenerator::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // The IsSuspendedGenerator intrinsic is only called in self-hosted code,
    // so it's safe to assume we have a single argument and the callee is our
    // intrinsic.

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));

    // Load the argument.
    Address argAddr(masm.getStackPointer(), ICStackValueOffset);
    ValueOperand argVal = regs.takeAnyValue();
    masm.loadValue(argAddr, argVal);

    // Check if it's an object.
    Label returnFalse;
    Register genObj = regs.takeAny();
    masm.branchTestObject(Assembler::NotEqual, argVal, &returnFalse);
    masm.unboxObject(argVal, genObj);

    // Check if it's a GeneratorObject.
    Register scratch = regs.takeAny();
    masm.branchTestObjClass(Assembler::NotEqual, genObj, &GeneratorObject::class_, scratch,
                            genObj, &returnFalse);

    // If the yield index slot holds an int32 value < YIELD_AND_AWAIT_INDEX_CLOSING,
    // the generator is suspended.
    masm.loadValue(Address(genObj, GeneratorObject::offsetOfYieldAndAwaitIndexSlot()), argVal);
    masm.branchTestInt32(Assembler::NotEqual, argVal, &returnFalse);
    masm.unboxInt32(argVal, scratch);
    masm.branch32(Assembler::AboveOrEqual, scratch,
                  Imm32(GeneratorObject::YIELD_AND_AWAIT_INDEX_CLOSING),
                  &returnFalse);

    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&returnFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);
    return true;
}

bool
ICCall_Native::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(ICTailCallReg);

    if (isSpread_)
        guardSpreadCall(masm, argcReg, &failure, isConstructing_);

    // Load the callee in R1.
    if (isSpread_) {
        unsigned skipToCallee = (2 + isConstructing_) * sizeof(Value);
        masm.loadValue(Address(masm.getStackPointer(), skipToCallee + ICStackValueOffset), R1);
    } else {
        unsigned nonArgsSlots = (1 + isConstructing_) * sizeof(Value);
        BaseValueIndex calleeSlot(masm.getStackPointer(), argcReg, ICStackValueOffset + nonArgsSlots);
        masm.loadValue(calleeSlot, R1);
    }
    regs.take(R1);

    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    // Ensure callee matches this stub's callee.
    Register callee = masm.extractObject(R1, ExtractTemp0);
    Address expectedCallee(ICStubReg, ICCall_Native::offsetOfCallee());
    masm.branchPtr(Assembler::NotEqual, expectedCallee, callee, &failure);

    regs.add(R1);
    regs.takeUnchecked(callee);

    // Push a stub frame so that we can perform a non-tail call.
    // Note that this leaves the return address in TailCallReg.
    enterStubFrame(masm, regs.getAny());

    // Values are on the stack left-to-right. Calling convention wants them
    // right-to-left so duplicate them on the stack in reverse order.
    // |this| and callee are pushed last.
    if (isSpread_)
        pushSpreadCallArguments(masm, regs, argcReg, /* isJitCall = */ false, isConstructing_);
    else
        pushCallArguments(masm, regs, argcReg, /* isJitCall = */ false, isConstructing_);

    // Native functions have the signature:
    //
    //    bool (*)(JSContext*, unsigned, Value* vp)
    //
    // Where vp[0] is space for callee/return value, vp[1] is |this|, and vp[2] onward
    // are the function arguments.

    // Initialize vp.
    Register vpReg = regs.takeAny();
    masm.moveStackPtrTo(vpReg);

    // Construct a native exit frame.
    masm.push(argcReg);

    Register scratch = regs.takeAny();
    EmitBaselineCreateStubFrameDescriptor(masm, scratch, ExitFrameLayout::Size());
    masm.push(scratch);
    masm.push(ICTailCallReg);
    masm.loadJSContext(scratch);
    masm.enterFakeExitFrameForNative(scratch, scratch, isConstructing_);

    // Execute call.
    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(argcReg);
    masm.passABIArg(vpReg);

#ifdef JS_SIMULATOR
    // The simulator requires VM calls to be redirected to a special swi
    // instruction to handle them, so we store the redirected pointer in the
    // stub and use that instead of the original one.
    masm.callWithABI(Address(ICStubReg, ICCall_Native::offsetOfNative()));
#else
    if (ignoresReturnValue_) {
        MOZ_ASSERT(callee_->hasJitInfo());
        masm.loadPtr(Address(callee, JSFunction::offsetOfJitInfo()), callee);
        masm.callWithABI(Address(callee, JSJitInfo::offsetOfIgnoresReturnValueNative()));
    } else {
        masm.callWithABI(Address(callee, JSFunction::offsetOfNative()));
    }
#endif

    // Test for failure.
    masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

    // Load the return value into R0.
    masm.loadValue(Address(masm.getStackPointer(), NativeExitFrameLayout::offsetOfResult()), R0);

    leaveStubFrame(masm);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_ClassHook::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(ICTailCallReg);

    // Load the callee in R1.
    unsigned nonArgSlots = (1 + isConstructing_) * sizeof(Value);
    BaseValueIndex calleeSlot(masm.getStackPointer(), argcReg, ICStackValueOffset + nonArgSlots);
    masm.loadValue(calleeSlot, R1);
    regs.take(R1);

    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    // Ensure the callee's class matches the one in this stub.
    // We use |Address(ICStubReg, ICCall_ClassHook::offsetOfNative())| below
    // instead of extracting the hook from callee. As a result the callee
    // register is no longer used and we must use spectreRegToZero := ICStubReg
    // instead.
    Register callee = masm.extractObject(R1, ExtractTemp0);
    Register scratch = regs.takeAny();
    masm.branchTestObjClass(Assembler::NotEqual, callee,
                            Address(ICStubReg, ICCall_ClassHook::offsetOfClass()),
                            scratch, ICStubReg, &failure);
    regs.add(R1);
    regs.takeUnchecked(callee);

    // Push a stub frame so that we can perform a non-tail call.
    // Note that this leaves the return address in TailCallReg.
    enterStubFrame(masm, regs.getAny());

    regs.add(scratch);
    pushCallArguments(masm, regs, argcReg, /* isJitCall = */ false, isConstructing_);
    regs.take(scratch);

    masm.assertStackAlignment(sizeof(Value), 0);

    // Native functions have the signature:
    //
    //    bool (*)(JSContext*, unsigned, Value* vp)
    //
    // Where vp[0] is space for callee/return value, vp[1] is |this|, and vp[2] onward
    // are the function arguments.

    // Initialize vp.
    Register vpReg = regs.takeAny();
    masm.moveStackPtrTo(vpReg);

    // Construct a native exit frame.
    masm.push(argcReg);

    EmitBaselineCreateStubFrameDescriptor(masm, scratch, ExitFrameLayout::Size());
    masm.push(scratch);
    masm.push(ICTailCallReg);
    masm.loadJSContext(scratch);
    masm.enterFakeExitFrameForNative(scratch, scratch, isConstructing_);

    // Execute call.
    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(argcReg);
    masm.passABIArg(vpReg);
    masm.callWithABI(Address(ICStubReg, ICCall_ClassHook::offsetOfNative()));

    // Test for failure.
    masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

    // Load the return value into R0.
    masm.loadValue(Address(masm.getStackPointer(), NativeExitFrameLayout::offsetOfResult()), R0);

    leaveStubFrame(masm);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_ScriptedApplyArray::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(ICTailCallReg);

    //
    // Validate inputs
    //

    Register target = guardFunApply(masm, regs, argcReg, FunApply_Array, &failure);
    if (regs.has(target)) {
        regs.take(target);
    } else {
        // If target is already a reserved reg, take another register for it, because it's
        // probably currently an ExtractTemp, which might get clobbered later.
        Register targetTemp = regs.takeAny();
        masm.movePtr(target, targetTemp);
        target = targetTemp;
    }

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());

    //
    // Push arguments
    //

    // Stack now looks like:
    //                                      BaselineFrameReg -------------------.
    //                                                                          v
    //      [..., fun_apply, TargetV, TargetThisV, ArgsArrayV, StubFrameHeader]

    // Push all array elements onto the stack:
    Address arrayVal(BaselineFrameReg, STUB_FRAME_SIZE);
    pushArrayArguments(masm, arrayVal, regs);

    // Stack now looks like:
    //                                      BaselineFrameReg -------------------.
    //                                                                          v
    //      [..., fun_apply, TargetV, TargetThisV, ArgsArrayV, StubFrameHeader,
    //       PushedArgN, ..., PushedArg0]
    // Can't fail after this, so it's ok to clobber argcReg.

    // Push actual argument 0 as |thisv| for call.
    masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE + sizeof(Value)));

    // All pushes after this use Push instead of push to make sure ARM can align
    // stack properly for call.
    Register scratch = regs.takeAny();
    EmitBaselineCreateStubFrameDescriptor(masm, scratch, JitFrameLayout::Size());

    // Reload argc from length of array.
    masm.extractObject(arrayVal, argcReg);
    masm.loadPtr(Address(argcReg, NativeObject::offsetOfElements()), argcReg);
    masm.load32(Address(argcReg, ObjectElements::offsetOfInitializedLength()), argcReg);

    masm.Push(argcReg);
    masm.Push(target);
    masm.Push(scratch);

    // Load nargs into scratch for underflow check, and then load jitcode pointer into target.
    masm.load16ZeroExtend(Address(target, JSFunction::offsetOfNargs()), scratch);
    masm.loadJitCodeRaw(target, target);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.branch32(Assembler::AboveOrEqual, argcReg, scratch, &noUnderflow);
    {
        // Call the arguments rectifier.
        TrampolinePtr argumentsRectifier = cx->runtime()->jitRuntime()->getArgumentsRectifier();
        masm.movePtr(argumentsRectifier, target);
    }
    masm.bind(&noUnderflow);
    regs.add(argcReg);

    // Do call.
    masm.callJit(target);
    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_ScriptedApplyArguments::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(ICTailCallReg);

    //
    // Validate inputs
    //

    Register target = guardFunApply(masm, regs, argcReg, FunApply_MagicArgs, &failure);
    if (regs.has(target)) {
        regs.take(target);
    } else {
        // If target is already a reserved reg, take another register for it, because it's
        // probably currently an ExtractTemp, which might get clobbered later.
        Register targetTemp = regs.takeAny();
        masm.movePtr(target, targetTemp);
        target = targetTemp;
    }

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());

    //
    // Push arguments
    //

    // Stack now looks like:
    //      [..., fun_apply, TargetV, TargetThisV, MagicArgsV, StubFrameHeader]

    // Push all arguments supplied to caller function onto the stack.
    pushCallerArguments(masm, regs);

    // Stack now looks like:
    //                                      BaselineFrameReg -------------------.
    //                                                                          v
    //      [..., fun_apply, TargetV, TargetThisV, MagicArgsV, StubFrameHeader,
    //       PushedArgN, ..., PushedArg0]
    // Can't fail after this, so it's ok to clobber argcReg.

    // Push actual argument 0 as |thisv| for call.
    masm.pushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE + sizeof(Value)));

    // All pushes after this use Push instead of push to make sure ARM can align
    // stack properly for call.
    Register scratch = regs.takeAny();
    EmitBaselineCreateStubFrameDescriptor(masm, scratch, JitFrameLayout::Size());

    masm.loadPtr(Address(BaselineFrameReg, 0), argcReg);
    masm.loadPtr(Address(argcReg, BaselineFrame::offsetOfNumActualArgs()), argcReg);
    masm.Push(argcReg);
    masm.Push(target);
    masm.Push(scratch);

    // Load nargs into scratch for underflow check, and then load jitcode pointer into target.
    masm.load16ZeroExtend(Address(target, JSFunction::offsetOfNargs()), scratch);
    masm.loadJitCodeRaw(target, target);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.branch32(Assembler::AboveOrEqual, argcReg, scratch, &noUnderflow);
    {
        // Call the arguments rectifier.
        TrampolinePtr argumentsRectifier = cx->runtime()->jitRuntime()->getArgumentsRectifier();
        masm.movePtr(argumentsRectifier, target);
    }
    masm.bind(&noUnderflow);
    regs.add(argcReg);

    // Do call
    masm.callJit(target);
    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_ScriptedFunCall::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
    bool canUseTailCallReg = regs.has(ICTailCallReg);

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(ICTailCallReg);

    // Load the callee in R1.
    // Stack Layout: [ ..., CalleeVal, ThisVal, Arg0Val, ..., ArgNVal, +ICStackValueOffset+ ]
    BaseValueIndex calleeSlot(masm.getStackPointer(), argcReg, ICStackValueOffset + sizeof(Value));
    masm.loadValue(calleeSlot, R1);
    regs.take(R1);

    // Ensure callee is fun_call.
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    Register callee = masm.extractObject(R1, ExtractTemp0);
    masm.branchTestObjClass(Assembler::NotEqual, callee, &JSFunction::class_, regs.getAny(),
                            callee, &failure);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrEnv()), callee);
    masm.branchPtr(Assembler::NotEqual, callee, ImmPtr(fun_call), &failure);

    // Ensure |this| is a function with a jit entry.
    BaseIndex thisSlot(masm.getStackPointer(), argcReg, TimesEight, ICStackValueOffset);
    masm.loadValue(thisSlot, R1);

    masm.branchTestObject(Assembler::NotEqual, R1, &failure);
    callee = masm.extractObject(R1, ExtractTemp0);

    masm.branchTestObjClass(Assembler::NotEqual, callee, &JSFunction::class_, regs.getAny(),
                            callee, &failure);
    masm.branchIfFunctionHasNoJitEntry(callee, /* constructing */ false, &failure);
    masm.branchFunctionKind(Assembler::Equal, JSFunction::ClassConstructor,
                            callee, regs.getAny(), &failure);

    // Load the start of the target JitCode.
    Register code = regs.takeAny();
    masm.loadJitCodeRaw(callee, code);

    // We no longer need R1.
    regs.add(R1);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());
    if (canUseTailCallReg)
        regs.add(ICTailCallReg);

    // Decrement argc if argc > 0. If argc == 0, push |undefined| as |this|.
    Label zeroArgs, done;
    masm.branchTest32(Assembler::Zero, argcReg, argcReg, &zeroArgs);

    // Avoid the copy of the callee (function.call).
    masm.sub32(Imm32(1), argcReg);

    // Values are on the stack left-to-right. Calling convention wants them
    // right-to-left so duplicate them on the stack in reverse order.

    pushCallArguments(masm, regs, argcReg, /* isJitCall = */ true);

    // Pop scripted callee (the original |this|).
    ValueOperand val = regs.takeAnyValue();
    masm.popValue(val);

    masm.jump(&done);
    masm.bind(&zeroArgs);

    // Copy scripted callee (the original |this|).
    Address thisSlotFromStubFrame(BaselineFrameReg, STUB_FRAME_SIZE);
    masm.loadValue(thisSlotFromStubFrame, val);

    // Align the stack.
    masm.alignJitStackBasedOnNArgs(0);

    // Store the new |this|.
    masm.pushValue(UndefinedValue());

    masm.bind(&done);

    // Unbox scripted callee.
    callee = masm.extractObject(val, ExtractTemp0);

    Register scratch = regs.takeAny();
    EmitBaselineCreateStubFrameDescriptor(masm, scratch, JitFrameLayout::Size());

    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.
    masm.Push(argcReg);
    masm.Push(callee);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), callee);
    masm.branch32(Assembler::AboveOrEqual, argcReg, callee, &noUnderflow);
    {
        // Call the arguments rectifier.
        TrampolinePtr argumentsRectifier = cx->runtime()->jitRuntime()->getArgumentsRectifier();
        masm.movePtr(argumentsRectifier, code);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    leaveStubFrame(masm, true);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

static bool
DoubleValueToInt32ForSwitch(Value* v)
{
    double d = v->toDouble();
    int32_t truncated = int32_t(d);
    if (d != double(truncated))
        return false;

    v->setInt32(truncated);
    return true;
}

bool
ICTableSwitch::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label isInt32, notInt32, outOfRange;
    Register scratch = R1.scratchReg();

    masm.branchTestInt32(Assembler::NotEqual, R0, &notInt32);

    Register key = masm.extractInt32(R0, ExtractTemp0);

    masm.bind(&isInt32);

    masm.load32(Address(ICStubReg, offsetof(ICTableSwitch, min_)), scratch);
    masm.sub32(scratch, key);
    masm.branch32(Assembler::BelowOrEqual,
                  Address(ICStubReg, offsetof(ICTableSwitch, length_)), key, &outOfRange);

    masm.loadPtr(Address(ICStubReg, offsetof(ICTableSwitch, table_)), scratch);
    masm.loadPtr(BaseIndex(scratch, key, ScalePointer), scratch);

    EmitChangeICReturnAddress(masm, scratch);
    EmitReturnFromIC(masm);

    masm.bind(&notInt32);

    masm.branchTestDouble(Assembler::NotEqual, R0, &outOfRange);
    if (cx->runtime()->jitSupportsFloatingPoint) {
        masm.unboxDouble(R0, FloatReg0);

        // N.B. -0 === 0, so convert -0 to a 0 int32.
        masm.convertDoubleToInt32(FloatReg0, key, &outOfRange, /* negativeZeroCheck = */ false);
    } else {
        // Pass pointer to double value.
        masm.pushValue(R0);
        masm.moveStackPtrTo(R0.scratchReg());

        masm.setupUnalignedABICall(scratch);
        masm.passABIArg(R0.scratchReg());
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, DoubleValueToInt32ForSwitch));

        // If the function returns |true|, the value has been converted to
        // int32.
        masm.movePtr(ReturnReg, scratch);
        masm.popValue(R0);
        masm.branchIfFalseBool(scratch, &outOfRange);
        masm.unboxInt32(R0, key);
    }
    masm.jump(&isInt32);

    masm.bind(&outOfRange);

    masm.loadPtr(Address(ICStubReg, offsetof(ICTableSwitch, defaultTarget_)), scratch);

    EmitChangeICReturnAddress(masm, scratch);
    EmitReturnFromIC(masm);
    return true;
}

ICStub*
ICTableSwitch::Compiler::getStub(ICStubSpace* space)
{
    JitCode* code = getStubCode();
    if (!code)
        return nullptr;

    jsbytecode* pc = pc_;
    pc += JUMP_OFFSET_LEN;
    int32_t low = GET_JUMP_OFFSET(pc);
    pc += JUMP_OFFSET_LEN;
    int32_t high = GET_JUMP_OFFSET(pc);
    int32_t length = high - low + 1;
    pc += JUMP_OFFSET_LEN;

    void** table = (void**) space->alloc(sizeof(void*) * length);
    if (!table) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    jsbytecode* defaultpc = pc_ + GET_JUMP_OFFSET(pc_);

    for (int32_t i = 0; i < length; i++) {
        int32_t off = GET_JUMP_OFFSET(pc);
        if (off)
            table[i] = pc_ + off;
        else
            table[i] = defaultpc;
        pc += JUMP_OFFSET_LEN;
    }

    return newStub<ICTableSwitch>(space, code, table, low, length, defaultpc);
}

void
ICTableSwitch::fixupJumpTable(JSScript* script, BaselineScript* baseline)
{
    defaultTarget_ = baseline->nativeCodeForPC(script, (jsbytecode*) defaultTarget_);

    for (int32_t i = 0; i < length_; i++)
        table_[i] = baseline->nativeCodeForPC(script, (jsbytecode*) table_[i]);
}

//
// GetIterator_Fallback
//

static bool
DoGetIteratorFallback(JSContext* cx, BaselineFrame* frame, ICGetIterator_Fallback* stub,
                      HandleValue value, MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "GetIterator");

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        RootedScript script(cx, frame->script());
        jsbytecode* pc = stub->icEntry()->pc(script);

        ICStubEngine engine = ICStubEngine::Baseline;
        GetIteratorIRGenerator gen(cx, script, pc, stub->state().mode(), value);
        bool attached = false;
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        engine, script, stub, &attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    JSObject* iterobj = ValueToIterator(cx, value);
    if (!iterobj)
        return false;

    res.setObject(*iterobj);
    return true;
}

typedef bool (*DoGetIteratorFallbackFn)(JSContext*, BaselineFrame*, ICGetIterator_Fallback*,
                                        HandleValue, MutableHandleValue);
static const VMFunction DoGetIteratorFallbackInfo =
    FunctionInfo<DoGetIteratorFallbackFn>(DoGetIteratorFallback, "DoGetIteratorFallback",
                                          TailCall, PopValues(1));

bool
ICGetIterator_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    // Sync stack for the decompiler.
    masm.pushValue(R0);

    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoGetIteratorFallbackInfo, masm);
}

//
// IteratorMore_Fallback
//

static bool
DoIteratorMoreFallback(JSContext* cx, BaselineFrame* frame, ICIteratorMore_Fallback* stub_,
                       HandleObject iterObj, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICIteratorMore_Fallback*> stub(frame, stub_);

    FallbackICSpew(cx, stub, "IteratorMore");

    if (!IteratorMore(cx, iterObj, res))
        return false;

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (!res.isMagic(JS_NO_ITER_VALUE) && !res.isString())
        stub->setHasNonStringResult();

    if (iterObj->is<PropertyIteratorObject>() &&
        !stub->hasStub(ICStub::IteratorMore_Native))
    {
        ICIteratorMore_Native::Compiler compiler(cx);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!newStub)
            return false;
        stub->addNewStub(newStub);
    }

    return true;
}

typedef bool (*DoIteratorMoreFallbackFn)(JSContext*, BaselineFrame*, ICIteratorMore_Fallback*,
                                         HandleObject, MutableHandleValue);
static const VMFunction DoIteratorMoreFallbackInfo =
    FunctionInfo<DoIteratorMoreFallbackFn>(DoIteratorMoreFallback, "DoIteratorMoreFallback",
                                           TailCall);

bool
ICIteratorMore_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.unboxObject(R0, R0.scratchReg());
    masm.push(R0.scratchReg());
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoIteratorMoreFallbackInfo, masm);
}

//
// IteratorMore_Native
//

bool
ICIteratorMore_Native::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;

    Register obj = masm.extractObject(R0, ExtractTemp0);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(1));
    Register nativeIterator = regs.takeAny();
    Register scratch = regs.takeAny();

    masm.branchTestObjClass(Assembler::NotEqual, obj, &PropertyIteratorObject::class_, scratch,
                            obj, &failure);
    masm.loadObjPrivate(obj, JSObject::ITER_CLASS_NFIXED_SLOTS, nativeIterator);

    // If props_cursor < props_end, load the next string and advance the cursor.
    // Else, return MagicValue(JS_NO_ITER_VALUE).
    Label iterDone;
    Address cursorAddr(nativeIterator, offsetof(NativeIterator, props_cursor));
    Address cursorEndAddr(nativeIterator, offsetof(NativeIterator, props_end));
    masm.loadPtr(cursorAddr, scratch);
    masm.branchPtr(Assembler::BelowOrEqual, cursorEndAddr, scratch, &iterDone);

    // Get next string.
    masm.loadPtr(Address(scratch, 0), scratch);

    // Increase the cursor.
    masm.addPtr(Imm32(sizeof(JSString*)), cursorAddr);

    masm.tagValue(JSVAL_TYPE_STRING, scratch, R0);
    EmitReturnFromIC(masm);

    masm.bind(&iterDone);
    masm.moveValue(MagicValue(JS_NO_ITER_VALUE), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// IteratorClose_Fallback
//

static void
DoIteratorCloseFallback(JSContext* cx, ICIteratorClose_Fallback* stub, HandleValue iterValue)
{
    FallbackICSpew(cx, stub, "IteratorClose");

    CloseIterator(&iterValue.toObject());
}

typedef void (*DoIteratorCloseFallbackFn)(JSContext*, ICIteratorClose_Fallback*, HandleValue);
static const VMFunction DoIteratorCloseFallbackInfo =
    FunctionInfo<DoIteratorCloseFallbackFn>(DoIteratorCloseFallback, "DoIteratorCloseFallback",
                                            TailCall);

bool
ICIteratorClose_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.pushValue(R0);
    masm.push(ICStubReg);

    return tailCallVM(DoIteratorCloseFallbackInfo, masm);
}

//
// InstanceOf_Fallback
//

static bool
TryAttachInstanceOfStub(JSContext* cx, BaselineFrame* frame, ICInstanceOf_Fallback* stub,
                        HandleValue lhs, HandleObject rhs, bool* attached)
{
    MOZ_ASSERT(!*attached);
    FallbackICSpew(cx, stub, "InstanceOf");

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        RootedScript script(cx, frame->script());
        jsbytecode* pc = stub->icEntry()->pc(script);

        ICStubEngine engine = ICStubEngine::Baseline;
        InstanceOfIRGenerator gen(cx, script, pc, stub->state().mode(),
                                  lhs,
                                  rhs);

        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        engine, script, stub, attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached InstanceOf CacheIR stub, attached is now %d", *attached);
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    return true;
}

static bool
DoInstanceOfFallback(JSContext* cx, BaselineFrame* frame, ICInstanceOf_Fallback* stub_,
                     HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICInstanceOf_Fallback*> stub(ICStubEngine::Baseline, frame, stub_);

    FallbackICSpew(cx, stub, "InstanceOf");

    if (!rhs.isObject()) {
        ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS, -1, rhs, nullptr);
        return false;
    }

    RootedObject obj(cx, &rhs.toObject());
    bool cond = false;
    if (!HasInstance(cx, obj, lhs, &cond))
        return false;

    res.setBoolean(cond);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (!obj->is<JSFunction>()) {
        stub->noteUnoptimizableAccess();
        return true;
    }

    // For functions, keep track of the |prototype| property in type information,
    // for use during Ion compilation.
    EnsureTrackPropertyTypes(cx, obj, NameToId(cx->names().prototype));

    bool attached = false;
    if (!TryAttachInstanceOfStub(cx, frame, stub, lhs, obj, &attached))
        return false;
    if (!attached)
        stub->noteUnoptimizableAccess();
    return true;
}

typedef bool (*DoInstanceOfFallbackFn)(JSContext*, BaselineFrame*, ICInstanceOf_Fallback*,
                                       HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoInstanceOfFallbackInfo =
    FunctionInfo<DoInstanceOfFallbackFn>(DoInstanceOfFallback, "DoInstanceOfFallback", TailCall,
                                         PopValues(2));

bool
ICInstanceOf_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    // Sync stack for the decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoInstanceOfFallbackInfo, masm);
}

//
// TypeOf_Fallback
//

static bool
DoTypeOfFallback(JSContext* cx, BaselineFrame* frame, ICTypeOf_Fallback* stub, HandleValue val,
                 MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "TypeOf");

    if (stub->state().maybeTransition())
        stub->discardStubs(cx);

    if (stub->state().canAttachStub()) {
        RootedScript script(cx, frame->script());
        jsbytecode* pc = stub->icEntry()->pc(script);

        ICStubEngine engine = ICStubEngine::Baseline;
        TypeOfIRGenerator gen(cx, script, pc, stub->state().mode(), val);
        bool attached = false;
        if (gen.tryAttachStub()) {
            ICStub* newStub = AttachBaselineCacheIRStub(cx, gen.writerRef(), gen.cacheKind(),
                                                        BaselineCacheIRStubKind::Regular,
                                                        engine, script, stub, &attached);
            if (newStub)
                JitSpew(JitSpew_BaselineIC, "  Attached CacheIR stub");
        }
        if (!attached)
            stub->state().trackNotAttached();
    }

    JSType type = js::TypeOfValue(val);
    RootedString string(cx, TypeName(type, cx->names()));
    res.setString(string);
    return true;
}

typedef bool (*DoTypeOfFallbackFn)(JSContext*, BaselineFrame* frame, ICTypeOf_Fallback*,
                                   HandleValue, MutableHandleValue);
static const VMFunction DoTypeOfFallbackInfo =
    FunctionInfo<DoTypeOfFallbackFn>(DoTypeOfFallback, "DoTypeOfFallback", TailCall);

bool
ICTypeOf_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoTypeOfFallbackInfo, masm);
}

static bool
DoRetSubFallback(JSContext* cx, BaselineFrame* frame, ICRetSub_Fallback* stub,
                 HandleValue val, uint8_t** resumeAddr)
{
    FallbackICSpew(cx, stub, "RetSub");

    // |val| is the bytecode offset where we should resume.

    MOZ_ASSERT(val.isInt32());
    MOZ_ASSERT(val.toInt32() >= 0);

    JSScript* script = frame->script();
    uint32_t offset = uint32_t(val.toInt32());

    *resumeAddr = script->baselineScript()->nativeCodeForPC(script, script->offsetToPC(offset));

    if (stub->numOptimizedStubs() >= ICRetSub_Fallback::MAX_OPTIMIZED_STUBS)
        return true;

    // Attach an optimized stub for this pc offset.
    JitSpew(JitSpew_BaselineIC, "  Generating RetSub stub for pc offset %u", offset);
    ICRetSub_Resume::Compiler compiler(cx, offset, *resumeAddr);
    ICStub* optStub = compiler.getStub(compiler.getStubSpace(script));
    if (!optStub)
        return false;

    stub->addNewStub(optStub);
    return true;
}

typedef bool(*DoRetSubFallbackFn)(JSContext* cx, BaselineFrame*, ICRetSub_Fallback*,
                                  HandleValue, uint8_t**);
static const VMFunction DoRetSubFallbackInfo =
    FunctionInfo<DoRetSubFallbackFn>(DoRetSubFallback, "DoRetSubFallback");

typedef bool (*ThrowFn)(JSContext*, HandleValue);
static const VMFunction ThrowInfoBaseline =
    FunctionInfo<ThrowFn>(js::Throw, "ThrowInfoBaseline", TailCall);

bool
ICRetSub_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // If R0 is BooleanValue(true), rethrow R1.
    Label rethrow;
    masm.branchTestBooleanTruthy(true, R0, &rethrow);
    {
        // Call a stub to get the native code address for the pc offset in R1.
        AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
        regs.take(R1);
        regs.takeUnchecked(ICTailCallReg);
        Register scratch = regs.getAny();

        enterStubFrame(masm, scratch);

        masm.pushValue(R1);
        masm.push(ICStubReg);
        pushStubPayload(masm, scratch);

        if (!callVM(DoRetSubFallbackInfo, masm))
            return false;

        leaveStubFrame(masm);

        EmitChangeICReturnAddress(masm, ReturnReg);
        EmitReturnFromIC(masm);
    }

    masm.bind(&rethrow);
    EmitRestoreTailCallReg(masm);
    masm.pushValue(R1);
    return tailCallVM(ThrowInfoBaseline, masm);
}

bool
ICRetSub_Resume::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // If R0 is BooleanValue(true), rethrow R1.
    Label fail, rethrow;
    masm.branchTestBooleanTruthy(true, R0, &rethrow);

    // R1 is the pc offset. Ensure it matches this stub's offset.
    Register offset = masm.extractInt32(R1, ExtractTemp0);
    masm.branch32(Assembler::NotEqual,
                  Address(ICStubReg, ICRetSub_Resume::offsetOfPCOffset()),
                  offset,
                  &fail);

    // pc offset matches, resume at the target pc.
    masm.loadPtr(Address(ICStubReg, ICRetSub_Resume::offsetOfAddr()), R0.scratchReg());
    EmitChangeICReturnAddress(masm, R0.scratchReg());
    EmitReturnFromIC(masm);

    // Rethrow the Value stored in R1.
    masm.bind(&rethrow);
    EmitRestoreTailCallReg(masm);
    masm.pushValue(R1);
    if (!tailCallVM(ThrowInfoBaseline, masm))
        return false;

    masm.bind(&fail);
    EmitStubGuardFailure(masm);
    return true;
}

ICTypeMonitor_SingleObject::ICTypeMonitor_SingleObject(JitCode* stubCode, JSObject* obj)
  : ICStub(TypeMonitor_SingleObject, stubCode),
    obj_(obj)
{ }

ICTypeMonitor_ObjectGroup::ICTypeMonitor_ObjectGroup(JitCode* stubCode, ObjectGroup* group)
  : ICStub(TypeMonitor_ObjectGroup, stubCode),
    group_(group)
{ }

ICTypeUpdate_SingleObject::ICTypeUpdate_SingleObject(JitCode* stubCode, JSObject* obj)
  : ICStub(TypeUpdate_SingleObject, stubCode),
    obj_(obj)
{ }

ICTypeUpdate_ObjectGroup::ICTypeUpdate_ObjectGroup(JitCode* stubCode, ObjectGroup* group)
  : ICStub(TypeUpdate_ObjectGroup, stubCode),
    group_(group)
{ }

ICCall_Scripted::ICCall_Scripted(JitCode* stubCode, ICStub* firstMonitorStub,
                                 JSFunction* callee, JSObject* templateObject,
                                 uint32_t pcOffset)
  : ICMonitoredStub(ICStub::Call_Scripted, stubCode, firstMonitorStub),
    callee_(callee),
    templateObject_(templateObject),
    pcOffset_(pcOffset)
{ }

/* static */ ICCall_Scripted*
ICCall_Scripted::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                       ICCall_Scripted& other)
{
    return New<ICCall_Scripted>(cx, space, other.jitCode(), firstMonitorStub, other.callee_,
                                other.templateObject_, other.pcOffset_);
}

/* static */ ICCall_AnyScripted*
ICCall_AnyScripted::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                          ICCall_AnyScripted& other)
{
    return New<ICCall_AnyScripted>(cx, space, other.jitCode(), firstMonitorStub, other.pcOffset_);
}

ICCall_Native::ICCall_Native(JitCode* stubCode, ICStub* firstMonitorStub,
                             JSFunction* callee, JSObject* templateObject,
                             uint32_t pcOffset)
  : ICMonitoredStub(ICStub::Call_Native, stubCode, firstMonitorStub),
    callee_(callee),
    templateObject_(templateObject),
    pcOffset_(pcOffset)
{
#ifdef JS_SIMULATOR
    // The simulator requires VM calls to be redirected to a special swi
    // instruction to handle them. To make this work, we store the redirected
    // pointer in the stub.
    native_ = Simulator::RedirectNativeFunction(JS_FUNC_TO_DATA_PTR(void*, callee->native()),
                                                Args_General3);
#endif
}

/* static */ ICCall_Native*
ICCall_Native::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                     ICCall_Native& other)
{
    return New<ICCall_Native>(cx, space, other.jitCode(), firstMonitorStub, other.callee_,
                              other.templateObject_, other.pcOffset_);
}

ICCall_ClassHook::ICCall_ClassHook(JitCode* stubCode, ICStub* firstMonitorStub,
                                   const Class* clasp, Native native,
                                   JSObject* templateObject, uint32_t pcOffset)
  : ICMonitoredStub(ICStub::Call_ClassHook, stubCode, firstMonitorStub),
    clasp_(clasp),
    native_(JS_FUNC_TO_DATA_PTR(void*, native)),
    templateObject_(templateObject),
    pcOffset_(pcOffset)
{
#ifdef JS_SIMULATOR
    // The simulator requires VM calls to be redirected to a special swi
    // instruction to handle them. To make this work, we store the redirected
    // pointer in the stub.
    native_ = Simulator::RedirectNativeFunction(native_, Args_General3);
#endif
}

/* static */ ICCall_ClassHook*
ICCall_ClassHook::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                        ICCall_ClassHook& other)
{
    ICCall_ClassHook* res = New<ICCall_ClassHook>(cx, space, other.jitCode(), firstMonitorStub,
                                                  other.clasp(), nullptr, other.templateObject_,
                                                  other.pcOffset_);
    if (res)
        res->native_ = other.native();
    return res;
}

/* static */ ICCall_ScriptedApplyArray*
ICCall_ScriptedApplyArray::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                                 ICCall_ScriptedApplyArray& other)
{
    return New<ICCall_ScriptedApplyArray>(cx, space, other.jitCode(), firstMonitorStub,
                                          other.pcOffset_);
}

/* static */ ICCall_ScriptedApplyArguments*
ICCall_ScriptedApplyArguments::Clone(JSContext* cx,
                                     ICStubSpace* space,
                                     ICStub* firstMonitorStub,
                                     ICCall_ScriptedApplyArguments& other)
{
    return New<ICCall_ScriptedApplyArguments>(cx, space, other.jitCode(), firstMonitorStub,
                                              other.pcOffset_);
}

/* static */ ICCall_ScriptedFunCall*
ICCall_ScriptedFunCall::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                              ICCall_ScriptedFunCall& other)
{
    return New<ICCall_ScriptedFunCall>(cx, space, other.jitCode(), firstMonitorStub,
                                       other.pcOffset_);
}

//
// Rest_Fallback
//

static bool
DoRestFallback(JSContext* cx, BaselineFrame* frame, ICRest_Fallback* stub,
               MutableHandleValue res)
{
    unsigned numFormals = frame->numFormalArgs() - 1;
    unsigned numActuals = frame->numActualArgs();
    unsigned numRest = numActuals > numFormals ? numActuals - numFormals : 0;
    Value* rest = frame->argv() + numFormals;

    ArrayObject* obj = ObjectGroup::newArrayObject(cx, rest, numRest, GenericObject,
                                                   ObjectGroup::NewArrayKind::UnknownIndex);
    if (!obj)
        return false;
    res.setObject(*obj);
    return true;
}

typedef bool (*DoRestFallbackFn)(JSContext*, BaselineFrame*, ICRest_Fallback*,
                                 MutableHandleValue);
static const VMFunction DoRestFallbackInfo =
    FunctionInfo<DoRestFallbackFn>(DoRestFallback, "DoRestFallback", TailCall);

bool
ICRest_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.push(ICStubReg);
    pushStubPayload(masm, R0.scratchReg());

    return tailCallVM(DoRestFallbackInfo, masm);
}

} // namespace jit
} // namespace js
