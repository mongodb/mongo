/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineJIT.h"

#include "mozilla/BinarySearch.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MemoryReporting.h"

#include "gc/FreeOp.h"
#include "jit/BaselineCompiler.h"
#include "jit/BaselineIC.h"
#include "jit/CompileInfo.h"
#include "jit/IonControlFlow.h"
#include "jit/JitCommon.h"
#include "jit/JitSpewer.h"
#include "vm/Debugger.h"
#include "vm/Interpreter.h"
#include "vm/TraceLogging.h"
#include "wasm/WasmInstance.h"

#include "gc/PrivateIterators-inl.h"
#include "jit/JitFrames-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Stack-inl.h"

using mozilla::BinarySearchIf;
using mozilla::DebugOnly;

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

void
ICStubSpace::freeAllAfterMinorGC(Zone* zone)
{
    if (zone->isAtomsZone())
        MOZ_ASSERT(allocator_.isEmpty());
    else
        zone->runtimeFromActiveCooperatingThread()->gc.freeAllLifoBlocksAfterMinorGC(&allocator_);
}

BaselineScript::BaselineScript(uint32_t prologueOffset, uint32_t epilogueOffset,
                               uint32_t profilerEnterToggleOffset,
                               uint32_t profilerExitToggleOffset,
                               uint32_t postDebugPrologueOffset)
  : method_(nullptr),
    templateEnv_(nullptr),
    fallbackStubSpace_(),
    dependentWasmImports_(nullptr),
    prologueOffset_(prologueOffset),
    epilogueOffset_(epilogueOffset),
    profilerEnterToggleOffset_(profilerEnterToggleOffset),
    profilerExitToggleOffset_(profilerExitToggleOffset),
#ifdef JS_TRACE_LOGGING
# ifdef DEBUG
    traceLoggerScriptsEnabled_(false),
    traceLoggerEngineEnabled_(false),
# endif
    traceLoggerScriptEvent_(),
#endif
    postDebugPrologueOffset_(postDebugPrologueOffset),
    flags_(0),
    inlinedBytecodeLength_(0),
    maxInliningDepth_(UINT8_MAX),
    pendingBuilder_(nullptr),
    controlFlowGraph_(nullptr)
{ }

static bool
CheckFrame(InterpreterFrame* fp)
{
    if (fp->isDebuggerEvalFrame()) {
        // Debugger eval-in-frame. These are likely short-running scripts so
        // don't bother compiling them for now.
        JitSpew(JitSpew_BaselineAbort, "debugger frame");
        return false;
    }

    if (fp->isFunctionFrame() && fp->numActualArgs() > BASELINE_MAX_ARGS_LENGTH) {
        // Fall back to the interpreter to avoid running out of stack space.
        JitSpew(JitSpew_BaselineAbort, "Too many arguments (%u)", fp->numActualArgs());
        return false;
    }

    return true;
}

static JitExecStatus
EnterBaseline(JSContext* cx, EnterJitData& data)
{
    MOZ_ASSERT(data.osrFrame);

    // Check for potential stack overflow before OSR-ing.
    uint8_t spDummy;
    uint32_t extra = BaselineFrame::Size() + (data.osrNumStackValues * sizeof(Value));
    uint8_t* checkSp = (&spDummy) - extra;
    if (!CheckRecursionLimitWithStackPointer(cx, checkSp))
        return JitExec_Aborted;

#ifdef DEBUG
    // Assert we don't GC before entering JIT code. A GC could discard JIT code
    // or move the function stored in the CalleeToken (it won't be traced at
    // this point). We use Maybe<> here so we can call reset() to call the
    // AutoAssertNoGC destructor before we enter JIT code.
    mozilla::Maybe<JS::AutoAssertNoGC> nogc;
    nogc.emplace(cx);
#endif

    MOZ_ASSERT(jit::IsBaselineEnabled(cx));
    MOZ_ASSERT(CheckFrame(data.osrFrame));

    EnterJitCode enter = cx->runtime()->jitRuntime()->enterJit();

    // Caller must construct |this| before invoking the function.
    MOZ_ASSERT_IF(data.constructing,
                  data.maxArgv[0].isObject() || data.maxArgv[0].isMagic(JS_UNINITIALIZED_LEXICAL));

    data.result.setInt32(data.numActualArgs);
    {
        AssertCompartmentUnchanged pcc(cx);
        ActivationEntryMonitor entryMonitor(cx, data.calleeToken);
        JitActivation activation(cx);

        data.osrFrame->setRunningInJit();

#ifdef DEBUG
        nogc.reset();
#endif
        // Single transition point from Interpreter to Baseline.
        CALL_GENERATED_CODE(enter, data.jitcode, data.maxArgc, data.maxArgv, data.osrFrame,
                            data.calleeToken, data.envChain.get(), data.osrNumStackValues,
                            data.result.address());

        data.osrFrame->clearRunningInJit();
    }

    MOZ_ASSERT(!cx->hasIonReturnOverride());

    // Jit callers wrap primitive constructor return, except for derived
    // class constructors, which are forced to do it themselves.
    if (!data.result.isMagic() &&
        data.constructing &&
        data.result.isPrimitive())
    {
        MOZ_ASSERT(data.maxArgv[0].isObject());
        data.result = data.maxArgv[0];
    }

    // Release temporary buffer used for OSR into Ion.
    cx->freeOsrTempData();

    MOZ_ASSERT_IF(data.result.isMagic(), data.result.isMagic(JS_ION_ERROR));
    return data.result.isMagic() ? JitExec_Error : JitExec_Ok;
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

    // Note: keep this in sync with SetEnterJitData.

    data.osrFrame = fp;
    data.osrNumStackValues = fp->script()->nfixed() + cx->interpreterRegs().stackDepth();

    RootedValue newTarget(cx);

    if (fp->isFunctionFrame()) {
        data.constructing = fp->isConstructing();
        data.numActualArgs = fp->numActualArgs();
        data.maxArgc = Max(fp->numActualArgs(), fp->numFormalArgs()) + 1; // +1 = include |this|
        data.maxArgv = fp->argv() - 1; // -1 = include |this|
        data.envChain = nullptr;
        data.calleeToken = CalleeToToken(&fp->callee(), data.constructing);
    } else {
        data.constructing = false;
        data.numActualArgs = 0;
        data.maxArgc = 0;
        data.maxArgv = nullptr;
        data.envChain = fp->environmentChain();

        data.calleeToken = CalleeToToken(fp->script());

        if (fp->isEvalFrame()) {
            newTarget = fp->newTarget();
            data.maxArgc = 1;
            data.maxArgv = newTarget.address();
        }
    }

    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
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

    script->ensureNonLazyCanonicalFunction();

    TempAllocator temp(&cx->tempLifoAlloc());
    JitContext jctx(cx, nullptr);

    BaselineCompiler compiler(cx, temp, script);
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
        script->setBaselineScript(cx->runtime(), BASELINE_DISABLED_SCRIPT);

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

    if (script->hasBaselineScript())
        return Method_Compiled;

    // Check this before calling ensureJitCompartmentExists, so we're less
    // likely to report OOM in JSRuntime::createJitRuntime.
    if (!CanLikelyAllocateMoreExecutableMemory())
        return Method_Skipped;

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return Method_Error;

    // Check script warm-up counter.
    if (script->incWarmUpCounter() <= JitOptions.baselineWarmUpThreshold)
        return Method_Skipped;

    // Frames can be marked as debuggee frames independently of its underlying
    // script being a debuggee script, e.g., when performing
    // Debugger.Frame.prototype.eval.
    return BaselineCompile(cx, script, osrFrame && osrFrame->isDebuggee());
}

MethodStatus
jit::CanEnterBaselineAtBranch(JSContext* cx, InterpreterFrame* fp)
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
    } else {
        if (state.asExecute()->isDebuggerEval()) {
            JitSpew(JitSpew_BaselineAbort, "debugger frame");
            return Method_CantCompile;
        }
    }

    RootedScript script(cx, state.script());
    return CanEnterBaselineJIT(cx, script, /* osrFrame = */ nullptr);
};

BaselineScript*
BaselineScript::New(JSScript* jsscript,
                    uint32_t prologueOffset, uint32_t epilogueOffset,
                    uint32_t profilerEnterToggleOffset,
                    uint32_t profilerExitToggleOffset,
                    uint32_t postDebugPrologueOffset,
                    size_t icEntries,
                    size_t pcMappingIndexEntries, size_t pcMappingSize,
                    size_t bytecodeTypeMapEntries,
                    size_t yieldEntries,
                    size_t traceLoggerToggleOffsetEntries)
{
    static const unsigned DataAlignment = sizeof(uintptr_t);

    size_t icEntriesSize = icEntries * sizeof(BaselineICEntry);
    size_t pcMappingIndexEntriesSize = pcMappingIndexEntries * sizeof(PCMappingIndexEntry);
    size_t bytecodeTypeMapSize = bytecodeTypeMapEntries * sizeof(uint32_t);
    size_t yieldEntriesSize = yieldEntries * sizeof(uintptr_t);
    size_t tlEntriesSize = traceLoggerToggleOffsetEntries * sizeof(uint32_t);

    size_t paddedICEntriesSize = AlignBytes(icEntriesSize, DataAlignment);
    size_t paddedPCMappingIndexEntriesSize = AlignBytes(pcMappingIndexEntriesSize, DataAlignment);
    size_t paddedPCMappingSize = AlignBytes(pcMappingSize, DataAlignment);
    size_t paddedBytecodeTypesMapSize = AlignBytes(bytecodeTypeMapSize, DataAlignment);
    size_t paddedYieldEntriesSize = AlignBytes(yieldEntriesSize, DataAlignment);
    size_t paddedTLEntriesSize = AlignBytes(tlEntriesSize, DataAlignment);

    size_t allocBytes = paddedICEntriesSize +
                        paddedPCMappingIndexEntriesSize +
                        paddedPCMappingSize +
                        paddedBytecodeTypesMapSize +
                        paddedYieldEntriesSize +
                        paddedTLEntriesSize;

    BaselineScript* script = jsscript->zone()->pod_malloc_with_extra<BaselineScript, uint8_t>(allocBytes);
    if (!script)
        return nullptr;
    new (script) BaselineScript(prologueOffset, epilogueOffset,
                                profilerEnterToggleOffset, profilerExitToggleOffset,
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

    script->traceLoggerToggleOffsetsOffset_ = tlEntriesSize ? offsetCursor : 0;
    script->numTraceLoggerToggleOffsets_ = traceLoggerToggleOffsetEntries;
    offsetCursor += paddedTLEntriesSize;

    MOZ_ASSERT(offsetCursor == sizeof(BaselineScript) + allocBytes);
    return script;
}

void
BaselineScript::trace(JSTracer* trc)
{
    TraceEdge(trc, &method_, "baseline-method");
    TraceNullableEdge(trc, &templateEnv_, "baseline-template-environment");

    // Mark all IC stub codes hanging off the IC stub entries.
    for (size_t i = 0; i < numICEntries(); i++) {
        BaselineICEntry& ent = icEntry(i);
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

    MOZ_ASSERT(!script->hasPendingIonBuilder());

    script->unlinkDependentWasmImports(fop);

    /*
     * When the script contains pointers to nursery things, the store buffer can
     * contain entries that point into the fallback stub space. Since we can
     * destroy scripts outside the context of a GC, this situation could result
     * in us trying to mark invalid store buffer entries.
     *
     * Defer freeing any allocated blocks until after the next minor GC.
     */
    script->fallbackStubSpace_.freeAllAfterMinorGC(script->method()->zone());

    fop->delete_(script);
}

void
JS::DeletePolicy<js::jit::BaselineScript>::operator()(const js::jit::BaselineScript* script)
{
    BaselineScript::Destroy(rt_->defaultFreeOp(), const_cast<BaselineScript*>(script));
}

void
BaselineScript::clearDependentWasmImports()
{
    // Remove any links from wasm::Instances that contain optimized import calls into
    // this BaselineScript.
    if (dependentWasmImports_) {
        for (DependentWasmImport& dep : *dependentWasmImports_)
            dep.instance->deoptimizeImportExit(dep.importIndex);
        dependentWasmImports_->clear();
    }
}

void
BaselineScript::unlinkDependentWasmImports(FreeOp* fop)
{
    // Remove any links from wasm::Instances that contain optimized FFI calls into
    // this BaselineScript.
    clearDependentWasmImports();
    if (dependentWasmImports_) {
        fop->delete_(dependentWasmImports_);
        dependentWasmImports_ = nullptr;
    }
}

bool
BaselineScript::addDependentWasmImport(JSContext* cx, wasm::Instance& instance, uint32_t idx)
{
    if (!dependentWasmImports_) {
        dependentWasmImports_ = cx->new_<Vector<DependentWasmImport>>(cx);
        if (!dependentWasmImports_)
            return false;
    }
    return dependentWasmImports_->emplaceBack(instance, idx);
}

void
BaselineScript::removeDependentWasmImport(wasm::Instance& instance, uint32_t idx)
{
    if (!dependentWasmImports_)
        return;

    for (DependentWasmImport& dep : *dependentWasmImports_) {
        if (dep.instance == &instance && dep.importIndex == idx) {
            dependentWasmImports_->erase(&dep);
            break;
        }
    }
}

BaselineICEntry&
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

struct ICEntries
{
    BaselineScript* const baseline_;

    explicit ICEntries(BaselineScript* baseline) : baseline_(baseline) {}

    BaselineICEntry& operator[](size_t index) const {
        return baseline_->icEntry(index);
    }
};

BaselineICEntry&
BaselineScript::icEntryFromReturnOffset(CodeOffset returnOffset)
{
    size_t loc;
#ifdef DEBUG
    bool found =
#endif
        BinarySearchIf(ICEntries(this), 0, numICEntries(),
                       [&returnOffset](BaselineICEntry& entry) {
                           size_t roffset = returnOffset.offset();
                           size_t entryRoffset = entry.returnOffset().offset();
                           if (roffset < entryRoffset)
                               return -1;
                           if (entryRoffset < roffset)
                               return 1;
                           return 0;
                       },
                       &loc);

    MOZ_ASSERT(found);
    MOZ_ASSERT(loc < numICEntries());
    MOZ_ASSERT(icEntry(loc).returnOffset().offset() == returnOffset.offset());
    return icEntry(loc);
}

static inline bool
ComputeBinarySearchMid(BaselineScript* baseline, uint32_t pcOffset, size_t* loc)
{
    return BinarySearchIf(ICEntries(baseline), 0, baseline->numICEntries(),
                          [pcOffset](BaselineICEntry& entry) {
                              uint32_t entryOffset = entry.pcOffset();
                              if (pcOffset < entryOffset)
                                  return -1;
                              if (entryOffset < pcOffset)
                                  return 1;
                              return 0;
                          },
                          loc);
}

uint8_t*
BaselineScript::returnAddressForIC(const BaselineICEntry& ent)
{
    return method()->raw() + ent.returnOffset().offset();
}

BaselineICEntry*
BaselineScript::maybeICEntryFromPCOffset(uint32_t pcOffset)
{
    // Multiple IC entries can have the same PC offset, but this method only looks for
    // those which have isForOp() set.
    size_t mid;
    if (!ComputeBinarySearchMid(this, pcOffset, &mid))
        return nullptr;

    MOZ_ASSERT(mid < numICEntries());

    // Found an IC entry with a matching PC offset.  Search backward, and then
    // forward from this IC entry, looking for one with the same PC offset which
    // has isForOp() set.
    for (size_t i = mid; icEntry(i).pcOffset() == pcOffset; i--) {
        if (icEntry(i).isForOp())
            return &icEntry(i);
        if (i == 0)
            break;
    }
    for (size_t i = mid+1; i < numICEntries() && icEntry(i).pcOffset() == pcOffset; i++) {
        if (icEntry(i).isForOp())
            return &icEntry(i);
    }
    return nullptr;
}

BaselineICEntry&
BaselineScript::icEntryFromPCOffset(uint32_t pcOffset)
{
    BaselineICEntry* entry = maybeICEntryFromPCOffset(pcOffset);
    MOZ_RELEASE_ASSERT(entry);
    return *entry;
}

BaselineICEntry*
BaselineScript::maybeICEntryFromPCOffset(uint32_t pcOffset, BaselineICEntry* prevLookedUpEntry)
{
    // Do a linear forward search from the last queried PC offset, or fallback to a
    // binary search if the last offset is too far away.
    if (prevLookedUpEntry && pcOffset >= prevLookedUpEntry->pcOffset() &&
        (pcOffset - prevLookedUpEntry->pcOffset()) <= 10)
    {
        BaselineICEntry* firstEntry = &icEntry(0);
        BaselineICEntry* lastEntry = &icEntry(numICEntries() - 1);
        BaselineICEntry* curEntry = prevLookedUpEntry;
        while (curEntry >= firstEntry && curEntry <= lastEntry) {
            if (curEntry->pcOffset() == pcOffset && curEntry->isForOp())
                return curEntry;
            curEntry++;
        }
        return nullptr;
    }

    return maybeICEntryFromPCOffset(pcOffset);
}

BaselineICEntry&
BaselineScript::icEntryFromPCOffset(uint32_t pcOffset, BaselineICEntry* prevLookedUpEntry)
{
    BaselineICEntry* entry = maybeICEntryFromPCOffset(pcOffset, prevLookedUpEntry);
    MOZ_RELEASE_ASSERT(entry);
    return *entry;
}

BaselineICEntry&
BaselineScript::callVMEntryFromPCOffset(uint32_t pcOffset)
{
    // Like icEntryFromPCOffset, but only looks for the fake ICEntries
    // inserted by VM calls.
    size_t mid;
    MOZ_ALWAYS_TRUE(ComputeBinarySearchMid(this, pcOffset, &mid));
    MOZ_ASSERT(mid < numICEntries());

    for (size_t i = mid; icEntry(i).pcOffset() == pcOffset; i--) {
        if (icEntry(i).kind() == ICEntry::Kind_CallVM)
            return icEntry(i);
        if (i == 0)
            break;
    }
    for (size_t i = mid+1; i < numICEntries() && icEntry(i).pcOffset() == pcOffset; i++) {
        if (icEntry(i).kind() == ICEntry::Kind_CallVM)
            return icEntry(i);
    }
    MOZ_CRASH("Invalid PC offset for callVM entry.");
}

BaselineICEntry&
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

BaselineICEntry&
BaselineScript::warmupCountICEntry()
{
    // The stack check will be at a very low offset, so just do a linear search
    // from the beginning.
    for (size_t i = 0; i < numICEntries() && icEntry(i).pcOffset() == 0; i++) {
        if (icEntry(i).kind() == ICEntry::Kind_WarmupCounter)
            return icEntry(i);
    }
    MOZ_CRASH("No warmup count ICEntry found.");
}

BaselineICEntry&
BaselineScript::icEntryFromReturnAddress(uint8_t* returnAddr)
{
    MOZ_ASSERT(returnAddr > method_->raw());
    MOZ_ASSERT(returnAddr < method_->raw() + method_->instructionsSize());
    CodeOffset offset(returnAddr - method_->raw());
    return icEntryFromReturnOffset(offset);
}

void
BaselineScript::copyYieldAndAwaitEntries(JSScript* script, Vector<uint32_t>& yieldAndAwaitOffsets)
{
    uint8_t** entries = yieldEntryList();

    for (size_t i = 0; i < yieldAndAwaitOffsets.length(); i++) {
        uint32_t offset = yieldAndAwaitOffsets[i];
        entries[i] = nativeCodeForPC(script, script->offsetToPC(offset));
    }
}

void
BaselineScript::copyICEntries(JSScript* script, const BaselineICEntry* entries)
{
    // Fix up the return offset in the IC entries and copy them in.
    // Also write out the IC entry ptrs in any fallback stubs that were added.
    for (uint32_t i = 0; i < numICEntries(); i++) {
        BaselineICEntry& realEntry = icEntry(i);
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
BaselineScript::initTraceLogger(JSScript* script, const Vector<CodeOffset>& offsets)
{
#ifdef DEBUG
    traceLoggerScriptsEnabled_ = TraceLogTextIdEnabled(TraceLogger_Scripts);
    traceLoggerEngineEnabled_ = TraceLogTextIdEnabled(TraceLogger_Engine);
#endif

    MOZ_ASSERT(offsets.length() == numTraceLoggerToggleOffsets_);
    for (size_t i = 0; i < offsets.length(); i++)
        traceLoggerToggleOffsets()[i] = offsets[i].offset();

    if (TraceLogTextIdEnabled(TraceLogger_Engine) || TraceLogTextIdEnabled(TraceLogger_Scripts)) {
        traceLoggerScriptEvent_ = TraceLoggerEvent(TraceLogger_Scripts, script);
        for (size_t i = 0; i < numTraceLoggerToggleOffsets_; i++) {
            CodeLocationLabel label(method_, CodeOffset(traceLoggerToggleOffsets()[i]));
            Assembler::ToggleToCmp(label);
        }
    }
}

void
BaselineScript::toggleTraceLoggerScripts(JSScript* script, bool enable)
{
    DebugOnly<bool> engineEnabled = TraceLogTextIdEnabled(TraceLogger_Engine);
    MOZ_ASSERT(enable == !traceLoggerScriptsEnabled_);
    MOZ_ASSERT(engineEnabled == traceLoggerEngineEnabled_);

    // Patch the logging script textId to be correct.
    // When logging log the specific textId else the global Scripts textId.
    if (enable && !traceLoggerScriptEvent_.hasTextId())
        traceLoggerScriptEvent_ = TraceLoggerEvent(TraceLogger_Scripts, script);

    AutoWritableJitCode awjc(method());

    // Enable/Disable the traceLogger.
    for (size_t i = 0; i < numTraceLoggerToggleOffsets_; i++) {
        CodeLocationLabel label(method_, CodeOffset(traceLoggerToggleOffsets()[i]));
        if (enable)
            Assembler::ToggleToCmp(label);
        else
            Assembler::ToggleToJmp(label);
    }

#if DEBUG
    traceLoggerScriptsEnabled_ = enable;
#endif
}

void
BaselineScript::toggleTraceLoggerEngine(bool enable)
{
    DebugOnly<bool> scriptsEnabled = TraceLogTextIdEnabled(TraceLogger_Scripts);
    MOZ_ASSERT(enable == !traceLoggerEngineEnabled_);
    MOZ_ASSERT(scriptsEnabled == traceLoggerScriptsEnabled_);

    AutoWritableJitCode awjc(method());

    // Enable/Disable the traceLogger prologue and epilogue.
    for (size_t i = 0; i < numTraceLoggerToggleOffsets_; i++) {
        CodeLocationLabel label(method_, CodeOffset(traceLoggerToggleOffsets()[i]));
        if (enable)
            Assembler::ToggleToCmp(label);
        else
            Assembler::ToggleToJmp(label);
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
        BaselineICEntry& entry = icEntry(i);
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
                    lastStub->toMonitoredFallbackStub()->maybeFallbackMonitorStub();
                if (lastMonStub)
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
        BaselineICEntry& entry = icEntry(i);
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
    script->setBaselineScript(fop->runtime(), nullptr);
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
    JitRuntime* jrt = runtime->jitRuntime();
    if (!jrt)
        return;

    for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
        for (auto script = zone->cellIter<JSScript>(); !script.done(); script.next()) {
            if (!script->hasBaselineScript())
                continue;
            AutoWritableJitCode awjc(script->baselineScript()->method());
            script->baselineScript()->toggleProfilerInstrumentation(enable);
        }
    }
}

#ifdef JS_TRACE_LOGGING
void
jit::ToggleBaselineTraceLoggerScripts(JSRuntime* runtime, bool enable)
{
    for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
        for (auto script = zone->cellIter<JSScript>(); !script.done(); script.next()) {
            if (!script->hasBaselineScript())
                continue;
            script->baselineScript()->toggleTraceLoggerScripts(script, enable);
        }
    }
}

void
jit::ToggleBaselineTraceLoggerEngine(JSRuntime* runtime, bool enable)
{
    for (ZonesIter zone(runtime, SkipAtoms); !zone.done(); zone.next()) {
        for (auto script = zone->cellIter<JSScript>(); !script.done(); script.next()) {
            if (!script->hasBaselineScript())
                continue;
            script->baselineScript()->toggleTraceLoggerEngine(enable);
        }
    }
}
#endif

static void
MarkActiveBaselineScripts(JSContext* cx, const JitActivationIterator& activation)
{
    for (OnlyJSJitFrameIter iter(activation); !iter.done(); ++iter) {
        const JSJitFrameIter& frame = iter.frame();
        switch (frame.type()) {
          case JitFrame_BaselineJS:
            frame.script()->baselineScript()->setActive();
            break;
          case JitFrame_Exit:
            if (frame.exitFrame()->is<LazyLinkExitFrameLayout>()) {
                LazyLinkExitFrameLayout* ll = frame.exitFrame()->as<LazyLinkExitFrameLayout>();
                ScriptFromCalleeToken(ll->jsFrame()->calleeToken())->baselineScript()->setActive();
            }
            break;
          case JitFrame_Bailout:
          case JitFrame_IonJS: {
            // Keep the baseline script around, since bailouts from the ion
            // jitcode might need to re-enter into the baseline jitcode.
            frame.script()->baselineScript()->setActive();
            for (InlineFrameIterator inlineIter(cx, &frame); inlineIter.more(); ++inlineIter)
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
    if (zone->isAtomsZone())
        return;
    JSContext* cx = TlsContext.get();
    for (const CooperatingContext& target : cx->runtime()->cooperatingContexts()) {
        for (JitActivationIterator iter(cx, target); !iter.done(); ++iter) {
            if (iter->compartment()->zone() == zone)
                MarkActiveBaselineScripts(cx, iter);
        }
    }
}
