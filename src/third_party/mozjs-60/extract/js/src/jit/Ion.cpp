/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Ion.h"

#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/ThreadLocal.h"

#include "gc/FreeOp.h"
#include "gc/Marking.h"
#include "jit/AliasAnalysis.h"
#include "jit/AlignmentMaskAnalysis.h"
#include "jit/BacktrackingAllocator.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineInspector.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIRSpewer.h"
#include "jit/CodeGenerator.h"
#include "jit/EagerSimdUnbox.h"
#include "jit/EdgeCaseAnalysis.h"
#include "jit/EffectiveAddressAnalysis.h"
#include "jit/FlowAliasAnalysis.h"
#include "jit/FoldLinearArithConstants.h"
#include "jit/InstructionReordering.h"
#include "jit/IonAnalysis.h"
#include "jit/IonBuilder.h"
#include "jit/IonIC.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/JitcodeMap.h"
#include "jit/JitCommon.h"
#include "jit/JitCompartment.h"
#include "jit/JitSpewer.h"
#include "jit/LICM.h"
#include "jit/Linker.h"
#include "jit/LIR.h"
#include "jit/LoopUnroller.h"
#include "jit/Lowering.h"
#include "jit/PerfSpewer.h"
#include "jit/RangeAnalysis.h"
#include "jit/ScalarReplacement.h"
#include "jit/Sink.h"
#include "jit/StupidAllocator.h"
#include "jit/ValueNumbering.h"
#include "jit/WasmBCE.h"
#include "js/Printf.h"
#include "vm/Debugger.h"
#include "vm/HelperThreads.h"
#include "vm/JSCompartment.h"
#include "vm/TraceLogging.h"
#include "vtune/VTuneWrapper.h"

#include "gc/PrivateIterators-inl.h"
#include "jit/JitFrames-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "vm/Debugger-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSCompartment-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::jit;

// Assert that JitCode is gc::Cell aligned.
JS_STATIC_ASSERT(sizeof(JitCode) % gc::CellAlignBytes == 0);

static MOZ_THREAD_LOCAL(JitContext*) TlsJitContext;

static JitContext*
CurrentJitContext()
{
    if (!TlsJitContext.init())
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

JitContext::JitContext(TempAllocator* temp)
  : cx(nullptr),
    temp(temp),
    runtime(nullptr),
    compartment(nullptr),
    prev_(CurrentJitContext()),
    assemblerCount_(0)
{
    SetJitContext(this);
}

JitContext::JitContext(CompileRuntime* rt, TempAllocator* temp)
  : cx(nullptr),
    temp(temp),
    runtime(rt),
    compartment(nullptr),
    prev_(CurrentJitContext()),
    assemblerCount_(0)
{
    SetJitContext(this);
}

JitContext::JitContext()
  : cx(nullptr),
    temp(nullptr),
    runtime(nullptr),
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
    if (!TlsJitContext.init())
        return false;

    CheckLogging();

#ifdef JS_CACHEIR_SPEW
    const char* env = getenv("CACHEIR_LOGS");
    if (env && env[0])
        CacheIRSpewer::singleton().init();
#endif

#if defined(JS_CODEGEN_ARM)
    InitARMFlags();
#endif
    CheckPerf();
    return true;
}

JitRuntime::JitRuntime(JSRuntime* rt)
  : execAlloc_(rt),
    backedgeExecAlloc_(rt),
    exceptionTailOffset_(0),
    bailoutTailOffset_(0),
    profilerExitFrameTailOffset_(0),
    enterJITOffset_(0),
    bailoutHandlerOffset_(0),
    argumentsRectifierOffset_(0),
    argumentsRectifierReturnOffset_(0),
    invalidatorOffset_(0),
    lazyLinkStubOffset_(0),
    interpreterStubOffset_(0),
    debugTrapHandler_(nullptr),
    baselineDebugModeOSRHandler_(nullptr),
    trampolineCode_(nullptr),
    functionWrappers_(nullptr),
    preventBackedgePatching_(false),
    jitcodeGlobalTable_(nullptr)
{
}

JitRuntime::~JitRuntime()
{
    js_delete(functionWrappers_.ref());

    // By this point, the jitcode global table should be empty.
    MOZ_ASSERT_IF(jitcodeGlobalTable_, jitcodeGlobalTable_->empty());
    js_delete(jitcodeGlobalTable_.ref());
}

uint32_t
JitRuntime::startTrampolineCode(MacroAssembler& masm)
{
    masm.assumeUnreachable("Shouldn't get here");
    masm.flushBuffer();
    masm.haltingAlign(CodeAlignment);
    masm.setFramePushed(0);
    return masm.currentOffset();
}

bool
JitRuntime::initialize(JSContext* cx, AutoLockForExclusiveAccess& lock)
{
    AutoAtomsCompartment ac(cx, lock);

    JitContext jctx(cx, nullptr);

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return false;

    functionWrappers_ = cx->new_<VMWrapperMap>(cx);
    if (!functionWrappers_ || !functionWrappers_->init())
        return false;

    MacroAssembler masm;

    Label bailoutTail;
    JitSpew(JitSpew_Codegen, "# Emitting bailout tail stub");
    generateBailoutTailStub(masm, &bailoutTail);

    if (cx->runtime()->jitSupportsFloatingPoint) {
        JitSpew(JitSpew_Codegen, "# Emitting bailout tables");

        // Initialize some Ion-only stubs that require floating-point support.
        BailoutTableVector& bailoutTables = bailoutTables_.writeRef();
        if (!bailoutTables.reserve(FrameSizeClass::ClassLimit().classId()))
            return false;

        for (uint32_t id = 0;; id++) {
            FrameSizeClass class_ = FrameSizeClass::FromClass(id);
            if (class_ == FrameSizeClass::ClassLimit())
                break;
            JitSpew(JitSpew_Codegen, "# Bailout table");
            bailoutTables.infallibleAppend(generateBailoutTable(masm, &bailoutTail, id));
        }

        JitSpew(JitSpew_Codegen, "# Emitting bailout handler");
        generateBailoutHandler(masm, &bailoutTail);

        JitSpew(JitSpew_Codegen, "# Emitting invalidator");
        generateInvalidator(masm, &bailoutTail);
    }

    // The arguments rectifier has to use the same frame layout as the function
    // frames it rectifies.
    static_assert(mozilla::IsBaseOf<JitFrameLayout, RectifierFrameLayout>::value,
                  "a rectifier frame can be used with jit frame");
    static_assert(mozilla::IsBaseOf<JitFrameLayout, WasmToJSJitFrameLayout>::value,
                  "wasm frames simply are jit frames");
    static_assert(sizeof(JitFrameLayout) == sizeof(WasmToJSJitFrameLayout),
                  "thus a rectifier frame can be used with a wasm frame");

    JitSpew(JitSpew_Codegen, "# Emitting sequential arguments rectifier");
    generateArgumentsRectifier(masm);

    JitSpew(JitSpew_Codegen, "# Emitting EnterJIT sequence");
    generateEnterJIT(cx, masm);

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Value");
    valuePreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::Value);

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for String");
    stringPreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::String);

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Object");
    objectPreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::Object);

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Shape");
    shapePreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::Shape);

    JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for ObjectGroup");
    objectGroupPreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::ObjectGroup);

    JitSpew(JitSpew_Codegen, "# Emitting malloc stub");
    generateMallocStub(masm);

    JitSpew(JitSpew_Codegen, "# Emitting free stub");
    generateFreeStub(masm);

    JitSpew(JitSpew_Codegen, "# Emitting lazy link stub");
    generateLazyLinkStub(masm);

    JitSpew(JitSpew_Codegen, "# Emitting interpreter stub");
    generateInterpreterStub(masm);

    JitSpew(JitSpew_Codegen, "# Emitting VM function wrappers");
    for (VMFunction* fun = VMFunction::functions; fun; fun = fun->next) {
        if (functionWrappers_->has(fun)) {
            // Duplicate VMFunction definition. See VMFunction::hash.
            continue;
        }
        JitSpew(JitSpew_Codegen, "# VM function wrapper");
        if (!generateVMWrapper(cx, masm, *fun))
            return false;
    }

    JitSpew(JitSpew_Codegen, "# Emitting profiler exit frame tail stub");
    Label profilerExitTail;
    generateProfilerExitFrameTailStub(masm, &profilerExitTail);

    JitSpew(JitSpew_Codegen, "# Emitting exception tail stub");
    void* handler = JS_FUNC_TO_DATA_PTR(void*, jit::HandleException);
    generateExceptionTailStub(masm, handler, &profilerExitTail);

    Linker linker(masm);
    AutoFlushICache afc("Trampolines");
    trampolineCode_ = linker.newCode(cx, CodeKind::Other);
    if (!trampolineCode_)
        return false;

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(trampolineCode_, "Trampolines");
#endif
#ifdef MOZ_VTUNE
    vtune::MarkStub(trampolineCode_, "Trampolines");
#endif

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
        AutoAtomsCompartment ac(cx, lock);
        debugTrapHandler_ = generateDebugTrapHandler(cx);
    }
    return debugTrapHandler_;
}

uint8_t*
JSContext::allocateOsrTempData(size_t size)
{
    osrTempData_ = (uint8_t*)js_realloc(osrTempData_, size);
    return osrTempData_;
}

void
JSContext::freeOsrTempData()
{
    js_free(osrTempData_);
    osrTempData_ = nullptr;
}

void
JitZoneGroup::patchIonBackedges(JSContext* cx, BackedgeTarget target)
{
    if (target == BackedgeLoopHeader) {
        // We must be on the active thread. The caller must use
        // AutoPreventBackedgePatching to ensure we don't reenter.
        MOZ_ASSERT(cx->runtime()->jitRuntime()->preventBackedgePatching());
        MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
    } else {
        // We must be called from InterruptRunningJitCode, or a signal handler
        // triggered there. rt->handlingJitInterrupt() ensures we can't reenter
        // this code.
        MOZ_ASSERT(!cx->runtime()->jitRuntime()->preventBackedgePatching());
        MOZ_ASSERT(cx->handlingJitInterrupt());
    }

    // Do nothing if we know all backedges are already jumping to `target`.
    if (backedgeTarget_ == target)
        return;

    backedgeTarget_ = target;

    cx->runtime()->jitRuntime()->backedgeExecAlloc().makeAllWritable();

    // Patch all loop backedges in Ion code so that they either jump to the
    // normal loop header or to an interrupt handler each time they run.
    for (InlineListIterator<PatchableBackedge> iter(backedgeList().begin());
         iter != backedgeList().end();
         iter++)
    {
        PatchableBackedge* patchableBackedge = *iter;
        if (target == BackedgeLoopHeader)
            PatchBackedge(patchableBackedge->backedge, patchableBackedge->loopHeader, target);
        else
            PatchBackedge(patchableBackedge->backedge, patchableBackedge->interruptCheck, target);
    }

    cx->runtime()->jitRuntime()->backedgeExecAlloc().makeAllExecutable();
}

JitZoneGroup::JitZoneGroup(ZoneGroup* group)
  : backedgeTarget_(group, BackedgeLoopHeader),
    backedgeList_(group)
{}

JitCompartment::JitCompartment()
  : stubCodes_(nullptr)
{
}

JitCompartment::~JitCompartment()
{
    js_delete(stubCodes_);
}

bool
JitCompartment::initialize(JSContext* cx)
{
    stubCodes_ = cx->new_<ICStubCodeMap>(cx->zone());
    if (!stubCodes_)
        return false;

    if (!stubCodes_->init()) {
        ReportOutOfMemory(cx);
        return false;
    }

    stringsCanBeInNursery = cx->nursery().canAllocateStrings();

    return true;
}

template <typename T>
static T
PopNextBitmaskValue(uint32_t* bitmask)
{
    MOZ_ASSERT(*bitmask);
    uint32_t index = mozilla::CountTrailingZeroes32(*bitmask);
    *bitmask ^= 1 << index;

    MOZ_ASSERT(index < uint32_t(T::Count));
    return T(index);
}

void
JitCompartment::performStubReadBarriers(uint32_t stubsToBarrier) const
{
    while (stubsToBarrier) {
        auto stub = PopNextBitmaskValue<StubIndex>(&stubsToBarrier);
        const ReadBarrieredJitCode& jitCode = stubs_[stub];
        MOZ_ASSERT(jitCode);
        jitCode.get();
    }
}

void
JitCompartment::performSIMDTemplateReadBarriers(uint32_t simdTemplatesToBarrier) const
{
    while (simdTemplatesToBarrier) {
        auto type = PopNextBitmaskValue<SimdType>(&simdTemplatesToBarrier);
        const ReadBarrieredObject& tpl = simdTemplateObjects_[type];
        MOZ_ASSERT(tpl);
        tpl.get();
    }
}

bool
JitZone::init(JSContext* cx)
{
    if (!baselineCacheIRStubCodes_.init()) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

void
jit::FreeIonBuilder(IonBuilder* builder)
{
    // The builder is allocated into its LifoAlloc, so destroying that will
    // destroy the builder and all other data accumulated during compilation,
    // except any final codegen (which includes an assembler and needs to be
    // explicitly destroyed).
    js_delete(builder->backgroundCodegen());
    js_delete(builder->alloc().lifoAlloc());
}

void
jit::FinishOffThreadBuilder(JSRuntime* runtime, IonBuilder* builder,
                            const AutoLockHelperThreadState& locked)
{
    MOZ_ASSERT(runtime);

    // Clean the references to the pending IonBuilder, if we just finished it.
    if (builder->script()->baselineScript()->hasPendingIonBuilder() &&
        builder->script()->baselineScript()->pendingIonBuilder() == builder)
    {
        builder->script()->baselineScript()->removePendingIonBuilder(runtime, builder->script());
    }

    // If the builder is still in one of the helper thread list, then remove it.
    if (builder->isInList())
        builder->script()->zone()->group()->ionLazyLinkListRemove(builder);

    // Clear the recompiling flag of the old ionScript, since we continue to
    // use the old ionScript if recompiling fails.
    if (builder->script()->hasIonScript())
        builder->script()->ionScript()->clearRecompiling();

    // Clean up if compilation did not succeed.
    if (builder->script()->isIonCompilingOffThread()) {
        IonScript* ion = nullptr;
        AbortReasonOr<Ok> status = builder->getOffThreadStatus();
        if (status.isErr() && status.unwrapErr() == AbortReason::Disable)
            ion = ION_DISABLED_SCRIPT;
        builder->script()->setIonScript(runtime, ion);
    }

    // Free Ion LifoAlloc off-thread. Free on the main thread if this OOMs.
    if (!StartOffThreadIonFree(builder, locked))
        FreeIonBuilder(builder);
}

static bool
LinkCodeGen(JSContext* cx, IonBuilder* builder, CodeGenerator *codegen)
{
    RootedScript script(cx, builder->script());
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
    TraceLoggerEvent event(TraceLogger_AnnotateScripts, script);
    AutoTraceLog logScript(logger, event);
    AutoTraceLog logLink(logger, TraceLogger_IonLinking);

    if (!codegen->link(cx, builder->constraints()))
        return false;

    return true;
}

static bool
LinkBackgroundCodeGen(JSContext* cx, IonBuilder* builder)
{
    CodeGenerator* codegen = builder->backgroundCodegen();
    if (!codegen)
        return false;

    JitContext jctx(cx, &builder->alloc());

    // Root the assembler until the builder is finished below. As it was
    // constructed off thread, the assembler has not been rooted previously,
    // though any GC activity would discard the builder.
    MacroAssembler::AutoRooter masm(cx, &codegen->masm);

    return LinkCodeGen(cx, builder, codegen);
}

void
jit::LinkIonScript(JSContext* cx, HandleScript calleeScript)
{
    IonBuilder* builder;

    {
        AutoLockHelperThreadState lock;

        // Get the pending builder from the Ion frame.
        MOZ_ASSERT(calleeScript->hasBaselineScript());
        builder = calleeScript->baselineScript()->pendingIonBuilder();
        calleeScript->baselineScript()->removePendingIonBuilder(cx->runtime(), calleeScript);

        // Remove from pending.
        cx->zone()->group()->ionLazyLinkListRemove(builder);
    }

    {
        AutoEnterAnalysis enterTypes(cx);
        if (!LinkBackgroundCodeGen(cx, builder)) {
            // Silently ignore OOM during code generation. The assembly code
            // doesn't has code to handle it after linking happened. So it's
            // not OK to throw a catchable exception from there.
            cx->clearPendingException();
        }
    }

    {
        AutoLockHelperThreadState lock;
        FinishOffThreadBuilder(cx->runtime(), builder, lock);
    }
}

uint8_t*
jit::LazyLinkTopActivation(JSContext* cx, LazyLinkExitFrameLayout* frame)
{
    RootedScript calleeScript(cx, ScriptFromCalleeToken(frame->jsFrame()->calleeToken()));

    LinkIonScript(cx, calleeScript);

    MOZ_ASSERT(calleeScript->hasBaselineScript());
    MOZ_ASSERT(calleeScript->jitCodeRaw());

    return calleeScript->jitCodeRaw();
}

/* static */ void
JitRuntime::Trace(JSTracer* trc, AutoLockForExclusiveAccess& lock)
{
    MOZ_ASSERT(!JS::CurrentThreadIsHeapMinorCollecting());

    // Shared stubs are allocated in the atoms compartment, so do not iterate
    // them after the atoms heap after it has been "finished."
    if (trc->runtime()->atomsAreFinished())
        return;

    Zone* zone = trc->runtime()->atomsCompartment(lock)->zone();
    for (auto i = zone->cellIter<JitCode>(); !i.done(); i.next()) {
        JitCode* code = i;
        TraceRoot(trc, &code, "wrapper");
    }
}

/* static */ void
JitRuntime::TraceJitcodeGlobalTableForMinorGC(JSTracer* trc)
{
    if (trc->runtime()->geckoProfiler().enabled() &&
        trc->runtime()->hasJitRuntime() &&
        trc->runtime()->jitRuntime()->hasJitcodeGlobalTable())
    {
        trc->runtime()->jitRuntime()->getJitcodeGlobalTable()->traceForMinorGC(trc);
    }
}

/* static */ bool
JitRuntime::MarkJitcodeGlobalTableIteratively(GCMarker* marker)
{
    if (marker->runtime()->hasJitRuntime() &&
        marker->runtime()->jitRuntime()->hasJitcodeGlobalTable())
    {
        return marker->runtime()->jitRuntime()->getJitcodeGlobalTable()->markIteratively(marker);
    }
    return false;
}

/* static */ void
JitRuntime::SweepJitcodeGlobalTable(JSRuntime* rt)
{
    if (rt->hasJitRuntime() && rt->jitRuntime()->hasJitcodeGlobalTable())
        rt->jitRuntime()->getJitcodeGlobalTable()->sweep(rt);
}

void
JitCompartment::sweep(JSCompartment* compartment)
{
    // Any outstanding compilations should have been cancelled by the GC.
    MOZ_ASSERT(!HasOffThreadIonCompile(compartment));

    stubCodes_->sweep();

    // If the sweep removed a bailout Fallback stub, nullptr the corresponding return addr.
    for (auto& it : bailoutReturnStubInfo_) {
        if (!stubCodes_->lookup(it.key))
           it = BailoutReturnStubInfo();
    }

    for (ReadBarrieredJitCode& stub : stubs_) {
        if (stub && IsAboutToBeFinalized(&stub))
            stub.set(nullptr);
    }

    for (ReadBarrieredObject& obj : simdTemplateObjects_) {
        if (obj && IsAboutToBeFinalized(&obj))
            obj.set(nullptr);
    }
}

void
JitZone::sweep()
{
    baselineCacheIRStubCodes_.sweep();
}

size_t
JitCompartment::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    size_t n = mallocSizeOf(this);
    if (stubCodes_)
        n += stubCodes_->sizeOfIncludingThis(mallocSizeOf);
    return n;
}

void
JitZone::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                size_t* jitZone,
                                size_t* baselineStubsOptimized,
                                size_t* cachedCFG) const
{
    *jitZone += mallocSizeOf(this);
    *jitZone += baselineCacheIRStubCodes_.sizeOfExcludingThis(mallocSizeOf);
    *jitZone += ionCacheIRStubInfoSet_.sizeOfExcludingThis(mallocSizeOf);

    *baselineStubsOptimized += optimizedStubSpace_.sizeOfExcludingThis(mallocSizeOf);
    *cachedCFG += cfgSpace_.sizeOfExcludingThis(mallocSizeOf);
}

TrampolinePtr
JitRuntime::getBailoutTable(const FrameSizeClass& frameClass) const
{
    MOZ_ASSERT(frameClass != FrameSizeClass::None());
    return trampolineCode(bailoutTables_.ref()[frameClass.classId()].startOffset);
}

uint32_t
JitRuntime::getBailoutTableSize(const FrameSizeClass& frameClass) const
{
    MOZ_ASSERT(frameClass != FrameSizeClass::None());
    return bailoutTables_.ref()[frameClass.classId()].size;
}

TrampolinePtr
JitRuntime::getVMWrapper(const VMFunction& f) const
{
    MOZ_ASSERT(functionWrappers_);
    MOZ_ASSERT(functionWrappers_->initialized());
    MOZ_ASSERT(trampolineCode_);

    JitRuntime::VMWrapperMap::Ptr p = functionWrappers_->readonlyThreadsafeLookup(&f);
    MOZ_ASSERT(p);
    return trampolineCode(p->value());
}

void
JitCodeHeader::init(JitCode* jitCode)
{
    jitCode_ = jitCode;

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    // On AMD Bobcat processors that may have eratas, insert a NOP slide to reduce crashes
    if (CPUInfo::NeedAmdBugWorkaround())
        memset((char *)&nops_, X86Encoding::OneByteOpcodeID::OP_NOP, sizeof(nops_));
#endif
}

template <AllowGC allowGC>
JitCode*
JitCode::New(JSContext* cx, uint8_t* code, uint32_t bufferSize, uint32_t headerSize,
             ExecutablePool* pool, CodeKind kind)
{
    JitCode* codeObj = Allocate<JitCode, allowGC>(cx);
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
    // Store the JitCode pointer in the JitCodeHeader so we can recover the
    // gcthing from relocation tables.
    JitCodeHeader::FromExecutable(code_)->init(this);

    insnSize_ = masm.instructionsSize();
    masm.executableCopy(code_);

    jumpRelocTableBytes_ = masm.jumpRelocationTableBytes();
    masm.copyJumpRelocationTable(code_ + jumpRelocTableOffset());

    dataRelocTableBytes_ = masm.dataRelocationTableBytes();
    masm.copyDataRelocationTable(code_ + dataRelocTableOffset());

    masm.processCodeLabels(code_);
}

void
JitCode::traceChildren(JSTracer* trc)
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
        // If we're moving objects, we need writable JIT code.
        bool movingObjects = JS::CurrentThreadIsHeapMinorCollecting() || zone()->isGCCompacting();
        MaybeAutoWritableJitCode awjc(this, movingObjects ? Reprotect : DontReprotect);

        uint8_t* start = code_ + dataRelocTableOffset();
        CompactBufferReader reader(start, start + dataRelocTableBytes_);
        MacroAssembler::TraceDataRelocations(trc, this, reader);
    }
}

void
JitCode::finalize(FreeOp* fop)
{
    // If this jitcode had a bytecode map, it must have already been removed.
#ifdef DEBUG
    JSRuntime* rt = fop->runtime();
    if (hasBytecodeMap_) {
        MOZ_ASSERT(rt->jitRuntime()->hasJitcodeGlobalTable());
        MOZ_ASSERT(!rt->jitRuntime()->getJitcodeGlobalTable()->lookup(raw()));
    }
#endif

#ifdef MOZ_VTUNE
    vtune::UnmarkCode(this);
#endif

    MOZ_ASSERT(pool_);

    // With W^X JIT code, reprotecting memory for each JitCode instance is
    // slow, so we record the ranges and poison them later all at once. It's
    // safe to ignore OOM here, it just means we won't poison the code.
    if (fop->appendJitPoisonRange(JitPoisonRange(pool_, code_ - headerSize_,
                                                 headerSize_ + bufferSize_)))
    {
        pool_->addRef();
    }
    code_ = nullptr;

    // Code buffers are stored inside ExecutablePools. Pools are refcounted.
    // Releasing the pool may free it. Horrible hack: if we are using perf
    // integration, we don't want to reuse code addresses, so we just leak the
    // memory instead.
    if (!PerfEnabled())
        pool_->release(headerSize_ + bufferSize_, CodeKind(kind_));

    pool_ = nullptr;
}

IonScript::IonScript()
  : method_(nullptr),
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
    icIndex_(0),
    icEntries_(0),
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
    fallbackStubSpace_()
{
}

IonScript*
IonScript::New(JSContext* cx, RecompileInfo recompileInfo,
               uint32_t frameSlots, uint32_t argumentSlots, uint32_t frameSize,
               size_t snapshotsListSize, size_t snapshotsRVATableSize,
               size_t recoversSize, size_t bailoutEntries,
               size_t constants, size_t safepointIndices,
               size_t osiIndices, size_t icEntries,
               size_t runtimeSize,  size_t safepointsSize,
               size_t backedgeEntries, size_t sharedStubEntries,
               OptimizationLevel optimizationLevel)
{
    constexpr size_t DataAlignment = sizeof(void*);

    if (snapshotsListSize >= MAX_BUFFER_SIZE ||
        (bailoutEntries >= MAX_BUFFER_SIZE / sizeof(uint32_t)))
    {
        ReportOutOfMemory(cx);
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
    size_t paddedICEntriesSize = AlignBytes(icEntries * sizeof(uint32_t), DataAlignment);
    size_t paddedRuntimeSize = AlignBytes(runtimeSize, DataAlignment);
    size_t paddedSafepointSize = AlignBytes(safepointsSize, DataAlignment);
    size_t paddedBackedgeSize = AlignBytes(backedgeEntries * sizeof(PatchableBackedge), DataAlignment);
    size_t paddedSharedStubSize = AlignBytes(sharedStubEntries * sizeof(IonICEntry), DataAlignment);

    size_t bytes = paddedSnapshotsSize +
                   paddedRecoversSize +
                   paddedBailoutSize +
                   paddedConstantsSize +
                   paddedSafepointIndicesSize +
                   paddedOsiIndicesSize +
                   paddedICEntriesSize +
                   paddedRuntimeSize +
                   paddedSafepointSize +
                   paddedBackedgeSize +
                   paddedSharedStubSize;
    IonScript* script = cx->zone()->pod_malloc_with_extra<IonScript, uint8_t>(bytes);
    if (!script)
        return nullptr;
    new (script) IonScript();

    uint32_t offsetCursor = sizeof(IonScript);

    script->runtimeData_ = offsetCursor;
    script->runtimeSize_ = runtimeSize;
    offsetCursor += paddedRuntimeSize;

    script->icIndex_ = offsetCursor;
    script->icEntries_ = icEntries;
    offsetCursor += paddedICEntriesSize;

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

    script->sharedStubList_ = offsetCursor;
    script->sharedStubEntries_ = sharedStubEntries;
    offsetCursor += paddedSharedStubSize;

    script->frameSlots_ = frameSlots;
    script->argumentSlots_ = argumentSlots;

    script->frameSize_ = frameSize;

    script->recompileInfo_ = recompileInfo;
    script->optimizationLevel_ = optimizationLevel;

    return script;
}

void
IonScript::adoptFallbackStubs(FallbackICStubSpace* stubSpace)

{
    fallbackStubSpace()->adoptFrom(stubSpace);
}

void
IonScript::trace(JSTracer* trc)
{
    if (method_)
        TraceEdge(trc, &method_, "method");

    for (size_t i = 0; i < numConstants(); i++)
        TraceEdge(trc, &getConstant(i), "constant");

    // Mark all IC stub codes hanging off the IC stub entries.
    for (size_t i = 0; i < numSharedStubs(); i++) {
        IonICEntry& ent = sharedStubList()[i];
        ent.trace(trc);
    }

    // Trace caches so that the JSScript pointer can be updated if moved.
    for (size_t i = 0; i < numICs(); i++)
        getICFromIndex(i).trace(trc);
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
    JitZoneGroup* jzg = cx->zone()->group()->jitZoneGroup;
    JitRuntime::AutoPreventBackedgePatching apbp(cx->runtime());

    for (size_t i = 0; i < backedgeEntries_; i++) {
        PatchableBackedgeInfo& info = backedges[i];
        PatchableBackedge* patchableBackedge = &backedgeList()[i];

        info.backedge.fixup(&masm);
        CodeLocationJump backedge(code, info.backedge);
        CodeLocationLabel loopHeader(code, CodeOffset(info.loopHeader->offset()));
        CodeLocationLabel interruptCheck(code, CodeOffset(info.interruptCheck->offset()));
        new(patchableBackedge) PatchableBackedge(backedge, loopHeader, interruptCheck);

        // Point the backedge to either of its possible targets, matching the
        // other backedges in the runtime.
        if (jzg->backedgeTarget() == JitZoneGroup::BackedgeInterruptCheck)
            PatchBackedge(backedge, interruptCheck, JitZoneGroup::BackedgeInterruptCheck);
        else
            PatchBackedge(backedge, loopHeader, JitZoneGroup::BackedgeLoopHeader);

        jzg->addPatchableBackedge(cx->runtime()->jitRuntime(), patchableBackedge);
    }
}

void
IonScript::copySafepointIndices(const SafepointIndex* si)
{
    // Jumps in the caches reflect the offset of those jumps in the compiled
    // code, not the absolute positions of the jumps. Update according to the
    // final code address now.
    SafepointIndex* table = safepointIndices();
    memcpy(table, si, safepointIndexEntries_ * sizeof(SafepointIndex));
}

void
IonScript::copyOsiIndices(const OsiIndex* oi)
{
    memcpy(osiIndices(), oi, osiIndexEntries_ * sizeof(OsiIndex));
}

void
IonScript::copyRuntimeData(const uint8_t* data)
{
    memcpy(runtimeData(), data, runtimeSize());
}

void
IonScript::copyICEntries(const uint32_t* icEntries, MacroAssembler& masm)
{
    memcpy(icIndex(), icEntries, numICs() * sizeof(uint32_t));

    // Jumps in the caches reflect the offset of those jumps in the compiled
    // code, not the absolute positions of the jumps. Update according to the
    // final code address now.
    for (size_t i = 0; i < numICs(); i++)
        getICFromIndex(i).updateBaseAddress(method_, masm);
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
    const OsiIndex* end = osiIndices() + osiIndexEntries_;
    for (const OsiIndex* it = osiIndices(); it != end; ++it) {
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
    script->unlinkFromRuntime(fop);

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
JS::DeletePolicy<js::jit::IonScript>::operator()(const js::jit::IonScript* script)
{
    IonScript::Destroy(rt_->defaultFreeOp(), const_cast<IonScript*>(script));
}

void
IonScript::purgeOptimizedStubs(Zone* zone)
{
    for (size_t i = 0; i < numSharedStubs(); i++) {
        IonICEntry& entry = sharedStubList()[i];
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

            lastStub->toFallbackStub()->setInvalid();

            MOZ_ASSERT(!lastStub->isMonitoredFallback(),
                       "None of the shared stubs used in Ion are monitored");
        } else if (lastStub->isTypeMonitor_Fallback()) {
            lastStub->toTypeMonitor_Fallback()->resetMonitorStubChain(zone);
            lastStub->toTypeMonitor_Fallback()->setInvalid();
        } else {
            MOZ_ASSERT(lastStub->isTableSwitch());
        }
    }

#ifdef DEBUG
    // All remaining stubs must be allocated in the fallback space.
    for (size_t i = 0; i < numSharedStubs(); i++) {
        IonICEntry& entry = sharedStubList()[i];
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
IonScript::purgeICs(Zone* zone)
{
    for (size_t i = 0; i < numICs(); i++)
        getICFromIndex(i).reset(zone);
}

void
IonScript::unlinkFromRuntime(FreeOp* fop)
{
    // The writes to the executable buffer below may clobber backedge jumps, so
    // make sure that those backedges are unlinked from the runtime and not
    // reclobbered with garbage if an interrupt is requested.
    JitZoneGroup* jzg = method()->zone()->group()->jitZoneGroup;
    JitRuntime::AutoPreventBackedgePatching apbp(fop->runtime());
    for (size_t i = 0; i < backedgeEntries_; i++)
        jzg->removePatchableBackedge(fop->runtime()->jitRuntime(), &backedgeList()[i]);

    // Clear the list of backedges, so that this method is idempotent. It is
    // called during destruction, and may be additionally called when the
    // script is invalidated.
    backedgeEntries_ = 0;
}

namespace js {
namespace jit {

static void
OptimizeSinCos(MIRGraph &graph)
{
    // Now, we are looking for:
    // var y = sin(x);
    // var z = cos(x);
    // Graph before:
    // - 1 op
    // - 6 mathfunction op1 Sin
    // - 7 mathfunction op1 Cos
    // Graph will look like:
    // - 1 op
    // - 5 sincos op1
    // - 6 mathfunction sincos5 Sin
    // - 7 mathfunction sincos5 Cos
    for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
        for (MInstructionIterator iter(block->begin()), end(block->end()); iter != end; ) {
            MInstruction *ins = *iter++;
            if (!ins->isMathFunction() || ins->isRecoveredOnBailout())
                continue;

            MMathFunction *insFunc = ins->toMathFunction();
            if (insFunc->function() != MMathFunction::Sin && insFunc->function() != MMathFunction::Cos)
                continue;

            // Check if sin/cos is already optimized.
            if (insFunc->getOperand(0)->type() == MIRType::SinCosDouble)
                continue;

            // insFunc is either a |sin(x)| or |cos(x)| instruction. The
            // following loop iterates over the uses of |x| to check if both
            // |sin(x)| and |cos(x)| instructions exist.
            bool hasSin = false;
            bool hasCos = false;
            for (MUseDefIterator uses(insFunc->input()); uses; uses++)
            {
                if (!uses.def()->isInstruction())
                    continue;

                // We should replacing the argument of the sin/cos just when it
                // is dominated by the |block|.
                if (!block->dominates(uses.def()->block()))
                    continue;

                MInstruction *insUse = uses.def()->toInstruction();
                if (!insUse->isMathFunction() || insUse->isRecoveredOnBailout())
                    continue;

                MMathFunction *mathIns = insUse->toMathFunction();
                if (!hasSin && mathIns->function() == MMathFunction::Sin) {
                    hasSin = true;
                    JitSpew(JitSpew_Sincos, "Found sin in block %d.", mathIns->block()->id());
                }
                else if (!hasCos && mathIns->function() == MMathFunction::Cos) {
                    hasCos = true;
                    JitSpew(JitSpew_Sincos, "Found cos in block %d.", mathIns->block()->id());
                }

                if (hasCos && hasSin)
                    break;
            }

            if (!hasCos || !hasSin) {
                JitSpew(JitSpew_Sincos, "No sin/cos pair found.");
                continue;
            }

            JitSpew(JitSpew_Sincos, "Found, at least, a pair sin/cos. Adding sincos in block %d",
                    block->id());
            // Adding the MSinCos and replacing the parameters of the
            // sin(x)/cos(x) to sin(sincos(x))/cos(sincos(x)).
            MSinCos *insSinCos = MSinCos::New(graph.alloc(),
                                              insFunc->input(),
                                              insFunc->toMathFunction()->cache());
            insSinCos->setImplicitlyUsedUnchecked();
            block->insertBefore(insFunc, insSinCos);
            for (MUseDefIterator uses(insFunc->input()); uses; )
            {
                MDefinition* def = uses.def();
                uses++;
                if (!def->isInstruction())
                    continue;

                // We should replacing the argument of the sin/cos just when it
                // is dominated by the |block|.
                if (!block->dominates(def->block()))
                    continue;

                MInstruction *insUse = def->toInstruction();
                if (!insUse->isMathFunction() || insUse->isRecoveredOnBailout())
                    continue;

                MMathFunction *mathIns = insUse->toMathFunction();
                if (mathIns->function() != MMathFunction::Sin && mathIns->function() != MMathFunction::Cos)
                    continue;

                mathIns->replaceOperand(0, insSinCos);
                JitSpew(JitSpew_Sincos, "Replacing %s by sincos in block %d",
                        mathIns->function() == MMathFunction::Sin ? "sin" : "cos",
                        mathIns->block()->id());
            }
        }
    }
}

bool
OptimizeMIR(MIRGenerator* mir)
{
    MIRGraph& graph = mir->graph();
    GraphSpewer& gs = mir->graphSpewer();
    TraceLoggerThread* logger = TraceLoggerForCurrentThread();

    if (mir->shouldCancel("Start"))
        return false;

    if (!mir->compilingWasm()) {
        if (!MakeMRegExpHoistable(mir, graph))
            return false;

        if (mir->shouldCancel("Make MRegExp Hoistable"))
            return false;
    }

    gs.spewPass("BuildSSA");
    AssertBasicGraphCoherency(graph);

    if (!JitOptions.disablePgo && !mir->compilingWasm()) {
        AutoTraceLog log(logger, TraceLogger_PruneUnusedBranches);
        if (!PruneUnusedBranches(mir, graph))
            return false;
        gs.spewPass("Prune Unused Branches");
        AssertBasicGraphCoherency(graph);

        if (mir->shouldCancel("Prune Unused Branches"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_FoldEmptyBlocks);
        if (!FoldEmptyBlocks(graph))
            return false;
        gs.spewPass("Fold Empty Blocks");
        AssertBasicGraphCoherency(graph);

        if (mir->shouldCancel("Fold Empty Blocks"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_FoldTests);
        if (!FoldTests(graph))
            return false;
        gs.spewPass("Fold Tests");
        AssertBasicGraphCoherency(graph);

        if (mir->shouldCancel("Fold Tests"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_SplitCriticalEdges);
        if (!SplitCriticalEdges(graph))
            return false;
        gs.spewPass("Split Critical Edges");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("Split Critical Edges"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_RenumberBlocks);
        RenumberBlocks(graph);
        gs.spewPass("Renumber Blocks");
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
        gs.spewPass("Eliminate phis");
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

    if (!JitOptions.disableRecoverIns && mir->optimizationInfo().scalarReplacementEnabled()) {
        AutoTraceLog log(logger, TraceLogger_ScalarReplacement);
        if (!ScalarReplacement(mir, graph))
            return false;
        gs.spewPass("Scalar Replacement");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("Scalar Replacement"))
            return false;
    }

    if (!mir->compilingWasm()) {
        AutoTraceLog log(logger, TraceLogger_ApplyTypes);
        if (!ApplyTypeInformation(mir, graph))
            return false;
        gs.spewPass("Apply types");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Apply types"))
            return false;
    }

    if (!JitOptions.disableRecoverIns && mir->optimizationInfo().eagerSimdUnboxEnabled()) {
        AutoTraceLog log(logger, TraceLogger_EagerSimdUnbox);
        if (!EagerSimdUnbox(mir, graph))
            return false;
        gs.spewPass("Eager Simd Unbox");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("Eager Simd Unbox"))
            return false;
    }

    if (mir->optimizationInfo().amaEnabled()) {
        AutoTraceLog log(logger, TraceLogger_AlignmentMaskAnalysis);
        AlignmentMaskAnalysis ama(graph);
        if (!ama.analyze())
            return false;
        gs.spewPass("Alignment Mask Analysis");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Alignment Mask Analysis"))
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
        {
            AutoTraceLog log(logger, TraceLogger_AliasAnalysis);
            if (JitOptions.disableFlowAA) {
                AliasAnalysis analysis(mir, graph);
                if (!analysis.analyze())
                    return false;
            } else {
                FlowAliasAnalysis analysis(mir, graph);
                if (!analysis.analyze())
                    return false;
            }

            gs.spewPass("Alias analysis");
            AssertExtendedGraphCoherency(graph);

            if (mir->shouldCancel("Alias analysis"))
                return false;
        }

        if (!mir->compilingWasm()) {
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
        gs.spewPass("GVN");
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
            gs.spewPass("LICM");
            AssertExtendedGraphCoherency(graph);

            if (mir->shouldCancel("LICM"))
                return false;
        }
    }

    RangeAnalysis r(mir, graph);
    if (mir->optimizationInfo().rangeAnalysisEnabled()) {
        AutoTraceLog log(logger, TraceLogger_RangeAnalysis);
        if (!r.addBetaNodes())
            return false;
        gs.spewPass("Beta");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("RA Beta"))
            return false;

        if (!r.analyze() || !r.addRangeAssertions())
            return false;
        gs.spewPass("Range Analysis");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Range Analysis"))
            return false;

        if (!r.removeBetaNodes())
            return false;
        gs.spewPass("De-Beta");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("RA De-Beta"))
            return false;

        if (mir->optimizationInfo().gvnEnabled()) {
            bool shouldRunUCE = false;
            if (!r.prepareForUCE(&shouldRunUCE))
                return false;
            gs.spewPass("RA check UCE");
            AssertExtendedGraphCoherency(graph);

            if (mir->shouldCancel("RA check UCE"))
                return false;

            if (shouldRunUCE) {
                if (!gvn.run(ValueNumberer::DontUpdateAliasAnalysis))
                    return false;
                gs.spewPass("UCE After RA");
                AssertExtendedGraphCoherency(graph);

                if (mir->shouldCancel("UCE After RA"))
                    return false;
            }
        }

        if (mir->optimizationInfo().autoTruncateEnabled()) {
            if (!r.truncate())
                return false;
            gs.spewPass("Truncate Doubles");
            AssertExtendedGraphCoherency(graph);

            if (mir->shouldCancel("Truncate Doubles"))
                return false;
        }

        if (mir->optimizationInfo().loopUnrollingEnabled()) {
            AutoTraceLog log(logger, TraceLogger_LoopUnrolling);

            if (!UnrollLoops(graph, r.loopIterationBounds))
                return false;

            gs.spewPass("Unroll Loops");
            AssertExtendedGraphCoherency(graph);
        }
    }

    if (!JitOptions.disableRecoverIns) {
        AutoTraceLog log(logger, TraceLogger_Sink);
        if (!Sink(mir, graph))
            return false;
        gs.spewPass("Sink");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Sink"))
            return false;
    }

    if (!JitOptions.disableRecoverIns && mir->optimizationInfo().rangeAnalysisEnabled()) {
        AutoTraceLog log(logger, TraceLogger_RemoveUnnecessaryBitops);
        if (!r.removeUnnecessaryBitops())
            return false;
        gs.spewPass("Remove Unnecessary Bitops");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Remove Unnecessary Bitops"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_FoldLinearArithConstants);
        if (!FoldLinearArithConstants(mir, graph))
            return false;
        gs.spewPass("Fold Linear Arithmetic Constants");
        AssertBasicGraphCoherency(graph);

        if (mir->shouldCancel("Fold Linear Arithmetic Constants"))
            return false;
    }

    if (mir->optimizationInfo().eaaEnabled()) {
        AutoTraceLog log(logger, TraceLogger_EffectiveAddressAnalysis);
        EffectiveAddressAnalysis eaa(mir, graph);
        if (!eaa.analyze())
            return false;
        gs.spewPass("Effective Address Analysis");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Effective Address Analysis"))
            return false;
    }

    if (mir->optimizationInfo().sincosEnabled()) {
        AutoTraceLog log(logger, TraceLogger_Sincos);
        OptimizeSinCos(graph);
        gs.spewPass("Sincos optimization");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Sincos optimization"))
            return false;
    }

    // BCE marks bounds checks as dead, so do BCE before DCE.
    if (mir->compilingWasm()) {
        if (!EliminateBoundsChecks(mir, graph))
            return false;
        gs.spewPass("Redundant Bounds Check Elimination");
        AssertGraphCoherency(graph);

        if (mir->shouldCancel("BCE"))
            return false;
    }

    {
        AutoTraceLog log(logger, TraceLogger_EliminateDeadCode);
        if (!EliminateDeadCode(mir, graph))
            return false;
        gs.spewPass("DCE");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("DCE"))
            return false;
    }

    if (mir->optimizationInfo().instructionReorderingEnabled()) {
        AutoTraceLog log(logger, TraceLogger_ReorderInstructions);
        if (!ReorderInstructions(graph))
            return false;
        gs.spewPass("Reordering");

        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Reordering"))
            return false;
    }

    // Make loops contiguous. We do this after GVN/UCE and range analysis,
    // which can remove CFG edges, exposing more blocks that can be moved.
    {
        AutoTraceLog log(logger, TraceLogger_MakeLoopsContiguous);
        if (!MakeLoopsContiguous(graph))
            return false;
        gs.spewPass("Make loops contiguous");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("Make loops contiguous"))
            return false;
    }
    AssertExtendedGraphCoherency(graph, /* underValueNumberer = */ false, /* force = */ true);

    // Passes after this point must not move instructions; these analyses
    // depend on knowing the final order in which instructions will execute.

    if (mir->optimizationInfo().edgeCaseAnalysisEnabled()) {
        AutoTraceLog log(logger, TraceLogger_EdgeCaseAnalysis);
        EdgeCaseAnalysis edgeCaseAnalysis(mir, graph);
        if (!edgeCaseAnalysis.analyzeLate())
            return false;
        gs.spewPass("Edge Case Analysis (Late)");
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
        gs.spewPass("Bounds Check Elimination");
        AssertGraphCoherency(graph);
    }

    if (!mir->compilingWasm()) {
        AutoTraceLog log(logger, TraceLogger_AddKeepAliveInstructions);
        if (!AddKeepAliveInstructions(graph))
            return false;
        gs.spewPass("Add KeepAlive Instructions");
        AssertGraphCoherency(graph);
    }

    AssertGraphCoherency(graph, /* force = */ true);

    DumpMIRExpressions(graph);

    return true;
}

LIRGraph*
GenerateLIR(MIRGenerator* mir)
{
    MIRGraph& graph = mir->graph();
    GraphSpewer& gs = mir->graphSpewer();

    TraceLoggerThread* logger = TraceLoggerForCurrentThread();

    LIRGraph* lir = mir->alloc().lifoAlloc()->new_<LIRGraph>(&graph);
    if (!lir || !lir->init())
        return nullptr;

    LIRGenerator lirgen(mir, graph, *lir);
    {
        AutoTraceLog log(logger, TraceLogger_GenerateLIR);
        if (!lirgen.generate())
            return nullptr;
        gs.spewPass("Generate LIR");

        if (mir->shouldCancel("Generate LIR"))
            return nullptr;
    }

    AllocationIntegrityState integrity(*lir);

    {
        AutoTraceLog log(logger, TraceLogger_RegisterAllocation);

        IonRegisterAllocator allocator = mir->optimizationInfo().registerAllocator();

        switch (allocator) {
          case RegisterAllocator_Backtracking:
          case RegisterAllocator_Testbed: {
#ifdef DEBUG
            if (JitOptions.fullDebugChecks) {
                if (!integrity.record())
                    return nullptr;
            }
#endif

            BacktrackingAllocator regalloc(mir, &lirgen, *lir,
                                           allocator == RegisterAllocator_Testbed);
            if (!regalloc.go())
                return nullptr;

#ifdef DEBUG
            if (JitOptions.fullDebugChecks) {
                if (!integrity.check(false))
                    return nullptr;
            }
#endif

            gs.spewPass("Allocate Registers [Backtracking]");
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
            gs.spewPass("Allocate Registers [Stupid]");
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
    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
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
    AutoEnterIonCompilation enter(mir->safeForMinorGC());
    AutoSpewEndFunction spewEndFunction(mir);

    if (!OptimizeMIR(mir))
        return nullptr;

    LIRGraph* lir = GenerateLIR(mir);
    if (!lir)
        return nullptr;

    return GenerateCode(mir, lir);
}

// Find a builder which the current thread can finish.
static IonBuilder*
GetFinishedBuilder(ZoneGroup* group, GlobalHelperThreadState::IonBuilderVector& finished)
{
    for (size_t i = 0; i < finished.length(); i++) {
        IonBuilder* testBuilder = finished[i];
        if (testBuilder->script()->runtimeFromAnyThread() == group->runtime &&
            testBuilder->script()->zone()->group() == group) {
            HelperThreadState().remove(finished, &i);
            group->numFinishedBuilders--;
            return testBuilder;
        }
    }

    return nullptr;
}

void
AttachFinishedCompilations(ZoneGroup* group, JSContext* maybecx)
{
    MOZ_ASSERT_IF(maybecx, maybecx->zone()->group() == group);

    if (!group->numFinishedBuilders)
        return;

    AutoLockHelperThreadState lock;
    GlobalHelperThreadState::IonBuilderVector& finished = HelperThreadState().ionFinishedList(lock);

    // Incorporate any off thread compilations for the runtime which have
    // finished, failed or have been cancelled.
    while (true) {
        // Find a finished builder for the zone group.
        IonBuilder* builder = GetFinishedBuilder(group, finished);
        if (!builder)
            break;

        JSScript* script = builder->script();
        MOZ_ASSERT(script->hasBaselineScript());
        script->baselineScript()->setPendingIonBuilder(group->runtime, script, builder);
        group->ionLazyLinkListAdd(builder);

        // Don't keep more than 100 lazy link builders in a zone group.
        // Link the oldest ones immediately. Only do this if we have a valid
        // context to use (otherwise this method might have been called in the
        // middle of a compartment change on the current thread's context).
        if (maybecx) {
            while (group->ionLazyLinkListSize() > 100) {
                jit::IonBuilder* builder = group->ionLazyLinkList().getLast();
                RootedScript script(maybecx, builder->script());

                AutoUnlockHelperThreadState unlock(lock);
                AutoCompartment ac(maybecx, script);
                jit::LinkIonScript(maybecx, script);
            }
        }
    }

    MOZ_ASSERT(!group->numFinishedBuilders);
}

static void
TrackAllProperties(JSContext* cx, JSObject* obj)
{
    MOZ_ASSERT(obj->isSingleton());

    for (Shape::Range<NoGC> range(obj->as<NativeObject>().lastProperty()); !range.empty(); range.popFront())
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
        environment = environment->enclosingEnvironment();
    }

    if (baselineFrame) {
        JSObject* scope = baselineFrame->environmentChain();
        if (scope->is<CallObject>() && scope->isSingleton())
            TrackAllProperties(cx, scope);
    }
}

static void
TrackIonAbort(JSContext* cx, JSScript* script, jsbytecode* pc, const char* message)
{
    if (!cx->runtime()->jitRuntime()->isOptimizationTrackingEnabled(cx->zone()->group()))
        return;

    // Only bother tracking aborts of functions we're attempting to
    // Ion-compile after successfully running in Baseline.
    if (!script->hasBaselineScript())
        return;

    JitcodeGlobalTable* table = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
    void* ptr = script->baselineScript()->method()->raw();
    JitcodeGlobalEntry& entry = table->lookupInfallible(ptr);
    entry.baselineEntry().trackIonAbort(pc, message);
}

static void
TrackAndSpewIonAbort(JSContext* cx, JSScript* script, const char* message)
{
    JitSpew(JitSpew_IonAbort, "%s", message);
    TrackIonAbort(cx, script, script->code(), message);
}

static AbortReason
IonCompile(JSContext* cx, JSScript* script,
           BaselineFrame* baselineFrame, jsbytecode* osrPc,
           bool recompile, OptimizationLevel optimizationLevel)
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
    TraceLoggerEvent event(TraceLogger_AnnotateScripts, script);
    AutoTraceLog logScript(logger, event);
    AutoTraceLog logCompile(logger, TraceLogger_IonCompilation);

    // Make sure the script's canonical function isn't lazy. We can't de-lazify
    // it in a helper thread.
    script->ensureNonLazyCanonicalFunction();

    TrackPropertiesForSingletonScopes(cx, script, baselineFrame);

    LifoAlloc* alloc = cx->new_<LifoAlloc>(TempAllocator::PreferredLifoChunkSize);
    if (!alloc)
        return AbortReason::Alloc;

    ScopedJSDeletePtr<LifoAlloc> autoDelete(alloc);

    TempAllocator* temp = alloc->new_<TempAllocator>(alloc);
    if (!temp)
        return AbortReason::Alloc;

    JitContext jctx(cx, temp);

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return AbortReason::Alloc;

    if (!cx->compartment()->jitCompartment()->ensureIonStubsExist(cx))
        return AbortReason::Alloc;

    MIRGraph* graph = alloc->new_<MIRGraph>(temp);
    if (!graph)
        return AbortReason::Alloc;

    InlineScriptTree* inlineScriptTree = InlineScriptTree::New(temp, nullptr, nullptr, script);
    if (!inlineScriptTree)
        return AbortReason::Alloc;

    CompileInfo* info = alloc->new_<CompileInfo>(CompileRuntime::get(cx->runtime()),
                                                 script, script->functionNonDelazifying(), osrPc,
                                                 Analysis_None,
                                                 script->needsArgsObj(), inlineScriptTree);
    if (!info)
        return AbortReason::Alloc;

    BaselineInspector* inspector = alloc->new_<BaselineInspector>(script);
    if (!inspector)
        return AbortReason::Alloc;

    BaselineFrameInspector* baselineFrameInspector = nullptr;
    if (baselineFrame) {
        baselineFrameInspector = NewBaselineFrameInspector(temp, baselineFrame);
        if (!baselineFrameInspector)
            return AbortReason::Alloc;
    }

    CompilerConstraintList* constraints = NewCompilerConstraintList(*temp);
    if (!constraints)
        return AbortReason::Alloc;

    const OptimizationInfo* optimizationInfo = IonOptimizations.get(optimizationLevel);
    const JitCompileOptions options(cx);

    IonBuilder* builder = alloc->new_<IonBuilder>((JSContext*) nullptr,
                                                  CompileCompartment::get(cx->compartment()),
                                                  options, temp, graph, constraints,
                                                  inspector, info, optimizationInfo,
                                                  baselineFrameInspector);
    if (!builder)
        return AbortReason::Alloc;

    if (cx->zone()->group()->storeBuffer().cancelIonCompilations())
        builder->setNotSafeForMinorGC();

    MOZ_ASSERT(recompile == builder->script()->hasIonScript());
    MOZ_ASSERT(builder->script()->canIonCompile());

    RootedScript builderScript(cx, builder->script());

    if (recompile)
        builderScript->ionScript()->setRecompiling();

    SpewBeginFunction(builder, builderScript);

    AbortReasonOr<Ok> buildResult = Ok();
    {
        AutoEnterAnalysis enter(cx);
        buildResult = builder->build();
        builder->clearForBackEnd();
    }

    if (buildResult.isErr()) {
        AbortReason reason = buildResult.unwrapErr();
        builder->graphSpewer().endFunction();
        if (reason == AbortReason::PreliminaryObjects) {
            // Some group was accessed which has associated preliminary objects
            // to analyze. Do this now and we will try to build again shortly.
            const MIRGenerator::ObjectGroupVector& groups = builder->abortedPreliminaryGroups();
            for (size_t i = 0; i < groups.length(); i++) {
                ObjectGroup* group = groups[i];
                if (group->newScript()) {
                    if (!group->newScript()->maybeAnalyze(cx, group, nullptr, /* force = */ true))
                        return AbortReason::Alloc;
                } else if (group->maybePreliminaryObjects()) {
                    group->maybePreliminaryObjects()->maybeAnalyze(cx, group, /* force = */ true);
                } else {
                    MOZ_CRASH("Unexpected aborted preliminary group");
                }
            }
        }

        if (builder->hadActionableAbort()) {
            JSScript* abortScript;
            jsbytecode* abortPc;
            const char* abortMessage;
            builder->actionableAbortLocationAndMessage(&abortScript, &abortPc, &abortMessage);
            TrackIonAbort(cx, abortScript, abortPc, abortMessage);
        }

        if (cx->isThrowingOverRecursed()) {
            // Non-analysis compilations should never fail with stack overflow.
            MOZ_CRASH("Stack overflow during compilation");
        }

        return reason;
    }

    AssertBasicGraphCoherency(builder->graph());

    // If possible, compile the script off thread.
    if (options.offThreadCompilationAvailable()) {
        JitSpew(JitSpew_IonSyncLogs, "Can't log script %s:%zu"
                ". (Compiled on background thread.)",
                builderScript->filename(), builderScript->lineno());

        if (!CreateMIRRootList(*builder))
            return AbortReason::Alloc;

        AutoLockHelperThreadState lock;
        if (!StartOffThreadIonCompile(builder, lock)) {
            JitSpew(JitSpew_IonAbort, "Unable to start off-thread ion compilation.");
            builder->graphSpewer().endFunction();
            return AbortReason::Alloc;
        }

        if (!recompile)
            builderScript->setIonScript(cx->runtime(), ION_COMPILING_SCRIPT);

        // The allocator and associated data will be destroyed after being
        // processed in the finishedOffThreadCompilations list.
        autoDelete.forget();

        return AbortReason::NoAbort;
    }

    bool succeeded = false;
    {
        ScopedJSDeletePtr<CodeGenerator> codegen;
        AutoEnterAnalysis enter(cx);
        codegen = CompileBackEnd(builder);
        if (!codegen) {
            JitSpew(JitSpew_IonAbort, "Failed during back-end compilation.");
            if (cx->isExceptionPending())
                return AbortReason::Error;
            return AbortReason::Disable;
        }

        succeeded = LinkCodeGen(cx, builder, codegen);
    }

    if (succeeded)
        return AbortReason::NoAbort;
    if (cx->isExceptionPending())
        return AbortReason::Error;
    return AbortReason::Disable;
}

static bool
CheckFrame(JSContext* cx, BaselineFrame* frame)
{
    MOZ_ASSERT(!frame->script()->isGenerator());
    MOZ_ASSERT(!frame->script()->isAsync());
    MOZ_ASSERT(!frame->isDebuggerEvalFrame());
    MOZ_ASSERT(!frame->isEvalFrame());

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
CanIonCompileOrInlineScript(JSScript* script, const char** reason)
{
    if (script->isForEval()) {
        // Eval frames are not yet supported. Supporting this will require new
        // logic in pushBailoutFrame to deal with linking prev.
        // Additionally, JSOP_DEFVAR support will require baking in isEvalFrame().
        *reason = "eval script";
        return false;
    }

    if (script->isGenerator()) {
        *reason = "generator script";
        return false;
    }
    if (script->isAsync()) {
        *reason = "async script";
        return false;
    }

    if (script->hasNonSyntacticScope() && !script->functionNonDelazifying()) {
        // Support functions with a non-syntactic global scope but not other
        // scripts. For global scripts, IonBuilder currently uses the global
        // object as scope chain, this is not valid when the script has a
        // non-syntactic global scope.
        *reason = "has non-syntactic global scope";
        return false;
    }

    if (script->functionHasExtraBodyVarScope() &&
        script->functionExtraBodyVarScope()->hasEnvironment())
    {
        // This restriction will be lifted when intra-function scope chains
        // are compilable by Ion. See bug 1273858.
        *reason = "has extra var environment";
        return false;
    }

    if (script->nTypeSets() >= UINT16_MAX) {
        // In this case multiple bytecode ops can share a single observed
        // TypeSet (see bug 1303710).
        *reason = "too many typesets";
        return false;
    }

    return true;
}

static bool
ScriptIsTooLarge(JSContext* cx, JSScript* script)
{
    if (!JitOptions.limitScriptSize)
        return false;

    uint32_t numLocalsAndArgs = NumLocalsAndArgs(script);

    if (script->length() > MAX_ACTIVE_THREAD_SCRIPT_SIZE ||
        numLocalsAndArgs > MAX_ACTIVE_THREAD_LOCALS_AND_ARGS)
    {
        if (!OffThreadCompilationAvailable(cx)) {
            JitSpew(JitSpew_IonAbort, "Script too large (%zu bytes) (%u locals/args)",
                    script->length(), numLocalsAndArgs);
            TrackIonAbort(cx, script, script->code(), "too large");
            return true;
        }
    }

    return false;
}

bool
CanIonCompileScript(JSContext* cx, JSScript* script)
{
    if (!script->canIonCompile())
        return false;

    const char* reason = nullptr;
    if (!CanIonCompileOrInlineScript(script, &reason)) {
        TrackAndSpewIonAbort(cx, script, reason);
        return false;
    }

    if (ScriptIsTooLarge(cx, script))
        return false;

    return true;
}

bool
CanIonInlineScript(JSScript* script)
{
    if (!script->canIonCompile())
        return false;

    const char* reason = nullptr;
    if (!CanIonCompileOrInlineScript(script, &reason)) {
        JitSpew(JitSpew_Inlining, "Cannot Ion compile script (%s)", reason);
        return false;
    }

    return true;
}

static OptimizationLevel
GetOptimizationLevel(HandleScript script, jsbytecode* pc)
{
    return IonOptimizations.levelForScript(script, pc);
}

static MethodStatus
Compile(JSContext* cx, HandleScript script, BaselineFrame* osrFrame, jsbytecode* osrPc,
        bool forceRecompile = false)
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

    if (!CanIonCompileScript(cx, script)) {
        JitSpew(JitSpew_IonAbort, "Aborted compilation of %s:%zu", script->filename(), script->lineno());
        return Method_CantCompile;
    }

    bool recompile = false;
    OptimizationLevel optimizationLevel = GetOptimizationLevel(script, osrPc);
    if (optimizationLevel == OptimizationLevel::DontCompile)
        return Method_Skipped;

    if (!CanLikelyAllocateMoreExecutableMemory()) {
        script->resetWarmUpCounter();
        return Method_Skipped;
    }

    if (script->hasIonScript()) {
        IonScript* scriptIon = script->ionScript();
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

    if (script->baselineScript()->hasPendingIonBuilder()) {
        IonBuilder* buildIon = script->baselineScript()->pendingIonBuilder();
        if (optimizationLevel <= buildIon->optimizationInfo().level() && !forceRecompile)
            return Method_Compiled;

        recompile = true;
    }

    AbortReason reason = IonCompile(cx, script, osrFrame, osrPc, recompile, optimizationLevel);
    if (reason == AbortReason::Error)
        return Method_Error;

    if (reason == AbortReason::Disable)
        return Method_CantCompile;

    if (reason == AbortReason::Alloc) {
        ReportOutOfMemory(cx);
        return Method_Error;
    }

    // Compilation succeeded or we invalidated right away or an inlining/alloc abort
    if (script->hasIonScript())
        return Method_Compiled;
    return Method_Skipped;
}

} // namespace jit
} // namespace js

bool
jit::OffThreadCompilationAvailable(JSContext* cx)
{
    // Even if off thread compilation is enabled, compilation must still occur
    // on the active thread in some cases.
    //
    // Require cpuCount > 1 so that Ion compilation jobs and active-thread
    // execution are not competing for the same resources.
    return cx->runtime()->canUseOffthreadIonCompilation()
        && HelperThreadState().cpuCount > 1
        && CanUseExtraThreads();
}

MethodStatus
jit::CanEnterIon(JSContext* cx, RunState& state)
{
    MOZ_ASSERT(jit::IsIonEnabled(cx));

    HandleScript script = state.script();

    // Skip if the script has been disabled.
    if (!script->canIonCompile())
        return Method_Skipped;

    // Skip if the script is being compiled off thread.
    if (script->isIonCompilingOffThread())
        return Method_Skipped;

    // Skip if the code is expected to result in a bailout.
    if (script->hasIonScript() && script->ionScript()->bailoutExpected())
        return Method_Skipped;

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
    }

    // If --ion-eager is used, compile with Baseline first, so that we
    // can directly enter IonMonkey.
    if (JitOptions.eagerCompilation && !script->hasBaselineScript()) {
        MethodStatus status = CanEnterBaselineMethod(cx, state);
        if (status != Method_Compiled)
            return status;
    }

    MOZ_ASSERT(!script->isIonCompilingOffThread());
    MOZ_ASSERT(script->canIonCompile());

    // Attempt compilation. Returns Method_Compiled if already compiled.
    MethodStatus status = Compile(cx, script, nullptr, nullptr);
    if (status != Method_Compiled) {
        if (status == Method_CantCompile)
            ForbidCompilation(cx, script);
        return status;
    }

    if (state.script()->baselineScript()->hasPendingIonBuilder()) {
        LinkIonScript(cx, state.script());
        if (!state.script()->hasIonScript())
            return jit::Method_Skipped;
    }

    return Method_Compiled;
}

static MethodStatus
BaselineCanEnterAtEntry(JSContext* cx, HandleScript script, BaselineFrame* frame)
{
    MOZ_ASSERT(jit::IsIonEnabled(cx));
    MOZ_ASSERT(frame->callee()->nonLazyScript()->canIonCompile());
    MOZ_ASSERT(!frame->callee()->nonLazyScript()->isIonCompilingOffThread());
    MOZ_ASSERT(!frame->callee()->nonLazyScript()->hasIonScript());
    MOZ_ASSERT(frame->isFunctionFrame());

    // Mark as forbidden if frame can't be handled.
    if (!CheckFrame(cx, frame)) {
        ForbidCompilation(cx, script);
        return Method_CantCompile;
    }

    // Attempt compilation. Returns Method_Compiled if already compiled.
    MethodStatus status = Compile(cx, script, frame, nullptr);
    if (status != Method_Compiled) {
        if (status == Method_CantCompile)
            ForbidCompilation(cx, script);
        return status;
    }

    return Method_Compiled;
}

// Decide if a transition from baseline execution to Ion code should occur.
// May compile or recompile the target JSScript.
static MethodStatus
BaselineCanEnterAtBranch(JSContext* cx, HandleScript script, BaselineFrame* osrFrame, jsbytecode* pc)
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
    if (!JitOptions.osr)
        return Method_Skipped;

    // Mark as forbidden if frame can't be handled.
    if (!CheckFrame(cx, osrFrame)) {
        ForbidCompilation(cx, script);
        return Method_CantCompile;
    }

    // Check if the jitcode still needs to get linked and do this
    // to have a valid IonScript.
    if (script->baselineScript()->hasPendingIonBuilder())
        LinkIonScript(cx, script);

    // By default a recompilation doesn't happen on osr mismatch.
    // Decide if we want to force a recompilation if this happens too much.
    bool force = false;
    if (script->hasIonScript() && pc != script->ionScript()->osrPc()) {
        uint32_t count = script->ionScript()->incrOsrPcMismatchCounter();
        if (count <= JitOptions.osrPcMismatchesBeforeRecompile)
            return Method_Skipped;
        force = true;
    }

    // Attempt compilation.
    // - Returns Method_Compiled if the right ionscript is present
    //   (Meaning it was present or a sequantial compile finished)
    // - Returns Method_Skipped if pc doesn't match
    //   (This means a background thread compilation with that pc could have started or not.)
    MethodStatus status = Compile(cx, script, osrFrame, pc, force);
    if (status != Method_Compiled) {
        if (status == Method_CantCompile)
            ForbidCompilation(cx, script);
        return status;
    }

    // Return the compilation was skipped when the osr pc wasn't adjusted.
    // This can happen when there was still an IonScript available and a
    // background compilation started, but hasn't finished yet.
    // Or when we didn't force a recompile.
    if (script->hasIonScript() && pc != script->ionScript()->osrPc())
        return Method_Skipped;

    return Method_Compiled;
}

bool
jit::IonCompileScriptForBaseline(JSContext* cx, BaselineFrame* frame, jsbytecode* pc)
{
    // A TI OOM will disable TI and Ion.
    if (!jit::IsIonEnabled(cx))
        return true;

    RootedScript script(cx, frame->script());
    bool isLoopEntry = JSOp(*pc) == JSOP_LOOPENTRY;

    MOZ_ASSERT(!isLoopEntry || LoopEntryCanIonOsr(pc));

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
            "WarmUpCounter for %s:%zu reached %d at pc %p, trying to switch to Ion!",
            script->filename(), script->lineno(), (int) script->getWarmUpCount(), (void*) pc);

    MethodStatus stat;
    if (isLoopEntry) {
        MOZ_ASSERT(LoopEntryCanIonOsr(pc));
        JitSpew(JitSpew_BaselineOSR, "  Compile at loop entry!");
        stat = BaselineCanEnterAtBranch(cx, script, frame, pc);
    } else if (frame->isFunctionFrame()) {
        JitSpew(JitSpew_BaselineOSR, "  Compile function from top for later entry!");
        stat = BaselineCanEnterAtEntry(cx, script, frame);
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

    return true;
}


MethodStatus
jit::Recompile(JSContext* cx, HandleScript script, BaselineFrame* osrFrame, jsbytecode* osrPc,
               bool force)
{
    MOZ_ASSERT(script->hasIonScript());
    if (script->ionScript()->isRecompiling())
        return Method_Compiled;

    MethodStatus status = Compile(cx, script, osrFrame, osrPc, force);
    if (status != Method_Compiled) {
        if (status == Method_CantCompile)
            ForbidCompilation(cx, script);
        return status;
    }

    return Method_Compiled;
}

static void
InvalidateActivation(FreeOp* fop, const JitActivationIterator& activations, bool invalidateAll)
{
    JitSpew(JitSpew_IonInvalidate, "BEGIN invalidating activation");

#ifdef CHECK_OSIPOINT_REGISTERS
    if (JitOptions.checkOsiPointRegisters)
        activations->asJit()->setCheckRegs(false);
#endif

    size_t frameno = 1;

    for (OnlyJSJitFrameIter iter(activations); !iter.done(); ++iter, ++frameno) {
        const JSJitFrameIter& frame = iter.frame();
        MOZ_ASSERT_IF(frameno == 1, frame.isExitFrame() ||
                                    frame.type() == JitFrame_Bailout ||
                                    frame.type() == JitFrame_JSJitToWasm);

#ifdef JS_JITSPEW
        switch (frame.type()) {
          case JitFrame_Exit:
            JitSpew(JitSpew_IonInvalidate, "#%zu exit frame @ %p", frameno, frame.fp());
            break;
          case JitFrame_JSJitToWasm:
            JitSpew(JitSpew_IonInvalidate, "#%zu wasm exit frame @ %p", frameno, frame.fp());
            break;
          case JitFrame_BaselineJS:
          case JitFrame_IonJS:
          case JitFrame_Bailout:
          {
            MOZ_ASSERT(frame.isScripted());
            const char* type = "Unknown";
            if (frame.isIonJS())
                type = "Optimized";
            else if (frame.isBaselineJS())
                type = "Baseline";
            else if (frame.isBailoutJS())
                type = "Bailing";
            JitSpew(JitSpew_IonInvalidate,
                    "#%zu %s JS frame @ %p, %s:%zu (fun: %p, script: %p, pc %p)",
                    frameno, type, frame.fp(), frame.script()->maybeForwardedFilename(),
                    frame.script()->lineno(), frame.maybeCallee(), (JSScript*)frame.script(),
                    frame.returnAddressToFp());
            break;
          }
          case JitFrame_BaselineStub:
            JitSpew(JitSpew_IonInvalidate, "#%zu baseline stub frame @ %p", frameno, frame.fp());
            break;
          case JitFrame_Rectifier:
            JitSpew(JitSpew_IonInvalidate, "#%zu rectifier frame @ %p", frameno, frame.fp());
            break;
          case JitFrame_IonICCall:
            JitSpew(JitSpew_IonInvalidate, "#%zu ion IC call frame @ %p", frameno, frame.fp());
            break;
          case JitFrame_CppToJSJit:
            JitSpew(JitSpew_IonInvalidate, "#%zu entry frame @ %p", frameno, frame.fp());
            break;
          case JitFrame_WasmToJSJit:
            JitSpew(JitSpew_IonInvalidate, "#%zu wasm frames @ %p", frameno, frame.fp());
            break;
        }
#endif // JS_JITSPEW

        if (!frame.isIonScripted())
            continue;

        // See if the frame has already been invalidated.
        if (frame.checkInvalidation())
            continue;

        JSScript* script = frame.script();
        if (!script->hasIonScript())
            continue;

        if (!invalidateAll && !script->ionScript()->invalidated())
            continue;

        IonScript* ionScript = script->ionScript();

        // Purge ICs before we mark this script as invalidated. This will
        // prevent lastJump_ from appearing to be a bogus pointer, just
        // in case anyone tries to read it.
        ionScript->purgeICs(script->zone());
        ionScript->purgeOptimizedStubs(script->zone());

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
            ionCode->traceChildren(zone->barrierTracer());
        }
        ionCode->setInvalidated();

        // Don't adjust OSI points in a bailout path.
        if (frame.isBailoutJS())
            continue;

        // Write the delta (from the return address offset to the
        // IonScript pointer embedded into the invalidation epilogue)
        // where the safepointed call instruction used to be. We rely on
        // the call sequence causing the safepoint being >= the size of
        // a uint32, which is checked during safepoint index
        // construction.
        AutoWritableJitCode awjc(ionCode);
        const SafepointIndex* si = ionScript->getSafepointIndex(frame.returnAddressToFp());
        CodeLocationLabel dataLabelToMunge(frame.returnAddressToFp());
        ptrdiff_t delta = ionScript->invalidateEpilogueDataOffset() -
                          (frame.returnAddressToFp() - ionCode->raw());
        Assembler::PatchWrite_Imm32(dataLabelToMunge, Imm32(delta));

        CodeLocationLabel osiPatchPoint = SafepointReader::InvalidationPatchPoint(ionScript, si);
        CodeLocationLabel invalidateEpilogue(ionCode, CodeOffset(ionScript->invalidateEpilogueOffset()));

        JitSpew(JitSpew_IonInvalidate, "   ! Invalidate ionScript %p (inv count %zu) -> patching osipoint %p",
                ionScript, ionScript->invalidationCount(), (void*) osiPatchPoint.raw());
        Assembler::PatchWrite_NearCall(osiPatchPoint, invalidateEpilogue);
    }

    JitSpew(JitSpew_IonInvalidate, "END invalidating activation");
}

void
jit::InvalidateAll(FreeOp* fop, Zone* zone)
{
    // The caller should previously have cancelled off thread compilation.
#ifdef DEBUG
    for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next())
        MOZ_ASSERT(!HasOffThreadIonCompile(comp));
#endif
    if (zone->isAtomsZone())
        return;
    JSContext* cx = TlsContext.get();
    for (const CooperatingContext& target : cx->runtime()->cooperatingContexts()) {
        for (JitActivationIterator iter(cx, target); !iter.done(); ++iter) {
            if (iter->compartment()->zone() == zone) {
                JitSpew(JitSpew_IonInvalidate, "Invalidating all frames for GC");
                InvalidateActivation(fop, iter, true);
            }
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
            CancelOffThreadIonCompile(co->script());

        if (!co->ion())
            continue;

        JitSpew(JitSpew_IonInvalidate, " Invalidate %s:%zu, IonScript %p",
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

    // This method can be called both during GC and during the course of normal
    // script execution. In the former case this class will already be on the
    // stack, and in the latter case the invalidations will all be on the
    // current thread's stack, but the assertion under ActivationIterator can't
    // tell that this is a thread local use of the iterator.
    JSRuntime::AutoProhibitActiveContextChange apacc(fop->runtime());

    JSContext* cx = TlsContext.get();
    for (const CooperatingContext& target : cx->runtime()->cooperatingContexts()) {
        for (JitActivationIterator iter(cx, target); !iter.done(); ++iter)
            InvalidateActivation(fop, iter, false);
    }

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

        script->setIonScript(cx->runtime(), nullptr);
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

void
jit::IonScript::invalidate(JSContext* cx, bool resetUses, const char* reason)
{
    JitSpew(JitSpew_IonInvalidate, " Invalidate IonScript %p: %s", this, reason);

    // RecompileInfoVector has inline space for at least one element.
    RecompileInfoVector list;
    MOZ_RELEASE_ASSERT(list.reserve(1));
    list.infallibleAppend(recompileInfo());

    Invalidate(cx, list, resetUses, true);
}

void
jit::Invalidate(JSContext* cx, JSScript* script, bool resetUses, bool cancelOffThread)
{
    MOZ_ASSERT(script->hasIonScript());

    if (cx->runtime()->geckoProfiler().enabled()) {
        // Register invalidation with profiler.
        // Format of event payload string:
        //      "<filename>:<lineno>"

        // Get the script filename, if any, and its length.
        const char* filename = script->filename();
        if (filename == nullptr)
            filename = "<unknown>";

        // Construct the descriptive string.
        UniqueChars buf = JS_smprintf("Invalidate %s:%zu", filename, script->lineno());

        // Ignore the event on allocation failure.
        if (buf) {
            cx->runtime()->geckoProfiler().markEvent(buf.get());
        }
    }

    // RecompileInfoVector has inline space for at least one element.
    RecompileInfoVector scripts;
    MOZ_ASSERT(script->hasIonScript());
    MOZ_RELEASE_ASSERT(scripts.reserve(1));
    scripts.infallibleAppend(script->ionScript()->recompileInfo());

    Invalidate(cx, scripts, resetUses, cancelOffThread);
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
        script->setIonScript(fop->runtime(), nullptr);
        FinishInvalidationOf(fop, script, ion);
    }
}

void
jit::ForbidCompilation(JSContext* cx, JSScript* script)
{
    JitSpew(JitSpew_IonAbort, "Disabling Ion compilation of script %s:%zu",
            script->filename(), script->lineno());

    CancelOffThreadIonCompile(script);

    if (script->hasIonScript())
        Invalidate(cx, script, false);

    script->setIonScript(cx->runtime(), ION_DISABLED_SCRIPT);
}

AutoFlushICache*
JSContext::autoFlushICache() const
{
    return autoFlushICache_;
}

void
JSContext::setAutoFlushICache(AutoFlushICache* afc)
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
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    AutoFlushICache* afc = TlsContext.get()->autoFlushICache();
    MOZ_ASSERT(afc);
    MOZ_ASSERT(!afc->start_);
    JitSpewCont(JitSpew_CacheFlush, "(%" PRIxPTR " %zx):", start, len);

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
// indicate a program fault but it might indicate a lost opportunity to merge cache
// flushing.  It can be corrected by wrapping the call in an AutoFlushICache to context.
//
// Note this can be called without TLS JSContext defined so this case needs
// to be guarded against. E.g. when patching instructions from the exception
// handler on MacOS running the ARM simulator.
void
AutoFlushICache::flush(uintptr_t start, size_t len)
{
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_NONE)
    // Nothing
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
      defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    JSContext* cx = TlsContext.get();
    AutoFlushICache* afc = cx ? cx->autoFlushICache() : nullptr;
    if (!afc) {
        JitSpewCont(JitSpew_CacheFlush, "#");
        ExecutableAllocator::cacheFlush((void*)start, len);
        MOZ_ASSERT(len <= 32);
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
#else
    MOZ_CRASH("Unresolved porting API - AutoFlushICache::flush");
#endif
}

// Flag the current dynamic AutoFlushICache as inhibiting flushing. Useful in error paths
// where the changes are being abandoned.
void
AutoFlushICache::setInhibit()
{
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_NONE)
    // Nothing
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
      defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    AutoFlushICache* afc = TlsContext.get()->autoFlushICache();
    MOZ_ASSERT(afc);
    MOZ_ASSERT(afc->start_);
    JitSpewCont(JitSpew_CacheFlush, "I");
    afc->inhibit_ = true;
#else
    MOZ_CRASH("Unresolved porting API - AutoFlushICache::setInhibit");
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
// In some cases flushing is not necessary, such as when compiling an wasm module which
// is flushed again when dynamically linked, and also in error paths that abandon the
// code.  Flushing within the set code range can be inhibited within the AutoFlushICache
// dynamic context by setting an inhibit flag.
//
// The JS compiler can be re-entered while within an AutoFlushICache dynamic context and
// it is assumed that code being assembled or patched is not executed before the exit of
// the respective AutoFlushICache dynamic context.
//
AutoFlushICache::AutoFlushICache(const char* nonce, bool inhibit)
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  : start_(0),
    stop_(0),
#ifdef JS_JITSPEW
    name_(nonce),
#endif
    inhibit_(inhibit)
#endif
{
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    JSContext* cx = TlsContext.get();
    AutoFlushICache* afc = cx->autoFlushICache();
    if (afc)
        JitSpew(JitSpew_CacheFlush, "<%s,%s%s ", nonce, afc->name_, inhibit ? " I" : "");
    else
        JitSpewCont(JitSpew_CacheFlush, "<%s%s ", nonce, inhibit ? " I" : "");

    prev_ = afc;
    cx->setAutoFlushICache(this);
#endif
}

AutoFlushICache::~AutoFlushICache()
{
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    JSContext* cx = TlsContext.get();
    MOZ_ASSERT(cx->autoFlushICache() == this);

    if (!inhibit_ && start_)
        ExecutableAllocator::cacheFlush((void*)start_, size_t(stop_ - start_));

    JitSpewCont(JitSpew_CacheFlush, "%s%s>", name_, start_ ? "" : " U");
    JitSpewFin(JitSpew_CacheFlush);
    cx->setAutoFlushICache(prev_);
#endif
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
jit::JitSupportsUnalignedAccesses()
{
    return js::jit::MacroAssembler::SupportsUnalignedAccesses();
}

bool
jit::JitSupportsSimd()
{
    return js::jit::MacroAssembler::SupportsSimd();
}

bool
jit::JitSupportsAtomics()
{
#if defined(JS_CODEGEN_ARM)
    // Bug 1146902, bug 1077318: Enable Ion inlining of Atomics
    // operations on ARM only when the CPU has byte, halfword, and
    // doubleword load-exclusive and store-exclusive instructions,
    // until we can add support for systems that don't have those.
    return js::jit::HasLDSTREXBHD();
#else
    return true;
#endif
}

// If you change these, please also change the comment in TempAllocator.
/* static */ const size_t TempAllocator::BallastSize            = 16 * 1024;
/* static */ const size_t TempAllocator::PreferredLifoChunkSize = 32 * 1024;
