/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineJIT.h"

#include "mozilla/MemoryReporting.h"

#include "asmjs/AsmJSModule.h"
#include "jit/BaselineCompiler.h"
#include "jit/BaselineIC.h"
#include "jit/CompileInfo.h"
#include "jit/JitCommon.h"
#include "jit/JitSpewer.h"
#include "vm/Debugger.h"
#include "vm/Interpreter.h"
#include "vm/TraceLogging.h"

#include "jsobjinlines.h"
#include "jsopcodeinlines.h"
#include "jsscriptinlines.h"

#include "jit/JitFrames-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::jit;

/* static */ PCMappingSlotInfo::SlotLocation
PCMappingSlotInfo::ToSlotLocation(const StackValue* stackVal)
{
    if (stackVal->kind() == StackValue::Register) {
        if (stackVal->reg() == R0)
            return SlotInR0;
        MOZ_ASSERT(stackVal->reg() == R1);
        return SlotInR1;
    }
    MOZ_ASSERT(stackVal->kind() != StackValue::Stack);
    return SlotIgnore;
}

BaselineScript::BaselineScript(uint32_t prologueOffset, uint32_t epilogueOffset,
                               uint32_t profilerEnterToggleOffset,
                               uint32_t profilerExitToggleOffset,
                               uint32_t traceLoggerEnterToggleOffset,
                               uint32_t traceLoggerExitToggleOffset,
                               uint32_t postDebugPrologueOffset)
  : method_(nullptr),
    templateScope_(nullptr),
    fallbackStubSpace_(),
    dependentAsmJSModules_(nullptr),
    prologueOffset_(prologueOffset),
    epilogueOffset_(epilogueOffset),
    profilerEnterToggleOffset_(profilerEnterToggleOffset),
    profilerExitToggleOffset_(profilerExitToggleOffset),
#ifdef JS_TRACE_LOGGING
# ifdef DEBUG
    traceLoggerScriptsEnabled_(false),
    traceLoggerEngineEnabled_(false),
# endif
    traceLoggerEnterToggleOffset_(traceLoggerEnterToggleOffset),
    traceLoggerExitToggleOffset_(traceLoggerExitToggleOffset),
    traceLoggerScriptEvent_(),
#endif
    postDebugPrologueOffset_(postDebugPrologueOffset),
    flags_(0),
    inlinedBytecodeLength_(0),
    maxInliningDepth_(UINT8_MAX),
    pendingBuilder_(nullptr)
{ }

static const unsigned BASELINE_MAX_ARGS_LENGTH = 20000;

static bool
CheckFrame(InterpreterFrame* fp)
{
    if (fp->isDebuggerEvalFrame()) {
        // Debugger eval-in-frame. These are likely short-running scripts so
        // don't bother compiling them for now.
        JitSpew(JitSpew_BaselineAbort, "debugger frame");
        return false;
    }

    if (fp->isNonEvalFunctionFrame() && fp->numActualArgs() > BASELINE_MAX_ARGS_LENGTH) {
        // Fall back to the interpreter to avoid running out of stack space.
        JitSpew(JitSpew_BaselineAbort, "Too many arguments (%u)", fp->numActualArgs());
        return false;
    }

    return true;
}

static JitExecStatus
EnterBaseline(JSContext* cx, EnterJitData& data)
{
    if (data.osrFrame) {
        // Check for potential stack overflow before OSR-ing.
        uint8_t spDummy;
        uint32_t extra = BaselineFrame::Size() + (data.osrNumStackValues * sizeof(Value));
        uint8_t* checkSp = (&spDummy) - extra;
        JS_CHECK_RECURSION_WITH_SP(cx, checkSp, return JitExec_Aborted);
    } else {
        JS_CHECK_RECURSION(cx, return JitExec_Aborted);
    }

#ifdef DEBUG
    // Assert we don't GC before entering JIT code. A GC could discard JIT code
    // or move the function stored in the CalleeToken (it won't be traced at
    // this point). We use Maybe<> here so we can call reset() to call the
    // AutoAssertOnGC destructor before we enter JIT code.
    mozilla::Maybe<JS::AutoAssertOnGC> nogc;
    nogc.emplace(cx->runtime());
#endif

    MOZ_ASSERT(jit::IsBaselineEnabled(cx));
    MOZ_ASSERT_IF(data.osrFrame, CheckFrame(data.osrFrame));

    EnterJitCode enter = cx->runtime()->jitRuntime()->enterBaseline();

    bool constructingLegacyGen =
        data.constructing && CalleeTokenToFunction(data.calleeToken)->isLegacyGenerator();

    // Caller must construct |this| before invoking the Ion function. Legacy
    // generators can be called with 'new' but when we resume them, the
    // this-slot and arguments are |undefined| (they are stored in the
    // CallObject).
    MOZ_ASSERT_IF(data.constructing && !constructingLegacyGen,
                  data.maxArgv[0].isObject() || data.maxArgv[0].isMagic(JS_UNINITIALIZED_LEXICAL));

    data.result.setInt32(data.numActualArgs);
    {
        AssertCompartmentUnchanged pcc(cx);
        ActivationEntryMonitor entryMonitor(cx, data.calleeToken);
        JitActivation activation(cx);

        if (data.osrFrame)
            data.osrFrame->setRunningInJit();

#ifdef DEBUG
        nogc.reset();
#endif
        // Single transition point from Interpreter to Baseline.
        CALL_GENERATED_CODE(enter, data.jitcode, data.maxArgc, data.maxArgv, data.osrFrame, data.calleeToken,
                            data.scopeChain.get(), data.osrNumStackValues, data.result.address());

        if (data.osrFrame)
            data.osrFrame->clearRunningInJit();
    }

    MOZ_ASSERT(!cx->runtime()->jitRuntime()->hasIonReturnOverride());

    // Jit callers wrap primitive constructor return, except for derived
    // class constructors, which are forced to do it themselves.
    if (!data.result.isMagic() &&
        data.constructing &&
        data.result.isPrimitive() &&
        !constructingLegacyGen)
    {
        MOZ_ASSERT(data.maxArgv[0].isObject());
        data.result = data.maxArgv[0];
    }

    // Release temporary buffer used for OSR into Ion.
    cx->runtime()->getJitRuntime(cx)->freeOsrTempData();

    MOZ_ASSERT_IF(data.result.isMagic(), data.result.isMagic(JS_ION_ERROR));
    return data.result.isMagic() ? JitExec_Error : JitExec_Ok;
}

JitExecStatus
jit::EnterBaselineMethod(JSContext* cx, RunState& state)
{
    BaselineScript* baseline = state.script()->baselineScript();

    EnterJitData data(cx);
    data.jitcode = baseline->method()->raw();

    AutoValueVector vals(cx);
    if (!SetEnterJitData(cx, data, state, vals))
        return JitExec_Error;

    JitExecStatus status = EnterBaseline(cx, data);
    if (status != JitExec_Ok)
        return status;

    state.setReturnValue(data.result);
    return JitExec_Ok;
}

JitExecStatus
jit::EnterBaselineAtBranch(JSContext* cx, InterpreterFrame* fp, jsbytecode* pc)
{
    MOZ_ASSERT(JSOp(*pc) == JSOP_LOOPENTRY);

    BaselineScript* baseline = fp->script()->baselineScript();

    EnterJitData data(cx);
    data.jitcode = baseline->nativeCodeForPC(fp->script(), pc);

    // Skip debug breakpoint/trap handler, the interpreter already handled it
    // for the current op.
    if (fp->isDebuggee()) {
        MOZ_RELEASE_ASSERT(baseline->hasDebugInstrumentation());
        data.jitcode += MacroAssembler::ToggledCallSize(data.jitcode);
    }

    data.osrFrame = fp;
    data.osrNumStackValues = fp->script()->nfixed() + cx->interpreterRegs().stackDepth();

    AutoValueVector vals(cx);
    RootedValue thisv(cx);

    if (fp->isNonEvalFunctionFrame()) {
        data.constructing = fp->isConstructing();
        data.numActualArgs = fp->numActualArgs();
        data.maxArgc = Max(fp->numActualArgs(), fp->numFormalArgs()) + 1; // +1 = include |this|
        data.maxArgv = fp->argv() - 1; // -1 = include |this|
        data.scopeChain = nullptr;
        data.calleeToken = CalleeToToken(&fp->callee(), data.constructing);
    } else {
        thisv.setUndefined();
        data.constructing = false;
        data.numActualArgs = 0;
        data.maxArgc = 1;
        data.maxArgv = thisv.address();
        data.scopeChain = fp->scopeChain();

        // For eval function frames, set the callee token to the enclosing function.
        if (fp->isFunctionFrame())
            data.calleeToken = CalleeToToken(&fp->callee(), /* constructing = */ false);
        else
            data.calleeToken = CalleeToToken(fp->script());

        if (fp->isEvalFrame()) {
            if (!vals.reserve(2))
                return JitExec_Aborted;

            vals.infallibleAppend(thisv);

            if (fp->isFunctionFrame())
                vals.infallibleAppend(fp->newTarget());
            else
                vals.infallibleAppend(NullValue());

            data.maxArgc = 2;
            data.maxArgv = vals.begin();
        }
    }

    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    TraceLogStopEvent(logger, TraceLogger_Interpreter);
    TraceLogStartEvent(logger, TraceLogger_Baseline);

    JitExecStatus status = EnterBaseline(cx, data);
    if (status != JitExec_Ok)
        return status;

    fp->setReturnValue(data.result);
    return JitExec_Ok;
}

MethodStatus
jit::BaselineCompile(JSContext* cx, JSScript* script, bool forceDebugInstrumentation)
{
    MOZ_ASSERT(!script->hasBaselineScript());
    MOZ_ASSERT(script->canBaselineCompile());
    MOZ_ASSERT(IsBaselineEnabled(cx));

    script->ensureNonLazyCanonicalFunction(cx);

    LifoAlloc alloc(TempAllocator::PreferredLifoChunkSize);
    TempAllocator* temp = alloc.new_<TempAllocator>(&alloc);
    if (!temp) {
        ReportOutOfMemory(cx);
        return Method_Error;
    }

    JitContext jctx(cx, temp);

    BaselineCompiler compiler(cx, *temp, script);
    if (!compiler.init()) {
        ReportOutOfMemory(cx);
        return Method_Error;
    }

    if (forceDebugInstrumentation)
        compiler.setCompileDebugInstrumentation();

    MethodStatus status = compiler.compile();

    MOZ_ASSERT_IF(status == Method_Compiled, script->hasBaselineScript());
    MOZ_ASSERT_IF(status != Method_Compiled, !script->hasBaselineScript());

    if (status == Method_CantCompile)
        script->setBaselineScript(cx, BASELINE_DISABLED_SCRIPT);

    return status;
}

static MethodStatus
CanEnterBaselineJIT(JSContext* cx, HandleScript script, InterpreterFrame* osrFrame)
{
    MOZ_ASSERT(jit::IsBaselineEnabled(cx));

    // Skip if the script has been disabled.
    if (!script->canBaselineCompile())
        return Method_Skipped;

    if (script->length() > BaselineScript::MAX_JSSCRIPT_LENGTH)
        return Method_CantCompile;

    if (script->nslots() > BaselineScript::MAX_JSSCRIPT_SLOTS)
        return Method_CantCompile;

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return Method_Error;

    if (script->hasBaselineScript())
        return Method_Compiled;

    // Check script warm-up counter.
    if (script->incWarmUpCounter() <= JitOptions.baselineWarmUpThreshold)
        return Method_Skipped;

    // Frames can be marked as debuggee frames independently of its underlying
    // script being a debuggee script, e.g., when performing
    // Debugger.Frame.prototype.eval.
    return BaselineCompile(cx, script, osrFrame && osrFrame->isDebuggee());
}

MethodStatus
jit::CanEnterBaselineAtBranch(JSContext* cx, InterpreterFrame* fp, bool newType)
{
   if (!CheckFrame(fp))
       return Method_CantCompile;

   // This check is needed in the following corner case. Consider a function h,
   //
   //   function h(x) {
   //      h(false);
   //      if (!x)
   //        return;
   //      for (var i = 0; i < N; i++)
   //         /* do stuff */
   //   }
   //
   // Suppose h is not yet compiled in baseline and is executing in the
   // interpreter. Let this interpreter frame be f_older. The debugger marks
   // f_older as isDebuggee. At the point of the recursive call h(false), h is
   // compiled in baseline without debug instrumentation, pushing a baseline
   // frame f_newer. The debugger never flags f_newer as isDebuggee, and never
   // recompiles h. When the recursive call returns and execution proceeds to
   // the loop, the interpreter attempts to OSR into baseline. Since h is
   // already compiled in baseline, execution jumps directly into baseline
   // code. This is incorrect as h's baseline script does not have debug
   // instrumentation.
   if (fp->isDebuggee() && !Debugger::ensureExecutionObservabilityOfOsrFrame(cx, fp))
       return Method_Error;

   RootedScript script(cx, fp->script());
   return CanEnterBaselineJIT(cx, script, fp);
}

MethodStatus
jit::CanEnterBaselineMethod(JSContext* cx, RunState& state)
{
    if (state.isInvoke()) {
        InvokeState& invoke = *state.asInvoke();

        if (invoke.args().length() > BASELINE_MAX_ARGS_LENGTH) {
            JitSpew(JitSpew_BaselineAbort, "Too many arguments (%u)", invoke.args().length());
            return Method_CantCompile;
        }

        if (!state.maybeCreateThisForConstructor(cx)) {
            if (cx->isThrowingOutOfMemory()) {
                cx->recoverFromOutOfMemory();
                return Method_Skipped;
            }
            return Method_Error;
        }
    } else {
        MOZ_ASSERT(state.isExecute());
        ExecuteType type = state.asExecute()->type();
        if (type == EXECUTE_DEBUG) {
            JitSpew(JitSpew_BaselineAbort, "debugger frame");
            return Method_CantCompile;
        }
    }

    RootedScript script(cx, state.script());
    return CanEnterBaselineJIT(cx, script, /* osrFrame = */ nullptr);
};

BaselineScript*
BaselineScript::New(JSScript* jsscript, uint32_t prologueOffset, uint32_t epilogueOffset,
                    uint32_t profilerEnterToggleOffset, uint32_t profilerExitToggleOffset,
                    uint32_t traceLoggerEnterToggleOffset, uint32_t traceLoggerExitToggleOffset,
                    uint32_t postDebugPrologueOffset,
                    size_t icEntries, size_t pcMappingIndexEntries, size_t pcMappingSize,
                    size_t bytecodeTypeMapEntries, size_t yieldEntries)
{
    static const unsigned DataAlignment = sizeof(uintptr_t);

    size_t icEntriesSize = icEntries * sizeof(ICEntry);
    size_t pcMappingIndexEntriesSize = pcMappingIndexEntries * sizeof(PCMappingIndexEntry);
    size_t bytecodeTypeMapSize = bytecodeTypeMapEntries * sizeof(uint32_t);
    size_t yieldEntriesSize = yieldEntries * sizeof(uintptr_t);

    size_t paddedICEntriesSize = AlignBytes(icEntriesSize, DataAlignment);
    size_t paddedPCMappingIndexEntriesSize = AlignBytes(pcMappingIndexEntriesSize, DataAlignment);
    size_t paddedPCMappingSize = AlignBytes(pcMappingSize, DataAlignment);
    size_t paddedBytecodeTypesMapSize = AlignBytes(bytecodeTypeMapSize, DataAlignment);
    size_t paddedYieldEntriesSize = AlignBytes(yieldEntriesSize, DataAlignment);

    size_t allocBytes = paddedICEntriesSize +
                        paddedPCMappingIndexEntriesSize +
                        paddedPCMappingSize +
                        paddedBytecodeTypesMapSize +
                        paddedYieldEntriesSize;

    BaselineScript* script = jsscript->zone()->pod_malloc_with_extra<BaselineScript, uint8_t>(allocBytes);
    if (!script)
        return nullptr;
    new (script) BaselineScript(prologueOffset, epilogueOffset,
                                profilerEnterToggleOffset, profilerExitToggleOffset,
                                traceLoggerEnterToggleOffset, traceLoggerExitToggleOffset,
                                postDebugPrologueOffset);

    size_t offsetCursor = sizeof(BaselineScript);
    MOZ_ASSERT(offsetCursor == AlignBytes(sizeof(BaselineScript), DataAlignment));

    script->icEntriesOffset_ = offsetCursor;
    script->icEntries_ = icEntries;
    offsetCursor += paddedICEntriesSize;

    script->pcMappingIndexOffset_ = offsetCursor;
    script->pcMappingIndexEntries_ = pcMappingIndexEntries;
    offsetCursor += paddedPCMappingIndexEntriesSize;

    script->pcMappingOffset_ = offsetCursor;
    script->pcMappingSize_ = pcMappingSize;
    offsetCursor += paddedPCMappingSize;

    script->bytecodeTypeMapOffset_ = bytecodeTypeMapEntries ? offsetCursor : 0;
    offsetCursor += paddedBytecodeTypesMapSize;

    script->yieldEntriesOffset_ = yieldEntries ? offsetCursor : 0;
    offsetCursor += paddedYieldEntriesSize;

    MOZ_ASSERT(offsetCursor == sizeof(BaselineScript) + allocBytes);
    return script;
}

void
BaselineScript::trace(JSTracer* trc)
{
    TraceEdge(trc, &method_, "baseline-method");
    if (templateScope_)
        TraceEdge(trc, &templateScope_, "baseline-template-scope");

    // Mark all IC stub codes hanging off the IC stub entries.
    for (size_t i = 0; i < numICEntries(); i++) {
        ICEntry& ent = icEntry(i);
        ent.trace(trc);
    }
}

/* static */
void
BaselineScript::writeBarrierPre(Zone* zone, BaselineScript* script)
{
    if (zone->needsIncrementalBarrier())
        script->trace(zone->barrierTracer());
}

void
BaselineScript::Trace(JSTracer* trc, BaselineScript* script)
{
    script->trace(trc);
}

void
BaselineScript::Destroy(FreeOp* fop, BaselineScript* script)
{
    /*
     * When the script contains pointers to nursery things, the store buffer
     * will contain entries refering to the referenced things. Since we can
     * destroy scripts outside the context of a GC, this situation can result
     * in invalid store buffer entries. Assert that if we do destroy scripts
     * outside of a GC that we at least emptied the nursery first.
     */
    MOZ_ASSERT(fop->runtime()->gc.nursery.isEmpty());

    MOZ_ASSERT(!script->hasPendingIonBuilder());

    script->unlinkDependentAsmJSModules(fop);

    fop->delete_(script);
}

void
BaselineScript::clearDependentAsmJSModules()
{
    // Remove any links from AsmJSModules that contain optimized FFI calls into
    // this BaselineScript.
    if (dependentAsmJSModules_) {
        for (size_t i = 0; i < dependentAsmJSModules_->length(); i++) {
            DependentAsmJSModuleExit exit = (*dependentAsmJSModules_)[i];
            exit.module->exit(exit.exitIndex).deoptimize(*exit.module);
        }

        dependentAsmJSModules_->clear();
    }
}

void
BaselineScript::unlinkDependentAsmJSModules(FreeOp* fop)
{
    // Remove any links from AsmJSModules that contain optimized FFI calls into
    // this BaselineScript.
    clearDependentAsmJSModules();
    if (dependentAsmJSModules_) {
        fop->delete_(dependentAsmJSModules_);
        dependentAsmJSModules_ = nullptr;
    }
}

bool
BaselineScript::addDependentAsmJSModule(JSContext* cx, DependentAsmJSModuleExit exit)
{
    if (!dependentAsmJSModules_) {
        dependentAsmJSModules_ = cx->new_<Vector<DependentAsmJSModuleExit> >(cx);
        if (!dependentAsmJSModules_)
            return false;
    }
    return dependentAsmJSModules_->append(exit);
}

void
BaselineScript::removeDependentAsmJSModule(DependentAsmJSModuleExit exit)
{
    if (!dependentAsmJSModules_)
        return;

    for (size_t i = 0; i < dependentAsmJSModules_->length(); i++) {
        if ((*dependentAsmJSModules_)[i].module == exit.module &&
            (*dependentAsmJSModules_)[i].exitIndex == exit.exitIndex)
        {
            dependentAsmJSModules_->erase(dependentAsmJSModules_->begin() + i);
            break;
        }
    }
}

ICEntry&
BaselineScript::icEntry(size_t index)
{
    MOZ_ASSERT(index < numICEntries());
    return icEntryList()[index];
}

PCMappingIndexEntry&
BaselineScript::pcMappingIndexEntry(size_t index)
{
    MOZ_ASSERT(index < numPCMappingIndexEntries());
    return pcMappingIndexEntryList()[index];
}

CompactBufferReader
BaselineScript::pcMappingReader(size_t indexEntry)
{
    PCMappingIndexEntry& entry = pcMappingIndexEntry(indexEntry);

    uint8_t* dataStart = pcMappingData() + entry.bufferOffset;
    uint8_t* dataEnd = (indexEntry == numPCMappingIndexEntries() - 1)
        ? pcMappingData() + pcMappingSize_
        : pcMappingData() + pcMappingIndexEntry(indexEntry + 1).bufferOffset;

    return CompactBufferReader(dataStart, dataEnd);
}

ICEntry&
BaselineScript::icEntryFromReturnOffset(CodeOffset returnOffset)
{
    size_t bottom = 0;
    size_t top = numICEntries();
    size_t mid = bottom + (top - bottom) / 2;
    while (mid < top) {
        ICEntry& midEntry = icEntry(mid);
        if (midEntry.returnOffset().offset() < returnOffset.offset())
            bottom = mid + 1;
        else
            top = mid;
        mid = bottom + (top - bottom) / 2;
    }

    MOZ_ASSERT(mid < numICEntries());
    MOZ_ASSERT(icEntry(mid).returnOffset().offset() == returnOffset.offset());

    return icEntry(mid);
}

uint8_t*
BaselineScript::returnAddressForIC(const ICEntry& ent)
{
    return method()->raw() + ent.returnOffset().offset();
}

static inline size_t
ComputeBinarySearchMid(BaselineScript* baseline, uint32_t pcOffset)
{
    size_t bottom = 0;
    size_t top = baseline->numICEntries();
    size_t mid = bottom + (top - bottom) / 2;
    while (mid < top) {
        ICEntry& midEntry = baseline->icEntry(mid);
        if (midEntry.pcOffset() < pcOffset)
            bottom = mid + 1;
        else if (midEntry.pcOffset() > pcOffset)
            top = mid;
        else
            break;
        mid = bottom + (top - bottom) / 2;
    }
    return mid;
}

ICEntry&
BaselineScript::icEntryFromPCOffset(uint32_t pcOffset)
{
    // Multiple IC entries can have the same PC offset, but this method only looks for
    // those which have isForOp() set.
    size_t mid = ComputeBinarySearchMid(this, pcOffset);

    // Found an IC entry with a matching PC offset.  Search backward, and then
    // forward from this IC entry, looking for one with the same PC offset which
    // has isForOp() set.
    for (size_t i = mid; i < numICEntries() && icEntry(i).pcOffset() == pcOffset; i--) {
        if (icEntry(i).isForOp())
            return icEntry(i);
    }
    for (size_t i = mid+1; i < numICEntries() && icEntry(i).pcOffset() == pcOffset; i++) {
        if (icEntry(i).isForOp())
            return icEntry(i);
    }
    MOZ_CRASH("Invalid PC offset for IC entry.");
}

ICEntry&
BaselineScript::icEntryFromPCOffset(uint32_t pcOffset, ICEntry* prevLookedUpEntry)
{
    // Do a linear forward search from the last queried PC offset, or fallback to a
    // binary search if the last offset is too far away.
    if (prevLookedUpEntry && pcOffset >= prevLookedUpEntry->pcOffset() &&
        (pcOffset - prevLookedUpEntry->pcOffset()) <= 10)
    {
        ICEntry* firstEntry = &icEntry(0);
        ICEntry* lastEntry = &icEntry(numICEntries() - 1);
        ICEntry* curEntry = prevLookedUpEntry;
        while (curEntry >= firstEntry && curEntry <= lastEntry) {
            if (curEntry->pcOffset() == pcOffset && curEntry->isForOp())
                break;
            curEntry++;
        }
        MOZ_ASSERT(curEntry->pcOffset() == pcOffset && curEntry->isForOp());
        return *curEntry;
    }

    return icEntryFromPCOffset(pcOffset);
}

ICEntry&
BaselineScript::callVMEntryFromPCOffset(uint32_t pcOffset)
{
    // Like icEntryFromPCOffset, but only looks for the fake ICEntries
    // inserted by VM calls.
    size_t mid = ComputeBinarySearchMid(this, pcOffset);

    for (size_t i = mid; i < numICEntries() && icEntry(i).pcOffset() == pcOffset; i--) {
        if (icEntry(i).kind() == ICEntry::Kind_CallVM)
            return icEntry(i);
    }
    for (size_t i = mid+1; i < numICEntries() && icEntry(i).pcOffset() == pcOffset; i++) {
        if (icEntry(i).kind() == ICEntry::Kind_CallVM)
            return icEntry(i);
    }
    MOZ_CRASH("Invalid PC offset for callVM entry.");
}

ICEntry&
BaselineScript::stackCheckICEntry(bool earlyCheck)
{
    // The stack check will always be at offset 0, so just do a linear search
    // from the beginning. This is only needed for debug mode OSR, when
    // patching a frame that has invoked a Debugger hook via the interrupt
    // handler via the stack check, which is part of the prologue.
    ICEntry::Kind kind = earlyCheck ? ICEntry::Kind_EarlyStackCheck : ICEntry::Kind_StackCheck;
    for (size_t i = 0; i < numICEntries() && icEntry(i).pcOffset() == 0; i++) {
        if (icEntry(i).kind() == kind)
            return icEntry(i);
    }
    MOZ_CRASH("No stack check ICEntry found.");
}

ICEntry&
BaselineScript::icEntryFromReturnAddress(uint8_t* returnAddr)
{
    MOZ_ASSERT(returnAddr > method_->raw());
    MOZ_ASSERT(returnAddr < method_->raw() + method_->instructionsSize());
    CodeOffset offset(returnAddr - method_->raw());
    return icEntryFromReturnOffset(offset);
}

void
BaselineScript::copyYieldEntries(JSScript* script, Vector<uint32_t>& yieldOffsets)
{
    uint8_t** entries = yieldEntryList();

    for (size_t i = 0; i < yieldOffsets.length(); i++) {
        uint32_t offset = yieldOffsets[i];
        entries[i] = nativeCodeForPC(script, script->offsetToPC(offset));
    }
}

void
BaselineScript::copyICEntries(JSScript* script, const ICEntry* entries, MacroAssembler& masm)
{
    // Fix up the return offset in the IC entries and copy them in.
    // Also write out the IC entry ptrs in any fallback stubs that were added.
    for (uint32_t i = 0; i < numICEntries(); i++) {
        ICEntry& realEntry = icEntry(i);
        realEntry = entries[i];

        if (!realEntry.hasStub()) {
            // VM call without any stubs.
            continue;
        }

        // If the attached stub is a fallback stub, then fix it up with
        // a pointer to the (now available) realEntry.
        if (realEntry.firstStub()->isFallback())
            realEntry.firstStub()->toFallbackStub()->fixupICEntry(&realEntry);

        if (realEntry.firstStub()->isTypeMonitor_Fallback()) {
            ICTypeMonitor_Fallback* stub = realEntry.firstStub()->toTypeMonitor_Fallback();
            stub->fixupICEntry(&realEntry);
        }

        if (realEntry.firstStub()->isTableSwitch()) {
            ICTableSwitch* stub = realEntry.firstStub()->toTableSwitch();
            stub->fixupJumpTable(script, this);
        }
    }
}

void
BaselineScript::adoptFallbackStubs(FallbackICStubSpace* stubSpace)
{
    fallbackStubSpace_.adoptFrom(stubSpace);
}

void
BaselineScript::copyPCMappingEntries(const CompactBufferWriter& entries)
{
    MOZ_ASSERT(entries.length() > 0);
    MOZ_ASSERT(entries.length() == pcMappingSize_);

    memcpy(pcMappingData(), entries.buffer(), entries.length());
}

void
BaselineScript::copyPCMappingIndexEntries(const PCMappingIndexEntry* entries)
{
    for (uint32_t i = 0; i < numPCMappingIndexEntries(); i++)
        pcMappingIndexEntry(i) = entries[i];
}

uint8_t*
BaselineScript::nativeCodeForPC(JSScript* script, jsbytecode* pc, PCMappingSlotInfo* slotInfo)
{
    MOZ_ASSERT_IF(script->hasBaselineScript(), script->baselineScript() == this);

    uint32_t pcOffset = script->pcToOffset(pc);

    // Look for the first PCMappingIndexEntry with pc > the pc we are
    // interested in.
    uint32_t i = 1;
    for (; i < numPCMappingIndexEntries(); i++) {
        if (pcMappingIndexEntry(i).pcOffset > pcOffset)
            break;
    }

    // The previous entry contains the current pc.
    MOZ_ASSERT(i > 0);
    i--;

    PCMappingIndexEntry& entry = pcMappingIndexEntry(i);
    MOZ_ASSERT(pcOffset >= entry.pcOffset);

    CompactBufferReader reader(pcMappingReader(i));
    jsbytecode* curPC = script->offsetToPC(entry.pcOffset);
    uint32_t nativeOffset = entry.nativeOffset;

    MOZ_ASSERT(script->containsPC(curPC));
    MOZ_ASSERT(curPC <= pc);

    while (reader.more()) {
        // If the high bit is set, the native offset relative to the
        // previous pc != 0 and comes next.
        uint8_t b = reader.readByte();
        if (b & 0x80)
            nativeOffset += reader.readUnsigned();

        if (curPC == pc) {
            if (slotInfo)
                *slotInfo = PCMappingSlotInfo(b & ~0x80);
            return method_->raw() + nativeOffset;
        }

        curPC += GetBytecodeLength(curPC);
    }

    MOZ_CRASH("No native code for this pc");
}

jsbytecode*
BaselineScript::approximatePcForNativeAddress(JSScript* script, uint8_t* nativeAddress)
{
    MOZ_ASSERT(script->baselineScript() == this);
    MOZ_ASSERT(nativeAddress >= method_->raw());
    MOZ_ASSERT(nativeAddress < method_->raw() + method_->instructionsSize());

    uint32_t nativeOffset = nativeAddress - method_->raw();
    MOZ_ASSERT(nativeOffset < method_->instructionsSize());

    // Look for the first PCMappingIndexEntry with native offset > the native offset we are
    // interested in.
    uint32_t i = 1;
    for (; i < numPCMappingIndexEntries(); i++) {
        if (pcMappingIndexEntry(i).nativeOffset > nativeOffset)
            break;
    }

    // Go back an entry to search forward from.
    MOZ_ASSERT(i > 0);
    i--;

    PCMappingIndexEntry& entry = pcMappingIndexEntry(i);

    CompactBufferReader reader(pcMappingReader(i));
    jsbytecode* curPC = script->offsetToPC(entry.pcOffset);
    uint32_t curNativeOffset = entry.nativeOffset;

    MOZ_ASSERT(script->containsPC(curPC));

    // The native code address can occur before the start of ops.
    // Associate those with bytecode offset 0.
    if (curNativeOffset > nativeOffset)
        return script->code();

    jsbytecode* lastPC = curPC;
    while (true) {
        // If the high bit is set, the native offset relative to the
        // previous pc != 0 and comes next.
        uint8_t b = reader.readByte();
        if (b & 0x80)
            curNativeOffset += reader.readUnsigned();

        // Return the last PC that matched nativeOffset. Some bytecode
        // generate no native code (e.g., constant-pushing bytecode like
        // JSOP_INT8), and so their entries share the same nativeOffset as the
        // next op that does generate code.
        if (curNativeOffset > nativeOffset)
            return lastPC;

        // The native address may lie in-between the last delta-entry in
        // a pcMappingIndexEntry, and the next pcMappingIndexEntry.
        if (!reader.more())
            return curPC;

        lastPC = curPC;
        curPC += GetBytecodeLength(curPC);
    }
}

void
BaselineScript::toggleDebugTraps(JSScript* script, jsbytecode* pc)
{
    MOZ_ASSERT(script->baselineScript() == this);

    // Only scripts compiled for debug mode have toggled calls.
    if (!hasDebugInstrumentation())
        return;

    SrcNoteLineScanner scanner(script->notes(), script->lineno());

    AutoWritableJitCode awjc(method());

    for (uint32_t i = 0; i < numPCMappingIndexEntries(); i++) {
        PCMappingIndexEntry& entry = pcMappingIndexEntry(i);

        CompactBufferReader reader(pcMappingReader(i));
        jsbytecode* curPC = script->offsetToPC(entry.pcOffset);
        uint32_t nativeOffset = entry.nativeOffset;

        MOZ_ASSERT(script->containsPC(curPC));

        while (reader.more()) {
            uint8_t b = reader.readByte();
            if (b & 0x80)
                nativeOffset += reader.readUnsigned();

            scanner.advanceTo(script->pcToOffset(curPC));

            if (!pc || pc == curPC) {
                bool enabled = (script->stepModeEnabled() && scanner.isLineHeader()) ||
                    script->hasBreakpointsAt(curPC);

                // Patch the trap.
                CodeLocationLabel label(method(), CodeOffset(nativeOffset));
                Assembler::ToggleCall(label, enabled);
            }

            curPC += GetBytecodeLength(curPC);
        }
    }
}

#ifdef JS_TRACE_LOGGING
void
BaselineScript::initTraceLogger(JSRuntime* runtime, JSScript* script)
{
#ifdef DEBUG
    traceLoggerScriptsEnabled_ = TraceLogTextIdEnabled(TraceLogger_Scripts);
    traceLoggerEngineEnabled_ = TraceLogTextIdEnabled(TraceLogger_Engine);
#endif

    TraceLoggerThread* logger = TraceLoggerForMainThread(runtime);
    traceLoggerScriptEvent_ = TraceLoggerEvent(logger, TraceLogger_Scripts, script);

    if (TraceLogTextIdEnabled(TraceLogger_Engine) || TraceLogTextIdEnabled(TraceLogger_Scripts)) {
        CodeLocationLabel enter(method_, CodeOffset(traceLoggerEnterToggleOffset_));
        CodeLocationLabel exit(method_, CodeOffset(traceLoggerExitToggleOffset_));
        Assembler::ToggleToCmp(enter);
        Assembler::ToggleToCmp(exit);
    }
}

void
BaselineScript::toggleTraceLoggerScripts(JSRuntime* runtime, JSScript* script, bool enable)
{
    bool engineEnabled = TraceLogTextIdEnabled(TraceLogger_Engine);

    MOZ_ASSERT(enable == !traceLoggerScriptsEnabled_);
    MOZ_ASSERT(engineEnabled == traceLoggerEngineEnabled_);

    // Patch the logging script textId to be correct.
    // When logging log the specific textId else the global Scripts textId.
    TraceLoggerThread* logger = TraceLoggerForMainThread(runtime);
    if (enable)
        traceLoggerScriptEvent_ = TraceLoggerEvent(logger, TraceLogger_Scripts, script);
    else
        traceLoggerScriptEvent_ = TraceLoggerEvent(logger, TraceLogger_Scripts);

    AutoWritableJitCode awjc(method());

    // Enable/Disable the traceLogger prologue and epilogue.
    CodeLocationLabel enter(method_, CodeOffset(traceLoggerEnterToggleOffset_));
    CodeLocationLabel exit(method_, CodeOffset(traceLoggerExitToggleOffset_));
    if (!engineEnabled) {
        if (enable) {
            Assembler::ToggleToCmp(enter);
            Assembler::ToggleToCmp(exit);
        } else {
            Assembler::ToggleToJmp(enter);
            Assembler::ToggleToJmp(exit);
        }
    }

#if DEBUG
    traceLoggerScriptsEnabled_ = enable;
#endif
}

void
BaselineScript::toggleTraceLoggerEngine(bool enable)
{
    bool scriptsEnabled = TraceLogTextIdEnabled(TraceLogger_Scripts);

    MOZ_ASSERT(enable == !traceLoggerEngineEnabled_);
    MOZ_ASSERT(scriptsEnabled == traceLoggerScriptsEnabled_);

    AutoWritableJitCode awjc(method());

    // Enable/Disable the traceLogger prologue and epilogue.
    CodeLocationLabel enter(method_, CodeOffset(traceLoggerEnterToggleOffset_));
    CodeLocationLabel exit(method_, CodeOffset(traceLoggerExitToggleOffset_));
    if (!scriptsEnabled) {
        if (enable) {
            Assembler::ToggleToCmp(enter);
            Assembler::ToggleToCmp(exit);
        } else {
            Assembler::ToggleToJmp(enter);
            Assembler::ToggleToJmp(exit);
        }
    }

#if DEBUG
    traceLoggerEngineEnabled_ = enable;
#endif
}
#endif

void
BaselineScript::toggleProfilerInstrumentation(bool enable)
{
    if (enable == isProfilerInstrumentationOn())
        return;

    JitSpew(JitSpew_BaselineIC, "  toggling profiling %s for BaselineScript %p",
            enable ? "on" : "off", this);

    AutoWritableJitCode awjc(method());

    // Toggle the jump
    CodeLocationLabel enterToggleLocation(method_, CodeOffset(profilerEnterToggleOffset_));
    CodeLocationLabel exitToggleLocation(method_, CodeOffset(profilerExitToggleOffset_));
    if (enable) {
        Assembler::ToggleToCmp(enterToggleLocation);
        Assembler::ToggleToCmp(exitToggleLocation);
        flags_ |= uint32_t(PROFILER_INSTRUMENTATION_ON);
    } else {
        Assembler::ToggleToJmp(enterToggleLocation);
        Assembler::ToggleToJmp(exitToggleLocation);
        flags_ &= ~uint32_t(PROFILER_INSTRUMENTATION_ON);
    }
}

void
BaselineScript::purgeOptimizedStubs(Zone* zone)
{
    JitSpew(JitSpew_BaselineIC, "Purging optimized stubs");

    for (size_t i = 0; i < numICEntries(); i++) {
        ICEntry& entry = icEntry(i);
        if (!entry.hasStub())
            continue;

        ICStub* lastStub = entry.firstStub();
        while (lastStub->next())
            lastStub = lastStub->next();

        if (lastStub->isFallback()) {
            // Unlink all stubs allocated in the optimized space.
            ICStub* stub = entry.firstStub();
            ICStub* prev = nullptr;

            while (stub->next()) {
                if (!stub->allocatedInFallbackSpace()) {
                    lastStub->toFallbackStub()->unlinkStub(zone, prev, stub);
                    stub = stub->next();
                    continue;
                }

                prev = stub;
                stub = stub->next();
            }

            if (lastStub->isMonitoredFallback()) {
                // Monitor stubs can't make calls, so are always in the
                // optimized stub space.
                ICTypeMonitor_Fallback* lastMonStub =
                    lastStub->toMonitoredFallbackStub()->fallbackMonitorStub();
                lastMonStub->resetMonitorStubChain(zone);
            }
        } else if (lastStub->isTypeMonitor_Fallback()) {
            lastStub->toTypeMonitor_Fallback()->resetMonitorStubChain(zone);
        } else {
            MOZ_ASSERT(lastStub->isTableSwitch());
        }
    }

#ifdef DEBUG
    // All remaining stubs must be allocated in the fallback space.
    for (size_t i = 0; i < numICEntries(); i++) {
        ICEntry& entry = icEntry(i);
        if (!entry.hasStub())
            continue;

        ICStub* stub = entry.firstStub();
        while (stub->next()) {
            MOZ_ASSERT(stub->allocatedInFallbackSpace());
            stub = stub->next();
        }
    }
#endif
}

void
jit::FinishDiscardBaselineScript(FreeOp* fop, JSScript* script)
{
    if (!script->hasBaselineScript())
        return;

    if (script->baselineScript()->active()) {
        // Script is live on the stack. Keep the BaselineScript, but destroy
        // stubs allocated in the optimized stub space.
        script->baselineScript()->purgeOptimizedStubs(script->zone());

        // Reset |active| flag so that we don't need a separate script
        // iteration to unmark them.
        script->baselineScript()->resetActive();

        // The baseline caches have been wiped out, so the script will need to
        // warm back up before it can be inlined during Ion compilation.
        script->baselineScript()->clearIonCompiledOrInlined();
        return;
    }

    BaselineScript* baseline = script->baselineScript();
    script->setBaselineScript(nullptr, nullptr);
    BaselineScript::Destroy(fop, baseline);
}

void
jit::AddSizeOfBaselineData(JSScript* script, mozilla::MallocSizeOf mallocSizeOf, size_t* data,
                           size_t* fallbackStubs)
{
    if (script->hasBaselineScript())
        script->baselineScript()->addSizeOfIncludingThis(mallocSizeOf, data, fallbackStubs);
}

void
jit::ToggleBaselineProfiling(JSRuntime* runtime, bool enable)
{
    for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
        for (gc::ZoneCellIter i(zone, gc::AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            if (!script->hasBaselineScript())
                continue;
            script->baselineScript()->toggleProfilerInstrumentation(enable);
        }
    }
}

#ifdef JS_TRACE_LOGGING
void
jit::ToggleBaselineTraceLoggerScripts(JSRuntime* runtime, bool enable)
{
    for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
        for (gc::ZoneCellIter i(zone, gc::AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            if (!script->hasBaselineScript())
                continue;
            script->baselineScript()->toggleTraceLoggerScripts(runtime, script, enable);
        }
    }
}

void
jit::ToggleBaselineTraceLoggerEngine(JSRuntime* runtime, bool enable)
{
    for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
        for (gc::ZoneCellIter i(zone, gc::AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            if (!script->hasBaselineScript())
                continue;
            script->baselineScript()->toggleTraceLoggerEngine(enable);
        }
    }
}
#endif

static void
MarkActiveBaselineScripts(JSRuntime* rt, const JitActivationIterator& activation)
{
    for (jit::JitFrameIterator iter(activation); !iter.done(); ++iter) {
        switch (iter.type()) {
          case JitFrame_BaselineJS:
            iter.script()->baselineScript()->setActive();
            break;
          case JitFrame_LazyLink: {
            LazyLinkExitFrameLayout* ll = iter.exitFrame()->as<LazyLinkExitFrameLayout>();
            ScriptFromCalleeToken(ll->jsFrame()->calleeToken())->baselineScript()->setActive();
            break;
          }
          case JitFrame_Bailout:
          case JitFrame_IonJS: {
            // Keep the baseline script around, since bailouts from the ion
            // jitcode might need to re-enter into the baseline jitcode.
            iter.script()->baselineScript()->setActive();
            for (InlineFrameIterator inlineIter(rt, &iter); inlineIter.more(); ++inlineIter)
                inlineIter.script()->baselineScript()->setActive();
            break;
          }
          default:;
        }
    }
}

void
jit::MarkActiveBaselineScripts(Zone* zone)
{
    JSRuntime* rt = zone->runtimeFromMainThread();
    for (JitActivationIterator iter(rt); !iter.done(); ++iter) {
        if (iter->compartment()->zone() == zone)
            MarkActiveBaselineScripts(rt, iter);
    }
}
