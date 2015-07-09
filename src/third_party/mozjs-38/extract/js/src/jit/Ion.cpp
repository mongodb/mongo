/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Ion.h"

#include "mozilla/MemoryReporting.h"
#include "mozilla/ThreadLocal.h"

#include "jscompartment.h"
#include "jsprf.h"

#include "gc/Marking.h"
#include "jit/AliasAnalysis.h"
#include "jit/BacktrackingAllocator.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineInspector.h"
#include "jit/BaselineJIT.h"
#include "jit/CodeGenerator.h"
#include "jit/EdgeCaseAnalysis.h"
#include "jit/EffectiveAddressAnalysis.h"
#include "jit/IonAnalysis.h"
#include "jit/IonBuilder.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/JitcodeMap.h"
#include "jit/JitCommon.h"
#include "jit/JitCompartment.h"
#include "jit/JitSpewer.h"
#include "jit/LICM.h"
#include "jit/LinearScan.h"
#include "jit/LIR.h"
#include "jit/LoopUnroller.h"
#include "jit/Lowering.h"
#include "jit/PerfSpewer.h"
#include "jit/RangeAnalysis.h"
#include "jit/ScalarReplacement.h"
#include "jit/Sink.h"
#include "jit/StupidAllocator.h"
#include "jit/ValueNumbering.h"
#include "vm/HelperThreads.h"
#include "vm/TraceLogging.h"

#include "jscompartmentinlines.h"
#include "jsgcinlines.h"
#include "jsobjinlines.h"

using namespace js;
using namespace js::jit;

using mozilla::ThreadLocal;

// Assert that JitCode is gc::Cell aligned.
JS_STATIC_ASSERT(sizeof(JitCode) % gc::CellSize == 0);

static ThreadLocal<JitContext*> TlsJitContext;

static JitContext*
CurrentJitContext()
{
    if (!TlsJitContext.initialized())
        return nullptr;
    return TlsJitContext.get();
}

void
jit::SetJitContext(JitContext* ctx)
{
    TlsJitContext.set(ctx);
}

JitContext*
jit::GetJitContext()
{
    MOZ_ASSERT(CurrentJitContext());
    return CurrentJitContext();
}

JitContext*
jit::MaybeGetJitContext()
{
    return CurrentJitContext();
}

JitContext::JitContext(JSContext* cx, TempAllocator* temp)
  : cx(cx),
    temp(temp),
    runtime(CompileRuntime::get(cx->runtime())),
    compartment(CompileCompartment::get(cx->compartment())),
    prev_(CurrentJitContext()),
    assemblerCount_(0)
{
    SetJitContext(this);
}

JitContext::JitContext(ExclusiveContext* cx, TempAllocator* temp)
  : cx(nullptr),
    temp(temp),
    runtime(CompileRuntime::get(cx->runtime_)),
    compartment(nullptr),
    prev_(CurrentJitContext()),
    assemblerCount_(0)
{
    SetJitContext(this);
}

JitContext::JitContext(CompileRuntime* rt, CompileCompartment* comp, TempAllocator* temp)
  : cx(nullptr),
    temp(temp),
    runtime(rt),
    compartment(comp),
    prev_(CurrentJitContext()),
    assemblerCount_(0)
{
    SetJitContext(this);
}

JitContext::JitContext(CompileRuntime* rt)
  : cx(nullptr),
    temp(nullptr),
    runtime(rt),
    compartment(nullptr),
    prev_(CurrentJitContext()),
    assemblerCount_(0)
{
    SetJitContext(this);
}

JitContext::~JitContext()
{
    SetJitContext(prev_);
}

bool
jit::InitializeIon()
{
    if (!TlsJitContext.initialized() && !TlsJitContext.init())
        return false;
    CheckLogging();
#if defined(JS_CODEGEN_ARM)
    InitARMFlags();
#endif
    CheckPerf();
    return true;
}

JitRuntime::JitRuntime()
  : execAlloc_(),
    exceptionTail_(nullptr),
    bailoutTail_(nullptr),
    profilerExitFrameTail_(nullptr),
    enterJIT_(nullptr),
    bailoutHandler_(nullptr),
    argumentsRectifier_(nullptr),
    argumentsRectifierReturnAddr_(nullptr),
    invalidator_(nullptr),
    debugTrapHandler_(nullptr),
    baselineDebugModeOSRHandler_(nullptr),
    functionWrappers_(nullptr),
    osrTempData_(nullptr),
    mutatingBackedgeList_(false),
    ionReturnOverride_(MagicValue(JS_ARG_POISON)),
    jitcodeGlobalTable_(nullptr),
    hasIonNurseryObjects_(false)
{
}

JitRuntime::~JitRuntime()
{
    js_delete(functionWrappers_);
    freeOsrTempData();

    // By this point, the jitcode global table should be empty.
    MOZ_ASSERT_IF(jitcodeGlobalTable_, jitcodeGlobalTable_->empty());
    js_delete(jitcodeGlobalTable_);
}

bool
JitRuntime::initialize(JSContext* cx)
{
    MOZ_ASSERT(cx->runtime()->currentThreadHasExclusiveAccess());

    AutoCompartment ac(cx, cx->atomsCompartment());

    JitContext jctx(cx, nullptr);

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return false;

    functionWrappers_ = cx->new_<VMWrapperMap>(cx);
    if (!functionWrappers_ || !functionWrappers_->init())
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting profiler exit frame tail stub");
    profilerExitFrameTail_ = generateProfilerExitFrameTailStub(cx);
    if (!profilerExitFrameTail_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting exception tail stub");

    void* handler = JS_FUNC_TO_DATA_PTR(void*, jit::HandleException);

    exceptionTail_ = generateExceptionTailStub(cx, handler);
    if (!exceptionTail_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting bailout tail stub");
    bailoutTail_ = generateBailoutTailStub(cx);
    if (!bailoutTail_)
        return false;

    if (cx->runtime()->jitSupportsFloatingPoint) {
        JitSpew(JitSpew_Codegen, "# Emitting bailout tables");

        // Initialize some Ion-only stubs that require floating-point support.
        if (!bailoutTables_.reserve(FrameSizeClass::ClassLimit().classId()))
            return false;

        for (uint32_t id = 0;; id++) {
            FrameSizeClass class_ = FrameSizeClass::FromClass(id);
            if (class_ == FrameSizeClass::ClassLimit())
                break;
            bailoutTables_.infallibleAppend((JitCode*)nullptr);
            bailoutTables_[id] = generateBailoutTable(cx, id);
            if (!bailoutTables_[id])
                return false;
        }

        JitSpew(JitSpew_Codegen, "# Emitting bailout handler");
        bailoutHandler_ = generateBailoutHandler(cx);
        if (!bailoutHandler_)
            return false;

        JitSpew(JitSpew_Codegen, "# Emitting invalidator");
        invalidator_ = generateInvalidator(cx);
        if (!invalidator_)
            return false;
    }

    JitSpew(JitSpew_Codegen, "# Emitting sequential arguments rectifier");
    argumentsRectifier_ = generateArgumentsRectifier(cx, &argumentsRectifierReturnAddr_);
    if (!argumentsRectifier_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting EnterJIT sequence");
    enterJIT_ = generateEnterJIT(cx, EnterJitOptimized);
    if (!enterJIT_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting EnterBaselineJIT sequence");
    enterBaselineJIT_ = generateEnterJIT(cx, EnterJitBaseline);
    if (!enterBaselineJIT_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Value");
    valuePreBarrier_ = generatePreBarrier(cx, MIRType_Value);
    if (!valuePreBarrier_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for String");
    stringPreBarrier_ = generatePreBarrier(cx, MIRType_String);
    if (!stringPreBarrier_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Object");
    objectPreBarrier_ = generatePreBarrier(cx, MIRType_Object);
    if (!objectPreBarrier_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Shape");
    shapePreBarrier_ = generatePreBarrier(cx, MIRType_Shape);
    if (!shapePreBarrier_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for ObjectGroup");
    objectGroupPreBarrier_ = generatePreBarrier(cx, MIRType_ObjectGroup);
    if (!objectGroupPreBarrier_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting malloc stub");
    mallocStub_ = generateMallocStub(cx);
    if (!mallocStub_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting free stub");
    freeStub_ = generateFreeStub(cx);
    if (!freeStub_)
        return false;

    JitSpew(JitSpew_Codegen, "# Emitting VM function wrappers");
    for (VMFunction* fun = VMFunction::functions; fun; fun = fun->next) {
        if (!generateVMWrapper(cx, *fun))
            return false;
    }

    JitSpew(JitSpew_Codegen, "# Emitting lazy link stub");
    lazyLinkStub_ = generateLazyLinkStub(cx);
    if (!lazyLinkStub_)
        return false;

    jitcodeGlobalTable_ = cx->new_<JitcodeGlobalTable>();
    if (!jitcodeGlobalTable_)
        return false;

    return true;
}

JitCode*
JitRuntime::debugTrapHandler(JSContext* cx)
{
    if (!debugTrapHandler_) {
        // JitRuntime code stubs are shared across compartments and have to
        // be allocated in the atoms compartment.
        AutoLockForExclusiveAccess lock(cx);
        AutoCompartment ac(cx, cx->runtime()->atomsCompartment());
        debugTrapHandler_ = generateDebugTrapHandler(cx);
    }
    return debugTrapHandler_;
}

uint8_t*
JitRuntime::allocateOsrTempData(size_t size)
{
    osrTempData_ = (uint8_t*)js_realloc(osrTempData_, size);
    return osrTempData_;
}

void
JitRuntime::freeOsrTempData()
{
    js_free(osrTempData_);
    osrTempData_ = nullptr;
}

void
JitRuntime::patchIonBackedges(JSRuntime* rt, BackedgeTarget target)
{
    MOZ_ASSERT_IF(target == BackedgeLoopHeader, mutatingBackedgeList_);
    MOZ_ASSERT_IF(target == BackedgeInterruptCheck, !mutatingBackedgeList_);

    // Patch all loop backedges in Ion code so that they either jump to the
    // normal loop header or to an interrupt handler each time they run.
    for (InlineListIterator<PatchableBackedge> iter(backedgeList_.begin());
         iter != backedgeList_.end();
         iter++)
    {
        PatchableBackedge* patchableBackedge = *iter;
        if (target == BackedgeLoopHeader)
            PatchBackedge(patchableBackedge->backedge, patchableBackedge->loopHeader, target);
        else
            PatchBackedge(patchableBackedge->backedge, patchableBackedge->interruptCheck, target);
    }
}

JitCompartment::JitCompartment()
  : stubCodes_(nullptr),
    baselineCallReturnAddr_(nullptr),
    baselineGetPropReturnAddr_(nullptr),
    baselineSetPropReturnAddr_(nullptr),
    stringConcatStub_(nullptr),
    regExpExecStub_(nullptr),
    regExpTestStub_(nullptr)
{
}

JitCompartment::~JitCompartment()
{
    js_delete(stubCodes_);
}

bool
JitCompartment::initialize(JSContext* cx)
{
    stubCodes_ = cx->new_<ICStubCodeMap>(cx);
    if (!stubCodes_ || !stubCodes_->init())
        return false;

    return true;
}

bool
JitCompartment::ensureIonStubsExist(JSContext* cx)
{
    if (!stringConcatStub_) {
        stringConcatStub_ = generateStringConcatStub(cx);
        if (!stringConcatStub_)
            return false;
    }

    return true;
}

void
jit::FinishOffThreadBuilder(JSContext* cx, IonBuilder* builder)
{
    // Clean the references to the pending IonBuilder, if we just finished it.
    if (builder->script()->hasIonScript() && builder->script()->pendingIonBuilder() == builder)
        builder->script()->setPendingIonBuilder(cx, nullptr);
    if (builder->isInList())
        builder->remove();

    // Clear the recompiling flag of the old ionScript, since we continue to
    // use the old ionScript if recompiling fails.
    if (builder->script()->hasIonScript())
        builder->script()->ionScript()->clearRecompiling();

    // Clean up if compilation did not succeed.
    if (builder->script()->isIonCompilingOffThread()) {
        builder->script()->setIonScript(cx, builder->abortReason() == AbortReason_Disable
                                            ? ION_DISABLED_SCRIPT
                                            : nullptr);
    }

    // The builder is allocated into its LifoAlloc, so destroying that will
    // destroy the builder and all other data accumulated during compilation,
    // except any final codegen (which includes an assembler and needs to be
    // explicitly destroyed).
    js_delete(builder->backgroundCodegen());
    js_delete(builder->alloc().lifoAlloc());
}

static inline void
FinishAllOffThreadCompilations(JSCompartment* comp)
{
    AutoLockHelperThreadState lock;
    GlobalHelperThreadState::IonBuilderVector& finished = HelperThreadState().ionFinishedList();

    for (size_t i = 0; i < finished.length(); i++) {
        IonBuilder* builder = finished[i];
        if (builder->compartment == CompileCompartment::get(comp)) {
            FinishOffThreadBuilder(nullptr, builder);
            HelperThreadState().remove(finished, &i);
        }
    }
}

uint8_t*
jit::LazyLinkTopActivation(JSContext* cx)
{
    JitActivationIterator iter(cx->runtime());

    // First frame should be an exit frame.
    JitFrameIterator it(iter);
    LazyLinkExitFrameLayout* ll = it.exitFrame()->as<LazyLinkExitFrameLayout>();
    JSScript* calleeScript = ScriptFromCalleeToken(ll->jsFrame()->calleeToken());

    // Get the pending builder from the Ion frame.
    IonBuilder* builder = calleeScript->ionScript()->pendingBuilder();
    calleeScript->setPendingIonBuilder(cx, nullptr);

    AutoEnterAnalysis enterTypes(cx);
    RootedScript script(cx, builder->script());

    // Remove from pending.
    builder->remove();

    if (CodeGenerator* codegen = builder->backgroundCodegen()) {
        js::TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
        TraceLoggerEvent event(logger, TraceLogger_AnnotateScripts, script);
        AutoTraceLog logScript(logger, event);
        AutoTraceLog logLink(logger, TraceLogger_IonLinking);

        JitContext jctx(cx, &builder->alloc());

        // Root the assembler until the builder is finished below. As it
        // was constructed off thread, the assembler has not been rooted
        // previously, though any GC activity would discard the builder.
        codegen->masm.constructRoot(cx);

        if (!codegen->link(cx, builder->constraints())) {
            // Silently ignore OOM during code generation. The assembly code
            // doesn't has code to handle it after linking happened. So it's
            // not OK to throw a catchable exception from there.
            cx->clearPendingException();
        }
    }

    FinishOffThreadBuilder(cx, builder);

    MOZ_ASSERT(script->hasBaselineScript());
    MOZ_ASSERT(script->baselineOrIonRawPointer());

    return script->baselineOrIonRawPointer();
}
/* static */ void
JitRuntime::Mark(JSTracer* trc)
{
    MOZ_ASSERT(!trc->runtime()->isHeapMinorCollecting());
    Zone* zone = trc->runtime()->atomsCompartment()->zone();
    for (gc::ZoneCellIterUnderGC i(zone, gc::FINALIZE_JITCODE); !i.done(); i.next()) {
        JitCode* code = i.get<JitCode>();
        MarkJitCodeRoot(trc, &code, "wrapper");
    }
}

void
JitCompartment::mark(JSTracer* trc, JSCompartment* compartment)
{
    // Free temporary OSR buffer.
    trc->runtime()->jitRuntime()->freeOsrTempData();
}

void
JitCompartment::sweep(FreeOp* fop, JSCompartment* compartment)
{
    // Cancel any active or pending off thread compilations. Note that the
    // MIR graph does not hold any nursery pointers, so there's no need to
    // do this for minor GCs.
    MOZ_ASSERT(!fop->runtime()->isHeapMinorCollecting());
    CancelOffThreadIonCompile(compartment, nullptr);
    FinishAllOffThreadCompilations(compartment);

    stubCodes_->sweep(fop);

    // If the sweep removed the ICCall_Fallback stub, nullptr the baselineCallReturnAddr_ field.
    if (!stubCodes_->lookup(static_cast<uint32_t>(ICStub::Call_Fallback)))
        baselineCallReturnAddr_ = nullptr;
    // Similarly for the ICGetProp_Fallback stub.
    if (!stubCodes_->lookup(static_cast<uint32_t>(ICStub::GetProp_Fallback)))
        baselineGetPropReturnAddr_ = nullptr;
    if (!stubCodes_->lookup(static_cast<uint32_t>(ICStub::SetProp_Fallback)))
        baselineSetPropReturnAddr_ = nullptr;

    if (stringConcatStub_ && !IsJitCodeMarked(&stringConcatStub_))
        stringConcatStub_ = nullptr;

    if (regExpExecStub_ && !IsJitCodeMarked(&regExpExecStub_))
        regExpExecStub_ = nullptr;

    if (regExpTestStub_ && !IsJitCodeMarked(&regExpTestStub_))
        regExpTestStub_ = nullptr;

    for (size_t i = 0; i <= SimdTypeDescr::LAST_TYPE; i++) {
        ReadBarrieredObject& obj = simdTemplateObjects_[i];
        if (obj && IsObjectAboutToBeFinalized(obj.unsafeGet()))
            obj.set(nullptr);
    }
}

void
JitCompartment::toggleBarriers(bool enabled)
{
    // Toggle barriers in compartment wide stubs that have patchable pre barriers.
    if (regExpExecStub_)
        regExpExecStub_->togglePreBarriers(enabled);
    if (regExpTestStub_)
        regExpTestStub_->togglePreBarriers(enabled);

    // Toggle barriers in baseline IC stubs.
    for (ICStubCodeMap::Enum e(*stubCodes_); !e.empty(); e.popFront()) {
        JitCode* code = *e.front().value().unsafeGet();
        code->togglePreBarriers(enabled);
    }
}

JitCode*
JitRuntime::getBailoutTable(const FrameSizeClass& frameClass) const
{
    MOZ_ASSERT(frameClass != FrameSizeClass::None());
    return bailoutTables_[frameClass.classId()];
}

JitCode*
JitRuntime::getVMWrapper(const VMFunction& f) const
{
    MOZ_ASSERT(functionWrappers_);
    MOZ_ASSERT(functionWrappers_->initialized());
    JitRuntime::VMWrapperMap::Ptr p = functionWrappers_->readonlyThreadsafeLookup(&f);
    MOZ_ASSERT(p);

    return p->value();
}

template <AllowGC allowGC>
JitCode*
JitCode::New(JSContext* cx, uint8_t* code, uint32_t bufferSize, uint32_t headerSize,
             ExecutablePool* pool, CodeKind kind)
{
    JitCode* codeObj = js::NewJitCode<allowGC>(cx);
    if (!codeObj) {
        pool->release(headerSize + bufferSize, kind);
        return nullptr;
    }

    new (codeObj) JitCode(code, bufferSize, headerSize, pool, kind);
    return codeObj;
}

template
JitCode*
JitCode::New<CanGC>(JSContext* cx, uint8_t* code, uint32_t bufferSize, uint32_t headerSize,
                    ExecutablePool* pool, CodeKind kind);

template
JitCode*
JitCode::New<NoGC>(JSContext* cx, uint8_t* code, uint32_t bufferSize, uint32_t headerSize,
                   ExecutablePool* pool, CodeKind kind);

void
JitCode::copyFrom(MacroAssembler& masm)
{
    // Store the JitCode pointer right before the code buffer, so we can
    // recover the gcthing from relocation tables.
    *(JitCode**)(code_ - sizeof(JitCode*)) = this;
    insnSize_ = masm.instructionsSize();
    masm.executableCopy(code_);

    jumpRelocTableBytes_ = masm.jumpRelocationTableBytes();
    masm.copyJumpRelocationTable(code_ + jumpRelocTableOffset());

    dataRelocTableBytes_ = masm.dataRelocationTableBytes();
    masm.copyDataRelocationTable(code_ + dataRelocTableOffset());

    preBarrierTableBytes_ = masm.preBarrierTableBytes();
    masm.copyPreBarrierTable(code_ + preBarrierTableOffset());

    masm.processCodeLabels(code_);
}

void
JitCode::trace(JSTracer* trc)
{
    // Note that we cannot mark invalidated scripts, since we've basically
    // corrupted the code stream by injecting bailouts.
    if (invalidated())
        return;

    if (jumpRelocTableBytes_) {
        uint8_t* start = code_ + jumpRelocTableOffset();
        CompactBufferReader reader(start, start + jumpRelocTableBytes_);
        MacroAssembler::TraceJumpRelocations(trc, this, reader);
    }
    if (dataRelocTableBytes_) {
        uint8_t* start = code_ + dataRelocTableOffset();
        CompactBufferReader reader(start, start + dataRelocTableBytes_);
        MacroAssembler::TraceDataRelocations(trc, this, reader);
    }
}

void
JitCode::fixupNurseryObjects(JSContext* cx, const ObjectVector& nurseryObjects)
{
    if (nurseryObjects.empty() || !dataRelocTableBytes_)
        return;

    uint8_t* start = code_ + dataRelocTableOffset();
    CompactBufferReader reader(start, start + dataRelocTableBytes_);
    MacroAssembler::FixupNurseryObjects(cx, this, reader, nurseryObjects);
}

void
JitCode::finalize(FreeOp* fop)
{
    JSRuntime* rt = fop->runtime();

    // If this jitcode has a bytecode map, de-register it.
    if (hasBytecodeMap_) {
        MOZ_ASSERT(rt->jitRuntime()->hasJitcodeGlobalTable());
        rt->jitRuntime()->getJitcodeGlobalTable()->removeEntry(raw(), rt);
    }

    // Buffer can be freed at any time hereafter. Catch use-after-free bugs.
    // Don't do this if the Ion code is protected, as the signal handler will
    // deadlock trying to reacquire the interrupt lock.
    memset(code_, JS_SWEPT_CODE_PATTERN, bufferSize_);
    code_ = nullptr;

    // Code buffers are stored inside JSC pools.
    // Pools are refcounted. Releasing the pool may free it.
    if (pool_) {
        // Horrible hack: if we are using perf integration, we don't
        // want to reuse code addresses, so we just leak the memory instead.
        if (!PerfEnabled())
            pool_->release(headerSize_ + bufferSize_, CodeKind(kind_));
        pool_ = nullptr;
    }
}

void
JitCode::togglePreBarriers(bool enabled)
{
    uint8_t* start = code_ + preBarrierTableOffset();
    CompactBufferReader reader(start, start + preBarrierTableBytes_);

    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        CodeLocationLabel loc(this, CodeOffsetLabel(offset));
        if (enabled)
            Assembler::ToggleToCmp(loc);
        else
            Assembler::ToggleToJmp(loc);
    }
}

IonScript::IonScript()
  : method_(nullptr),
    deoptTable_(nullptr),
    osrPc_(nullptr),
    osrEntryOffset_(0),
    skipArgCheckEntryOffset_(0),
    invalidateEpilogueOffset_(0),
    invalidateEpilogueDataOffset_(0),
    numBailouts_(0),
    hasProfilingInstrumentation_(false),
    recompiling_(false),
    runtimeData_(0),
    runtimeSize_(0),
    cacheIndex_(0),
    cacheEntries_(0),
    safepointIndexOffset_(0),
    safepointIndexEntries_(0),
    safepointsStart_(0),
    safepointsSize_(0),
    frameSlots_(0),
    frameSize_(0),
    bailoutTable_(0),
    bailoutEntries_(0),
    osiIndexOffset_(0),
    osiIndexEntries_(0),
    snapshots_(0),
    snapshotsListSize_(0),
    snapshotsRVATableSize_(0),
    constantTable_(0),
    constantEntries_(0),
    backedgeList_(0),
    backedgeEntries_(0),
    invalidationCount_(0),
    recompileInfo_(),
    osrPcMismatchCounter_(0),
    pendingBuilder_(nullptr)
{
}

IonScript*
IonScript::New(JSContext* cx, RecompileInfo recompileInfo,
               uint32_t frameSlots, uint32_t argumentSlots, uint32_t frameSize,
               size_t snapshotsListSize, size_t snapshotsRVATableSize,
               size_t recoversSize, size_t bailoutEntries,
               size_t constants, size_t safepointIndices,
               size_t osiIndices, size_t cacheEntries,
               size_t runtimeSize,  size_t safepointsSize,
               size_t backedgeEntries, OptimizationLevel optimizationLevel)
{
    static const int DataAlignment = sizeof(void*);

    if (snapshotsListSize >= MAX_BUFFER_SIZE ||
        (bailoutEntries >= MAX_BUFFER_SIZE / sizeof(uint32_t)))
    {
        js_ReportOutOfMemory(cx);
        return nullptr;
    }

    // This should not overflow on x86, because the memory is already allocated
    // *somewhere* and if their total overflowed there would be no memory left
    // at all.
    size_t paddedSnapshotsSize = AlignBytes(snapshotsListSize + snapshotsRVATableSize, DataAlignment);
    size_t paddedRecoversSize = AlignBytes(recoversSize, DataAlignment);
    size_t paddedBailoutSize = AlignBytes(bailoutEntries * sizeof(uint32_t), DataAlignment);
    size_t paddedConstantsSize = AlignBytes(constants * sizeof(Value), DataAlignment);
    size_t paddedSafepointIndicesSize = AlignBytes(safepointIndices * sizeof(SafepointIndex), DataAlignment);
    size_t paddedOsiIndicesSize = AlignBytes(osiIndices * sizeof(OsiIndex), DataAlignment);
    size_t paddedCacheEntriesSize = AlignBytes(cacheEntries * sizeof(uint32_t), DataAlignment);
    size_t paddedRuntimeSize = AlignBytes(runtimeSize, DataAlignment);
    size_t paddedSafepointSize = AlignBytes(safepointsSize, DataAlignment);
    size_t paddedBackedgeSize = AlignBytes(backedgeEntries * sizeof(PatchableBackedge), DataAlignment);
    size_t bytes = paddedSnapshotsSize +
                   paddedRecoversSize +
                   paddedBailoutSize +
                   paddedConstantsSize +
                   paddedSafepointIndicesSize +
                   paddedOsiIndicesSize +
                   paddedCacheEntriesSize +
                   paddedRuntimeSize +
                   paddedSafepointSize +
                   paddedBackedgeSize;
    IonScript* script = cx->zone()->pod_malloc_with_extra<IonScript, uint8_t>(bytes);
    if (!script)
        return nullptr;
    new (script) IonScript();

    uint32_t offsetCursor = sizeof(IonScript);

    script->runtimeData_ = offsetCursor;
    script->runtimeSize_ = runtimeSize;
    offsetCursor += paddedRuntimeSize;

    script->cacheIndex_ = offsetCursor;
    script->cacheEntries_ = cacheEntries;
    offsetCursor += paddedCacheEntriesSize;

    script->safepointIndexOffset_ = offsetCursor;
    script->safepointIndexEntries_ = safepointIndices;
    offsetCursor += paddedSafepointIndicesSize;

    script->safepointsStart_ = offsetCursor;
    script->safepointsSize_ = safepointsSize;
    offsetCursor += paddedSafepointSize;

    script->bailoutTable_ = offsetCursor;
    script->bailoutEntries_ = bailoutEntries;
    offsetCursor += paddedBailoutSize;

    script->osiIndexOffset_ = offsetCursor;
    script->osiIndexEntries_ = osiIndices;
    offsetCursor += paddedOsiIndicesSize;

    script->snapshots_ = offsetCursor;
    script->snapshotsListSize_ = snapshotsListSize;
    script->snapshotsRVATableSize_ = snapshotsRVATableSize;
    offsetCursor += paddedSnapshotsSize;

    script->recovers_ = offsetCursor;
    script->recoversSize_ = recoversSize;
    offsetCursor += paddedRecoversSize;

    script->constantTable_ = offsetCursor;
    script->constantEntries_ = constants;
    offsetCursor += paddedConstantsSize;

    script->backedgeList_ = offsetCursor;
    script->backedgeEntries_ = backedgeEntries;
    offsetCursor += paddedBackedgeSize;

    script->frameSlots_ = frameSlots;
    script->argumentSlots_ = argumentSlots;

    script->frameSize_ = frameSize;

    script->recompileInfo_ = recompileInfo;
    script->optimizationLevel_ = optimizationLevel;

    return script;
}

void
IonScript::trace(JSTracer* trc)
{
    if (method_)
        MarkJitCode(trc, &method_, "method");

    if (deoptTable_)
        MarkJitCode(trc, &deoptTable_, "deoptimizationTable");

    for (size_t i = 0; i < numConstants(); i++)
        gc::MarkValue(trc, &getConstant(i), "constant");
}

/* static */ void
IonScript::writeBarrierPre(Zone* zone, IonScript* ionScript)
{
    if (zone->needsIncrementalBarrier())
        ionScript->trace(zone->barrierTracer());
}

void
IonScript::copySnapshots(const SnapshotWriter* writer)
{
    MOZ_ASSERT(writer->listSize() == snapshotsListSize_);
    memcpy((uint8_t*)this + snapshots_,
           writer->listBuffer(), snapshotsListSize_);

    MOZ_ASSERT(snapshotsRVATableSize_);
    MOZ_ASSERT(writer->RVATableSize() == snapshotsRVATableSize_);
    memcpy((uint8_t*)this + snapshots_ + snapshotsListSize_,
           writer->RVATableBuffer(), snapshotsRVATableSize_);
}

void
IonScript::copyRecovers(const RecoverWriter* writer)
{
    MOZ_ASSERT(writer->size() == recoversSize_);
    memcpy((uint8_t*)this + recovers_, writer->buffer(), recoversSize_);
}

void
IonScript::copySafepoints(const SafepointWriter* writer)
{
    MOZ_ASSERT(writer->size() == safepointsSize_);
    memcpy((uint8_t*)this + safepointsStart_, writer->buffer(), safepointsSize_);
}

void
IonScript::copyBailoutTable(const SnapshotOffset* table)
{
    memcpy(bailoutTable(), table, bailoutEntries_ * sizeof(uint32_t));
}

void
IonScript::copyConstants(const Value* vp)
{
    for (size_t i = 0; i < constantEntries_; i++)
        constants()[i].init(vp[i]);
}

void
IonScript::copyPatchableBackedges(JSContext* cx, JitCode* code,
                                  PatchableBackedgeInfo* backedges,
                                  MacroAssembler& masm)
{
    JitRuntime* jrt = cx->runtime()->jitRuntime();
    JitRuntime::AutoMutateBackedges amb(jrt);

    for (size_t i = 0; i < backedgeEntries_; i++) {
        PatchableBackedgeInfo& info = backedges[i];
        PatchableBackedge* patchableBackedge = &backedgeList()[i];

        // Convert to actual offsets for the benefit of the ARM backend.
        info.backedge.fixup(&masm);
        uint32_t loopHeaderOffset = masm.actualOffset(info.loopHeader->offset());
        uint32_t interruptCheckOffset = masm.actualOffset(info.interruptCheck->offset());

        CodeLocationJump backedge(code, info.backedge);
        CodeLocationLabel loopHeader(code, CodeOffsetLabel(loopHeaderOffset));
        CodeLocationLabel interruptCheck(code, CodeOffsetLabel(interruptCheckOffset));
        new(patchableBackedge) PatchableBackedge(backedge, loopHeader, interruptCheck);

        // Point the backedge to either of its possible targets, according to
        // whether an interrupt is currently desired, matching the targets
        // established by ensureIonCodeAccessible() above. We don't handle the
        // interrupt immediately as the interrupt lock is held here.
        if (cx->runtime()->hasPendingInterrupt())
            PatchBackedge(backedge, interruptCheck, JitRuntime::BackedgeInterruptCheck);
        else
            PatchBackedge(backedge, loopHeader, JitRuntime::BackedgeLoopHeader);

        jrt->addPatchableBackedge(patchableBackedge);
    }
}

void
IonScript::copySafepointIndices(const SafepointIndex* si, MacroAssembler& masm)
{
    // Jumps in the caches reflect the offset of those jumps in the compiled
    // code, not the absolute positions of the jumps. Update according to the
    // final code address now.
    SafepointIndex* table = safepointIndices();
    memcpy(table, si, safepointIndexEntries_ * sizeof(SafepointIndex));
    for (size_t i = 0; i < safepointIndexEntries_; i++)
        table[i].adjustDisplacement(masm.actualOffset(table[i].displacement()));
}

void
IonScript::copyOsiIndices(const OsiIndex* oi, MacroAssembler& masm)
{
    memcpy(osiIndices(), oi, osiIndexEntries_ * sizeof(OsiIndex));
    for (unsigned i = 0; i < osiIndexEntries_; i++)
        osiIndices()[i].fixUpOffset(masm);
}

void
IonScript::copyRuntimeData(const uint8_t* data)
{
    memcpy(runtimeData(), data, runtimeSize());
}

void
IonScript::copyCacheEntries(const uint32_t* caches, MacroAssembler& masm)
{
    memcpy(cacheIndex(), caches, numCaches() * sizeof(uint32_t));

    // Jumps in the caches reflect the offset of those jumps in the compiled
    // code, not the absolute positions of the jumps. Update according to the
    // final code address now.
    for (size_t i = 0; i < numCaches(); i++)
        getCacheFromIndex(i).updateBaseAddress(method_, masm);
}

const SafepointIndex*
IonScript::getSafepointIndex(uint32_t disp) const
{
    MOZ_ASSERT(safepointIndexEntries_ > 0);

    const SafepointIndex* table = safepointIndices();
    if (safepointIndexEntries_ == 1) {
        MOZ_ASSERT(disp == table[0].displacement());
        return &table[0];
    }

    size_t minEntry = 0;
    size_t maxEntry = safepointIndexEntries_ - 1;
    uint32_t min = table[minEntry].displacement();
    uint32_t max = table[maxEntry].displacement();

    // Raise if the element is not in the list.
    MOZ_ASSERT(min <= disp && disp <= max);

    // Approximate the location of the FrameInfo.
    size_t guess = (disp - min) * (maxEntry - minEntry) / (max - min) + minEntry;
    uint32_t guessDisp = table[guess].displacement();

    if (table[guess].displacement() == disp)
        return &table[guess];

    // Doing a linear scan from the guess should be more efficient in case of
    // small group which are equally distributed on the code.
    //
    // such as:  <...      ...    ...  ...  .   ...    ...>
    if (guessDisp > disp) {
        while (--guess >= minEntry) {
            guessDisp = table[guess].displacement();
            MOZ_ASSERT(guessDisp >= disp);
            if (guessDisp == disp)
                return &table[guess];
        }
    } else {
        while (++guess <= maxEntry) {
            guessDisp = table[guess].displacement();
            MOZ_ASSERT(guessDisp <= disp);
            if (guessDisp == disp)
                return &table[guess];
        }
    }

    MOZ_CRASH("displacement not found.");
}

const OsiIndex*
IonScript::getOsiIndex(uint32_t disp) const
{
    for (const OsiIndex* it = osiIndices(), *end = osiIndices() + osiIndexEntries_;
         it != end;
         ++it)
    {
        if (it->returnPointDisplacement() == disp)
            return it;
    }

    MOZ_CRASH("Failed to find OSI point return address");
}

const OsiIndex*
IonScript::getOsiIndex(uint8_t* retAddr) const
{
    JitSpew(JitSpew_IonInvalidate, "IonScript %p has method %p raw %p", (void*) this, (void*)
            method(), method()->raw());

    MOZ_ASSERT(containsCodeAddress(retAddr));
    uint32_t disp = retAddr - method()->raw();
    return getOsiIndex(disp);
}

void
IonScript::Trace(JSTracer* trc, IonScript* script)
{
    if (script != ION_DISABLED_SCRIPT)
        script->trace(trc);
}

void
IonScript::Destroy(FreeOp* fop, IonScript* script)
{
    if (script->pendingBuilder())
        jit::FinishOffThreadBuilder(nullptr, script->pendingBuilder());

    script->destroyCaches();
    script->unlinkFromRuntime(fop);
    fop->free_(script);
}

void
IonScript::toggleBarriers(bool enabled)
{
    method()->togglePreBarriers(enabled);
}

void
IonScript::purgeCaches()
{
    // Don't reset any ICs if we're invalidated, otherwise, repointing the
    // inline jump could overwrite an invalidation marker. These ICs can
    // no longer run, however, the IC slow paths may be active on the stack.
    // ICs therefore are required to check for invalidation before patching,
    // to ensure the same invariant.
    if (invalidated())
        return;

    for (size_t i = 0; i < numCaches(); i++)
        getCacheFromIndex(i).reset();
}

void
IonScript::destroyCaches()
{
    for (size_t i = 0; i < numCaches(); i++)
        getCacheFromIndex(i).destroy();
}

void
IonScript::unlinkFromRuntime(FreeOp* fop)
{
    // The writes to the executable buffer below may clobber backedge jumps, so
    // make sure that those backedges are unlinked from the runtime and not
    // reclobbered with garbage if an interrupt is requested.
    JitRuntime* jrt = fop->runtime()->jitRuntime();
    JitRuntime::AutoMutateBackedges amb(jrt);
    for (size_t i = 0; i < backedgeEntries_; i++)
        jrt->removePatchableBackedge(&backedgeList()[i]);

    // Clear the list of backedges, so that this method is idempotent. It is
    // called during destruction, and may be additionally called when the
    // script is invalidated.
    backedgeEntries_ = 0;
}

void
jit::ToggleBarriers(JS::Zone* zone, bool needs)
{
    JSRuntime* rt = zone->runtimeFromMainThread();
    if (!rt->hasJitRuntime())
        return;

    for (gc::ZoneCellIterUnderGC i(zone, gc::FINALIZE_SCRIPT); !i.done(); i.next()) {
        JSScript* script = i.get<JSScript>();
        if (script->hasIonScript())
            script->ionScript()->toggleBarriers(needs);
        if (script->hasBaselineScript())
            script->baselineScript()->toggleBarriers(needs);
    }

    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
        if (comp->jitCompartment())
            comp->jitCompartment()->toggleBarriers(needs);
    }
}

namespace js {
namespace jit {

bool
OptimizeMIR(MIRGenerator* mir)
{
    MIRGraph& graph = mir->graph();
    TraceLoggerThread* logger;
    if (GetJitContext()->runtime->onMainThread())
        logger = TraceLoggerForMainThread(GetJitContext()->runtime);
    else
        logger = TraceLoggerForCurrentThread();

    if (!mir->compilingAsmJS()) {
        if (!MakeMRegExpHoistable(graph))
            return false;
    }

    IonSpewPass("BuildSSA");
    AssertBasicGraphCoherency(graph);

    if (mir->shouldCancel("Start"))
        return false;

    if (!mir->compilingAsmJS()) {
        AutoTraceLog log(logger, TraceLogger_FoldTests);
        FoldTests(graph);
        IonSpewPass("Fold Tests");
        AssertBasicGraphCoherency(graph);

        if (mir->shouldCancel("Fold Tests"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_SplitCriticalEdges);
        if (!SplitCriticalEdges(graph))
            return false;
        IonSpewPass("Split Critical Edges");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("Split Critical Edges"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_RenumberBlocks);
        if (!RenumberBlocks(graph))
            return false;
        IonSpewPass("Renumber Blocks");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("Renumber Blocks"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_DominatorTree);
        if (!BuildDominatorTree(graph))
            return false;
        // No spew: graph not changed.

        if (mir->shouldCancel("Dominator Tree"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_PhiAnalysis);
        // Aggressive phi elimination must occur before any code elimination. If the
        // script contains a try-statement, we only compiled the try block and not
        // the catch or finally blocks, so in this case it's also invalid to use
        // aggressive phi elimination.
        Observability observability = graph.hasTryBlock()
                                      ? ConservativeObservability
                                      : AggressiveObservability;
        if (!EliminatePhis(mir, graph, observability))
            return false;
        IonSpewPass("Eliminate phis");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("Eliminate phis"))
            return false;

        if (!BuildPhiReverseMapping(graph))
            return false;
        AssertExtendedGraphCoherency(graph);
        // No spew: graph not changed.

        if (mir->shouldCancel("Phi reverse mapping"))
            return false;
    }

    if (mir->optimizationInfo().scalarReplacementEnabled()) {
        AutoTraceLog log(logger, TraceLogger_ScalarReplacement);
        if (!ScalarReplacement(mir, graph))
            return false;
        IonSpewPass("Scalar Replacement");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("Scalar Replacement"))
            return false;
    }

    if (!mir->compilingAsmJS()) {
        AutoTraceLog log(logger, TraceLogger_ApplyTypes);
        if (!ApplyTypeInformation(mir, graph))
            return false;
        IonSpewPass("Apply types");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Apply types"))
            return false;
    }

    ValueNumberer gvn(mir, graph);
    if (!gvn.init())
        return false;

    // Alias analysis is required for LICM and GVN so that we don't move
    // loads across stores.
    if (mir->optimizationInfo().licmEnabled() ||
        mir->optimizationInfo().gvnEnabled())
    {
        AutoTraceLog log(logger, TraceLogger_AliasAnalysis);
        AliasAnalysis analysis(mir, graph);
        if (!analysis.analyze())
            return false;
        IonSpewPass("Alias analysis");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Alias analysis"))
            return false;

        if (!mir->compilingAsmJS()) {
            // Eliminating dead resume point operands requires basic block
            // instructions to be numbered. Reuse the numbering computed during
            // alias analysis.
            if (!EliminateDeadResumePointOperands(mir, graph))
                return false;

            if (mir->shouldCancel("Eliminate dead resume point operands"))
                return false;
        }
    }

    if (mir->optimizationInfo().gvnEnabled()) {
        AutoTraceLog log(logger, TraceLogger_GVN);
        if (!gvn.run(ValueNumberer::UpdateAliasAnalysis))
            return false;
        IonSpewPass("GVN");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("GVN"))
            return false;
    }

    if (mir->optimizationInfo().licmEnabled()) {
        AutoTraceLog log(logger, TraceLogger_LICM);
        // LICM can hoist instructions from conditional branches and trigger
        // repeated bailouts. Disable it if this script is known to bailout
        // frequently.
        JSScript* script = mir->info().script();
        if (!script || !script->hadFrequentBailouts()) {
            if (!LICM(mir, graph))
                return false;
            IonSpewPass("LICM");
            AssertExtendedGraphCoherency(graph);

            if (mir->shouldCancel("LICM"))
                return false;
        }
    }

    if (mir->optimizationInfo().rangeAnalysisEnabled()) {
        AutoTraceLog log(logger, TraceLogger_RangeAnalysis);
        RangeAnalysis r(mir, graph);
        if (!r.addBetaNodes())
            return false;
        IonSpewPass("Beta");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("RA Beta"))
            return false;

        if (!r.analyze() || !r.addRangeAssertions())
            return false;
        IonSpewPass("Range Analysis");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Range Analysis"))
            return false;

        if (!r.removeBetaNodes())
            return false;
        IonSpewPass("De-Beta");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("RA De-Beta"))
            return false;

        if (mir->optimizationInfo().gvnEnabled()) {
            bool shouldRunUCE = false;
            if (!r.prepareForUCE(&shouldRunUCE))
                return false;
            IonSpewPass("RA check UCE");
            AssertExtendedGraphCoherency(graph);

            if (mir->shouldCancel("RA check UCE"))
                return false;

            if (shouldRunUCE) {
                if (!gvn.run(ValueNumberer::DontUpdateAliasAnalysis))
                    return false;
                IonSpewPass("UCE After RA");
                AssertExtendedGraphCoherency(graph);

                if (mir->shouldCancel("UCE After RA"))
                    return false;
            }
        }

        if (mir->optimizationInfo().autoTruncateEnabled()) {
            if (!r.truncate())
                return false;
            IonSpewPass("Truncate Doubles");
            AssertExtendedGraphCoherency(graph);

            if (mir->shouldCancel("Truncate Doubles"))
                return false;
        }

        if (mir->optimizationInfo().loopUnrollingEnabled()) {
            AutoTraceLog log(logger, TraceLogger_LoopUnrolling);

            if (!UnrollLoops(graph, r.loopIterationBounds))
                return false;

            IonSpewPass("Unroll Loops");
            AssertExtendedGraphCoherency(graph);
        }
    }

    if (mir->optimizationInfo().eaaEnabled()) {
        AutoTraceLog log(logger, TraceLogger_EffectiveAddressAnalysis);
        EffectiveAddressAnalysis eaa(graph);
        if (!eaa.analyze())
            return false;
        IonSpewPass("Effective Address Analysis");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Effective Address Analysis"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_EliminateDeadCode);
        if (!EliminateDeadCode(mir, graph))
            return false;
        IonSpewPass("DCE");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("DCE"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_EliminateDeadCode);
        if (!Sink(mir, graph))
            return false;
        IonSpewPass("Sink");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Sink"))
            return false;
    }

    // Make loops contiguous. We do this after GVN/UCE and range analysis,
    // which can remove CFG edges, exposing more blocks that can be moved.
    {
        AutoTraceLog log(logger, TraceLogger_MakeLoopsContiguous);
        if (!MakeLoopsContiguous(graph))
            return false;
        IonSpewPass("Make loops contiguous");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Make loops contiguous"))
            return false;
    }

    // Passes after this point must not move instructions; these analyses
    // depend on knowing the final order in which instructions will execute.

    if (mir->optimizationInfo().edgeCaseAnalysisEnabled()) {
        AutoTraceLog log(logger, TraceLogger_EdgeCaseAnalysis);
        EdgeCaseAnalysis edgeCaseAnalysis(mir, graph);
        if (!edgeCaseAnalysis.analyzeLate())
            return false;
        IonSpewPass("Edge Case Analysis (Late)");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("Edge Case Analysis (Late)"))
            return false;
    }

    if (mir->optimizationInfo().eliminateRedundantChecksEnabled()) {
        AutoTraceLog log(logger, TraceLogger_EliminateRedundantChecks);
        // Note: check elimination has to run after all other passes that move
        // instructions. Since check uses are replaced with the actual index,
        // code motion after this pass could incorrectly move a load or store
        // before its bounds check.
        if (!EliminateRedundantChecks(graph))
            return false;
        IonSpewPass("Bounds Check Elimination");
        AssertGraphCoherency(graph);
    }

    return true;
}

LIRGraph*
GenerateLIR(MIRGenerator* mir)
{
    MIRGraph& graph = mir->graph();

    TraceLoggerThread* logger;
    if (GetJitContext()->runtime->onMainThread())
        logger = TraceLoggerForMainThread(GetJitContext()->runtime);
    else
        logger = TraceLoggerForCurrentThread();

    LIRGraph* lir = mir->alloc().lifoAlloc()->new_<LIRGraph>(&graph);
    if (!lir || !lir->init())
        return nullptr;

    LIRGenerator lirgen(mir, graph, *lir);
    {
        AutoTraceLog log(logger, TraceLogger_GenerateLIR);
        if (!lirgen.generate())
            return nullptr;
        IonSpewPass("Generate LIR");

        if (mir->shouldCancel("Generate LIR"))
            return nullptr;
    }

    AllocationIntegrityState integrity(*lir);

    {
        AutoTraceLog log(logger, TraceLogger_RegisterAllocation);

        switch (mir->optimizationInfo().registerAllocator()) {
          case RegisterAllocator_LSRA: {
#ifdef DEBUG
            if (!integrity.record())
                return nullptr;
#endif

            LinearScanAllocator regalloc(mir, &lirgen, *lir);
            if (!regalloc.go())
                return nullptr;

#ifdef DEBUG
            if (!integrity.check(false))
                return nullptr;
#endif

            IonSpewPass("Allocate Registers [LSRA]", &regalloc);
            break;
          }

          case RegisterAllocator_Backtracking: {
#ifdef DEBUG
            if (!integrity.record())
                return nullptr;
#endif

            BacktrackingAllocator regalloc(mir, &lirgen, *lir);
            if (!regalloc.go())
                return nullptr;

#ifdef DEBUG
            if (!integrity.check(false))
                return nullptr;
#endif

            IonSpewPass("Allocate Registers [Backtracking]");
            break;
          }

          case RegisterAllocator_Stupid: {
            // Use the integrity checker to populate safepoint information, so
            // run it in all builds.
            if (!integrity.record())
                return nullptr;

            StupidAllocator regalloc(mir, &lirgen, *lir);
            if (!regalloc.go())
                return nullptr;
            if (!integrity.check(true))
                return nullptr;
            IonSpewPass("Allocate Registers [Stupid]");
            break;
          }

          default:
            MOZ_CRASH("Bad regalloc");
        }

        if (mir->shouldCancel("Allocate Registers"))
            return nullptr;
    }

    return lir;
}

CodeGenerator*
GenerateCode(MIRGenerator* mir, LIRGraph* lir)
{
    TraceLoggerThread* logger;
    if (GetJitContext()->runtime->onMainThread())
        logger = TraceLoggerForMainThread(GetJitContext()->runtime);
    else
        logger = TraceLoggerForCurrentThread();
    AutoTraceLog log(logger, TraceLogger_GenerateCode);

    CodeGenerator* codegen = js_new<CodeGenerator>(mir, lir);
    if (!codegen)
        return nullptr;

    if (!codegen->generate()) {
        js_delete(codegen);
        return nullptr;
    }

    return codegen;
}

CodeGenerator*
CompileBackEnd(MIRGenerator* mir)
{
    // Everything in CompileBackEnd can potentially run on a helper thread.
    AutoEnterIonCompilation enter;

    if (!OptimizeMIR(mir))
        return nullptr;

    LIRGraph* lir = GenerateLIR(mir);
    if (!lir)
        return nullptr;

    return GenerateCode(mir, lir);
}

void
AttachFinishedCompilations(JSContext* cx)
{
    JitCompartment* ion = cx->compartment()->jitCompartment();
    if (!ion)
        return;

    AutoEnterAnalysis enterTypes(cx);
    AutoLockHelperThreadState lock;

    GlobalHelperThreadState::IonBuilderVector& finished = HelperThreadState().ionFinishedList();

    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());

    // Incorporate any off thread compilations for the compartment which have
    // finished, failed or have been cancelled.
    while (true) {
        IonBuilder* builder = nullptr;

        // Find a finished builder for the compartment.
        for (size_t i = 0; i < finished.length(); i++) {
            IonBuilder* testBuilder = finished[i];
            if (testBuilder->compartment == CompileCompartment::get(cx->compartment())) {
                builder = testBuilder;
                HelperThreadState().remove(finished, &i);
                break;
            }
        }
        if (!builder)
            break;

        // Try to defer linking if the script is on the stack, to postpone
        // invalidating them.
        if (builder->script()->hasIonScript()) {
            bool onStack = false;
            for (JitActivationIterator iter(cx->runtime()); !iter.done(); ++iter) {
                for (JitFrameIterator it(iter); !it.done(); ++it) {
                    if (!it.isIonJS())
                        continue;
                    if (it.checkInvalidation())
                        continue;

                    JSScript* script = it.script();
                    if (builder->script() == script) {
                        onStack = true;
                        break;
                    }
                }
                if (onStack)
                    break;
            }

            if (onStack) {
                builder->script()->setPendingIonBuilder(cx, builder);
                HelperThreadState().ionLazyLinkList().insertFront(builder);
                continue;
            }
        }

        if (CodeGenerator* codegen = builder->backgroundCodegen()) {
            RootedScript script(cx, builder->script());
            JitContext jctx(cx, &builder->alloc());
            TraceLoggerEvent event(logger, TraceLogger_AnnotateScripts, script);
            AutoTraceLog logScript(logger, event);
            AutoTraceLog logLink(logger, TraceLogger_IonLinking);

            // Root the assembler until the builder is finished below. As it
            // was constructed off thread, the assembler has not been rooted
            // previously, though any GC activity would discard the builder.
            codegen->masm.constructRoot(cx);

            bool success;
            {
                AutoUnlockHelperThreadState unlock;
                success = codegen->link(cx, builder->constraints());
            }

            if (!success) {
                // Silently ignore OOM during code generation. The caller is
                // InvokeInterruptCallback, which always runs at a
                // nondeterministic time. It's not OK to throw a catchable
                // exception from there.
                cx->clearPendingException();
            }
        }

        FinishOffThreadBuilder(cx, builder);
    }
}

void
MIRGenerator::traceNurseryObjects(JSTracer* trc)
{
    MarkObjectRootRange(trc, nurseryObjects_.length(), nurseryObjects_.begin(), "ion-nursery-objects");
}

class MarkOffThreadNurseryObjects : public gc::BufferableRef
{
  public:
    void mark(JSTracer* trc);
};

void
MarkOffThreadNurseryObjects::mark(JSTracer* trc)
{
    JSRuntime* rt = trc->runtime();

    MOZ_ASSERT(rt->jitRuntime()->hasIonNurseryObjects());
    rt->jitRuntime()->setHasIonNurseryObjects(false);

    AutoLockHelperThreadState lock;
    if (!HelperThreadState().threads)
        return;

    // Trace nursery objects of any builders which haven't started yet.
    GlobalHelperThreadState::IonBuilderVector& worklist = HelperThreadState().ionWorklist();
    for (size_t i = 0; i < worklist.length(); i++) {
        jit::IonBuilder* builder = worklist[i];
        if (builder->script()->runtimeFromAnyThread() == rt)
            builder->traceNurseryObjects(trc);
    }

    // Trace nursery objects of in-progress entries.
    for (size_t i = 0; i < HelperThreadState().threadCount; i++) {
        HelperThread& helper = HelperThreadState().threads[i];
        if (helper.ionBuilder && helper.ionBuilder->script()->runtimeFromAnyThread() == rt)
            helper.ionBuilder->traceNurseryObjects(trc);
    }

    // Trace nursery objects of any completed entries.
    GlobalHelperThreadState::IonBuilderVector& finished = HelperThreadState().ionFinishedList();
    for (size_t i = 0; i < finished.length(); i++) {
        jit::IonBuilder* builder = finished[i];
        if (builder->script()->runtimeFromAnyThread() == rt)
            builder->traceNurseryObjects(trc);
    }

    // Trace nursery objects of lazy-linked builders.
    jit::IonBuilder* builder = HelperThreadState().ionLazyLinkList().getFirst();
    while (builder) {
        if (builder->script()->runtimeFromAnyThread() == rt)
            builder->traceNurseryObjects(trc);
        builder = builder->getNext();
    }
}

static inline bool
OffThreadCompilationAvailable(JSContext* cx)
{
    // Even if off thread compilation is enabled, compilation must still occur
    // on the main thread in some cases.
    //
    // Require cpuCount > 1 so that Ion compilation jobs and main-thread
    // execution are not competing for the same resources.
    return cx->runtime()->canUseOffthreadIonCompilation()
        && HelperThreadState().cpuCount > 1
        && CanUseExtraThreads();
}

static void
TrackAllProperties(JSContext* cx, JSObject* obj)
{
    MOZ_ASSERT(obj->isSingleton());

    for (Shape::Range<NoGC> range(obj->lastProperty()); !range.empty(); range.popFront())
        EnsureTrackPropertyTypes(cx, obj, range.front().propid());
}

static void
TrackPropertiesForSingletonScopes(JSContext* cx, JSScript* script, BaselineFrame* baselineFrame)
{
    // Ensure that all properties of singleton call objects which the script
    // could access are tracked. These are generally accessed through
    // ALIASEDVAR operations in baseline and will not be tracked even if they
    // have been accessed in baseline code.
    JSObject* environment = script->functionNonDelazifying()
                            ? script->functionNonDelazifying()->environment()
                            : nullptr;

    while (environment && !environment->is<GlobalObject>()) {
        if (environment->is<CallObject>() && environment->isSingleton())
            TrackAllProperties(cx, environment);
        environment = environment->enclosingScope();
    }

    if (baselineFrame) {
        JSObject* scope = baselineFrame->scopeChain();
        if (scope->is<CallObject>() && scope->isSingleton())
            TrackAllProperties(cx, scope);
    }
}

static void
TrackIonAbort(JSContext* cx, JSScript* script, jsbytecode* pc, const char* message)
{
    if (!cx->runtime()->jitRuntime()->isOptimizationTrackingEnabled(cx->runtime()))
        return;

    // Only bother tracking aborts of functions we're attempting to
    // Ion-compile after successfully running in Baseline.
    if (!script->hasBaselineScript())
        return;

    JitcodeGlobalTable* table = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    JitcodeGlobalEntry entry;
    table->lookupInfallible(script->baselineScript()->method()->raw(), &entry, cx->runtime());
    entry.baselineEntry().trackIonAbort(pc, message);
}

static void
TrackAndSpewIonAbort(JSContext* cx, JSScript* script, const char* message)
{
    JitSpew(JitSpew_IonAbort, message);
    TrackIonAbort(cx, script, script->code(), message);
}

static AbortReason
IonCompile(JSContext* cx, JSScript* script,
           BaselineFrame* baselineFrame, jsbytecode* osrPc, bool constructing,
           bool recompile, OptimizationLevel optimizationLevel)
{
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    TraceLoggerEvent event(logger, TraceLogger_AnnotateScripts, script);
    AutoTraceLog logScript(logger, event);
    AutoTraceLog logCompile(logger, TraceLogger_IonCompilation);

    MOZ_ASSERT(optimizationLevel > Optimization_DontCompile);

    // Make sure the script's canonical function isn't lazy. We can't de-lazify
    // it in a helper thread.
    script->ensureNonLazyCanonicalFunction(cx);

    TrackPropertiesForSingletonScopes(cx, script, baselineFrame);

    LifoAlloc* alloc = cx->new_<LifoAlloc>(TempAllocator::PreferredLifoChunkSize);
    if (!alloc)
        return AbortReason_Alloc;

    ScopedJSDeletePtr<LifoAlloc> autoDelete(alloc);

    TempAllocator* temp = alloc->new_<TempAllocator>(alloc);
    if (!temp)
        return AbortReason_Alloc;

    JitContext jctx(cx, temp);

    AutoEnterAnalysis enter(cx);

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return AbortReason_Alloc;

    if (!cx->compartment()->jitCompartment()->ensureIonStubsExist(cx))
        return AbortReason_Alloc;

    MIRGraph* graph = alloc->new_<MIRGraph>(temp);
    if (!graph)
        return AbortReason_Alloc;

    InlineScriptTree* inlineScriptTree = InlineScriptTree::New(temp, nullptr, nullptr, script);
    if (!inlineScriptTree)
        return AbortReason_Alloc;

    CompileInfo* info = alloc->new_<CompileInfo>(script, script->functionNonDelazifying(), osrPc,
                                                 constructing, Analysis_None,
                                                 script->needsArgsObj(), inlineScriptTree);
    if (!info)
        return AbortReason_Alloc;

    BaselineInspector* inspector = alloc->new_<BaselineInspector>(script);
    if (!inspector)
        return AbortReason_Alloc;

    BaselineFrameInspector* baselineFrameInspector = nullptr;
    if (baselineFrame) {
        baselineFrameInspector = NewBaselineFrameInspector(temp, baselineFrame, info);
        if (!baselineFrameInspector)
            return AbortReason_Alloc;
    }

    CompilerConstraintList* constraints = NewCompilerConstraintList(*temp);
    if (!constraints)
        return AbortReason_Alloc;

    const OptimizationInfo* optimizationInfo = js_IonOptimizations.get(optimizationLevel);
    const JitCompileOptions options(cx);

    IonBuilder* builder = alloc->new_<IonBuilder>((JSContext*) nullptr,
                                                  CompileCompartment::get(cx->compartment()),
                                                  options, temp, graph, constraints,
                                                  inspector, info, optimizationInfo,
                                                  baselineFrameInspector);
    if (!builder)
        return AbortReason_Alloc;

    MOZ_ASSERT(recompile == builder->script()->hasIonScript());
    MOZ_ASSERT(builder->script()->canIonCompile());

    RootedScript builderScript(cx, builder->script());

    if (recompile)
        builderScript->ionScript()->setRecompiling();

#ifdef DEBUG
    IonSpewFunction ionSpewFunction(graph, builderScript);
#endif

    bool succeeded = builder->build();
    builder->clearForBackEnd();

    if (!succeeded) {
        AbortReason reason = builder->abortReason();
        if (reason == AbortReason_NewScriptProperties) {
            // Some type was accessed which needs the new script properties
            // analysis to be performed. Do this now and we will try to build
            // again shortly.
            const MIRGenerator::ObjectGroupVector& groups = builder->abortedNewScriptPropertiesGroups();
            for (size_t i = 0; i < groups.length(); i++) {
                if (!groups[i]->newScript()->maybeAnalyze(cx, groups[i], nullptr, /* force = */ true))
                    return AbortReason_Alloc;
            }
        }

        if (builder->hadActionableAbort()) {
            JSScript* abortScript;
            jsbytecode* abortPc;
            const char* abortMessage;
            builder->actionableAbortLocationAndMessage(&abortScript, &abortPc, &abortMessage);
            TrackIonAbort(cx, abortScript, abortPc, abortMessage);
        }

        return reason;
    }

    // If possible, compile the script off thread.
    if (OffThreadCompilationAvailable(cx)) {
        if (!recompile)
            builderScript->setIonScript(cx, ION_COMPILING_SCRIPT);

        JitSpew(JitSpew_IonLogs, "Can't log script %s:%d. (Compiled on background thread.)",
                builderScript->filename(), builderScript->lineno());

        JSRuntime* rt = cx->runtime();
        if (!builder->nurseryObjects().empty() && !rt->jitRuntime()->hasIonNurseryObjects()) {
            // Ensure the builder's nursery objects are marked when a nursery
            // GC happens on the main thread.
            MarkOffThreadNurseryObjects mark;
            rt->gc.storeBuffer.putGeneric(mark);
            rt->jitRuntime()->setHasIonNurseryObjects(true);
        }

        if (!StartOffThreadIonCompile(cx, builder)) {
            JitSpew(JitSpew_IonAbort, "Unable to start off-thread ion compilation.");
            return AbortReason_Alloc;
        }

        // The allocator and associated data will be destroyed after being
        // processed in the finishedOffThreadCompilations list.
        autoDelete.forget();

        return AbortReason_NoAbort;
    }

    ScopedJSDeletePtr<CodeGenerator> codegen(CompileBackEnd(builder));
    if (!codegen) {
        JitSpew(JitSpew_IonAbort, "Failed during back-end compilation.");
        return AbortReason_Disable;
    }

    bool success = codegen->link(cx, builder->constraints());

    if (success)
        return AbortReason_NoAbort;
    if (cx->isExceptionPending())
        return AbortReason_Error;
    return AbortReason_Disable;
}

static bool
CheckFrame(JSContext* cx, BaselineFrame* frame)
{
    MOZ_ASSERT(!frame->script()->isGenerator());
    MOZ_ASSERT(!frame->isDebuggerEvalFrame());

    // This check is to not overrun the stack.
    if (frame->isFunctionFrame()) {
        if (TooManyActualArguments(frame->numActualArgs())) {
            TrackAndSpewIonAbort(cx, frame->script(), "too many actual arguments");
            return false;
        }

        if (TooManyFormalArguments(frame->numFormalArgs())) {
            TrackAndSpewIonAbort(cx, frame->script(), "too many arguments");
            return false;
        }
    }

    return true;
}

static bool
CheckScript(JSContext* cx, JSScript* script, bool osr)
{
    if (script->isForEval()) {
        // Eval frames are not yet supported. Supporting this will require new
        // logic in pushBailoutFrame to deal with linking prev.
        // Additionally, JSOP_DEFVAR support will require baking in isEvalFrame().
        TrackAndSpewIonAbort(cx, script, "eval script");
        return false;
    }

    if (script->isGenerator()) {
        TrackAndSpewIonAbort(cx, script, "generator script");
        return false;
    }

    if (!script->compileAndGo() && !script->functionNonDelazifying()) {
        // Support non-CNG functions but not other scripts. For global scripts,
        // IonBuilder currently uses the global object as scope chain, this is
        // not valid for non-CNG code.
        TrackAndSpewIonAbort(cx, script, "not compile-and-go");
        return false;
    }

    return true;
}

static MethodStatus
CheckScriptSize(JSContext* cx, JSScript* script)
{
    if (!js_JitOptions.limitScriptSize)
        return Method_Compiled;

    uint32_t numLocalsAndArgs = NumLocalsAndArgs(script);

    if (script->length() > MAX_MAIN_THREAD_SCRIPT_SIZE ||
        numLocalsAndArgs > MAX_MAIN_THREAD_LOCALS_AND_ARGS)
    {
        if (!OffThreadCompilationAvailable(cx)) {
            JitSpew(JitSpew_IonAbort, "Script too large (%u bytes) (%u locals/args)",
                    script->length(), numLocalsAndArgs);
            TrackIonAbort(cx, script, script->code(), "too large");
            return Method_CantCompile;
        }
    }

    return Method_Compiled;
}

bool
CanIonCompileScript(JSContext* cx, JSScript* script, bool osr)
{
    if (!script->canIonCompile() || !CheckScript(cx, script, osr))
        return false;

    return CheckScriptSize(cx, script) == Method_Compiled;
}

static OptimizationLevel
GetOptimizationLevel(HandleScript script, jsbytecode* pc)
{
    return js_IonOptimizations.levelForScript(script, pc);
}

static MethodStatus
Compile(JSContext* cx, HandleScript script, BaselineFrame* osrFrame, jsbytecode* osrPc,
        bool constructing, bool forceRecompile = false)
{
    MOZ_ASSERT(jit::IsIonEnabled(cx));
    MOZ_ASSERT(jit::IsBaselineEnabled(cx));
    MOZ_ASSERT_IF(osrPc != nullptr, LoopEntryCanIonOsr(osrPc));

    if (!script->hasBaselineScript())
        return Method_Skipped;

    if (script->isDebuggee() || (osrFrame && osrFrame->isDebuggee())) {
        TrackAndSpewIonAbort(cx, script, "debugging");
        return Method_Skipped;
    }

    if (!CheckScript(cx, script, bool(osrPc))) {
        JitSpew(JitSpew_IonAbort, "Aborted compilation of %s:%d", script->filename(), script->lineno());
        return Method_CantCompile;
    }

    MethodStatus status = CheckScriptSize(cx, script);
    if (status != Method_Compiled) {
        JitSpew(JitSpew_IonAbort, "Aborted compilation of %s:%d", script->filename(), script->lineno());
        return status;
    }

    bool recompile = false;
    OptimizationLevel optimizationLevel = GetOptimizationLevel(script, osrPc);
    if (optimizationLevel == Optimization_DontCompile)
        return Method_Skipped;

    IonScript* scriptIon = script->maybeIonScript();
    if (scriptIon) {
        if (!scriptIon->method())
            return Method_CantCompile;

        // Don't recompile/overwrite higher optimized code,
        // with a lower optimization level.
        if (optimizationLevel <= scriptIon->optimizationLevel() && !forceRecompile)
            return Method_Compiled;

        // Don't start compiling if already compiling
        if (scriptIon->isRecompiling())
            return Method_Compiled;

        if (osrPc)
            scriptIon->resetOsrPcMismatchCounter();

        recompile = true;
    }

    AbortReason reason = IonCompile(cx, script, osrFrame, osrPc, constructing,
                                    recompile, optimizationLevel);
    if (reason == AbortReason_Error)
        return Method_Error;

    if (reason == AbortReason_Disable)
        return Method_CantCompile;

    if (reason == AbortReason_Alloc) {
        js_ReportOutOfMemory(cx);
        return Method_Error;
    }

    // Compilation succeeded or we invalidated right away or an inlining/alloc abort
    if (script->hasIonScript())
        return Method_Compiled;
    return Method_Skipped;
}

} // namespace jit
} // namespace js

// Decide if a transition from interpreter execution to Ion code should occur.
// May compile or recompile the target JSScript.
MethodStatus
jit::CanEnterAtBranch(JSContext* cx, JSScript* script, BaselineFrame* osrFrame, jsbytecode* pc)
{
    MOZ_ASSERT(jit::IsIonEnabled(cx));
    MOZ_ASSERT((JSOp)*pc == JSOP_LOOPENTRY);
    MOZ_ASSERT(LoopEntryCanIonOsr(pc));

    // Skip if the script has been disabled.
    if (!script->canIonCompile())
        return Method_Skipped;

    // Skip if the script is being compiled off thread.
    if (script->isIonCompilingOffThread())
        return Method_Skipped;

    // Skip if the code is expected to result in a bailout.
    if (script->hasIonScript() && script->ionScript()->bailoutExpected())
        return Method_Skipped;

    // Optionally ignore on user request.
    if (!js_JitOptions.osr)
        return Method_Skipped;

    // Mark as forbidden if frame can't be handled.
    if (!CheckFrame(cx, osrFrame)) {
        ForbidCompilation(cx, script);
        return Method_CantCompile;
    }

    // By default a recompilation doesn't happen on osr mismatch.
    // Decide if we want to force a recompilation if this happens too much.
    bool force = false;
    if (script->hasIonScript() && pc != script->ionScript()->osrPc()) {
        uint32_t count = script->ionScript()->incrOsrPcMismatchCounter();
        if (count <= js_JitOptions.osrPcMismatchesBeforeRecompile)
            return Method_Skipped;
        force = true;
    }

    // Attempt compilation.
    // - Returns Method_Compiled if the right ionscript is present
    //   (Meaning it was present or a sequantial compile finished)
    // - Returns Method_Skipped if pc doesn't match
    //   (This means a background thread compilation with that pc could have started or not.)
    RootedScript rscript(cx, script);
    MethodStatus status = Compile(cx, rscript, osrFrame, pc, osrFrame->isConstructing(),
                                  force);
    if (status != Method_Compiled) {
        if (status == Method_CantCompile)
            ForbidCompilation(cx, script);
        return status;
    }

    // Return the compilation was skipped when the osr pc wasn't adjusted.
    // This can happen when there was still an IonScript available and a
    // background compilation started, but hasn't finished yet.
    // Or when we didn't force a recompile.
    if (pc != script->ionScript()->osrPc())
        return Method_Skipped;

    return Method_Compiled;
}

MethodStatus
jit::CanEnter(JSContext* cx, RunState& state)
{
    MOZ_ASSERT(jit::IsIonEnabled(cx));

    JSScript* script = state.script();

    // Skip if the script has been disabled.
    if (!script->canIonCompile())
        return Method_Skipped;

    // Skip if the script is being compiled off thread.
    if (script->isIonCompilingOffThread())
        return Method_Skipped;

    // Skip if the code is expected to result in a bailout.
    if (script->hasIonScript() && script->ionScript()->bailoutExpected())
        return Method_Skipped;

    RootedScript rscript(cx, script);

    // If constructing, allocate a new |this| object before building Ion.
    // Creating |this| is done before building Ion because it may change the
    // type information and invalidate compilation results.
    if (state.isInvoke()) {
        InvokeState& invoke = *state.asInvoke();

        if (TooManyActualArguments(invoke.args().length())) {
            TrackAndSpewIonAbort(cx, script, "too many actual args");
            ForbidCompilation(cx, script);
            return Method_CantCompile;
        }

        if (TooManyFormalArguments(invoke.args().callee().as<JSFunction>().nargs())) {
            TrackAndSpewIonAbort(cx, script, "too many args");
            ForbidCompilation(cx, script);
            return Method_CantCompile;
        }

        if (!state.maybeCreateThisForConstructor(cx))
            return Method_Skipped;
    }

    // If --ion-eager is used, compile with Baseline first, so that we
    // can directly enter IonMonkey.
    if (js_JitOptions.eagerCompilation && !rscript->hasBaselineScript()) {
        MethodStatus status = CanEnterBaselineMethod(cx, state);
        if (status != Method_Compiled)
            return status;
    }

    // Attempt compilation. Returns Method_Compiled if already compiled.
    bool constructing = state.isInvoke() && state.asInvoke()->constructing();
    MethodStatus status = Compile(cx, rscript, nullptr, nullptr, constructing);
    if (status != Method_Compiled) {
        if (status == Method_CantCompile)
            ForbidCompilation(cx, rscript);
        return status;
    }

    return Method_Compiled;
}

MethodStatus
jit::CompileFunctionForBaseline(JSContext* cx, HandleScript script, BaselineFrame* frame)
{
    MOZ_ASSERT(jit::IsIonEnabled(cx));
    MOZ_ASSERT(frame->fun()->nonLazyScript()->canIonCompile());
    MOZ_ASSERT(!frame->fun()->nonLazyScript()->isIonCompilingOffThread());
    MOZ_ASSERT(!frame->fun()->nonLazyScript()->hasIonScript());
    MOZ_ASSERT(frame->isFunctionFrame());

    // Mark as forbidden if frame can't be handled.
    if (!CheckFrame(cx, frame)) {
        ForbidCompilation(cx, script);
        return Method_CantCompile;
    }

    // Attempt compilation. Returns Method_Compiled if already compiled.
    MethodStatus status = Compile(cx, script, frame, nullptr, frame->isConstructing());
    if (status != Method_Compiled) {
        if (status == Method_CantCompile)
            ForbidCompilation(cx, script);
        return status;
    }

    return Method_Compiled;
}

MethodStatus
jit::Recompile(JSContext* cx, HandleScript script, BaselineFrame* osrFrame, jsbytecode* osrPc,
               bool constructing, bool force)
{
    MOZ_ASSERT(script->hasIonScript());
    if (script->ionScript()->isRecompiling())
        return Method_Compiled;

    MethodStatus status = Compile(cx, script, osrFrame, osrPc, constructing, force);
    if (status != Method_Compiled) {
        if (status == Method_CantCompile)
            ForbidCompilation(cx, script);
        return status;
    }

    return Method_Compiled;
}

MethodStatus
jit::CanEnterUsingFastInvoke(JSContext* cx, HandleScript script, uint32_t numActualArgs)
{
    MOZ_ASSERT(jit::IsIonEnabled(cx));

    // Skip if the code is expected to result in a bailout.
    if (!script->hasIonScript() || script->ionScript()->bailoutExpected())
        return Method_Skipped;

    // Don't handle arguments underflow, to make this work we would have to pad
    // missing arguments with |undefined|.
    if (numActualArgs < script->functionNonDelazifying()->nargs())
        return Method_Skipped;

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return Method_Error;

    // This can GC, so afterward, script->ion is not guaranteed to be valid.
    if (!cx->runtime()->jitRuntime()->enterIon())
        return Method_Error;

    if (!script->hasIonScript())
        return Method_Skipped;

    return Method_Compiled;
}

static JitExecStatus
EnterIon(JSContext* cx, EnterJitData& data)
{
    JS_CHECK_RECURSION(cx, return JitExec_Aborted);
    MOZ_ASSERT(jit::IsIonEnabled(cx));
    MOZ_ASSERT(!data.osrFrame);

    EnterJitCode enter = cx->runtime()->jitRuntime()->enterIon();

    // Caller must construct |this| before invoking the Ion function.
    MOZ_ASSERT_IF(data.constructing, data.maxArgv[0].isObject());

    data.result.setInt32(data.numActualArgs);
    {
        AssertCompartmentUnchanged pcc(cx);
        JitActivation activation(cx);

        CALL_GENERATED_CODE(enter, data.jitcode, data.maxArgc, data.maxArgv, /* osrFrame = */nullptr, data.calleeToken,
                            /* scopeChain = */ nullptr, 0, data.result.address());
    }

    MOZ_ASSERT(!cx->runtime()->jitRuntime()->hasIonReturnOverride());

    // Jit callers wrap primitive constructor return.
    if (!data.result.isMagic() && data.constructing && data.result.isPrimitive())
        data.result = data.maxArgv[0];

    // Release temporary buffer used for OSR into Ion.
    cx->runtime()->getJitRuntime(cx)->freeOsrTempData();

    MOZ_ASSERT_IF(data.result.isMagic(), data.result.isMagic(JS_ION_ERROR));
    return data.result.isMagic() ? JitExec_Error : JitExec_Ok;
}

bool
jit::SetEnterJitData(JSContext* cx, EnterJitData& data, RunState& state, AutoValueVector& vals)
{
    data.osrFrame = nullptr;

    if (state.isInvoke()) {
        CallArgs& args = state.asInvoke()->args();
        unsigned numFormals = state.script()->functionNonDelazifying()->nargs();
        data.constructing = state.asInvoke()->constructing();
        data.numActualArgs = args.length();
        data.maxArgc = Max(args.length(), numFormals) + 1;
        data.scopeChain = nullptr;
        data.calleeToken = CalleeToToken(&args.callee().as<JSFunction>(), data.constructing);

        if (data.numActualArgs >= numFormals) {
            data.maxArgv = args.base() + 1;
        } else {
            MOZ_ASSERT(vals.empty());
            if (!vals.reserve(Max(args.length() + 1, numFormals + 1)))
                return false;

            // Append |this| and any provided arguments.
            for (size_t i = 1; i < args.length() + 2; ++i)
                vals.infallibleAppend(args.base()[i]);

            // Pad missing arguments with |undefined|.
            while (vals.length() < numFormals + 1)
                vals.infallibleAppend(UndefinedValue());

            MOZ_ASSERT(vals.length() >= numFormals + 1);
            data.maxArgv = vals.begin();
        }
    } else {
        data.constructing = false;
        data.numActualArgs = 0;
        data.maxArgc = 1;
        data.maxArgv = state.asExecute()->addressOfThisv();
        data.scopeChain = state.asExecute()->scopeChain();

        data.calleeToken = CalleeToToken(state.script());

        if (state.script()->isForEval() &&
            !(state.asExecute()->type() & InterpreterFrame::GLOBAL))
        {
            ScriptFrameIter iter(cx);
            if (iter.isFunctionFrame())
                data.calleeToken = CalleeToToken(iter.callee(cx), /* constructing = */ false);
        }
    }

    return true;
}

JitExecStatus
jit::IonCannon(JSContext* cx, RunState& state)
{
    IonScript* ion = state.script()->ionScript();

    EnterJitData data(cx);
    data.jitcode = ion->method()->raw();

    AutoValueVector vals(cx);
    if (!SetEnterJitData(cx, data, state, vals))
        return JitExec_Error;

    JitExecStatus status = EnterIon(cx, data);

    if (status == JitExec_Ok)
        state.setReturnValue(data.result);

    return status;
}

JitExecStatus
jit::FastInvoke(JSContext* cx, HandleFunction fun, CallArgs& args)
{
    JS_CHECK_RECURSION(cx, return JitExec_Error);

    IonScript* ion = fun->nonLazyScript()->ionScript();
    JitCode* code = ion->method();
    void* jitcode = code->raw();

    MOZ_ASSERT(jit::IsIonEnabled(cx));
    MOZ_ASSERT(!ion->bailoutExpected());

    JitActivation activation(cx);

    EnterJitCode enter = cx->runtime()->jitRuntime()->enterIon();
    void* calleeToken = CalleeToToken(fun, /* constructing = */ false);

    RootedValue result(cx, Int32Value(args.length()));
    MOZ_ASSERT(args.length() >= fun->nargs());

    CALL_GENERATED_CODE(enter, jitcode, args.length() + 1, args.array() - 1, /* osrFrame = */nullptr,
                        calleeToken, /* scopeChain = */ nullptr, 0, result.address());

    MOZ_ASSERT(!cx->runtime()->jitRuntime()->hasIonReturnOverride());

    args.rval().set(result);

    MOZ_ASSERT_IF(result.isMagic(), result.isMagic(JS_ION_ERROR));
    return result.isMagic() ? JitExec_Error : JitExec_Ok;
}

static void
InvalidateActivation(FreeOp* fop, const JitActivationIterator& activations, bool invalidateAll)
{
    JitSpew(JitSpew_IonInvalidate, "BEGIN invalidating activation");

#ifdef CHECK_OSIPOINT_REGISTERS
    if (js_JitOptions.checkOsiPointRegisters)
        activations->asJit()->setCheckRegs(false);
#endif

    size_t frameno = 1;

    for (JitFrameIterator it(activations); !it.done(); ++it, ++frameno) {
        MOZ_ASSERT_IF(frameno == 1, it.type() == JitFrame_Exit || it.type() == JitFrame_Bailout);

#ifdef DEBUG
        switch (it.type()) {
          case JitFrame_Exit:
            JitSpew(JitSpew_IonInvalidate, "#%d exit frame @ %p", frameno, it.fp());
            break;
          case JitFrame_BaselineJS:
          case JitFrame_IonJS:
          case JitFrame_Bailout:
          {
            MOZ_ASSERT(it.isScripted());
            const char* type = "Unknown";
            if (it.isIonJS())
                type = "Optimized";
            else if (it.isBaselineJS())
                type = "Baseline";
            else if (it.isBailoutJS())
                type = "Bailing";
            JitSpew(JitSpew_IonInvalidate, "#%d %s JS frame @ %p, %s:%d (fun: %p, script: %p, pc %p)",
                    frameno, type, it.fp(), it.script()->filename(), it.script()->lineno(),
                    it.maybeCallee(), (JSScript*)it.script(), it.returnAddressToFp());
            break;
          }
          case JitFrame_BaselineStub:
            JitSpew(JitSpew_IonInvalidate, "#%d baseline stub frame @ %p", frameno, it.fp());
            break;
          case JitFrame_Rectifier:
            JitSpew(JitSpew_IonInvalidate, "#%d rectifier frame @ %p", frameno, it.fp());
            break;
          case JitFrame_Unwound_IonJS:
          case JitFrame_Unwound_BaselineJS:
          case JitFrame_Unwound_BaselineStub:
          case JitFrame_Unwound_IonAccessorIC:
            MOZ_CRASH("invalid");
          case JitFrame_Unwound_Rectifier:
            JitSpew(JitSpew_IonInvalidate, "#%d unwound rectifier frame @ %p", frameno, it.fp());
            break;
          case JitFrame_IonAccessorIC:
            JitSpew(JitSpew_IonInvalidate, "#%d ion IC getter/setter frame @ %p", frameno, it.fp());
            break;
          case JitFrame_Entry:
            JitSpew(JitSpew_IonInvalidate, "#%d entry frame @ %p", frameno, it.fp());
            break;
        }
#endif

        if (!it.isIonScripted())
            continue;

        bool calledFromLinkStub = false;
        JitCode* lazyLinkStub = fop->runtime()->jitRuntime()->lazyLinkStub();
        if (it.returnAddressToFp() >= lazyLinkStub->raw() &&
            it.returnAddressToFp() < lazyLinkStub->rawEnd())
        {
            calledFromLinkStub = true;
        }

        // See if the frame has already been invalidated.
        if (!calledFromLinkStub && it.checkInvalidation())
            continue;

        JSScript* script = it.script();
        if (!script->hasIonScript())
            continue;

        if (!invalidateAll && !script->ionScript()->invalidated())
            continue;

        IonScript* ionScript = script->ionScript();

        // Purge ICs before we mark this script as invalidated. This will
        // prevent lastJump_ from appearing to be a bogus pointer, just
        // in case anyone tries to read it.
        ionScript->purgeCaches();

        // Clean up any pointers from elsewhere in the runtime to this IonScript
        // which is about to become disconnected from its JSScript.
        ionScript->unlinkFromRuntime(fop);

        // This frame needs to be invalidated. We do the following:
        //
        // 1. Increment the reference counter to keep the ionScript alive
        //    for the invalidation bailout or for the exception handler.
        // 2. Determine safepoint that corresponds to the current call.
        // 3. From safepoint, get distance to the OSI-patchable offset.
        // 4. From the IonScript, determine the distance between the
        //    call-patchable offset and the invalidation epilogue.
        // 5. Patch the OSI point with a call-relative to the
        //    invalidation epilogue.
        //
        // The code generator ensures that there's enough space for us
        // to patch in a call-relative operation at each invalidation
        // point.
        //
        // Note: you can't simplify this mechanism to "just patch the
        // instruction immediately after the call" because things may
        // need to move into a well-defined register state (using move
        // instructions after the call) in to capture an appropriate
        // snapshot after the call occurs.

        ionScript->incrementInvalidationCount();

        JitCode* ionCode = ionScript->method();

        JS::Zone* zone = script->zone();
        if (zone->needsIncrementalBarrier()) {
            // We're about to remove edges from the JSScript to gcthings
            // embedded in the JitCode. Perform one final trace of the
            // JitCode for the incremental GC, as it must know about
            // those edges.
            ionCode->trace(zone->barrierTracer());
        }
        ionCode->setInvalidated();

        // Don't adjust OSI points in the linkStub (which don't exist), or in a
        // bailout path.
        if (calledFromLinkStub || it.isBailoutJS())
            continue;

        // Write the delta (from the return address offset to the
        // IonScript pointer embedded into the invalidation epilogue)
        // where the safepointed call instruction used to be. We rely on
        // the call sequence causing the safepoint being >= the size of
        // a uint32, which is checked during safepoint index
        // construction.
        const SafepointIndex* si = ionScript->getSafepointIndex(it.returnAddressToFp());
        CodeLocationLabel dataLabelToMunge(it.returnAddressToFp());
        ptrdiff_t delta = ionScript->invalidateEpilogueDataOffset() -
                          (it.returnAddressToFp() - ionCode->raw());
        Assembler::PatchWrite_Imm32(dataLabelToMunge, Imm32(delta));

        CodeLocationLabel osiPatchPoint = SafepointReader::InvalidationPatchPoint(ionScript, si);
        CodeLocationLabel invalidateEpilogue(ionCode, CodeOffsetLabel(ionScript->invalidateEpilogueOffset()));

        JitSpew(JitSpew_IonInvalidate, "   ! Invalidate ionScript %p (inv count %u) -> patching osipoint %p",
                ionScript, ionScript->invalidationCount(), (void*) osiPatchPoint.raw());
        Assembler::PatchWrite_NearCall(osiPatchPoint, invalidateEpilogue);
    }

    JitSpew(JitSpew_IonInvalidate, "END invalidating activation");
}

void
jit::StopAllOffThreadCompilations(JSCompartment* comp)
{
    if (!comp->jitCompartment())
        return;
    CancelOffThreadIonCompile(comp, nullptr);
    FinishAllOffThreadCompilations(comp);
}

void
jit::StopAllOffThreadCompilations(Zone* zone)
{
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
        StopAllOffThreadCompilations(comp);
}

void
jit::InvalidateAll(FreeOp* fop, Zone* zone)
{
    StopAllOffThreadCompilations(zone);

    for (JitActivationIterator iter(fop->runtime()); !iter.done(); ++iter) {
        if (iter->compartment()->zone() == zone) {
            JitSpew(JitSpew_IonInvalidate, "Invalidating all frames for GC");
            InvalidateActivation(fop, iter, true);
        }
    }
}


void
jit::Invalidate(TypeZone& types, FreeOp* fop,
                const RecompileInfoVector& invalid, bool resetUses,
                bool cancelOffThread)
{
    JitSpew(JitSpew_IonInvalidate, "Start invalidation.");

    // Add an invalidation reference to all invalidated IonScripts to indicate
    // to the traversal which frames have been invalidated.
    size_t numInvalidations = 0;
    for (size_t i = 0; i < invalid.length(); i++) {
        const CompilerOutput* co = invalid[i].compilerOutput(types);
        if (!co)
            continue;
        MOZ_ASSERT(co->isValid());

        if (cancelOffThread)
            CancelOffThreadIonCompile(co->script()->compartment(), co->script());

        if (!co->ion())
            continue;

        JitSpew(JitSpew_IonInvalidate, " Invalidate %s:%u, IonScript %p",
                co->script()->filename(), co->script()->lineno(), co->ion());

        // Keep the ion script alive during the invalidation and flag this
        // ionScript as being invalidated.  This increment is removed by the
        // loop after the calls to InvalidateActivation.
        co->ion()->incrementInvalidationCount();
        numInvalidations++;
    }

    if (!numInvalidations) {
        JitSpew(JitSpew_IonInvalidate, " No IonScript invalidation.");
        return;
    }

    for (JitActivationIterator iter(fop->runtime()); !iter.done(); ++iter)
        InvalidateActivation(fop, iter, false);

    // Drop the references added above. If a script was never active, its
    // IonScript will be immediately destroyed. Otherwise, it will be held live
    // until its last invalidated frame is destroyed.
    for (size_t i = 0; i < invalid.length(); i++) {
        CompilerOutput* co = invalid[i].compilerOutput(types);
        if (!co)
            continue;
        MOZ_ASSERT(co->isValid());

        JSScript* script = co->script();
        IonScript* ionScript = co->ion();
        if (!ionScript)
            continue;

        script->setIonScript(nullptr, nullptr);
        ionScript->decrementInvalidationCount(fop);
        co->invalidate();
        numInvalidations--;

        // Wait for the scripts to get warm again before doing another
        // compile, unless we are recompiling *because* a script got hot
        // (resetUses is false).
        if (resetUses)
            script->resetWarmUpCounter();
    }

    // Make sure we didn't leak references by invalidating the same IonScript
    // multiple times in the above loop.
    MOZ_ASSERT(!numInvalidations);
}

void
jit::Invalidate(JSContext* cx, const RecompileInfoVector& invalid, bool resetUses,
                bool cancelOffThread)
{
    jit::Invalidate(cx->zone()->types, cx->runtime()->defaultFreeOp(), invalid, resetUses,
                    cancelOffThread);
}

bool
jit::IonScript::invalidate(JSContext* cx, bool resetUses, const char* reason)
{
    JitSpew(JitSpew_IonInvalidate, " Invalidate IonScript %p: %s", this, reason);
    RecompileInfoVector list;
    if (!list.append(recompileInfo()))
        return false;
    Invalidate(cx, list, resetUses, true);
    return true;
}

bool
jit::Invalidate(JSContext* cx, JSScript* script, bool resetUses, bool cancelOffThread)
{
    MOZ_ASSERT(script->hasIonScript());

    if (cx->runtime()->spsProfiler.enabled()) {
        // Register invalidation with profiler.
        // Format of event payload string:
        //      "<filename>:<lineno>"

        // Get the script filename, if any, and its length.
        const char* filename = script->filename();
        if (filename == nullptr)
            filename = "<unknown>";

        size_t len = strlen(filename) + 20;
        char* buf = js_pod_malloc<char>(len);
        if (!buf)
            return false;

        // Construct the descriptive string.
        JS_snprintf(buf, len, "Invalidate %s:%u", filename, (unsigned int)script->lineno());
        cx->runtime()->spsProfiler.markEvent(buf);
        js_free(buf);
    }

    RecompileInfoVector scripts;
    MOZ_ASSERT(script->hasIonScript());
    if (!scripts.append(script->ionScript()->recompileInfo()))
        return false;

    Invalidate(cx, scripts, resetUses, cancelOffThread);
    return true;
}

static void
FinishInvalidationOf(FreeOp* fop, JSScript* script, IonScript* ionScript)
{
    TypeZone& types = script->zone()->types;

    // Note: If the script is about to be swept, the compiler output may have
    // already been destroyed.
    if (CompilerOutput* output = ionScript->recompileInfo().compilerOutput(types))
        output->invalidate();

    // If this script has Ion code on the stack, invalidated() will return
    // true. In this case we have to wait until destroying it.
    if (!ionScript->invalidated())
        jit::IonScript::Destroy(fop, ionScript);
}

void
jit::FinishInvalidation(FreeOp* fop, JSScript* script)
{
    // In all cases, nullptr out script->ion to avoid re-entry.
    if (script->hasIonScript()) {
        IonScript* ion = script->ionScript();
        script->setIonScript(nullptr, nullptr);
        FinishInvalidationOf(fop, script, ion);
    }
}

void
jit::ForbidCompilation(JSContext* cx, JSScript* script)
{
    JitSpew(JitSpew_IonAbort, "Disabling Ion compilation of script %s:%d",
            script->filename(), script->lineno());

    CancelOffThreadIonCompile(cx->compartment(), script);

    if (script->hasIonScript()) {
        // It is only safe to modify script->ion if the script is not currently
        // running, because JitFrameIterator needs to tell what ionScript to
        // use (either the one on the JSScript, or the one hidden in the
        // breadcrumbs Invalidation() leaves). Therefore, if invalidation
        // fails, we cannot disable the script.
        if (!Invalidate(cx, script, false))
            return;
    }

    script->setIonScript(cx, ION_DISABLED_SCRIPT);
}

AutoFlushICache*
PerThreadData::autoFlushICache() const
{
    return autoFlushICache_;
}

void
PerThreadData::setAutoFlushICache(AutoFlushICache* afc)
{
    autoFlushICache_ = afc;
}

// Set the range for the merging of flushes.  The flushing is deferred until the end of
// the AutoFlushICache context.  Subsequent flushing within this range will is also
// deferred.  This is only expected to be defined once for each AutoFlushICache
// context.  It assumes the range will be flushed is required to be within an
// AutoFlushICache context.
void
AutoFlushICache::setRange(uintptr_t start, size_t len)
{
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    AutoFlushICache* afc = TlsPerThreadData.get()->PerThreadData::autoFlushICache();
    MOZ_ASSERT(afc);
    MOZ_ASSERT(!afc->start_);
    JitSpewCont(JitSpew_CacheFlush, "(%x %x):", start, len);

    uintptr_t stop = start + len;
    afc->start_ = start;
    afc->stop_ = stop;
#endif
}

// Flush the instruction cache.
//
// If called within a dynamic AutoFlushICache context and if the range is already pending
// flushing for this AutoFlushICache context then the request is ignored with the
// understanding that it will be flushed on exit from the AutoFlushICache context.
// Otherwise the range is flushed immediately.
//
// Updates outside the current code object are typically the exception so they are flushed
// immediately rather than attempting to merge them.
//
// For efficiency it is expected that all large ranges will be flushed within an
// AutoFlushICache, so check.  If this assertion is hit then it does not necessarily
// indicate a progam fault but it might indicate a lost opportunity to merge cache
// flushing.  It can be corrected by wrapping the call in an AutoFlushICache to context.
//
// Note this can be called without TLS PerThreadData defined so this case needs
// to be guarded against. E.g. when patching instructions from the exception
// handler on MacOS running the ARM simulator.
void
AutoFlushICache::flush(uintptr_t start, size_t len)
{
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    PerThreadData* pt = TlsPerThreadData.get();
    AutoFlushICache* afc = pt ? pt->PerThreadData::autoFlushICache() : nullptr;
    if (!afc) {
        JitSpewCont(JitSpew_CacheFlush, "#");
        ExecutableAllocator::cacheFlush((void*)start, len);
        MOZ_ASSERT(len <= 16);
        return;
    }

    uintptr_t stop = start + len;
    if (start >= afc->start_ && stop <= afc->stop_) {
        // Update is within the pending flush range, so defer to the end of the context.
        JitSpewCont(JitSpew_CacheFlush, afc->inhibit_ ? "-" : "=");
        return;
    }

    JitSpewCont(JitSpew_CacheFlush, afc->inhibit_ ? "x" : "*");
    ExecutableAllocator::cacheFlush((void*)start, len);
#endif
}

// Flag the current dynamic AutoFlushICache as inhibiting flushing. Useful in error paths
// where the changes are being abandoned.
void
AutoFlushICache::setInhibit()
{
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    AutoFlushICache* afc = TlsPerThreadData.get()->PerThreadData::autoFlushICache();
    MOZ_ASSERT(afc);
    MOZ_ASSERT(afc->start_);
    JitSpewCont(JitSpew_CacheFlush, "I");
    afc->inhibit_ = true;
#endif
}

// The common use case is merging cache flushes when preparing a code object.  In this
// case the entire range of the code object is being flushed and as the code is patched
// smaller redundant flushes could occur.  The design allows an AutoFlushICache dynamic
// thread local context to be declared in which the range of the code object can be set
// which defers flushing until the end of this dynamic context.  The redundant flushing
// within this code range is also deferred avoiding redundant flushing.  Flushing outside
// this code range is not affected and proceeds immediately.
//
// In some cases flushing is not necessary, such as when compiling an asm.js module which
// is flushed again when dynamically linked, and also in error paths that abandon the
// code.  Flushing within the set code range can be inhibited within the AutoFlushICache
// dynamic context by setting an inhibit flag.
//
// The JS compiler can be re-entered while within an AutoFlushICache dynamic context and
// it is assumed that code being assembled or patched is not executed before the exit of
// the respective AutoFlushICache dynamic context.
//
AutoFlushICache::AutoFlushICache(const char* nonce, bool inhibit)
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
  : start_(0),
    stop_(0),
    name_(nonce),
    inhibit_(inhibit)
#endif
{
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    PerThreadData* pt = TlsPerThreadData.get();
    AutoFlushICache* afc = pt->PerThreadData::autoFlushICache();
    if (afc)
        JitSpew(JitSpew_CacheFlush, "<%s,%s%s ", nonce, afc->name_, inhibit ? " I" : "");
    else
        JitSpewCont(JitSpew_CacheFlush, "<%s%s ", nonce, inhibit ? " I" : "");

    prev_ = afc;
    pt->PerThreadData::setAutoFlushICache(this);
#endif
}

AutoFlushICache::~AutoFlushICache()
{
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    PerThreadData* pt = TlsPerThreadData.get();
    MOZ_ASSERT(pt->PerThreadData::autoFlushICache() == this);

    if (!inhibit_ && start_)
        ExecutableAllocator::cacheFlush((void*)start_, size_t(stop_ - start_));

    JitSpewCont(JitSpew_CacheFlush, "%s%s>", name_, start_ ? "" : " U");
    JitSpewFin(JitSpew_CacheFlush);
    pt->PerThreadData::setAutoFlushICache(prev_);
#endif
}

void
jit::PurgeCaches(JSScript* script)
{
    if (script->hasIonScript())
        script->ionScript()->purgeCaches();
}

size_t
jit::SizeOfIonData(JSScript* script, mozilla::MallocSizeOf mallocSizeOf)
{
    size_t result = 0;

    if (script->hasIonScript())
        result += script->ionScript()->sizeOfIncludingThis(mallocSizeOf);

    return result;
}

void
jit::DestroyJitScripts(FreeOp* fop, JSScript* script)
{
    if (script->hasIonScript())
        jit::IonScript::Destroy(fop, script->ionScript());

    if (script->hasBaselineScript())
        jit::BaselineScript::Destroy(fop, script->baselineScript());
}

void
jit::TraceJitScripts(JSTracer* trc, JSScript* script)
{
    if (script->hasIonScript())
        jit::IonScript::Trace(trc, script->ionScript());

    if (script->hasBaselineScript())
        jit::BaselineScript::Trace(trc, script->baselineScript());
}

bool
jit::JitSupportsFloatingPoint()
{
    return js::jit::MacroAssembler::SupportsFloatingPoint();
}

bool
jit::JitSupportsSimd()
{
    return js::jit::MacroAssembler::SupportsSimd();
}

// If you change these, please also change the comment in TempAllocator.
/* static */ const size_t TempAllocator::BallastSize            = 16 * 1024;
/* static */ const size_t TempAllocator::PreferredLifoChunkSize = 32 * 1024;

