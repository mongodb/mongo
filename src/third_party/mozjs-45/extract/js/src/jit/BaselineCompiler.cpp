/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCompiler.h"

#include "mozilla/Casting.h"
#include "mozilla/UniquePtr.h"

#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/FixedList.h"
#include "jit/IonAnalysis.h"
#include "jit/JitcodeMap.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "jit/SharedICHelpers.h"
#include "jit/VMFunctions.h"
#include "vm/ScopeObject.h"
#include "vm/TraceLogging.h"

#include "jsscriptinlines.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::AssertedCast;

BaselineCompiler::BaselineCompiler(JSContext* cx, TempAllocator& alloc, JSScript* script)
  : BaselineCompilerSpecific(cx, alloc, script),
    yieldOffsets_(cx),
    modifiesArguments_(false)
{
}

bool
BaselineCompiler::init()
{
    if (!analysis_.init(alloc_, cx->runtime()->gsnCache))
        return false;

    if (!labels_.init(alloc_, script->length()))
        return false;

    for (size_t i = 0; i < script->length(); i++)
        new (&labels_[i]) Label();

    if (!frame.init(alloc_))
        return false;

    return true;
}

bool
BaselineCompiler::addPCMappingEntry(bool addIndexEntry)
{
    // Don't add multiple entries for a single pc.
    size_t nentries = pcMappingEntries_.length();
    if (nentries > 0 && pcMappingEntries_[nentries - 1].pcOffset == script->pcToOffset(pc))
        return true;

    PCMappingEntry entry;
    entry.pcOffset = script->pcToOffset(pc);
    entry.nativeOffset = masm.currentOffset();
    entry.slotInfo = getStackTopSlotInfo();
    entry.addIndexEntry = addIndexEntry;

    return pcMappingEntries_.append(entry);
}

MethodStatus
BaselineCompiler::compile()
{
    JitSpew(JitSpew_BaselineScripts, "Baseline compiling script %s:%d (%p)",
            script->filename(), script->lineno(), script);

    JitSpew(JitSpew_Codegen, "# Emitting baseline code for script %s:%d",
            script->filename(), script->lineno());

    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    TraceLoggerEvent scriptEvent(logger, TraceLogger_AnnotateScripts, script);
    AutoTraceLog logScript(logger, scriptEvent);
    AutoTraceLog logCompile(logger, TraceLogger_BaselineCompilation);

    if (!script->ensureHasTypes(cx) || !script->ensureHasAnalyzedArgsUsage(cx))
        return Method_Error;

    // When a Debugger set the collectCoverageInfo flag, we recompile baseline
    // scripts without entering the interpreter again. We have to create the
    // ScriptCounts if they do not exist.
    if (!script->hasScriptCounts() && cx->compartment()->collectCoverage()) {
        if (!script->initScriptCounts(cx))
            return Method_Error;
    }

    // Pin analysis info during compilation.
    AutoEnterAnalysis autoEnterAnalysis(cx);

    MOZ_ASSERT(!script->hasBaselineScript());

    if (!emitPrologue())
        return Method_Error;

    MethodStatus status = emitBody();
    if (status != Method_Compiled)
        return status;

    if (!emitEpilogue())
        return Method_Error;

    if (!emitOutOfLinePostBarrierSlot())
        return Method_Error;

    Linker linker(masm);
    if (masm.oom()) {
        ReportOutOfMemory(cx);
        return Method_Error;
    }

    AutoFlushICache afc("Baseline");
    JitCode* code = linker.newCode<CanGC>(cx, BASELINE_CODE);
    if (!code)
        return Method_Error;

    JSObject* templateScope = nullptr;
    if (script->functionNonDelazifying()) {
        RootedFunction fun(cx, script->functionNonDelazifying());
        if (fun->needsCallObject()) {
            RootedScript scriptRoot(cx, script);
            templateScope = CallObject::createTemplateObject(cx, scriptRoot, gc::TenuredHeap);
            if (!templateScope)
                return Method_Error;

            if (fun->isNamedLambda()) {
                RootedObject declEnvObject(cx, DeclEnvObject::createTemplateObject(cx, fun, TenuredObject));
                if (!declEnvObject)
                    return Method_Error;
                templateScope->as<ScopeObject>().setEnclosingScope(declEnvObject);
            }
        }
    }

    // Encode the pc mapping table. See PCMappingIndexEntry for
    // more information.
    Vector<PCMappingIndexEntry> pcMappingIndexEntries(cx);
    CompactBufferWriter pcEntries;
    uint32_t previousOffset = 0;

    for (size_t i = 0; i < pcMappingEntries_.length(); i++) {
        PCMappingEntry& entry = pcMappingEntries_[i];

        if (entry.addIndexEntry) {
            PCMappingIndexEntry indexEntry;
            indexEntry.pcOffset = entry.pcOffset;
            indexEntry.nativeOffset = entry.nativeOffset;
            indexEntry.bufferOffset = pcEntries.length();
            if (!pcMappingIndexEntries.append(indexEntry)) {
                ReportOutOfMemory(cx);
                return Method_Error;
            }
            previousOffset = entry.nativeOffset;
        }

        // Use the high bit of the SlotInfo byte to indicate the
        // native code offset (relative to the previous op) > 0 and
        // comes next in the buffer.
        MOZ_ASSERT((entry.slotInfo.toByte() & 0x80) == 0);

        if (entry.nativeOffset == previousOffset) {
            pcEntries.writeByte(entry.slotInfo.toByte());
        } else {
            MOZ_ASSERT(entry.nativeOffset > previousOffset);
            pcEntries.writeByte(0x80 | entry.slotInfo.toByte());
            pcEntries.writeUnsigned(entry.nativeOffset - previousOffset);
        }

        previousOffset = entry.nativeOffset;
    }

    if (pcEntries.oom()) {
        ReportOutOfMemory(cx);
        return Method_Error;
    }

    // Note: There is an extra entry in the bytecode type map for the search hint, see below.
    size_t bytecodeTypeMapEntries = script->nTypeSets() + 1;

    mozilla::UniquePtr<BaselineScript, JS::DeletePolicy<BaselineScript> > baselineScript(
        BaselineScript::New(script, prologueOffset_.offset(),
                            epilogueOffset_.offset(),
                            profilerEnterFrameToggleOffset_.offset(),
                            profilerExitFrameToggleOffset_.offset(),
                            traceLoggerEnterToggleOffset_.offset(),
                            traceLoggerExitToggleOffset_.offset(),
                            postDebugPrologueOffset_.offset(),
                            icEntries_.length(),
                            pcMappingIndexEntries.length(),
                            pcEntries.length(),
                            bytecodeTypeMapEntries,
                            yieldOffsets_.length()));
    if (!baselineScript) {
        ReportOutOfMemory(cx);
        return Method_Error;
    }

    baselineScript->setMethod(code);
    baselineScript->setTemplateScope(templateScope);

    JitSpew(JitSpew_BaselineScripts, "Created BaselineScript %p (raw %p) for %s:%d",
            (void*) baselineScript.get(), (void*) code->raw(),
            script->filename(), script->lineno());

#ifdef JS_ION_PERF
    writePerfSpewerBaselineProfile(script, code);
#endif

    MOZ_ASSERT(pcMappingIndexEntries.length() > 0);
    baselineScript->copyPCMappingIndexEntries(&pcMappingIndexEntries[0]);

    MOZ_ASSERT(pcEntries.length() > 0);
    baselineScript->copyPCMappingEntries(pcEntries);

    // Copy IC entries
    if (icEntries_.length())
        baselineScript->copyICEntries(script, &icEntries_[0], masm);

    // Adopt fallback stubs from the compiler into the baseline script.
    baselineScript->adoptFallbackStubs(&stubSpace_);

    // All barriers are emitted off-by-default, toggle them on if needed.
    if (cx->zone()->needsIncrementalBarrier())
        baselineScript->toggleBarriers(true);

    // If profiler instrumentation is enabled, toggle instrumentation on.
    if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(cx->runtime()))
        baselineScript->toggleProfilerInstrumentation(true);

    AutoWritableJitCode awjc(code);

    // Patch IC loads using IC entries.
    for (size_t i = 0; i < icLoadLabels_.length(); i++) {
        CodeOffset label = icLoadLabels_[i].label;
        size_t icEntry = icLoadLabels_[i].icEntry;
        ICEntry* entryAddr = &(baselineScript->icEntry(icEntry));
        Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, label),
                                           ImmPtr(entryAddr),
                                           ImmPtr((void*)-1));
    }

    if (modifiesArguments_)
        baselineScript->setModifiesArguments();

#ifdef JS_TRACE_LOGGING
    // Initialize the tracelogger instrumentation.
    baselineScript->initTraceLogger(cx->runtime(), script);
#endif

    uint32_t* bytecodeMap = baselineScript->bytecodeTypeMap();
    FillBytecodeTypeMap(script, bytecodeMap);

    // The last entry in the last index found, and is used to avoid binary
    // searches for the sought entry when queries are in linear order.
    bytecodeMap[script->nTypeSets()] = 0;

    baselineScript->copyYieldEntries(script, yieldOffsets_);

    if (compileDebugInstrumentation_)
        baselineScript->setHasDebugInstrumentation();

    // Always register a native => bytecode mapping entry, since profiler can be
    // turned on with baseline jitcode on stack, and baseline jitcode cannot be invalidated.
    {
        JitSpew(JitSpew_Profiling, "Added JitcodeGlobalEntry for baseline script %s:%d (%p)",
                    script->filename(), script->lineno(), baselineScript.get());

        // Generate profiling string.
        char* str = JitcodeGlobalEntry::createScriptString(cx, script);
        if (!str)
            return Method_Error;

        JitcodeGlobalEntry::BaselineEntry entry;
        entry.init(code, code->raw(), code->rawEnd(), script, str);

        JitcodeGlobalTable* globalTable = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
        if (!globalTable->addEntry(entry, cx->runtime())) {
            entry.destroy();
            ReportOutOfMemory(cx);
            return Method_Error;
        }

        // Mark the jitcode as having a bytecode map.
        code->setHasBytecodeMap();
    }

    script->setBaselineScript(cx, baselineScript.release());

    return Method_Compiled;
}

void
BaselineCompiler::emitInitializeLocals(size_t n, const Value& v)
{
    MOZ_ASSERT(frame.nlocals() > 0 && n <= frame.nlocals());

    // Use R0 to minimize code size. If the number of locals to push is <
    // LOOP_UNROLL_FACTOR, then the initialization pushes are emitted directly
    // and inline.  Otherwise, they're emitted in a partially unrolled loop.
    static const size_t LOOP_UNROLL_FACTOR = 4;
    size_t toPushExtra = n % LOOP_UNROLL_FACTOR;

    masm.moveValue(v, R0);

    // Handle any extra pushes left over by the optional unrolled loop below.
    for (size_t i = 0; i < toPushExtra; i++)
        masm.pushValue(R0);

    // Partially unrolled loop of pushes.
    if (n >= LOOP_UNROLL_FACTOR) {
        size_t toPush = n - toPushExtra;
        MOZ_ASSERT(toPush % LOOP_UNROLL_FACTOR == 0);
        MOZ_ASSERT(toPush >= LOOP_UNROLL_FACTOR);
        masm.move32(Imm32(toPush), R1.scratchReg());
        // Emit unrolled loop with 4 pushes per iteration.
        Label pushLoop;
        masm.bind(&pushLoop);
        for (size_t i = 0; i < LOOP_UNROLL_FACTOR; i++)
            masm.pushValue(R0);
        masm.branchSub32(Assembler::NonZero,
                         Imm32(LOOP_UNROLL_FACTOR), R1.scratchReg(), &pushLoop);
    }
}

bool
BaselineCompiler::emitPrologue()
{
#ifdef JS_USE_LINK_REGISTER
    // Push link register from generateEnterJIT()'s BLR.
    masm.pushReturnAddress();
    masm.checkStackAlignment();
#endif
    emitProfilerEnterFrame();

    masm.push(BaselineFrameReg);
    masm.moveStackPtrTo(BaselineFrameReg);
    masm.subFromStackPtr(Imm32(BaselineFrame::Size()));

    // Initialize BaselineFrame. For eval scripts, the scope chain
    // is passed in R1, so we have to be careful not to clobber it.

    // Initialize BaselineFrame::flags.
    uint32_t flags = 0;
    if (script->isForEval())
        flags |= BaselineFrame::EVAL;
    masm.store32(Imm32(flags), frame.addressOfFlags());

    if (script->isForEval())
        masm.storePtr(ImmGCPtr(script), frame.addressOfEvalScript());

    // Handle scope chain pre-initialization (in case GC gets run
    // during stack check).  For global and eval scripts, the scope
    // chain is in R1.  For function scripts, the scope chain is in
    // the callee, nullptr is stored for now so that GC doesn't choke
    // on a bogus ScopeChain value in the frame.
    if (function())
        masm.storePtr(ImmPtr(nullptr), frame.addressOfScopeChain());
    else
        masm.storePtr(R1.scratchReg(), frame.addressOfScopeChain());

    // Functions with a large number of locals require two stack checks.
    // The VMCall for a fallible stack check can only occur after the
    // scope chain has been initialized, as that is required for proper
    // exception handling if the VMCall returns false.  The scope chain
    // initialization can only happen after the UndefinedValues for the
    // local slots have been pushed.
    // However by that time, the stack might have grown too much.
    // In these cases, we emit an extra, early, infallible check
    // before pushing the locals.  The early check sets a flag on the
    // frame if the stack check fails (but otherwise doesn't throw an
    // exception).  If the flag is set, then the jitcode skips past
    // the pushing of the locals, and directly to scope chain initialization
    // followed by the actual stack check, which will throw the correct
    // exception.
    Label earlyStackCheckFailed;
    if (needsEarlyStackCheck()) {
        if (!emitStackCheck(/* earlyCheck = */ true))
            return false;
        masm.branchTest32(Assembler::NonZero,
                          frame.addressOfFlags(),
                          Imm32(BaselineFrame::OVER_RECURSED),
                          &earlyStackCheckFailed);
    }

    // Initialize local vars to |undefined| and lexicals to a magic value that
    // throws on touch.
    if (frame.nvars() > 0)
        emitInitializeLocals(frame.nvars(), UndefinedValue());
    if (frame.nlexicals() > 0)
        emitInitializeLocals(frame.nlexicals(), MagicValue(JS_UNINITIALIZED_LEXICAL));

    if (needsEarlyStackCheck())
        masm.bind(&earlyStackCheckFailed);

#ifdef JS_TRACE_LOGGING
    if (!emitTraceLoggerEnter())
        return false;
#endif

    // Record the offset of the prologue, because Ion can bailout before
    // the scope chain is initialized.
    prologueOffset_ = CodeOffset(masm.currentOffset());

    // When compiling with Debugger instrumentation, set the debuggeeness of
    // the frame before any operation that can call into the VM.
    emitIsDebuggeeCheck();

    // Initialize the scope chain before any operation that may
    // call into the VM and trigger a GC.
    if (!initScopeChain())
        return false;

    if (!emitStackCheck())
        return false;

    if (!emitDebugPrologue())
        return false;

    if (!emitWarmUpCounterIncrement())
        return false;

    if (!emitArgumentTypeChecks())
        return false;

    return true;
}

bool
BaselineCompiler::emitEpilogue()
{
    // Record the offset of the epilogue, so we can do early return from
    // Debugger handlers during on-stack recompile.
    epilogueOffset_ = CodeOffset(masm.currentOffset());

    masm.bind(&return_);

#ifdef JS_TRACE_LOGGING
    if (!emitTraceLoggerExit())
        return false;
#endif

    masm.moveToStackPtr(BaselineFrameReg);
    masm.pop(BaselineFrameReg);

    emitProfilerExitFrame();

    masm.ret();
    return true;
}

// On input:
//  R2.scratchReg() contains object being written to.
//  Called with the baseline stack synced, except for R0 which is preserved.
//  All other registers are usable as scratch.
// This calls:
//    void PostWriteBarrier(JSRuntime* rt, JSObject* obj);
bool
BaselineCompiler::emitOutOfLinePostBarrierSlot()
{
    masm.bind(&postBarrierSlot_);

    Register objReg = R2.scratchReg();
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(R0);
    regs.take(objReg);
    regs.take(BaselineFrameReg);
    Register scratch = regs.takeAny();
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
    // On ARM, save the link register before calling.  It contains the return
    // address.  The |masm.ret()| later will pop this into |pc| to return.
    masm.push(lr);
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    masm.push(ra);
#endif
    masm.pushValue(R0);

    masm.setupUnalignedABICall(scratch);
    masm.movePtr(ImmPtr(cx->runtime()), scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(objReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, PostWriteBarrier));

    masm.popValue(R0);
    masm.ret();
    return true;
}

bool
BaselineCompiler::emitIC(ICStub* stub, ICEntry::Kind kind)
{
    ICEntry* entry = allocateICEntry(stub, kind);
    if (!entry)
        return false;

    CodeOffset patchOffset;
    EmitCallIC(&patchOffset, masm);
    entry->setReturnOffset(CodeOffset(masm.currentOffset()));
    if (!addICLoadLabel(patchOffset))
        return false;

    return true;
}

typedef bool (*CheckOverRecursedWithExtraFn)(JSContext*, BaselineFrame*, uint32_t, uint32_t);
static const VMFunction CheckOverRecursedWithExtraInfo =
    FunctionInfo<CheckOverRecursedWithExtraFn>(CheckOverRecursedWithExtra);

bool
BaselineCompiler::emitStackCheck(bool earlyCheck)
{
    Label skipCall;
    void* limitAddr = cx->runtime()->addressOfJitStackLimit();
    uint32_t slotsSize = script->nslots() * sizeof(Value);
    uint32_t tolerance = earlyCheck ? slotsSize : 0;

    masm.moveStackPtrTo(R1.scratchReg());

    // If this is the early stack check, locals haven't been pushed yet.  Adjust the
    // stack pointer to account for the locals that would be pushed before performing
    // the guard around the vmcall to the stack check.
    if (earlyCheck)
        masm.subPtr(Imm32(tolerance), R1.scratchReg());

    // If this is the late stack check for a frame which contains an early stack check,
    // then the early stack check might have failed and skipped past the pushing of locals
    // on the stack.
    //
    // If this is a possibility, then the OVER_RECURSED flag should be checked, and the
    // VMCall to CheckOverRecursed done unconditionally if it's set.
    Label forceCall;
    if (!earlyCheck && needsEarlyStackCheck()) {
        masm.branchTest32(Assembler::NonZero,
                          frame.addressOfFlags(),
                          Imm32(BaselineFrame::OVER_RECURSED),
                          &forceCall);
    }

    masm.branchPtr(Assembler::BelowOrEqual, AbsoluteAddress(limitAddr), R1.scratchReg(),
                   &skipCall);

    if (!earlyCheck && needsEarlyStackCheck())
        masm.bind(&forceCall);

    prepareVMCall();
    pushArg(Imm32(earlyCheck));
    pushArg(Imm32(tolerance));
    masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());
    pushArg(R1.scratchReg());

    CallVMPhase phase = POST_INITIALIZE;
    if (earlyCheck)
        phase = PRE_INITIALIZE;
    else if (needsEarlyStackCheck())
        phase = CHECK_OVER_RECURSED;

    if (!callVMNonOp(CheckOverRecursedWithExtraInfo, phase))
        return false;

    icEntries_.back().setFakeKind(earlyCheck
                                  ? ICEntry::Kind_EarlyStackCheck
                                  : ICEntry::Kind_StackCheck);

    masm.bind(&skipCall);
    return true;
}

void
BaselineCompiler::emitIsDebuggeeCheck()
{
    if (compileDebugInstrumentation_) {
        masm.Push(BaselineFrameReg);
        masm.setupUnalignedABICall(R0.scratchReg());
        masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
        masm.passABIArg(R0.scratchReg());
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, jit::FrameIsDebuggeeCheck));
        masm.Pop(BaselineFrameReg);
    }
}

typedef bool (*DebugPrologueFn)(JSContext*, BaselineFrame*, jsbytecode*, bool*);
static const VMFunction DebugPrologueInfo = FunctionInfo<DebugPrologueFn>(jit::DebugPrologue);

bool
BaselineCompiler::emitDebugPrologue()
{
    if (compileDebugInstrumentation_) {
        // Load pointer to BaselineFrame in R0.
        masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

        prepareVMCall();
        pushArg(ImmPtr(pc));
        pushArg(R0.scratchReg());
        if (!callVM(DebugPrologueInfo))
            return false;

        // Fix up the fake ICEntry appended by callVM for on-stack recompilation.
        icEntries_.back().setFakeKind(ICEntry::Kind_DebugPrologue);

        // If the stub returns |true|, we have to return the value stored in the
        // frame's return value slot.
        Label done;
        masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, &done);
        {
            masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
            masm.jump(&return_);
        }
        masm.bind(&done);
    }

    postDebugPrologueOffset_ = CodeOffset(masm.currentOffset());

    return true;
}

typedef bool (*InitGlobalOrEvalScopeObjectsFn)(JSContext*, BaselineFrame*);
static const VMFunction InitGlobalOrEvalScopeObjectsInfo =
    FunctionInfo<InitGlobalOrEvalScopeObjectsFn>(jit::InitGlobalOrEvalScopeObjects);

typedef bool (*InitFunctionScopeObjectsFn)(JSContext*, BaselineFrame*);
static const VMFunction InitFunctionScopeObjectsInfo =
    FunctionInfo<InitFunctionScopeObjectsFn>(jit::InitFunctionScopeObjects);

bool
BaselineCompiler::initScopeChain()
{
    CallVMPhase phase = POST_INITIALIZE;
    if (needsEarlyStackCheck())
        phase = CHECK_OVER_RECURSED;

    RootedFunction fun(cx, function());
    if (fun) {
        // Use callee->environment as scope chain. Note that we do
        // this also for needsCallObject functions, so that the scope
        // chain slot is properly initialized if the call triggers GC.
        Register callee = R0.scratchReg();
        Register scope = R1.scratchReg();
        masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(), callee);
        masm.loadPtr(Address(callee, JSFunction::offsetOfEnvironment()), scope);
        masm.storePtr(scope, frame.addressOfScopeChain());

        if (fun->needsCallObject()) {
            // Call into the VM to create a new call object.
            prepareVMCall();

            masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
            pushArg(R0.scratchReg());

            if (!callVMNonOp(InitFunctionScopeObjectsInfo, phase))
                return false;
        }
    } else if (module()) {
        // Modules use a pre-created scope object.
        Register scope = R1.scratchReg();
        masm.movePtr(ImmGCPtr(&module()->initialEnvironment()), scope);
        masm.storePtr(scope, frame.addressOfScopeChain());
    } else {
        // ScopeChain pointer in BaselineFrame has already been initialized
        // in prologue, but we need to do two more things:
        //
        // 1. Check for redeclaration errors
        // 2. Possibly create a new call object for strict eval.

        prepareVMCall();
        masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
        pushArg(R0.scratchReg());

        if (!callVMNonOp(InitGlobalOrEvalScopeObjectsInfo, phase))
            return false;
    }

    return true;
}

typedef bool (*InterruptCheckFn)(JSContext*);
static const VMFunction InterruptCheckInfo = FunctionInfo<InterruptCheckFn>(InterruptCheck);

bool
BaselineCompiler::emitInterruptCheck()
{
    frame.syncStack(0);

    Label done;
    void* interrupt = cx->runtimeAddressOfInterruptUint32();
    masm.branch32(Assembler::Equal, AbsoluteAddress(interrupt), Imm32(0), &done);

    prepareVMCall();
    if (!callVM(InterruptCheckInfo))
        return false;

    masm.bind(&done);
    return true;
}

bool
BaselineCompiler::emitWarmUpCounterIncrement(bool allowOsr)
{
    // Emit no warm-up counter increments or bailouts if Ion is not
    // enabled, or if the script will never be Ion-compileable

    if (!ionCompileable_ && !ionOSRCompileable_)
        return true;

    Register scriptReg = R2.scratchReg();
    Register countReg = R0.scratchReg();
    Address warmUpCounterAddr(scriptReg, JSScript::offsetOfWarmUpCounter());

    masm.movePtr(ImmGCPtr(script), scriptReg);
    masm.load32(warmUpCounterAddr, countReg);
    masm.add32(Imm32(1), countReg);
    masm.store32(countReg, warmUpCounterAddr);

    // If this is a loop inside a catch or finally block, increment the warmup
    // counter but don't attempt OSR (Ion only compiles the try block).
    if (analysis_.info(pc).loopEntryInCatchOrFinally) {
        MOZ_ASSERT(JSOp(*pc) == JSOP_LOOPENTRY);
        return true;
    }

    // OSR not possible at this loop entry.
    if (!allowOsr) {
        MOZ_ASSERT(JSOp(*pc) == JSOP_LOOPENTRY);
        return true;
    }

    Label skipCall;

    const OptimizationInfo* info = IonOptimizations.get(IonOptimizations.firstLevel());
    uint32_t warmUpThreshold = info->compilerWarmUpThreshold(script, pc);
    masm.branch32(Assembler::LessThan, countReg, Imm32(warmUpThreshold), &skipCall);

    masm.branchPtr(Assembler::Equal,
                   Address(scriptReg, JSScript::offsetOfIonScript()),
                   ImmPtr(ION_COMPILING_SCRIPT), &skipCall);

    // Call IC.
    ICWarmUpCounter_Fallback::Compiler stubCompiler(cx);
    if (!emitNonOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    masm.bind(&skipCall);

    return true;
}

bool
BaselineCompiler::emitArgumentTypeChecks()
{
    if (!function())
        return true;

    frame.pushThis();
    frame.popRegsAndSync(1);

    ICTypeMonitor_Fallback::Compiler compiler(cx, ICStubCompiler::Engine::Baseline,
                                              (uint32_t) 0);
    if (!emitNonOpIC(compiler.getStub(&stubSpace_)))
        return false;

    for (size_t i = 0; i < function()->nargs(); i++) {
        frame.pushArg(i);
        frame.popRegsAndSync(1);

        ICTypeMonitor_Fallback::Compiler compiler(cx, ICStubCompiler::Engine::Baseline,
                                                  i + 1);
        if (!emitNonOpIC(compiler.getStub(&stubSpace_)))
            return false;
    }

    return true;
}

bool
BaselineCompiler::emitDebugTrap()
{
    MOZ_ASSERT(compileDebugInstrumentation_);
    MOZ_ASSERT(frame.numUnsyncedSlots() == 0);

    bool enabled = script->stepModeEnabled() || script->hasBreakpointsAt(pc);

    // Emit patchable call to debug trap handler.
    JitCode* handler = cx->runtime()->jitRuntime()->debugTrapHandler(cx);
    if (!handler)
        return false;
    mozilla::DebugOnly<CodeOffset> offset = masm.toggledCall(handler, enabled);

#ifdef DEBUG
    // Patchable call offset has to match the pc mapping offset.
    PCMappingEntry& entry = pcMappingEntries_.back();
    MOZ_ASSERT((&offset)->offset() == entry.nativeOffset);
#endif

    // Add an IC entry for the return offset -> pc mapping.
    return appendICEntry(ICEntry::Kind_DebugTrap, masm.currentOffset());
}

void
BaselineCompiler::emitCoverage(jsbytecode* pc)
{
    PCCounts* counts = script->maybeGetPCCounts(pc);
    if (!counts)
        return;

    uint64_t* counterAddr = &counts->numExec();
    masm.inc64(AbsoluteAddress(counterAddr));
}

#ifdef JS_TRACE_LOGGING
bool
BaselineCompiler::emitTraceLoggerEnter()
{
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    Register loggerReg = regs.takeAnyGeneral();
    Register scriptReg = regs.takeAnyGeneral();

    Label noTraceLogger;
    traceLoggerEnterToggleOffset_ = masm.toggledJump(&noTraceLogger);

    masm.Push(loggerReg);
    masm.Push(scriptReg);

    masm.movePtr(ImmPtr(logger), loggerReg);

    // Script start.
    masm.movePtr(ImmGCPtr(script), scriptReg);
    masm.loadPtr(Address(scriptReg, JSScript::offsetOfBaselineScript()), scriptReg);
    Address scriptEvent(scriptReg, BaselineScript::offsetOfTraceLoggerScriptEvent());
    masm.computeEffectiveAddress(scriptEvent, scriptReg);
    masm.tracelogStartEvent(loggerReg, scriptReg);

    // Engine start.
    masm.tracelogStartId(loggerReg, TraceLogger_Baseline, /* force = */ true);

    masm.Pop(scriptReg);
    masm.Pop(loggerReg);

    masm.bind(&noTraceLogger);

    return true;
}

bool
BaselineCompiler::emitTraceLoggerExit()
{
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    Register loggerReg = regs.takeAnyGeneral();

    Label noTraceLogger;
    traceLoggerExitToggleOffset_ = masm.toggledJump(&noTraceLogger);

    masm.Push(loggerReg);
    masm.movePtr(ImmPtr(logger), loggerReg);

    masm.tracelogStopId(loggerReg, TraceLogger_Baseline, /* force = */ true);
    masm.tracelogStopId(loggerReg, TraceLogger_Scripts, /* force = */ true);

    masm.Pop(loggerReg);

    masm.bind(&noTraceLogger);

    return true;
}
#endif

void
BaselineCompiler::emitProfilerEnterFrame()
{
    // Store stack position to lastProfilingFrame variable, guarded by a toggled jump.
    // Starts off initially disabled.
    Label noInstrument;
    CodeOffset toggleOffset = masm.toggledJump(&noInstrument);
    masm.profilerEnterFrame(masm.getStackPointer(), R0.scratchReg());
    masm.bind(&noInstrument);

    // Store the start offset in the appropriate location.
    MOZ_ASSERT(!profilerEnterFrameToggleOffset_.bound());
    profilerEnterFrameToggleOffset_ = toggleOffset;
}

void
BaselineCompiler::emitProfilerExitFrame()
{
    // Store previous frame to lastProfilingFrame variable, guarded by a toggled jump.
    // Starts off initially disabled.
    Label noInstrument;
    CodeOffset toggleOffset = masm.toggledJump(&noInstrument);
    masm.profilerExitFrame();
    masm.bind(&noInstrument);

    // Store the start offset in the appropriate location.
    MOZ_ASSERT(!profilerExitFrameToggleOffset_.bound());
    profilerExitFrameToggleOffset_ = toggleOffset;
}

MethodStatus
BaselineCompiler::emitBody()
{
    MOZ_ASSERT(pc == script->code());

    bool lastOpUnreachable = false;
    uint32_t emittedOps = 0;
    mozilla::DebugOnly<jsbytecode*> prevpc = pc;
    bool compileCoverage = script->hasScriptCounts();

    while (true) {
        JSOp op = JSOp(*pc);
        JitSpew(JitSpew_BaselineOp, "Compiling op @ %d: %s",
                int(script->pcToOffset(pc)), CodeName[op]);

        BytecodeInfo* info = analysis_.maybeInfo(pc);

        // Skip unreachable ops.
        if (!info) {
            // Test if last instructions and stop emitting in that case.
            pc += GetBytecodeLength(pc);
            if (pc >= script->codeEnd())
                break;

            lastOpUnreachable = true;
            prevpc = pc;
            continue;
        }

        // Fully sync the stack if there are incoming jumps.
        if (info->jumpTarget) {
            frame.syncStack(0);
            frame.setStackDepth(info->stackDepth);
        }

        // Always sync in debug mode.
        if (compileDebugInstrumentation_)
            frame.syncStack(0);

        // At the beginning of any op, at most the top 2 stack-values are unsynced.
        if (frame.stackDepth() > 2)
            frame.syncStack(2);

        frame.assertValidState(*info);

        masm.bind(labelOf(pc));

        // Add a PC -> native mapping entry for the current op. These entries are
        // used when we need the native code address for a given pc, for instance
        // for bailouts from Ion, the debugger and exception handling. See
        // PCMappingIndexEntry for more information.
        bool addIndexEntry = (pc == script->code() || lastOpUnreachable || emittedOps > 100);
        if (addIndexEntry)
            emittedOps = 0;
        if (!addPCMappingEntry(addIndexEntry)) {
            ReportOutOfMemory(cx);
            return Method_Error;
        }

        // Emit traps for breakpoints and step mode.
        if (compileDebugInstrumentation_ && !emitDebugTrap())
            return Method_Error;

        // Emit code coverage code, to fill the same data as the interpreter.
        if (compileCoverage)
            emitCoverage(pc);

        switch (op) {
          default:
            JitSpew(JitSpew_BaselineAbort, "Unhandled op: %s", CodeName[op]);
            return Method_CantCompile;

#define EMIT_OP(OP)                            \
          case OP:                             \
            if (!this->emit_##OP())            \
                return Method_Error;           \
            break;
OPCODE_LIST(EMIT_OP)
#undef EMIT_OP
        }

        // Test if last instructions and stop emitting in that case.
        pc += GetBytecodeLength(pc);
        if (pc >= script->codeEnd())
            break;

        emittedOps++;
        lastOpUnreachable = false;
#ifdef DEBUG
        prevpc = pc;
#endif
    }

    MOZ_ASSERT(JSOp(*prevpc) == JSOP_RETRVAL);
    return Method_Compiled;
}

bool
BaselineCompiler::emit_JSOP_NOP()
{
    return true;
}

bool
BaselineCompiler::emit_JSOP_LABEL()
{
    return true;
}

bool
BaselineCompiler::emit_JSOP_POP()
{
    frame.pop();
    return true;
}

bool
BaselineCompiler::emit_JSOP_POPN()
{
    frame.popn(GET_UINT16(pc));
    return true;
}

bool
BaselineCompiler::emit_JSOP_DUPAT()
{
    frame.syncStack(0);

    // DUPAT takes a value on the stack and re-pushes it on top.  It's like
    // GETLOCAL but it addresses from the top of the stack instead of from the
    // stack frame.

    int depth = -(GET_UINT24(pc) + 1);
    masm.loadValue(frame.addressOfStackValue(frame.peek(depth)), R0);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_DUP()
{
    // Keep top stack value in R0, sync the rest so that we can use R1. We use
    // separate registers because every register can be used by at most one
    // StackValue.
    frame.popRegsAndSync(1);
    masm.moveValue(R0, R1);

    // inc/dec ops use DUP followed by ONE, ADD. Push R0 last to avoid a move.
    frame.push(R1);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_DUP2()
{
    frame.syncStack(0);

    masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

    frame.push(R0);
    frame.push(R1);
    return true;
}

bool
BaselineCompiler::emit_JSOP_SWAP()
{
    // Keep top stack values in R0 and R1.
    frame.popRegsAndSync(2);

    frame.push(R1);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_PICK()
{
    frame.syncStack(0);

    // Pick takes a value on the stack and moves it to the top.
    // For instance, pick 2:
    //     before: A B C D E
    //     after : A B D E C

    // First, move value at -(amount + 1) into R0.
    int depth = -(GET_INT8(pc) + 1);
    masm.loadValue(frame.addressOfStackValue(frame.peek(depth)), R0);

    // Move the other values down.
    depth++;
    for (; depth < 0; depth++) {
        Address source = frame.addressOfStackValue(frame.peek(depth));
        Address dest = frame.addressOfStackValue(frame.peek(depth - 1));
        masm.loadValue(source, R1);
        masm.storeValue(R1, dest);
    }

    // Push R0.
    frame.pop();
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_GOTO()
{
    frame.syncStack(0);

    jsbytecode* target = pc + GET_JUMP_OFFSET(pc);
    masm.jump(labelOf(target));
    return true;
}

bool
BaselineCompiler::emitToBoolean()
{
    Label skipIC;
    masm.branchTestBoolean(Assembler::Equal, R0, &skipIC);

    // Call IC
    ICToBool_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    masm.bind(&skipIC);
    return true;
}

bool
BaselineCompiler::emitTest(bool branchIfTrue)
{
    bool knownBoolean = frame.peek(-1)->isKnownBoolean();

    // Keep top stack value in R0.
    frame.popRegsAndSync(1);

    if (!knownBoolean && !emitToBoolean())
        return false;

    // IC will leave a BooleanValue in R0, just need to branch on it.
    masm.branchTestBooleanTruthy(branchIfTrue, R0, labelOf(pc + GET_JUMP_OFFSET(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_IFEQ()
{
    return emitTest(false);
}

bool
BaselineCompiler::emit_JSOP_IFNE()
{
    return emitTest(true);
}

bool
BaselineCompiler::emitAndOr(bool branchIfTrue)
{
    bool knownBoolean = frame.peek(-1)->isKnownBoolean();

    // AND and OR leave the original value on the stack.
    frame.syncStack(0);

    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);
    if (!knownBoolean && !emitToBoolean())
        return false;

    masm.branchTestBooleanTruthy(branchIfTrue, R0, labelOf(pc + GET_JUMP_OFFSET(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_AND()
{
    return emitAndOr(false);
}

bool
BaselineCompiler::emit_JSOP_OR()
{
    return emitAndOr(true);
}

bool
BaselineCompiler::emit_JSOP_NOT()
{
    bool knownBoolean = frame.peek(-1)->isKnownBoolean();

    // Keep top stack value in R0.
    frame.popRegsAndSync(1);

    if (!knownBoolean && !emitToBoolean())
        return false;

    masm.notBoolean(R0);

    frame.push(R0, JSVAL_TYPE_BOOLEAN);
    return true;
}

bool
BaselineCompiler::emit_JSOP_POS()
{
    // Keep top stack value in R0.
    frame.popRegsAndSync(1);

    // Inline path for int32 and double.
    Label done;
    masm.branchTestNumber(Assembler::Equal, R0, &done);

    // Call IC.
    ICToNumber_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    masm.bind(&done);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_LOOPHEAD()
{
    return emitInterruptCheck();
}

bool
BaselineCompiler::emit_JSOP_LOOPENTRY()
{
    frame.syncStack(0);
    return emitWarmUpCounterIncrement(LoopEntryCanIonOsr(pc));
}

bool
BaselineCompiler::emit_JSOP_VOID()
{
    frame.pop();
    frame.push(UndefinedValue());
    return true;
}

bool
BaselineCompiler::emit_JSOP_UNDEFINED()
{
    // If this ever changes, change what JSOP_GIMPLICITTHIS does too.
    frame.push(UndefinedValue());
    return true;
}

bool
BaselineCompiler::emit_JSOP_HOLE()
{
    frame.push(MagicValue(JS_ELEMENTS_HOLE));
    return true;
}

bool
BaselineCompiler::emit_JSOP_NULL()
{
    frame.push(NullValue());
    return true;
}

typedef bool (*ThrowUninitializedThisFn)(JSContext*, BaselineFrame* frame);
static const VMFunction ThrowUninitializedThisInfo =
    FunctionInfo<ThrowUninitializedThisFn>(BaselineThrowUninitializedThis);

bool
BaselineCompiler::emit_JSOP_CHECKTHIS()
{
    frame.syncStack(0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

    return emitCheckThis(R0);
}

bool
BaselineCompiler::emitCheckThis(ValueOperand val)
{
    Label thisOK;
    masm.branchTestMagic(Assembler::NotEqual, val, &thisOK);

    prepareVMCall();

    masm.loadBaselineFramePtr(BaselineFrameReg, val.scratchReg());
    pushArg(val.scratchReg());

    if (!callVM(ThrowUninitializedThisInfo))
        return false;

    masm.bind(&thisOK);
    return true;
}

typedef bool (*ThrowBadDerivedReturnFn)(JSContext*, HandleValue);
static const VMFunction ThrowBadDerivedReturnInfo =
    FunctionInfo<ThrowBadDerivedReturnFn>(jit::ThrowBadDerivedReturn);

bool
BaselineCompiler::emit_JSOP_CHECKRETURN()
{
    MOZ_ASSERT(script->isDerivedClassConstructor());

    // Load |this| in R0, return value in R1.
    frame.popRegsAndSync(1);
    emitLoadReturnValue(R1);

    Label done, returnOK;
    masm.branchTestObject(Assembler::Equal, R1, &done);
    masm.branchTestUndefined(Assembler::Equal, R1, &returnOK);

    prepareVMCall();
    pushArg(R1);
    if (!callVM(ThrowBadDerivedReturnInfo))
        return false;
    masm.assumeUnreachable("Should throw on bad derived constructor return");

    masm.bind(&returnOK);

    if (!emitCheckThis(R0))
        return false;

    // Store |this| in the return value slot.
    masm.storeValue(R0, frame.addressOfReturnValue());
    masm.or32(Imm32(BaselineFrame::HAS_RVAL), frame.addressOfFlags());

    masm.bind(&done);
    return true;
}

typedef bool (*GetFunctionThisFn)(JSContext*, BaselineFrame*, MutableHandleValue);
static const VMFunction GetFunctionThisInfo =
    FunctionInfo<GetFunctionThisFn>(jit::BaselineGetFunctionThis);

bool
BaselineCompiler::emit_JSOP_FUNCTIONTHIS()
{
    MOZ_ASSERT(function());
    MOZ_ASSERT(!function()->isArrow());

    frame.pushThis();

    // In strict mode code or self-hosted functions, |this| is left alone.
    if (script->strict() || (function() && function()->isSelfHostedBuiltin()))
        return true;

    // Load |thisv| in R0. Skip the call if it's already an object.
    Label skipCall;
    frame.popRegsAndSync(1);
    masm.branchTestObject(Assembler::Equal, R0, &skipCall);

    prepareVMCall();
    masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());

    pushArg(R1.scratchReg());

    if (!callVM(GetFunctionThisInfo))
        return false;

    masm.bind(&skipCall);
    frame.push(R0);
    return true;
}

typedef bool (*GetNonSyntacticGlobalThisFn)(JSContext*, HandleObject, MutableHandleValue);
static const VMFunction GetNonSyntacticGlobalThisInfo =
    FunctionInfo<GetNonSyntacticGlobalThisFn>(js::GetNonSyntacticGlobalThis);

bool
BaselineCompiler::emit_JSOP_GLOBALTHIS()
{
    frame.syncStack(0);

    if (!script->hasNonSyntacticScope()) {
        ClonedBlockObject* globalLexical = &script->global().lexicalScope();
        masm.moveValue(globalLexical->thisValue(), R0);
        frame.push(R0);
        return true;
    }

    prepareVMCall();

    masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());
    pushArg(R0.scratchReg());

    if (!callVM(GetNonSyntacticGlobalThisInfo))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_TRUE()
{
    frame.push(BooleanValue(true));
    return true;
}

bool
BaselineCompiler::emit_JSOP_FALSE()
{
    frame.push(BooleanValue(false));
    return true;
}

bool
BaselineCompiler::emit_JSOP_ZERO()
{
    frame.push(Int32Value(0));
    return true;
}

bool
BaselineCompiler::emit_JSOP_ONE()
{
    frame.push(Int32Value(1));
    return true;
}

bool
BaselineCompiler::emit_JSOP_INT8()
{
    frame.push(Int32Value(GET_INT8(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_INT32()
{
    frame.push(Int32Value(GET_INT32(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_UINT16()
{
    frame.push(Int32Value(GET_UINT16(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_UINT24()
{
    frame.push(Int32Value(GET_UINT24(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_DOUBLE()
{
    frame.push(script->getConst(GET_UINT32_INDEX(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_STRING()
{
    frame.push(StringValue(script->getAtom(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_SYMBOL()
{
    unsigned which = GET_UINT8(pc);
    JS::Symbol* sym = cx->runtime()->wellKnownSymbols->get(which);
    frame.push(SymbolValue(sym));
    return true;
}

typedef JSObject* (*DeepCloneObjectLiteralFn)(JSContext*, HandleObject, NewObjectKind);
static const VMFunction DeepCloneObjectLiteralInfo =
    FunctionInfo<DeepCloneObjectLiteralFn>(DeepCloneObjectLiteral);

bool
BaselineCompiler::emit_JSOP_OBJECT()
{
    if (JS::CompartmentOptionsRef(cx).cloneSingletons()) {
        RootedObject obj(cx, script->getObject(GET_UINT32_INDEX(pc)));
        if (!obj)
            return false;

        prepareVMCall();

        pushArg(ImmWord(TenuredObject));
        pushArg(ImmGCPtr(obj));

        if (!callVM(DeepCloneObjectLiteralInfo))
            return false;

        // Box and push return value.
        masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
        frame.push(R0);
        return true;
    }

    JS::CompartmentOptionsRef(cx).setSingletonsAsValues();
    frame.push(ObjectValue(*script->getObject(pc)));
    return true;
}

bool
BaselineCompiler::emit_JSOP_CALLSITEOBJ()
{
    RootedObject cso(cx, script->getObject(pc));
    RootedObject raw(cx, script->getObject(GET_UINT32_INDEX(pc) + 1));
    if (!cso || !raw)
        return false;
    RootedValue rawValue(cx);
    rawValue.setObject(*raw);

    if (!ProcessCallSiteObjOperation(cx, cso, raw, rawValue))
        return false;

    frame.push(ObjectValue(*cso));
    return true;
}

typedef JSObject* (*CloneRegExpObjectFn)(JSContext*, JSObject*);
static const VMFunction CloneRegExpObjectInfo =
    FunctionInfo<CloneRegExpObjectFn>(CloneRegExpObject);

bool
BaselineCompiler::emit_JSOP_REGEXP()
{
    RootedObject reObj(cx, script->getRegExp(pc));

    prepareVMCall();
    pushArg(ImmGCPtr(reObj));
    if (!callVM(CloneRegExpObjectInfo))
        return false;

    // Box and push return value.
    masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
    frame.push(R0);
    return true;
}

typedef JSObject* (*LambdaFn)(JSContext*, HandleFunction, HandleObject);
static const VMFunction LambdaInfo = FunctionInfo<LambdaFn>(js::Lambda);

bool
BaselineCompiler::emit_JSOP_LAMBDA()
{
    RootedFunction fun(cx, script->getFunction(GET_UINT32_INDEX(pc)));

    prepareVMCall();
    masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());

    pushArg(R0.scratchReg());
    pushArg(ImmGCPtr(fun));

    if (!callVM(LambdaInfo))
        return false;

    // Box and push return value.
    masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
    frame.push(R0);
    return true;
}

typedef JSObject* (*LambdaArrowFn)(JSContext*, HandleFunction, HandleObject, HandleValue);
static const VMFunction LambdaArrowInfo = FunctionInfo<LambdaArrowFn>(js::LambdaArrow);

bool
BaselineCompiler::emit_JSOP_LAMBDA_ARROW()
{
    // Keep pushed newTarget in R0.
    frame.popRegsAndSync(1);

    RootedFunction fun(cx, script->getFunction(GET_UINT32_INDEX(pc)));

    prepareVMCall();
    masm.loadPtr(frame.addressOfScopeChain(), R2.scratchReg());

    pushArg(R0);
    pushArg(R2.scratchReg());
    pushArg(ImmGCPtr(fun));

    if (!callVM(LambdaArrowInfo))
        return false;

    // Box and push return value.
    masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
    frame.push(R0);
    return true;
}

void
BaselineCompiler::storeValue(const StackValue* source, const Address& dest,
                             const ValueOperand& scratch)
{
    switch (source->kind()) {
      case StackValue::Constant:
        masm.storeValue(source->constant(), dest);
        break;
      case StackValue::Register:
        masm.storeValue(source->reg(), dest);
        break;
      case StackValue::LocalSlot:
        masm.loadValue(frame.addressOfLocal(source->localSlot()), scratch);
        masm.storeValue(scratch, dest);
        break;
      case StackValue::ArgSlot:
        masm.loadValue(frame.addressOfArg(source->argSlot()), scratch);
        masm.storeValue(scratch, dest);
        break;
      case StackValue::ThisSlot:
        masm.loadValue(frame.addressOfThis(), scratch);
        masm.storeValue(scratch, dest);
        break;
      case StackValue::EvalNewTargetSlot:
        MOZ_ASSERT(script->isForEval());
        masm.loadValue(frame.addressOfEvalNewTarget(), scratch);
        masm.storeValue(scratch, dest);
        break;
      case StackValue::Stack:
        masm.loadValue(frame.addressOfStackValue(source), scratch);
        masm.storeValue(scratch, dest);
        break;
      default:
        MOZ_CRASH("Invalid kind");
    }
}

bool
BaselineCompiler::emit_JSOP_BITOR()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_BITXOR()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_BITAND()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_LSH()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_RSH()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_URSH()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_ADD()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_SUB()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_MUL()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_DIV()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_MOD()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emit_JSOP_POW()
{
    return emitBinaryArith();
}

bool
BaselineCompiler::emitBinaryArith()
{
    // Keep top JSStack value in R0 and R2
    frame.popRegsAndSync(2);

    // Call IC
    ICBinaryArith_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::Baseline);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emitUnaryArith()
{
    // Keep top stack value in R0.
    frame.popRegsAndSync(1);

    // Call IC
    ICUnaryArith_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::Baseline);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_BITNOT()
{
    return emitUnaryArith();
}

bool
BaselineCompiler::emit_JSOP_NEG()
{
    return emitUnaryArith();
}

bool
BaselineCompiler::emit_JSOP_LT()
{
    return emitCompare();
}

bool
BaselineCompiler::emit_JSOP_LE()
{
    return emitCompare();
}

bool
BaselineCompiler::emit_JSOP_GT()
{
    return emitCompare();
}

bool
BaselineCompiler::emit_JSOP_GE()
{
    return emitCompare();
}

bool
BaselineCompiler::emit_JSOP_EQ()
{
    return emitCompare();
}

bool
BaselineCompiler::emit_JSOP_NE()
{
    return emitCompare();
}

bool
BaselineCompiler::emitCompare()
{
    // CODEGEN

    // Keep top JSStack value in R0 and R1.
    frame.popRegsAndSync(2);

    // Call IC.
    ICCompare_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::Baseline);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0, JSVAL_TYPE_BOOLEAN);
    return true;
}

bool
BaselineCompiler::emit_JSOP_STRICTEQ()
{
    return emitCompare();
}

bool
BaselineCompiler::emit_JSOP_STRICTNE()
{
    return emitCompare();
}

bool
BaselineCompiler::emit_JSOP_CONDSWITCH()
{
    return true;
}

bool
BaselineCompiler::emit_JSOP_CASE()
{
    frame.popRegsAndSync(2);
    frame.push(R0);
    frame.syncStack(0);

    // Call IC.
    ICCompare_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::Baseline);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    Register payload = masm.extractInt32(R0, R0.scratchReg());
    jsbytecode* target = pc + GET_JUMP_OFFSET(pc);

    Label done;
    masm.branch32(Assembler::Equal, payload, Imm32(0), &done);
    {
        // Pop the switch value if the case matches.
        masm.addToStackPtr(Imm32(sizeof(Value)));
        masm.jump(labelOf(target));
    }
    masm.bind(&done);
    return true;
}

bool
BaselineCompiler::emit_JSOP_DEFAULT()
{
    frame.pop();
    return emit_JSOP_GOTO();
}

bool
BaselineCompiler::emit_JSOP_LINENO()
{
    return true;
}

bool
BaselineCompiler::emit_JSOP_NEWARRAY()
{
    frame.syncStack(0);

    uint32_t length = GET_UINT32(pc);
    MOZ_ASSERT(length <= INT32_MAX,
               "the bytecode emitter must fail to compile code that would "
               "produce JSOP_NEWARRAY with a length exceeding int32_t range");

    // Pass length in R0.
    masm.move32(Imm32(AssertedCast<int32_t>(length)), R0.scratchReg());

    ObjectGroup* group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Array);
    if (!group)
        return false;

    ICNewArray_Fallback::Compiler stubCompiler(cx, group);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_SPREADCALLARRAY()
{
    return emit_JSOP_NEWARRAY();
}

typedef JSObject* (*NewArrayCopyOnWriteFn)(JSContext*, HandleArrayObject, gc::InitialHeap);
const VMFunction jit::NewArrayCopyOnWriteInfo =
    FunctionInfo<NewArrayCopyOnWriteFn>(js::NewDenseCopyOnWriteArray);

bool
BaselineCompiler::emit_JSOP_NEWARRAY_COPYONWRITE()
{
    RootedScript scriptRoot(cx, script);
    JSObject* obj = ObjectGroup::getOrFixupCopyOnWriteObject(cx, scriptRoot, pc);
    if (!obj)
        return false;

    prepareVMCall();

    pushArg(Imm32(gc::DefaultHeap));
    pushArg(ImmGCPtr(obj));

    if (!callVM(NewArrayCopyOnWriteInfo))
        return false;

    // Box and push return value.
    masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_INITELEM_ARRAY()
{
    // Keep the object and rhs on the stack.
    frame.syncStack(0);

    // Load object in R0, index in R1.
    masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
    uint32_t index = GET_UINT32(pc);
    MOZ_ASSERT(index <= INT32_MAX,
               "the bytecode emitter must fail to compile code that would "
               "produce JSOP_INITELEM_ARRAY with a length exceeding "
               "int32_t range");
    masm.moveValue(Int32Value(AssertedCast<int32_t>(index)), R1);

    // Call IC.
    ICSetElem_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Pop the rhs, so that the object is on the top of the stack.
    frame.pop();
    return true;
}

bool
BaselineCompiler::emit_JSOP_NEWOBJECT()
{
    frame.syncStack(0);

    ICNewObject_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_NEWINIT()
{
    frame.syncStack(0);
    JSProtoKey key = JSProtoKey(GET_UINT8(pc));

    if (key == JSProto_Array) {
        // Pass length in R0.
        masm.move32(Imm32(0), R0.scratchReg());

        ObjectGroup* group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Array);
        if (!group)
            return false;

        ICNewArray_Fallback::Compiler stubCompiler(cx, group);
        if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
            return false;
    } else {
        MOZ_ASSERT(key == JSProto_Object);

        ICNewObject_Fallback::Compiler stubCompiler(cx);
        if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
            return false;
    }

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_INITELEM()
{
    // Store RHS in the scratch slot.
    storeValue(frame.peek(-1), frame.addressOfScratchValue(), R2);
    frame.pop();

    // Keep object and index in R0 and R1.
    frame.popRegsAndSync(2);

    // Push the object to store the result of the IC.
    frame.push(R0);
    frame.syncStack(0);

    // Keep RHS on the stack.
    frame.pushScratchValue();

    // Call IC.
    ICSetElem_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Pop the rhs, so that the object is on the top of the stack.
    frame.pop();
    return true;
}

bool
BaselineCompiler::emit_JSOP_INITHIDDENELEM()
{
    return emit_JSOP_INITELEM();
}

typedef bool (*MutateProtoFn)(JSContext* cx, HandlePlainObject obj, HandleValue newProto);
static const VMFunction MutateProtoInfo = FunctionInfo<MutateProtoFn>(MutatePrototype);

bool
BaselineCompiler::emit_JSOP_MUTATEPROTO()
{
    // Keep values on the stack for the decompiler.
    frame.syncStack(0);

    masm.extractObject(frame.addressOfStackValue(frame.peek(-2)), R0.scratchReg());
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

    prepareVMCall();

    pushArg(R1);
    pushArg(R0.scratchReg());

    if (!callVM(MutateProtoInfo))
        return false;

    frame.pop();
    return true;
}

bool
BaselineCompiler::emit_JSOP_INITPROP()
{
    // Keep lhs in R0, rhs in R1.
    frame.popRegsAndSync(2);

    // Push the object to store the result of the IC.
    frame.push(R0);
    frame.syncStack(0);

    // Call IC.
    ICSetProp_Fallback::Compiler compiler(cx);
    return emitOpIC(compiler.getStub(&stubSpace_));
}

bool
BaselineCompiler::emit_JSOP_INITLOCKEDPROP()
{
    return emit_JSOP_INITPROP();
}

bool
BaselineCompiler::emit_JSOP_INITHIDDENPROP()
{
    return emit_JSOP_INITPROP();
}

typedef bool (*NewbornArrayPushFn)(JSContext*, HandleObject, const Value&);
static const VMFunction NewbornArrayPushInfo = FunctionInfo<NewbornArrayPushFn>(NewbornArrayPush);

bool
BaselineCompiler::emit_JSOP_ARRAYPUSH()
{
    // Keep value in R0, object in R1.
    frame.popRegsAndSync(2);
    masm.unboxObject(R1, R1.scratchReg());

    prepareVMCall();

    pushArg(R0);
    pushArg(R1.scratchReg());

    return callVM(NewbornArrayPushInfo);
}

bool
BaselineCompiler::emit_JSOP_GETELEM()
{
    // Keep top two stack values in R0 and R1.
    frame.popRegsAndSync(2);

    // Call IC.
    ICGetElem_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_CALLELEM()
{
    return emit_JSOP_GETELEM();
}

bool
BaselineCompiler::emit_JSOP_SETELEM()
{
    // Store RHS in the scratch slot.
    storeValue(frame.peek(-1), frame.addressOfScratchValue(), R2);
    frame.pop();

    // Keep object and index in R0 and R1.
    frame.popRegsAndSync(2);

    // Keep RHS on the stack.
    frame.pushScratchValue();

    // Call IC.
    ICSetElem_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    return true;
}

bool
BaselineCompiler::emit_JSOP_STRICTSETELEM()
{
    return emit_JSOP_SETELEM();
}

typedef bool (*DeleteElementFn)(JSContext*, HandleValue, HandleValue, bool*);
static const VMFunction DeleteElementStrictInfo
    = FunctionInfo<DeleteElementFn>(DeleteElementJit<true>);
static const VMFunction DeleteElementNonStrictInfo
    = FunctionInfo<DeleteElementFn>(DeleteElementJit<false>);

bool
BaselineCompiler::emit_JSOP_DELELEM()
{
    // Keep values on the stack for the decompiler.
    frame.syncStack(0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R1);

    prepareVMCall();

    pushArg(R1);
    pushArg(R0);

    bool strict = JSOp(*pc) == JSOP_STRICTDELELEM;
    if (!callVM(strict ? DeleteElementStrictInfo : DeleteElementNonStrictInfo))
        return false;

    masm.boxNonDouble(JSVAL_TYPE_BOOLEAN, ReturnReg, R1);
    frame.popn(2);
    frame.push(R1);
    return true;
}

bool
BaselineCompiler::emit_JSOP_STRICTDELELEM()
{
    return emit_JSOP_DELELEM();
}

bool
BaselineCompiler::emit_JSOP_IN()
{
    frame.popRegsAndSync(2);

    ICIn_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_GETGNAME()
{
    if (script->hasNonSyntacticScope())
        return emit_JSOP_GETNAME();

    RootedPropertyName name(cx, script->getName(pc));

    // These names are non-configurable on the global and cannot be shadowed.
    if (name == cx->names().undefined) {
        frame.push(UndefinedValue());
        return true;
    }
    if (name == cx->names().NaN) {
        frame.push(cx->runtime()->NaNValue);
        return true;
    }
    if (name == cx->names().Infinity) {
        frame.push(cx->runtime()->positiveInfinityValue);
        return true;
    }

    frame.syncStack(0);

    masm.movePtr(ImmGCPtr(&script->global().lexicalScope()), R0.scratchReg());

    // Call IC.
    ICGetName_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_BINDGNAME()
{
    if (!script->hasNonSyntacticScope()) {
        // We can bind name to the global lexical scope if the binding already
        // exists, is initialized, and is writable (i.e., an initialized
        // 'let') at compile time.
        RootedPropertyName name(cx, script->getName(pc));
        Rooted<ClonedBlockObject*> globalLexical(cx, &script->global().lexicalScope());
        if (Shape* shape = globalLexical->lookup(cx, name)) {
            if (shape->writable() &&
                !globalLexical->getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL))
            {
                frame.push(ObjectValue(*globalLexical));
                return true;
            }
        }

        // We can bind name to the global object if the property exists on the
        // global and is non-configurable, as then it cannot be shadowed.
        if (Shape* shape = script->global().lookup(cx, name)) {
            if (!shape->configurable()) {
                frame.push(ObjectValue(script->global()));
                return true;
            }
        }

        // Otherwise we have to use the dynamic scope chain.
    }

    return emit_JSOP_BINDNAME();
}

bool
BaselineCompiler::emit_JSOP_SETPROP()
{
    // Keep lhs in R0, rhs in R1.
    frame.popRegsAndSync(2);

    // Call IC.
    ICSetProp_Fallback::Compiler compiler(cx);
    if (!emitOpIC(compiler.getStub(&stubSpace_)))
        return false;

    // The IC will return the RHS value in R0, mark it as pushed value.
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_STRICTSETPROP()
{
    return emit_JSOP_SETPROP();
}

bool
BaselineCompiler::emit_JSOP_SETNAME()
{
    return emit_JSOP_SETPROP();
}

bool
BaselineCompiler::emit_JSOP_STRICTSETNAME()
{
    return emit_JSOP_SETPROP();
}

bool
BaselineCompiler::emit_JSOP_SETGNAME()
{
    return emit_JSOP_SETPROP();
}

bool
BaselineCompiler::emit_JSOP_STRICTSETGNAME()
{
    return emit_JSOP_SETPROP();
}

bool
BaselineCompiler::emit_JSOP_GETPROP()
{
    // Keep object in R0.
    frame.popRegsAndSync(1);

    // Call IC.
    ICGetProp_Fallback::Compiler compiler(cx, ICStubCompiler::Engine::Baseline);
    if (!emitOpIC(compiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_CALLPROP()
{
    return emit_JSOP_GETPROP();
}

bool
BaselineCompiler::emit_JSOP_LENGTH()
{
    return emit_JSOP_GETPROP();
}

bool
BaselineCompiler::emit_JSOP_GETXPROP()
{
    return emit_JSOP_GETPROP();
}

typedef bool (*DeletePropertyFn)(JSContext*, HandleValue, HandlePropertyName, bool*);
static const VMFunction DeletePropertyStrictInfo =
    FunctionInfo<DeletePropertyFn>(DeletePropertyJit<true>);
static const VMFunction DeletePropertyNonStrictInfo =
    FunctionInfo<DeletePropertyFn>(DeletePropertyJit<false>);

bool
BaselineCompiler::emit_JSOP_DELPROP()
{
    // Keep value on the stack for the decompiler.
    frame.syncStack(0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

    prepareVMCall();

    pushArg(ImmGCPtr(script->getName(pc)));
    pushArg(R0);

    bool strict = JSOp(*pc) == JSOP_STRICTDELPROP;
    if (!callVM(strict ? DeletePropertyStrictInfo : DeletePropertyNonStrictInfo))
        return false;

    masm.boxNonDouble(JSVAL_TYPE_BOOLEAN, ReturnReg, R1);
    frame.pop();
    frame.push(R1);
    return true;
}

bool
BaselineCompiler::emit_JSOP_STRICTDELPROP()
{
    return emit_JSOP_DELPROP();
}

void
BaselineCompiler::getScopeCoordinateObject(Register reg)
{
    ScopeCoordinate sc(pc);

    masm.loadPtr(frame.addressOfScopeChain(), reg);
    for (unsigned i = sc.hops(); i; i--)
        masm.extractObject(Address(reg, ScopeObject::offsetOfEnclosingScope()), reg);
}

Address
BaselineCompiler::getScopeCoordinateAddressFromObject(Register objReg, Register reg)
{
    ScopeCoordinate sc(pc);
    Shape* shape = ScopeCoordinateToStaticScopeShape(script, pc);

    Address addr;
    if (shape->numFixedSlots() <= sc.slot()) {
        masm.loadPtr(Address(objReg, NativeObject::offsetOfSlots()), reg);
        return Address(reg, (sc.slot() - shape->numFixedSlots()) * sizeof(Value));
    }

    return Address(objReg, NativeObject::getFixedSlotOffset(sc.slot()));
}

Address
BaselineCompiler::getScopeCoordinateAddress(Register reg)
{
    getScopeCoordinateObject(reg);
    return getScopeCoordinateAddressFromObject(reg, reg);
}

bool
BaselineCompiler::emit_JSOP_GETALIASEDVAR()
{
    frame.syncStack(0);

    Address address = getScopeCoordinateAddress(R0.scratchReg());
    masm.loadValue(address, R0);

    if (ionCompileable_) {
        // No need to monitor types if we know Ion can't compile this script.
        ICTypeMonitor_Fallback::Compiler compiler(cx, ICStubCompiler::Engine::Baseline,
                                                  (ICMonitoredFallbackStub*) nullptr);
        if (!emitOpIC(compiler.getStub(&stubSpace_)))
            return false;
    }

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_SETALIASEDVAR()
{
    JSScript* outerScript = ScopeCoordinateFunctionScript(script, pc);
    if (outerScript && outerScript->treatAsRunOnce()) {
        // Type updates for this operation might need to be tracked, so treat
        // this as a SETPROP.

        // Load rhs into R1.
        frame.syncStack(1);
        frame.popValue(R1);

        // Load and box lhs into R0.
        getScopeCoordinateObject(R2.scratchReg());
        masm.tagValue(JSVAL_TYPE_OBJECT, R2.scratchReg(), R0);

        // Call SETPROP IC.
        ICSetProp_Fallback::Compiler compiler(cx);
        if (!emitOpIC(compiler.getStub(&stubSpace_)))
            return false;

        // The IC will return the RHS value in R0, mark it as pushed value.
        frame.push(R0);
        return true;
    }

    // Keep rvalue in R0.
    frame.popRegsAndSync(1);
    Register objReg = R2.scratchReg();

    getScopeCoordinateObject(objReg);
    Address address = getScopeCoordinateAddressFromObject(objReg, R1.scratchReg());
    masm.patchableCallPreBarrier(address, MIRType_Value);
    masm.storeValue(R0, address);
    frame.push(R0);

    // Only R0 is live at this point.
    // Scope coordinate object is already in R2.scratchReg().
    Register temp = R1.scratchReg();

    Label skipBarrier;
    masm.branchPtrInNurseryRange(Assembler::Equal, objReg, temp, &skipBarrier);
    masm.branchValueIsNurseryObject(Assembler::NotEqual, R0, temp, &skipBarrier);

    masm.call(&postBarrierSlot_); // Won't clobber R0

    masm.bind(&skipBarrier);
    return true;
}

bool
BaselineCompiler::emit_JSOP_GETNAME()
{
    frame.syncStack(0);

    masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());

    // Call IC.
    ICGetName_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_BINDNAME()
{
    frame.syncStack(0);

    if (*pc == JSOP_BINDGNAME && !script->hasNonSyntacticScope())
        masm.movePtr(ImmGCPtr(&script->global().lexicalScope()), R0.scratchReg());
    else
        masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());

    // Call IC.
    ICBindName_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0);
    return true;
}

typedef bool (*DeleteNameFn)(JSContext*, HandlePropertyName, HandleObject,
                             MutableHandleValue);
static const VMFunction DeleteNameInfo = FunctionInfo<DeleteNameFn>(DeleteNameOperation);

bool
BaselineCompiler::emit_JSOP_DELNAME()
{
    frame.syncStack(0);
    masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());

    prepareVMCall();

    pushArg(R0.scratchReg());
    pushArg(ImmGCPtr(script->getName(pc)));

    if (!callVM(DeleteNameInfo))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_GETIMPORT()
{
    ModuleEnvironmentObject* env = GetModuleEnvironmentForScript(script);
    MOZ_ASSERT(env);

    ModuleEnvironmentObject* targetEnv;
    Shape* shape;
    MOZ_ALWAYS_TRUE(env->lookupImport(NameToId(script->getName(pc)), &targetEnv, &shape));

    EnsureTrackPropertyTypes(cx, targetEnv, shape->propid());

    frame.syncStack(0);

    uint32_t slot = shape->slot();
    Register scratch = R0.scratchReg();
    masm.movePtr(ImmGCPtr(targetEnv), scratch);
    if (slot < targetEnv->numFixedSlots()) {
        masm.loadValue(Address(scratch, NativeObject::getFixedSlotOffset(slot)), R0);
    } else {
        masm.loadPtr(Address(scratch, NativeObject::offsetOfSlots()), scratch);
        masm.loadValue(Address(scratch, (slot - targetEnv->numFixedSlots()) * sizeof(Value)), R0);
    }

    // Imports are initialized by this point except in rare circumstances, so
    // don't emit a check unless we have to.
    if (targetEnv->getSlot(shape->slot()).isMagic(JS_UNINITIALIZED_LEXICAL))
        emitUninitializedLexicalCheck(R0);

    if (ionCompileable_) {
        // No need to monitor types if we know Ion can't compile this script.
        ICTypeMonitor_Fallback::Compiler compiler(cx, ICStubCompiler::Engine::Baseline,
                                                  (ICMonitoredFallbackStub*) nullptr);
        if (!emitOpIC(compiler.getStub(&stubSpace_)))
            return false;
    }

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_GETINTRINSIC()
{
    frame.syncStack(0);

    ICGetIntrinsic_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    frame.push(R0);
    return true;
}

typedef bool (*DefVarFn)(JSContext*, HandlePropertyName, unsigned, HandleObject);
static const VMFunction DefVarInfo = FunctionInfo<DefVarFn>(DefVar);

bool
BaselineCompiler::emit_JSOP_DEFVAR()
{
    frame.syncStack(0);

    unsigned attrs = JSPROP_ENUMERATE;
    if (!script->isForEval())
        attrs |= JSPROP_PERMANENT;
    MOZ_ASSERT(attrs <= UINT32_MAX);

    masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());

    prepareVMCall();

    pushArg(R0.scratchReg());
    pushArg(Imm32(attrs));
    pushArg(ImmGCPtr(script->getName(pc)));

    return callVM(DefVarInfo);
}

typedef bool (*DefLexicalFn)(JSContext*, HandlePropertyName, unsigned, HandleObject);
static const VMFunction DefLexicalInfo = FunctionInfo<DefLexicalFn>(DefLexical);

bool
BaselineCompiler::emit_JSOP_DEFCONST()
{
    return emit_JSOP_DEFLET();
}

bool
BaselineCompiler::emit_JSOP_DEFLET()
{
    frame.syncStack(0);

    unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;
    if (*pc == JSOP_DEFCONST)
        attrs |= JSPROP_READONLY;
    MOZ_ASSERT(attrs <= UINT32_MAX);

    masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());

    prepareVMCall();

    pushArg(R0.scratchReg());
    pushArg(Imm32(attrs));
    pushArg(ImmGCPtr(script->getName(pc)));

    return callVM(DefLexicalInfo);
}

typedef bool (*DefFunOperationFn)(JSContext*, HandleScript, HandleObject, HandleFunction);
static const VMFunction DefFunOperationInfo = FunctionInfo<DefFunOperationFn>(DefFunOperation);

bool
BaselineCompiler::emit_JSOP_DEFFUN()
{
    RootedFunction fun(cx, script->getFunction(GET_UINT32_INDEX(pc)));

    frame.syncStack(0);
    masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());

    prepareVMCall();

    pushArg(ImmGCPtr(fun));
    pushArg(R0.scratchReg());
    pushArg(ImmGCPtr(script));

    return callVM(DefFunOperationInfo);
}

typedef bool (*InitPropGetterSetterFn)(JSContext*, jsbytecode*, HandleObject, HandlePropertyName,
                                       HandleObject);
static const VMFunction InitPropGetterSetterInfo =
    FunctionInfo<InitPropGetterSetterFn>(InitGetterSetterOperation);

bool
BaselineCompiler::emitInitPropGetterSetter()
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_INITPROP_GETTER ||
               JSOp(*pc) == JSOP_INITHIDDENPROP_GETTER ||
               JSOp(*pc) == JSOP_INITPROP_SETTER ||
               JSOp(*pc) == JSOP_INITHIDDENPROP_SETTER);

    // Keep values on the stack for the decompiler.
    frame.syncStack(0);

    prepareVMCall();

    masm.extractObject(frame.addressOfStackValue(frame.peek(-1)), R0.scratchReg());
    masm.extractObject(frame.addressOfStackValue(frame.peek(-2)), R1.scratchReg());

    pushArg(R0.scratchReg());
    pushArg(ImmGCPtr(script->getName(pc)));
    pushArg(R1.scratchReg());
    pushArg(ImmPtr(pc));

    if (!callVM(InitPropGetterSetterInfo))
        return false;

    frame.pop();
    return true;
}

bool
BaselineCompiler::emit_JSOP_INITPROP_GETTER()
{
    return emitInitPropGetterSetter();
}

bool
BaselineCompiler::emit_JSOP_INITHIDDENPROP_GETTER()
{
    return emitInitPropGetterSetter();
}

bool
BaselineCompiler::emit_JSOP_INITPROP_SETTER()
{
    return emitInitPropGetterSetter();
}

bool
BaselineCompiler::emit_JSOP_INITHIDDENPROP_SETTER()
{
    return emitInitPropGetterSetter();
}

typedef bool (*InitElemGetterSetterFn)(JSContext*, jsbytecode*, HandleObject, HandleValue,
                                       HandleObject);
static const VMFunction InitElemGetterSetterInfo =
    FunctionInfo<InitElemGetterSetterFn>(InitGetterSetterOperation);

bool
BaselineCompiler::emitInitElemGetterSetter()
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_INITELEM_GETTER ||
               JSOp(*pc) == JSOP_INITHIDDENELEM_GETTER ||
               JSOp(*pc) == JSOP_INITELEM_SETTER ||
               JSOp(*pc) == JSOP_INITHIDDENELEM_SETTER);

    // Load index and value in R0 and R1, but keep values on the stack for the
    // decompiler.
    frame.syncStack(0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R0);
    masm.extractObject(frame.addressOfStackValue(frame.peek(-1)), R1.scratchReg());

    prepareVMCall();

    pushArg(R1.scratchReg());
    pushArg(R0);
    masm.extractObject(frame.addressOfStackValue(frame.peek(-3)), R0.scratchReg());
    pushArg(R0.scratchReg());
    pushArg(ImmPtr(pc));

    if (!callVM(InitElemGetterSetterInfo))
        return false;

    frame.popn(2);
    return true;
}

bool
BaselineCompiler::emit_JSOP_INITELEM_GETTER()
{
    return emitInitElemGetterSetter();
}

bool
BaselineCompiler::emit_JSOP_INITHIDDENELEM_GETTER()
{
    return emitInitElemGetterSetter();
}

bool
BaselineCompiler::emit_JSOP_INITELEM_SETTER()
{
    return emitInitElemGetterSetter();
}

bool
BaselineCompiler::emit_JSOP_INITHIDDENELEM_SETTER()
{
    return emitInitElemGetterSetter();
}

bool
BaselineCompiler::emit_JSOP_INITELEM_INC()
{
    // Keep the object and rhs on the stack.
    frame.syncStack(0);

    // Load object in R0, index in R1.
    masm.loadValue(frame.addressOfStackValue(frame.peek(-3)), R0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-2)), R1);

    // Call IC.
    ICSetElem_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Pop the rhs
    frame.pop();

    // Increment index
    Address indexAddr = frame.addressOfStackValue(frame.peek(-1));
    masm.incrementInt32Value(indexAddr);
    return true;
}

bool
BaselineCompiler::emit_JSOP_GETLOCAL()
{
    frame.pushLocal(GET_LOCALNO(pc));
    return true;
}

bool
BaselineCompiler::emit_JSOP_SETLOCAL()
{
    // Ensure no other StackValue refers to the old value, for instance i + (i = 3).
    // This also allows us to use R0 as scratch below.
    frame.syncStack(1);

    uint32_t local = GET_LOCALNO(pc);
    storeValue(frame.peek(-1), frame.addressOfLocal(local), R0);
    return true;
}

bool
BaselineCompiler::emitFormalArgAccess(uint32_t arg, bool get)
{
    // Fast path: the script does not use |arguments| or formals don't
    // alias the arguments object.
    if (!script->argumentsAliasesFormals()) {
        if (get) {
            frame.pushArg(arg);
        } else {
            // See the comment in emit_JSOP_SETLOCAL.
            frame.syncStack(1);
            storeValue(frame.peek(-1), frame.addressOfArg(arg), R0);
        }

        return true;
    }

    // Sync so that we can use R0.
    frame.syncStack(0);

    // If the script is known to have an arguments object, we can just use it.
    // Else, we *may* have an arguments object (because we can't invalidate
    // when needsArgsObj becomes |true|), so we have to test HAS_ARGS_OBJ.
    Label done;
    if (!script->needsArgsObj()) {
        Label hasArgsObj;
        masm.branchTest32(Assembler::NonZero, frame.addressOfFlags(),
                          Imm32(BaselineFrame::HAS_ARGS_OBJ), &hasArgsObj);
        if (get)
            masm.loadValue(frame.addressOfArg(arg), R0);
        else
            storeValue(frame.peek(-1), frame.addressOfArg(arg), R0);
        masm.jump(&done);
        masm.bind(&hasArgsObj);
    }

    // Load the arguments object data vector.
    Register reg = R2.scratchReg();
    masm.loadPtr(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfArgsObj()), reg);
    masm.loadPrivate(Address(reg, ArgumentsObject::getDataSlotOffset()), reg);

    // Load/store the argument.
    Address argAddr(reg, ArgumentsData::offsetOfArgs() + arg * sizeof(Value));
    if (get) {
        masm.loadValue(argAddr, R0);
        frame.push(R0);
    } else {
        masm.patchableCallPreBarrier(argAddr, MIRType_Value);
        masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);
        masm.storeValue(R0, argAddr);

        MOZ_ASSERT(frame.numUnsyncedSlots() == 0);

        Register temp = R1.scratchReg();

        // Reload the arguments object
        Register reg = R2.scratchReg();
        masm.loadPtr(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfArgsObj()), reg);

        Label skipBarrier;

        masm.branchPtrInNurseryRange(Assembler::Equal, reg, temp, &skipBarrier);
        masm.branchValueIsNurseryObject(Assembler::NotEqual, R0, temp, &skipBarrier);

        masm.call(&postBarrierSlot_);

        masm.bind(&skipBarrier);
    }

    masm.bind(&done);
    return true;
}

bool
BaselineCompiler::emit_JSOP_GETARG()
{
    uint32_t arg = GET_ARGNO(pc);
    return emitFormalArgAccess(arg, /* get = */ true);
}

bool
BaselineCompiler::emit_JSOP_SETARG()
{
    // Ionmonkey can't inline functions with SETARG with magic arguments.
    if (!script->argsObjAliasesFormals() && script->argumentsAliasesFormals())
        script->setUninlineable();

    modifiesArguments_ = true;

    uint32_t arg = GET_ARGNO(pc);
    return emitFormalArgAccess(arg, /* get = */ false);
}

bool
BaselineCompiler::emit_JSOP_NEWTARGET()
{
    if (script->isForEval()) {
        frame.pushEvalNewTarget();
        return true;
    }

    MOZ_ASSERT(function());
    frame.syncStack(0);

    if (function()->isArrow()) {
        // Arrow functions store their |new.target| value in an
        // extended slot.
        Register scratch = R0.scratchReg();
        masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(), scratch);
        masm.loadValue(Address(scratch, FunctionExtended::offsetOfArrowNewTargetSlot()), R0);
        frame.push(R0);
        return true;
    }

    // if (!isConstructing()) push(undefined)
    Label constructing, done;
    masm.branchTestPtr(Assembler::NonZero, frame.addressOfCalleeToken(),
                       Imm32(CalleeToken_FunctionConstructing), &constructing);
    masm.moveValue(UndefinedValue(), R0);
    masm.jump(&done);

    masm.bind(&constructing);

    // else push(argv[Max(numActualArgs, numFormalArgs)])
    Register argvLen = R0.scratchReg();

    Address actualArgs(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs());
    masm.loadPtr(actualArgs, argvLen);

    Label actualArgsSufficient;

    masm.branchPtr(Assembler::AboveOrEqual, argvLen, Imm32(function()->nargs()),
                   &actualArgsSufficient);
    masm.move32(Imm32(function()->nargs()), argvLen);
    masm.bind(&actualArgsSufficient);

    BaseValueIndex newTarget(BaselineFrameReg, argvLen, BaselineFrame::offsetOfArg(0));
    masm.loadValue(newTarget, R0);

    masm.bind(&done);
    frame.push(R0);

    return true;
}

typedef bool (*ThrowRuntimeLexicalErrorFn)(JSContext* cx, unsigned);
static const VMFunction ThrowRuntimeLexicalErrorInfo =
    FunctionInfo<ThrowRuntimeLexicalErrorFn>(jit::ThrowRuntimeLexicalError);

bool
BaselineCompiler::emitThrowConstAssignment()
{
    prepareVMCall();
    pushArg(Imm32(JSMSG_BAD_CONST_ASSIGN));
    return callVM(ThrowRuntimeLexicalErrorInfo);
}

bool
BaselineCompiler::emit_JSOP_THROWSETCONST()
{
    return emitThrowConstAssignment();
}

bool
BaselineCompiler::emit_JSOP_THROWSETALIASEDCONST()
{
    return emitThrowConstAssignment();
}

bool
BaselineCompiler::emitUninitializedLexicalCheck(const ValueOperand& val)
{
    Label done;
    masm.branchTestMagicValue(Assembler::NotEqual, val, JS_UNINITIALIZED_LEXICAL, &done);

    prepareVMCall();
    pushArg(Imm32(JSMSG_UNINITIALIZED_LEXICAL));
    if (!callVM(ThrowRuntimeLexicalErrorInfo))
        return false;

    masm.bind(&done);
    return true;
}

bool
BaselineCompiler::emit_JSOP_CHECKLEXICAL()
{
    frame.syncStack(0);
    masm.loadValue(frame.addressOfLocal(GET_LOCALNO(pc)), R0);
    return emitUninitializedLexicalCheck(R0);
}

bool
BaselineCompiler::emit_JSOP_INITLEXICAL()
{
    return emit_JSOP_SETLOCAL();
}

bool
BaselineCompiler::emit_JSOP_INITGLEXICAL()
{
    frame.popRegsAndSync(1);
    frame.push(ObjectValue(script->global().lexicalScope()));
    frame.push(R0);
    return emit_JSOP_SETPROP();
}

bool
BaselineCompiler::emit_JSOP_CHECKALIASEDLEXICAL()
{
    frame.syncStack(0);
    masm.loadValue(getScopeCoordinateAddress(R0.scratchReg()), R0);
    return emitUninitializedLexicalCheck(R0);
}

bool
BaselineCompiler::emit_JSOP_INITALIASEDLEXICAL()
{
    return emit_JSOP_SETALIASEDVAR();
}

bool
BaselineCompiler::emit_JSOP_UNINITIALIZED()
{
    frame.push(MagicValue(JS_UNINITIALIZED_LEXICAL));
    return true;
}

bool
BaselineCompiler::emitCall()
{
    MOZ_ASSERT(IsCallPC(pc));

    bool construct = JSOp(*pc) == JSOP_NEW || JSOp(*pc) == JSOP_SUPERCALL;
    uint32_t argc = GET_ARGC(pc);

    frame.syncStack(0);
    masm.move32(Imm32(argc), R0.scratchReg());

    // Call IC
    ICCall_Fallback::Compiler stubCompiler(cx, /* isConstructing = */ construct,
                                           /* isSpread = */ false);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Update FrameInfo.
    frame.popn(2 + argc + construct);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emitSpreadCall()
{
    MOZ_ASSERT(IsCallPC(pc));

    frame.syncStack(0);
    masm.move32(Imm32(1), R0.scratchReg());

    // Call IC
    bool construct = JSOp(*pc) == JSOP_SPREADNEW || JSOp(*pc) == JSOP_SPREADSUPERCALL;
    ICCall_Fallback::Compiler stubCompiler(cx, /* isConstructing = */ construct,
                                           /* isSpread = */ true);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    // Update FrameInfo.
    frame.popn(3 + construct);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_CALL()
{
    return emitCall();
}

bool
BaselineCompiler::emit_JSOP_CALLITER()
{
    return emitCall();
}

bool
BaselineCompiler::emit_JSOP_NEW()
{
    return emitCall();
}

bool
BaselineCompiler::emit_JSOP_SUPERCALL()
{
    return emitCall();
}

bool
BaselineCompiler::emit_JSOP_FUNCALL()
{
    return emitCall();
}

bool
BaselineCompiler::emit_JSOP_FUNAPPLY()
{
    return emitCall();
}

bool
BaselineCompiler::emit_JSOP_EVAL()
{
    return emitCall();
}

bool
BaselineCompiler::emit_JSOP_STRICTEVAL()
{
    return emitCall();
}

bool
BaselineCompiler::emit_JSOP_SPREADCALL()
{
    return emitSpreadCall();
}

bool
BaselineCompiler::emit_JSOP_SPREADNEW()
{
    return emitSpreadCall();
}

bool
BaselineCompiler::emit_JSOP_SPREADSUPERCALL()
{
    return emitSpreadCall();
}

bool
BaselineCompiler::emit_JSOP_SPREADEVAL()
{
    return emitSpreadCall();
}

bool
BaselineCompiler::emit_JSOP_STRICTSPREADEVAL()
{
    return emitSpreadCall();
}

typedef bool (*ImplicitThisFn)(JSContext*, HandleObject, HandlePropertyName,
                               MutableHandleValue);
static const VMFunction ImplicitThisInfo = FunctionInfo<ImplicitThisFn>(ImplicitThisOperation);

bool
BaselineCompiler::emit_JSOP_IMPLICITTHIS()
{
    frame.syncStack(0);
    masm.loadPtr(frame.addressOfScopeChain(), R0.scratchReg());

    prepareVMCall();

    pushArg(ImmGCPtr(script->getName(pc)));
    pushArg(R0.scratchReg());

    if (!callVM(ImplicitThisInfo))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_GIMPLICITTHIS()
{
    if (!script->hasNonSyntacticScope()) {
        frame.push(UndefinedValue());
        return true;
    }

    return emit_JSOP_IMPLICITTHIS();
}

bool
BaselineCompiler::emit_JSOP_INSTANCEOF()
{
    frame.popRegsAndSync(2);

    ICInstanceOf_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_TYPEOF()
{
    frame.popRegsAndSync(1);

    ICTypeOf_Fallback::Compiler stubCompiler(cx);
    if (!emitOpIC(stubCompiler.getStub(&stubSpace_)))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_TYPEOFEXPR()
{
    return emit_JSOP_TYPEOF();
}

typedef bool (*ThrowMsgFn)(JSContext*, const unsigned);
static const VMFunction ThrowMsgInfo = FunctionInfo<ThrowMsgFn>(js::ThrowMsgOperation);

bool
BaselineCompiler::emit_JSOP_THROWMSG()
{
    prepareVMCall();
    pushArg(Imm32(GET_UINT16(pc)));
    return callVM(ThrowMsgInfo);
}

typedef bool (*ThrowFn)(JSContext*, HandleValue);
static const VMFunction ThrowInfo = FunctionInfo<ThrowFn>(js::Throw);

bool
BaselineCompiler::emit_JSOP_THROW()
{
    // Keep value to throw in R0.
    frame.popRegsAndSync(1);

    prepareVMCall();
    pushArg(R0);

    return callVM(ThrowInfo);
}

typedef bool (*ThrowingFn)(JSContext*, HandleValue);
static const VMFunction ThrowingInfo = FunctionInfo<ThrowingFn>(js::ThrowingOperation);

bool
BaselineCompiler::emit_JSOP_THROWING()
{
    // Keep value to throw in R0.
    frame.popRegsAndSync(1);

    prepareVMCall();
    pushArg(R0);

    return callVM(ThrowingInfo);
}

bool
BaselineCompiler::emit_JSOP_TRY()
{
    // Ionmonkey can't inline function with JSOP_TRY.
    script->setUninlineable();
    return true;
}

bool
BaselineCompiler::emit_JSOP_FINALLY()
{
    // JSOP_FINALLY has a def count of 2, but these values are already on the
    // stack (they're pushed by JSOP_GOSUB). Update the compiler's stack state.
    frame.setStackDepth(frame.stackDepth() + 2);

    // To match the interpreter, emit an interrupt check at the start of the
    // finally block.
    return emitInterruptCheck();
}

bool
BaselineCompiler::emit_JSOP_GOSUB()
{
    // Push |false| so that RETSUB knows the value on top of the
    // stack is not an exception but the offset to the op following
    // this GOSUB.
    frame.push(BooleanValue(false));

    int32_t nextOffset = script->pcToOffset(GetNextPc(pc));
    frame.push(Int32Value(nextOffset));

    // Jump to the finally block.
    frame.syncStack(0);
    jsbytecode* target = pc + GET_JUMP_OFFSET(pc);
    masm.jump(labelOf(target));
    return true;
}

bool
BaselineCompiler::emit_JSOP_RETSUB()
{
    frame.popRegsAndSync(2);

    ICRetSub_Fallback::Compiler stubCompiler(cx);
    return emitOpIC(stubCompiler.getStub(&stubSpace_));
}

typedef bool (*PushBlockScopeFn)(JSContext*, BaselineFrame*, Handle<StaticBlockObject*>);
static const VMFunction PushBlockScopeInfo = FunctionInfo<PushBlockScopeFn>(jit::PushBlockScope);

bool
BaselineCompiler::emit_JSOP_PUSHBLOCKSCOPE()
{
    StaticBlockObject& blockObj = script->getObject(pc)->as<StaticBlockObject>();

    // Call a stub to push the block on the block chain.
    prepareVMCall();
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    pushArg(ImmGCPtr(&blockObj));
    pushArg(R0.scratchReg());

    return callVM(PushBlockScopeInfo);
}

typedef bool (*PopBlockScopeFn)(JSContext*, BaselineFrame*);
static const VMFunction PopBlockScopeInfo = FunctionInfo<PopBlockScopeFn>(jit::PopBlockScope);

typedef bool (*DebugLeaveThenPopBlockScopeFn)(JSContext*, BaselineFrame*, jsbytecode*);
static const VMFunction DebugLeaveThenPopBlockScopeInfo =
    FunctionInfo<DebugLeaveThenPopBlockScopeFn>(jit::DebugLeaveThenPopBlockScope);

bool
BaselineCompiler::emit_JSOP_POPBLOCKSCOPE()
{
    prepareVMCall();

    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    if (compileDebugInstrumentation_) {
        pushArg(ImmPtr(pc));
        pushArg(R0.scratchReg());
        return callVM(DebugLeaveThenPopBlockScopeInfo);
    }

    pushArg(R0.scratchReg());
    return callVM(PopBlockScopeInfo);
}

typedef bool (*FreshenBlockScopeFn)(JSContext*, BaselineFrame*);
static const VMFunction FreshenBlockScopeInfo =
    FunctionInfo<FreshenBlockScopeFn>(jit::FreshenBlockScope);

typedef bool (*DebugLeaveThenFreshenBlockScopeFn)(JSContext*, BaselineFrame*, jsbytecode*);
static const VMFunction DebugLeaveThenFreshenBlockScopeInfo =
    FunctionInfo<DebugLeaveThenFreshenBlockScopeFn>(jit::DebugLeaveThenFreshenBlockScope);

bool
BaselineCompiler::emit_JSOP_FRESHENBLOCKSCOPE()
{
    prepareVMCall();

    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    if (compileDebugInstrumentation_) {
        pushArg(ImmPtr(pc));
        pushArg(R0.scratchReg());
        return callVM(DebugLeaveThenFreshenBlockScopeInfo);
    }

    pushArg(R0.scratchReg());
    return callVM(FreshenBlockScopeInfo);
}

typedef bool (*DebugLeaveBlockFn)(JSContext*, BaselineFrame*, jsbytecode*);
static const VMFunction DebugLeaveBlockInfo = FunctionInfo<DebugLeaveBlockFn>(jit::DebugLeaveBlock);

bool
BaselineCompiler::emit_JSOP_DEBUGLEAVEBLOCK()
{
    if (!compileDebugInstrumentation_)
        return true;

    prepareVMCall();
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    pushArg(ImmPtr(pc));
    pushArg(R0.scratchReg());

    return callVM(DebugLeaveBlockInfo);
}

typedef bool (*EnterWithFn)(JSContext*, BaselineFrame*, HandleValue, Handle<StaticWithObject*>);
static const VMFunction EnterWithInfo = FunctionInfo<EnterWithFn>(jit::EnterWith);

bool
BaselineCompiler::emit_JSOP_ENTERWITH()
{
    StaticWithObject& withObj = script->getObject(pc)->as<StaticWithObject>();

    // Pop "with" object to R0.
    frame.popRegsAndSync(1);

    // Call a stub to push the object onto the scope chain.
    prepareVMCall();
    masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());

    pushArg(ImmGCPtr(&withObj));
    pushArg(R0);
    pushArg(R1.scratchReg());

    return callVM(EnterWithInfo);
}

typedef bool (*LeaveWithFn)(JSContext*, BaselineFrame*);
static const VMFunction LeaveWithInfo = FunctionInfo<LeaveWithFn>(jit::LeaveWith);

bool
BaselineCompiler::emit_JSOP_LEAVEWITH()
{
    // Call a stub to pop the with object from the scope chain.
    prepareVMCall();

    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    pushArg(R0.scratchReg());

    return callVM(LeaveWithInfo);
}

typedef bool (*GetAndClearExceptionFn)(JSContext*, MutableHandleValue);
static const VMFunction GetAndClearExceptionInfo =
    FunctionInfo<GetAndClearExceptionFn>(GetAndClearException);

bool
BaselineCompiler::emit_JSOP_EXCEPTION()
{
    prepareVMCall();

    if (!callVM(GetAndClearExceptionInfo))
        return false;

    frame.push(R0);
    return true;
}

typedef bool (*OnDebuggerStatementFn)(JSContext*, BaselineFrame*, jsbytecode* pc, bool*);
static const VMFunction OnDebuggerStatementInfo =
    FunctionInfo<OnDebuggerStatementFn>(jit::OnDebuggerStatement);

bool
BaselineCompiler::emit_JSOP_DEBUGGER()
{
    prepareVMCall();
    pushArg(ImmPtr(pc));

    frame.assertSyncedStack();
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    pushArg(R0.scratchReg());

    if (!callVM(OnDebuggerStatementInfo))
        return false;

    // If the stub returns |true|, return the frame's return value.
    Label done;
    masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, &done);
    {
        masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
        masm.jump(&return_);
    }
    masm.bind(&done);
    return true;
}

typedef bool (*DebugEpilogueFn)(JSContext*, BaselineFrame*, jsbytecode*);
static const VMFunction DebugEpilogueInfo =
    FunctionInfo<DebugEpilogueFn>(jit::DebugEpilogueOnBaselineReturn);

bool
BaselineCompiler::emitReturn()
{
    if (compileDebugInstrumentation_) {
        // Move return value into the frame's rval slot.
        masm.storeValue(JSReturnOperand, frame.addressOfReturnValue());
        masm.or32(Imm32(BaselineFrame::HAS_RVAL), frame.addressOfFlags());

        // Load BaselineFrame pointer in R0.
        frame.syncStack(0);
        masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

        prepareVMCall();
        pushArg(ImmPtr(pc));
        pushArg(R0.scratchReg());
        if (!callVM(DebugEpilogueInfo))
            return false;

        // Fix up the fake ICEntry appended by callVM for on-stack recompilation.
        icEntries_.back().setFakeKind(ICEntry::Kind_DebugEpilogue);

        masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
    }

    // Only emit the jump if this JSOP_RETRVAL is not the last instruction.
    // Not needed for last instruction, because last instruction flows
    // into return label.
    if (pc + GetBytecodeLength(pc) < script->codeEnd())
        masm.jump(&return_);

    return true;
}

bool
BaselineCompiler::emit_JSOP_RETURN()
{
    MOZ_ASSERT(frame.stackDepth() == 1);

    frame.popValue(JSReturnOperand);
    return emitReturn();
}

void
BaselineCompiler::emitLoadReturnValue(ValueOperand val)
{
    Label done, noRval;
    masm.branchTest32(Assembler::Zero, frame.addressOfFlags(),
                      Imm32(BaselineFrame::HAS_RVAL), &noRval);
    masm.loadValue(frame.addressOfReturnValue(), val);
    masm.jump(&done);

    masm.bind(&noRval);
    masm.moveValue(UndefinedValue(), val);

    masm.bind(&done);
}

bool
BaselineCompiler::emit_JSOP_RETRVAL()
{
    MOZ_ASSERT(frame.stackDepth() == 0);

    masm.moveValue(UndefinedValue(), JSReturnOperand);

    if (!script->noScriptRval()) {
        // Return the value in the return value slot, if any.
        Label done;
        Address flags = frame.addressOfFlags();
        masm.branchTest32(Assembler::Zero, flags, Imm32(BaselineFrame::HAS_RVAL), &done);
        masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
        masm.bind(&done);
    }

    return emitReturn();
}

typedef bool (*ToIdFn)(JSContext*, HandleScript, jsbytecode*, HandleValue, MutableHandleValue);
static const VMFunction ToIdInfo = FunctionInfo<ToIdFn>(js::ToIdOperation);

bool
BaselineCompiler::emit_JSOP_TOID()
{
    // Load index in R0, but keep values on the stack for the decompiler.
    frame.syncStack(0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

    // No-op if index is int32.
    Label done;
    masm.branchTestInt32(Assembler::Equal, R0, &done);

    prepareVMCall();

    pushArg(R0);
    pushArg(ImmPtr(pc));
    pushArg(ImmGCPtr(script));

    if (!callVM(ToIdInfo))
        return false;

    masm.bind(&done);
    frame.pop(); // Pop index.
    frame.push(R0);
    return true;
}

typedef bool (*ThrowObjectCoercibleFn)(JSContext*, HandleValue);
static const VMFunction ThrowObjectCoercibleInfo = FunctionInfo<ThrowObjectCoercibleFn>(ThrowObjectCoercible);

bool
BaselineCompiler::emit_JSOP_CHECKOBJCOERCIBLE()
{
    frame.syncStack(0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

    Label fail, done;

    masm.branchTestUndefined(Assembler::Equal, R0, &fail);
    masm.branchTestNull(Assembler::NotEqual, R0, &done);

    masm.bind(&fail);
    prepareVMCall();

    pushArg(R0);

    if (!callVM(ThrowObjectCoercibleInfo))
        return false;

    masm.bind(&done);
    return true;
}

typedef JSString* (*ToStringFn)(JSContext*, HandleValue);
static const VMFunction ToStringInfo = FunctionInfo<ToStringFn>(ToStringSlow);

bool
BaselineCompiler::emit_JSOP_TOSTRING()
{
    // Keep top stack value in R0.
    frame.popRegsAndSync(1);

    // Inline path for string.
    Label done;
    masm.branchTestString(Assembler::Equal, R0, &done);

    prepareVMCall();

    pushArg(R0);

    // Call ToStringSlow which doesn't handle string inputs.
    if (!callVM(ToStringInfo))
        return false;

    masm.tagValue(JSVAL_TYPE_STRING, ReturnReg, R0);

    masm.bind(&done);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_TABLESWITCH()
{
    frame.popRegsAndSync(1);

    // Call IC.
    ICTableSwitch::Compiler compiler(cx, pc);
    return emitOpIC(compiler.getStub(&stubSpace_));
}

bool
BaselineCompiler::emit_JSOP_ITER()
{
    frame.popRegsAndSync(1);

    ICIteratorNew_Fallback::Compiler compiler(cx);
    if (!emitOpIC(compiler.getStub(&stubSpace_)))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_MOREITER()
{
    frame.syncStack(0);
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), R0);

    ICIteratorMore_Fallback::Compiler compiler(cx);
    if (!emitOpIC(compiler.getStub(&stubSpace_)))
        return false;

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_ISNOITER()
{
    frame.syncStack(0);

    Label isMagic, done;
    masm.branchTestMagic(Assembler::Equal, frame.addressOfStackValue(frame.peek(-1)),
                         &isMagic);
    masm.moveValue(BooleanValue(false), R0);
    masm.jump(&done);

    masm.bind(&isMagic);
    masm.moveValue(BooleanValue(true), R0);

    masm.bind(&done);
    frame.push(R0, JSVAL_TYPE_BOOLEAN);
    return true;
}

bool
BaselineCompiler::emit_JSOP_ENDITER()
{
    frame.popRegsAndSync(1);

    ICIteratorClose_Fallback::Compiler compiler(cx);
    return emitOpIC(compiler.getStub(&stubSpace_));
}

bool
BaselineCompiler::emit_JSOP_GETRVAL()
{
    frame.syncStack(0);

    emitLoadReturnValue(R0);

    frame.push(R0);
    return true;
}

bool
BaselineCompiler::emit_JSOP_SETRVAL()
{
    // Store to the frame's return value slot.
    storeValue(frame.peek(-1), frame.addressOfReturnValue(), R2);
    masm.or32(Imm32(BaselineFrame::HAS_RVAL), frame.addressOfFlags());
    frame.pop();
    return true;
}

bool
BaselineCompiler::emit_JSOP_CALLEE()
{
    MOZ_ASSERT(function());
    frame.syncStack(0);
    masm.loadFunctionFromCalleeToken(frame.addressOfCalleeToken(), R0.scratchReg());
    masm.tagValue(JSVAL_TYPE_OBJECT, R0.scratchReg(), R0);
    frame.push(R0);
    return true;
}

typedef bool (*NewArgumentsObjectFn)(JSContext*, BaselineFrame*, MutableHandleValue);
static const VMFunction NewArgumentsObjectInfo =
    FunctionInfo<NewArgumentsObjectFn>(jit::NewArgumentsObject);

bool
BaselineCompiler::emit_JSOP_ARGUMENTS()
{
    frame.syncStack(0);

    Label done;
    if (!script->argumentsHasVarBinding() || !script->needsArgsObj()) {
        // We assume the script does not need an arguments object. However, this
        // assumption can be invalidated later, see argumentsOptimizationFailed
        // in JSScript. Because we can't invalidate baseline JIT code, we set a
        // flag on BaselineScript when that happens and guard on it here.
        masm.moveValue(MagicValue(JS_OPTIMIZED_ARGUMENTS), R0);

        // Load script->baseline.
        Register scratch = R1.scratchReg();
        masm.movePtr(ImmGCPtr(script), scratch);
        masm.loadPtr(Address(scratch, JSScript::offsetOfBaselineScript()), scratch);

        // If we don't need an arguments object, skip the VM call.
        masm.branchTest32(Assembler::Zero, Address(scratch, BaselineScript::offsetOfFlags()),
                          Imm32(BaselineScript::NEEDS_ARGS_OBJ), &done);
    }

    prepareVMCall();

    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    pushArg(R0.scratchReg());

    if (!callVM(NewArgumentsObjectInfo))
        return false;

    masm.bind(&done);
    frame.push(R0);
    return true;
}

typedef bool (*RunOnceScriptPrologueFn)(JSContext*, HandleScript);
static const VMFunction RunOnceScriptPrologueInfo =
    FunctionInfo<RunOnceScriptPrologueFn>(js::RunOnceScriptPrologue);

bool
BaselineCompiler::emit_JSOP_RUNONCE()
{
    frame.syncStack(0);

    prepareVMCall();

    masm.movePtr(ImmGCPtr(script), R0.scratchReg());
    pushArg(R0.scratchReg());

    return callVM(RunOnceScriptPrologueInfo);
}

bool
BaselineCompiler::emit_JSOP_REST()
{
    frame.syncStack(0);

    JSObject* templateObject =
        ObjectGroup::newArrayObject(cx, nullptr, 0, TenuredObject,
                                    ObjectGroup::NewArrayKind::UnknownIndex);
    if (!templateObject)
        return false;

    // Call IC.
    ICRest_Fallback::Compiler compiler(cx, &templateObject->as<ArrayObject>());
    if (!emitOpIC(compiler.getStub(&stubSpace_)))
        return false;

    // Mark R0 as pushed stack value.
    frame.push(R0);
    return true;
}

typedef JSObject* (*CreateGeneratorFn)(JSContext*, BaselineFrame*);
static const VMFunction CreateGeneratorInfo = FunctionInfo<CreateGeneratorFn>(jit::CreateGenerator);

bool
BaselineCompiler::emit_JSOP_GENERATOR()
{
    MOZ_ASSERT(frame.stackDepth() == 0);

    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());

    prepareVMCall();
    pushArg(R0.scratchReg());
    if (!callVM(CreateGeneratorInfo))
        return false;

    masm.tagValue(JSVAL_TYPE_OBJECT, ReturnReg, R0);
    frame.push(R0);
    return true;
}

bool
BaselineCompiler::addYieldOffset()
{
    MOZ_ASSERT(*pc == JSOP_INITIALYIELD || *pc == JSOP_YIELD);

    uint32_t yieldIndex = GET_UINT24(pc);

    while (yieldIndex >= yieldOffsets_.length()) {
        if (!yieldOffsets_.append(0))
            return false;
    }

    static_assert(JSOP_INITIALYIELD_LENGTH == JSOP_YIELD_LENGTH,
                  "code below assumes INITIALYIELD and YIELD have same length");
    yieldOffsets_[yieldIndex] = script->pcToOffset(pc + JSOP_YIELD_LENGTH);
    return true;
}

bool
BaselineCompiler::emit_JSOP_INITIALYIELD()
{
    if (!addYieldOffset())
        return false;

    frame.syncStack(0);
    MOZ_ASSERT(frame.stackDepth() == 1);

    Register genObj = R2.scratchReg();
    masm.unboxObject(frame.addressOfStackValue(frame.peek(-1)), genObj);

    MOZ_ASSERT(GET_UINT24(pc) == 0);
    masm.storeValue(Int32Value(0), Address(genObj, GeneratorObject::offsetOfYieldIndexSlot()));

    Register scopeObj = R0.scratchReg();
    Address scopeChainSlot(genObj, GeneratorObject::offsetOfScopeChainSlot());
    masm.loadPtr(frame.addressOfScopeChain(), scopeObj);
    masm.patchableCallPreBarrier(scopeChainSlot, MIRType_Value);
    masm.storeValue(JSVAL_TYPE_OBJECT, scopeObj, scopeChainSlot);

    Register temp = R1.scratchReg();
    Label skipBarrier;
    masm.branchPtrInNurseryRange(Assembler::Equal, genObj, temp, &skipBarrier);
    masm.branchPtrInNurseryRange(Assembler::NotEqual, scopeObj, temp, &skipBarrier);
    masm.push(genObj);
    MOZ_ASSERT(genObj == R2.scratchReg());
    masm.call(&postBarrierSlot_);
    masm.pop(genObj);
    masm.bind(&skipBarrier);

    masm.tagValue(JSVAL_TYPE_OBJECT, genObj, JSReturnOperand);
    return emitReturn();
}

typedef bool (*NormalSuspendFn)(JSContext*, HandleObject, BaselineFrame*, jsbytecode*, uint32_t);
static const VMFunction NormalSuspendInfo = FunctionInfo<NormalSuspendFn>(jit::NormalSuspend);

bool
BaselineCompiler::emit_JSOP_YIELD()
{
    if (!addYieldOffset())
        return false;

    // Store generator in R0.
    frame.popRegsAndSync(1);

    Register genObj = R2.scratchReg();
    masm.unboxObject(R0, genObj);

    MOZ_ASSERT(frame.stackDepth() >= 1);

    if (frame.stackDepth() == 1 && !script->isLegacyGenerator()) {
        // If the expression stack is empty, we can inline the YIELD. Don't do
        // this for legacy generators: we have to throw an exception if the
        // generator is in the closing state, see GeneratorObject::suspend.

        masm.storeValue(Int32Value(GET_UINT24(pc)),
                        Address(genObj, GeneratorObject::offsetOfYieldIndexSlot()));

        Register scopeObj = R0.scratchReg();
        Address scopeChainSlot(genObj, GeneratorObject::offsetOfScopeChainSlot());
        masm.loadPtr(frame.addressOfScopeChain(), scopeObj);
        masm.patchableCallPreBarrier(scopeChainSlot, MIRType_Value);
        masm.storeValue(JSVAL_TYPE_OBJECT, scopeObj, scopeChainSlot);

        Register temp = R1.scratchReg();
        Label skipBarrier;
        masm.branchPtrInNurseryRange(Assembler::Equal, genObj, temp, &skipBarrier);
        masm.branchPtrInNurseryRange(Assembler::NotEqual, scopeObj, temp, &skipBarrier);
        MOZ_ASSERT(genObj == R2.scratchReg());
        masm.call(&postBarrierSlot_);
        masm.bind(&skipBarrier);
    } else {
        masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());

        prepareVMCall();
        pushArg(Imm32(frame.stackDepth()));
        pushArg(ImmPtr(pc));
        pushArg(R1.scratchReg());
        pushArg(genObj);

        if (!callVM(NormalSuspendInfo))
            return false;
    }

    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), JSReturnOperand);
    return emitReturn();
}

typedef bool (*DebugAfterYieldFn)(JSContext*, BaselineFrame*);
static const VMFunction DebugAfterYieldInfo = FunctionInfo<DebugAfterYieldFn>(jit::DebugAfterYield);

bool
BaselineCompiler::emit_JSOP_DEBUGAFTERYIELD()
{
    if (!compileDebugInstrumentation_)
        return true;

    frame.assertSyncedStack();
    masm.loadBaselineFramePtr(BaselineFrameReg, R0.scratchReg());
    prepareVMCall();
    pushArg(R0.scratchReg());
    return callVM(DebugAfterYieldInfo);
}

typedef bool (*FinalSuspendFn)(JSContext*, HandleObject, BaselineFrame*, jsbytecode*);
static const VMFunction FinalSuspendInfo = FunctionInfo<FinalSuspendFn>(jit::FinalSuspend);

bool
BaselineCompiler::emit_JSOP_FINALYIELDRVAL()
{
    // Store generator in R0, BaselineFrame pointer in R1.
    frame.popRegsAndSync(1);
    masm.unboxObject(R0, R0.scratchReg());
    masm.loadBaselineFramePtr(BaselineFrameReg, R1.scratchReg());

    prepareVMCall();
    pushArg(ImmPtr(pc));
    pushArg(R1.scratchReg());
    pushArg(R0.scratchReg());

    if (!callVM(FinalSuspendInfo))
        return false;

    masm.loadValue(frame.addressOfReturnValue(), JSReturnOperand);
    return emitReturn();
}

typedef bool (*InterpretResumeFn)(JSContext*, HandleObject, HandleValue, HandlePropertyName,
                                  MutableHandleValue);
static const VMFunction InterpretResumeInfo = FunctionInfo<InterpretResumeFn>(jit::InterpretResume);

typedef bool (*GeneratorThrowFn)(JSContext*, BaselineFrame*, Handle<GeneratorObject*>,
                                 HandleValue, uint32_t);
static const VMFunction GeneratorThrowInfo = FunctionInfo<GeneratorThrowFn>(jit::GeneratorThrowOrClose, TailCall);

bool
BaselineCompiler::emit_JSOP_RESUME()
{
    GeneratorObject::ResumeKind resumeKind = GeneratorObject::getResumeKind(pc);

    frame.syncStack(0);
    masm.checkStackAlignment();

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(BaselineFrameReg);

    // Load generator object.
    Register genObj = regs.takeAny();
    masm.unboxObject(frame.addressOfStackValue(frame.peek(-2)), genObj);

    // Load callee.
    Register callee = regs.takeAny();
    masm.unboxObject(Address(genObj, GeneratorObject::offsetOfCalleeSlot()), callee);

    // Load the script. Note that we don't relazify generator scripts, so it's
    // guaranteed to be non-lazy.
    Register scratch1 = regs.takeAny();
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), scratch1);

    // Load the BaselineScript or call a stub if we don't have one.
    Label interpret;
    masm.loadPtr(Address(scratch1, JSScript::offsetOfBaselineScript()), scratch1);
    masm.branchPtr(Assembler::BelowOrEqual, scratch1, ImmPtr(BASELINE_DISABLED_SCRIPT), &interpret);

    Register constructing = regs.takeAny();
    ValueOperand newTarget = regs.takeAnyValue();
    masm.loadValue(Address(genObj, GeneratorObject::offsetOfNewTargetSlot()), newTarget);
    masm.move32(Imm32(0), constructing);
    {
        Label notConstructing;
        masm.branchTestObject(Assembler::NotEqual, newTarget, &notConstructing);
        masm.pushValue(newTarget);
        masm.move32(Imm32(CalleeToken_FunctionConstructing), constructing);
        masm.bind(&notConstructing);
    }
    regs.add(newTarget);

    // Push |undefined| for all formals.
    Register scratch2 = regs.takeAny();
    Label loop, loopDone;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), scratch2);
    masm.bind(&loop);
    masm.branchTest32(Assembler::Zero, scratch2, scratch2, &loopDone);
    {
        masm.pushValue(UndefinedValue());
        masm.sub32(Imm32(1), scratch2);
        masm.jump(&loop);
    }
    masm.bind(&loopDone);

    // Push |undefined| for |this|.
    masm.pushValue(UndefinedValue());

    // Update BaselineFrame frameSize field and create the frame descriptor.
    masm.computeEffectiveAddress(Address(BaselineFrameReg, BaselineFrame::FramePointerOffset),
                                 scratch2);
    masm.subStackPtrFrom(scratch2);
    masm.store32(scratch2, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));
    masm.makeFrameDescriptor(scratch2, JitFrame_BaselineJS);

    masm.Push(Imm32(0)); // actual argc

    // Duplicate PushCalleeToken with a variable instead.
    masm.orPtr(constructing, callee);
    masm.Push(callee);
    masm.Push(scratch2); // frame descriptor

    regs.add(callee);
    regs.add(constructing);

    // Load the return value.
    ValueOperand retVal = regs.takeAnyValue();
    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), retVal);

    // Push a fake return address on the stack. We will resume here when the
    // generator returns.
    Label genStart, returnTarget;
#ifdef JS_USE_LINK_REGISTER
    masm.call(&genStart);
#else
    masm.callAndPushReturnAddress(&genStart);
#endif

    // Add an IC entry so the return offset -> pc mapping works.
    if (!appendICEntry(ICEntry::Kind_Op, masm.currentOffset()))
        return false;

    masm.jump(&returnTarget);
    masm.bind(&genStart);
#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif

    // If profiler instrumentation is on, update lastProfilingFrame on
    // current JitActivation
    {
        Register scratchReg = scratch2;
        Label skip;
        AbsoluteAddress addressOfEnabled(cx->runtime()->spsProfiler.addressOfEnabled());
        masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skip);
        masm.loadPtr(AbsoluteAddress(cx->runtime()->addressOfProfilingActivation()), scratchReg);
        masm.storePtr(masm.getStackPointer(),
                      Address(scratchReg, JitActivation::offsetOfLastProfilingFrame()));
        masm.bind(&skip);
    }

    // Construct BaselineFrame.
    masm.push(BaselineFrameReg);
    masm.moveStackPtrTo(BaselineFrameReg);
    masm.subFromStackPtr(Imm32(BaselineFrame::Size()));
    masm.checkStackAlignment();

    // Store flags and scope chain.
    masm.store32(Imm32(BaselineFrame::HAS_CALL_OBJ), frame.addressOfFlags());
    masm.unboxObject(Address(genObj, GeneratorObject::offsetOfScopeChainSlot()), scratch2);
    masm.storePtr(scratch2, frame.addressOfScopeChain());

    // Store the arguments object if there is one.
    Label noArgsObj;
    masm.unboxObject(Address(genObj, GeneratorObject::offsetOfArgsObjSlot()), scratch2);
    masm.branchTestPtr(Assembler::Zero, scratch2, scratch2, &noArgsObj);
    {
        masm.storePtr(scratch2, frame.addressOfArgsObj());
        masm.or32(Imm32(BaselineFrame::HAS_ARGS_OBJ), frame.addressOfFlags());
    }
    masm.bind(&noArgsObj);

    // Push expression slots if needed.
    Label noExprStack;
    Address exprStackSlot(genObj, GeneratorObject::offsetOfExpressionStackSlot());
    masm.branchTestNull(Assembler::Equal, exprStackSlot, &noExprStack);
    {
        masm.unboxObject(exprStackSlot, scratch2);

        Register initLength = regs.takeAny();
        masm.loadPtr(Address(scratch2, NativeObject::offsetOfElements()), scratch2);
        masm.load32(Address(scratch2, ObjectElements::offsetOfInitializedLength()), initLength);

        Label loop, loopDone;
        masm.bind(&loop);
        masm.branchTest32(Assembler::Zero, initLength, initLength, &loopDone);
        {
            masm.pushValue(Address(scratch2, 0));
            masm.addPtr(Imm32(sizeof(Value)), scratch2);
            masm.sub32(Imm32(1), initLength);
            masm.jump(&loop);
        }
        masm.bind(&loopDone);

        masm.patchableCallPreBarrier(exprStackSlot, MIRType_Value);
        masm.storeValue(NullValue(), exprStackSlot);
        regs.add(initLength);
    }

    masm.bind(&noExprStack);
    masm.pushValue(retVal);

    if (resumeKind == GeneratorObject::NEXT) {
        // Determine the resume address based on the yieldIndex and the
        // yieldIndex -> native table in the BaselineScript.
        masm.load32(Address(scratch1, BaselineScript::offsetOfYieldEntriesOffset()), scratch2);
        masm.addPtr(scratch2, scratch1);
        masm.unboxInt32(Address(genObj, GeneratorObject::offsetOfYieldIndexSlot()), scratch2);
        masm.loadPtr(BaseIndex(scratch1, scratch2, ScaleFromElemWidth(sizeof(uintptr_t))), scratch1);

        // Mark as running and jump to the generator's JIT code.
        masm.storeValue(Int32Value(GeneratorObject::YIELD_INDEX_RUNNING),
                        Address(genObj, GeneratorObject::offsetOfYieldIndexSlot()));
        masm.jump(scratch1);
    } else {
        MOZ_ASSERT(resumeKind == GeneratorObject::THROW || resumeKind == GeneratorObject::CLOSE);

        // Update the frame's frameSize field.
        masm.computeEffectiveAddress(Address(BaselineFrameReg, BaselineFrame::FramePointerOffset),
                                     scratch2);
        masm.movePtr(scratch2, scratch1);
        masm.subStackPtrFrom(scratch2);
        masm.store32(scratch2, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));
        masm.loadBaselineFramePtr(BaselineFrameReg, scratch2);

        prepareVMCall();
        pushArg(Imm32(resumeKind));
        pushArg(retVal);
        pushArg(genObj);
        pushArg(scratch2);

        JitCode* code = cx->runtime()->jitRuntime()->getVMWrapper(GeneratorThrowInfo);
        if (!code)
            return false;

        // Create the frame descriptor.
        masm.subStackPtrFrom(scratch1);
        masm.makeFrameDescriptor(scratch1, JitFrame_BaselineJS);

        // Push the frame descriptor and a dummy return address (it doesn't
        // matter what we push here, frame iterators will use the frame pc
        // set in jit::GeneratorThrowOrClose).
        masm.push(scratch1);

        // On ARM64, the callee will push the return address.
#ifndef JS_CODEGEN_ARM64
        masm.push(ImmWord(0));
#endif
        masm.jump(code);
    }

    // If the generator script has no JIT code, call into the VM.
    masm.bind(&interpret);

    prepareVMCall();
    if (resumeKind == GeneratorObject::NEXT) {
        pushArg(ImmGCPtr(cx->names().next));
    } else if (resumeKind == GeneratorObject::THROW) {
        pushArg(ImmGCPtr(cx->names().throw_));
    } else {
        MOZ_ASSERT(resumeKind == GeneratorObject::CLOSE);
        pushArg(ImmGCPtr(cx->names().close));
    }

    masm.loadValue(frame.addressOfStackValue(frame.peek(-1)), retVal);
    pushArg(retVal);
    pushArg(genObj);

    if (!callVM(InterpretResumeInfo))
        return false;

    // After the generator returns, we restore the stack pointer, push the
    // return value and we're done.
    masm.bind(&returnTarget);
    masm.computeEffectiveAddress(frame.addressOfStackValue(frame.peek(-1)), masm.getStackPointer());
    frame.popn(2);
    frame.push(R0);
    return true;
}

