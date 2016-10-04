/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineIC.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/SizePrintfMacros.h"
#include "mozilla/TemplateLib.h"

#include "jslibmath.h"
#include "jstypes.h"

#include "builtin/Eval.h"
#include "builtin/SIMD.h"
#include "jit/BaselineDebugModeOSR.h"
#include "jit/BaselineJIT.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/Lowering.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/SharedICHelpers.h"
#include "jit/VMFunctions.h"
#include "js/Conversions.h"
#include "js/TraceableVector.h"
#include "vm/Opcodes.h"
#include "vm/TypedArrayCommon.h"

#include "jsboolinlines.h"
#include "jsscriptinlines.h"

#include "jit/JitFrames-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/ScopeObject-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/UnboxedObject-inl.h"

using mozilla::DebugOnly;

namespace js {
namespace jit {

//
// WarmUpCounter_Fallback
//

static bool
EnsureCanEnterIon(JSContext* cx, ICWarmUpCounter_Fallback* stub, BaselineFrame* frame,
                  HandleScript script, jsbytecode* pc, void** jitcodePtr)
{
    MOZ_ASSERT(jitcodePtr);
    MOZ_ASSERT(!*jitcodePtr);

    bool isLoopEntry = (JSOp(*pc) == JSOP_LOOPENTRY);

    MethodStatus stat;
    if (isLoopEntry) {
        MOZ_ASSERT(LoopEntryCanIonOsr(pc));
        JitSpew(JitSpew_BaselineOSR, "  Compile at loop entry!");
        stat = CanEnterAtBranch(cx, script, frame, pc);
    } else if (frame->isFunctionFrame()) {
        JitSpew(JitSpew_BaselineOSR, "  Compile function from top for later entry!");
        stat = CompileFunctionForBaseline(cx, script, frame);
    } else {
        return true;
    }

    if (stat == Method_Error) {
        JitSpew(JitSpew_BaselineOSR, "  Compile with Ion errored!");
        return false;
    }

    if (stat == Method_CantCompile)
        JitSpew(JitSpew_BaselineOSR, "  Can't compile with Ion!");
    else if (stat == Method_Skipped)
        JitSpew(JitSpew_BaselineOSR, "  Skipped compile with Ion!");
    else if (stat == Method_Compiled)
        JitSpew(JitSpew_BaselineOSR, "  Compiled with Ion!");
    else
        MOZ_CRASH("Invalid MethodStatus!");

    // Failed to compile.  Reset warm-up counter and return.
    if (stat != Method_Compiled) {
        // TODO: If stat == Method_CantCompile, insert stub that just skips the
        // warm-up counter entirely, instead of resetting it.
        bool bailoutExpected = script->hasIonScript() && script->ionScript()->bailoutExpected();
        if (stat == Method_CantCompile || bailoutExpected) {
            JitSpew(JitSpew_BaselineOSR, "  Reset WarmUpCounter cantCompile=%s bailoutExpected=%s!",
                    stat == Method_CantCompile ? "yes" : "no",
                    bailoutExpected ? "yes" : "no");
            script->resetWarmUpCounter();
        }
        return true;
    }

    if (isLoopEntry) {
        IonScript* ion = script->ionScript();
        MOZ_ASSERT(cx->runtime()->spsProfiler.enabled() == ion->hasProfilingInstrumentation());
        MOZ_ASSERT(ion->osrPc() == pc);

        JitSpew(JitSpew_BaselineOSR, "  OSR possible!");
        *jitcodePtr = ion->method()->raw() + ion->osrEntryOffset();
    }

    return true;
}

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
PrepareOsrTempData(JSContext* cx, ICWarmUpCounter_Fallback* stub, BaselineFrame* frame,
                   HandleScript script, jsbytecode* pc, void* jitcode)
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

    IonOsrTempData* info = (IonOsrTempData*)cx->runtime()->getJitRuntime(cx)->allocateOsrTempData(totalSpace);
    if (!info)
        return nullptr;

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
DoWarmUpCounterFallback(JSContext* cx, BaselineFrame* frame, ICWarmUpCounter_Fallback* stub,
                        IonOsrTempData** infoPtr)
{
    MOZ_ASSERT(infoPtr);
    *infoPtr = nullptr;

    // A TI OOM will disable TI and Ion.
    if (!jit::IsIonEnabled(cx))
        return true;

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(script);
    bool isLoopEntry = JSOp(*pc) == JSOP_LOOPENTRY;

    MOZ_ASSERT(!isLoopEntry || LoopEntryCanIonOsr(pc));

    FallbackICSpew(cx, stub, "WarmUpCounter(%d)", isLoopEntry ? int(script->pcToOffset(pc)) : int(-1));

    if (!script->canIonCompile()) {
        // TODO: ASSERT that ion-compilation-disabled checker stub doesn't exist.
        // TODO: Clear all optimized stubs.
        // TODO: Add a ion-compilation-disabled checker IC stub
        script->resetWarmUpCounter();
        return true;
    }

    MOZ_ASSERT(!script->isIonCompilingOffThread());

    // If Ion script exists, but PC is not at a loop entry, then Ion will be entered for
    // this script at an appropriate LOOPENTRY or the next time this function is called.
    if (script->hasIonScript() && !isLoopEntry) {
        JitSpew(JitSpew_BaselineOSR, "IonScript exists, but not at loop entry!");
        // TODO: ASSERT that a ion-script-already-exists checker stub doesn't exist.
        // TODO: Clear all optimized stubs.
        // TODO: Add a ion-script-already-exists checker stub.
        return true;
    }

    // Ensure that Ion-compiled code is available.
    JitSpew(JitSpew_BaselineOSR,
            "WarmUpCounter for %s:%" PRIuSIZE " reached %d at pc %p, trying to switch to Ion!",
            script->filename(), script->lineno(), (int) script->getWarmUpCount(), (void*) pc);
    void* jitcode = nullptr;
    if (!EnsureCanEnterIon(cx, stub, frame, script, pc, &jitcode))
        return false;

    // Jitcode should only be set here if not at loop entry.
    MOZ_ASSERT_IF(!isLoopEntry, !jitcode);
    if (!jitcode)
        return true;

    // Prepare the temporary heap copy of the fake InterpreterFrame and actual args list.
    JitSpew(JitSpew_BaselineOSR, "Got jitcode.  Preparing for OSR into ion.");
    IonOsrTempData* info = PrepareOsrTempData(cx, stub, frame, script, pc, jitcode);
    if (!info)
        return false;
    *infoPtr = info;

    return true;
}

typedef bool (*DoWarmUpCounterFallbackFn)(JSContext*, BaselineFrame*,
                                          ICWarmUpCounter_Fallback*, IonOsrTempData** infoPtr);
static const VMFunction DoWarmUpCounterFallbackInfo =
    FunctionInfo<DoWarmUpCounterFallbackFn>(DoWarmUpCounterFallback);

bool
ICWarmUpCounter_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, R1.scratchReg());

    Label noCompiledCode;
    // Call DoWarmUpCounterFallback to compile/check-for Ion-compiled function
    {
        // Push IonOsrTempData pointer storage
        masm.subFromStackPtr(Imm32(sizeof(void*)));
        masm.push(masm.getStackPointer());

        // Push stub pointer.
        masm.push(ICStubReg);

        pushFramePtr(masm, R0.scratchReg());

        if (!callVM(DoWarmUpCounterFallbackInfo, masm))
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
        AbsoluteAddress addressOfEnabled(cx->runtime()->spsProfiler.addressOfEnabled());
        masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &checkOk);
        masm.loadPtr(AbsoluteAddress((void*)&cx->runtime()->jitActivation), scratchReg);
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
    FallbackICSpew(cx, stub->getChainFallback(), "TypeUpdate(%s)",
                   ICStub::KindString(stub->kind()));

    RootedScript script(cx, frame->script());
    RootedObject obj(cx, &objval.toObject());
    RootedId id(cx);

    switch(stub->kind()) {
      case ICStub::SetElem_DenseOrUnboxedArray:
      case ICStub::SetElem_DenseOrUnboxedArrayAdd: {
        id = JSID_VOID;
        AddTypePropertyId(cx, obj, id, value);
        break;
      }
      case ICStub::SetProp_Native:
      case ICStub::SetProp_NativeAdd:
      case ICStub::SetProp_Unboxed: {
        MOZ_ASSERT(obj->isNative() || obj->is<UnboxedPlainObject>());
        jsbytecode* pc = stub->getChainFallback()->icEntry()->pc(script);
        if (*pc == JSOP_SETALIASEDVAR || *pc == JSOP_INITALIASEDLEXICAL)
            id = NameToId(ScopeCoordinateName(cx->runtime()->scopeCoordinateNameCache, script, pc));
        else
            id = NameToId(script->getName(pc));
        AddTypePropertyId(cx, obj, id, value);
        break;
      }
      case ICStub::SetProp_TypedObject: {
        MOZ_ASSERT(obj->is<TypedObject>());
        jsbytecode* pc = stub->getChainFallback()->icEntry()->pc(script);
        id = NameToId(script->getName(pc));
        if (stub->toSetProp_TypedObject()->isObjectReference()) {
            // Ignore all values being written except plain objects. Null
            // is included implicitly in type information for this property,
            // and non-object non-null values will cause the stub to fail to
            // match shortly and we will end up doing the assignment in the VM.
            if (value.isObject())
                AddTypePropertyId(cx, obj, id, value);
        } else {
            // Ignore undefined values, which are included implicitly in type
            // information for this property.
            if (!value.isUndefined())
                AddTypePropertyId(cx, obj, id, value);
        }
        break;
      }
      default:
        MOZ_CRASH("Invalid stub");
    }

    return stub->addUpdateStubForValue(cx, script, obj, id, value);
}

typedef bool (*DoTypeUpdateFallbackFn)(JSContext*, BaselineFrame*, ICUpdatedStub*, HandleValue,
                                       HandleValue);
const VMFunction DoTypeUpdateFallbackInfo =
    FunctionInfo<DoTypeUpdateFallbackFn>(DoTypeUpdateFallback, NonTailCall);

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

    // Currently, we will never generate primitive stub checks for object.  However,
    // when we do get to the point where we want to collapse our monitor chains of
    // objects and singletons down (when they get too long) to a generic "any object"
    // in coordination with the typeset doing the same thing, this will need to
    // be re-enabled.
    /*
    if (flags_ & TypeToFlag(JSVAL_TYPE_OBJECT))
        masm.branchTestObject(Assembler::Equal, R0, &success);
    */
    MOZ_ASSERT(!(flags_ & TypeToFlag(JSVAL_TYPE_OBJECT)));

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
    Register obj = masm.extractObject(R0, R1.scratchReg());
    masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), R1.scratchReg());

    Address expectedGroup(ICStubReg, ICTypeUpdate_ObjectGroup::offsetOfGroup());
    masm.branchPtr(Assembler::NotEqual, expectedGroup, R1.scratchReg(), &failure);

    // Group matches, load true into R1.scratchReg() and return.
    masm.mov(ImmWord(1), R1.scratchReg());
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

typedef bool (*DoCallNativeGetterFn)(JSContext*, HandleFunction, HandleObject, MutableHandleValue);
static const VMFunction DoCallNativeGetterInfo =
    FunctionInfo<DoCallNativeGetterFn>(DoCallNativeGetter);

//
// NewArray_Fallback
//

static bool
DoNewArray(JSContext* cx, BaselineFrame* frame, ICNewArray_Fallback* stub, uint32_t length,
           MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "NewArray");

    RootedObject obj(cx);
    if (stub->templateObject()) {
        RootedObject templateObject(cx, stub->templateObject());
        obj = NewArrayOperationWithTemplate(cx, templateObject);
        if (!obj)
            return false;
    } else {
        RootedScript script(cx, frame->script());
        jsbytecode* pc = stub->icEntry()->pc(script);
        obj = NewArrayOperation(cx, script, pc, length);
        if (!obj)
            return false;

        if (obj && !obj->isSingleton() && !obj->group()->maybePreliminaryObjects()) {
            JSObject* templateObject = NewArrayOperation(cx, script, pc, length, TenuredObject);
            if (!templateObject)
                return false;
            stub->setTemplateObject(templateObject);
        }
    }

    res.setObject(*obj);
    return true;
}

typedef bool(*DoNewArrayFn)(JSContext*, BaselineFrame*, ICNewArray_Fallback*, uint32_t,
                            MutableHandleValue);
static const VMFunction DoNewArrayInfo = FunctionInfo<DoNewArrayFn>(DoNewArray, TailCall);

bool
ICNewArray_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.push(R0.scratchReg()); // length
    masm.push(ICStubReg); // stub.
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoNewArrayInfo, masm);
}

//
// NewObject_Fallback
//

// Unlike typical baseline IC stubs, the code for NewObject_WithTemplate is
// specialized for the template object being allocated.
static JitCode*
GenerateNewObjectWithTemplateCode(JSContext* cx, JSObject* templateObject)
{
    JitContext jctx(cx, nullptr);
    MacroAssembler masm;
#ifdef JS_CODEGEN_ARM
    masm.setSecondScratchReg(BaselineSecondScratchReg);
#endif

    Label failure;
    Register objReg = R0.scratchReg();
    Register tempReg = R1.scratchReg();
    masm.movePtr(ImmGCPtr(templateObject->group()), tempReg);
    masm.branchTest32(Assembler::NonZero, Address(tempReg, ObjectGroup::offsetOfFlags()),
                      Imm32(OBJECT_FLAG_PRE_TENURE), &failure);
    masm.branchPtr(Assembler::NotEqual, AbsoluteAddress(cx->compartment()->addressOfMetadataCallback()),
                   ImmWord(0), &failure);
    masm.createGCObject(objReg, tempReg, templateObject, gc::DefaultHeap, &failure);
    masm.tagValue(JSVAL_TYPE_OBJECT, objReg, R0);

    EmitReturnFromIC(masm);
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    Linker linker(masm);
    AutoFlushICache afc("GenerateNewObjectWithTemplateCode");
    return linker.newCode<CanGC>(cx, BASELINE_CODE);
}

static bool
DoNewObject(JSContext* cx, BaselineFrame* frame, ICNewObject_Fallback* stub, MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "NewObject");

    RootedObject obj(cx);

    RootedObject templateObject(cx, stub->templateObject());
    if (templateObject) {
        MOZ_ASSERT(!templateObject->group()->maybePreliminaryObjects());
        obj = NewObjectOperationWithTemplate(cx, templateObject);
    } else {
        RootedScript script(cx, frame->script());
        jsbytecode* pc = stub->icEntry()->pc(script);
        obj = NewObjectOperation(cx, script, pc);

        if (obj && !obj->isSingleton() && !obj->group()->maybePreliminaryObjects()) {
            JSObject* templateObject = NewObjectOperation(cx, script, pc, TenuredObject);
            if (!templateObject)
                return false;

            if (templateObject->is<UnboxedPlainObject>() ||
                !templateObject->as<PlainObject>().hasDynamicSlots())
            {
                JitCode* code = GenerateNewObjectWithTemplateCode(cx, templateObject);
                if (!code)
                    return false;

                ICStubSpace* space =
                    ICStubCompiler::StubSpaceForKind(ICStub::NewObject_WithTemplate, script);
                ICStub* templateStub = ICStub::New<ICNewObject_WithTemplate>(cx, space, code);
                if (!templateStub)
                    return false;

                stub->addNewStub(templateStub);
            }

            stub->setTemplateObject(templateObject);
        }
    }

    if (!obj)
        return false;

    res.setObject(*obj);
    return true;
}

typedef bool(*DoNewObjectFn)(JSContext*, BaselineFrame*, ICNewObject_Fallback*, MutableHandleValue);
static const VMFunction DoNewObjectInfo = FunctionInfo<DoNewObjectFn>(DoNewObject, TailCall);

bool
ICNewObject_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.push(ICStubReg); // stub.
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoNewObjectInfo, masm);
}

//
// ToBool_Fallback
//

static bool
DoToBoolFallback(JSContext* cx, BaselineFrame* frame, ICToBool_Fallback* stub, HandleValue arg,
                 MutableHandleValue ret)
{
    FallbackICSpew(cx, stub, "ToBool");

    bool cond = ToBoolean(arg);
    ret.setBoolean(cond);

    // Check to see if a new stub should be generated.
    if (stub->numOptimizedStubs() >= ICToBool_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    MOZ_ASSERT(!arg.isBoolean());

    JSScript* script = frame->script();

    // Try to generate new stubs.
    if (arg.isInt32()) {
        JitSpew(JitSpew_BaselineIC, "  Generating ToBool(Int32) stub.");
        ICToBool_Int32::Compiler compiler(cx);
        ICStub* int32Stub = compiler.getStub(compiler.getStubSpace(script));
        if (!int32Stub)
            return false;

        stub->addNewStub(int32Stub);
        return true;
    }

    if (arg.isDouble() && cx->runtime()->jitSupportsFloatingPoint) {
        JitSpew(JitSpew_BaselineIC, "  Generating ToBool(Double) stub.");
        ICToBool_Double::Compiler compiler(cx);
        ICStub* doubleStub = compiler.getStub(compiler.getStubSpace(script));
        if (!doubleStub)
            return false;

        stub->addNewStub(doubleStub);
        return true;
    }

    if (arg.isString()) {
        JitSpew(JitSpew_BaselineIC, "  Generating ToBool(String) stub");
        ICToBool_String::Compiler compiler(cx);
        ICStub* stringStub = compiler.getStub(compiler.getStubSpace(script));
        if (!stringStub)
            return false;

        stub->addNewStub(stringStub);
        return true;
    }

    if (arg.isNull() || arg.isUndefined()) {
        ICToBool_NullUndefined::Compiler compiler(cx);
        ICStub* nilStub = compiler.getStub(compiler.getStubSpace(script));
        if (!nilStub)
            return false;

        stub->addNewStub(nilStub);
        return true;
    }

    if (arg.isObject()) {
        JitSpew(JitSpew_BaselineIC, "  Generating ToBool(Object) stub.");
        ICToBool_Object::Compiler compiler(cx);
        ICStub* objStub = compiler.getStub(compiler.getStubSpace(script));
        if (!objStub)
            return false;

        stub->addNewStub(objStub);
        return true;
    }

    return true;
}

typedef bool (*pf)(JSContext*, BaselineFrame*, ICToBool_Fallback*, HandleValue,
                   MutableHandleValue);
static const VMFunction fun = FunctionInfo<pf>(DoToBoolFallback, TailCall);

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
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(fun, masm);
}

//
// ToBool_Int32
//

bool
ICToBool_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);

    Label ifFalse;
    masm.branchTestInt32Truthy(false, R0, &ifFalse);

    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_String
//

bool
ICToBool_String::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);

    Label ifFalse;
    masm.branchTestStringTruthy(false, R0, &ifFalse);

    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_NullUndefined
//

bool
ICToBool_NullUndefined::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure, ifFalse;
    masm.branchTestNull(Assembler::Equal, R0, &ifFalse);
    masm.branchTestUndefined(Assembler::NotEqual, R0, &failure);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_Double
//

bool
ICToBool_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure, ifTrue;
    masm.branchTestDouble(Assembler::NotEqual, R0, &failure);
    masm.unboxDouble(R0, FloatReg0);
    masm.branchTestDoubleTruthy(true, FloatReg0, &ifTrue);

    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifTrue);
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// ToBool_Object
//

bool
ICToBool_Object::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure, ifFalse, slowPath;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    Register objReg = masm.extractObject(R0, ExtractTemp0);
    Register scratch = R1.scratchReg();
    masm.branchTestObjectTruthy(false, objReg, scratch, &slowPath, &ifFalse);

    // If object doesn't emulate undefined, it evaulates to true.
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&slowPath);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(objReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::EmulatesUndefined));
    masm.convertBoolToInt32(ReturnReg, ReturnReg);
    masm.xor32(Imm32(1), ReturnReg);
    masm.tagValue(JSVAL_TYPE_BOOLEAN, ReturnReg, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
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
    FunctionInfo<DoToNumberFallbackFn>(DoToNumberFallback, TailCall, PopValues(1));

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

static Shape*
LastPropertyForSetProp(JSObject* obj)
{
    if (obj->isNative())
        return obj->as<NativeObject>().lastProperty();

    if (obj->is<UnboxedPlainObject>()) {
        UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando();
        return expando ? expando->lastProperty() : nullptr;
    }

    return nullptr;
}

static bool
IsCacheableSetPropWriteSlot(JSObject* obj, Shape* oldShape, Shape* propertyShape)
{
    // Object shape must not have changed during the property set.
    if (LastPropertyForSetProp(obj) != oldShape)
        return false;

    if (!propertyShape->hasSlot() ||
        !propertyShape->hasDefaultSetter() ||
        !propertyShape->writable())
    {
        return false;
    }

    return true;
}

static bool
IsCacheableSetPropAddSlot(JSContext* cx, JSObject* obj, Shape* oldShape,
                          jsid id, Shape* propertyShape, size_t* protoChainDepth)
{
    // The property must be the last added property of the object.
    if (LastPropertyForSetProp(obj) != propertyShape)
        return false;

    // Object must be extensible, oldShape must be immediate parent of current shape.
    if (!obj->nonProxyIsExtensible() || propertyShape->previous() != oldShape)
        return false;

    // Basic shape checks.
    if (propertyShape->inDictionary() ||
        !propertyShape->hasSlot() ||
        !propertyShape->hasDefaultSetter() ||
        !propertyShape->writable())
    {
        return false;
    }

    // Watch out for resolve or addProperty hooks.
    if (ClassMayResolveId(cx->names(), obj->getClass(), id, obj) ||
        obj->getClass()->addProperty)
    {
        return false;
    }

    size_t chainDepth = 0;
    // Walk up the object prototype chain and ensure that all prototypes are
    // native, and that all prototypes have no setter defined on the property.
    for (JSObject* proto = obj->getProto(); proto; proto = proto->getProto()) {
        chainDepth++;
        // if prototype is non-native, don't optimize
        if (!proto->isNative())
            return false;

        // if prototype defines this property in a non-plain way, don't optimize
        Shape* protoShape = proto->as<NativeObject>().lookup(cx, id);
        if (protoShape && !protoShape->hasDefaultSetter())
            return false;

        // Otherwise, if there's no such property, watch out for a resolve hook
        // that would need to be invoked and thus prevent inlining of property
        // addition.
        if (ClassMayResolveId(cx->names(), proto->getClass(), id, proto))
            return false;
    }

    // Only add a IC entry if the dynamic slots didn't change when the shapes
    // changed.  Need to ensure that a shape change for a subsequent object
    // won't involve reallocating the slot array.
    if (NativeObject::dynamicSlotsCount(propertyShape) != NativeObject::dynamicSlotsCount(oldShape))
        return false;

    *protoChainDepth = chainDepth;
    return true;
}

static bool
IsCacheableSetPropCall(JSContext* cx, JSObject* obj, JSObject* holder, Shape* shape,
                       bool* isScripted, bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(isScripted);

    if (!shape || !IsCacheableProtoChain(obj, holder))
        return false;

    if (shape->hasSlot() || shape->hasDefaultSetter())
        return false;

    if (!shape->hasSetterValue())
        return false;

    if (!shape->setterValue().isObject() || !shape->setterObject()->is<JSFunction>())
        return false;

    JSFunction* func = &shape->setterObject()->as<JSFunction>();

    if (func->isNative()) {
        *isScripted = false;
        return true;
    }

    if (!func->hasJITCode()) {
        *isTemporarilyUnoptimizable = true;
        return false;
    }

    *isScripted = true;
    return true;
}

template <class T>
static bool
GetElemNativeStubExists(ICGetElem_Fallback* stub, HandleObject obj, HandleObject holder,
                        Handle<T> key, bool needsAtomize)
{
    bool indirect = (obj.get() != holder.get());
    MOZ_ASSERT_IF(indirect, holder->isNative());

    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (iter->kind() != ICStub::GetElem_NativeSlotName &&
            iter->kind() != ICStub::GetElem_NativeSlotSymbol &&
            iter->kind() != ICStub::GetElem_NativePrototypeSlotName &&
            iter->kind() != ICStub::GetElem_NativePrototypeSlotSymbol &&
            iter->kind() != ICStub::GetElem_NativePrototypeCallNativeName &&
            iter->kind() != ICStub::GetElem_NativePrototypeCallNativeSymbol &&
            iter->kind() != ICStub::GetElem_NativePrototypeCallScriptedName &&
            iter->kind() != ICStub::GetElem_NativePrototypeCallScriptedSymbol)
        {
            continue;
        }

        if (indirect && (iter->kind() != ICStub::GetElem_NativePrototypeSlotName &&
                         iter->kind() != ICStub::GetElem_NativePrototypeSlotSymbol &&
                         iter->kind() != ICStub::GetElem_NativePrototypeCallNativeName &&
                         iter->kind() != ICStub::GetElem_NativePrototypeCallNativeSymbol &&
                         iter->kind() != ICStub::GetElem_NativePrototypeCallScriptedName &&
                         iter->kind() != ICStub::GetElem_NativePrototypeCallScriptedSymbol))
        {
            continue;
        }

        if(mozilla::IsSame<T, JS::Symbol*>::value !=
           static_cast<ICGetElemNativeStub*>(*iter)->isSymbol())
        {
            continue;
        }

        ICGetElemNativeStubImpl<T>* getElemNativeStub =
            reinterpret_cast<ICGetElemNativeStubImpl<T>*>(*iter);
        if (key != getElemNativeStub->key())
            continue;

        if (ReceiverGuard(obj) != getElemNativeStub->receiverGuard())
            continue;

        // If the new stub needs atomization, and the old stub doesn't atomize, then
        // an appropriate stub doesn't exist.
        if (needsAtomize && !getElemNativeStub->needsAtomize())
            continue;

        // For prototype gets, check the holder and holder shape.
        if (indirect) {
            if (iter->isGetElem_NativePrototypeSlotName() ||
                iter->isGetElem_NativePrototypeSlotSymbol()) {
                ICGetElem_NativePrototypeSlot<T>* protoStub =
                    reinterpret_cast<ICGetElem_NativePrototypeSlot<T>*>(*iter);

                if (holder != protoStub->holder())
                    continue;

                if (holder->as<NativeObject>().lastProperty() != protoStub->holderShape())
                    continue;
            } else {
                MOZ_ASSERT(iter->isGetElem_NativePrototypeCallNativeName() ||
                           iter->isGetElem_NativePrototypeCallNativeSymbol() ||
                           iter->isGetElem_NativePrototypeCallScriptedName() ||
                           iter->isGetElem_NativePrototypeCallScriptedSymbol());

                ICGetElemNativePrototypeCallStub<T>* protoStub =
                    reinterpret_cast<ICGetElemNativePrototypeCallStub<T>*>(*iter);

                if (holder != protoStub->holder())
                    continue;

                if (holder->as<NativeObject>().lastProperty() != protoStub->holderShape())
                    continue;
            }
        }

        return true;
    }
    return false;
}

template <class T>
static void
RemoveExistingGetElemNativeStubs(JSContext* cx, ICGetElem_Fallback* stub, HandleObject obj,
                                 HandleObject holder, Handle<T> key, bool needsAtomize)
{
    bool indirect = (obj.get() != holder.get());
    MOZ_ASSERT_IF(indirect, holder->isNative());

    for (ICStubIterator iter = stub->beginChain(); !iter.atEnd(); iter++) {
        switch (iter->kind()) {
          case ICStub::GetElem_NativeSlotName:
          case ICStub::GetElem_NativeSlotSymbol:
            if (indirect)
                continue;
          case ICStub::GetElem_NativePrototypeSlotName:
          case ICStub::GetElem_NativePrototypeSlotSymbol:
          case ICStub::GetElem_NativePrototypeCallNativeName:
          case ICStub::GetElem_NativePrototypeCallNativeSymbol:
          case ICStub::GetElem_NativePrototypeCallScriptedName:
          case ICStub::GetElem_NativePrototypeCallScriptedSymbol:
            break;
          default:
            continue;
        }

        if(mozilla::IsSame<T, JS::Symbol*>::value !=
           static_cast<ICGetElemNativeStub*>(*iter)->isSymbol())
        {
            continue;
        }

        ICGetElemNativeStubImpl<T>* getElemNativeStub =
            reinterpret_cast<ICGetElemNativeStubImpl<T>*>(*iter);
        if (key != getElemNativeStub->key())
            continue;

        if (ReceiverGuard(obj) != getElemNativeStub->receiverGuard())
            continue;

        // For prototype gets, check the holder and holder shape.
        if (indirect) {
            if (iter->isGetElem_NativePrototypeSlotName() ||
                iter->isGetElem_NativePrototypeSlotSymbol()) {
                ICGetElem_NativePrototypeSlot<T>* protoStub =
                    reinterpret_cast<ICGetElem_NativePrototypeSlot<T>*>(*iter);

                if (holder != protoStub->holder())
                    continue;

                // If the holder matches, but the holder's lastProperty doesn't match, then
                // this stub is invalid anyway.  Unlink it.
                if (holder->as<NativeObject>().lastProperty() != protoStub->holderShape()) {
                    iter.unlink(cx);
                    continue;
                }
            } else {
                MOZ_ASSERT(iter->isGetElem_NativePrototypeCallNativeName() ||
                           iter->isGetElem_NativePrototypeCallNativeSymbol() ||
                           iter->isGetElem_NativePrototypeCallScriptedName() ||
                           iter->isGetElem_NativePrototypeCallScriptedSymbol());
                ICGetElemNativePrototypeCallStub<T>* protoStub =
                    reinterpret_cast<ICGetElemNativePrototypeCallStub<T>*>(*iter);

                if (holder != protoStub->holder())
                    continue;

                // If the holder matches, but the holder's lastProperty doesn't match, then
                // this stub is invalid anyway.  Unlink it.
                if (holder->as<NativeObject>().lastProperty() != protoStub->holderShape()) {
                    iter.unlink(cx);
                    continue;
                }
            }
        }

        // If the new stub needs atomization, and the old stub doesn't atomize, then
        // remove the old stub.
        if (needsAtomize && !getElemNativeStub->needsAtomize()) {
            iter.unlink(cx);
            continue;
        }

        // Should never get here, because this means a matching stub exists, and if
        // a matching stub exists, this procedure should never have been called.
        MOZ_CRASH("Procedure should never have been called.");
    }
}

static bool
TypedArrayGetElemStubExists(ICGetElem_Fallback* stub, HandleObject obj)
{
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (!iter->isGetElem_TypedArray())
            continue;
        if (obj->maybeShape() == iter->toGetElem_TypedArray()->shape())
            return true;
    }
    return false;
}

static bool
ArgumentsGetElemStubExists(ICGetElem_Fallback* stub, ICGetElem_Arguments::Which which)
{
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (!iter->isGetElem_Arguments())
            continue;
        if (iter->toGetElem_Arguments()->which() == which)
            return true;
    }
    return false;
}

template <class T>
static T
getKey(jsid id)
{
    MOZ_ASSERT_UNREACHABLE("Key has to be PropertyName or Symbol");
    return false;
}

template <>
JS::Symbol* getKey<JS::Symbol*>(jsid id)
{
    if (!JSID_IS_SYMBOL(id))
        return nullptr;
    return JSID_TO_SYMBOL(id);
}

template <>
PropertyName* getKey<PropertyName*>(jsid id)
{
    uint32_t dummy;
    if (!JSID_IS_ATOM(id) || JSID_TO_ATOM(id)->isIndex(&dummy))
        return nullptr;
    return JSID_TO_ATOM(id)->asPropertyName();
}

static bool
IsOptimizableElementPropertyName(JSContext* cx, HandleValue key, MutableHandleId idp)
{
    if (!key.isString())
        return false;

    // Convert to interned property name.
    if (!ValueToId<CanGC>(cx, key, idp))
        return false;

    uint32_t dummy;
    if (!JSID_IS_ATOM(idp) || JSID_TO_ATOM(idp)->isIndex(&dummy))
        return false;

    return true;
}

template <class T>
static bool
checkAtomize(HandleValue key)
{
    MOZ_ASSERT_UNREACHABLE("Key has to be PropertyName or Symbol");
    return false;
}

template <>
bool checkAtomize<JS::Symbol*>(HandleValue key)
{
    return false;
}

template <>
bool checkAtomize<PropertyName*>(HandleValue key)
{
    return !key.toString()->isAtom();
}

template <class T>
static bool
TryAttachNativeOrUnboxedGetValueElemStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                                         ICGetElem_Fallback* stub, HandleObject obj,
                                         HandleValue keyVal, bool* attached)
{
    MOZ_ASSERT(keyVal.isString() || keyVal.isSymbol());

    // Convert to id.
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, keyVal, &id))
        return false;

    Rooted<T> key(cx, getKey<T>(id));
    if (!key)
        return true;
    bool needsAtomize = checkAtomize<T>(keyVal);

    RootedShape shape(cx);
    RootedObject holder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, id, &holder, &shape))
        return false;
    if (!holder || (holder != obj && !holder->isNative()))
        return true;

    // If a suitable stub already exists, nothing else to do.
    if (GetElemNativeStubExists<T>(stub, obj, holder, key, needsAtomize))
        return true;

    // Remove any existing stubs that may interfere with the new stub being added.
    RemoveExistingGetElemNativeStubs<T>(cx, stub, obj, holder, key, needsAtomize);

    ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();

    if (obj->is<UnboxedPlainObject>() && holder == obj) {
        const UnboxedLayout::Property* property = obj->as<UnboxedPlainObject>().layout().lookup(id);

        // Once unboxed objects support symbol-keys, we need to change the following accordingly
        MOZ_ASSERT_IF(!keyVal.isString(), !property);

        if (property) {
            if (!cx->runtime()->jitSupportsFloatingPoint)
                return true;

            RootedPropertyName name(cx, JSID_TO_ATOM(id)->asPropertyName());
            ICGetElemNativeCompiler<PropertyName*> compiler(cx, ICStub::GetElem_UnboxedPropertyName,
                                                            monitorStub, obj, holder,
                                                            name,
                                                            ICGetElemNativeStub::UnboxedProperty,
                                                            needsAtomize, property->offset +
                                                            UnboxedPlainObject::offsetOfData(),
                                                            property->type);
            ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
            if (!newStub)
                return false;

            stub->addNewStub(newStub);
            *attached = true;
            return true;
        }

        Shape* shape = obj->as<UnboxedPlainObject>().maybeExpando()->lookup(cx, id);
        if (!shape->hasDefaultGetter() || !shape->hasSlot())
            return true;

        bool isFixedSlot;
        uint32_t offset;
        GetFixedOrDynamicSlotOffset(shape, &isFixedSlot, &offset);

        ICGetElemNativeStub::AccessType acctype =
            isFixedSlot ? ICGetElemNativeStub::FixedSlot
                        : ICGetElemNativeStub::DynamicSlot;
        ICGetElemNativeCompiler<T> compiler(cx, getGetElemStubKind<T>(ICStub::GetElem_NativeSlotName),
                                            monitorStub, obj, holder, key,
                                            acctype, needsAtomize, offset);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    if (!holder->isNative())
        return true;

    if (IsCacheableGetPropReadSlot(obj, holder, shape)) {
        bool isFixedSlot;
        uint32_t offset;
        GetFixedOrDynamicSlotOffset(shape, &isFixedSlot, &offset);

        ICStub::Kind kind = (obj == holder) ? ICStub::GetElem_NativeSlotName
                                            : ICStub::GetElem_NativePrototypeSlotName;
        kind = getGetElemStubKind<T>(kind);

        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(Native %s%s slot) stub "
                                    "(obj=%p, holder=%p, holderShape=%p)",
                    (obj == holder) ? "direct" : "prototype",
                    needsAtomize ? " atomizing" : "",
                obj.get(), holder.get(), holder->as<NativeObject>().lastProperty());

        AccType acctype = isFixedSlot ? ICGetElemNativeStub::FixedSlot
                                      : ICGetElemNativeStub::DynamicSlot;
        ICGetElemNativeCompiler<T> compiler(cx, kind, monitorStub, obj, holder, key,
                                            acctype, needsAtomize, offset);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    return true;
}

template <class T>
static bool
TryAttachNativeGetAccessorElemStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                                   ICGetElem_Fallback* stub, HandleNativeObject obj,
                                   HandleValue keyVal, bool* attached,
                                   bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(!*attached);
    MOZ_ASSERT(keyVal.isString() || keyVal.isSymbol());

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, keyVal, &id))
        return false;

    Rooted<T> key(cx, getKey<T>(id));
    if (!key)
        return true;
    bool needsAtomize = checkAtomize<T>(keyVal);

    RootedShape shape(cx);
    RootedObject baseHolder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, id, &baseHolder, &shape))
        return false;
    if (!baseHolder || baseHolder->isNative())
        return true;

    HandleNativeObject holder = baseHolder.as<NativeObject>();

    bool getterIsScripted = false;
    if (IsCacheableGetPropCall(cx, obj, baseHolder, shape, &getterIsScripted,
                               isTemporarilyUnoptimizable, /*isDOMProxy=*/false))
    {
        RootedFunction getter(cx, &shape->getterObject()->as<JSFunction>());

        // For now, we do not handle own property getters
        if (obj == holder)
            return true;

        // If a suitable stub already exists, nothing else to do.
        if (GetElemNativeStubExists<T>(stub, obj, holder, key, needsAtomize))
            return true;

        // Remove any existing stubs that may interfere with the new stub being added.
        RemoveExistingGetElemNativeStubs<T>(cx, stub, obj, holder, key, needsAtomize);

        ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
        ICStub::Kind kind = getterIsScripted ? ICStub::GetElem_NativePrototypeCallScriptedName
                                             : ICStub::GetElem_NativePrototypeCallNativeName;
        kind = getGetElemStubKind<T>(kind);

        if (getterIsScripted) {
            JitSpew(JitSpew_BaselineIC,
                    "  Generating GetElem(Native %s%s call scripted %s:%" PRIuSIZE ") stub "
                    "(obj=%p, shape=%p, holder=%p, holderShape=%p)",
                        (obj == holder) ? "direct" : "prototype",
                        needsAtomize ? " atomizing" : "",
                        getter->nonLazyScript()->filename(), getter->nonLazyScript()->lineno(),
                        obj.get(), obj->lastProperty(), holder.get(), holder->lastProperty());
        } else {
            JitSpew(JitSpew_BaselineIC,
                    "  Generating GetElem(Native %s%s call native) stub "
                    "(obj=%p, shape=%p, holder=%p, holderShape=%p)",
                        (obj == holder) ? "direct" : "prototype",
                        needsAtomize ? " atomizing" : "",
                        obj.get(), obj->lastProperty(), holder.get(), holder->lastProperty());
        }

        AccType acctype = getterIsScripted ? ICGetElemNativeStub::ScriptedGetter
                                           : ICGetElemNativeStub::NativeGetter;
        ICGetElemNativeCompiler<T> compiler(cx, kind, monitorStub, obj, holder, key, acctype,
                                            needsAtomize, getter, script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    return true;
}

static bool
IsPrimitiveArrayTypedObject(JSObject* obj)
{
    if (!obj->is<TypedObject>())
        return false;
    TypeDescr& descr = obj->as<TypedObject>().typeDescr();
    return descr.is<ArrayTypeDescr>() &&
           descr.as<ArrayTypeDescr>().elementType().is<ScalarTypeDescr>();
}

static Scalar::Type
PrimitiveArrayTypedObjectType(JSObject* obj)
{
    MOZ_ASSERT(IsPrimitiveArrayTypedObject(obj));
    TypeDescr& descr = obj->as<TypedObject>().typeDescr();
    return descr.as<ArrayTypeDescr>().elementType().as<ScalarTypeDescr>().type();
}

static Scalar::Type
TypedThingElementType(JSObject* obj)
{
    return IsAnyTypedArray(obj)
           ? AnyTypedArrayType(obj)
           : PrimitiveArrayTypedObjectType(obj);
}

static bool
TypedThingRequiresFloatingPoint(JSObject* obj)
{
    Scalar::Type type = TypedThingElementType(obj);
    return type == Scalar::Uint32 ||
           type == Scalar::Float32 ||
           type == Scalar::Float64;
}

static bool
IsNativeDenseElementAccess(HandleObject obj, HandleValue key)
{
    if (obj->isNative() && key.isInt32() && key.toInt32() >= 0 && !IsAnyTypedArray(obj.get()))
        return true;
    return false;
}

static bool
IsNativeOrUnboxedDenseElementAccess(HandleObject obj, HandleValue key)
{
    if (!obj->isNative() && !obj->is<UnboxedArrayObject>())
        return false;
    if (key.isInt32() && key.toInt32() >= 0 && !IsAnyTypedArray(obj.get()))
        return true;
    return false;
}

static bool
TryAttachGetElemStub(JSContext* cx, JSScript* script, jsbytecode* pc, ICGetElem_Fallback* stub,
                     HandleValue lhs, HandleValue rhs, HandleValue res, bool* attached)
{
    // Check for String[i] => Char accesses.
    if (lhs.isString() && rhs.isInt32() && res.isString() &&
        !stub->hasStub(ICStub::GetElem_String))
    {
        // NoSuchMethod handling doesn't apply to string targets.

        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(String[Int32]) stub");
        ICGetElem_String::Compiler compiler(cx);
        ICStub* stringStub = compiler.getStub(compiler.getStubSpace(script));
        if (!stringStub)
            return false;

        stub->addNewStub(stringStub);
        *attached = true;
        return true;
    }

    if (lhs.isMagic(JS_OPTIMIZED_ARGUMENTS) && rhs.isInt32() &&
        !ArgumentsGetElemStubExists(stub, ICGetElem_Arguments::Magic))
    {
        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(MagicArgs[Int32]) stub");
        ICGetElem_Arguments::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                               ICGetElem_Arguments::Magic);
        ICStub* argsStub = compiler.getStub(compiler.getStubSpace(script));
        if (!argsStub)
            return false;

        stub->addNewStub(argsStub);
        *attached = true;
        return true;
    }

    // Otherwise, GetElem is only optimized on objects.
    if (!lhs.isObject())
        return true;
    RootedObject obj(cx, &lhs.toObject());

    // Check for ArgumentsObj[int] accesses
    if (obj->is<ArgumentsObject>() && rhs.isInt32()) {
        ICGetElem_Arguments::Which which = ICGetElem_Arguments::Mapped;
        if (obj->is<UnmappedArgumentsObject>())
            which = ICGetElem_Arguments::Unmapped;
        if (!ArgumentsGetElemStubExists(stub, which)) {
            JitSpew(JitSpew_BaselineIC, "  Generating GetElem(ArgsObj[Int32]) stub");
            ICGetElem_Arguments::Compiler compiler(
                cx, stub->fallbackMonitorStub()->firstMonitorStub(), which);
            ICStub* argsStub = compiler.getStub(compiler.getStubSpace(script));
            if (!argsStub)
                return false;

            stub->addNewStub(argsStub);
            *attached = true;
            return true;
        }
    }

    // Check for NativeObject[int] dense accesses.
    if (IsNativeDenseElementAccess(obj, rhs)) {
        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(Native[Int32] dense) stub");
        ICGetElem_Dense::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                           obj->as<NativeObject>().lastProperty());
        ICStub* denseStub = compiler.getStub(compiler.getStubSpace(script));
        if (!denseStub)
            return false;

        stub->addNewStub(denseStub);
        *attached = true;
        return true;
    }

    // Check for NativeObject[id] and UnboxedPlainObject[id] shape-optimizable accesses.
    if (obj->isNative() || obj->is<UnboxedPlainObject>()) {
        RootedScript rootedScript(cx, script);
        if (rhs.isString()) {
            if (!TryAttachNativeOrUnboxedGetValueElemStub<PropertyName*>(cx, rootedScript, pc, stub,
                                                                         obj, rhs, attached))
            {
                return false;
            }
        } else if (rhs.isSymbol()) {
            if (!TryAttachNativeOrUnboxedGetValueElemStub<JS::Symbol*>(cx, rootedScript, pc, stub,
                                                                       obj, rhs, attached))
            {
                return false;
            }
        }
        if (*attached)
            return true;
        script = rootedScript;
    }

    // Check for UnboxedArray[int] accesses.
    if (obj->is<UnboxedArrayObject>() && rhs.isInt32() && rhs.toInt32() >= 0) {
        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(UnboxedArray[Int32]) stub");
        ICGetElem_UnboxedArray::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                                  obj->group());
        ICStub* unboxedStub = compiler.getStub(compiler.getStubSpace(script));
        if (!unboxedStub)
            return false;

        stub->addNewStub(unboxedStub);
        *attached = true;
        return true;
    }

    // Check for TypedArray[int] => Number and TypedObject[int] => Number accesses.
    if ((IsAnyTypedArray(obj.get()) || IsPrimitiveArrayTypedObject(obj)) &&
        rhs.isNumber() &&
        res.isNumber() &&
        !TypedArrayGetElemStubExists(stub, obj))
    {
        if (!cx->runtime()->jitSupportsFloatingPoint &&
            (TypedThingRequiresFloatingPoint(obj) || rhs.isDouble()))
        {
            return true;
        }

        // Don't attach typed object stubs if they might be neutered, as the
        // stub will always bail out.
        if (IsPrimitiveArrayTypedObject(obj) && cx->compartment()->neuteredTypedObjects)
            return true;

        JitSpew(JitSpew_BaselineIC, "  Generating GetElem(TypedArray[Int32]) stub");
        ICGetElem_TypedArray::Compiler compiler(cx, obj->maybeShape(), TypedThingElementType(obj));
        ICStub* typedArrayStub = compiler.getStub(compiler.getStubSpace(script));
        if (!typedArrayStub)
            return false;

        stub->addNewStub(typedArrayStub);
        *attached = true;
        return true;
    }

    // GetElem operations on non-native objects cannot be cached by either
    // Baseline or Ion. Indicate this in the cache so that Ion does not
    // generate a cache for this op.
    if (!obj->isNative())
        stub->noteNonNativeAccess();

    // GetElem operations which could access negative indexes generally can't
    // be optimized without the potential for bailouts, as we can't statically
    // determine that an object has no properties on such indexes.
    if (rhs.isNumber() && rhs.toNumber() < 0)
        stub->noteNegativeIndex();

    return true;
}

static bool
DoGetElemFallback(JSContext* cx, BaselineFrame* frame, ICGetElem_Fallback* stub_, HandleValue lhs,
                  HandleValue rhs, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICGetElem_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
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
            TypeScript::Monitor(cx, frame->script(), pc, res);
    }

    bool attached = false;
    if (stub->numOptimizedStubs() >= ICGetElem_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        stub->noteUnoptimizableAccess();
        attached = true;
    }

    // Try to attach an optimized getter stub.
    bool isTemporarilyUnoptimizable = false;
    if (!attached && lhs.isObject() && lhs.toObject().isNative()){
        if (rhs.isString()) {
            RootedScript rootedScript(cx, frame->script());
            RootedNativeObject obj(cx, &lhs.toObject().as<NativeObject>());
            if (!TryAttachNativeGetAccessorElemStub<PropertyName*>(cx, rootedScript, pc, stub,
                                                                   obj, rhs, &attached,
                                                                   &isTemporarilyUnoptimizable))
            {
                return false;
            }
            script = rootedScript;
        } else if (rhs.isSymbol()) {
            RootedScript rootedScript(cx, frame->script());
            RootedNativeObject obj(cx, &lhs.toObject().as<NativeObject>());
            if (!TryAttachNativeGetAccessorElemStub<JS::Symbol*>(cx, rootedScript, pc, stub,
                                                                 obj, rhs, &attached,
                                                                 &isTemporarilyUnoptimizable))
            {
                return false;
            }
            script = rootedScript;
        }
    }

    if (!isOptimizedArgs) {
        if (!GetElementOperation(cx, op, &lhsCopy, rhs, res))
            return false;
        TypeScript::Monitor(cx, frame->script(), pc, res);
    }

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, frame->script(), res,
        ICStubCompiler::Engine::Baseline))
    {
        return false;
    }

    if (attached)
        return true;

    // Try to attach an optimized stub.
    if (!TryAttachGetElemStub(cx, frame->script(), pc, stub, lhs, rhs, res, &attached))
        return false;

    if (!attached && !isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoGetElemFallbackFn)(JSContext*, BaselineFrame*, ICGetElem_Fallback*,
                                    HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoGetElemFallbackInfo =
    FunctionInfo<DoGetElemFallbackFn>(DoGetElemFallback, TailCall, PopValues(2));

bool
ICGetElem_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    // Restore the tail call register.
    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoGetElemFallbackInfo, masm);
}

//
// GetElem_NativeSlot
//

static bool
DoAtomizeString(JSContext* cx, HandleString string, MutableHandleValue result)
{
    JitSpew(JitSpew_BaselineIC, "  AtomizeString called");

    RootedValue key(cx, StringValue(string));

    // Convert to interned property name.
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, key, &id))
        return false;

    if (!JSID_IS_ATOM(id)) {
        result.set(key);
        return true;
    }

    result.set(StringValue(JSID_TO_ATOM(id)));
    return true;
}

typedef bool (*DoAtomizeStringFn)(JSContext*, HandleString, MutableHandleValue);
static const VMFunction DoAtomizeStringInfo = FunctionInfo<DoAtomizeStringFn>(DoAtomizeString);

template <class T>
bool
ICGetElemNativeCompiler<T>::emitCallNative(MacroAssembler& masm, Register objReg)
{
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
    regs.takeUnchecked(objReg);
    regs.takeUnchecked(ICTailCallReg);

    enterStubFrame(masm, regs.getAny());

    // Push object.
    masm.push(objReg);

    // Push native callee.
    masm.loadPtr(Address(ICStubReg, ICGetElemNativeGetterStub<T>::offsetOfGetter()), objReg);
    masm.push(objReg);

    regs.add(objReg);

    // Call helper.
    if (!callVM(DoCallNativeGetterInfo, masm))
        return false;

    leaveStubFrame(masm);

    return true;
}

template <class T>
bool
ICGetElemNativeCompiler<T>::emitCallScripted(MacroAssembler& masm, Register objReg)
{
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
    regs.takeUnchecked(objReg);
    regs.takeUnchecked(ICTailCallReg);

    // Enter stub frame.
    enterStubFrame(masm, regs.getAny());

    // Align the stack such that the JitFrameLayout is aligned on
    // JitStackAlignment.
    masm.alignJitStackBasedOnNArgs(0);

    // Push |this| for getter (target object).
    {
        ValueOperand val = regs.takeAnyValue();
        masm.tagValue(JSVAL_TYPE_OBJECT, objReg, val);
        masm.Push(val);
        regs.add(val);
    }

    regs.add(objReg);

    Register callee = regs.takeAny();
    masm.loadPtr(Address(ICStubReg, ICGetElemNativeGetterStub<T>::offsetOfGetter()), callee);

    // Push argc, callee, and descriptor.
    {
        Register callScratch = regs.takeAny();
        EmitBaselineCreateStubFrameDescriptor(masm, callScratch);
        masm.Push(Imm32(0));  // ActualArgc is 0
        masm.Push(callee);
        masm.Push(callScratch);
        regs.add(callScratch);
    }

    Register code = regs.takeAnyExcluding(ArgumentsRectifierReg);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), code);
    masm.loadBaselineOrIonRaw(code, code, nullptr);

    Register scratch = regs.takeAny();

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), scratch);
    masm.branch32(Assembler::Equal, scratch, Imm32(0), &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != code);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.movePtr(ImmWord(0), ArgumentsRectifierReg);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    leaveStubFrame(masm, true);

    return true;
}

template <class T>
bool
ICGetElemNativeCompiler<T>::emitCheckKey(MacroAssembler& masm, Label& failure)
{
    MOZ_ASSERT_UNREACHABLE("Key has to be PropertyName or Symbol");
    return false;
}

template <>
bool
ICGetElemNativeCompiler<JS::Symbol*>::emitCheckKey(MacroAssembler& masm, Label& failure)
{
    MOZ_ASSERT(!needsAtomize_);
    masm.branchTestSymbol(Assembler::NotEqual, R1, &failure);
    Address symbolAddr(ICStubReg, ICGetElemNativeStubImpl<JS::Symbol*>::offsetOfKey());
    Register symExtract = masm.extractObject(R1, ExtractTemp1);
    masm.branchPtr(Assembler::NotEqual, symbolAddr, symExtract, &failure);
    return true;
}

template <>
bool
ICGetElemNativeCompiler<PropertyName*>::emitCheckKey(MacroAssembler& masm, Label& failure)
{
    masm.branchTestString(Assembler::NotEqual, R1, &failure);
    // Check key identity.  Don't automatically fail if this fails, since the incoming
    // key maybe a non-interned string.  Switch to a slowpath vm-call based check.
    Address nameAddr(ICStubReg, ICGetElemNativeStubImpl<PropertyName*>::offsetOfKey());
    Register strExtract = masm.extractString(R1, ExtractTemp1);

    // If needsAtomize_ is true, and the string is not already an atom, then atomize the
    // string before proceeding.
    if (needsAtomize_) {
        Label skipAtomize;

        // If string is already an atom, skip the atomize.
        masm.branchTest32(Assembler::NonZero,
                          Address(strExtract, JSString::offsetOfFlags()),
                          Imm32(JSString::ATOM_BIT),
                          &skipAtomize);

        // Stow R0.
        EmitStowICValues(masm, 1);

        enterStubFrame(masm, R0.scratchReg());

        // Atomize the string into a new value.
        masm.push(strExtract);
        if (!callVM(DoAtomizeStringInfo, masm))
            return false;

        // Atomized string is now in JSReturnOperand (R0).
        // Leave stub frame, move atomized string into R1.
        MOZ_ASSERT(R0 == JSReturnOperand);
        leaveStubFrame(masm);
        masm.moveValue(JSReturnOperand, R1);

        // Unstow R0
        EmitUnstowICValues(masm, 1);

        // Extract string from R1 again.
        DebugOnly<Register> strExtract2 = masm.extractString(R1, ExtractTemp1);
        MOZ_ASSERT(Register(strExtract2) == strExtract);

        masm.bind(&skipAtomize);
    }

    // Key has been atomized if necessary.  Do identity check on string pointer.
    masm.branchPtr(Assembler::NotEqual, nameAddr, strExtract, &failure);
    return true;
}

template <class T>
bool
ICGetElemNativeCompiler<T>::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    Label failurePopR1;
    bool popR1 = false;

    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox object.
    Register objReg = masm.extractObject(R0, ExtractTemp0);

    // Check object shape/group.
    GuardReceiverObject(masm, ReceiverGuard(obj_), objReg, scratchReg,
                        ICGetElemNativeStub::offsetOfReceiverGuard(), &failure);

    // Since this stub sometimes enters a stub frame, we manually set this to true (lie).
#ifdef DEBUG
    entersStubFrame_ = true;
#endif

    if (!emitCheckKey(masm, failure))
        return false;

    Register holderReg;
    if (obj_ == holder_) {
        holderReg = objReg;

        if (obj_->is<UnboxedPlainObject>() && acctype_ != ICGetElemNativeStub::UnboxedProperty) {
            // The property will be loaded off the unboxed expando.
            masm.push(R1.scratchReg());
            popR1 = true;
            holderReg = R1.scratchReg();
            masm.loadPtr(Address(objReg, UnboxedPlainObject::offsetOfExpando()), holderReg);
        }
    } else {
        // Shape guard holder.
        if (regs.empty()) {
            masm.push(R1.scratchReg());
            popR1 = true;
            holderReg = R1.scratchReg();
        } else {
            holderReg = regs.takeAny();
        }

        if (kind == ICStub::GetElem_NativePrototypeCallNativeName ||
            kind == ICStub::GetElem_NativePrototypeCallNativeSymbol ||
            kind == ICStub::GetElem_NativePrototypeCallScriptedName ||
            kind == ICStub::GetElem_NativePrototypeCallScriptedSymbol)
        {
            masm.loadPtr(Address(ICStubReg,
                                 ICGetElemNativePrototypeCallStub<T>::offsetOfHolder()),
                         holderReg);
            masm.loadPtr(Address(ICStubReg,
                                 ICGetElemNativePrototypeCallStub<T>::offsetOfHolderShape()),
                         scratchReg);
        } else {
            masm.loadPtr(Address(ICStubReg,
                                 ICGetElem_NativePrototypeSlot<T>::offsetOfHolder()),
                         holderReg);
            masm.loadPtr(Address(ICStubReg,
                                 ICGetElem_NativePrototypeSlot<T>::offsetOfHolderShape()),
                         scratchReg);
        }
        masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratchReg,
                                popR1 ? &failurePopR1 : &failure);
    }

    if (acctype_ == ICGetElemNativeStub::DynamicSlot ||
        acctype_ == ICGetElemNativeStub::FixedSlot)
    {
        masm.load32(Address(ICStubReg, ICGetElemNativeSlotStub<T>::offsetOfOffset()),
                    scratchReg);

        // Load from object.
        if (acctype_ == ICGetElemNativeStub::DynamicSlot)
            masm.addPtr(Address(holderReg, NativeObject::offsetOfSlots()), scratchReg);
        else
            masm.addPtr(holderReg, scratchReg);

        Address valAddr(scratchReg, 0);
        masm.loadValue(valAddr, R0);
        if (popR1)
            masm.addToStackPtr(ImmWord(sizeof(size_t)));

    } else if (acctype_ == ICGetElemNativeStub::UnboxedProperty) {
        masm.load32(Address(ICStubReg, ICGetElemNativeSlotStub<T>::offsetOfOffset()),
                    scratchReg);
        masm.loadUnboxedProperty(BaseIndex(objReg, scratchReg, TimesOne), unboxedType_,
                                 TypedOrValueRegister(R0));
        if (popR1)
            masm.addToStackPtr(ImmWord(sizeof(size_t)));
    } else {
        MOZ_ASSERT(acctype_ == ICGetElemNativeStub::NativeGetter ||
                   acctype_ == ICGetElemNativeStub::ScriptedGetter);
        MOZ_ASSERT(kind == ICStub::GetElem_NativePrototypeCallNativeName ||
                   kind == ICStub::GetElem_NativePrototypeCallNativeSymbol ||
                   kind == ICStub::GetElem_NativePrototypeCallScriptedName ||
                   kind == ICStub::GetElem_NativePrototypeCallScriptedSymbol);

        if (acctype_ == ICGetElemNativeStub::NativeGetter) {
            // If calling a native getter, there is no chance of failure now.

            // GetElem key (R1) is no longer needed.
            if (popR1)
                masm.addToStackPtr(ImmWord(sizeof(size_t)));

            emitCallNative(masm, objReg);

        } else {
            MOZ_ASSERT(acctype_ == ICGetElemNativeStub::ScriptedGetter);

            // Load function in scratchReg and ensure that it has a jit script.
            masm.loadPtr(Address(ICStubReg, ICGetElemNativeGetterStub<T>::offsetOfGetter()),
                         scratchReg);
            masm.branchIfFunctionHasNoScript(scratchReg, popR1 ? &failurePopR1 : &failure);
            masm.loadPtr(Address(scratchReg, JSFunction::offsetOfNativeOrScript()), scratchReg);
            masm.loadBaselineOrIonRaw(scratchReg, scratchReg, popR1 ? &failurePopR1 : &failure);

            // At this point, we are guaranteed to successfully complete.
            if (popR1)
                masm.addToStackPtr(Imm32(sizeof(size_t)));

            emitCallScripted(masm, objReg);
        }
    }

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    if (popR1) {
        masm.bind(&failurePopR1);
        masm.pop(R1.scratchReg());
    }
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    return true;
}

//
// GetElem_String
//

bool
ICGetElem_String::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    masm.branchTestString(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox string in R0.
    Register str = masm.extractString(R0, ExtractTemp0);

    // Check for non-linear strings.
    masm.branchIfRope(str, &failure);

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    masm.branch32(Assembler::BelowOrEqual, Address(str, JSString::offsetOfLength()),
                  key, &failure);

    // Get char code.
    masm.loadStringChar(str, key, scratchReg);

    // Check if char code >= UNIT_STATIC_LIMIT.
    masm.branch32(Assembler::AboveOrEqual, scratchReg, Imm32(StaticStrings::UNIT_STATIC_LIMIT),
                  &failure);

    // Load static string.
    masm.movePtr(ImmPtr(&cx->staticStrings().unitStaticTable), str);
    masm.loadPtr(BaseIndex(str, scratchReg, ScalePointer), str);

    // Return.
    masm.tagValue(JSVAL_TYPE_STRING, str, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetElem_Dense
//

bool
ICGetElem_Dense::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and shape guard.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICGetElem_Dense::offsetOfShape()), scratchReg);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);

    // Load obj->elements.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratchReg);

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    Address initLength(scratchReg, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::BelowOrEqual, initLength, key, &failure);

    // Hole check and load value.
    BaseObjectElementIndex element(scratchReg, key);
    masm.branchTestMagic(Assembler::Equal, element, &failure);

    // Load value from element location.
    masm.loadValue(element, R0);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetElem_UnboxedArray
//

bool
ICGetElem_UnboxedArray::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and group guard.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICGetElem_UnboxedArray::offsetOfGroup()), scratchReg);
    masm.branchTestObjGroup(Assembler::NotEqual, obj, scratchReg, &failure);

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    masm.load32(Address(obj, UnboxedArrayObject::offsetOfCapacityIndexAndInitializedLength()),
                scratchReg);
    masm.and32(Imm32(UnboxedArrayObject::InitializedLengthMask), scratchReg);
    masm.branch32(Assembler::BelowOrEqual, scratchReg, key, &failure);

    // Load obj->elements.
    masm.loadPtr(Address(obj, UnboxedArrayObject::offsetOfElements()), scratchReg);

    // Load value.
    size_t width = UnboxedTypeSize(elementType_);
    BaseIndex addr(scratchReg, key, ScaleFromElemWidth(width));
    masm.loadUnboxedProperty(addr, elementType_, R0);

    // Only monitor the result if its type might change.
    if (elementType_ == JSVAL_TYPE_OBJECT)
        EmitEnterTypeMonitorIC(masm);
    else
        EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetElem_TypedArray
//

static void
LoadTypedThingLength(MacroAssembler& masm, TypedThingLayout layout, Register obj, Register result)
{
    switch (layout) {
      case Layout_TypedArray:
        masm.unboxInt32(Address(obj, TypedArrayObject::lengthOffset()), result);
        break;
      case Layout_OutlineTypedObject:
      case Layout_InlineTypedObject:
        masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), result);
        masm.loadPtr(Address(result, ObjectGroup::offsetOfAddendum()), result);
        masm.unboxInt32(Address(result, ArrayTypeDescr::offsetOfLength()), result);
        break;
      default:
        MOZ_CRASH();
    }
}

bool
ICGetElem_TypedArray::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;

    if (layout_ != Layout_TypedArray)
        CheckForNeuteredTypedObject(cx, masm, &failure);

    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and shape guard.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICGetElem_TypedArray::offsetOfShape()), scratchReg);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);

    // Ensure the index is an integer.
    if (cx->runtime()->jitSupportsFloatingPoint) {
        Label isInt32;
        masm.branchTestInt32(Assembler::Equal, R1, &isInt32);
        {
            // If the index is a double, try to convert it to int32. It's okay
            // to convert -0 to 0: the shape check ensures the object is a typed
            // array so the difference is not observable.
            masm.branchTestDouble(Assembler::NotEqual, R1, &failure);
            masm.unboxDouble(R1, FloatReg0);
            masm.convertDoubleToInt32(FloatReg0, scratchReg, &failure, /* negZeroCheck = */false);
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R1);
        }
        masm.bind(&isInt32);
    } else {
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
    }

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    LoadTypedThingLength(masm, layout_, obj, scratchReg);
    masm.branch32(Assembler::BelowOrEqual, scratchReg, key, &failure);

    // Load the elements vector.
    LoadTypedThingData(masm, layout_, obj, scratchReg);

    // Load the value.
    BaseIndex source(scratchReg, key, ScaleFromElemWidth(Scalar::byteSize(type_)));
    masm.loadFromTypedArray(type_, source, R0, false, scratchReg, &failure);

    // Todo: Allow loading doubles from uint32 arrays, but this requires monitoring.
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// GetElem_Arguments
//
bool
ICGetElem_Arguments::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    if (which_ == ICGetElem_Arguments::Magic) {
        // Ensure that this is a magic arguments value.
        masm.branchTestMagicValue(Assembler::NotEqual, R0, JS_OPTIMIZED_ARGUMENTS, &failure);

        // Ensure that frame has not loaded different arguments object since.
        masm.branchTest32(Assembler::NonZero,
                          Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags()),
                          Imm32(BaselineFrame::HAS_ARGS_OBJ),
                          &failure);

        // Ensure that index is an integer.
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
        Register idx = masm.extractInt32(R1, ExtractTemp1);

        AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
        Register scratch = regs.takeAny();

        // Load num actual arguments
        Address actualArgs(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs());
        masm.loadPtr(actualArgs, scratch);

        // Ensure idx < argc
        masm.branch32(Assembler::AboveOrEqual, idx, scratch, &failure);

        // Load argval
        masm.movePtr(BaselineFrameReg, scratch);
        masm.addPtr(Imm32(BaselineFrame::offsetOfArg(0)), scratch);
        BaseValueIndex element(scratch, idx);
        masm.loadValue(element, R0);

        // Enter type monitor IC to type-check result.
        EmitEnterTypeMonitorIC(masm);

        masm.bind(&failure);
        EmitStubGuardFailure(masm);
        return true;
    }

    MOZ_ASSERT(which_ == ICGetElem_Arguments::Mapped ||
               which_ == ICGetElem_Arguments::Unmapped);

    const Class* clasp = (which_ == ICGetElem_Arguments::Mapped)
                         ? &MappedArgumentsObject::class_
                         : &UnmappedArgumentsObject::class_;

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Guard on input being an arguments object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    masm.branchTestObjClass(Assembler::NotEqual, objReg, scratchReg, clasp, &failure);

    // Guard on index being int32
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
    Register idxReg = masm.extractInt32(R1, ExtractTemp1);

    // Get initial ArgsObj length value.
    masm.unboxInt32(Address(objReg, ArgumentsObject::getInitialLengthSlotOffset()), scratchReg);

    // Test if length has been overridden.
    masm.branchTest32(Assembler::NonZero,
                      scratchReg,
                      Imm32(ArgumentsObject::LENGTH_OVERRIDDEN_BIT),
                      &failure);

    // Length has not been overridden, ensure that R1 is an integer and is <= length.
    masm.rshiftPtr(Imm32(ArgumentsObject::PACKED_BITS_COUNT), scratchReg);
    masm.branch32(Assembler::AboveOrEqual, idxReg, scratchReg, &failure);

    // Length check succeeded, now check the correct bit.  We clobber potential type regs
    // now.  Inputs will have to be reconstructed if we fail after this point, but that's
    // unlikely.
    Label failureReconstructInputs;
    regs = availableGeneralRegs(0);
    regs.takeUnchecked(objReg);
    regs.takeUnchecked(idxReg);
    regs.take(scratchReg);
    Register argData = regs.takeAny();
    Register tempReg = regs.takeAny();

    // Load ArgumentsData
    masm.loadPrivate(Address(objReg, ArgumentsObject::getDataSlotOffset()), argData);

    // Load deletedBits bitArray pointer into scratchReg
    masm.loadPtr(Address(argData, offsetof(ArgumentsData, deletedBits)), scratchReg);

    // In tempReg, calculate index of word containing bit: (idx >> logBitsPerWord)
    masm.movePtr(idxReg, tempReg);
    const uint32_t shift = mozilla::tl::FloorLog2<(sizeof(size_t) * JS_BITS_PER_BYTE)>::value;
    MOZ_ASSERT(shift == 5 || shift == 6);
    masm.rshiftPtr(Imm32(shift), tempReg);
    masm.loadPtr(BaseIndex(scratchReg, tempReg, ScaleFromElemWidth(sizeof(size_t))), scratchReg);

    // Don't bother testing specific bit, if any bit is set in the word, fail.
    masm.branchPtr(Assembler::NotEqual, scratchReg, ImmPtr(nullptr), &failureReconstructInputs);

    // Load the value.  use scratchReg and tempReg to form a ValueOperand to load into.
    masm.addPtr(Imm32(ArgumentsData::offsetOfArgs()), argData);
    regs.add(scratchReg);
    regs.add(tempReg);
    ValueOperand tempVal = regs.takeAnyValue();
    masm.loadValue(BaseValueIndex(argData, idxReg), tempVal);

    // Makesure that this is not a FORWARD_TO_CALL_SLOT magic value.
    masm.branchTestMagic(Assembler::Equal, tempVal, &failureReconstructInputs);

    // Copy value from temp to R0.
    masm.moveValue(tempVal, R0);

    // Type-check result
    EmitEnterTypeMonitorIC(masm);

    // Failed, but inputs are deconstructed into object and int, and need to be
    // reconstructed into values.
    masm.bind(&failureReconstructInputs);
    masm.tagValue(JSVAL_TYPE_OBJECT, objReg, R0);
    masm.tagValue(JSVAL_TYPE_INT32, idxReg, R1);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// SetElem_Fallback
//

static bool
SetElemAddHasSameShapes(ICSetElem_DenseOrUnboxedArrayAdd* stub, JSObject* obj)
{
    static const size_t MAX_DEPTH = ICSetElem_DenseOrUnboxedArrayAdd::MAX_PROTO_CHAIN_DEPTH;
    ICSetElem_DenseOrUnboxedArrayAddImpl<MAX_DEPTH>* nstub = stub->toImplUnchecked<MAX_DEPTH>();

    if (obj->maybeShape() != nstub->shape(0))
        return false;

    JSObject* proto = obj->getProto();
    for (size_t i = 0; i < stub->protoChainDepth(); i++) {
        if (!proto->isNative())
            return false;
        if (proto->as<NativeObject>().lastProperty() != nstub->shape(i + 1))
            return false;
        proto = obj->getProto();
        if (!proto) {
            if (i != stub->protoChainDepth() - 1)
                return false;
            break;
        }
    }

    return true;
}

static bool
DenseOrUnboxedArraySetElemStubExists(JSContext* cx, ICStub::Kind kind,
                                     ICSetElem_Fallback* stub, HandleObject obj)
{
    MOZ_ASSERT(kind == ICStub::SetElem_DenseOrUnboxedArray ||
               kind == ICStub::SetElem_DenseOrUnboxedArrayAdd);

    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (kind == ICStub::SetElem_DenseOrUnboxedArray && iter->isSetElem_DenseOrUnboxedArray()) {
            ICSetElem_DenseOrUnboxedArray* nstub = iter->toSetElem_DenseOrUnboxedArray();
            if (obj->maybeShape() == nstub->shape() && obj->getGroup(cx) == nstub->group())
                return true;
        }

        if (kind == ICStub::SetElem_DenseOrUnboxedArrayAdd && iter->isSetElem_DenseOrUnboxedArrayAdd()) {
            ICSetElem_DenseOrUnboxedArrayAdd* nstub = iter->toSetElem_DenseOrUnboxedArrayAdd();
            if (obj->getGroup(cx) == nstub->group() && SetElemAddHasSameShapes(nstub, obj))
                return true;
        }
    }
    return false;
}

static bool
TypedArraySetElemStubExists(ICSetElem_Fallback* stub, HandleObject obj, bool expectOOB)
{
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        if (!iter->isSetElem_TypedArray())
            continue;
        ICSetElem_TypedArray* taStub = iter->toSetElem_TypedArray();
        if (obj->maybeShape() == taStub->shape() && taStub->expectOutOfBounds() == expectOOB)
            return true;
    }
    return false;
}

static bool
RemoveExistingTypedArraySetElemStub(JSContext* cx, ICSetElem_Fallback* stub, HandleObject obj)
{
    for (ICStubIterator iter = stub->beginChain(); !iter.atEnd(); iter++) {
        if (!iter->isSetElem_TypedArray())
            continue;

        if (obj->maybeShape() != iter->toSetElem_TypedArray()->shape())
            continue;

        // TypedArraySetElem stubs are only removed using this procedure if
        // being replaced with one that expects out of bounds index.
        MOZ_ASSERT(!iter->toSetElem_TypedArray()->expectOutOfBounds());
        iter.unlink(cx);
        return true;
    }
    return false;
}

static bool
CanOptimizeDenseOrUnboxedArraySetElem(JSObject* obj, uint32_t index,
                                      Shape* oldShape, uint32_t oldCapacity, uint32_t oldInitLength,
                                      bool* isAddingCaseOut, size_t* protoDepthOut)
{
    uint32_t initLength = GetAnyBoxedOrUnboxedInitializedLength(obj);
    uint32_t capacity = GetAnyBoxedOrUnboxedCapacity(obj);

    *isAddingCaseOut = false;
    *protoDepthOut = 0;

    // Some initial sanity checks.
    if (initLength < oldInitLength || capacity < oldCapacity)
        return false;

    // Unboxed arrays need to be able to emit floating point code.
    if (obj->is<UnboxedArrayObject>() && !obj->runtimeFromMainThread()->jitSupportsFloatingPoint)
        return false;

    Shape* shape = obj->maybeShape();

    // Cannot optimize if the shape changed.
    if (oldShape != shape)
        return false;

    // Cannot optimize if the capacity changed.
    if (oldCapacity != capacity)
        return false;

    // Cannot optimize if the index doesn't fit within the new initialized length.
    if (index >= initLength)
        return false;

    // Cannot optimize if the value at position after the set is a hole.
    if (obj->isNative() && !obj->as<NativeObject>().containsDenseElement(index))
        return false;

    // At this point, if we know that the initLength did not change, then
    // an optimized set is possible.
    if (oldInitLength == initLength)
        return true;

    // If it did change, ensure that it changed specifically by incrementing by 1
    // to accomodate this particular indexed set.
    if (oldInitLength + 1 != initLength)
        return false;
    if (index != oldInitLength)
        return false;

    // The checks are not complete.  The object may have a setter definition,
    // either directly, or via a prototype, or via the target object for a prototype
    // which is a proxy, that handles a particular integer write.
    // Scan the prototype and shape chain to make sure that this is not the case.
    if (obj->isIndexed())
        return false;
    JSObject* curObj = obj->getProto();
    while (curObj) {
        ++*protoDepthOut;
        if (!curObj->isNative() || curObj->isIndexed())
            return false;
        curObj = curObj->getProto();
    }

    if (*protoDepthOut > ICSetElem_DenseOrUnboxedArrayAdd::MAX_PROTO_CHAIN_DEPTH)
        return false;

    *isAddingCaseOut = true;
    return true;
}

static bool
DoSetElemFallback(JSContext* cx, BaselineFrame* frame, ICSetElem_Fallback* stub_, Value* stack,
                  HandleValue objv, HandleValue index, HandleValue rhs)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICSetElem_Fallback*> stub(frame, stub_);

    RootedScript script(cx, frame->script());
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

    // Check the old capacity
    uint32_t oldCapacity = 0;
    uint32_t oldInitLength = 0;
    if (index.isInt32() && index.toInt32() >= 0) {
        oldCapacity = GetAnyBoxedOrUnboxedCapacity(obj);
        oldInitLength = GetAnyBoxedOrUnboxedInitializedLength(obj);
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
        if (!SetObjectElement(cx, obj, index, rhs, JSOp(*pc) == JSOP_STRICTSETELEM, script, pc))
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

    if (stub->numOptimizedStubs() >= ICSetElem_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with inert megamorphic stub.
        // But for now we just bail.
        return true;
    }

    // Try to generate new stubs.
    if (IsNativeOrUnboxedDenseElementAccess(obj, index) && !rhs.isMagic(JS_ELEMENTS_HOLE)) {
        bool addingCase;
        size_t protoDepth;

        if (CanOptimizeDenseOrUnboxedArraySetElem(obj, index.toInt32(),
                                                  oldShape, oldCapacity, oldInitLength,
                                                  &addingCase, &protoDepth))
        {
            RootedShape shape(cx, obj->maybeShape());
            RootedObjectGroup group(cx, obj->getGroup(cx));
            if (!group)
                return false;

            if (addingCase &&
                !DenseOrUnboxedArraySetElemStubExists(cx, ICStub::SetElem_DenseOrUnboxedArrayAdd,
                                                      stub, obj))
            {
                JitSpew(JitSpew_BaselineIC,
                        "  Generating SetElem_DenseOrUnboxedArrayAdd stub "
                        "(shape=%p, group=%p, protoDepth=%u)",
                        shape.get(), group.get(), protoDepth);
                ICSetElemDenseOrUnboxedArrayAddCompiler compiler(cx, obj, protoDepth);
                ICUpdatedStub* newStub = compiler.getStub(compiler.getStubSpace(script));
                if (!newStub)
                    return false;
                if (compiler.needsUpdateStubs() &&
                    !newStub->addUpdateStubForValue(cx, script, obj, JSID_VOIDHANDLE, rhs))
                {
                    return false;
                }

                stub->addNewStub(newStub);
            } else if (!addingCase &&
                       !DenseOrUnboxedArraySetElemStubExists(cx,
                                                             ICStub::SetElem_DenseOrUnboxedArray,
                                                             stub, obj))
            {
                JitSpew(JitSpew_BaselineIC,
                        "  Generating SetElem_DenseOrUnboxedArray stub (shape=%p, group=%p)",
                        shape.get(), group.get());
                ICSetElem_DenseOrUnboxedArray::Compiler compiler(cx, shape, group);
                ICUpdatedStub* newStub = compiler.getStub(compiler.getStubSpace(script));
                if (!newStub)
                    return false;
                if (compiler.needsUpdateStubs() &&
                    !newStub->addUpdateStubForValue(cx, script, obj, JSID_VOIDHANDLE, rhs))
                {
                    return false;
                }

                stub->addNewStub(newStub);
            }
        }

        return true;
    }

    if ((IsAnyTypedArray(obj.get()) || IsPrimitiveArrayTypedObject(obj)) &&
        index.isNumber() &&
        rhs.isNumber())
    {
        if (!cx->runtime()->jitSupportsFloatingPoint &&
            (TypedThingRequiresFloatingPoint(obj) || index.isDouble()))
        {
            return true;
        }

        bool expectOutOfBounds;
        double idx = index.toNumber();
        if (IsAnyTypedArray(obj)) {
            expectOutOfBounds = (idx < 0 || idx >= double(AnyTypedArrayLength(obj)));
        } else {
            // Typed objects throw on out of bounds accesses. Don't attach
            // a stub in this case.
            if (idx < 0 || idx >= double(obj->as<TypedObject>().length()))
                return true;
            expectOutOfBounds = false;

            // Don't attach stubs if typed objects in the compartment might be
            // neutered, as the stub will always bail out.
            if (cx->compartment()->neuteredTypedObjects)
                return true;
        }

        if (!TypedArraySetElemStubExists(stub, obj, expectOutOfBounds)) {
            // Remove any existing TypedArraySetElemStub that doesn't handle out-of-bounds
            if (expectOutOfBounds)
                RemoveExistingTypedArraySetElemStub(cx, stub, obj);

            Shape* shape = obj->maybeShape();
            Scalar::Type type = TypedThingElementType(obj);

            JitSpew(JitSpew_BaselineIC,
                    "  Generating SetElem_TypedArray stub (shape=%p, type=%u, oob=%s)",
                    shape, type, expectOutOfBounds ? "yes" : "no");
            ICSetElem_TypedArray::Compiler compiler(cx, shape, type, expectOutOfBounds);
            ICStub* typedArrayStub = compiler.getStub(compiler.getStubSpace(script));
            if (!typedArrayStub)
                return false;

            stub->addNewStub(typedArrayStub);
            return true;
        }
    }

    return true;
}

typedef bool (*DoSetElemFallbackFn)(JSContext*, BaselineFrame*, ICSetElem_Fallback*, Value*,
                                    HandleValue, HandleValue, HandleValue);
static const VMFunction DoSetElemFallbackInfo =
    FunctionInfo<DoSetElemFallbackFn>(DoSetElemFallback, TailCall, PopValues(2));

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
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoSetElemFallbackInfo, masm);
}

void
BaselineScript::noteArrayWriteHole(uint32_t pcOffset)
{
    ICEntry& entry = icEntryFromPCOffset(pcOffset);
    ICFallbackStub* stub = entry.fallbackStub();

    if (stub->isSetElem_Fallback())
        stub->toSetElem_Fallback()->noteArrayWriteHole();
}

//
// SetElem_DenseOrUnboxedArray
//

template <typename T>
void
EmitUnboxedPreBarrierForBaseline(MacroAssembler &masm, T address, JSValueType type)
{
    if (type == JSVAL_TYPE_OBJECT)
        EmitPreBarrier(masm, address, MIRType_Object);
    else if (type == JSVAL_TYPE_STRING)
        EmitPreBarrier(masm, address, MIRType_String);
    else
        MOZ_ASSERT(!UnboxedTypeNeedsPreBarrier(type));
}

bool
ICSetElem_DenseOrUnboxedArray::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // R0 = object
    // R1 = key
    // Stack = { ... rhs-value, <return-addr>? }
    Label failure, failurePopR0;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and guard on its group and, if this is a native access, its shape.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICSetElem_DenseOrUnboxedArray::offsetOfGroup()),
                 scratchReg);
    masm.branchTestObjGroup(Assembler::NotEqual, obj, scratchReg, &failure);
    if (unboxedType_ == JSVAL_TYPE_MAGIC) {
        masm.loadPtr(Address(ICStubReg, ICSetElem_DenseOrUnboxedArray::offsetOfShape()),
                     scratchReg);
        masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);
    }

    if (needsUpdateStubs()) {
        // Stow both R0 and R1 (object and key)
        // But R0 and R1 still hold their values.
        EmitStowICValues(masm, 2);

        // Stack is now: { ..., rhs-value, object-value, key-value, maybe?-RET-ADDR }
        // Load rhs-value into R0
        masm.loadValue(Address(masm.getStackPointer(), 2 * sizeof(Value) + ICStackValueOffset), R0);

        // Call the type-update stub.
        if (!callTypeUpdateIC(masm, sizeof(Value)))
            return false;

        // Unstow R0 and R1 (object and key)
        EmitUnstowICValues(masm, 2);

        // Restore object.
        obj = masm.extractObject(R0, ExtractTemp0);

        // Trigger post barriers here on the value being written. Fields which
        // objects can be written to also need update stubs.
        masm.Push(R1);
        masm.loadValue(Address(masm.getStackPointer(), sizeof(Value) + ICStackValueOffset), R1);

        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(R0);
        saveRegs.addUnchecked(obj);
        saveRegs.add(ICStubReg);
        emitPostWriteBarrierSlot(masm, obj, R1, scratchReg, saveRegs);

        masm.Pop(R1);
    }

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    if (unboxedType_ == JSVAL_TYPE_MAGIC) {
        // Set element on a native object.

        // Load obj->elements in scratchReg.
        masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratchReg);

        // Bounds check.
        Address initLength(scratchReg, ObjectElements::offsetOfInitializedLength());
        masm.branch32(Assembler::BelowOrEqual, initLength, key, &failure);

        // Hole check.
        BaseIndex element(scratchReg, key, TimesEight);
        masm.branchTestMagic(Assembler::Equal, element, &failure);

        // Perform a single test to see if we either need to convert double
        // elements or clone the copy on write elements in the object.
        Label noSpecialHandling;
        Address elementsFlags(scratchReg, ObjectElements::offsetOfFlags());
        masm.branchTest32(Assembler::Zero, elementsFlags,
                          Imm32(ObjectElements::CONVERT_DOUBLE_ELEMENTS |
                                ObjectElements::COPY_ON_WRITE),
                          &noSpecialHandling);

        // Fail if we need to clone copy on write elements.
        masm.branchTest32(Assembler::NonZero, elementsFlags,
                          Imm32(ObjectElements::COPY_ON_WRITE),
                          &failure);

        // Failure is not possible now.  Free up registers.
        regs.add(R0);
        regs.add(R1);
        regs.takeUnchecked(obj);
        regs.takeUnchecked(key);

        Address valueAddr(masm.getStackPointer(), ICStackValueOffset);

        // We need to convert int32 values being stored into doubles. In this case
        // the heap typeset is guaranteed to contain both int32 and double, so it's
        // okay to store a double. Note that double arrays are only created by
        // IonMonkey, so if we have no floating-point support Ion is disabled and
        // there should be no double arrays.
        if (cx->runtime()->jitSupportsFloatingPoint)
            masm.convertInt32ValueToDouble(valueAddr, regs.getAny(), &noSpecialHandling);
        else
            masm.assumeUnreachable("There shouldn't be double arrays when there is no FP support.");

        masm.bind(&noSpecialHandling);

        ValueOperand tmpVal = regs.takeAnyValue();
        masm.loadValue(valueAddr, tmpVal);
        EmitPreBarrier(masm, element, MIRType_Value);
        masm.storeValue(tmpVal, element);
    } else {
        // Set element on an unboxed array.

        // Bounds check.
        Address initLength(obj, UnboxedArrayObject::offsetOfCapacityIndexAndInitializedLength());
        masm.load32(initLength, scratchReg);
        masm.and32(Imm32(UnboxedArrayObject::InitializedLengthMask), scratchReg);
        masm.branch32(Assembler::BelowOrEqual, scratchReg, key, &failure);

        // Load obj->elements.
        masm.loadPtr(Address(obj, UnboxedArrayObject::offsetOfElements()), scratchReg);

        // Compute the address being written to.
        BaseIndex address(scratchReg, key, ScaleFromElemWidth(UnboxedTypeSize(unboxedType_)));

        EmitUnboxedPreBarrierForBaseline(masm, address, unboxedType_);

        Address valueAddr(masm.getStackPointer(), ICStackValueOffset + sizeof(Value));
        masm.Push(R0);
        masm.loadValue(valueAddr, R0);
        masm.storeUnboxedProperty(address, unboxedType_,
                                  ConstantOrRegister(TypedOrValueRegister(R0)), &failurePopR0);
        masm.Pop(R0);
    }

    EmitReturnFromIC(masm);

    if (failurePopR0.used()) {
        // Failure case: restore the value of R0
        masm.bind(&failurePopR0);
        masm.popValue(R0);
    }

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// SetElem_DenseOrUnboxedArrayAdd
//

ICUpdatedStub*
ICSetElemDenseOrUnboxedArrayAddCompiler::getStub(ICStubSpace* space)
{
    Rooted<ShapeVector> shapes(cx, ShapeVector(cx));
    if (!shapes.append(obj_->maybeShape()))
        return nullptr;

    if (!GetProtoShapes(obj_, protoChainDepth_, &shapes))
        return nullptr;

    JS_STATIC_ASSERT(ICSetElem_DenseOrUnboxedArrayAdd::MAX_PROTO_CHAIN_DEPTH == 4);

    ICUpdatedStub* stub = nullptr;
    switch (protoChainDepth_) {
      case 0: stub = getStubSpecific<0>(space, shapes); break;
      case 1: stub = getStubSpecific<1>(space, shapes); break;
      case 2: stub = getStubSpecific<2>(space, shapes); break;
      case 3: stub = getStubSpecific<3>(space, shapes); break;
      case 4: stub = getStubSpecific<4>(space, shapes); break;
      default: MOZ_CRASH("ProtoChainDepth too high.");
    }
    if (!stub || !stub->initUpdatingChain(cx, space))
        return nullptr;
    return stub;
}

bool
ICSetElemDenseOrUnboxedArrayAddCompiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // R0 = object
    // R1 = key
    // Stack = { ... rhs-value, <return-addr>? }
    Label failure, failurePopR0, failureUnstow;
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and guard on its group and, if this is a native access, its shape.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICSetElem_DenseOrUnboxedArrayAdd::offsetOfGroup()),
                 scratchReg);
    masm.branchTestObjGroup(Assembler::NotEqual, obj, scratchReg, &failure);
    if (unboxedType_ == JSVAL_TYPE_MAGIC) {
        masm.loadPtr(Address(ICStubReg, ICSetElem_DenseOrUnboxedArrayAddImpl<0>::offsetOfShape(0)),
                     scratchReg);
        masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);
    }

    // Stow both R0 and R1 (object and key)
    // But R0 and R1 still hold their values.
    EmitStowICValues(masm, 2);

    // We may need to free up some registers.
    regs = availableGeneralRegs(0);
    regs.take(R0);
    regs.take(scratchReg);

    // Shape guard objects on the proto chain.
    Register protoReg = regs.takeAny();
    for (size_t i = 0; i < protoChainDepth_; i++) {
        masm.loadObjProto(i == 0 ? obj : protoReg, protoReg);
        masm.branchTestPtr(Assembler::Zero, protoReg, protoReg, &failureUnstow);
        masm.loadPtr(Address(ICStubReg, ICSetElem_DenseOrUnboxedArrayAddImpl<0>::offsetOfShape(i + 1)),
                     scratchReg);
        masm.branchTestObjShape(Assembler::NotEqual, protoReg, scratchReg, &failureUnstow);
    }
    regs.add(protoReg);
    regs.add(scratchReg);

    if (needsUpdateStubs()) {
        // Stack is now: { ..., rhs-value, object-value, key-value, maybe?-RET-ADDR }
        // Load rhs-value in to R0
        masm.loadValue(Address(masm.getStackPointer(), 2 * sizeof(Value) + ICStackValueOffset), R0);

        // Call the type-update stub.
        if (!callTypeUpdateIC(masm, sizeof(Value)))
            return false;
    }

    // Unstow R0 and R1 (object and key)
    EmitUnstowICValues(masm, 2);

    // Restore object.
    obj = masm.extractObject(R0, ExtractTemp0);

    if (needsUpdateStubs()) {
        // Trigger post barriers here on the value being written. Fields which
        // objects can be written to also need update stubs.
        masm.Push(R1);
        masm.loadValue(Address(masm.getStackPointer(), sizeof(Value) + ICStackValueOffset), R1);

        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(R0);
        saveRegs.addUnchecked(obj);
        saveRegs.add(ICStubReg);
        emitPostWriteBarrierSlot(masm, obj, R1, scratchReg, saveRegs);

        masm.Pop(R1);
    }

    // Reset register set.
    regs = availableGeneralRegs(2);
    scratchReg = regs.takeAny();

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    if (unboxedType_ == JSVAL_TYPE_MAGIC) {
        // Adding element to a native object.

        // Load obj->elements in scratchReg.
        masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratchReg);

        // Bounds check (key == initLength)
        Address initLength(scratchReg, ObjectElements::offsetOfInitializedLength());
        masm.branch32(Assembler::NotEqual, initLength, key, &failure);

        // Capacity check.
        Address capacity(scratchReg, ObjectElements::offsetOfCapacity());
        masm.branch32(Assembler::BelowOrEqual, capacity, key, &failure);

        // Check for copy on write elements.
        Address elementsFlags(scratchReg, ObjectElements::offsetOfFlags());
        masm.branchTest32(Assembler::NonZero, elementsFlags,
                          Imm32(ObjectElements::COPY_ON_WRITE),
                          &failure);

        // Failure is not possible now.  Free up registers.
        regs.add(R0);
        regs.add(R1);
        regs.takeUnchecked(obj);
        regs.takeUnchecked(key);

        // Increment initLength before write.
        masm.add32(Imm32(1), initLength);

        // If length is now <= key, increment length before write.
        Label skipIncrementLength;
        Address length(scratchReg, ObjectElements::offsetOfLength());
        masm.branch32(Assembler::Above, length, key, &skipIncrementLength);
        masm.add32(Imm32(1), length);
        masm.bind(&skipIncrementLength);

        // Convert int32 values to double if convertDoubleElements is set. In this
        // case the heap typeset is guaranteed to contain both int32 and double, so
        // it's okay to store a double.
        Label dontConvertDoubles;
        masm.branchTest32(Assembler::Zero, elementsFlags,
                          Imm32(ObjectElements::CONVERT_DOUBLE_ELEMENTS),
                          &dontConvertDoubles);

        Address valueAddr(masm.getStackPointer(), ICStackValueOffset);

        // Note that double arrays are only created by IonMonkey, so if we have no
        // floating-point support Ion is disabled and there should be no double arrays.
        if (cx->runtime()->jitSupportsFloatingPoint)
            masm.convertInt32ValueToDouble(valueAddr, regs.getAny(), &dontConvertDoubles);
        else
            masm.assumeUnreachable("There shouldn't be double arrays when there is no FP support.");
        masm.bind(&dontConvertDoubles);

        // Write the value.  No need for pre-barrier since we're not overwriting an old value.
        ValueOperand tmpVal = regs.takeAnyValue();
        BaseIndex element(scratchReg, key, TimesEight);
        masm.loadValue(valueAddr, tmpVal);
        masm.storeValue(tmpVal, element);
    } else {
        // Adding element to an unboxed array.

        // Bounds check (key == initLength)
        Address initLengthAddr(obj, UnboxedArrayObject::offsetOfCapacityIndexAndInitializedLength());
        masm.load32(initLengthAddr, scratchReg);
        masm.and32(Imm32(UnboxedArrayObject::InitializedLengthMask), scratchReg);
        masm.branch32(Assembler::NotEqual, scratchReg, key, &failure);

        // Capacity check.
        masm.checkUnboxedArrayCapacity(obj, Int32Key(key), scratchReg, &failure);

        // Load obj->elements.
        masm.loadPtr(Address(obj, UnboxedArrayObject::offsetOfElements()), scratchReg);

        // Write the value first, since this can fail. No need for pre-barrier
        // since we're not overwriting an old value.
        masm.Push(R0);
        Address valueAddr(masm.getStackPointer(), ICStackValueOffset + sizeof(Value));
        masm.loadValue(valueAddr, R0);
        BaseIndex address(scratchReg, key, ScaleFromElemWidth(UnboxedTypeSize(unboxedType_)));
        masm.storeUnboxedProperty(address, unboxedType_,
                                  ConstantOrRegister(TypedOrValueRegister(R0)), &failurePopR0);
        masm.Pop(R0);

        // Increment initialized length.
        masm.add32(Imm32(1), initLengthAddr);

        // If length is now <= key, increment length.
        Address lengthAddr(obj, UnboxedArrayObject::offsetOfLength());
        Label skipIncrementLength;
        masm.branch32(Assembler::Above, lengthAddr, key, &skipIncrementLength);
        masm.add32(Imm32(1), lengthAddr);
        masm.bind(&skipIncrementLength);
    }

    EmitReturnFromIC(masm);

    if (failurePopR0.used()) {
        // Failure case: restore the value of R0
        masm.bind(&failurePopR0);
        masm.popValue(R0);
        masm.jump(&failure);
    }

    // Failure case - fail but first unstow R0 and R1
    masm.bind(&failureUnstow);
    EmitUnstowICValues(masm, 2);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// SetElem_TypedArray
//

// Write an arbitrary value to a typed array or typed object address at dest.
// If the value could not be converted to the appropriate format, jump to
// failure or failureModifiedScratch.
template <typename T>
static void
StoreToTypedArray(JSContext* cx, MacroAssembler& masm, Scalar::Type type, Address value, T dest,
                  Register scratch, Label* failure, Label* failureModifiedScratch)
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
            masm.branchTruncateDouble(FloatReg0, scratch, failureModifiedScratch);
            masm.jump(&isInt32);
        } else {
            masm.jump(failure);
        }
    }

    masm.bind(&done);
}

bool
ICSetElem_TypedArray::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;

    if (layout_ != Layout_TypedArray)
        CheckForNeuteredTypedObject(cx, masm, &failure);

    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratchReg = regs.takeAny();

    // Unbox R0 and shape guard.
    Register obj = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICSetElem_TypedArray::offsetOfShape()), scratchReg);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratchReg, &failure);

    // Ensure the index is an integer.
    if (cx->runtime()->jitSupportsFloatingPoint) {
        Label isInt32;
        masm.branchTestInt32(Assembler::Equal, R1, &isInt32);
        {
            // If the index is a double, try to convert it to int32. It's okay
            // to convert -0 to 0: the shape check ensures the object is a typed
            // array so the difference is not observable.
            masm.branchTestDouble(Assembler::NotEqual, R1, &failure);
            masm.unboxDouble(R1, FloatReg0);
            masm.convertDoubleToInt32(FloatReg0, scratchReg, &failure, /* negZeroCheck = */false);
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R1);
        }
        masm.bind(&isInt32);
    } else {
        masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
    }

    // Unbox key.
    Register key = masm.extractInt32(R1, ExtractTemp1);

    // Bounds check.
    Label oobWrite;
    LoadTypedThingLength(masm, layout_, obj, scratchReg);
    masm.branch32(Assembler::BelowOrEqual, scratchReg, key,
                  expectOutOfBounds_ ? &oobWrite : &failure);

    // Load the elements vector.
    LoadTypedThingData(masm, layout_, obj, scratchReg);

    BaseIndex dest(scratchReg, key, ScaleFromElemWidth(Scalar::byteSize(type_)));
    Address value(masm.getStackPointer(), ICStackValueOffset);

    // We need a second scratch register. It's okay to clobber the type tag of
    // R0 or R1, as long as it's restored before jumping to the next stub.
    regs = availableGeneralRegs(0);
    regs.takeUnchecked(obj);
    regs.takeUnchecked(key);
    regs.take(scratchReg);
    Register secondScratch = regs.takeAny();

    Label failureModifiedSecondScratch;
    StoreToTypedArray(cx, masm, type_, value, dest,
                      secondScratch, &failure, &failureModifiedSecondScratch);
    EmitReturnFromIC(masm);

    if (failureModifiedSecondScratch.used()) {
        // Writing to secondScratch may have clobbered R0 or R1, restore them
        // first.
        masm.bind(&failureModifiedSecondScratch);
        masm.tagValue(JSVAL_TYPE_OBJECT, obj, R0);
        masm.tagValue(JSVAL_TYPE_INT32, key, R1);
    }

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    if (expectOutOfBounds_) {
        MOZ_ASSERT(layout_ == Layout_TypedArray);
        masm.bind(&oobWrite);
        EmitReturnFromIC(masm);
    }
    return true;
}

//
// In_Fallback
//

static bool
TryAttachDenseInStub(JSContext* cx, HandleScript script, ICIn_Fallback* stub,
                     HandleValue key, HandleObject obj, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!IsNativeDenseElementAccess(obj, key))
        return true;

    JitSpew(JitSpew_BaselineIC, "  Generating In(Native[Int32] dense) stub");
    ICIn_Dense::Compiler compiler(cx, obj->as<NativeObject>().lastProperty());
    ICStub* denseStub = compiler.getStub(compiler.getStubSpace(script));
    if (!denseStub)
        return false;

    *attached = true;
    stub->addNewStub(denseStub);
    return true;
}

static bool
TryAttachNativeInStub(JSContext* cx, HandleScript script, ICIn_Fallback* stub,
                      HandleValue key, HandleObject obj, bool* attached)
{
    MOZ_ASSERT(!*attached);

    RootedId id(cx);
    if (!IsOptimizableElementPropertyName(cx, key, &id))
        return true;

    RootedPropertyName name(cx, JSID_TO_ATOM(id)->asPropertyName());
    RootedShape shape(cx);
    RootedObject holder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, id, &holder, &shape))
        return false;

    if (IsCacheableGetPropReadSlot(obj, holder, shape)) {
        ICStub::Kind kind = (obj == holder) ? ICStub::In_Native
                                            : ICStub::In_NativePrototype;
        JitSpew(JitSpew_BaselineIC, "  Generating In(Native %s) stub",
                    (obj == holder) ? "direct" : "prototype");
        ICInNativeCompiler compiler(cx, kind, obj, holder, name);
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        *attached = true;
        stub->addNewStub(newStub);
        return true;
    }

    return true;
}

static bool
TryAttachNativeInDoesNotExistStub(JSContext* cx, HandleScript script,
                                  ICIn_Fallback* stub, HandleValue key,
                                  HandleObject obj, bool* attached)
{
    MOZ_ASSERT(!*attached);

    RootedId id(cx);
    if (!IsOptimizableElementPropertyName(cx, key, &id))
        return true;

    // Check if does-not-exist can be confirmed on property.
    RootedPropertyName name(cx, JSID_TO_ATOM(id)->asPropertyName());
    RootedObject lastProto(cx);
    size_t protoChainDepth = SIZE_MAX;
    if (!CheckHasNoSuchProperty(cx, obj, name, &lastProto, &protoChainDepth))
        return true;
    MOZ_ASSERT(protoChainDepth < SIZE_MAX);

    if (protoChainDepth > ICIn_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH)
        return true;

    // Confirmed no-such-property. Add stub.
    JitSpew(JitSpew_BaselineIC, "  Generating In_NativeDoesNotExist stub");
    ICInNativeDoesNotExistCompiler compiler(cx, obj, name, protoChainDepth);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;

    *attached = true;
    stub->addNewStub(newStub);
    return true;
}

static bool
DoInFallback(JSContext* cx, BaselineFrame* frame, ICIn_Fallback* stub_,
             HandleValue key, HandleValue objValue, MutableHandleValue res)
{
    // This fallback stub may trigger debug mode toggling.
    DebugModeOSRVolatileStub<ICIn_Fallback*> stub(frame, stub_);

    FallbackICSpew(cx, stub, "In");

    if (!objValue.isObject()) {
        ReportValueError(cx, JSMSG_IN_NOT_OBJECT, -1, objValue, nullptr);
        return false;
    }

    RootedObject obj(cx, &objValue.toObject());

    bool cond = false;
    if (!OperatorIn(cx, key, obj, &cond))
        return false;
    res.setBoolean(cond);

    if (stub.invalid())
        return true;

    if (stub->numOptimizedStubs() >= ICIn_Fallback::MAX_OPTIMIZED_STUBS)
        return true;

    if (obj->isNative()) {
        RootedScript script(cx, frame->script());
        bool attached = false;
        if (cond) {
            if (!TryAttachDenseInStub(cx, script, stub, key, obj, &attached))
                return false;
            if (attached)
                return true;
            if (!TryAttachNativeInStub(cx, script, stub, key, obj, &attached))
                return false;
            if (attached)
                return true;
        } else {
            if (!TryAttachNativeInDoesNotExistStub(cx, script, stub, key, obj, &attached))
                return false;
            if (attached)
                return true;
        }
    }

    return true;
}

typedef bool (*DoInFallbackFn)(JSContext*, BaselineFrame*, ICIn_Fallback*, HandleValue,
                               HandleValue, MutableHandleValue);
static const VMFunction DoInFallbackInfo =
    FunctionInfo<DoInFallbackFn>(DoInFallback, TailCall, PopValues(2));

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
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoInFallbackInfo, masm);
}

bool
ICInNativeCompiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure, failurePopR0Scratch;

    masm.branchTestString(Assembler::NotEqual, R0, &failure);
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    // Check key identity.
    Register strExtract = masm.extractString(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICInNativeStub::offsetOfName()), scratch);
    masm.branchPtr(Assembler::NotEqual, strExtract, scratch, &failure);

    // Unbox and shape guard object.
    Register objReg = masm.extractObject(R1, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICInNativeStub::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    if (kind == ICStub::In_NativePrototype) {
        // Shape guard holder. Use R0 scrachReg since on x86 there're not enough registers.
        Register holderReg = R0.scratchReg();
        masm.push(R0.scratchReg());
        masm.loadPtr(Address(ICStubReg, ICIn_NativePrototype::offsetOfHolder()),
                     holderReg);
        masm.loadPtr(Address(ICStubReg, ICIn_NativePrototype::offsetOfHolderShape()),
                     scratch);
        masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failurePopR0Scratch);
        masm.addToStackPtr(Imm32(sizeof(size_t)));
    }

    masm.moveValue(BooleanValue(true), R0);

    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failurePopR0Scratch);
    masm.pop(R0.scratchReg());
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

ICStub*
ICInNativeDoesNotExistCompiler::getStub(ICStubSpace* space)
{
    Rooted<ShapeVector> shapes(cx, ShapeVector(cx));
    if (!shapes.append(obj_->as<NativeObject>().lastProperty()))
        return nullptr;

    if (!GetProtoShapes(obj_, protoChainDepth_, &shapes))
        return nullptr;

    JS_STATIC_ASSERT(ICIn_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH == 8);

    ICStub* stub = nullptr;
    switch (protoChainDepth_) {
      case 0: stub = getStubSpecific<0>(space, shapes); break;
      case 1: stub = getStubSpecific<1>(space, shapes); break;
      case 2: stub = getStubSpecific<2>(space, shapes); break;
      case 3: stub = getStubSpecific<3>(space, shapes); break;
      case 4: stub = getStubSpecific<4>(space, shapes); break;
      case 5: stub = getStubSpecific<5>(space, shapes); break;
      case 6: stub = getStubSpecific<6>(space, shapes); break;
      case 7: stub = getStubSpecific<7>(space, shapes); break;
      case 8: stub = getStubSpecific<8>(space, shapes); break;
      default: MOZ_CRASH("ProtoChainDepth too high.");
    }
    if (!stub)
        return nullptr;
    return stub;
}

bool
ICInNativeDoesNotExistCompiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure, failurePopR0Scratch;

    masm.branchTestString(Assembler::NotEqual, R0, &failure);
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

#ifdef DEBUG
    // Ensure that protoChainDepth_ matches the protoChainDepth stored on the stub.
    {
        Label ok;
        masm.load16ZeroExtend(Address(ICStubReg, ICStub::offsetOfExtra()), scratch);
        masm.branch32(Assembler::Equal, scratch, Imm32(protoChainDepth_), &ok);
        masm.assumeUnreachable("Non-matching proto chain depth on stub.");
        masm.bind(&ok);
    }
#endif // DEBUG

    // Check key identity.
    Register strExtract = masm.extractString(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICIn_NativeDoesNotExist::offsetOfName()), scratch);
    masm.branchPtr(Assembler::NotEqual, strExtract, scratch, &failure);

    // Unbox and guard against old shape.
    Register objReg = masm.extractObject(R1, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICIn_NativeDoesNotExist::offsetOfShape(0)),
                 scratch);
    masm.branchTestObjShape(Assembler::NotEqual, objReg, scratch, &failure);

    // Check the proto chain.
    Register protoReg = R0.scratchReg();
    masm.push(R0.scratchReg());
    for (size_t i = 0; i < protoChainDepth_; ++i) {
        masm.loadObjProto(i == 0 ? objReg : protoReg, protoReg);
        masm.branchTestPtr(Assembler::Zero, protoReg, protoReg, &failurePopR0Scratch);
        size_t shapeOffset = ICIn_NativeDoesNotExistImpl<0>::offsetOfShape(i + 1);
        masm.loadPtr(Address(ICStubReg, shapeOffset), scratch);
        masm.branchTestObjShape(Assembler::NotEqual, protoReg, scratch, &failurePopR0Scratch);
    }
    masm.addToStackPtr(Imm32(sizeof(size_t)));

    // Shape and type checks succeeded, ok to proceed.
    masm.moveValue(BooleanValue(false), R0);

    EmitReturnFromIC(masm);

    masm.bind(&failurePopR0Scratch);
    masm.pop(R0.scratchReg());
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICIn_Dense::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;

    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    // Unbox and shape guard object.
    Register obj = masm.extractObject(R1, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICIn_Dense::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, obj, scratch, &failure);

    // Load obj->elements.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    // Unbox key and bounds check.
    Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
    Register key = masm.extractInt32(R0, ExtractTemp0);
    masm.branch32(Assembler::BelowOrEqual, initLength, key, &failure);

    // Hole check.
    JS_STATIC_ASSERT(sizeof(Value) == 8);
    BaseIndex element(scratch, key, TimesEight);
    masm.branchTestMagic(Assembler::Equal, element, &failure);

    masm.moveValue(BooleanValue(true), R0);

    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

// Try to update existing SetProp setter call stubs for the given holder in
// place with a new shape and setter.
static bool
UpdateExistingSetPropCallStubs(ICSetProp_Fallback* fallbackStub,
                               ICStub::Kind kind,
                               NativeObject* holder,
                               JSObject* receiver,
                               JSFunction* setter)
{
    MOZ_ASSERT(kind == ICStub::SetProp_CallScripted ||
               kind == ICStub::SetProp_CallNative);
    MOZ_ASSERT(holder);
    MOZ_ASSERT(receiver);

    bool isOwnSetter = (holder == receiver);
    bool foundMatchingStub = false;
    ReceiverGuard receiverGuard(receiver);
    for (ICStubConstIterator iter = fallbackStub->beginChainConst(); !iter.atEnd(); iter++) {
        if (iter->kind() == kind) {
            ICSetPropCallSetter* setPropStub = static_cast<ICSetPropCallSetter*>(*iter);
            if (setPropStub->holder() == holder && setPropStub->isOwnSetter() == isOwnSetter) {
                // If this is an own setter, update the receiver guard as well,
                // since that's the shape we'll be guarding on. Furthermore,
                // isOwnSetter() relies on holderShape_ and receiverGuard_ being
                // the same shape.
                if (isOwnSetter)
                    setPropStub->receiverGuard().update(receiverGuard);

                MOZ_ASSERT(setPropStub->holderShape() != holder->lastProperty() ||
                           !setPropStub->receiverGuard().matches(receiverGuard),
                           "Why didn't we end up using this stub?");

                // We want to update the holder shape to match the new one no
                // matter what, even if the receiver shape is different.
                setPropStub->holderShape() = holder->lastProperty();

                // Make sure to update the setter, since a shape change might
                // have changed which setter we want to use.
                setPropStub->setter() = setter;
                if (setPropStub->receiverGuard().matches(receiverGuard))
                    foundMatchingStub = true;
            }
        }
    }

    return foundMatchingStub;
}

// Attach an optimized stub for a GETGNAME/CALLGNAME slot-read op.
static bool
TryAttachGlobalNameValueStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                             ICGetName_Fallback* stub, Handle<ClonedBlockObject*> globalLexical,
                             HandlePropertyName name, bool* attached)
{
    MOZ_ASSERT(globalLexical->isGlobal());
    MOZ_ASSERT(!*attached);

    RootedId id(cx, NameToId(name));

    // The property must be found, and it must be found as a normal data property.
    RootedShape shape(cx, globalLexical->lookup(cx, id));
    RootedNativeObject current(cx, globalLexical);
    while (true) {
        shape = current->lookup(cx, id);
        if (shape)
            break;
        if (current == globalLexical) {
            current = &globalLexical->global();
        } else {
            JSObject* proto = current->getProto();
            if (!proto || !proto->is<NativeObject>())
                return true;
            current = &proto->as<NativeObject>();
        }
    }

    // Instantiate this global property, for use during Ion compilation.
    if (IsIonEnabled(cx))
        EnsureTrackPropertyTypes(cx, current, id);

    if (shape->hasDefaultGetter() && shape->hasSlot()) {

        // TODO: if there's a previous stub discard it, or just update its Shape + slot?

        ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
        ICStub* newStub;
        if (current == globalLexical) {
            MOZ_ASSERT(shape->slot() >= current->numFixedSlots());
            uint32_t slot = shape->slot() - current->numFixedSlots();

            JitSpew(JitSpew_BaselineIC, "  Generating GetName(GlobalName lexical) stub");
            ICGetName_GlobalLexical::Compiler compiler(cx, monitorStub, slot);
            newStub = compiler.getStub(compiler.getStubSpace(script));
        } else {
            bool isFixedSlot;
            uint32_t offset;
            GetFixedOrDynamicSlotOffset(shape, &isFixedSlot, &offset);

            // Check the prototype chain from the global to the current
            // prototype. Ignore the global lexical scope as it doesn' figure
            // into the prototype chain. We guard on the global lexical
            // scope's shape independently.
            if (!IsCacheableGetPropReadSlot(&globalLexical->global(), current, shape))
                return true;

            JitSpew(JitSpew_BaselineIC, "  Generating GetName(GlobalName non-lexical) stub");
            ICGetPropNativeCompiler compiler(cx, ICStub::GetName_Global,
                                             ICStubCompiler::Engine::Baseline, monitorStub,
                                             globalLexical, current, name, isFixedSlot, offset,
                                             /* inputDefinitelyObject = */ true);
            newStub = compiler.getStub(compiler.getStubSpace(script));
        }
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
    }
    return true;
}

// Attach an optimized stub for a GETGNAME/CALLGNAME getter op.
static bool
TryAttachGlobalNameAccessorStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                                ICGetName_Fallback* stub, Handle<ClonedBlockObject*> globalLexical,
                                HandlePropertyName name, bool* attached,
                                bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(globalLexical->isGlobal());
    RootedId id(cx, NameToId(name));

    // There must not be a shadowing binding on the global lexical scope.
    if (globalLexical->lookup(cx, id))
        return true;

    RootedGlobalObject global(cx, &globalLexical->global());

    // The property must be found, and it must be found as a normal data property.
    RootedShape shape(cx);
    RootedNativeObject current(cx, global);
    while (true) {
        shape = current->lookup(cx, id);
        if (shape)
            break;
        JSObject* proto = current->getProto();
        if (!proto || !proto->is<NativeObject>())
            return true;
        current = &proto->as<NativeObject>();
    }

    // Instantiate this global property, for use during Ion compilation.
    if (IsIonEnabled(cx))
        EnsureTrackPropertyTypes(cx, current, id);

    // Try to add a getter stub. We don't handle scripted getters yet; if this
    // changes we need to make sure IonBuilder::getPropTryCommonGetter (which
    // requires a Baseline stub) handles non-outerized this objects correctly.
    bool isScripted;
    if (IsCacheableGetPropCall(cx, global, current, shape, &isScripted, isTemporarilyUnoptimizable) &&
        !isScripted)
    {
        ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
        RootedFunction getter(cx, &shape->getterObject()->as<JSFunction>());

        // The CallNativeGlobal stub needs to generate 3 shape checks:
        //
        // 1. The global lexical scope shape check.
        // 2. The global object shape check.
        // 3. The holder shape check.
        //
        // 1 is done as the receiver check, as for GETNAME the global lexical scope is in the
        // receiver position. 2 is done as a manual check that other GetProp stubs don't do. 3 is
        // done as the holder check per normal.
        //
        // In the case the holder is the global object, check 2 is redundant but is not yet
        // optimized away.
        JitSpew(JitSpew_BaselineIC, "  Generating GetName(GlobalName/NativeGetter) stub");
        if (UpdateExistingGetPropCallStubs(stub, ICStub::GetProp_CallNativeGlobal, current,
                                           globalLexical, getter))
        {
            *attached = true;
            return true;
        }
        ICGetPropCallNativeCompiler compiler(cx, ICStub::GetProp_CallNativeGlobal,
                                             ICStubCompiler::Engine::Baseline,
                                             monitorStub, globalLexical, current,
                                             getter, script->pcToOffset(pc),
                                             /* outerClass = */ nullptr,
                                             /* inputDefinitelyObject = */ true);

        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
    }
    return true;
}

static bool
TryAttachScopeNameStub(JSContext* cx, HandleScript script, ICGetName_Fallback* stub,
                       HandleObject initialScopeChain, HandlePropertyName name, bool* attached)
{
    MOZ_ASSERT(!*attached);

    Rooted<ShapeVector> shapes(cx, ShapeVector(cx));
    RootedId id(cx, NameToId(name));
    RootedObject scopeChain(cx, initialScopeChain);

    Shape* shape = nullptr;
    while (scopeChain) {
        if (!shapes.append(scopeChain->maybeShape()))
            return false;

        if (scopeChain->is<GlobalObject>()) {
            shape = scopeChain->as<GlobalObject>().lookup(cx, id);
            if (shape)
                break;
            return true;
        }

        if (!scopeChain->is<ScopeObject>() || scopeChain->is<DynamicWithObject>())
            return true;

        // Check for an 'own' property on the scope. There is no need to
        // check the prototype as non-with scopes do not inherit properties
        // from any prototype.
        shape = scopeChain->as<NativeObject>().lookup(cx, id);
        if (shape)
            break;

        scopeChain = scopeChain->enclosingScope();
    }

    // We don't handle getters here. When this changes, we need to make sure
    // IonBuilder::getPropTryCommonGetter (which requires a Baseline stub to
    // work) handles non-outerized this objects correctly.

    if (!IsCacheableGetPropReadSlot(scopeChain, scopeChain, shape))
        return true;

    bool isFixedSlot;
    uint32_t offset;
    GetFixedOrDynamicSlotOffset(shape, &isFixedSlot, &offset);

    ICStub* monitorStub = stub->fallbackMonitorStub()->firstMonitorStub();
    ICStub* newStub;

    switch (shapes.length()) {
      case 1: {
        ICGetName_Scope<0>::Compiler compiler(cx, monitorStub, Move(shapes.get()), isFixedSlot,
                                              offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 2: {
        ICGetName_Scope<1>::Compiler compiler(cx, monitorStub, Move(shapes.get()), isFixedSlot,
                                              offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 3: {
        ICGetName_Scope<2>::Compiler compiler(cx, monitorStub, Move(shapes.get()), isFixedSlot,
                                              offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 4: {
        ICGetName_Scope<3>::Compiler compiler(cx, monitorStub, Move(shapes.get()), isFixedSlot,
                                              offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 5: {
        ICGetName_Scope<4>::Compiler compiler(cx, monitorStub, Move(shapes.get()), isFixedSlot,
                                              offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 6: {
        ICGetName_Scope<5>::Compiler compiler(cx, monitorStub, Move(shapes.get()), isFixedSlot,
                                              offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      case 7: {
        ICGetName_Scope<6>::Compiler compiler(cx, monitorStub, Move(shapes.get()), isFixedSlot,
                                              offset);
        newStub = compiler.getStub(compiler.getStubSpace(script));
        break;
      }
      default:
        return true;
    }

    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
DoGetNameFallback(JSContext* cx, BaselineFrame* frame, ICGetName_Fallback* stub_,
                  HandleObject scopeChain, MutableHandleValue res)
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
    bool isTemporarilyUnoptimizable = false;

    // Attach new stub.
    if (stub->numOptimizedStubs() >= ICGetName_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with generic stub.
        attached = true;
    }

    if (!attached && IsGlobalOp(JSOp(*pc)) && !script->hasNonSyntacticScope()) {
        if (!TryAttachGlobalNameAccessorStub(cx, script, pc, stub,
                                             scopeChain.as<ClonedBlockObject>(),
                                             name, &attached, &isTemporarilyUnoptimizable))
        {
            return false;
        }
    }

    static_assert(JSOP_GETGNAME_LENGTH == JSOP_GETNAME_LENGTH,
                  "Otherwise our check for JSOP_TYPEOF isn't ok");
    if (JSOp(pc[JSOP_GETGNAME_LENGTH]) == JSOP_TYPEOF) {
        if (!GetScopeNameForTypeOf(cx, scopeChain, name, res))
            return false;
    } else {
        if (!GetScopeName(cx, scopeChain, name, res))
            return false;
    }

    TypeScript::Monitor(cx, script, pc, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, script, res, ICStubCompiler::Engine::Baseline))
        return false;
    if (attached)
        return true;

    if (IsGlobalOp(JSOp(*pc)) && !script->hasNonSyntacticScope()) {
        Handle<ClonedBlockObject*> globalLexical = scopeChain.as<ClonedBlockObject>();
        if (!TryAttachGlobalNameValueStub(cx, script, pc, stub, globalLexical, name, &attached))
            return false;
    } else {
        if (!TryAttachScopeNameStub(cx, script, stub, scopeChain, name, &attached))
            return false;
    }

    if (!attached && !isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();
    return true;
}

typedef bool (*DoGetNameFallbackFn)(JSContext*, BaselineFrame*, ICGetName_Fallback*,
                                    HandleObject, MutableHandleValue);
static const VMFunction DoGetNameFallbackInfo = FunctionInfo<DoGetNameFallbackFn>(DoGetNameFallback, TailCall);

bool
ICGetName_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    masm.push(R0.scratchReg());
    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoGetNameFallbackInfo, masm);
}

bool
ICGetName_GlobalLexical::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    Register obj = R0.scratchReg();
    Register scratch = R1.scratchReg();

    // There's no need to guard on the shape. Lexical bindings are
    // non-configurable, and this stub cannot be shared across globals.

    // Load dynamic slot.
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), obj);
    masm.load32(Address(ICStubReg, ICGetName_GlobalLexical::offsetOfSlot()), scratch);
    masm.loadValue(BaseIndex(obj, scratch, TimesEight), R0);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

template <size_t NumHops>
bool
ICGetName_Scope<NumHops>::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(1));
    Register obj = R0.scratchReg();
    Register walker = regs.takeAny();
    Register scratch = regs.takeAny();

    // Use a local to silence Clang tautological-compare warning if NumHops is 0.
    size_t numHops = NumHops;

    for (size_t index = 0; index < NumHops + 1; index++) {
        Register scope = index ? walker : obj;

        // Shape guard.
        masm.loadPtr(Address(ICStubReg, ICGetName_Scope::offsetOfShape(index)), scratch);
        masm.branchTestObjShape(Assembler::NotEqual, scope, scratch, &failure);

        if (index < numHops)
            masm.extractObject(Address(scope, ScopeObject::offsetOfEnclosingScope()), walker);
    }

    Register scope = NumHops ? walker : obj;

    if (!isFixedSlot_) {
        masm.loadPtr(Address(scope, NativeObject::offsetOfSlots()), walker);
        scope = walker;
    }

    masm.load32(Address(ICStubReg, ICGetName_Scope::offsetOfOffset()), scratch);

    // GETNAME needs to check for uninitialized lexicals.
    BaseIndex slot(scope, scratch, TimesOne);
    masm.branchTestMagic(Assembler::Equal, slot, &failure);
    masm.loadValue(slot, R0);

    // Enter type monitor IC to type-check result.
    EmitEnterTypeMonitorIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// BindName_Fallback
//

static bool
DoBindNameFallback(JSContext* cx, BaselineFrame* frame, ICBindName_Fallback* stub,
                   HandleObject scopeChain, MutableHandleValue res)
{
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    mozilla::DebugOnly<JSOp> op = JSOp(*pc);
    FallbackICSpew(cx, stub, "BindName(%s)", CodeName[JSOp(*pc)]);

    MOZ_ASSERT(op == JSOP_BINDNAME || op == JSOP_BINDGNAME);

    RootedPropertyName name(cx, frame->script()->getName(pc));

    RootedObject scope(cx);
    if (!LookupNameUnqualified(cx, name, scopeChain, &scope))
        return false;

    res.setObject(*scope);
    return true;
}

typedef bool (*DoBindNameFallbackFn)(JSContext*, BaselineFrame*, ICBindName_Fallback*,
                                     HandleObject, MutableHandleValue);
static const VMFunction DoBindNameFallbackInfo =
    FunctionInfo<DoBindNameFallbackFn>(DoBindNameFallback, TailCall);

bool
ICBindName_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    masm.push(R0.scratchReg());
    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

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

    JitSpew(JitSpew_BaselineIC, "  Generating GetIntrinsic optimized stub");
    ICGetIntrinsic_Constant::Compiler compiler(cx, res);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    return true;
}

typedef bool (*DoGetIntrinsicFallbackFn)(JSContext*, BaselineFrame*, ICGetIntrinsic_Fallback*,
                                         MutableHandleValue);
static const VMFunction DoGetIntrinsicFallbackInfo =
    FunctionInfo<DoGetIntrinsicFallbackFn>(DoGetIntrinsicFallback, TailCall);

bool
ICGetIntrinsic_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoGetIntrinsicFallbackInfo, masm);
}

bool
ICGetIntrinsic_Constant::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    masm.loadValue(Address(ICStubReg, ICGetIntrinsic_Constant::offsetOfValue()), R0);

    EmitReturnFromIC(masm);
    return true;
}

//
// SetProp_Fallback
//

// Attach an optimized property set stub for a SETPROP/SETGNAME/SETNAME op on a
// value property.
static bool
TryAttachSetValuePropStub(JSContext* cx, HandleScript script, jsbytecode* pc, ICSetProp_Fallback* stub,
                          HandleObject obj, HandleShape oldShape, HandleObjectGroup oldGroup,
                          HandlePropertyName name, HandleId id, HandleValue rhs, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (obj->watched())
        return true;

    RootedShape shape(cx);
    RootedObject holder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, id, &holder, &shape))
        return false;
    if (obj != holder)
        return true;

    if (!obj->isNative()) {
        if (obj->is<UnboxedPlainObject>()) {
            UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando();
            if (expando) {
                shape = expando->lookup(cx, name);
                if (!shape)
                    return true;
            } else {
                return true;
            }
        } else {
            return true;
        }
    }

    size_t chainDepth;
    if (IsCacheableSetPropAddSlot(cx, obj, oldShape, id, shape, &chainDepth)) {
        // Don't attach if proto chain depth is too high.
        if (chainDepth > ICSetProp_NativeAdd::MAX_PROTO_CHAIN_DEPTH)
            return true;

        // Don't attach if we are adding a property to an object which the new
        // script properties analysis hasn't been performed for yet, as there
        // may be a shape change required here afterwards. Pretend we attached
        // a stub, though, so the access is not marked as unoptimizable.
        if (oldGroup->newScript() && !oldGroup->newScript()->analyzed()) {
            *attached = true;
            return true;
        }

        bool isFixedSlot;
        uint32_t offset;
        GetFixedOrDynamicSlotOffset(shape, &isFixedSlot, &offset);

        JitSpew(JitSpew_BaselineIC, "  Generating SetProp(NativeObject.ADD) stub");
        ICSetPropNativeAddCompiler compiler(cx, obj, oldShape, oldGroup,
                                            chainDepth, isFixedSlot, offset);
        ICUpdatedStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;
        if (!newStub->addUpdateStubForValue(cx, script, obj, id, rhs))
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    if (IsCacheableSetPropWriteSlot(obj, oldShape, shape)) {
        // For some property writes, such as the initial overwrite of global
        // properties, TI will not mark the property as having been
        // overwritten. Don't attach a stub in this case, so that we don't
        // execute another write to the property without TI seeing that write.
        EnsureTrackPropertyTypes(cx, obj, id);
        if (!PropertyHasBeenMarkedNonConstant(obj, id)) {
            *attached = true;
            return true;
        }

        bool isFixedSlot;
        uint32_t offset;
        GetFixedOrDynamicSlotOffset(shape, &isFixedSlot, &offset);

        JitSpew(JitSpew_BaselineIC, "  Generating SetProp(NativeObject.PROP) stub");
        MOZ_ASSERT(LastPropertyForSetProp(obj) == oldShape,
                   "Should this really be a SetPropWriteSlot?");
        ICSetProp_Native::Compiler compiler(cx, obj, isFixedSlot, offset);
        ICSetProp_Native* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;
        if (!newStub->addUpdateStubForValue(cx, script, obj, id, rhs))
            return false;

        if (IsPreliminaryObject(obj))
            newStub->notePreliminaryObject();
        else
            StripPreliminaryObjectStubs(cx, stub);

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    return true;
}

// Attach an optimized property set stub for a SETPROP/SETGNAME/SETNAME op on
// an accessor property.
static bool
TryAttachSetAccessorPropStub(JSContext* cx, HandleScript script, jsbytecode* pc,
                             ICSetProp_Fallback* stub,
                             HandleObject obj, const RootedReceiverGuard& receiverGuard,
                             HandlePropertyName name,
                             HandleId id, HandleValue rhs, bool* attached,
                             bool* isTemporarilyUnoptimizable)
{
    MOZ_ASSERT(!*attached);
    MOZ_ASSERT(!*isTemporarilyUnoptimizable);

    if (obj->watched())
        return true;

    RootedShape shape(cx);
    RootedObject holder(cx);
    if (!EffectlesslyLookupProperty(cx, obj, id, &holder, &shape))
        return false;

    bool isScripted = false;
    bool cacheableCall = IsCacheableSetPropCall(cx, obj, holder, shape,
                                                &isScripted, isTemporarilyUnoptimizable);

    // Try handling scripted setters.
    if (cacheableCall && isScripted) {
        RootedFunction callee(cx, &shape->setterObject()->as<JSFunction>());
        MOZ_ASSERT(callee->hasScript());

        if (UpdateExistingSetPropCallStubs(stub, ICStub::SetProp_CallScripted,
                                           &holder->as<NativeObject>(), obj, callee)) {
            *attached = true;
            return true;
        }

        JitSpew(JitSpew_BaselineIC, "  Generating SetProp(NativeObj/ScriptedSetter %s:%" PRIuSIZE ") stub",
                    callee->nonLazyScript()->filename(), callee->nonLazyScript()->lineno());

        ICSetProp_CallScripted::Compiler compiler(cx, obj, holder, callee, script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    // Try handling JSNative setters.
    if (cacheableCall && !isScripted) {
        RootedFunction callee(cx, &shape->setterObject()->as<JSFunction>());
        MOZ_ASSERT(callee->isNative());

        if (UpdateExistingSetPropCallStubs(stub, ICStub::SetProp_CallNative,
                                           &holder->as<NativeObject>(), obj, callee)) {
            *attached = true;
            return true;
        }

        JitSpew(JitSpew_BaselineIC, "  Generating SetProp(NativeObj/NativeSetter %p) stub",
                    callee->native());

        ICSetProp_CallNative::Compiler compiler(cx, obj, holder, callee, script->pcToOffset(pc));
        ICStub* newStub = compiler.getStub(compiler.getStubSpace(script));
        if (!newStub)
            return false;

        stub->addNewStub(newStub);
        *attached = true;
        return true;
    }

    return true;
}

static bool
TryAttachUnboxedSetPropStub(JSContext* cx, HandleScript script,
                            ICSetProp_Fallback* stub, HandleId id,
                            HandleObject obj, HandleValue rhs, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!cx->runtime()->jitSupportsFloatingPoint)
        return true;

    if (!obj->is<UnboxedPlainObject>())
        return true;

    const UnboxedLayout::Property* property = obj->as<UnboxedPlainObject>().layout().lookup(id);
    if (!property)
        return true;

    ICSetProp_Unboxed::Compiler compiler(cx, obj->group(),
                                         property->offset + UnboxedPlainObject::offsetOfData(),
                                         property->type);
    ICUpdatedStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;
    if (compiler.needsUpdateStubs() && !newStub->addUpdateStubForValue(cx, script, obj, id, rhs))
        return false;

    stub->addNewStub(newStub);

    StripPreliminaryObjectStubs(cx, stub);

    *attached = true;
    return true;
}

static bool
TryAttachTypedObjectSetPropStub(JSContext* cx, HandleScript script,
                                ICSetProp_Fallback* stub, HandleId id,
                                HandleObject obj, HandleValue rhs, bool* attached)
{
    MOZ_ASSERT(!*attached);

    if (!cx->runtime()->jitSupportsFloatingPoint)
        return true;

    if (!obj->is<TypedObject>())
        return true;

    if (!obj->as<TypedObject>().typeDescr().is<StructTypeDescr>())
        return true;
    Rooted<StructTypeDescr*> structDescr(cx);
    structDescr = &obj->as<TypedObject>().typeDescr().as<StructTypeDescr>();

    size_t fieldIndex;
    if (!structDescr->fieldIndex(id, &fieldIndex))
        return true;

    Rooted<TypeDescr*> fieldDescr(cx, &structDescr->fieldDescr(fieldIndex));
    if (!fieldDescr->is<SimpleTypeDescr>())
        return true;

    uint32_t fieldOffset = structDescr->fieldOffset(fieldIndex);

    ICSetProp_TypedObject::Compiler compiler(cx, obj->maybeShape(), obj->group(), fieldOffset,
                                             &fieldDescr->as<SimpleTypeDescr>());
    ICUpdatedStub* newStub = compiler.getStub(compiler.getStubSpace(script));
    if (!newStub)
        return false;
    if (compiler.needsUpdateStubs() && !newStub->addUpdateStubForValue(cx, script, obj, id, rhs))
        return false;

    stub->addNewStub(newStub);

    *attached = true;
    return true;
}

static bool
DoSetPropFallback(JSContext* cx, BaselineFrame* frame, ICSetProp_Fallback* stub_,
                  HandleValue lhs, HandleValue rhs, MutableHandleValue res)
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
        name = ScopeCoordinateName(cx->runtime()->scopeCoordinateNameCache, script, pc);
    else
        name = script->getName(pc);
    RootedId id(cx, NameToId(name));

    RootedObject obj(cx, ToObjectFromStack(cx, lhs));
    if (!obj)
        return false;
    RootedShape oldShape(cx, obj->maybeShape());
    RootedObjectGroup oldGroup(cx, obj->getGroup(cx));
    if (!oldGroup)
        return false;
    RootedReceiverGuard oldGuard(cx, ReceiverGuard(obj));

    if (obj->is<UnboxedPlainObject>()) {
        MOZ_ASSERT(!oldShape);
        if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando())
            oldShape = expando->lastProperty();
    }

    bool attached = false;
    // There are some reasons we can fail to attach a stub that are temporary.
    // We want to avoid calling noteUnoptimizableAccess() if the reason we
    // failed to attach a stub is one of those temporary reasons, since we might
    // end up attaching a stub for the exact same access later.
    bool isTemporarilyUnoptimizable = false;
    if (stub->numOptimizedStubs() < ICSetProp_Fallback::MAX_OPTIMIZED_STUBS &&
        lhs.isObject() &&
        !TryAttachSetAccessorPropStub(cx, script, pc, stub, obj, oldGuard, name, id,
                                      rhs, &attached, &isTemporarilyUnoptimizable))
    {
        return false;
    }

    if (op == JSOP_INITPROP ||
        op == JSOP_INITLOCKEDPROP ||
        op == JSOP_INITHIDDENPROP)
    {
        if (!InitPropertyOperation(cx, op, obj, id, rhs))
            return false;
    } else if (op == JSOP_SETNAME ||
               op == JSOP_STRICTSETNAME ||
               op == JSOP_SETGNAME ||
               op == JSOP_STRICTSETGNAME)
    {
        if (!SetNameOperation(cx, script, pc, obj, rhs))
            return false;
    } else if (op == JSOP_SETALIASEDVAR || op == JSOP_INITALIASEDLEXICAL) {
        obj->as<ScopeObject>().setAliasedVar(cx, ScopeCoordinate(pc), name, rhs);
    } else if (op == JSOP_INITGLEXICAL) {
        RootedValue v(cx, rhs);
        ClonedBlockObject* lexicalScope;
        if (script->hasNonSyntacticScope())
            lexicalScope = &NearestEnclosingExtensibleLexicalScope(frame->scopeChain());
        else
            lexicalScope = &cx->global()->lexicalScope();
        InitGlobalLexicalOperation(cx, lexicalScope, script, pc, v);
    } else {
        MOZ_ASSERT(op == JSOP_SETPROP || op == JSOP_STRICTSETPROP);

        RootedValue v(cx, rhs);
        if (!PutProperty(cx, obj, id, v, op == JSOP_STRICTSETPROP))
            return false;
    }

    // Leave the RHS on the stack.
    res.set(rhs);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    if (stub->numOptimizedStubs() >= ICSetProp_Fallback::MAX_OPTIMIZED_STUBS) {
        // TODO: Discard all stubs in this IC and replace with generic setprop stub.
        return true;
    }

    if (!attached &&
        lhs.isObject() &&
        !TryAttachSetValuePropStub(cx, script, pc, stub, obj, oldShape, oldGroup,
                                   name, id, rhs, &attached))
    {
        return false;
    }
    if (attached)
        return true;

    if (!attached &&
        lhs.isObject() &&
        !TryAttachUnboxedSetPropStub(cx, script, stub, id, obj, rhs, &attached))
    {
        return false;
    }
    if (attached)
        return true;

    if (!attached &&
        lhs.isObject() &&
        !TryAttachTypedObjectSetPropStub(cx, script, stub, id, obj, rhs, &attached))
    {
        return false;
    }
    if (attached)
        return true;

    MOZ_ASSERT(!attached);
    if (!isTemporarilyUnoptimizable)
        stub->noteUnoptimizableAccess();

    return true;
}

typedef bool (*DoSetPropFallbackFn)(JSContext*, BaselineFrame*, ICSetProp_Fallback*,
                                    HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoSetPropFallbackInfo =
    FunctionInfo<DoSetPropFallbackFn>(DoSetPropFallback, TailCall, PopValues(2));

bool
ICSetProp_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(R0 == JSReturnOperand);

    EmitRestoreTailCallReg(masm);

    // Ensure stack is fully synced for the expression decompiler.
    masm.pushValue(R0);
    masm.pushValue(R1);

    // Push arguments.
    masm.pushValue(R1);
    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

    if (!tailCallVM(DoSetPropFallbackInfo, masm))
        return false;

    // Even though the fallback frame doesn't enter a stub frame, the CallScripted
    // frame that we are emulating does. Again, we lie.
#ifdef DEBUG
    EmitRepushTailCallReg(masm);
    EmitStowICValues(masm, 1);
    enterStubFrame(masm, R1.scratchReg());
#else
    inStubFrame_ = true;
#endif

    // What follows is bailout-only code for inlined script getters.
    // The return address pointed to by the baseline stack points here.
    returnOffset_ = masm.currentOffset();

    leaveStubFrame(masm, true);

    // Retrieve the stashed initial argument from the caller's frame before returning
    EmitUnstowICValues(masm, 1);
    EmitReturnFromIC(masm);

    return true;
}

void
ICSetProp_Fallback::Compiler::postGenerateStubCode(MacroAssembler& masm, Handle<JitCode*> code)
{
    cx->compartment()->jitCompartment()->initBaselineSetPropReturnAddr(code->raw() + returnOffset_);
}

static void
GuardGroupAndShapeMaybeUnboxedExpando(MacroAssembler& masm, JSObject* obj,
                                      Register object, Register scratch,
                                      size_t offsetOfGroup, size_t offsetOfShape, Label* failure)
{
    // Guard against object group.
    masm.loadPtr(Address(ICStubReg, offsetOfGroup), scratch);
    masm.branchPtr(Assembler::NotEqual, Address(object, JSObject::offsetOfGroup()), scratch,
                   failure);

    // Guard against shape or expando shape.
    masm.loadPtr(Address(ICStubReg, offsetOfShape), scratch);
    if (obj->is<UnboxedPlainObject>()) {
        Address expandoAddress(object, UnboxedPlainObject::offsetOfExpando());
        masm.branchPtr(Assembler::Equal, expandoAddress, ImmWord(0), failure);
        Label done;
        masm.push(object);
        masm.loadPtr(expandoAddress, object);
        masm.branchTestObjShape(Assembler::Equal, object, scratch, &done);
        masm.pop(object);
        masm.jump(failure);
        masm.bind(&done);
        masm.pop(object);
    } else {
        masm.branchTestObjShape(Assembler::NotEqual, object, scratch, failure);
    }
}

bool
ICSetProp_Native::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    Register objReg = masm.extractObject(R0, ExtractTemp0);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    GuardGroupAndShapeMaybeUnboxedExpando(masm, obj_, objReg, scratch,
                                          ICSetProp_Native::offsetOfGroup(),
                                          ICSetProp_Native::offsetOfShape(),
                                          &failure);

    // Stow both R0 and R1 (object and value).
    EmitStowICValues(masm, 2);

    // Type update stub expects the value to check in R0.
    masm.moveValue(R1, R0);

    // Call the type-update stub.
    if (!callTypeUpdateIC(masm, sizeof(Value)))
        return false;

    // Unstow R0 and R1 (object and key)
    EmitUnstowICValues(masm, 2);

    regs.add(R0);
    regs.takeUnchecked(objReg);

    Register holderReg;
    if (obj_->is<UnboxedPlainObject>()) {
        // We are loading off the expando object, so use that for the holder.
        holderReg = regs.takeAny();
        masm.loadPtr(Address(objReg, UnboxedPlainObject::offsetOfExpando()), holderReg);
        if (!isFixedSlot_)
            masm.loadPtr(Address(holderReg, NativeObject::offsetOfSlots()), holderReg);
    } else if (isFixedSlot_) {
        holderReg = objReg;
    } else {
        holderReg = regs.takeAny();
        masm.loadPtr(Address(objReg, NativeObject::offsetOfSlots()), holderReg);
    }

    // Perform the store.
    masm.load32(Address(ICStubReg, ICSetProp_Native::offsetOfOffset()), scratch);
    EmitPreBarrier(masm, BaseIndex(holderReg, scratch, TimesOne), MIRType_Value);
    masm.storeValue(R1, BaseIndex(holderReg, scratch, TimesOne));
    if (holderReg != objReg)
        regs.add(holderReg);
    if (cx->runtime()->gc.nursery.exists()) {
        Register scr = regs.takeAny();
        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(R1);
        emitPostWriteBarrierSlot(masm, objReg, R1, scr, saveRegs);
        regs.add(scr);
    }

    // The RHS has to be in R0.
    masm.moveValue(R1, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

ICUpdatedStub*
ICSetPropNativeAddCompiler::getStub(ICStubSpace* space)
{
    Rooted<ShapeVector> shapes(cx, ShapeVector(cx));
    if (!shapes.append(oldShape_))
        return nullptr;

    if (!GetProtoShapes(obj_, protoChainDepth_, &shapes))
        return nullptr;

    JS_STATIC_ASSERT(ICSetProp_NativeAdd::MAX_PROTO_CHAIN_DEPTH == 4);

    ICUpdatedStub* stub = nullptr;
    switch(protoChainDepth_) {
      case 0: stub = getStubSpecific<0>(space, shapes); break;
      case 1: stub = getStubSpecific<1>(space, shapes); break;
      case 2: stub = getStubSpecific<2>(space, shapes); break;
      case 3: stub = getStubSpecific<3>(space, shapes); break;
      case 4: stub = getStubSpecific<4>(space, shapes); break;
      default: MOZ_CRASH("ProtoChainDepth too high.");
    }
    if (!stub || !stub->initUpdatingChain(cx, space))
        return nullptr;
    return stub;
}

bool
ICSetPropNativeAddCompiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    Label failureUnstow;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);
    Register objReg = masm.extractObject(R0, ExtractTemp0);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    GuardGroupAndShapeMaybeUnboxedExpando(masm, obj_, objReg, scratch,
                                          ICSetProp_NativeAdd::offsetOfGroup(),
                                          ICSetProp_NativeAddImpl<0>::offsetOfShape(0),
                                          &failure);

    // Stow both R0 and R1 (object and value).
    EmitStowICValues(masm, 2);

    regs = availableGeneralRegs(1);
    scratch = regs.takeAny();
    Register protoReg = regs.takeAny();
    // Check the proto chain.
    for (size_t i = 0; i < protoChainDepth_; i++) {
        masm.loadObjProto(i == 0 ? objReg : protoReg, protoReg);
        masm.branchTestPtr(Assembler::Zero, protoReg, protoReg, &failureUnstow);
        masm.loadPtr(Address(ICStubReg, ICSetProp_NativeAddImpl<0>::offsetOfShape(i + 1)),
                     scratch);
        masm.branchTestObjShape(Assembler::NotEqual, protoReg, scratch, &failureUnstow);
    }

    // Shape and type checks succeeded, ok to proceed.

    // Load RHS into R0 for TypeUpdate check.
    // Stack is currently: [..., ObjValue, RHSValue, MaybeReturnAddr? ]
    masm.loadValue(Address(masm.getStackPointer(), ICStackValueOffset), R0);

    // Call the type-update stub.
    if (!callTypeUpdateIC(masm, sizeof(Value)))
        return false;

    // Unstow R0 and R1 (object and key)
    EmitUnstowICValues(masm, 2);
    regs = availableGeneralRegs(2);
    scratch = regs.takeAny();

    if (obj_->is<PlainObject>()) {
        // Try to change the object's group.
        Label noGroupChange;

        // Check if the cache has a new group to change to.
        masm.loadPtr(Address(ICStubReg, ICSetProp_NativeAdd::offsetOfNewGroup()), scratch);
        masm.branchTestPtr(Assembler::Zero, scratch, scratch, &noGroupChange);

        // Check if the old group still has a newScript.
        masm.loadPtr(Address(objReg, JSObject::offsetOfGroup()), scratch);
        masm.branchPtr(Assembler::Equal,
                       Address(scratch, ObjectGroup::offsetOfAddendum()),
                       ImmWord(0),
                       &noGroupChange);

        // Reload the new group from the cache.
        masm.loadPtr(Address(ICStubReg, ICSetProp_NativeAdd::offsetOfNewGroup()), scratch);

        // Change the object's group.
        Address groupAddr(objReg, JSObject::offsetOfGroup());
        EmitPreBarrier(masm, groupAddr, MIRType_ObjectGroup);
        masm.storePtr(scratch, groupAddr);

        masm.bind(&noGroupChange);
    }

    Register holderReg;
    regs.add(R0);
    regs.takeUnchecked(objReg);

    if (obj_->is<UnboxedPlainObject>()) {
        holderReg = regs.takeAny();
        masm.loadPtr(Address(objReg, UnboxedPlainObject::offsetOfExpando()), holderReg);

        // Write the expando object's new shape.
        Address shapeAddr(holderReg, JSObject::offsetOfShape());
        EmitPreBarrier(masm, shapeAddr, MIRType_Shape);
        masm.loadPtr(Address(ICStubReg, ICSetProp_NativeAdd::offsetOfNewShape()), scratch);
        masm.storePtr(scratch, shapeAddr);

        if (!isFixedSlot_)
            masm.loadPtr(Address(holderReg, NativeObject::offsetOfSlots()), holderReg);
    } else {
        // Write the object's new shape.
        Address shapeAddr(objReg, JSObject::offsetOfShape());
        EmitPreBarrier(masm, shapeAddr, MIRType_Shape);
        masm.loadPtr(Address(ICStubReg, ICSetProp_NativeAdd::offsetOfNewShape()), scratch);
        masm.storePtr(scratch, shapeAddr);

        if (isFixedSlot_) {
            holderReg = objReg;
        } else {
            holderReg = regs.takeAny();
            masm.loadPtr(Address(objReg, NativeObject::offsetOfSlots()), holderReg);
        }
    }

    // Perform the store.  No write barrier required since this is a new
    // initialization.
    masm.load32(Address(ICStubReg, ICSetProp_NativeAdd::offsetOfOffset()), scratch);
    masm.storeValue(R1, BaseIndex(holderReg, scratch, TimesOne));

    if (holderReg != objReg)
        regs.add(holderReg);

    if (cx->runtime()->gc.nursery.exists()) {
        Register scr = regs.takeAny();
        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(R1);
        emitPostWriteBarrierSlot(masm, objReg, R1, scr, saveRegs);
    }

    // The RHS has to be in R0.
    masm.moveValue(R1, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failureUnstow);
    EmitUnstowICValues(masm, 2);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICSetProp_Unboxed::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    // Unbox and group guard.
    Register object = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICSetProp_Unboxed::offsetOfGroup()), scratch);
    masm.branchPtr(Assembler::NotEqual, Address(object, JSObject::offsetOfGroup()), scratch,
                   &failure);

    if (needsUpdateStubs()) {
        // Stow both R0 and R1 (object and value).
        masm.push(object);
        masm.push(ICStubReg);
        EmitStowICValues(masm, 2);

        // Move RHS into R0 for TypeUpdate check.
        masm.moveValue(R1, R0);

        // Call the type update stub.
        if (!callTypeUpdateIC(masm, sizeof(Value)))
            return false;

        // Unstow R0 and R1 (object and key)
        EmitUnstowICValues(masm, 2);
        masm.pop(ICStubReg);
        masm.pop(object);

        // Trigger post barriers here on the values being written. Fields which
        // objects can be written to also need update stubs.
        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(R0);
        saveRegs.add(R1);
        saveRegs.addUnchecked(object);
        saveRegs.add(ICStubReg);
        emitPostWriteBarrierSlot(masm, object, R1, scratch, saveRegs);
    }

    // Compute the address being written to.
    masm.load32(Address(ICStubReg, ICSetProp_Unboxed::offsetOfFieldOffset()), scratch);
    BaseIndex address(object, scratch, TimesOne);

    EmitUnboxedPreBarrierForBaseline(masm, address, fieldType_);
    masm.storeUnboxedProperty(address, fieldType_,
                              ConstantOrRegister(TypedOrValueRegister(R1)), &failure);

    // The RHS has to be in R0.
    masm.moveValue(R1, R0);

    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICSetProp_TypedObject::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;

    CheckForNeuteredTypedObject(cx, masm, &failure);

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(2));
    Register scratch = regs.takeAny();

    // Unbox and shape guard.
    Register object = masm.extractObject(R0, ExtractTemp0);
    masm.loadPtr(Address(ICStubReg, ICSetProp_TypedObject::offsetOfShape()), scratch);
    masm.branchTestObjShape(Assembler::NotEqual, object, scratch, &failure);

    // Guard that the object group matches.
    masm.loadPtr(Address(ICStubReg, ICSetProp_TypedObject::offsetOfGroup()), scratch);
    masm.branchPtr(Assembler::NotEqual, Address(object, JSObject::offsetOfGroup()), scratch,
                   &failure);

    if (needsUpdateStubs()) {
        // Stow both R0 and R1 (object and value).
        masm.push(object);
        masm.push(ICStubReg);
        EmitStowICValues(masm, 2);

        // Move RHS into R0 for TypeUpdate check.
        masm.moveValue(R1, R0);

        // Call the type update stub.
        if (!callTypeUpdateIC(masm, sizeof(Value)))
            return false;

        // Unstow R0 and R1 (object and key)
        EmitUnstowICValues(masm, 2);
        masm.pop(ICStubReg);
        masm.pop(object);

        // Trigger post barriers here on the values being written. Descriptors
        // which can write objects also need update stubs.
        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(R0);
        saveRegs.add(R1);
        saveRegs.addUnchecked(object);
        saveRegs.add(ICStubReg);
        emitPostWriteBarrierSlot(masm, object, R1, scratch, saveRegs);
    }

    // Save the rhs on the stack so we can get a second scratch register.
    Label failurePopRHS;
    masm.pushValue(R1);
    regs = availableGeneralRegs(1);
    regs.takeUnchecked(object);
    regs.take(scratch);
    Register secondScratch = regs.takeAny();

    // Get the object's data pointer.
    LoadTypedThingData(masm, layout_, object, scratch);

    // Compute the address being written to.
    masm.load32(Address(ICStubReg, ICSetProp_TypedObject::offsetOfFieldOffset()), secondScratch);
    masm.addPtr(secondScratch, scratch);

    Address dest(scratch, 0);
    Address value(masm.getStackPointer(), 0);

    if (fieldDescr_->is<ScalarTypeDescr>()) {
        Scalar::Type type = fieldDescr_->as<ScalarTypeDescr>().type();
        StoreToTypedArray(cx, masm, type, value, dest,
                          secondScratch, &failurePopRHS, &failurePopRHS);
        masm.popValue(R1);
        EmitReturnFromIC(masm);
    } else {
        ReferenceTypeDescr::Type type = fieldDescr_->as<ReferenceTypeDescr>().type();

        masm.popValue(R1);

        switch (type) {
          case ReferenceTypeDescr::TYPE_ANY:
            EmitPreBarrier(masm, dest, MIRType_Value);
            masm.storeValue(R1, dest);
            break;

          case ReferenceTypeDescr::TYPE_OBJECT: {
            EmitPreBarrier(masm, dest, MIRType_Object);
            Label notObject;
            masm.branchTestObject(Assembler::NotEqual, R1, &notObject);
            Register rhsObject = masm.extractObject(R1, ExtractTemp0);
            masm.storePtr(rhsObject, dest);
            EmitReturnFromIC(masm);
            masm.bind(&notObject);
            masm.branchTestNull(Assembler::NotEqual, R1, &failure);
            masm.storePtr(ImmWord(0), dest);
            break;
          }

          case ReferenceTypeDescr::TYPE_STRING: {
            EmitPreBarrier(masm, dest, MIRType_String);
            masm.branchTestString(Assembler::NotEqual, R1, &failure);
            Register rhsString = masm.extractString(R1, ExtractTemp0);
            masm.storePtr(rhsString, dest);
            break;
          }

          default:
            MOZ_CRASH();
        }

        EmitReturnFromIC(masm);
    }

    masm.bind(&failurePopRHS);
    masm.popValue(R1);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICSetProp_CallScripted::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    Label failureUnstow;
    Label failureLeaveStubFrame;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Stow R0 and R1 to free up registers.
    EmitStowICValues(masm, 2);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(1));
    Register scratch = regs.takeAnyExcluding(ICTailCallReg);

    // Unbox and shape guard.
    uint32_t framePushed = masm.framePushed();
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    GuardReceiverObject(masm, ReceiverGuard(receiver_), objReg, scratch,
                        ICSetProp_CallScripted::offsetOfReceiverGuard(), &failureUnstow);

    if (receiver_ != holder_) {
        Register holderReg = regs.takeAny();
        masm.loadPtr(Address(ICStubReg, ICSetProp_CallScripted::offsetOfHolder()), holderReg);
        masm.loadPtr(Address(ICStubReg, ICSetProp_CallScripted::offsetOfHolderShape()), scratch);
        masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failureUnstow);
        regs.add(holderReg);
    }

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, scratch);

    // Load callee function and code.  To ensure that |code| doesn't end up being
    // ArgumentsRectifierReg, if it's available we assign it to |callee| instead.
    Register callee;
    if (regs.has(ArgumentsRectifierReg)) {
        callee = ArgumentsRectifierReg;
        regs.take(callee);
    } else {
        callee = regs.takeAny();
    }
    Register code = regs.takeAny();
    masm.loadPtr(Address(ICStubReg, ICSetProp_CallScripted::offsetOfSetter()), callee);
    masm.branchIfFunctionHasNoScript(callee, &failureLeaveStubFrame);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), code);
    masm.loadBaselineOrIonRaw(code, code, &failureLeaveStubFrame);

    // Align the stack such that the JitFrameLayout is aligned on
    // JitStackAlignment.
    masm.alignJitStackBasedOnNArgs(1);

    // Setter is called with the new value as the only argument, and |obj| as thisv.
    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.

    // To Push R1, read it off of the stowed values on stack.
    // Stack: [ ..., R0, R1, ..STUBFRAME-HEADER.., padding? ]
    masm.PushValue(Address(BaselineFrameReg, STUB_FRAME_SIZE));
    masm.Push(R0);
    EmitBaselineCreateStubFrameDescriptor(masm, scratch);
    masm.Push(Imm32(1));  // ActualArgc is 1
    masm.Push(callee);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), scratch);
    masm.branch32(Assembler::BelowOrEqual, scratch, Imm32(1), &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != code);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.movePtr(ImmWord(1), ArgumentsRectifierReg);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    uint32_t framePushedAfterCall = masm.framePushed();

    leaveStubFrame(masm, true);
    // Do not care about return value from function. The original RHS should be returned
    // as the result of this operation.
    EmitUnstowICValues(masm, 2);
    masm.moveValue(R1, R0);
    EmitReturnFromIC(masm);

    // Leave stub frame and go to next stub.
    masm.bind(&failureLeaveStubFrame);
    masm.setFramePushed(framePushedAfterCall);
    inStubFrame_ = true;
    leaveStubFrame(masm, false);

    // Unstow R0 and R1
    masm.bind(&failureUnstow);
    masm.setFramePushed(framePushed);
    EmitUnstowICValues(masm, 2);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

static bool
DoCallNativeSetter(JSContext* cx, HandleFunction callee, HandleObject obj, HandleValue val)
{
    MOZ_ASSERT(callee->isNative());
    JSNative natfun = callee->native();

    JS::AutoValueArray<3> vp(cx);
    vp[0].setObject(*callee.get());
    vp[1].setObject(*obj.get());
    vp[2].set(val);

    return natfun(cx, 1, vp.begin());
}

typedef bool (*DoCallNativeSetterFn)(JSContext*, HandleFunction, HandleObject, HandleValue);
static const VMFunction DoCallNativeSetterInfo =
    FunctionInfo<DoCallNativeSetterFn>(DoCallNativeSetter);

bool
ICSetProp_CallNative::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    Label failureUnstow;

    // Guard input is an object.
    masm.branchTestObject(Assembler::NotEqual, R0, &failure);

    // Stow R0 and R1 to free up registers.
    EmitStowICValues(masm, 2);

    AllocatableGeneralRegisterSet regs(availableGeneralRegs(1));
    Register scratch = regs.takeAnyExcluding(ICTailCallReg);

    // Unbox and shape guard.
    uint32_t framePushed = masm.framePushed();
    Register objReg = masm.extractObject(R0, ExtractTemp0);
    GuardReceiverObject(masm, ReceiverGuard(receiver_), objReg, scratch,
                        ICSetProp_CallNative::offsetOfReceiverGuard(), &failureUnstow);

    if (receiver_ != holder_) {
        Register holderReg = regs.takeAny();
        masm.loadPtr(Address(ICStubReg, ICSetProp_CallNative::offsetOfHolder()), holderReg);
        masm.loadPtr(Address(ICStubReg, ICSetProp_CallNative::offsetOfHolderShape()), scratch);
        masm.branchTestObjShape(Assembler::NotEqual, holderReg, scratch, &failureUnstow);
        regs.add(holderReg);
    }

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, scratch);

    // Load callee function and code.  To ensure that |code| doesn't end up being
    // ArgumentsRectifierReg, if it's available we assign it to |callee| instead.
    Register callee = regs.takeAny();
    masm.loadPtr(Address(ICStubReg, ICSetProp_CallNative::offsetOfSetter()), callee);

    // To Push R1, read it off of the stowed values on stack.
    // Stack: [ ..., R0, R1, ..STUBFRAME-HEADER.. ]
    masm.moveStackPtrTo(scratch);
    masm.pushValue(Address(scratch, STUB_FRAME_SIZE));
    masm.push(objReg);
    masm.push(callee);

    // Don't need to preserve R0 anymore.
    regs.add(R0);

    if (!callVM(DoCallNativeSetterInfo, masm))
        return false;
    leaveStubFrame(masm);

    // Do not care about return value from function. The original RHS should be returned
    // as the result of this operation.
    EmitUnstowICValues(masm, 2);
    masm.moveValue(R1, R0);
    EmitReturnFromIC(masm);

    // Unstow R0 and R1
    masm.bind(&failureUnstow);
    masm.setFramePushed(framePushed);
    EmitUnstowICValues(masm, 2);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// Call_Fallback
//

static bool
TryAttachFunApplyStub(JSContext* cx, ICCall_Fallback* stub, HandleScript script, jsbytecode* pc,
                      HandleValue thisv, uint32_t argc, Value* argv, bool* attached)
{
    if (argc != 2)
        return true;

    if (!thisv.isObject() || !thisv.toObject().is<JSFunction>())
        return true;
    RootedFunction target(cx, &thisv.toObject().as<JSFunction>());

    bool isScripted = target->hasJITCode();

    // right now, only handle situation where second argument is |arguments|
    if (argv[1].isMagic(JS_OPTIMIZED_ARGUMENTS) && !script->needsArgsObj()) {
        if (isScripted && !stub->hasStub(ICStub::Call_ScriptedApplyArguments)) {
            JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedApplyArguments stub");

            ICCall_ScriptedApplyArguments::Compiler compiler(
                cx, stub->fallbackMonitorStub()->firstMonitorStub(), script->pcToOffset(pc));
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
        if (isScripted && !stub->hasStub(ICStub::Call_ScriptedApplyArray)) {
            JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedApplyArray stub");

            ICCall_ScriptedApplyArray::Compiler compiler(
                cx, stub->fallbackMonitorStub()->firstMonitorStub(), script->pcToOffset(pc));
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
                     HandleValue thisv, bool* attached)
{
    // Try to attach a stub for Function.prototype.call with scripted |this|.

    *attached = false;
    if (!thisv.isObject() || !thisv.toObject().is<JSFunction>())
        return true;
    RootedFunction target(cx, &thisv.toObject().as<JSFunction>());

    // Attach a stub if the script can be Baseline-compiled. We do this also
    // if the script is not yet compiled to avoid attaching a CallNative stub
    // that handles everything, even after the callee becomes hot.
    if (target->hasScript() && target->nonLazyScript()->canBaselineCompile() &&
        !stub->hasStub(ICStub::Call_ScriptedFunCall))
    {
        JitSpew(JitSpew_BaselineIC, "  Generating Call_ScriptedFunCall stub");

        ICCall_ScriptedFunCall::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
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

static bool
GetTemplateObjectForNative(JSContext* cx, Native native, const CallArgs& args,
                           MutableHandleObject res, bool* skipAttach)
{
    // Check for natives to which template objects can be attached. This is
    // done to provide templates to Ion for inlining these natives later on.

    if (native == ArrayConstructor) {
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

            // With this and other array templates, set forceAnalyze so that we
            // don't end up with a template whose structure might change later.
            res.set(NewFullyAllocatedArrayForCallingAllocationSite(cx, count, TenuredObject));
            if (!res)
                return false;
            return true;
        }
    }

    if (native == js::array_concat || native == js::array_slice) {
        if (args.thisv().isObject()) {
            JSObject* obj = &args.thisv().toObject();
            if (!obj->isSingleton()) {
                if (obj->group()->maybePreliminaryObjects()) {
                    *skipAttach = true;
                    return true;
                }
                res.set(NewFullyAllocatedArrayTryReuseGroup(cx, &args.thisv().toObject(), 0,
                                                            TenuredObject));
                return !!res;
            }
        }
    }

    if (native == js::str_split && args.length() == 1 && args[0].isString()) {
        ObjectGroup* group = ObjectGroup::callingAllocationSiteGroup(cx, JSProto_Array);
        if (!group)
            return false;
        if (group->maybePreliminaryObjects()) {
            *skipAttach = true;
            return true;
        }

        res.set(NewFullyAllocatedArrayForCallingAllocationSite(cx, 0, TenuredObject));
        if (!res)
            return false;
        return true;
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

    if (JitSupportsSimd()) {
#define ADD_INT32X4_SIMD_OP_NAME_(OP) || native == js::simd_int32x4_##OP
#define ADD_FLOAT32X4_SIMD_OP_NAME_(OP) || native == js::simd_float32x4_##OP
       if (false
           ION_COMMONX4_SIMD_OP(ADD_INT32X4_SIMD_OP_NAME_)
           COMP_COMMONX4_TO_INT32X4_SIMD_OP(ADD_INT32X4_SIMD_OP_NAME_)
           COMP_COMMONX4_TO_INT32X4_SIMD_OP(ADD_FLOAT32X4_SIMD_OP_NAME_)
           FOREACH_INT32X4_SIMD_OP(ADD_INT32X4_SIMD_OP_NAME_))
       {
            Rooted<SimdTypeDescr*> descr(cx, cx->global()->getOrCreateSimdTypeDescr<Int32x4>(cx));
            res.set(cx->compartment()->jitCompartment()->getSimdTemplateObjectFor(cx, descr));
            return !!res;
       }
       if (false
           FOREACH_FLOAT32X4_SIMD_OP(ADD_FLOAT32X4_SIMD_OP_NAME_)
           ION_COMMONX4_SIMD_OP(ADD_FLOAT32X4_SIMD_OP_NAME_))
       {
            Rooted<SimdTypeDescr*> descr(cx, cx->global()->getOrCreateSimdTypeDescr<Float32x4>(cx));
            res.set(cx->compartment()->jitCompartment()->getSimdTemplateObjectFor(cx, descr));
            return !!res;
       }
#undef ADD_INT32X4_SIMD_OP_NAME_
#undef ADD_FLOAT32X4_SIMD_OP_NAME_
    }

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
IsOptimizableCallStringSplit(Value callee, Value thisv, int argc, Value* args)
{
    if (argc != 1 || !thisv.isString() || !args[0].isString())
        return false;

    if (!thisv.toString()->isAtom() || !args[0].toString()->isAtom())
        return false;

    if (!callee.isObject() || !callee.toObject().is<JSFunction>())
        return false;

    JSFunction& calleeFun = callee.toObject().as<JSFunction>();
    if (!calleeFun.isNative() || calleeFun.native() != js::str_split)
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
    // optimized StringSplit stub.
    if (stub->numOptimizedStubs() == 0 && IsOptimizableCallStringSplit(callee, thisv, argc, vp + 2))
        return true;

    MOZ_ASSERT_IF(stub->hasStub(ICStub::Call_StringSplit), stub->numOptimizedStubs() == 1);

    stub->unlinkStubsWithKind(cx, ICStub::Call_StringSplit);

    if (!callee.isObject())
        return true;

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
                ICCall_ClassHook::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
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

    if (fun->hasScript()) {
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

        if (!fun->hasJITCode()) {
            // Don't treat this as an unoptimizable case, as we'll add a stub
            // when the callee becomes hot.
            *handled = true;
            return true;
        }

        // Check if this stub chain has already generalized scripted calls.
        if (stub->scriptedStubsAreGeneralized()) {
            JitSpew(JitSpew_BaselineIC, "  Chain already has generalized scripted call stub!");
            return true;
        }

        if (stub->scriptedStubCount() >= ICCall_Fallback::MAX_SCRIPTED_STUBS) {
            // Create a Call_AnyScripted stub.
            JitSpew(JitSpew_BaselineIC, "  Generating Call_AnyScripted stub (cons=%s, spread=%s)",
                    constructing ? "yes" : "no", isSpread ? "yes" : "no");
            ICCallScriptedCompiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
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

        JitSpew(JitSpew_BaselineIC,
                "  Generating Call_Scripted stub (fun=%p, %s:%" PRIuSIZE ", cons=%s, spread=%s)",
                fun.get(), fun->nonLazyScript()->filename(), fun->nonLazyScript()->lineno(),
                constructing ? "yes" : "no", isSpread ? "yes" : "no");
        ICCallScriptedCompiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
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
            if (fun->native() == fun_apply)
                return TryAttachFunApplyStub(cx, stub, script, pc, thisv, argc, vp + 2, handled);

            // Don't try to attach a "regular" optimized call stubs for FUNAPPLY ops,
            // since MagicArguments may escape through them.
            return true;
        }

        if (op == JSOP_FUNCALL && fun->native() == fun_call) {
            if (!TryAttachFunCallStub(cx, stub, script, pc, thisv, handled))
                return false;
            if (*handled)
                return true;
        }

        if (stub->nativeStubCount() >= ICCall_Fallback::MAX_NATIVE_STUBS) {
            JitSpew(JitSpew_BaselineIC,
                    "  Too many Call_Native stubs. TODO: add Call_AnyNative!");
            return true;
        }

        if (fun->native() == intrinsic_IsSuspendedStarGenerator) {
            // This intrinsic only appears in self-hosted code.
            MOZ_ASSERT(op != JSOP_NEW);
            MOZ_ASSERT(argc == 1);
            JitSpew(JitSpew_BaselineIC, "  Generating Call_IsSuspendedStarGenerator stub");

            ICCall_IsSuspendedStarGenerator::Compiler compiler(cx);
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
            if (!GetTemplateObjectForNative(cx, fun->native(), args, &templateObject, &skipAttach))
                return false;
            if (skipAttach) {
                *handled = true;
                return true;
            }
            MOZ_ASSERT_IF(templateObject, !templateObject->group()->maybePreliminaryObjects());
        }

        JitSpew(JitSpew_BaselineIC, "  Generating Call_Native stub (fun=%p, cons=%s, spread=%s)",
                fun.get(), constructing ? "yes" : "no", isSpread ? "yes" : "no");
        ICCall_Native::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                         fun, templateObject, constructing, isSpread,
                                         script->pcToOffset(pc));
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
CopyArray(JSContext* cx, HandleObject obj, MutableHandleValue result)
{
    uint32_t length = GetAnyBoxedOrUnboxedArrayLength(obj);
    JSObject* nobj = NewFullyAllocatedArrayTryReuseGroup(cx, obj, length, TenuredObject,
                                                         /* forceAnalyze = */ true);
    if (!nobj)
        return false;
    CopyAnyBoxedOrUnboxedDenseElements(cx, nobj, obj, 0, 0, length);

    result.setObject(*nobj);
    return true;
}

static bool
TryAttachStringSplit(JSContext* cx, ICCall_Fallback* stub, HandleScript script,
                     uint32_t argc, Value* vp, jsbytecode* pc, HandleValue res,
                     bool* attached)
{
    if (stub->numOptimizedStubs() != 0)
        return true;

    RootedValue callee(cx, vp[0]);
    RootedValue thisv(cx, vp[1]);
    Value* args = vp + 2;

    // String.prototype.split will not yield a constructable.
    if (JSOp(*pc) == JSOP_NEW)
        return true;

    if (!IsOptimizableCallStringSplit(callee, thisv, argc, args))
        return true;

    MOZ_ASSERT(callee.isObject());
    MOZ_ASSERT(callee.toObject().is<JSFunction>());

    RootedString thisString(cx, thisv.toString());
    RootedString argString(cx, args[0].toString());
    RootedObject obj(cx, &res.toObject());
    RootedValue arr(cx);

    // Copy the array before storing in stub.
    if (!CopyArray(cx, obj, &arr))
        return false;

    // Atomize all elements of the array.
    RootedObject arrObj(cx, &arr.toObject());
    uint32_t initLength = GetAnyBoxedOrUnboxedArrayLength(arrObj);
    for (uint32_t i = 0; i < initLength; i++) {
        JSAtom* str = js::AtomizeString(cx, GetAnyBoxedOrUnboxedDenseElement(arrObj, i).toString());
        if (!str)
            return false;

        if (!SetAnyBoxedOrUnboxedDenseElement(cx, arrObj, i, StringValue(str))) {
            // The value could not be stored to an unboxed dense element.
            return true;
        }
    }

    ICCall_StringSplit::Compiler compiler(cx, stub->fallbackMonitorStub()->firstMonitorStub(),
                                          script->pcToOffset(pc), thisString, argString,
                                          arr);
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
    bool constructing = (op == JSOP_NEW);

    // Ensure vp array is rooted - we may GC in here.
    AutoArrayRooter vpRoot(cx, argc + 2 + constructing, vp);

    RootedValue callee(cx, vp[0]);
    RootedValue thisv(cx, vp[1]);

    Value* args = vp + 2;

    // Handle funapply with JSOP_ARGUMENTS
    if (op == JSOP_FUNAPPLY && argc == 2 && args[1].isMagic(JS_OPTIMIZED_ARGUMENTS)) {
        CallArgs callArgs = CallArgsFromVp(argc, vp);
        if (!GuardFunApplyArgumentsOptimization(cx, frame, callArgs))
            return false;
    }

    bool createSingleton = ObjectGroup::useSingletonForNewObject(cx, script, pc);

    // Try attaching a call stub.
    bool handled = false;
    if (!TryAttachCallStub(cx, stub, script, pc, op, argc, vp, constructing, false,
                           createSingleton, &handled))
    {
        return false;
    }

    if (op == JSOP_NEW) {
        // Callees from the stack could have any old non-constructor callee.
        if (!IsConstructor(callee)) {
            ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, callee, nullptr);
            return false;
        }

        ConstructArgs cargs(cx);
        if (!cargs.init(argc))
            return false;

        for (uint32_t i = 0; i < argc; i++)
            cargs[i].set(args[i]);

        RootedValue newTarget(cx, args[argc]);
        MOZ_ASSERT(IsConstructor(newTarget),
                   "either callee == newTarget, or the initial |new| checked "
                   "that IsConstructor(newTarget)");

        if (!Construct(cx, callee, cargs, newTarget, res))
            return false;
    } else if ((op == JSOP_EVAL || op == JSOP_STRICTEVAL) &&
               frame->scopeChain()->global().valueIsEval(callee))
    {
        if (!DirectEval(cx, CallArgsFromVp(argc, vp)))
            return false;
        res.set(vp[0]);
    } else {
        MOZ_ASSERT(op == JSOP_CALL ||
                   op == JSOP_CALLITER ||
                   op == JSOP_FUNCALL ||
                   op == JSOP_FUNAPPLY ||
                   op == JSOP_EVAL ||
                   op == JSOP_STRICTEVAL);
        if (op == JSOP_CALLITER && callee.isPrimitive()) {
            MOZ_ASSERT(argc == 0, "thisv must be on top of the stack");
            ReportValueError(cx, JSMSG_NOT_ITERABLE, -1, thisv, nullptr);
            return false;
        }
        if (!Invoke(cx, thisv, callee, argc, args, res))
            return false;
    }

    TypeScript::Monitor(cx, script, pc, res);

    // Check if debug mode toggling made the stub invalid.
    if (stub.invalid())
        return true;

    // Attach a new TypeMonitor stub for this value.
    ICTypeMonitor_Fallback* typeMonFbStub = stub->fallbackMonitorStub();
    if (!typeMonFbStub->addMonitorStubForValue(cx, script, res,
        ICStubCompiler::Engine::Baseline))
    {
        return false;
    }

    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, script, res, ICStubCompiler::Engine::Baseline))
        return false;

    // If 'callee' is a potential Call_StringSplit, try to attach an
    // optimized StringSplit stub.
    if (!TryAttachStringSplit(cx, stub, script, argc, vp, pc, res, &handled))
        return false;

    if (!handled)
        stub->noteUnoptimizableCall();
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
    bool constructing = (op == JSOP_SPREADNEW);
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

    // Attach a new TypeMonitor stub for this value.
    ICTypeMonitor_Fallback* typeMonFbStub = stub->fallbackMonitorStub();
    if (!typeMonFbStub->addMonitorStubForValue(cx, script, res,
        ICStubCompiler::Engine::Baseline))
    {
        return false;
    }
    // Add a type monitor stub for the resulting value.
    if (!stub->addMonitorStubForValue(cx, script, res, ICStubCompiler::Engine::Baseline))
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

// (see Bug 1149377 comment 31) MSVC 2013 PGO miss-compiles branchTestObjClass
// calls from this function.
#if defined(_MSC_VER) && _MSC_VER == 1800
# pragma optimize("g", off)
#endif
Register
ICCallStubCompiler::guardFunApply(MacroAssembler& masm, AllocatableGeneralRegisterSet regs,
                                  Register argcReg, bool checkNative, FunApplyThing applyThing,
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

        masm.branchTestObjClass(Assembler::NotEqual, secondArgObj, regsx.getAny(),
                                &ArrayObject::class_, failure);

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
        JS_STATIC_ASSERT(sizeof(Value) == 8);
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

    masm.branchTestObjClass(Assembler::NotEqual, callee, regs.getAny(), &JSFunction::class_,
                            failure);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);

    masm.branchPtr(Assembler::NotEqual, callee, ImmPtr(fun_apply), failure);

    // Load the |thisv|, ensure that it's a scripted function with a valid baseline or ion
    // script, or a native function.
    Address thisSlot(masm.getStackPointer(), ICStackValueOffset + (2 * sizeof(Value)));
    masm.loadValue(thisSlot, val);

    masm.branchTestObject(Assembler::NotEqual, val, failure);
    Register target = masm.extractObject(val, ExtractTemp1);
    regs.add(val);
    regs.takeUnchecked(target);

    masm.branchTestObjClass(Assembler::NotEqual, target, regs.getAny(), &JSFunction::class_,
                            failure);

    if (checkNative) {
        masm.branchIfInterpreted(target, failure);
    } else {
        masm.branchIfFunctionHasNoScript(target, failure);
        Register temp = regs.takeAny();
        masm.loadPtr(Address(target, JSFunction::offsetOfNativeOrScript()), temp);
        masm.loadBaselineOrIonRaw(temp, temp, failure);
        regs.add(temp);
    }
    return target;
}
#if defined(_MSC_VER) && _MSC_VER == 1800
# pragma optimize("", on)
#endif

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
static const VMFunction DoCallFallbackInfo = FunctionInfo<DoCallFallbackFn>(DoCallFallback);

typedef bool (*DoSpreadCallFallbackFn)(JSContext*, BaselineFrame*, ICCall_Fallback*,
                                       Value*, MutableHandleValue);
static const VMFunction DoSpreadCallFallbackInfo =
    FunctionInfo<DoSpreadCallFallbackFn>(DoSpreadCallFallback);

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

        PushFramePtr(masm, R0.scratchReg());

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

    PushFramePtr(masm, R0.scratchReg());

    if (!callVM(DoCallFallbackInfo, masm))
        return false;

    uint32_t framePushed = masm.framePushed();
    leaveStubFrame(masm);
    EmitReturnFromIC(masm);

    // The following asmcode is only used when an Ion inlined frame bails out
    // into into baseline jitcode. The return address pushed onto the
    // reconstructed baseline stack points here.
    returnOffset_ = masm.currentOffset();

    // Here we are again in a stub frame. Marking as so.
    inStubFrame_ = true;
    masm.setFramePushed(framePushed);

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
    // EmitEnterTypeMonitorIC with a custom struct offset.
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

    cx->compartment()->jitCompartment()->initBaselineCallReturnAddr(code->raw() + returnOffset_,
                                                                    isConstructing_);
}

typedef bool (*CreateThisFn)(JSContext* cx, HandleObject callee, HandleObject newTarget,
                             MutableHandleValue rval);
static const VMFunction CreateThisInfoBaseline = FunctionInfo<CreateThisFn>(CreateThis);

bool
ICCallScriptedCompiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
    bool canUseTailCallReg = regs.has(ICTailCallReg);

    Register argcReg = R0.scratchReg();
    MOZ_ASSERT(argcReg != ArgumentsRectifierReg);

    regs.take(argcReg);
    regs.take(ArgumentsRectifierReg);
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
        masm.branchIfFunctionHasNoScript(callee, &failure);
    } else {
        // Ensure the object is a function.
        masm.branchTestObjClass(Assembler::NotEqual, callee, regs.getAny(), &JSFunction::class_,
                                &failure);
        if (isConstructing_) {
            masm.branchIfNotInterpretedConstructor(callee, regs.getAny(), &failure);
        } else {
            masm.branchIfFunctionHasNoScript(callee, &failure);
            masm.branchFunctionKind(Assembler::Equal, JSFunction::ClassConstructor, callee,
                                    regs.getAny(), &failure);
        }
    }

    // Load the JSScript.
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);

    // Load the start of the target JitCode.
    Register code;
    if (!isConstructing_) {
        code = regs.takeAny();
        masm.loadBaselineOrIonRaw(callee, code, &failure);
    } else {
        Address scriptCode(callee, JSScript::offsetOfBaselineOrIonRaw());
        masm.branchPtr(Assembler::Equal, scriptCode, ImmPtr(nullptr), &failure);
    }

    // We no longer need R1.
    regs.add(R1);

    // Push a stub frame so that we can perform a non-tail call.
    enterStubFrame(masm, regs.getAny());
    if (canUseTailCallReg)
        regs.add(ICTailCallReg);

    Label failureLeaveStubFrame;

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
        MOZ_ASSERT(JSReturnOperand == R0);
        regs = availableGeneralRegs(0);
        regs.take(R0);
        regs.take(ArgumentsRectifierReg);
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
        // have destroyed the callee BaselineScript and IonScript. CreateThis is
        // safely repeatable though, so in this case we just leave the stub frame
        // and jump to the next stub.

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
        masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);

        code = regs.takeAny();
        masm.loadBaselineOrIonRaw(callee, code, &failureLeaveStubFrame);

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

    EmitBaselineCreateStubFrameDescriptor(masm, scratch);

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
        MOZ_ASSERT(ArgumentsRectifierReg != code);
        MOZ_ASSERT(ArgumentsRectifierReg != argcReg);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.movePtr(argcReg, ArgumentsRectifierReg);
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

    // Leave stub frame and restore argc for the next stub.
    masm.bind(&failureLeaveStubFrame);
    inStubFrame_ = true;
    leaveStubFrame(masm, false);
    if (argcReg != R0.scratchReg())
        masm.movePtr(argcReg, R0.scratchReg());

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

typedef bool (*CopyArrayFn)(JSContext*, HandleObject, MutableHandleValue);
static const VMFunction CopyArrayInfo = FunctionInfo<CopyArrayFn>(CopyArray);

bool
ICCall_StringSplit::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // Stack Layout: [ ..., CalleeVal, ThisVal, Arg0Val, +ICStackValueOffset+ ]
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));
    Label failureRestoreArgc;
#ifdef DEBUG
    Label oneArg;
    Register argcReg = R0.scratchReg();
    masm.branch32(Assembler::Equal, argcReg, Imm32(1), &oneArg);
    masm.assumeUnreachable("Expected argc == 1");
    masm.bind(&oneArg);
#endif
    Register scratchReg = regs.takeAny();

    // Guard that callee is native function js::str_split.
    {
        Address calleeAddr(masm.getStackPointer(), ICStackValueOffset + (2 * sizeof(Value)));
        ValueOperand calleeVal = regs.takeAnyValue();

        // Ensure that callee is an object.
        masm.loadValue(calleeAddr, calleeVal);
        masm.branchTestObject(Assembler::NotEqual, calleeVal, &failureRestoreArgc);

        // Ensure that callee is a function.
        Register calleeObj = masm.extractObject(calleeVal, ExtractTemp0);
        masm.branchTestObjClass(Assembler::NotEqual, calleeObj, scratchReg,
                                &JSFunction::class_, &failureRestoreArgc);

        // Ensure that callee's function impl is the native str_split.
        masm.loadPtr(Address(calleeObj, JSFunction::offsetOfNativeOrScript()), scratchReg);
        masm.branchPtr(Assembler::NotEqual, scratchReg, ImmPtr(js::str_split), &failureRestoreArgc);

        regs.add(calleeVal);
    }

    // Guard argument.
    {
        // Ensure that arg is a string.
        Address argAddr(masm.getStackPointer(), ICStackValueOffset);
        ValueOperand argVal = regs.takeAnyValue();

        masm.loadValue(argAddr, argVal);
        masm.branchTestString(Assembler::NotEqual, argVal, &failureRestoreArgc);

        Register argString = masm.extractString(argVal, ExtractTemp0);
        masm.branchPtr(Assembler::NotEqual, Address(ICStubReg, offsetOfExpectedArg()),
                       argString, &failureRestoreArgc);
        regs.add(argVal);
    }

    // Guard this-value.
    {
        // Ensure that thisv is a string.
        Address thisvAddr(masm.getStackPointer(), ICStackValueOffset + sizeof(Value));
        ValueOperand thisvVal = regs.takeAnyValue();

        masm.loadValue(thisvAddr, thisvVal);
        masm.branchTestString(Assembler::NotEqual, thisvVal, &failureRestoreArgc);

        Register thisvString = masm.extractString(thisvVal, ExtractTemp0);
        masm.branchPtr(Assembler::NotEqual, Address(ICStubReg, offsetOfExpectedThis()),
                       thisvString, &failureRestoreArgc);
        regs.add(thisvVal);
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
    masm.move32(Imm32(1), R0.scratchReg());
    EmitStubGuardFailure(masm);
    return true;
}

bool
ICCall_IsSuspendedStarGenerator::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    // The IsSuspendedStarGenerator intrinsic is only called in self-hosted
    // code, so it's safe to assume we have a single argument and the callee
    // is our intrinsic.

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

    // Check if it's a StarGeneratorObject.
    Register scratch = regs.takeAny();
    masm.branchTestObjClass(Assembler::NotEqual, genObj, scratch, &StarGeneratorObject::class_,
                            &returnFalse);

    // If the yield index slot holds an int32 value < YIELD_INDEX_CLOSING,
    // the generator is suspended.
    masm.loadValue(Address(genObj, GeneratorObject::offsetOfYieldIndexSlot()), argVal);
    masm.branchTestInt32(Assembler::NotEqual, argVal, &returnFalse);
    masm.unboxInt32(argVal, scratch);
    masm.branch32(Assembler::AboveOrEqual, scratch, Imm32(StarGeneratorObject::YIELD_INDEX_CLOSING),
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
        masm.loadValue(Address(masm.getStackPointer(), ICStackValueOffset + 2 * sizeof(Value)), R1);
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

    if (isConstructing_) {
        // Stack looks like: [ ..., Arg0Val, ThisVal, CalleeVal ]
        // Replace ThisVal with MagicValue(JS_IS_CONSTRUCTING)
        masm.storeValue(MagicValue(JS_IS_CONSTRUCTING), Address(masm.getStackPointer(), sizeof(Value)));
    }


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
    EmitBaselineCreateStubFrameDescriptor(masm, scratch);
    masm.push(scratch);
    masm.push(ICTailCallReg);
    masm.enterFakeExitFrameForNative(isConstructing_);

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
    masm.callWithABI(Address(callee, JSFunction::offsetOfNativeOrScript()));
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
    Register callee = masm.extractObject(R1, ExtractTemp0);
    Register scratch = regs.takeAny();
    masm.loadObjClass(callee, scratch);
    masm.branchPtr(Assembler::NotEqual,
                   Address(ICStubReg, ICCall_ClassHook::offsetOfClass()),
                   scratch, &failure);

    regs.add(R1);
    regs.takeUnchecked(callee);

    // Push a stub frame so that we can perform a non-tail call.
    // Note that this leaves the return address in TailCallReg.
    enterStubFrame(masm, regs.getAny());

    regs.add(scratch);
    pushCallArguments(masm, regs, argcReg, /* isJitCall = */ false, isConstructing_);
    regs.take(scratch);

    if (isConstructing_) {
        // Stack looks like: [ ..., Arg0Val, ThisVal, CalleeVal ]
        // Replace ThisVal with MagicValue(JS_IS_CONSTRUCTING)
        masm.storeValue(MagicValue(JS_IS_CONSTRUCTING), Address(masm.getStackPointer(), sizeof(Value)));
    }

    masm.checkStackAlignment();

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

    EmitBaselineCreateStubFrameDescriptor(masm, scratch);
    masm.push(scratch);
    masm.push(ICTailCallReg);
    masm.enterFakeExitFrameForNative(isConstructing_);

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
    regs.takeUnchecked(ArgumentsRectifierReg);

    //
    // Validate inputs
    //

    Register target = guardFunApply(masm, regs, argcReg, /*checkNative=*/false,
                                    FunApply_Array, &failure);
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
    EmitBaselineCreateStubFrameDescriptor(masm, scratch);

    // Reload argc from length of array.
    masm.extractObject(arrayVal, argcReg);
    masm.loadPtr(Address(argcReg, NativeObject::offsetOfElements()), argcReg);
    masm.load32(Address(argcReg, ObjectElements::offsetOfInitializedLength()), argcReg);

    masm.Push(argcReg);
    masm.Push(target);
    masm.Push(scratch);

    // Load nargs into scratch for underflow check, and then load jitcode pointer into target.
    masm.load16ZeroExtend(Address(target, JSFunction::offsetOfNargs()), scratch);
    masm.loadPtr(Address(target, JSFunction::offsetOfNativeOrScript()), target);
    masm.loadBaselineOrIonRaw(target, target, nullptr);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.branch32(Assembler::AboveOrEqual, argcReg, scratch, &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != target);
        MOZ_ASSERT(ArgumentsRectifierReg != argcReg);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), target);
        masm.loadPtr(Address(target, JitCode::offsetOfCode()), target);
        masm.movePtr(argcReg, ArgumentsRectifierReg);
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
ICCall_ScriptedApplyArguments::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(0));

    Register argcReg = R0.scratchReg();
    regs.take(argcReg);
    regs.takeUnchecked(ICTailCallReg);
    regs.takeUnchecked(ArgumentsRectifierReg);

    //
    // Validate inputs
    //

    Register target = guardFunApply(masm, regs, argcReg, /*checkNative=*/false,
                                    FunApply_MagicArgs, &failure);
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
    EmitBaselineCreateStubFrameDescriptor(masm, scratch);

    masm.loadPtr(Address(BaselineFrameReg, 0), argcReg);
    masm.loadPtr(Address(argcReg, BaselineFrame::offsetOfNumActualArgs()), argcReg);
    masm.Push(argcReg);
    masm.Push(target);
    masm.Push(scratch);

    // Load nargs into scratch for underflow check, and then load jitcode pointer into target.
    masm.load16ZeroExtend(Address(target, JSFunction::offsetOfNargs()), scratch);
    masm.loadPtr(Address(target, JSFunction::offsetOfNativeOrScript()), target);
    masm.loadBaselineOrIonRaw(target, target, nullptr);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.branch32(Assembler::AboveOrEqual, argcReg, scratch, &noUnderflow);
    {
        // Call the arguments rectifier.
        MOZ_ASSERT(ArgumentsRectifierReg != target);
        MOZ_ASSERT(ArgumentsRectifierReg != argcReg);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), target);
        masm.loadPtr(Address(target, JitCode::offsetOfCode()), target);
        masm.movePtr(argcReg, ArgumentsRectifierReg);
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
    MOZ_ASSERT(argcReg != ArgumentsRectifierReg);

    regs.take(argcReg);
    regs.take(ArgumentsRectifierReg);
    regs.takeUnchecked(ICTailCallReg);

    // Load the callee in R1.
    // Stack Layout: [ ..., CalleeVal, ThisVal, Arg0Val, ..., ArgNVal, +ICStackValueOffset+ ]
    BaseValueIndex calleeSlot(masm.getStackPointer(), argcReg, ICStackValueOffset + sizeof(Value));
    masm.loadValue(calleeSlot, R1);
    regs.take(R1);

    // Ensure callee is fun_call.
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);

    Register callee = masm.extractObject(R1, ExtractTemp0);
    masm.branchTestObjClass(Assembler::NotEqual, callee, regs.getAny(), &JSFunction::class_,
                            &failure);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);
    masm.branchPtr(Assembler::NotEqual, callee, ImmPtr(fun_call), &failure);

    // Ensure |this| is a scripted function with JIT code.
    BaseIndex thisSlot(masm.getStackPointer(), argcReg, TimesEight, ICStackValueOffset);
    masm.loadValue(thisSlot, R1);

    masm.branchTestObject(Assembler::NotEqual, R1, &failure);
    callee = masm.extractObject(R1, ExtractTemp0);

    masm.branchTestObjClass(Assembler::NotEqual, callee, regs.getAny(), &JSFunction::class_,
                            &failure);
    masm.branchIfFunctionHasNoScript(callee, &failure);
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);

    // Load the start of the target JitCode.
    Register code = regs.takeAny();
    masm.loadBaselineOrIonRaw(callee, code, &failure);

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
    EmitBaselineCreateStubFrameDescriptor(masm, scratch);

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
        MOZ_ASSERT(ArgumentsRectifierReg != code);
        MOZ_ASSERT(ArgumentsRectifierReg != argcReg);

        JitCode* argumentsRectifier =
            cx->runtime()->jitRuntime()->getArgumentsRectifier();

        masm.movePtr(ImmGCPtr(argumentsRectifier), code);
        masm.loadPtr(Address(code, JitCode::offsetOfCode()), code);
        masm.movePtr(argcReg, ArgumentsRectifierReg);
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
    if (!table)
        return nullptr;

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
// IteratorNew_Fallback
//

static bool
DoIteratorNewFallback(JSContext* cx, BaselineFrame* frame, ICIteratorNew_Fallback* stub,
                      HandleValue value, MutableHandleValue res)
{
    jsbytecode* pc = stub->icEntry()->pc(frame->script());
    FallbackICSpew(cx, stub, "IteratorNew");

    uint8_t flags = GET_UINT8(pc);
    res.set(value);
    return ValueToIterator(cx, flags, res);
}

typedef bool (*DoIteratorNewFallbackFn)(JSContext*, BaselineFrame*, ICIteratorNew_Fallback*,
                                        HandleValue, MutableHandleValue);
static const VMFunction DoIteratorNewFallbackInfo =
    FunctionInfo<DoIteratorNewFallbackFn>(DoIteratorNewFallback, TailCall, PopValues(1));

bool
ICIteratorNew_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    // Sync stack for the decompiler.
    masm.pushValue(R0);

    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoIteratorNewFallbackInfo, masm);
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
    FunctionInfo<DoIteratorMoreFallbackFn>(DoIteratorMoreFallback, TailCall);

bool
ICIteratorMore_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.unboxObject(R0, R0.scratchReg());
    masm.push(R0.scratchReg());
    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

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

    masm.branchTestObjClass(Assembler::NotEqual, obj, scratch,
                            &PropertyIteratorObject::class_, &failure);
    masm.loadObjPrivate(obj, JSObject::ITER_CLASS_NFIXED_SLOTS, nativeIterator);

    masm.branchTest32(Assembler::NonZero, Address(nativeIterator, offsetof(NativeIterator, flags)),
                      Imm32(JSITER_FOREACH), &failure);

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

static bool
DoIteratorCloseFallback(JSContext* cx, ICIteratorClose_Fallback* stub, HandleValue iterValue)
{
    FallbackICSpew(cx, stub, "IteratorClose");

    RootedObject iteratorObject(cx, &iterValue.toObject());
    return CloseIterator(cx, iteratorObject);
}

typedef bool (*DoIteratorCloseFallbackFn)(JSContext*, ICIteratorClose_Fallback*, HandleValue);
static const VMFunction DoIteratorCloseFallbackInfo =
    FunctionInfo<DoIteratorCloseFallbackFn>(DoIteratorCloseFallback, TailCall);

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
                        HandleFunction fun, bool* attached)
{
    MOZ_ASSERT(!*attached);
    if (fun->isBoundFunction())
        return true;

    Shape* shape = fun->lookupPure(cx->names().prototype);
    if (!shape || !shape->hasSlot() || !shape->hasDefaultGetter())
        return true;

    uint32_t slot = shape->slot();
    MOZ_ASSERT(fun->numFixedSlots() == 0, "Stub code relies on this");

    if (!fun->getSlot(slot).isObject())
        return true;

    JSObject* protoObject = &fun->getSlot(slot).toObject();

    JitSpew(JitSpew_BaselineIC, "  Generating InstanceOf(Function) stub");
    ICInstanceOf_Function::Compiler compiler(cx, fun->lastProperty(), protoObject, slot);
    ICStub* newStub = compiler.getStub(compiler.getStubSpace(frame->script()));
    if (!newStub)
        return false;

    stub->addNewStub(newStub);
    *attached = true;
    return true;
}

static bool
DoInstanceOfFallback(JSContext* cx, BaselineFrame* frame, ICInstanceOf_Fallback* stub,
                     HandleValue lhs, HandleValue rhs, MutableHandleValue res)
{
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

    if (!obj->is<JSFunction>()) {
        stub->noteUnoptimizableAccess();
        return true;
    }

    // For functions, keep track of the |prototype| property in type information,
    // for use during Ion compilation.
    EnsureTrackPropertyTypes(cx, obj, NameToId(cx->names().prototype));

    if (stub->numOptimizedStubs() >= ICInstanceOf_Fallback::MAX_OPTIMIZED_STUBS)
        return true;

    RootedFunction fun(cx, &obj->as<JSFunction>());
    bool attached = false;
    if (!TryAttachInstanceOfStub(cx, frame, stub, fun, &attached))
        return false;
    if (!attached)
        stub->noteUnoptimizableAccess();
    return true;
}

typedef bool (*DoInstanceOfFallbackFn)(JSContext*, BaselineFrame*, ICInstanceOf_Fallback*,
                                       HandleValue, HandleValue, MutableHandleValue);
static const VMFunction DoInstanceOfFallbackInfo =
    FunctionInfo<DoInstanceOfFallbackFn>(DoInstanceOfFallback, TailCall, PopValues(2));

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
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoInstanceOfFallbackInfo, masm);
}

bool
ICInstanceOf_Function::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    Label failure;

    // Ensure RHS is an object.
    masm.branchTestObject(Assembler::NotEqual, R1, &failure);
    Register rhsObj = masm.extractObject(R1, ExtractTemp0);

    // Allow using R1's type register as scratch. We have to restore it when
    // we want to jump to the next stub.
    Label failureRestoreR1;
    AllocatableGeneralRegisterSet regs(availableGeneralRegs(1));
    regs.takeUnchecked(rhsObj);

    Register scratch1 = regs.takeAny();
    Register scratch2 = regs.takeAny();

    // Shape guard.
    masm.loadPtr(Address(ICStubReg, ICInstanceOf_Function::offsetOfShape()), scratch1);
    masm.branchTestObjShape(Assembler::NotEqual, rhsObj, scratch1, &failureRestoreR1);

    // Guard on the .prototype object.
    masm.loadPtr(Address(rhsObj, NativeObject::offsetOfSlots()), scratch1);
    masm.load32(Address(ICStubReg, ICInstanceOf_Function::offsetOfSlot()), scratch2);
    BaseValueIndex prototypeSlot(scratch1, scratch2);
    masm.branchTestObject(Assembler::NotEqual, prototypeSlot, &failureRestoreR1);
    masm.unboxObject(prototypeSlot, scratch1);
    masm.branchPtr(Assembler::NotEqual,
                   Address(ICStubReg, ICInstanceOf_Function::offsetOfPrototypeObject()),
                   scratch1, &failureRestoreR1);

    // If LHS is a primitive, return false.
    Label returnFalse, returnTrue;
    masm.branchTestObject(Assembler::NotEqual, R0, &returnFalse);

    // LHS is an object. Load its proto.
    masm.unboxObject(R0, scratch2);
    masm.loadObjProto(scratch2, scratch2);

    {
        // Walk the proto chain until we either reach the target object,
        // nullptr or LazyProto.
        Label loop;
        masm.bind(&loop);

        masm.branchPtr(Assembler::Equal, scratch2, scratch1, &returnTrue);
        masm.branchTestPtr(Assembler::Zero, scratch2, scratch2, &returnFalse);

        MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);
        masm.branchPtr(Assembler::Equal, scratch2, ImmWord(1), &failureRestoreR1);

        masm.loadObjProto(scratch2, scratch2);
        masm.jump(&loop);
    }

    EmitReturnFromIC(masm);

    masm.bind(&returnFalse);
    masm.moveValue(BooleanValue(false), R0);
    EmitReturnFromIC(masm);

    masm.bind(&returnTrue);
    masm.moveValue(BooleanValue(true), R0);
    EmitReturnFromIC(masm);

    masm.bind(&failureRestoreR1);
    masm.tagValue(JSVAL_TYPE_OBJECT, rhsObj, R1);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

//
// TypeOf_Fallback
//

static bool
DoTypeOfFallback(JSContext* cx, BaselineFrame* frame, ICTypeOf_Fallback* stub, HandleValue val,
                 MutableHandleValue res)
{
    FallbackICSpew(cx, stub, "TypeOf");
    JSType type = js::TypeOfValue(val);
    RootedString string(cx, TypeName(type, cx->names()));

    res.setString(string);

    MOZ_ASSERT(type != JSTYPE_NULL);
    if (type != JSTYPE_OBJECT && type != JSTYPE_FUNCTION) {
        // Create a new TypeOf stub.
        JitSpew(JitSpew_BaselineIC, "  Generating TypeOf stub for JSType (%d)", (int) type);
        ICTypeOf_Typed::Compiler compiler(cx, type, string);
        ICStub* typeOfStub = compiler.getStub(compiler.getStubSpace(frame->script()));
        if (!typeOfStub)
            return false;
        stub->addNewStub(typeOfStub);
    }

    return true;
}

typedef bool (*DoTypeOfFallbackFn)(JSContext*, BaselineFrame* frame, ICTypeOf_Fallback*,
                                   HandleValue, MutableHandleValue);
static const VMFunction DoTypeOfFallbackInfo =
    FunctionInfo<DoTypeOfFallbackFn>(DoTypeOfFallback, TailCall);

bool
ICTypeOf_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.pushValue(R0);
    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoTypeOfFallbackInfo, masm);
}

bool
ICTypeOf_Typed::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);
    MOZ_ASSERT(type_ != JSTYPE_NULL);
    MOZ_ASSERT(type_ != JSTYPE_FUNCTION);
    MOZ_ASSERT(type_ != JSTYPE_OBJECT);

    Label failure;
    switch(type_) {
      case JSTYPE_VOID:
        masm.branchTestUndefined(Assembler::NotEqual, R0, &failure);
        break;

      case JSTYPE_STRING:
        masm.branchTestString(Assembler::NotEqual, R0, &failure);
        break;

      case JSTYPE_NUMBER:
        masm.branchTestNumber(Assembler::NotEqual, R0, &failure);
        break;

      case JSTYPE_BOOLEAN:
        masm.branchTestBoolean(Assembler::NotEqual, R0, &failure);
        break;

      case JSTYPE_SYMBOL:
        masm.branchTestSymbol(Assembler::NotEqual, R0, &failure);
        break;

      default:
        MOZ_CRASH("Unexpected type");
    }

    masm.movePtr(ImmGCPtr(typeString_), R0.scratchReg());
    masm.tagValue(JSVAL_TYPE_STRING, R0.scratchReg(), R0);
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
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
static const VMFunction DoRetSubFallbackInfo = FunctionInfo<DoRetSubFallbackFn>(DoRetSubFallback);

typedef bool (*ThrowFn)(JSContext*, HandleValue);
static const VMFunction ThrowInfoBaseline = FunctionInfo<ThrowFn>(js::Throw, TailCall);

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
        pushFramePtr(masm, scratch);

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

ICGetElemNativeStub::ICGetElemNativeStub(ICStub::Kind kind, JitCode* stubCode,
                                         ICStub* firstMonitorStub,
                                         ReceiverGuard guard, AccessType acctype,
                                         bool needsAtomize, bool isSymbol)
  : ICMonitoredStub(kind, stubCode, firstMonitorStub),
    receiverGuard_(guard)
{
    extra_ = (static_cast<uint16_t>(acctype) << ACCESSTYPE_SHIFT) |
             (static_cast<uint16_t>(needsAtomize) << NEEDS_ATOMIZE_SHIFT) |
             (static_cast<uint16_t>(isSymbol) << ISSYMBOL_SHIFT);
}

ICGetElemNativeStub::~ICGetElemNativeStub()
{ }

template <class T>
ICGetElemNativeGetterStub<T>::ICGetElemNativeGetterStub(
                           ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                           ReceiverGuard guard, const T* key, AccType acctype, bool needsAtomize,
                           JSFunction* getter, uint32_t pcOffset)
  : ICGetElemNativeStubImpl<T>(kind, stubCode, firstMonitorStub, guard, key, acctype, needsAtomize),
    getter_(getter),
    pcOffset_(pcOffset)
{
    MOZ_ASSERT(kind == ICStub::GetElem_NativePrototypeCallNativeName ||
               kind == ICStub::GetElem_NativePrototypeCallNativeSymbol ||
               kind == ICStub::GetElem_NativePrototypeCallScriptedName ||
               kind == ICStub::GetElem_NativePrototypeCallScriptedSymbol);
    MOZ_ASSERT(acctype == ICGetElemNativeStub::NativeGetter ||
               acctype == ICGetElemNativeStub::ScriptedGetter);
}

template <class T>
ICGetElem_NativePrototypeSlot<T>::ICGetElem_NativePrototypeSlot(
                               JitCode* stubCode, ICStub* firstMonitorStub, ReceiverGuard guard,
                               const T* key, AccType acctype, bool needsAtomize, uint32_t offset,
                               JSObject* holder, Shape* holderShape)
  : ICGetElemNativeSlotStub<T>(getGetElemStubKind<T>(ICStub::GetElem_NativePrototypeSlotName),
                               stubCode, firstMonitorStub, guard, key, acctype, needsAtomize, offset),
    holder_(holder),
    holderShape_(holderShape)
{ }

template <class T>
ICGetElemNativePrototypeCallStub<T>::ICGetElemNativePrototypeCallStub(
                                  ICStub::Kind kind, JitCode* stubCode, ICStub* firstMonitorStub,
                                  ReceiverGuard guard, const T* key, AccType acctype,
                                  bool needsAtomize, JSFunction* getter, uint32_t pcOffset,
                                  JSObject* holder, Shape* holderShape)
  : ICGetElemNativeGetterStub<T>(kind, stubCode, firstMonitorStub, guard, key, acctype, needsAtomize,
                                 getter, pcOffset),
    holder_(holder),
    holderShape_(holderShape)
{}

template <class T>
/* static */ ICGetElem_NativePrototypeCallNative<T>*
ICGetElem_NativePrototypeCallNative<T>::Clone(JSContext* cx,
                                              ICStubSpace* space,
                                              ICStub* firstMonitorStub,
                                              ICGetElem_NativePrototypeCallNative<T>& other)
{
    return ICStub::New<ICGetElem_NativePrototypeCallNative<T>>(cx, space, other.jitCode(),
                firstMonitorStub, other.receiverGuard(), &other.key().get(), other.accessType(),
                other.needsAtomize(), other.getter(), other.pcOffset_, other.holder(),
                other.holderShape());
}

template ICGetElem_NativePrototypeCallNative<JS::Symbol*>*
ICGetElem_NativePrototypeCallNative<JS::Symbol*>::Clone(JSContext*, ICStubSpace*, ICStub*,
                                          ICGetElem_NativePrototypeCallNative<JS::Symbol*>&);
template ICGetElem_NativePrototypeCallNative<js::PropertyName*>*
ICGetElem_NativePrototypeCallNative<js::PropertyName*>::Clone(JSContext*, ICStubSpace*, ICStub*,
                                          ICGetElem_NativePrototypeCallNative<js::PropertyName*>&);

template <class T>
/* static */ ICGetElem_NativePrototypeCallScripted<T>*
ICGetElem_NativePrototypeCallScripted<T>::Clone(JSContext* cx,
                                                ICStubSpace* space,
                                                ICStub* firstMonitorStub,
                                                ICGetElem_NativePrototypeCallScripted<T>& other)
{
    return ICStub::New<ICGetElem_NativePrototypeCallScripted<T>>(cx, space, other.jitCode(),
                firstMonitorStub, other.receiverGuard(), &other.key().get(), other.accessType(),
                other.needsAtomize(), other.getter(), other.pcOffset_, other.holder(),
                other.holderShape());
}

template ICGetElem_NativePrototypeCallScripted<JS::Symbol*>*
ICGetElem_NativePrototypeCallScripted<JS::Symbol*>::Clone(JSContext*, ICStubSpace*, ICStub*,
                                        ICGetElem_NativePrototypeCallScripted<JS::Symbol*>&);
template ICGetElem_NativePrototypeCallScripted<js::PropertyName*>*
ICGetElem_NativePrototypeCallScripted<js::PropertyName*>::Clone(JSContext*, ICStubSpace*, ICStub*,
                                        ICGetElem_NativePrototypeCallScripted<js::PropertyName*>&);

ICGetElem_Dense::ICGetElem_Dense(JitCode* stubCode, ICStub* firstMonitorStub, Shape* shape)
    : ICMonitoredStub(GetElem_Dense, stubCode, firstMonitorStub),
      shape_(shape)
{ }

/* static */ ICGetElem_Dense*
ICGetElem_Dense::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                       ICGetElem_Dense& other)
{
    return New<ICGetElem_Dense>(cx, space, other.jitCode(), firstMonitorStub, other.shape_);
}

ICGetElem_UnboxedArray::ICGetElem_UnboxedArray(JitCode* stubCode, ICStub* firstMonitorStub,
                                               ObjectGroup *group)
  : ICMonitoredStub(GetElem_UnboxedArray, stubCode, firstMonitorStub),
    group_(group)
{ }

/* static */ ICGetElem_UnboxedArray*
ICGetElem_UnboxedArray::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                              ICGetElem_UnboxedArray& other)
{
    return New<ICGetElem_UnboxedArray>(cx, space, other.jitCode(), firstMonitorStub, other.group_);
}

ICGetElem_TypedArray::ICGetElem_TypedArray(JitCode* stubCode, Shape* shape, Scalar::Type type)
  : ICStub(GetElem_TypedArray, stubCode),
    shape_(shape)
{
    extra_ = uint16_t(type);
    MOZ_ASSERT(extra_ == type);
}

/* static */ ICGetElem_Arguments*
ICGetElem_Arguments::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                           ICGetElem_Arguments& other)
{
    return New<ICGetElem_Arguments>(cx, space, other.jitCode(), firstMonitorStub, other.which());
}

ICSetElem_DenseOrUnboxedArray::ICSetElem_DenseOrUnboxedArray(JitCode* stubCode, Shape* shape, ObjectGroup* group)
  : ICUpdatedStub(SetElem_DenseOrUnboxedArray, stubCode),
    shape_(shape),
    group_(group)
{ }

ICSetElem_DenseOrUnboxedArrayAdd::ICSetElem_DenseOrUnboxedArrayAdd(JitCode* stubCode, ObjectGroup* group,
                                                                   size_t protoChainDepth)
  : ICUpdatedStub(SetElem_DenseOrUnboxedArrayAdd, stubCode),
    group_(group)
{
    MOZ_ASSERT(protoChainDepth <= MAX_PROTO_CHAIN_DEPTH);
    extra_ = protoChainDepth;
}

template <size_t ProtoChainDepth>
ICUpdatedStub*
ICSetElemDenseOrUnboxedArrayAddCompiler::getStubSpecific(ICStubSpace* space,
                                                         Handle<ShapeVector> shapes)
{
    RootedObjectGroup group(cx, obj_->getGroup(cx));
    if (!group)
        return nullptr;
    Rooted<JitCode*> stubCode(cx, getStubCode());
    return newStub<ICSetElem_DenseOrUnboxedArrayAddImpl<ProtoChainDepth>>(space, stubCode, group, shapes);
}

ICSetElem_TypedArray::ICSetElem_TypedArray(JitCode* stubCode, Shape* shape, Scalar::Type type,
                                           bool expectOutOfBounds)
  : ICStub(SetElem_TypedArray, stubCode),
    shape_(shape)
{
    extra_ = uint8_t(type);
    MOZ_ASSERT(extra_ == type);
    extra_ |= (static_cast<uint16_t>(expectOutOfBounds) << 8);
}

ICInNativeStub::ICInNativeStub(ICStub::Kind kind, JitCode* stubCode, HandleShape shape,
                               HandlePropertyName name)
  : ICStub(kind, stubCode),
    shape_(shape),
    name_(name)
{ }

ICIn_NativePrototype::ICIn_NativePrototype(JitCode* stubCode, HandleShape shape,
                                           HandlePropertyName name, HandleObject holder,
                                           HandleShape holderShape)
  : ICInNativeStub(In_NativePrototype, stubCode, shape, name),
    holder_(holder),
    holderShape_(holderShape)
{ }

ICIn_NativeDoesNotExist::ICIn_NativeDoesNotExist(JitCode* stubCode, size_t protoChainDepth,
                                                 HandlePropertyName name)
  : ICStub(In_NativeDoesNotExist, stubCode),
    name_(name)
{
    MOZ_ASSERT(protoChainDepth <= MAX_PROTO_CHAIN_DEPTH);
    extra_ = protoChainDepth;
}

/* static */ size_t
ICIn_NativeDoesNotExist::offsetOfShape(size_t idx)
{
    MOZ_ASSERT(ICIn_NativeDoesNotExistImpl<0>::offsetOfShape(idx) ==
               ICIn_NativeDoesNotExistImpl<
                    ICIn_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH>::offsetOfShape(idx));
    return ICIn_NativeDoesNotExistImpl<0>::offsetOfShape(idx);
}

template <size_t ProtoChainDepth>
ICIn_NativeDoesNotExistImpl<ProtoChainDepth>::ICIn_NativeDoesNotExistImpl(
        JitCode* stubCode, Handle<ShapeVector> shapes, HandlePropertyName name)
  : ICIn_NativeDoesNotExist(stubCode, ProtoChainDepth, name)
{
    MOZ_ASSERT(shapes.length() == NumShapes);
    for (size_t i = 0; i < NumShapes; i++)
        shapes_[i].init(shapes[i]);
}

ICInNativeDoesNotExistCompiler::ICInNativeDoesNotExistCompiler(
        JSContext* cx, HandleObject obj, HandlePropertyName name, size_t protoChainDepth)
  : ICStubCompiler(cx, ICStub::In_NativeDoesNotExist, Engine::Baseline),
    obj_(cx, obj),
    name_(cx, name),
    protoChainDepth_(protoChainDepth)
{
    MOZ_ASSERT(protoChainDepth_ <= ICIn_NativeDoesNotExist::MAX_PROTO_CHAIN_DEPTH);
}

ICIn_Dense::ICIn_Dense(JitCode* stubCode, HandleShape shape)
  : ICStub(In_Dense, stubCode),
    shape_(shape)
{ }

ICGetName_GlobalLexical::ICGetName_GlobalLexical(JitCode* stubCode, ICStub* firstMonitorStub,
                                                 uint32_t slot)
  : ICMonitoredStub(GetName_GlobalLexical, stubCode, firstMonitorStub),
    slot_(slot)
{ }

template <size_t NumHops>
ICGetName_Scope<NumHops>::ICGetName_Scope(JitCode* stubCode, ICStub* firstMonitorStub,
                                          Handle<ShapeVector> shapes, uint32_t offset)
  : ICMonitoredStub(GetStubKind(), stubCode, firstMonitorStub),
    offset_(offset)
{
    JS_STATIC_ASSERT(NumHops <= MAX_HOPS);
    MOZ_ASSERT(shapes.length() == NumHops + 1);
    for (size_t i = 0; i < NumHops + 1; i++)
        shapes_[i].init(shapes[i]);
}

ICGetIntrinsic_Constant::ICGetIntrinsic_Constant(JitCode* stubCode, const Value& value)
  : ICStub(GetIntrinsic_Constant, stubCode),
    value_(value)
{ }

ICGetIntrinsic_Constant::~ICGetIntrinsic_Constant()
{ }

ICGetName_Global::ICGetName_Global(JitCode* stubCode, ICStub* firstMonitorStub,
                                   ReceiverGuard guard, uint32_t offset,
                                   JSObject* holder, Shape* holderShape, Shape* globalShape)
  : ICGetPropNativePrototypeStub(GetName_Global, stubCode, firstMonitorStub, guard, offset,
                                 holder, holderShape),
    globalShape_(globalShape)
{ }

/* static */ ICGetName_Global*
ICGetName_Global::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                        ICGetName_Global& other)
{
    return New<ICGetName_Global>(cx, space, other.jitCode(), firstMonitorStub,
                                 other.receiverGuard(), other.offset(),
                                 other.holder(), other.holderShape(), other.globalShape());
}

ICInstanceOf_Function::ICInstanceOf_Function(JitCode* stubCode, Shape* shape,
                                             JSObject* prototypeObj, uint32_t slot)
  : ICStub(InstanceOf_Function, stubCode),
    shape_(shape),
    prototypeObj_(prototypeObj),
    slot_(slot)
{ }

ICSetProp_Native::ICSetProp_Native(JitCode* stubCode, ObjectGroup* group, Shape* shape,
                                   uint32_t offset)
  : ICUpdatedStub(SetProp_Native, stubCode),
    group_(group),
    shape_(shape),
    offset_(offset)
{ }

ICSetProp_Native*
ICSetProp_Native::Compiler::getStub(ICStubSpace* space)
{
    RootedObjectGroup group(cx, obj_->getGroup(cx));
    if (!group)
        return nullptr;

    RootedShape shape(cx, LastPropertyForSetProp(obj_));
    ICSetProp_Native* stub = newStub<ICSetProp_Native>(space, getStubCode(), group, shape, offset_);
    if (!stub || !stub->initUpdatingChain(cx, space))
        return nullptr;
    return stub;
}

ICSetProp_NativeAdd::ICSetProp_NativeAdd(JitCode* stubCode, ObjectGroup* group,
                                         size_t protoChainDepth,
                                         Shape* newShape,
                                         ObjectGroup* newGroup,
                                         uint32_t offset)
  : ICUpdatedStub(SetProp_NativeAdd, stubCode),
    group_(group),
    newShape_(newShape),
    newGroup_(newGroup),
    offset_(offset)
{
    MOZ_ASSERT(protoChainDepth <= MAX_PROTO_CHAIN_DEPTH);
    extra_ = protoChainDepth;
}

template <size_t ProtoChainDepth>
ICSetProp_NativeAddImpl<ProtoChainDepth>::ICSetProp_NativeAddImpl(JitCode* stubCode,
                                                                  ObjectGroup* group,
                                                                  Handle<ShapeVector> shapes,
                                                                  Shape* newShape,
                                                                  ObjectGroup* newGroup,
                                                                  uint32_t offset)
  : ICSetProp_NativeAdd(stubCode, group, ProtoChainDepth, newShape, newGroup, offset)
{
    MOZ_ASSERT(shapes.length() == NumShapes);
    for (size_t i = 0; i < NumShapes; i++)
        shapes_[i].init(shapes[i]);
}

ICSetPropNativeAddCompiler::ICSetPropNativeAddCompiler(JSContext* cx, HandleObject obj,
                                                       HandleShape oldShape,
                                                       HandleObjectGroup oldGroup,
                                                       size_t protoChainDepth,
                                                       bool isFixedSlot,
                                                       uint32_t offset)
  : ICStubCompiler(cx, ICStub::SetProp_NativeAdd, Engine::Baseline),
    obj_(cx, obj),
    oldShape_(cx, oldShape),
    oldGroup_(cx, oldGroup),
    protoChainDepth_(protoChainDepth),
    isFixedSlot_(isFixedSlot),
    offset_(offset)
{
    MOZ_ASSERT(protoChainDepth_ <= ICSetProp_NativeAdd::MAX_PROTO_CHAIN_DEPTH);
}

ICSetPropCallSetter::ICSetPropCallSetter(Kind kind, JitCode* stubCode, ReceiverGuard receiverGuard,
                                         JSObject* holder, Shape* holderShape,
                                         JSFunction* setter, uint32_t pcOffset)
  : ICStub(kind, stubCode),
    receiverGuard_(receiverGuard),
    holder_(holder),
    holderShape_(holderShape),
    setter_(setter),
    pcOffset_(pcOffset)
{
    MOZ_ASSERT(kind == ICStub::SetProp_CallScripted || kind == ICStub::SetProp_CallNative);
}

/* static */ ICSetProp_CallScripted*
ICSetProp_CallScripted::Clone(JSContext* cx, ICStubSpace* space, ICStub*,
                              ICSetProp_CallScripted& other)
{
    return New<ICSetProp_CallScripted>(cx, space, other.jitCode(), other.receiverGuard(),
                                       other.holder_, other.holderShape_, other.setter_,
                                       other.pcOffset_);
}

/* static */ ICSetProp_CallNative*
ICSetProp_CallNative::Clone(JSContext* cx, ICStubSpace* space, ICStub*, ICSetProp_CallNative& other)
{
    return New<ICSetProp_CallNative>(cx, space, other.jitCode(), other.receiverGuard(),
                                     other.holder_, other.holderShape_, other.setter_,
                                     other.pcOffset_);
}

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

static bool DoRestFallback(JSContext* cx, BaselineFrame* frame, ICRest_Fallback* stub,
                           MutableHandleValue res)
{
    unsigned numFormals = frame->numFormalArgs() - 1;
    unsigned numActuals = frame->numActualArgs();
    unsigned numRest = numActuals > numFormals ? numActuals - numFormals : 0;
    Value* rest = frame->argv() + numFormals;

    JSObject* obj = ObjectGroup::newArrayObject(cx, rest, numRest, GenericObject,
                                                ObjectGroup::NewArrayKind::UnknownIndex);
    if (!obj)
        return false;
    res.setObject(*obj);
    return true;
}

typedef bool (*DoRestFallbackFn)(JSContext*, BaselineFrame*, ICRest_Fallback*,
                                 MutableHandleValue);
static const VMFunction DoRestFallbackInfo =
    FunctionInfo<DoRestFallbackFn>(DoRestFallback, TailCall);

bool
ICRest_Fallback::Compiler::generateStubCode(MacroAssembler& masm)
{
    MOZ_ASSERT(engine_ == Engine::Baseline);

    EmitRestoreTailCallReg(masm);

    masm.push(ICStubReg);
    pushFramePtr(masm, R0.scratchReg());

    return tailCallVM(DoRestFallbackInfo, masm);
}

} // namespace jit
} // namespace js
