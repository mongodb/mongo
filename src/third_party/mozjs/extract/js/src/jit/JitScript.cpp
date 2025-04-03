/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitScript-inl.h"

#include "mozilla/BinarySearch.h"
#include "mozilla/CheckedInt.h"

#include <utility>

#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/BytecodeAnalysis.h"
#include "jit/IonScript.h"
#include "jit/JitFrames.h"
#include "jit/JitSpewer.h"
#include "jit/ScriptFromCalleeToken.h"
#include "jit/TrialInlining.h"
#include "vm/BytecodeUtil.h"
#include "vm/Compartment.h"
#include "vm/FrameIter.h"  // js::OnlyJSJitFrameIter
#include "vm/JitActivation.h"
#include "vm/JSScript.h"

#include "gc/GCContext-inl.h"
#include "jit/JSJitFrameIter-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::CheckedInt;

JitScript::JitScript(JSScript* script, Offset fallbackStubsOffset,
                     Offset endOffset, const char* profileString)
    : profileString_(profileString),
      endOffset_(endOffset),
      icScript_(script->getWarmUpCount(),
                fallbackStubsOffset - offsetOfICScript(),
                endOffset - offsetOfICScript(),
                /*depth=*/0) {
  // Ensure the baselineScript_ and ionScript_ fields match the BaselineDisabled
  // and IonDisabled script flags.
  if (!script->canBaselineCompile()) {
    setBaselineScriptImpl(script, BaselineDisabledScriptPtr);
  }
  if (!script->canIonCompile()) {
    setIonScriptImpl(script, IonDisabledScriptPtr);
  }
}

#ifdef DEBUG
JitScript::~JitScript() {
  // The contents of the stub space are removed and freed separately after the
  // next minor GC. See prepareForDestruction.
  MOZ_ASSERT(jitScriptStubSpace_.isEmpty());

  // BaselineScript and IonScript must have been destroyed at this point.
  MOZ_ASSERT(!hasBaselineScript());
  MOZ_ASSERT(!hasIonScript());
}
#else
JitScript::~JitScript() = default;
#endif

bool JSScript::createJitScript(JSContext* cx) {
  MOZ_ASSERT(!hasJitScript());
  cx->check(this);

  // Scripts with a JitScript can run in the Baseline Interpreter. Make sure
  // we don't create a JitScript for scripts we shouldn't Baseline interpret.
  MOZ_ASSERT_IF(IsBaselineInterpreterEnabled(),
                CanBaselineInterpretScript(this));

  // Store the profile string in the JitScript if the profiler is enabled.
  const char* profileString = nullptr;
  if (cx->runtime()->geckoProfiler().enabled()) {
    profileString = cx->runtime()->geckoProfiler().profileString(cx, this);
    if (!profileString) {
      return false;
    }
  }

  static_assert(sizeof(JitScript) % sizeof(uintptr_t) == 0,
                "Trailing arrays must be aligned properly");
  static_assert(sizeof(ICEntry) % sizeof(uintptr_t) == 0,
                "Trailing arrays must be aligned properly");

  static_assert(
      sizeof(JitScript) == offsetof(JitScript, icScript_) + sizeof(ICScript),
      "icScript_ must be the last field");

  // Calculate allocation size.
  CheckedInt<uint32_t> allocSize = sizeof(JitScript);
  allocSize += CheckedInt<uint32_t>(numICEntries()) * sizeof(ICEntry);
  allocSize += CheckedInt<uint32_t>(numICEntries()) * sizeof(ICFallbackStub);
  if (!allocSize.isValid()) {
    ReportAllocationOverflow(cx);
    return false;
  }

  void* raw = cx->pod_malloc<uint8_t>(allocSize.value());
  MOZ_ASSERT(uintptr_t(raw) % alignof(JitScript) == 0);
  if (!raw) {
    return false;
  }

  size_t fallbackStubsOffset =
      sizeof(JitScript) + numICEntries() * sizeof(ICEntry);

  UniquePtr<JitScript> jitScript(new (raw) JitScript(
      this, fallbackStubsOffset, allocSize.value(), profileString));

  // Sanity check the length computation.
  MOZ_ASSERT(jitScript->numICEntries() == numICEntries());

  jitScript->icScript()->initICEntries(cx, this);

  warmUpData_.initJitScript(jitScript.release());
  AddCellMemory(this, allocSize.value(), MemoryUse::JitScript);

  // We have a JitScript so we can set the script's jitCodeRaw pointer to the
  // Baseline Interpreter code.
  updateJitCodeRaw(cx->runtime());

  return true;
}

void JSScript::maybeReleaseJitScript(JS::GCContext* gcx) {
  MOZ_ASSERT(hasJitScript());

  if (zone()->jitZone()->keepJitScripts() || jitScript()->hasBaselineScript() ||
      jitScript()->active()) {
    return;
  }

  releaseJitScript(gcx);
}

void JSScript::releaseJitScript(JS::GCContext* gcx) {
  MOZ_ASSERT(hasJitScript());
  MOZ_ASSERT(!hasBaselineScript());
  MOZ_ASSERT(!hasIonScript());

  gcx->removeCellMemory(this, jitScript()->allocBytes(), MemoryUse::JitScript);

  JitScript::Destroy(zone(), jitScript());
  warmUpData_.clearJitScript();
  updateJitCodeRaw(gcx->runtime());
}

void JSScript::releaseJitScriptOnFinalize(JS::GCContext* gcx) {
  MOZ_ASSERT(hasJitScript());

  if (hasIonScript()) {
    IonScript* ion = jitScript()->clearIonScript(gcx, this);
    jit::IonScript::Destroy(gcx, ion);
  }

  if (hasBaselineScript()) {
    BaselineScript* baseline = jitScript()->clearBaselineScript(gcx, this);
    jit::BaselineScript::Destroy(gcx, baseline);
  }

  releaseJitScript(gcx);
}

void JitScript::trace(JSTracer* trc) {
  icScript_.trace(trc);

  if (hasBaselineScript()) {
    baselineScript()->trace(trc);
  }

  if (hasIonScript()) {
    ionScript()->trace(trc);
  }

  if (templateEnv_.isSome()) {
    TraceNullableEdge(trc, templateEnv_.ptr(), "jitscript-template-env");
  }

  if (hasInliningRoot()) {
    inliningRoot()->trace(trc);
  }
}

void ICScript::trace(JSTracer* trc) {
  // Mark all IC stub codes hanging off the IC stub entries.
  for (size_t i = 0; i < numICEntries(); i++) {
    ICEntry& ent = icEntry(i);
    ent.trace(trc);
  }
}

bool ICScript::addInlinedChild(JSContext* cx, UniquePtr<ICScript> child,
                               uint32_t pcOffset) {
  MOZ_ASSERT(!hasInlinedChild(pcOffset));

  if (!inlinedChildren_) {
    inlinedChildren_ = cx->make_unique<Vector<CallSite>>(cx);
    if (!inlinedChildren_) {
      return false;
    }
  }

  // First reserve space in inlinedChildren_ to ensure that if the ICScript is
  // added to the inlining root, it can also be added to inlinedChildren_.
  CallSite callsite(child.get(), pcOffset);
  if (!inlinedChildren_->reserve(inlinedChildren_->length() + 1)) {
    return false;
  }
  if (!inliningRoot()->addInlinedScript(std::move(child))) {
    return false;
  }
  inlinedChildren_->infallibleAppend(callsite);
  return true;
}

ICScript* ICScript::findInlinedChild(uint32_t pcOffset) {
  for (auto& callsite : *inlinedChildren_) {
    if (callsite.pcOffset_ == pcOffset) {
      return callsite.callee_;
    }
  }
  MOZ_CRASH("Inlined child expected at pcOffset");
}

void ICScript::removeInlinedChild(uint32_t pcOffset) {
  MOZ_ASSERT(inliningRoot());
  inlinedChildren_->eraseIf([pcOffset](const CallSite& callsite) -> bool {
    return callsite.pcOffset_ == pcOffset;
  });
}

bool ICScript::hasInlinedChild(uint32_t pcOffset) {
  if (!inlinedChildren_) {
    return false;
  }
  for (auto& callsite : *inlinedChildren_) {
    if (callsite.pcOffset_ == pcOffset) {
      return true;
    }
  }
  return false;
}

void JitScript::resetWarmUpCount(uint32_t count) {
  icScript_.resetWarmUpCount(count);
  if (hasInliningRoot()) {
    inliningRoot()->resetWarmUpCounts(count);
  }
}

void JitScript::ensureProfileString(JSContext* cx, JSScript* script) {
  MOZ_ASSERT(cx->runtime()->geckoProfiler().enabled());

  if (profileString_) {
    return;
  }

  AutoEnterOOMUnsafeRegion oomUnsafe;
  profileString_ = cx->runtime()->geckoProfiler().profileString(cx, script);
  if (!profileString_) {
    oomUnsafe.crash("Failed to allocate profile string");
  }
}

/* static */
void JitScript::Destroy(Zone* zone, JitScript* script) {
  script->prepareForDestruction(zone);

  js_delete(script);
}

void JitScript::prepareForDestruction(Zone* zone) {
  // When the script contains pointers to nursery things, the store buffer can
  // contain entries that point into the fallback stub space. Since we can
  // destroy scripts outside the context of a GC, this situation could result
  // in us trying to mark invalid store buffer entries.
  //
  // Defer freeing any allocated blocks until after the next minor GC.
  jitScriptStubSpace_.freeAllAfterMinorGC(zone);

  // Trigger write barriers.
  baselineScript_.set(zone, nullptr);
  ionScript_.set(zone, nullptr);
}

struct FallbackStubs {
  ICScript* const icScript_;

  explicit FallbackStubs(ICScript* icScript) : icScript_(icScript) {}

  size_t numEntries() const { return icScript_->numICEntries(); }
  ICFallbackStub* operator[](size_t index) const {
    return icScript_->fallbackStub(index);
  }
};

static bool ComputeBinarySearchMid(FallbackStubs stubs, uint32_t pcOffset,
                                   size_t* loc) {
  return mozilla::BinarySearchIf(
      stubs, 0, stubs.numEntries(),
      [pcOffset](const ICFallbackStub* stub) {
        if (pcOffset < stub->pcOffset()) {
          return -1;
        }
        if (stub->pcOffset() < pcOffset) {
          return 1;
        }
        return 0;
      },
      loc);
}

ICEntry& ICScript::icEntryFromPCOffset(uint32_t pcOffset) {
  size_t mid;
  MOZ_ALWAYS_TRUE(ComputeBinarySearchMid(FallbackStubs(this), pcOffset, &mid));

  MOZ_ASSERT(mid < numICEntries());

  ICEntry& entry = icEntry(mid);
  MOZ_ASSERT(fallbackStubForICEntry(&entry)->pcOffset() == pcOffset);
  return entry;
}

ICEntry* ICScript::interpreterICEntryFromPCOffset(uint32_t pcOffset) {
  // We have to return the entry to store in BaselineFrame::interpreterICEntry
  // when resuming in the Baseline Interpreter at pcOffset. The bytecode op at
  // pcOffset does not necessarily have an ICEntry, so we want to return the
  // first ICEntry for which the following is true:
  //
  //    entry.pcOffset() >= pcOffset
  //
  // Fortunately, ComputeBinarySearchMid returns exactly this entry.

  size_t mid;
  ComputeBinarySearchMid(FallbackStubs(this), pcOffset, &mid);

  if (mid < numICEntries()) {
    ICEntry& entry = icEntry(mid);
    MOZ_ASSERT(fallbackStubForICEntry(&entry)->pcOffset() >= pcOffset);
    return &entry;
  }

  // Resuming at a pc after the last ICEntry. Just return nullptr:
  // BaselineFrame::interpreterICEntry will never be used in this case.
  return nullptr;
}

void JitScript::purgeOptimizedStubs(JSScript* script) {
  MOZ_ASSERT(script->jitScript() == this);

  Zone* zone = script->zone();
  if (IsAboutToBeFinalizedUnbarriered(script)) {
    // We're sweeping and the script is dead. Don't purge optimized stubs
    // because (1) accessing CacheIRStubInfo pointers in ICStubs is invalid
    // because we may have swept them already when we started (incremental)
    // sweeping and (2) it's unnecessary because this script will be finalized
    // soon anyway.
    return;
  }

  JitSpew(JitSpew_BaselineIC, "Purging optimized stubs");

  icScript()->purgeOptimizedStubs(zone);
  if (hasInliningRoot()) {
    inliningRoot()->purgeOptimizedStubs(zone);
  }
#ifdef DEBUG
  failedICHash_.reset();
  hasPurgedStubs_ = true;
#endif
}

void ICScript::purgeOptimizedStubs(Zone* zone) {
  for (size_t i = 0; i < numICEntries(); i++) {
    ICEntry& entry = icEntry(i);
    ICStub* lastStub = entry.firstStub();
    while (!lastStub->isFallback()) {
      lastStub = lastStub->toCacheIRStub()->next();
    }

    // Unlink all stubs allocated in the optimized space.
    ICStub* stub = entry.firstStub();
    ICCacheIRStub* prev = nullptr;

    while (stub != lastStub) {
      if (!stub->toCacheIRStub()->allocatedInFallbackSpace()) {
        lastStub->toFallbackStub()->unlinkStub(zone, &entry, prev,
                                               stub->toCacheIRStub());
        stub = stub->toCacheIRStub()->next();
        continue;
      }

      prev = stub->toCacheIRStub();
      stub = stub->toCacheIRStub()->next();
    }

    lastStub->toFallbackStub()->clearHasFoldedStub();
  }

#ifdef DEBUG
  // All remaining stubs must be allocated in the fallback space.
  for (size_t i = 0; i < numICEntries(); i++) {
    ICEntry& entry = icEntry(i);
    ICStub* stub = entry.firstStub();
    while (!stub->isFallback()) {
      MOZ_ASSERT(stub->toCacheIRStub()->allocatedInFallbackSpace());
      stub = stub->toCacheIRStub()->next();
    }
  }
#endif
}

bool JitScript::ensureHasCachedBaselineJitData(JSContext* cx,
                                               HandleScript script) {
  if (templateEnv_.isSome()) {
    return true;
  }

  if (!script->function() ||
      !script->function()->needsFunctionEnvironmentObjects()) {
    templateEnv_.emplace();
    return true;
  }

  Rooted<EnvironmentObject*> templateEnv(cx);
  Rooted<JSFunction*> fun(cx, script->function());

  if (fun->needsNamedLambdaEnvironment()) {
    templateEnv = NamedLambdaObject::createTemplateObject(cx, fun);
    if (!templateEnv) {
      return false;
    }
  }

  if (fun->needsCallObject()) {
    templateEnv = CallObject::createTemplateObject(cx, script, templateEnv);
    if (!templateEnv) {
      return false;
    }
  }

  templateEnv_.emplace(templateEnv);
  return true;
}

bool JitScript::ensureHasCachedIonData(JSContext* cx, HandleScript script) {
  MOZ_ASSERT(script->jitScript() == this);

  if (usesEnvironmentChain_.isSome()) {
    return true;
  }

  if (!ensureHasCachedBaselineJitData(cx, script)) {
    return false;
  }

  usesEnvironmentChain_.emplace(ScriptUsesEnvironmentChain(script));
  return true;
}

void JitScript::setBaselineScriptImpl(JSScript* script,
                                      BaselineScript* baselineScript) {
  JSRuntime* rt = script->runtimeFromMainThread();
  setBaselineScriptImpl(rt->gcContext(), script, baselineScript);
}

void JitScript::setBaselineScriptImpl(JS::GCContext* gcx, JSScript* script,
                                      BaselineScript* baselineScript) {
  if (hasBaselineScript()) {
    gcx->removeCellMemory(script, baselineScript_->allocBytes(),
                          MemoryUse::BaselineScript);
    baselineScript_.set(script->zone(), nullptr);
  }

  MOZ_ASSERT(ionScript_ == nullptr || ionScript_ == IonDisabledScriptPtr);

  baselineScript_.set(script->zone(), baselineScript);
  if (hasBaselineScript()) {
    AddCellMemory(script, baselineScript_->allocBytes(),
                  MemoryUse::BaselineScript);
  }

  script->resetWarmUpResetCounter();
  script->updateJitCodeRaw(gcx->runtime());
}

void JitScript::setIonScriptImpl(JSScript* script, IonScript* ionScript) {
  JSRuntime* rt = script->runtimeFromMainThread();
  setIonScriptImpl(rt->gcContext(), script, ionScript);
}

void JitScript::setIonScriptImpl(JS::GCContext* gcx, JSScript* script,
                                 IonScript* ionScript) {
  MOZ_ASSERT_IF(ionScript != IonDisabledScriptPtr,
                !baselineScript()->hasPendingIonCompileTask());

  JS::Zone* zone = script->zone();
  if (hasIonScript()) {
    gcx->removeCellMemory(script, ionScript_->allocBytes(),
                          MemoryUse::IonScript);
    ionScript_.set(zone, nullptr);
  }

  ionScript_.set(zone, ionScript);
  MOZ_ASSERT_IF(hasIonScript(), hasBaselineScript());
  if (hasIonScript()) {
    AddCellMemory(script, ionScript_->allocBytes(), MemoryUse::IonScript);
  }

  script->updateJitCodeRaw(gcx->runtime());
}

#ifdef JS_STRUCTURED_SPEW
static bool HasEnteredCounters(ICEntry& entry) {
  ICStub* stub = entry.firstStub();
  if (stub && !stub->isFallback()) {
    return true;
  }
  return false;
}

void jit::JitSpewBaselineICStats(JSScript* script, const char* dumpReason) {
  MOZ_ASSERT(script->hasJitScript());
  JSContext* cx = TlsContext.get();
  AutoStructuredSpewer spew(cx, SpewChannel::BaselineICStats, script);
  if (!spew) {
    return;
  }

  JitScript* jitScript = script->jitScript();
  spew->property("reason", dumpReason);
  spew->beginListProperty("entries");
  for (size_t i = 0; i < jitScript->numICEntries(); i++) {
    ICEntry& entry = jitScript->icEntry(i);
    ICFallbackStub* fallback = jitScript->fallbackStub(i);
    if (!HasEnteredCounters(entry)) {
      continue;
    }

    uint32_t pcOffset = fallback->pcOffset();
    jsbytecode* pc = script->offsetToPC(pcOffset);

    unsigned column;
    unsigned int line = PCToLineNumber(script, pc, &column);

    spew->beginObject();
    spew->property("op", CodeName(JSOp(*pc)));
    spew->property("pc", pcOffset);
    spew->property("line", line);
    spew->property("column", column);

    spew->beginListProperty("counts");
    ICStub* stub = entry.firstStub();
    while (stub && !stub->isFallback()) {
      uint32_t count = stub->enteredCount();
      spew->value(count);
      stub = stub->toCacheIRStub()->next();
    }
    spew->endList();
    spew->property("fallback_count", fallback->enteredCount());
    spew->endObject();
  }
  spew->endList();
}
#endif

static void MarkActiveJitScripts(JSContext* cx,
                                 const JitActivationIterator& activation) {
  for (OnlyJSJitFrameIter iter(activation); !iter.done(); ++iter) {
    const JSJitFrameIter& frame = iter.frame();
    switch (frame.type()) {
      case FrameType::BaselineJS:
        frame.script()->jitScript()->setActive();
        break;
      case FrameType::Exit:
        if (frame.exitFrame()->is<LazyLinkExitFrameLayout>()) {
          LazyLinkExitFrameLayout* ll =
              frame.exitFrame()->as<LazyLinkExitFrameLayout>();
          JSScript* script =
              ScriptFromCalleeToken(ll->jsFrame()->calleeToken());
          script->jitScript()->setActive();
        }
        break;
      case FrameType::Bailout:
      case FrameType::IonJS: {
        // Keep the JitScript and BaselineScript around, since bailouts from
        // the ion jitcode need to re-enter into the Baseline code.
        frame.script()->jitScript()->setActive();
        for (InlineFrameIterator inlineIter(cx, &frame); inlineIter.more();
             ++inlineIter) {
          inlineIter.script()->jitScript()->setActive();
        }
        break;
      }
      default:;
    }
  }
}

void jit::MarkActiveJitScripts(Zone* zone) {
  if (zone->isAtomsZone()) {
    return;
  }
  JSContext* cx = TlsContext.get();
  for (JitActivationIterator iter(cx); !iter.done(); ++iter) {
    if (iter->compartment()->zone() == zone) {
      MarkActiveJitScripts(cx, iter);
    }
  }
}

InliningRoot* JitScript::getOrCreateInliningRoot(JSContext* cx,
                                                 JSScript* script) {
  if (!inliningRoot_) {
    inliningRoot_ = js::MakeUnique<InliningRoot>(cx, script);
    if (!inliningRoot_) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    icScript_.inliningRoot_ = inliningRoot_.get();
  }
  return inliningRoot_.get();
}

gc::AllocSite* JitScript::createAllocSite(JSScript* script) {
  MOZ_ASSERT(script->jitScript() == this);

  Nursery& nursery = script->runtimeFromMainThread()->gc.nursery();
  if (!nursery.canCreateAllocSite()) {
    // Don't block attaching an optimized stub, but don't process allocations
    // for this site.
    return script->zone()->unknownAllocSite(JS::TraceKind::Object);
  }

  if (!allocSites_.reserve(allocSites_.length() + 1)) {
    return nullptr;
  }

  ICStubSpace* stubSpace = jitScriptStubSpace();
  auto* site =
      static_cast<gc::AllocSite*>(stubSpace->alloc(sizeof(gc::AllocSite)));
  if (!site) {
    return nullptr;
  }

  new (site) gc::AllocSite(script->zone(), script, JS::TraceKind::Object);

  allocSites_.infallibleAppend(site);

  nursery.noteAllocSiteCreated();

  return site;
}

bool JitScript::resetAllocSites(bool resetNurserySites,
                                bool resetPretenuredSites) {
  MOZ_ASSERT(resetNurserySites || resetPretenuredSites);

  bool anyReset = false;

  for (gc::AllocSite* site : allocSites_) {
    if ((resetNurserySites && site->initialHeap() == gc::Heap::Default) ||
        (resetPretenuredSites && site->initialHeap() == gc::Heap::Tenured)) {
      if (site->maybeResetState()) {
        anyReset = true;
      }
    }
  }

  return anyReset;
}

JitScriptICStubSpace* ICScript::jitScriptStubSpace() {
  if (isInlined()) {
    return inliningRoot_->jitScriptStubSpace();
  }
  return outerJitScript()->jitScriptStubSpace();
}

JitScript* ICScript::outerJitScript() {
  MOZ_ASSERT(!isInlined());
  uint8_t* ptr = reinterpret_cast<uint8_t*>(this);
  return reinterpret_cast<JitScript*>(ptr - JitScript::offsetOfICScript());
}

#ifdef DEBUG
// This hash is used to verify that we do not recompile after a
// TranspiledCacheIR invalidation with the exact same ICs.
//
// It should change iff an ICEntry in this ICScript (or an ICScript
// inlined into this ICScript) is modified such that we will make a
// different decision in WarpScriptOracle::maybeInlineIC. This means:
//
// 1. The hash will change if we attach a new stub.
// 2. The hash will change if the entered count of any CacheIR stub
//    other than the first changes from 0.
// 3. The hash will change if the entered count of the fallback stub
//    changes from 0.
//
HashNumber ICScript::hash() {
  HashNumber h = 0;
  for (size_t i = 0; i < numICEntries(); i++) {
    ICStub* stub = icEntry(i).firstStub();

    // Hash the address of the first stub.
    h = mozilla::AddToHash(h, stub);

    // Hash whether subsequent stubs have entry count 0.
    if (!stub->isFallback()) {
      stub = stub->toCacheIRStub()->next();
      while (!stub->isFallback()) {
        h = mozilla::AddToHash(h, stub->enteredCount() == 0);
        stub = stub->toCacheIRStub()->next();
      }
    }

    // Hash whether the fallback has entry count 0.
    MOZ_ASSERT(stub->isFallback());
    h = mozilla::AddToHash(h, stub->enteredCount() == 0);
  }

  return h;
}
#endif
