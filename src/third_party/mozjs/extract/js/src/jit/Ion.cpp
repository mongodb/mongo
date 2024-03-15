/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Ion.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/ThreadLocal.h"

#include "gc/GCContext.h"
#include "gc/PublicIterators.h"
#include "jit/AliasAnalysis.h"
#include "jit/AlignmentMaskAnalysis.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/BacktrackingAllocator.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineJIT.h"
#include "jit/CodeGenerator.h"
#include "jit/CompileInfo.h"
#include "jit/EdgeCaseAnalysis.h"
#include "jit/EffectiveAddressAnalysis.h"
#include "jit/ExecutableAllocator.h"
#include "jit/FoldLinearArithConstants.h"
#include "jit/InlineScriptTree.h"
#include "jit/InstructionReordering.h"
#include "jit/Invalidation.h"
#include "jit/IonAnalysis.h"
#include "jit/IonCompileTask.h"
#include "jit/IonIC.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/IonScript.h"
#include "jit/JitcodeMap.h"
#include "jit/JitFrames.h"
#include "jit/JitRealm.h"
#include "jit/JitRuntime.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/LICM.h"
#include "jit/Linker.h"
#include "jit/LIR.h"
#include "jit/Lowering.h"
#include "jit/PerfSpewer.h"
#include "jit/RangeAnalysis.h"
#include "jit/ScalarReplacement.h"
#include "jit/ScriptFromCalleeToken.h"
#include "jit/Sink.h"
#include "jit/ValueNumbering.h"
#include "jit/WarpBuilder.h"
#include "jit/WarpOracle.h"
#include "jit/WasmBCE.h"
#include "js/Printf.h"
#include "js/UniquePtr.h"
#include "util/Memory.h"
#include "util/WindowsWrapper.h"
#include "vm/HelperThreads.h"
#include "vm/Realm.h"
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif

#include "gc/GC-inl.h"
#include "gc/StableCellHasher-inl.h"
#include "jit/InlineScriptTree-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/SafepointIndex-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/Realm-inl.h"

#if defined(ANDROID)
#  include <sys/system_properties.h>
#endif

using mozilla::CheckedInt;
using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

JitRuntime::~JitRuntime() {
  MOZ_ASSERT(numFinishedOffThreadTasks_ == 0);
  MOZ_ASSERT(ionLazyLinkListSize_ == 0);
  MOZ_ASSERT(ionLazyLinkList_.ref().isEmpty());

  // By this point, the jitcode global table should be empty.
  MOZ_ASSERT_IF(jitcodeGlobalTable_, jitcodeGlobalTable_->empty());
  js_delete(jitcodeGlobalTable_.ref());

  // interpreterEntryMap should be cleared out during finishRoots()
  MOZ_ASSERT_IF(interpreterEntryMap_, interpreterEntryMap_->empty());
  js_delete(interpreterEntryMap_.ref());

  js_delete(jitHintsMap_.ref());
}

uint32_t JitRuntime::startTrampolineCode(MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "startTrampolineCode");

  masm.assumeUnreachable("Shouldn't get here");
  masm.flushBuffer();
  masm.haltingAlign(CodeAlignment);
  masm.setFramePushed(0);
  return masm.currentOffset();
}

bool JitRuntime::initialize(JSContext* cx) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));

  AutoAllocInAtomsZone az(cx);
  JitContext jctx(cx);

  if (!generateTrampolines(cx)) {
    return false;
  }

  if (!generateBaselineICFallbackCode(cx)) {
    return false;
  }

  jitcodeGlobalTable_ = cx->new_<JitcodeGlobalTable>();
  if (!jitcodeGlobalTable_) {
    return false;
  }

  if (!JitOptions.disableJitHints) {
    jitHintsMap_ = cx->new_<JitHintsMap>();
    if (!jitHintsMap_) {
      return false;
    }
  }

  if (JitOptions.emitInterpreterEntryTrampoline) {
    interpreterEntryMap_ = cx->new_<EntryTrampolineMap>();
    if (!interpreterEntryMap_) {
      return false;
    }
  }

  if (!GenerateBaselineInterpreter(cx, baselineInterpreter_)) {
    return false;
  }

  // Initialize the jitCodeRaw of the Runtime's canonical SelfHostedLazyScript
  // to point to the interpreter trampoline.
  cx->runtime()->selfHostedLazyScript.ref().jitCodeRaw_ =
      interpreterStub().value;

  return true;
}

bool JitRuntime::generateTrampolines(JSContext* cx) {
  TempAllocator temp(&cx->tempLifoAlloc());
  StackMacroAssembler masm(cx, temp);
  PerfSpewerRangeRecorder rangeRecorder(masm);

  Label bailoutTail;
  JitSpew(JitSpew_Codegen, "# Emitting bailout tail stub");
  generateBailoutTailStub(masm, &bailoutTail);

  JitSpew(JitSpew_Codegen, "# Emitting bailout handler");
  generateBailoutHandler(masm, &bailoutTail);
  rangeRecorder.recordOffset("Trampoline: Bailout");

  JitSpew(JitSpew_Codegen, "# Emitting invalidator");
  generateInvalidator(masm, &bailoutTail);
  rangeRecorder.recordOffset("Trampoline: Invalidator");

  // The arguments rectifier has to use the same frame layout as the function
  // frames it rectifies.
  static_assert(std::is_base_of_v<JitFrameLayout, RectifierFrameLayout>,
                "a rectifier frame can be used with jit frame");
  static_assert(std::is_base_of_v<JitFrameLayout, WasmToJSJitFrameLayout>,
                "wasm frames simply are jit frames");
  static_assert(sizeof(JitFrameLayout) == sizeof(WasmToJSJitFrameLayout),
                "thus a rectifier frame can be used with a wasm frame");

  JitSpew(JitSpew_Codegen, "# Emitting arguments rectifier");
  generateArgumentsRectifier(masm, ArgumentsRectifierKind::Normal);
  rangeRecorder.recordOffset("Trampoline: Arguments Rectifier");

  JitSpew(JitSpew_Codegen, "# Emitting trial inlining arguments rectifier");
  generateArgumentsRectifier(masm, ArgumentsRectifierKind::TrialInlining);
  rangeRecorder.recordOffset(
      "Trampoline: Arguments Rectifier (Trial Inlining)");

  JitSpew(JitSpew_Codegen, "# Emitting EnterJIT sequence");
  generateEnterJIT(cx, masm);
  rangeRecorder.recordOffset("Trampoline: EnterJIT");

  JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Value");
  valuePreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::Value);
  rangeRecorder.recordOffset("Trampoline: PreBarrier Value");

  JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for String");
  stringPreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::String);
  rangeRecorder.recordOffset("Trampoline: PreBarrier String");

  JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Object");
  objectPreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::Object);
  rangeRecorder.recordOffset("Trampoline: PreBarrier Object");

  JitSpew(JitSpew_Codegen, "# Emitting Pre Barrier for Shape");
  shapePreBarrierOffset_ = generatePreBarrier(cx, masm, MIRType::Shape);
  rangeRecorder.recordOffset("Trampoline: PreBarrier Shape");

  JitSpew(JitSpew_Codegen, "# Emitting free stub");
  generateFreeStub(masm);
  rangeRecorder.recordOffset("Trampoline: FreeStub");

  JitSpew(JitSpew_Codegen, "# Emitting lazy link stub");
  generateLazyLinkStub(masm);
  rangeRecorder.recordOffset("Trampoline: LazyLinkStub");

  JitSpew(JitSpew_Codegen, "# Emitting interpreter stub");
  generateInterpreterStub(masm);
  rangeRecorder.recordOffset("Trampoline: Interpreter");

  JitSpew(JitSpew_Codegen, "# Emitting double-to-int32-value stub");
  generateDoubleToInt32ValueStub(masm);
  rangeRecorder.recordOffset("Trampoline: DoubleToInt32ValueStub");

  JitSpew(JitSpew_Codegen, "# Emitting VM function wrappers");
  if (!generateVMWrappers(cx, masm)) {
    return false;
  }
  rangeRecorder.recordOffset("Trampoline: VM Wrapper");

  JitSpew(JitSpew_Codegen, "# Emitting profiler exit frame tail stub");
  Label profilerExitTail;
  generateProfilerExitFrameTailStub(masm, &profilerExitTail);
  rangeRecorder.recordOffset("Trampoline: ProfilerExitFrameTailStub");

  JitSpew(JitSpew_Codegen, "# Emitting exception tail stub");
  generateExceptionTailStub(masm, &profilerExitTail, &bailoutTail);
  rangeRecorder.recordOffset("Trampoline: ExceptionTailStub");

  Linker linker(masm);
  trampolineCode_ = linker.newCode(cx, CodeKind::Other);
  if (!trampolineCode_) {
    return false;
  }

  rangeRecorder.collectRangesForJitCode(trampolineCode_);
#ifdef MOZ_VTUNE
  vtune::MarkStub(trampolineCode_, "Trampolines");
#endif

  return true;
}

JitCode* JitRuntime::debugTrapHandler(JSContext* cx,
                                      DebugTrapHandlerKind kind) {
  if (!debugTrapHandlers_[kind]) {
    // JitRuntime code stubs are shared across compartments and have to
    // be allocated in the atoms zone.
    mozilla::Maybe<AutoAllocInAtomsZone> az;
    if (!cx->zone()->isAtomsZone()) {
      az.emplace(cx);
    }
    debugTrapHandlers_[kind] = generateDebugTrapHandler(cx, kind);
  }
  return debugTrapHandlers_[kind];
}

JitRuntime::IonCompileTaskList& JitRuntime::ionLazyLinkList(JSRuntime* rt) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt),
             "Should only be mutated by the main thread.");
  return ionLazyLinkList_.ref();
}

void JitRuntime::ionLazyLinkListRemove(JSRuntime* rt,
                                       jit::IonCompileTask* task) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt),
             "Should only be mutated by the main thread.");
  MOZ_ASSERT(rt == task->script()->runtimeFromMainThread());
  MOZ_ASSERT(ionLazyLinkListSize_ > 0);

  task->removeFrom(ionLazyLinkList(rt));
  ionLazyLinkListSize_--;

  MOZ_ASSERT(ionLazyLinkList(rt).isEmpty() == (ionLazyLinkListSize_ == 0));
}

void JitRuntime::ionLazyLinkListAdd(JSRuntime* rt, jit::IonCompileTask* task) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt),
             "Should only be mutated by the main thread.");
  MOZ_ASSERT(rt == task->script()->runtimeFromMainThread());
  ionLazyLinkList(rt).insertFront(task);
  ionLazyLinkListSize_++;
}

uint8_t* JitRuntime::allocateIonOsrTempData(size_t size) {
  // Free the old buffer (if needed) before allocating a new one. Note that we
  // could use realloc here but it's likely not worth the complexity.
  freeIonOsrTempData();
  ionOsrTempData_.ref().reset(static_cast<uint8_t*>(js_malloc(size)));
  return ionOsrTempData_.ref().get();
}

void JitRuntime::freeIonOsrTempData() { ionOsrTempData_.ref().reset(); }

JitRealm::JitRealm() : initialStringHeap(gc::Heap::Tenured) {}

void JitRealm::initialize(bool zoneHasNurseryStrings) {
  setStringsCanBeInNursery(zoneHasNurseryStrings);
}

template <typename T>
static T PopNextBitmaskValue(uint32_t* bitmask) {
  MOZ_ASSERT(*bitmask);
  uint32_t index = mozilla::CountTrailingZeroes32(*bitmask);
  *bitmask ^= 1 << index;

  MOZ_ASSERT(index < uint32_t(T::Count));
  return T(index);
}

void JitRealm::performStubReadBarriers(uint32_t stubsToBarrier) const {
  while (stubsToBarrier) {
    auto stub = PopNextBitmaskValue<StubIndex>(&stubsToBarrier);
    const WeakHeapPtr<JitCode*>& jitCode = stubs_[stub];
    MOZ_ASSERT(jitCode);
    jitCode.get();
  }
}

static bool LinkCodeGen(JSContext* cx, CodeGenerator* codegen,
                        HandleScript script, const WarpSnapshot* snapshot) {
  if (!codegen->link(cx, snapshot)) {
    return false;
  }

  return true;
}

static bool LinkBackgroundCodeGen(JSContext* cx, IonCompileTask* task) {
  CodeGenerator* codegen = task->backgroundCodegen();
  if (!codegen) {
    return false;
  }

  JitContext jctx(cx);
  RootedScript script(cx, task->script());
  return LinkCodeGen(cx, codegen, script, task->snapshot());
}

void jit::LinkIonScript(JSContext* cx, HandleScript calleeScript) {
  // Get the pending IonCompileTask from the script.
  MOZ_ASSERT(calleeScript->hasBaselineScript());
  IonCompileTask* task =
      calleeScript->baselineScript()->pendingIonCompileTask();
  calleeScript->baselineScript()->removePendingIonCompileTask(cx->runtime(),
                                                              calleeScript);

  // Remove from pending.
  cx->runtime()->jitRuntime()->ionLazyLinkListRemove(cx->runtime(), task);

  {
    gc::AutoSuppressGC suppressGC(cx);
    if (!LinkBackgroundCodeGen(cx, task)) {
      // Silently ignore OOM during code generation. The assembly code
      // doesn't have code to handle it after linking happened. So it's
      // not OK to throw a catchable exception from there.
      cx->clearPendingException();
    }
  }

  {
    AutoLockHelperThreadState lock;
    FinishOffThreadTask(cx->runtime(), task, lock);
  }
}

uint8_t* jit::LazyLinkTopActivation(JSContext* cx,
                                    LazyLinkExitFrameLayout* frame) {
  RootedScript calleeScript(
      cx, ScriptFromCalleeToken(frame->jsFrame()->calleeToken()));

  LinkIonScript(cx, calleeScript);

  MOZ_ASSERT(calleeScript->hasBaselineScript());
  MOZ_ASSERT(calleeScript->jitCodeRaw());

  return calleeScript->jitCodeRaw();
}

/* static */
void JitRuntime::TraceAtomZoneRoots(JSTracer* trc) {
  MOZ_ASSERT(!JS::RuntimeHeapIsMinorCollecting());

  // Shared stubs are allocated in the atoms zone, so do not iterate
  // them after the atoms heap after it has been "finished."
  if (trc->runtime()->atomsAreFinished()) {
    return;
  }

  Zone* zone = trc->runtime()->atomsZone();
  for (auto i = zone->cellIterUnsafe<JitCode>(); !i.done(); i.next()) {
    JitCode* code = i;
    TraceRoot(trc, &code, "wrapper");
  }
}

/* static */
bool JitRuntime::MarkJitcodeGlobalTableIteratively(GCMarker* marker) {
  if (marker->runtime()->hasJitRuntime() &&
      marker->runtime()->jitRuntime()->hasJitcodeGlobalTable()) {
    return marker->runtime()
        ->jitRuntime()
        ->getJitcodeGlobalTable()
        ->markIteratively(marker);
  }
  return false;
}

/* static */
void JitRuntime::TraceWeakJitcodeGlobalTable(JSRuntime* rt, JSTracer* trc) {
  if (rt->hasJitRuntime() && rt->jitRuntime()->hasJitcodeGlobalTable()) {
    rt->jitRuntime()->getJitcodeGlobalTable()->traceWeak(rt, trc);
  }
}

void JitRealm::traceWeak(JSTracer* trc, JS::Realm* realm) {
  // Any outstanding compilations should have been cancelled by the GC.
  MOZ_ASSERT(!HasOffThreadIonCompile(realm));

  for (WeakHeapPtr<JitCode*>& stub : stubs_) {
    TraceWeakEdge(trc, &stub, "JitRealm::stubs_");
  }
}

bool JitZone::addInlinedCompilation(const RecompileInfo& info,
                                    JSScript* inlined) {
  MOZ_ASSERT(inlined != info.script());

  auto p = inlinedCompilations_.lookupForAdd(inlined);
  if (p) {
    auto& compilations = p->value();
    if (!compilations.empty() && compilations.back() == info) {
      return true;
    }
    return compilations.append(info);
  }

  RecompileInfoVector compilations;
  if (!compilations.append(info)) {
    return false;
  }
  return inlinedCompilations_.add(p, inlined, std::move(compilations));
}

void jit::AddPendingInvalidation(RecompileInfoVector& invalid,
                                 JSScript* script) {
  MOZ_ASSERT(script);

  CancelOffThreadIonCompile(script);

  // Let the script warm up again before attempting another compile.
  script->resetWarmUpCounterToDelayIonCompilation();

  JitScript* jitScript = script->maybeJitScript();
  if (!jitScript) {
    return;
  }

  auto addPendingInvalidation = [&invalid](const RecompileInfo& info) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!invalid.append(info)) {
      // BUG 1536159: For diagnostics, compute the size of the failed
      // allocation. This presumes the vector growth strategy is to double. This
      // is only used for crash reporting so not a problem if we get it wrong.
      size_t allocSize = 2 * sizeof(RecompileInfo) * invalid.capacity();
      oomUnsafe.crash(allocSize, "Could not update RecompileInfoVector");
    }
  };

  // Trigger invalidation of the IonScript.
  if (jitScript->hasIonScript()) {
    RecompileInfo info(script, jitScript->ionScript()->compilationId());
    addPendingInvalidation(info);
  }

  // Trigger invalidation of any callers inlining this script.
  auto* inlinedCompilations =
      script->zone()->jitZone()->maybeInlinedCompilations(script);
  if (inlinedCompilations) {
    for (const RecompileInfo& info : *inlinedCompilations) {
      addPendingInvalidation(info);
    }
    script->zone()->jitZone()->removeInlinedCompilations(script);
  }
}

IonScript* RecompileInfo::maybeIonScriptToInvalidate() const {
  // Make sure this is not called under CodeGenerator::link (before the
  // IonScript is created).
  MOZ_ASSERT_IF(
      script_->zone()->jitZone()->currentCompilationId(),
      script_->zone()->jitZone()->currentCompilationId().ref() != id_);

  if (!script_->hasIonScript() ||
      script_->ionScript()->compilationId() != id_) {
    return nullptr;
  }

  return script_->ionScript();
}

bool RecompileInfo::traceWeak(JSTracer* trc) {
  // Sweep the RecompileInfo if either the script is dead or the IonScript has
  // been invalidated.

  if (!TraceManuallyBarrieredWeakEdge(trc, &script_, "RecompileInfo::script")) {
    return false;
  }

  return maybeIonScriptToInvalidate() != nullptr;
}

void JitZone::traceWeak(JSTracer* trc) {
  baselineCacheIRStubCodes_.traceWeak(trc);
  inlinedCompilations_.traceWeak(trc);

  TraceWeakEdge(trc, &lastStubFoldingBailoutChild_,
                "JitZone::lastStubFoldingBailoutChild_");
  TraceWeakEdge(trc, &lastStubFoldingBailoutParent_,
                "JitZone::lastStubFoldingBailoutParent_");
}

size_t JitRealm::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(this);
}

void JitZone::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                     JS::CodeSizes* code, size_t* jitZone,
                                     size_t* baselineStubsOptimized) const {
  *jitZone += mallocSizeOf(this);
  *jitZone +=
      baselineCacheIRStubCodes_.shallowSizeOfExcludingThis(mallocSizeOf);
  *jitZone += ionCacheIRStubInfoSet_.shallowSizeOfExcludingThis(mallocSizeOf);

  execAlloc().addSizeOfCode(code);

  *baselineStubsOptimized +=
      optimizedStubSpace_.sizeOfExcludingThis(mallocSizeOf);
}

void JitCodeHeader::init(JitCode* jitCode) {
  // As long as JitCode isn't moveable, we can avoid tracing this and
  // mutating executable data.
  MOZ_ASSERT(!gc::IsMovableKind(gc::AllocKind::JITCODE));
  jitCode_ = jitCode;
}

template <AllowGC allowGC>
JitCode* JitCode::New(JSContext* cx, uint8_t* code, uint32_t totalSize,
                      uint32_t headerSize, ExecutablePool* pool,
                      CodeKind kind) {
  uint32_t bufferSize = totalSize - headerSize;
  JitCode* codeObj =
      cx->newCell<JitCode, allowGC>(code, bufferSize, headerSize, pool, kind);
  if (!codeObj) {
    // The caller already allocated `totalSize` bytes of executable memory.
    pool->release(totalSize, kind);
    return nullptr;
  }

  cx->zone()->incJitMemory(totalSize);

  return codeObj;
}

template JitCode* JitCode::New<CanGC>(JSContext* cx, uint8_t* code,
                                      uint32_t bufferSize, uint32_t headerSize,
                                      ExecutablePool* pool, CodeKind kind);

template JitCode* JitCode::New<NoGC>(JSContext* cx, uint8_t* code,
                                     uint32_t bufferSize, uint32_t headerSize,
                                     ExecutablePool* pool, CodeKind kind);

void JitCode::copyFrom(MacroAssembler& masm) {
  // Store the JitCode pointer in the JitCodeHeader so we can recover the
  // gcthing from relocation tables.
  JitCodeHeader::FromExecutable(raw())->init(this);

  insnSize_ = masm.instructionsSize();
  masm.executableCopy(raw());

  jumpRelocTableBytes_ = masm.jumpRelocationTableBytes();
  masm.copyJumpRelocationTable(raw() + jumpRelocTableOffset());

  dataRelocTableBytes_ = masm.dataRelocationTableBytes();
  masm.copyDataRelocationTable(raw() + dataRelocTableOffset());

  masm.processCodeLabels(raw());
}

void JitCode::traceChildren(JSTracer* trc) {
  // Note that we cannot mark invalidated scripts, since we've basically
  // corrupted the code stream by injecting bailouts.
  if (invalidated()) {
    return;
  }

  if (jumpRelocTableBytes_) {
    uint8_t* start = raw() + jumpRelocTableOffset();
    CompactBufferReader reader(start, start + jumpRelocTableBytes_);
    MacroAssembler::TraceJumpRelocations(trc, this, reader);
  }
  if (dataRelocTableBytes_) {
    uint8_t* start = raw() + dataRelocTableOffset();
    CompactBufferReader reader(start, start + dataRelocTableBytes_);
    MacroAssembler::TraceDataRelocations(trc, this, reader);
  }
}

void JitCode::finalize(JS::GCContext* gcx) {
  // If this jitcode had a bytecode map, it must have already been removed.
#ifdef DEBUG
  JSRuntime* rt = gcx->runtime();
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
  if (gcx->appendJitPoisonRange(JitPoisonRange(pool_, raw() - headerSize_,
                                               headerSize_ + bufferSize_))) {
    pool_->addRef();
  }
  setHeaderPtr(nullptr);

#ifdef JS_ION_PERF
  // Code buffers are stored inside ExecutablePools. Pools are refcounted.
  // Releasing the pool may free it. Horrible hack: if we are using perf
  // integration, we don't want to reuse code addresses, so we just leak the
  // memory instead.
  if (!PerfEnabled()) {
    pool_->release(headerSize_ + bufferSize_, CodeKind(kind_));
  }
#else
  pool_->release(headerSize_ + bufferSize_, CodeKind(kind_));
#endif

  zone()->decJitMemory(headerSize_ + bufferSize_);

  pool_ = nullptr;
}

IonScript::IonScript(IonCompilationId compilationId, uint32_t localSlotsSize,
                     uint32_t argumentSlotsSize, uint32_t frameSize)
    : localSlotsSize_(localSlotsSize),
      argumentSlotsSize_(argumentSlotsSize),
      frameSize_(frameSize),
      compilationId_(compilationId) {}

IonScript* IonScript::New(JSContext* cx, IonCompilationId compilationId,
                          uint32_t localSlotsSize, uint32_t argumentSlotsSize,
                          uint32_t frameSize, size_t snapshotsListSize,
                          size_t snapshotsRVATableSize, size_t recoversSize,
                          size_t constants, size_t nurseryObjects,
                          size_t safepointIndices, size_t osiIndices,
                          size_t icEntries, size_t runtimeSize,
                          size_t safepointsSize) {
  if (snapshotsListSize >= MAX_BUFFER_SIZE) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  // Verify the hardcoded sizes in header are accurate.
  static_assert(SizeOf_OsiIndex == sizeof(OsiIndex),
                "IonScript has wrong size for OsiIndex");
  static_assert(SizeOf_SafepointIndex == sizeof(SafepointIndex),
                "IonScript has wrong size for SafepointIndex");

  CheckedInt<Offset> allocSize = sizeof(IonScript);
  allocSize += CheckedInt<Offset>(constants) * sizeof(Value);
  allocSize += CheckedInt<Offset>(runtimeSize);
  allocSize += CheckedInt<Offset>(nurseryObjects) * sizeof(HeapPtr<JSObject*>);
  allocSize += CheckedInt<Offset>(osiIndices) * sizeof(OsiIndex);
  allocSize += CheckedInt<Offset>(safepointIndices) * sizeof(SafepointIndex);
  allocSize += CheckedInt<Offset>(icEntries) * sizeof(uint32_t);
  allocSize += CheckedInt<Offset>(safepointsSize);
  allocSize += CheckedInt<Offset>(snapshotsListSize);
  allocSize += CheckedInt<Offset>(snapshotsRVATableSize);
  allocSize += CheckedInt<Offset>(recoversSize);

  if (!allocSize.isValid()) {
    ReportAllocationOverflow(cx);
    return nullptr;
  }

  void* raw = cx->pod_malloc<uint8_t>(allocSize.value());
  MOZ_ASSERT(uintptr_t(raw) % alignof(IonScript) == 0);
  if (!raw) {
    return nullptr;
  }
  IonScript* script = new (raw)
      IonScript(compilationId, localSlotsSize, argumentSlotsSize, frameSize);

  Offset offsetCursor = sizeof(IonScript);

  MOZ_ASSERT(offsetCursor % alignof(Value) == 0);
  script->constantTableOffset_ = offsetCursor;
  offsetCursor += constants * sizeof(Value);

  MOZ_ASSERT(offsetCursor % alignof(uint64_t) == 0);
  script->runtimeDataOffset_ = offsetCursor;
  offsetCursor += runtimeSize;

  MOZ_ASSERT(offsetCursor % alignof(HeapPtr<JSObject*>) == 0);
  script->initElements<HeapPtr<JSObject*>>(offsetCursor, nurseryObjects);
  script->nurseryObjectsOffset_ = offsetCursor;
  offsetCursor += nurseryObjects * sizeof(HeapPtr<JSObject*>);

  MOZ_ASSERT(offsetCursor % alignof(OsiIndex) == 0);
  script->osiIndexOffset_ = offsetCursor;
  offsetCursor += osiIndices * sizeof(OsiIndex);

  MOZ_ASSERT(offsetCursor % alignof(SafepointIndex) == 0);
  script->safepointIndexOffset_ = offsetCursor;
  offsetCursor += safepointIndices * sizeof(SafepointIndex);

  MOZ_ASSERT(offsetCursor % alignof(uint32_t) == 0);
  script->icIndexOffset_ = offsetCursor;
  offsetCursor += icEntries * sizeof(uint32_t);

  script->safepointsOffset_ = offsetCursor;
  offsetCursor += safepointsSize;

  script->snapshotsOffset_ = offsetCursor;
  offsetCursor += snapshotsListSize;

  script->rvaTableOffset_ = offsetCursor;
  offsetCursor += snapshotsRVATableSize;

  script->recoversOffset_ = offsetCursor;
  offsetCursor += recoversSize;

  script->allocBytes_ = offsetCursor;

  MOZ_ASSERT(script->numConstants() == constants);
  MOZ_ASSERT(script->runtimeSize() == runtimeSize);
  MOZ_ASSERT(script->numNurseryObjects() == nurseryObjects);
  MOZ_ASSERT(script->numOsiIndices() == osiIndices);
  MOZ_ASSERT(script->numSafepointIndices() == safepointIndices);
  MOZ_ASSERT(script->numICs() == icEntries);
  MOZ_ASSERT(script->safepointsSize() == safepointsSize);
  MOZ_ASSERT(script->snapshotsListSize() == snapshotsListSize);
  MOZ_ASSERT(script->snapshotsRVATableSize() == snapshotsRVATableSize);
  MOZ_ASSERT(script->recoversSize() == recoversSize);
  MOZ_ASSERT(script->endOffset() == offsetCursor);

  return script;
}

void IonScript::trace(JSTracer* trc) {
  if (method_) {
    TraceEdge(trc, &method_, "method");
  }

  for (size_t i = 0; i < numConstants(); i++) {
    TraceEdge(trc, &getConstant(i), "constant");
  }

  for (size_t i = 0; i < numNurseryObjects(); i++) {
    TraceEdge(trc, &nurseryObjects()[i], "nursery-object");
  }

  // Trace caches so that the JSScript pointer can be updated if moved.
  for (size_t i = 0; i < numICs(); i++) {
    getICFromIndex(i).trace(trc, this);
  }
}

/* static */
void IonScript::preWriteBarrier(Zone* zone, IonScript* ionScript) {
  PreWriteBarrier(zone, ionScript);
}

void IonScript::copySnapshots(const SnapshotWriter* writer) {
  MOZ_ASSERT(writer->listSize() == snapshotsListSize());
  memcpy(offsetToPointer<uint8_t>(snapshotsOffset()), writer->listBuffer(),
         snapshotsListSize());

  MOZ_ASSERT(snapshotsRVATableSize());
  MOZ_ASSERT(writer->RVATableSize() == snapshotsRVATableSize());
  memcpy(offsetToPointer<uint8_t>(rvaTableOffset()), writer->RVATableBuffer(),
         snapshotsRVATableSize());
}

void IonScript::copyRecovers(const RecoverWriter* writer) {
  MOZ_ASSERT(writer->size() == recoversSize());
  memcpy(offsetToPointer<uint8_t>(recoversOffset()), writer->buffer(),
         recoversSize());
}

void IonScript::copySafepoints(const SafepointWriter* writer) {
  MOZ_ASSERT(writer->size() == safepointsSize());
  memcpy(offsetToPointer<uint8_t>(safepointsOffset()), writer->buffer(),
         safepointsSize());
}

void IonScript::copyConstants(const Value* vp) {
  for (size_t i = 0; i < numConstants(); i++) {
    constants()[i].init(vp[i]);
  }
}

void IonScript::copySafepointIndices(const CodegenSafepointIndex* si) {
  // Convert CodegenSafepointIndex to more compact form.
  SafepointIndex* table = safepointIndices();
  for (size_t i = 0; i < numSafepointIndices(); ++i) {
    table[i] = SafepointIndex(si[i]);
  }
}

void IonScript::copyOsiIndices(const OsiIndex* oi) {
  memcpy(osiIndices(), oi, numOsiIndices() * sizeof(OsiIndex));
}

void IonScript::copyRuntimeData(const uint8_t* data) {
  memcpy(runtimeData(), data, runtimeSize());
}

void IonScript::copyICEntries(const uint32_t* icEntries) {
  memcpy(icIndex(), icEntries, numICs() * sizeof(uint32_t));

  // Update the codeRaw_ field in the ICs now that we know the code address.
  for (size_t i = 0; i < numICs(); i++) {
    getICFromIndex(i).resetCodeRaw(this);
  }
}

const SafepointIndex* IonScript::getSafepointIndex(uint32_t disp) const {
  MOZ_ASSERT(numSafepointIndices() > 0);

  const SafepointIndex* table = safepointIndices();
  if (numSafepointIndices() == 1) {
    MOZ_ASSERT(disp == table[0].displacement());
    return &table[0];
  }

  size_t minEntry = 0;
  size_t maxEntry = numSafepointIndices() - 1;
  uint32_t min = table[minEntry].displacement();
  uint32_t max = table[maxEntry].displacement();

  // Raise if the element is not in the list.
  MOZ_ASSERT(min <= disp && disp <= max);

  // Approximate the location of the FrameInfo.
  size_t guess = (disp - min) * (maxEntry - minEntry) / (max - min) + minEntry;
  uint32_t guessDisp = table[guess].displacement();

  if (table[guess].displacement() == disp) {
    return &table[guess];
  }

  // Doing a linear scan from the guess should be more efficient in case of
  // small group which are equally distributed on the code.
  //
  // such as:  <...      ...    ...  ...  .   ...    ...>
  if (guessDisp > disp) {
    while (--guess >= minEntry) {
      guessDisp = table[guess].displacement();
      MOZ_ASSERT(guessDisp >= disp);
      if (guessDisp == disp) {
        return &table[guess];
      }
    }
  } else {
    while (++guess <= maxEntry) {
      guessDisp = table[guess].displacement();
      MOZ_ASSERT(guessDisp <= disp);
      if (guessDisp == disp) {
        return &table[guess];
      }
    }
  }

  MOZ_CRASH("displacement not found.");
}

const OsiIndex* IonScript::getOsiIndex(uint32_t disp) const {
  const OsiIndex* end = osiIndices() + numOsiIndices();
  for (const OsiIndex* it = osiIndices(); it != end; ++it) {
    if (it->returnPointDisplacement() == disp) {
      return it;
    }
  }

  MOZ_CRASH("Failed to find OSI point return address");
}

const OsiIndex* IonScript::getOsiIndex(uint8_t* retAddr) const {
  JitSpew(JitSpew_IonInvalidate, "IonScript %p has method %p raw %p",
          (void*)this, (void*)method(), method()->raw());

  MOZ_ASSERT(containsCodeAddress(retAddr));
  uint32_t disp = retAddr - method()->raw();
  return getOsiIndex(disp);
}

void IonScript::Destroy(JS::GCContext* gcx, IonScript* script) {
  // Make sure there are no pointers into the IonScript's nursery objects list
  // in the store buffer. Because this can be called during sweeping when
  // discarding JIT code, we have to lock the store buffer when we find an
  // object that's (still) in the nursery.
  mozilla::Maybe<gc::AutoLockStoreBuffer> lock;
  for (size_t i = 0, len = script->numNurseryObjects(); i < len; i++) {
    JSObject* obj = script->nurseryObjects()[i];
    if (!IsInsideNursery(obj)) {
      continue;
    }
    if (lock.isNothing()) {
      lock.emplace(&gcx->runtime()->gc.storeBuffer());
    }
    script->nurseryObjects()[i] = HeapPtr<JSObject*>();
  }

  // This allocation is tracked by JSScript::setIonScriptImpl.
  gcx->deleteUntracked(script);
}

void JS::DeletePolicy<js::jit::IonScript>::operator()(
    const js::jit::IonScript* script) {
  IonScript::Destroy(rt_->gcContext(), const_cast<IonScript*>(script));
}

void IonScript::purgeICs(Zone* zone) {
  for (size_t i = 0; i < numICs(); i++) {
    getICFromIndex(i).reset(zone, this);
  }
}

namespace js {
namespace jit {

bool OptimizeMIR(MIRGenerator* mir) {
  MIRGraph& graph = mir->graph();
  GraphSpewer& gs = mir->graphSpewer();

  if (mir->shouldCancel("Start")) {
    return false;
  }

  gs.spewPass("BuildSSA");
  AssertBasicGraphCoherency(graph);

  if (JitSpewEnabled(JitSpew_MIRExpressions)) {
    JitSpewCont(JitSpew_MIRExpressions, "\n");
    DumpMIRExpressions(JitSpewPrinter(), graph, mir->outerInfo(),
                       "BuildSSA (== input to OptimizeMIR)");
  }

  if (!JitOptions.disablePruning && !mir->compilingWasm()) {
    JitSpewCont(JitSpew_Prune, "\n");
    if (!PruneUnusedBranches(mir, graph)) {
      return false;
    }
    gs.spewPass("Prune Unused Branches");
    AssertBasicGraphCoherency(graph);

    if (mir->shouldCancel("Prune Unused Branches")) {
      return false;
    }
  }

  {
    if (!FoldEmptyBlocks(graph)) {
      return false;
    }
    gs.spewPass("Fold Empty Blocks");
    AssertBasicGraphCoherency(graph);

    if (mir->shouldCancel("Fold Empty Blocks")) {
      return false;
    }
  }

  // Remove trivially dead resume point operands before folding tests, so the
  // latter pass can optimize more aggressively.
  if (!mir->compilingWasm()) {
    if (!EliminateTriviallyDeadResumePointOperands(mir, graph)) {
      return false;
    }
    gs.spewPass("Eliminate trivially dead resume point operands");
    AssertBasicGraphCoherency(graph);

    if (mir->shouldCancel("Eliminate trivially dead resume point operands")) {
      return false;
    }
  }

  {
    if (!FoldTests(graph)) {
      return false;
    }
    gs.spewPass("Fold Tests");
    AssertBasicGraphCoherency(graph);

    if (mir->shouldCancel("Fold Tests")) {
      return false;
    }
  }

  {
    if (!SplitCriticalEdges(graph)) {
      return false;
    }
    gs.spewPass("Split Critical Edges");
    AssertGraphCoherency(graph);

    if (mir->shouldCancel("Split Critical Edges")) {
      return false;
    }
  }

  {
    RenumberBlocks(graph);
    gs.spewPass("Renumber Blocks");
    AssertGraphCoherency(graph);

    if (mir->shouldCancel("Renumber Blocks")) {
      return false;
    }
  }

  {
    if (!BuildDominatorTree(graph)) {
      return false;
    }
    // No spew: graph not changed.

    if (mir->shouldCancel("Dominator Tree")) {
      return false;
    }
  }

  {
    // Aggressive phi elimination must occur before any code elimination. If the
    // script contains a try-statement, we only compiled the try block and not
    // the catch or finally blocks, so in this case it's also invalid to use
    // aggressive phi elimination.
    Observability observability = graph.hasTryBlock()
                                      ? ConservativeObservability
                                      : AggressiveObservability;
    if (!EliminatePhis(mir, graph, observability)) {
      return false;
    }
    gs.spewPass("Eliminate phis");
    AssertGraphCoherency(graph);

    if (mir->shouldCancel("Eliminate phis")) {
      return false;
    }

    if (!BuildPhiReverseMapping(graph)) {
      return false;
    }
    AssertExtendedGraphCoherency(graph);
    // No spew: graph not changed.

    if (mir->shouldCancel("Phi reverse mapping")) {
      return false;
    }
  }

  if (!mir->compilingWasm() && !JitOptions.disableIteratorIndices) {
    if (!OptimizeIteratorIndices(mir, graph)) {
      return false;
    }
    gs.spewPass("Iterator Indices");
    AssertGraphCoherency(graph);

    if (mir->shouldCancel("Iterator Indices")) {
      return false;
    }
  }

  if (!JitOptions.disableRecoverIns &&
      mir->optimizationInfo().scalarReplacementEnabled()) {
    JitSpewCont(JitSpew_Escape, "\n");
    if (!ScalarReplacement(mir, graph)) {
      return false;
    }
    gs.spewPass("Scalar Replacement");
    AssertGraphCoherency(graph);

    if (mir->shouldCancel("Scalar Replacement")) {
      return false;
    }
  }

  if (!mir->compilingWasm()) {
    if (!ApplyTypeInformation(mir, graph)) {
      return false;
    }
    gs.spewPass("Apply types");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("Apply types")) {
      return false;
    }
  }

  if (mir->optimizationInfo().amaEnabled()) {
    AlignmentMaskAnalysis ama(graph);
    if (!ama.analyze()) {
      return false;
    }
    gs.spewPass("Alignment Mask Analysis");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("Alignment Mask Analysis")) {
      return false;
    }
  }

  ValueNumberer gvn(mir, graph);

  // Alias analysis is required for LICM and GVN so that we don't move
  // loads across stores. We also use alias information when removing
  // redundant shapeguards.
  if (mir->optimizationInfo().licmEnabled() ||
      mir->optimizationInfo().gvnEnabled() ||
      mir->optimizationInfo().eliminateRedundantShapeGuardsEnabled()) {
    {
      AliasAnalysis analysis(mir, graph);
      JitSpewCont(JitSpew_Alias, "\n");
      if (!analysis.analyze()) {
        return false;
      }

      gs.spewPass("Alias analysis");
      AssertExtendedGraphCoherency(graph);

      if (mir->shouldCancel("Alias analysis")) {
        return false;
      }
    }

    if (!mir->compilingWasm()) {
      // Eliminating dead resume point operands requires basic block
      // instructions to be numbered. Reuse the numbering computed during
      // alias analysis.
      if (!EliminateDeadResumePointOperands(mir, graph)) {
        return false;
      }

      gs.spewPass("Eliminate dead resume point operands");
      AssertExtendedGraphCoherency(graph);

      if (mir->shouldCancel("Eliminate dead resume point operands")) {
        return false;
      }
    }
  }

  if (mir->optimizationInfo().gvnEnabled()) {
    JitSpewCont(JitSpew_GVN, "\n");
    if (!gvn.run(ValueNumberer::UpdateAliasAnalysis)) {
      return false;
    }
    gs.spewPass("GVN");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("GVN")) {
      return false;
    }
  }

  // LICM can hoist instructions from conditional branches and
  // trigger bailouts. Disable it if bailing out of a hoisted
  // instruction has previously invalidated this script.
  if (mir->licmEnabled()) {
    JitSpewCont(JitSpew_LICM, "\n");
    if (!LICM(mir, graph)) {
      return false;
    }
    gs.spewPass("LICM");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("LICM")) {
      return false;
    }
  }

  RangeAnalysis r(mir, graph);
  if (mir->optimizationInfo().rangeAnalysisEnabled()) {
    JitSpewCont(JitSpew_Range, "\n");
    if (!r.addBetaNodes()) {
      return false;
    }
    gs.spewPass("Beta");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("RA Beta")) {
      return false;
    }

    if (!r.analyze() || !r.addRangeAssertions()) {
      return false;
    }
    gs.spewPass("Range Analysis");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("Range Analysis")) {
      return false;
    }

    if (!r.removeBetaNodes()) {
      return false;
    }
    gs.spewPass("De-Beta");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("RA De-Beta")) {
      return false;
    }

    if (mir->optimizationInfo().gvnEnabled()) {
      bool shouldRunUCE = false;
      if (!r.prepareForUCE(&shouldRunUCE)) {
        return false;
      }
      gs.spewPass("RA check UCE");
      AssertExtendedGraphCoherency(graph);

      if (mir->shouldCancel("RA check UCE")) {
        return false;
      }

      if (shouldRunUCE) {
        if (!gvn.run(ValueNumberer::DontUpdateAliasAnalysis)) {
          return false;
        }
        gs.spewPass("UCE After RA");
        AssertExtendedGraphCoherency(graph);

        if (mir->shouldCancel("UCE After RA")) {
          return false;
        }
      }
    }

    if (mir->optimizationInfo().autoTruncateEnabled()) {
      if (!r.truncate()) {
        return false;
      }
      gs.spewPass("Truncate Doubles");
      AssertExtendedGraphCoherency(graph);

      if (mir->shouldCancel("Truncate Doubles")) {
        return false;
      }
    }
  }

  if (!JitOptions.disableRecoverIns) {
    JitSpewCont(JitSpew_Sink, "\n");
    if (!Sink(mir, graph)) {
      return false;
    }
    gs.spewPass("Sink");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("Sink")) {
      return false;
    }
  }

  if (!JitOptions.disableRecoverIns &&
      mir->optimizationInfo().rangeAnalysisEnabled()) {
    JitSpewCont(JitSpew_Range, "\n");
    if (!r.removeUnnecessaryBitops()) {
      return false;
    }
    gs.spewPass("Remove Unnecessary Bitops");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("Remove Unnecessary Bitops")) {
      return false;
    }
  }

  {
    JitSpewCont(JitSpew_FLAC, "\n");
    if (!FoldLinearArithConstants(mir, graph)) {
      return false;
    }
    gs.spewPass("Fold Linear Arithmetic Constants");
    AssertBasicGraphCoherency(graph);

    if (mir->shouldCancel("Fold Linear Arithmetic Constants")) {
      return false;
    }
  }

  if (mir->optimizationInfo().eaaEnabled()) {
    EffectiveAddressAnalysis eaa(mir, graph);
    JitSpewCont(JitSpew_EAA, "\n");
    if (!eaa.analyze()) {
      return false;
    }
    gs.spewPass("Effective Address Analysis");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("Effective Address Analysis")) {
      return false;
    }
  }

  // BCE marks bounds checks as dead, so do BCE before DCE.
  if (mir->compilingWasm()) {
    JitSpewCont(JitSpew_WasmBCE, "\n");
    if (!EliminateBoundsChecks(mir, graph)) {
      return false;
    }
    gs.spewPass("Redundant Bounds Check Elimination");
    AssertGraphCoherency(graph);

    if (mir->shouldCancel("BCE")) {
      return false;
    }
  }

  {
    if (!EliminateDeadCode(mir, graph)) {
      return false;
    }
    gs.spewPass("DCE");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("DCE")) {
      return false;
    }
  }

  if (mir->optimizationInfo().instructionReorderingEnabled() &&
      !mir->outerInfo().hadReorderingBailout()) {
    if (!ReorderInstructions(graph)) {
      return false;
    }
    gs.spewPass("Reordering");

    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("Reordering")) {
      return false;
    }
  }

  // Make loops contiguous. We do this after GVN/UCE and range analysis,
  // which can remove CFG edges, exposing more blocks that can be moved.
  {
    if (!MakeLoopsContiguous(graph)) {
      return false;
    }
    gs.spewPass("Make loops contiguous");
    AssertExtendedGraphCoherency(graph);

    if (mir->shouldCancel("Make loops contiguous")) {
      return false;
    }
  }
  AssertExtendedGraphCoherency(graph, /* underValueNumberer = */ false,
                               /* force = */ true);

  // Remove unreachable blocks created by MBasicBlock::NewFakeLoopPredecessor
  // to ensure every loop header has two predecessors. (This only happens due
  // to OSR.)  After this point, it is no longer possible to build the
  // dominator tree.
  if (!mir->compilingWasm() && graph.osrBlock()) {
    graph.removeFakeLoopPredecessors();
    gs.spewPass("Remove fake loop predecessors");
    AssertGraphCoherency(graph);

    if (mir->shouldCancel("Remove fake loop predecessors")) {
      return false;
    }
  }

  // Passes after this point must not move instructions; these analyses
  // depend on knowing the final order in which instructions will execute.

  if (mir->optimizationInfo().edgeCaseAnalysisEnabled()) {
    EdgeCaseAnalysis edgeCaseAnalysis(mir, graph);
    if (!edgeCaseAnalysis.analyzeLate()) {
      return false;
    }
    gs.spewPass("Edge Case Analysis (Late)");
    AssertGraphCoherency(graph);

    if (mir->shouldCancel("Edge Case Analysis (Late)")) {
      return false;
    }
  }

  if (mir->optimizationInfo().eliminateRedundantChecksEnabled()) {
    // Note: check elimination has to run after all other passes that move
    // instructions. Since check uses are replaced with the actual index,
    // code motion after this pass could incorrectly move a load or store
    // before its bounds check.
    if (!EliminateRedundantChecks(graph)) {
      return false;
    }
    gs.spewPass("Bounds Check Elimination");
    AssertGraphCoherency(graph);
  }

  if (mir->optimizationInfo().eliminateRedundantShapeGuardsEnabled()) {
    if (!EliminateRedundantShapeGuards(graph)) {
      return false;
    }
    gs.spewPass("Shape Guard Elimination");
    AssertGraphCoherency(graph);
  }

  // Run the GC Barrier Elimination pass after instruction reordering, to
  // ensure we don't move instructions that can trigger GC between stores we
  // optimize here.
  if (mir->optimizationInfo().eliminateRedundantGCBarriersEnabled()) {
    if (!EliminateRedundantGCBarriers(graph)) {
      return false;
    }
    gs.spewPass("GC Barrier Elimination");
    AssertGraphCoherency(graph);
  }

  if (!mir->compilingWasm() && !mir->outerInfo().hadUnboxFoldingBailout()) {
    if (!FoldLoadsWithUnbox(mir, graph)) {
      return false;
    }
    gs.spewPass("FoldLoadsWithUnbox");
    AssertGraphCoherency(graph);
  }

  if (!mir->compilingWasm()) {
    if (!AddKeepAliveInstructions(graph)) {
      return false;
    }
    gs.spewPass("Add KeepAlive Instructions");
    AssertGraphCoherency(graph);
  }

  AssertGraphCoherency(graph, /* force = */ true);

  if (JitSpewEnabled(JitSpew_MIRExpressions)) {
    JitSpewCont(JitSpew_MIRExpressions, "\n");
    DumpMIRExpressions(JitSpewPrinter(), graph, mir->outerInfo(),
                       "BeforeLIR (== result of OptimizeMIR)");
  }

  return true;
}

LIRGraph* GenerateLIR(MIRGenerator* mir) {
  MIRGraph& graph = mir->graph();
  GraphSpewer& gs = mir->graphSpewer();

  LIRGraph* lir = mir->alloc().lifoAlloc()->new_<LIRGraph>(&graph);
  if (!lir || !lir->init()) {
    return nullptr;
  }

  LIRGenerator lirgen(mir, graph, *lir);
  {
    if (!lirgen.generate()) {
      return nullptr;
    }
    gs.spewPass("Generate LIR");

    if (mir->shouldCancel("Generate LIR")) {
      return nullptr;
    }
  }

#ifdef DEBUG
  AllocationIntegrityState integrity(*lir);
#endif

  {
    IonRegisterAllocator allocator =
        mir->optimizationInfo().registerAllocator();

    switch (allocator) {
      case RegisterAllocator_Backtracking:
      case RegisterAllocator_Testbed: {
#ifdef DEBUG
        if (JitOptions.fullDebugChecks) {
          if (!integrity.record()) {
            return nullptr;
          }
        }
#endif

        BacktrackingAllocator regalloc(mir, &lirgen, *lir,
                                       allocator == RegisterAllocator_Testbed);
        if (!regalloc.go()) {
          return nullptr;
        }

#ifdef DEBUG
        if (JitOptions.fullDebugChecks) {
          if (!integrity.check()) {
            return nullptr;
          }
        }
#endif

        gs.spewPass("Allocate Registers [Backtracking]");
        break;
      }

      default:
        MOZ_CRASH("Bad regalloc");
    }

    if (mir->shouldCancel("Allocate Registers")) {
      return nullptr;
    }
  }

  return lir;
}

CodeGenerator* GenerateCode(MIRGenerator* mir, LIRGraph* lir) {
  auto codegen = MakeUnique<CodeGenerator>(mir, lir);
  if (!codegen) {
    return nullptr;
  }

  if (!codegen->generate()) {
    return nullptr;
  }

  return codegen.release();
}

CodeGenerator* CompileBackEnd(MIRGenerator* mir, WarpSnapshot* snapshot) {
  // Everything in CompileBackEnd can potentially run on a helper thread.
  AutoEnterIonBackend enter;
  AutoSpewEndFunction spewEndFunction(mir);

  {
    WarpCompilation comp(mir->alloc());
    WarpBuilder builder(*snapshot, *mir, &comp);
    if (!builder.build()) {
      return nullptr;
    }
  }

  if (!OptimizeMIR(mir)) {
    return nullptr;
  }

  LIRGraph* lir = GenerateLIR(mir);
  if (!lir) {
    return nullptr;
  }

  return GenerateCode(mir, lir);
}

static AbortReasonOr<WarpSnapshot*> CreateWarpSnapshot(JSContext* cx,
                                                       MIRGenerator* mirGen,
                                                       HandleScript script) {
  // Suppress GC during compilation.
  gc::AutoSuppressGC suppressGC(cx);

  SpewBeginFunction(mirGen, script);

  WarpOracle oracle(cx, *mirGen, script);

  AbortReasonOr<WarpSnapshot*> result = oracle.createSnapshot();

  MOZ_ASSERT_IF(result.isErr(), result.unwrapErr() == AbortReason::Alloc ||
                                    result.unwrapErr() == AbortReason::Error ||
                                    result.unwrapErr() == AbortReason::Disable);
  MOZ_ASSERT_IF(!result.isErr(), result.unwrap());

  return result;
}

static AbortReason IonCompile(JSContext* cx, HandleScript script,
                              jsbytecode* osrPc) {
  cx->check(script);

  auto alloc =
      cx->make_unique<LifoAlloc>(TempAllocator::PreferredLifoChunkSize);
  if (!alloc) {
    return AbortReason::Error;
  }

  if (!cx->realm()->ensureJitRealmExists(cx)) {
    return AbortReason::Error;
  }

  if (!cx->realm()->jitRealm()->ensureIonStubsExist(cx)) {
    return AbortReason::Error;
  }

  TempAllocator* temp = alloc->new_<TempAllocator>(alloc.get());
  if (!temp) {
    return AbortReason::Alloc;
  }

  MIRGraph* graph = alloc->new_<MIRGraph>(temp);
  if (!graph) {
    return AbortReason::Alloc;
  }

  InlineScriptTree* inlineScriptTree =
      InlineScriptTree::New(temp, nullptr, nullptr, script);
  if (!inlineScriptTree) {
    return AbortReason::Alloc;
  }

  CompileInfo* info = alloc->new_<CompileInfo>(
      CompileRuntime::get(cx->runtime()), script, script->function(), osrPc,
      script->needsArgsObj(), inlineScriptTree);
  if (!info) {
    return AbortReason::Alloc;
  }

  const OptimizationInfo* optimizationInfo =
      IonOptimizations.get(OptimizationLevel::Normal);
  const JitCompileOptions options(cx);

  MIRGenerator* mirGen =
      alloc->new_<MIRGenerator>(CompileRealm::get(cx->realm()), options, temp,
                                graph, info, optimizationInfo);
  if (!mirGen) {
    return AbortReason::Alloc;
  }

  MOZ_ASSERT(!script->baselineScript()->hasPendingIonCompileTask());
  MOZ_ASSERT(!script->hasIonScript());
  MOZ_ASSERT(script->canIonCompile());

  if (osrPc) {
    script->jitScript()->setHadIonOSR();
  }

  AbortReasonOr<WarpSnapshot*> result = CreateWarpSnapshot(cx, mirGen, script);
  if (result.isErr()) {
    return result.unwrapErr();
  }
  WarpSnapshot* snapshot = result.unwrap();

  // If possible, compile the script off thread.
  if (options.offThreadCompilationAvailable()) {
    JitSpew(JitSpew_IonSyncLogs,
            "Can't log script %s:%u:%u"
            ". (Compiled on background thread.)",
            script->filename(), script->lineno(), script->column());

    IonCompileTask* task = alloc->new_<IonCompileTask>(cx, *mirGen, snapshot);
    if (!task) {
      return AbortReason::Alloc;
    }

    AutoLockHelperThreadState lock;
    if (!StartOffThreadIonCompile(task, lock)) {
      JitSpew(JitSpew_IonAbort, "Unable to start off-thread ion compilation.");
      mirGen->graphSpewer().endFunction();
      return AbortReason::Alloc;
    }

    script->jitScript()->setIsIonCompilingOffThread(script);

    // The allocator and associated data will be destroyed after being
    // processed in the finishedOffThreadCompilations list.
    (void)alloc.release();

    return AbortReason::NoAbort;
  }

  bool succeeded = false;
  {
    gc::AutoSuppressGC suppressGC(cx);
    JitContext jctx(cx);
    UniquePtr<CodeGenerator> codegen(CompileBackEnd(mirGen, snapshot));
    if (!codegen) {
      JitSpew(JitSpew_IonAbort, "Failed during back-end compilation.");
      if (cx->isExceptionPending()) {
        return AbortReason::Error;
      }
      return AbortReason::Disable;
    }

    succeeded = LinkCodeGen(cx, codegen.get(), script, snapshot);
  }

  if (succeeded) {
    return AbortReason::NoAbort;
  }
  if (cx->isExceptionPending()) {
    return AbortReason::Error;
  }
  return AbortReason::Disable;
}

static bool CheckFrame(JSContext* cx, BaselineFrame* frame) {
  MOZ_ASSERT(!frame->isDebuggerEvalFrame());
  MOZ_ASSERT(!frame->isEvalFrame());

  // This check is to not overrun the stack.
  if (frame->isFunctionFrame()) {
    if (TooManyActualArguments(frame->numActualArgs())) {
      JitSpew(JitSpew_IonAbort, "too many actual arguments");
      return false;
    }

    if (TooManyFormalArguments(frame->numFormalArgs())) {
      JitSpew(JitSpew_IonAbort, "too many arguments");
      return false;
    }
  }

  return true;
}

static bool CanIonCompileOrInlineScript(JSScript* script, const char** reason) {
  if (script->isForEval()) {
    // Eval frames are not yet supported. Supporting this will require new
    // logic in pushBailoutFrame to deal with linking prev.
    // Additionally, JSOp::GlobalOrEvalDeclInstantiation support will require
    // baking in isEvalFrame().
    *reason = "eval script";
    return false;
  }

  if (script->isAsync()) {
    if (script->isModule()) {
      *reason = "async module";
      return false;
    }
  }

  if (script->hasNonSyntacticScope() && !script->function()) {
    // Support functions with a non-syntactic global scope but not other
    // scripts. For global scripts, WarpBuilder currently uses the global
    // object as scope chain, this is not valid when the script has a
    // non-syntactic global scope.
    *reason = "has non-syntactic global scope";
    return false;
  }

  return true;
}  // namespace jit

static bool ScriptIsTooLarge(JSContext* cx, JSScript* script) {
  if (!JitOptions.limitScriptSize) {
    return false;
  }

  size_t numLocalsAndArgs = NumLocalsAndArgs(script);

  bool canCompileOffThread = OffThreadCompilationAvailable(cx);
  size_t maxScriptSize = canCompileOffThread
                             ? JitOptions.ionMaxScriptSize
                             : JitOptions.ionMaxScriptSizeMainThread;
  size_t maxLocalsAndArgs = canCompileOffThread
                                ? JitOptions.ionMaxLocalsAndArgs
                                : JitOptions.ionMaxLocalsAndArgsMainThread;

  if (script->length() > maxScriptSize || numLocalsAndArgs > maxLocalsAndArgs) {
    JitSpew(JitSpew_IonAbort,
            "Script too large (%zu bytes) (%zu locals/args) @ %s:%u:%u",
            script->length(), numLocalsAndArgs, script->filename(),
            script->lineno(), script->column());
    return true;
  }

  return false;
}

bool CanIonCompileScript(JSContext* cx, JSScript* script) {
  if (!script->canIonCompile()) {
    return false;
  }

  const char* reason = nullptr;
  if (!CanIonCompileOrInlineScript(script, &reason)) {
    JitSpew(JitSpew_IonAbort, "%s", reason);
    return false;
  }

  if (ScriptIsTooLarge(cx, script)) {
    return false;
  }

  return true;
}

bool CanIonInlineScript(JSScript* script) {
  if (!script->canIonCompile()) {
    return false;
  }

  const char* reason = nullptr;
  if (!CanIonCompileOrInlineScript(script, &reason)) {
    JitSpew(JitSpew_Inlining, "Cannot Ion compile script (%s)", reason);
    return false;
  }

  return true;
}

static MethodStatus Compile(JSContext* cx, HandleScript script,
                            BaselineFrame* osrFrame, jsbytecode* osrPc) {
  MOZ_ASSERT(jit::IsIonEnabled(cx));
  MOZ_ASSERT(jit::IsBaselineJitEnabled(cx));

  MOZ_ASSERT(script->hasBaselineScript());
  MOZ_ASSERT(!script->baselineScript()->hasPendingIonCompileTask());
  MOZ_ASSERT(!script->hasIonScript());

  AutoGeckoProfilerEntry pseudoFrame(
      cx, "Ion script compilation",
      JS::ProfilingCategoryPair::JS_IonCompilation);

  if (script->isDebuggee() || (osrFrame && osrFrame->isDebuggee())) {
    JitSpew(JitSpew_IonAbort, "debugging");
    return Method_Skipped;
  }

  if (!CanIonCompileScript(cx, script)) {
    JitSpew(JitSpew_IonAbort, "Aborted compilation of %s:%u:%u",
            script->filename(), script->lineno(), script->column());
    return Method_CantCompile;
  }

  OptimizationLevel optimizationLevel =
      IonOptimizations.levelForScript(script, osrPc);
  if (optimizationLevel == OptimizationLevel::DontCompile) {
    return Method_Skipped;
  }

  MOZ_ASSERT(optimizationLevel == OptimizationLevel::Normal);

  if (!CanLikelyAllocateMoreExecutableMemory()) {
    script->resetWarmUpCounterToDelayIonCompilation();
    return Method_Skipped;
  }

  MOZ_ASSERT(!script->hasIonScript());

  AbortReason reason = IonCompile(cx, script, osrPc);
  if (reason == AbortReason::Error) {
    MOZ_ASSERT(cx->isExceptionPending());
    return Method_Error;
  }

  if (reason == AbortReason::Disable) {
    return Method_CantCompile;
  }

  if (reason == AbortReason::Alloc) {
    ReportOutOfMemory(cx);
    return Method_Error;
  }

  // Compilation succeeded or we invalidated right away or an inlining/alloc
  // abort
  if (script->hasIonScript()) {
    return Method_Compiled;
  }
  return Method_Skipped;
}

}  // namespace jit
}  // namespace js

bool jit::OffThreadCompilationAvailable(JSContext* cx) {
  // Even if off thread compilation is enabled, compilation must still occur
  // on the main thread in some cases.
  //
  // Require cpuCount > 1 so that Ion compilation jobs and active-thread
  // execution are not competing for the same resources.
  return cx->runtime()->canUseOffthreadIonCompilation() &&
         GetHelperThreadCPUCount() > 1 && CanUseExtraThreads();
}

MethodStatus jit::CanEnterIon(JSContext* cx, RunState& state) {
  MOZ_ASSERT(jit::IsIonEnabled(cx));

  HandleScript script = state.script();
  MOZ_ASSERT(!script->hasIonScript());

  // Skip if the script has been disabled.
  if (!script->canIonCompile()) {
    return Method_Skipped;
  }

  // Skip if the script is being compiled off thread.
  if (script->isIonCompilingOffThread()) {
    return Method_Skipped;
  }

  if (state.isInvoke()) {
    InvokeState& invoke = *state.asInvoke();

    if (TooManyActualArguments(invoke.args().length())) {
      JitSpew(JitSpew_IonAbort, "too many actual args");
      ForbidCompilation(cx, script);
      return Method_CantCompile;
    }

    if (TooManyFormalArguments(
            invoke.args().callee().as<JSFunction>().nargs())) {
      JitSpew(JitSpew_IonAbort, "too many args");
      ForbidCompilation(cx, script);
      return Method_CantCompile;
    }
  }

  // If --ion-eager is used, compile with Baseline first, so that we
  // can directly enter IonMonkey.
  if (JitOptions.eagerIonCompilation() && !script->hasBaselineScript()) {
    MethodStatus status =
        CanEnterBaselineMethod<BaselineTier::Compiler>(cx, state);
    if (status != Method_Compiled) {
      return status;
    }
    // Bytecode analysis may forbid compilation for a script.
    if (!script->canIonCompile()) {
      return Method_CantCompile;
    }
  }

  if (!script->hasBaselineScript()) {
    return Method_Skipped;
  }

  MOZ_ASSERT(!script->isIonCompilingOffThread());
  MOZ_ASSERT(script->canIonCompile());

  // Attempt compilation. Returns Method_Compiled if already compiled.
  MethodStatus status = Compile(cx, script, /* osrFrame = */ nullptr,
                                /* osrPc = */ nullptr);
  if (status != Method_Compiled) {
    if (status == Method_CantCompile) {
      ForbidCompilation(cx, script);
    }
    return status;
  }

  if (state.script()->baselineScript()->hasPendingIonCompileTask()) {
    LinkIonScript(cx, state.script());
    if (!state.script()->hasIonScript()) {
      return jit::Method_Skipped;
    }
  }

  return Method_Compiled;
}

static MethodStatus BaselineCanEnterAtEntry(JSContext* cx, HandleScript script,
                                            BaselineFrame* frame) {
  MOZ_ASSERT(jit::IsIonEnabled(cx));
  MOZ_ASSERT(script->canIonCompile());
  MOZ_ASSERT(!script->isIonCompilingOffThread());
  MOZ_ASSERT(!script->hasIonScript());
  MOZ_ASSERT(frame->isFunctionFrame());

  // Mark as forbidden if frame can't be handled.
  if (!CheckFrame(cx, frame)) {
    ForbidCompilation(cx, script);
    return Method_CantCompile;
  }

  if (script->baselineScript()->hasPendingIonCompileTask()) {
    LinkIonScript(cx, script);
    if (script->hasIonScript()) {
      return Method_Compiled;
    }
  }

  // Attempt compilation. Returns Method_Compiled if already compiled.
  MethodStatus status = Compile(cx, script, frame, nullptr);
  if (status != Method_Compiled) {
    if (status == Method_CantCompile) {
      ForbidCompilation(cx, script);
    }
    return status;
  }

  return Method_Compiled;
}

// Decide if a transition from baseline execution to Ion code should occur.
// May compile or recompile the target JSScript.
static MethodStatus BaselineCanEnterAtBranch(JSContext* cx, HandleScript script,
                                             BaselineFrame* osrFrame,
                                             jsbytecode* pc) {
  MOZ_ASSERT(jit::IsIonEnabled(cx));
  MOZ_ASSERT((JSOp)*pc == JSOp::LoopHead);

  // Skip if the script has been disabled.
  if (!script->canIonCompile()) {
    return Method_Skipped;
  }

  // Skip if the script is being compiled off thread.
  if (script->isIonCompilingOffThread()) {
    return Method_Skipped;
  }

  // Optionally ignore on user request.
  if (!JitOptions.osr) {
    return Method_Skipped;
  }

  // Mark as forbidden if frame can't be handled.
  if (!CheckFrame(cx, osrFrame)) {
    ForbidCompilation(cx, script);
    return Method_CantCompile;
  }

  // Check if the jitcode still needs to get linked and do this
  // to have a valid IonScript.
  if (script->baselineScript()->hasPendingIonCompileTask()) {
    LinkIonScript(cx, script);
  }

  // By default a recompilation doesn't happen on osr mismatch.
  // Decide if we want to force a recompilation if this happens too much.
  if (script->hasIonScript()) {
    if (pc == script->ionScript()->osrPc()) {
      return Method_Compiled;
    }

    uint32_t count = script->ionScript()->incrOsrPcMismatchCounter();
    if (count <= JitOptions.osrPcMismatchesBeforeRecompile &&
        !JitOptions.eagerIonCompilation()) {
      return Method_Skipped;
    }

    JitSpew(JitSpew_IonScripts, "Forcing OSR Mismatch Compilation");
    Invalidate(cx, script);
  }

  // Attempt compilation.
  // - Returns Method_Compiled if the right ionscript is present
  //   (Meaning it was present or a sequantial compile finished)
  // - Returns Method_Skipped if pc doesn't match
  //   (This means a background thread compilation with that pc could have
  //   started or not.)
  MethodStatus status = Compile(cx, script, osrFrame, pc);
  if (status != Method_Compiled) {
    if (status == Method_CantCompile) {
      ForbidCompilation(cx, script);
    }
    return status;
  }

  // Return the compilation was skipped when the osr pc wasn't adjusted.
  // This can happen when there was still an IonScript available and a
  // background compilation started, but hasn't finished yet.
  // Or when we didn't force a recompile.
  if (script->hasIonScript() && pc != script->ionScript()->osrPc()) {
    return Method_Skipped;
  }

  return Method_Compiled;
}

static bool IonCompileScriptForBaseline(JSContext* cx, BaselineFrame* frame,
                                        jsbytecode* pc) {
  MOZ_ASSERT(IsIonEnabled(cx));

  RootedScript script(cx, frame->script());
  bool isLoopHead = JSOp(*pc) == JSOp::LoopHead;

  // The Baseline JIT code checks for Ion disabled or compiling off-thread.
  MOZ_ASSERT(script->canIonCompile());
  MOZ_ASSERT(!script->isIonCompilingOffThread());

  // If Ion script exists, but PC is not at a loop entry, then Ion will be
  // entered for this script at an appropriate LOOPENTRY or the next time this
  // function is called.
  if (script->hasIonScript() && !isLoopHead) {
    JitSpew(JitSpew_BaselineOSR, "IonScript exists, but not at loop entry!");
    // TODO: ASSERT that a ion-script-already-exists checker stub doesn't exist.
    // TODO: Clear all optimized stubs.
    // TODO: Add a ion-script-already-exists checker stub.
    return true;
  }

  // Ensure that Ion-compiled code is available.
  JitSpew(JitSpew_BaselineOSR,
          "WarmUpCounter for %s:%u:%u reached %d at pc %p, trying to switch to "
          "Ion!",
          script->filename(), script->lineno(), script->column(),
          (int)script->getWarmUpCount(), (void*)pc);

  MethodStatus stat;
  if (isLoopHead) {
    JitSpew(JitSpew_BaselineOSR, "  Compile at loop head!");
    stat = BaselineCanEnterAtBranch(cx, script, frame, pc);
  } else if (frame->isFunctionFrame()) {
    JitSpew(JitSpew_BaselineOSR,
            "  Compile function from top for later entry!");
    stat = BaselineCanEnterAtEntry(cx, script, frame);
  } else {
    return true;
  }

  if (stat == Method_Error) {
    JitSpew(JitSpew_BaselineOSR, "  Compile with Ion errored!");
    return false;
  }

  if (stat == Method_CantCompile) {
    MOZ_ASSERT(!script->canIonCompile());
    JitSpew(JitSpew_BaselineOSR, "  Can't compile with Ion!");
  } else if (stat == Method_Skipped) {
    JitSpew(JitSpew_BaselineOSR, "  Skipped compile with Ion!");
  } else if (stat == Method_Compiled) {
    JitSpew(JitSpew_BaselineOSR, "  Compiled with Ion!");
  } else {
    MOZ_CRASH("Invalid MethodStatus!");
  }

  return true;
}

bool jit::IonCompileScriptForBaselineAtEntry(JSContext* cx,
                                             BaselineFrame* frame) {
  JSScript* script = frame->script();
  return IonCompileScriptForBaseline(cx, frame, script->code());
}

/* clang-format off */
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
/* clang-format on */

static IonOsrTempData* PrepareOsrTempData(JSContext* cx, BaselineFrame* frame,
                                          uint32_t frameSize, void* jitcode) {
  uint32_t numValueSlots = frame->numValueSlots(frameSize);

  // Calculate the amount of space to allocate:
  //      BaselineFrame space:
  //          (sizeof(Value) * numValueSlots)
  //        + sizeof(BaselineFrame)
  //
  //      IonOsrTempData space:
  //          sizeof(IonOsrTempData)

  size_t frameSpace = sizeof(BaselineFrame) + sizeof(Value) * numValueSlots;
  size_t ionOsrTempDataSpace = sizeof(IonOsrTempData);

  size_t totalSpace = AlignBytes(frameSpace, sizeof(Value)) +
                      AlignBytes(ionOsrTempDataSpace, sizeof(Value));

  JitRuntime* jrt = cx->runtime()->jitRuntime();
  uint8_t* buf = jrt->allocateIonOsrTempData(totalSpace);
  if (!buf) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  IonOsrTempData* info = new (buf) IonOsrTempData();
  info->jitcode = jitcode;

  // Copy the BaselineFrame + local/stack Values to the buffer. Arguments and
  // |this| are not copied but left on the stack: the Baseline and Ion frame
  // share the same frame prefix and Ion won't clobber these values. Note
  // that info->baselineFrame will point to the *end* of the frame data, like
  // the frame pointer register in baseline frames.
  uint8_t* frameStart =
      (uint8_t*)info + AlignBytes(ionOsrTempDataSpace, sizeof(Value));
  info->baselineFrame = frameStart + frameSpace;

  memcpy(frameStart, (uint8_t*)frame - numValueSlots * sizeof(Value),
         frameSpace);

  JitSpew(JitSpew_BaselineOSR, "Allocated IonOsrTempData at %p", info);
  JitSpew(JitSpew_BaselineOSR, "Jitcode is %p", info->jitcode);

  // All done.
  return info;
}

bool jit::IonCompileScriptForBaselineOSR(JSContext* cx, BaselineFrame* frame,
                                         uint32_t frameSize, jsbytecode* pc,
                                         IonOsrTempData** infoPtr) {
  MOZ_ASSERT(infoPtr);
  *infoPtr = nullptr;

  MOZ_ASSERT(frame->debugFrameSize() == frameSize);
  MOZ_ASSERT(JSOp(*pc) == JSOp::LoopHead);

  if (!IonCompileScriptForBaseline(cx, frame, pc)) {
    return false;
  }

  RootedScript script(cx, frame->script());
  if (!script->hasIonScript() || script->ionScript()->osrPc() != pc ||
      frame->isDebuggee()) {
    return true;
  }

  IonScript* ion = script->ionScript();
  MOZ_ASSERT(cx->runtime()->geckoProfiler().enabled() ==
             ion->hasProfilingInstrumentation());
  MOZ_ASSERT(ion->osrPc() == pc);

  ion->resetOsrPcMismatchCounter();

  JitSpew(JitSpew_BaselineOSR, "  OSR possible!");
  void* jitcode = ion->method()->raw() + ion->osrEntryOffset();

  // Prepare the temporary heap copy of the fake InterpreterFrame and actual
  // args list.
  JitSpew(JitSpew_BaselineOSR, "Got jitcode.  Preparing for OSR into ion.");
  IonOsrTempData* info = PrepareOsrTempData(cx, frame, frameSize, jitcode);
  if (!info) {
    return false;
  }

  *infoPtr = info;
  return true;
}

static void InvalidateActivation(JS::GCContext* gcx,
                                 const JitActivationIterator& activations,
                                 bool invalidateAll) {
  JitSpew(JitSpew_IonInvalidate, "BEGIN invalidating activation");

#ifdef CHECK_OSIPOINT_REGISTERS
  if (JitOptions.checkOsiPointRegisters) {
    activations->asJit()->setCheckRegs(false);
  }
#endif

  size_t frameno = 1;

  for (OnlyJSJitFrameIter iter(activations); !iter.done(); ++iter, ++frameno) {
    const JSJitFrameIter& frame = iter.frame();
    MOZ_ASSERT_IF(frameno == 1, frame.isExitFrame() ||
                                    frame.type() == FrameType::Bailout ||
                                    frame.type() == FrameType::JSJitToWasm);

#ifdef JS_JITSPEW
    switch (frame.type()) {
      case FrameType::Exit:
        JitSpew(JitSpew_IonInvalidate, "#%zu exit frame @ %p", frameno,
                frame.fp());
        break;
      case FrameType::JSJitToWasm:
        JitSpew(JitSpew_IonInvalidate, "#%zu wasm exit frame @ %p", frameno,
                frame.fp());
        break;
      case FrameType::BaselineJS:
      case FrameType::IonJS:
      case FrameType::Bailout: {
        MOZ_ASSERT(frame.isScripted());
        const char* type = "Unknown";
        if (frame.isIonJS()) {
          type = "Optimized";
        } else if (frame.isBaselineJS()) {
          type = "Baseline";
        } else if (frame.isBailoutJS()) {
          type = "Bailing";
        }
        JSScript* script = frame.maybeForwardedScript();
        JitSpew(JitSpew_IonInvalidate,
                "#%zu %s JS frame @ %p, %s:%u:%u (fun: %p, script: %p, pc %p)",
                frameno, type, frame.fp(), script->maybeForwardedFilename(),
                script->lineno(), script->column(), frame.maybeCallee(), script,
                frame.resumePCinCurrentFrame());
        break;
      }
      case FrameType::BaselineStub:
        JitSpew(JitSpew_IonInvalidate, "#%zu baseline stub frame @ %p", frameno,
                frame.fp());
        break;
      case FrameType::BaselineInterpreterEntry:
        JitSpew(JitSpew_IonInvalidate,
                "#%zu baseline interpreter entry frame @ %p", frameno,
                frame.fp());
        break;
      case FrameType::Rectifier:
        JitSpew(JitSpew_IonInvalidate, "#%zu rectifier frame @ %p", frameno,
                frame.fp());
        break;
      case FrameType::IonICCall:
        JitSpew(JitSpew_IonInvalidate, "#%zu ion IC call frame @ %p", frameno,
                frame.fp());
        break;
      case FrameType::CppToJSJit:
        JitSpew(JitSpew_IonInvalidate, "#%zu entry frame @ %p", frameno,
                frame.fp());
        break;
      case FrameType::WasmToJSJit:
        JitSpew(JitSpew_IonInvalidate, "#%zu wasm frames @ %p", frameno,
                frame.fp());
        break;
    }
#endif  // JS_JITSPEW

    if (!frame.isIonScripted()) {
      continue;
    }

    // See if the frame has already been invalidated.
    if (frame.checkInvalidation()) {
      continue;
    }

    JSScript* script = frame.maybeForwardedScript();
    if (!script->hasIonScript()) {
      continue;
    }

    if (!invalidateAll && !script->ionScript()->invalidated()) {
      continue;
    }

    IonScript* ionScript = script->ionScript();

    // Purge ICs before we mark this script as invalidated. This will
    // prevent lastJump_ from appearing to be a bogus pointer, just
    // in case anyone tries to read it.
    ionScript->purgeICs(script->zone());

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

    // We're about to remove edges from the JSScript to GC things embedded in
    // the JitCode. Perform a barrier to let the GC know about those edges.
    PreWriteBarrier(script->zone(), ionCode, [](JSTracer* trc, JitCode* code) {
      code->traceChildren(trc);
    });

    ionCode->setInvalidated();

    // Don't adjust OSI points in a bailout path.
    if (frame.isBailoutJS()) {
      continue;
    }

    // Write the delta (from the return address offset to the
    // IonScript pointer embedded into the invalidation epilogue)
    // where the safepointed call instruction used to be. We rely on
    // the call sequence causing the safepoint being >= the size of
    // a uint32, which is checked during safepoint index
    // construction.
    AutoWritableJitCode awjc(ionCode);
    const SafepointIndex* si =
        ionScript->getSafepointIndex(frame.resumePCinCurrentFrame());
    CodeLocationLabel dataLabelToMunge(frame.resumePCinCurrentFrame());
    ptrdiff_t delta = ionScript->invalidateEpilogueDataOffset() -
                      (frame.resumePCinCurrentFrame() - ionCode->raw());
    Assembler::PatchWrite_Imm32(dataLabelToMunge, Imm32(delta));

    CodeLocationLabel osiPatchPoint =
        SafepointReader::InvalidationPatchPoint(ionScript, si);
    CodeLocationLabel invalidateEpilogue(
        ionCode, CodeOffset(ionScript->invalidateEpilogueOffset()));

    JitSpew(
        JitSpew_IonInvalidate,
        "   ! Invalidate ionScript %p (inv count %zu) -> patching osipoint %p",
        ionScript, ionScript->invalidationCount(), (void*)osiPatchPoint.raw());
    Assembler::PatchWrite_NearCall(osiPatchPoint, invalidateEpilogue);
  }

  JitSpew(JitSpew_IonInvalidate, "END invalidating activation");
}

void jit::InvalidateAll(JS::GCContext* gcx, Zone* zone) {
  // The caller should previously have cancelled off thread compilation.
#ifdef DEBUG
  for (RealmsInZoneIter realm(zone); !realm.done(); realm.next()) {
    MOZ_ASSERT(!HasOffThreadIonCompile(realm));
  }
#endif
  if (zone->isAtomsZone()) {
    return;
  }
  JSContext* cx = TlsContext.get();
  for (JitActivationIterator iter(cx); !iter.done(); ++iter) {
    if (iter->compartment()->zone() == zone) {
      JitSpew(JitSpew_IonInvalidate, "Invalidating all frames for GC");
      InvalidateActivation(gcx, iter, true);
    }
  }
}

static void ClearIonScriptAfterInvalidation(JSContext* cx, JSScript* script,
                                            IonScript* ionScript,
                                            bool resetUses) {
  // Null out the JitScript's IonScript pointer. The caller is responsible for
  // destroying the IonScript using the invalidation count mechanism.
  DebugOnly<IonScript*> clearedIonScript =
      script->jitScript()->clearIonScript(cx->gcContext(), script);
  MOZ_ASSERT(clearedIonScript == ionScript);

  // Wait for the scripts to get warm again before doing another
  // compile, unless we are recompiling *because* a script got hot
  // (resetUses is false).
  if (resetUses) {
    script->resetWarmUpCounterToDelayIonCompilation();
  }
}

void jit::Invalidate(JSContext* cx, const RecompileInfoVector& invalid,
                     bool resetUses, bool cancelOffThread) {
  JitSpew(JitSpew_IonInvalidate, "Start invalidation.");

  // Add an invalidation reference to all invalidated IonScripts to indicate
  // to the traversal which frames have been invalidated.
  size_t numInvalidations = 0;
  for (const RecompileInfo& info : invalid) {
    if (cancelOffThread) {
      CancelOffThreadIonCompile(info.script());
    }

    IonScript* ionScript = info.maybeIonScriptToInvalidate();
    if (!ionScript) {
      continue;
    }

    JitSpew(JitSpew_IonInvalidate, " Invalidate %s:%u:%u, IonScript %p",
            info.script()->filename(), info.script()->lineno(),
            info.script()->column(), ionScript);

    // Keep the ion script alive during the invalidation and flag this
    // ionScript as being invalidated.  This increment is removed by the
    // loop after the calls to InvalidateActivation.
    ionScript->incrementInvalidationCount();
    numInvalidations++;
  }

  if (!numInvalidations) {
    JitSpew(JitSpew_IonInvalidate, " No IonScript invalidation.");
    return;
  }

  JS::GCContext* gcx = cx->gcContext();
  for (JitActivationIterator iter(cx); !iter.done(); ++iter) {
    InvalidateActivation(gcx, iter, false);
  }

  // Drop the references added above. If a script was never active, its
  // IonScript will be immediately destroyed. Otherwise, it will be held live
  // until its last invalidated frame is destroyed.
  for (const RecompileInfo& info : invalid) {
    IonScript* ionScript = info.maybeIonScriptToInvalidate();
    if (!ionScript) {
      continue;
    }

    if (ionScript->invalidationCount() == 1) {
      // decrementInvalidationCount will destroy the IonScript so null out
      // jitScript->ionScript_ now. We don't want to do this unconditionally
      // because maybeIonScriptToInvalidate depends on script->ionScript() (we
      // would leak the IonScript if |invalid| contains duplicates).
      ClearIonScriptAfterInvalidation(cx, info.script(), ionScript, resetUses);
    }

    ionScript->decrementInvalidationCount(gcx);
    numInvalidations--;
  }

  // Make sure we didn't leak references by invalidating the same IonScript
  // multiple times in the above loop.
  MOZ_ASSERT(!numInvalidations);

  // Finally, null out jitScript->ionScript_ for IonScripts that are still on
  // the stack.
  for (const RecompileInfo& info : invalid) {
    if (IonScript* ionScript = info.maybeIonScriptToInvalidate()) {
      ClearIonScriptAfterInvalidation(cx, info.script(), ionScript, resetUses);
    }
  }
}

void jit::IonScript::invalidate(JSContext* cx, JSScript* script, bool resetUses,
                                const char* reason) {
  // Note: we could short circuit here if we already invalidated this
  // IonScript, but jit::Invalidate also cancels off-thread compilations of
  // |script|.
  MOZ_RELEASE_ASSERT(invalidated() || script->ionScript() == this);

  JitSpew(JitSpew_IonInvalidate, " Invalidate IonScript %p: %s", this, reason);

  // RecompileInfoVector has inline space for at least one element.
  RecompileInfoVector list;
  MOZ_RELEASE_ASSERT(list.reserve(1));
  list.infallibleEmplaceBack(script, compilationId());

  Invalidate(cx, list, resetUses, true);
}

void jit::Invalidate(JSContext* cx, JSScript* script, bool resetUses,
                     bool cancelOffThread) {
  MOZ_ASSERT(script->hasIonScript());

  if (cx->runtime()->geckoProfiler().enabled()) {
    // Register invalidation with profiler.
    // Format of event payload string:
    //      "<filename>:<lineno>"

    // Get the script filename, if any, and its length.
    const char* filename = script->filename();
    if (filename == nullptr) {
      filename = "<unknown>";
    }

    // Construct the descriptive string.
    UniqueChars buf =
        JS_smprintf("%s:%u:%u", filename, script->lineno(), script->column());

    // Ignore the event on allocation failure.
    if (buf) {
      cx->runtime()->geckoProfiler().markEvent("Invalidate", buf.get());
    }
  }

  // RecompileInfoVector has inline space for at least one element.
  RecompileInfoVector scripts;
  MOZ_ASSERT(script->hasIonScript());
  MOZ_RELEASE_ASSERT(scripts.reserve(1));
  scripts.infallibleEmplaceBack(script, script->ionScript()->compilationId());

  Invalidate(cx, scripts, resetUses, cancelOffThread);
}

void jit::FinishInvalidation(JS::GCContext* gcx, JSScript* script) {
  if (!script->hasIonScript()) {
    return;
  }

  // In all cases, null out jitScript->ionScript_ to avoid re-entry.
  IonScript* ion = script->jitScript()->clearIonScript(gcx, script);

  // If this script has Ion code on the stack, invalidated() will return
  // true. In this case we have to wait until destroying it.
  if (!ion->invalidated()) {
    jit::IonScript::Destroy(gcx, ion);
  }
}

void jit::ForbidCompilation(JSContext* cx, JSScript* script) {
  JitSpew(JitSpew_IonAbort, "Disabling Ion compilation of script %s:%u:%u",
          script->filename(), script->lineno(), script->column());

  CancelOffThreadIonCompile(script);

  if (script->hasIonScript()) {
    Invalidate(cx, script, false);
  }

  script->disableIon();
}

size_t jit::SizeOfIonData(JSScript* script,
                          mozilla::MallocSizeOf mallocSizeOf) {
  size_t result = 0;

  if (script->hasIonScript()) {
    result += script->ionScript()->sizeOfIncludingThis(mallocSizeOf);
  }

  return result;
}

// If you change these, please also change the comment in TempAllocator.
/* static */ const size_t TempAllocator::BallastSize = 16 * 1024;
/* static */ const size_t TempAllocator::PreferredLifoChunkSize = 32 * 1024;
