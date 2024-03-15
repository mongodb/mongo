/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/shadow/Realm.h"  // JS::shadow::Realm
#include "vm/Realm-inl.h"

#include "mozilla/MemoryReporting.h"

#include <stddef.h>

#include "jsfriendapi.h"

#include "builtin/WrappedFunctionObject.h"
#include "debugger/DebugAPI.h"
#include "debugger/Debugger.h"
#include "debugger/Environment.h"
#include "debugger/Frame.h"
#include "debugger/Script.h"
#include "debugger/Source.h"
#include "gc/GC.h"
#include "jit/JitRealm.h"
#include "jit/JitRuntime.h"
#include "js/CallAndConstruct.h"      // JS::IsCallable
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/GCVariant.h"
#include "js/Proxy.h"
#include "js/RootingAPI.h"
#include "js/Wrapper.h"
#include "vm/Compartment.h"
#include "vm/DateTime.h"
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/PIC.h"

#include "gc/Marking-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

Realm::DebuggerVectorEntry::DebuggerVectorEntry(js::Debugger* dbg_,
                                                JSObject* link)
    : dbg(dbg_), debuggerLink(link) {}

ObjectRealm::ObjectRealm(JS::Zone* zone)
    : innerViews(zone, zone), iteratorCache(zone) {}

Realm::Realm(Compartment* comp, const JS::RealmOptions& options)
    : JS::shadow::Realm(comp),
      zone_(comp->zone()),
      runtime_(comp->runtimeFromMainThread()),
      creationOptions_(options.creationOptions()),
      behaviors_(options.behaviors()),
      objects_(zone_),
      randomKeyGenerator_(runtime_->forkRandomKeyGenerator()),
      debuggers_(zone_),
      allocatedDuringIncrementalGC_(zone_->isGCMarkingOrSweeping() ||
                                    zone_->isGCFinished()),
      wasm(runtime_) {
  runtime_->numRealms++;
}

Realm::~Realm() {
  MOZ_ASSERT(!hasBeenEnteredIgnoringJit());
  MOZ_ASSERT(!isDebuggee());

  // Write the code coverage information in a file.
  if (lcovRealm_) {
    runtime_->lcovOutput().writeLCovResult(*lcovRealm_);
  }

  MOZ_ASSERT(runtime_->numRealms > 0);
  runtime_->numRealms--;
}

void Realm::init(JSContext* cx, JSPrincipals* principals) {
  /*
   * As a hack, we clear our timezone cache every time we create a new realm.
   * This ensures that the cache is always relatively fresh, but shouldn't
   * interfere with benchmarks that create tons of date objects (unless they
   * also create tons of iframes, which seems unlikely).
   */
  js::ResetTimeZoneInternal(ResetTimeZoneMode::DontResetIfOffsetUnchanged);

  if (principals) {
    // Any realm with the trusted principals -- and there can be
    // multiple -- is a system realm.
    isSystem_ = (principals == cx->runtime()->trustedPrincipals());
    JS_HoldPrincipals(principals);
    principals_ = principals;
  }
}

bool JSRuntime::createJitRuntime(JSContext* cx) {
  using namespace js::jit;

  MOZ_ASSERT(!jitRuntime_);

  if (!CanLikelyAllocateMoreExecutableMemory()) {
    // Try to release memory first instead of potentially reporting OOM below.
    if (OnLargeAllocationFailure) {
      OnLargeAllocationFailure();
    }
  }

  jit::JitRuntime* jrt = cx->new_<jit::JitRuntime>();
  if (!jrt) {
    return false;
  }

  // Unfortunately, initialization depends on jitRuntime_ being non-null, so
  // we can't just wait to assign jitRuntime_.
  jitRuntime_ = jrt;

  if (!jitRuntime_->initialize(cx)) {
    js_delete(jitRuntime_.ref());
    jitRuntime_ = nullptr;
    return false;
  }

  return true;
}

bool Realm::ensureJitRealmExists(JSContext* cx) {
  using namespace js::jit;

  if (jitRealm_) {
    return true;
  }

  if (!zone()->getJitZone(cx)) {
    return false;
  }

  UniquePtr<JitRealm> jitRealm = cx->make_unique<JitRealm>();
  if (!jitRealm) {
    return false;
  }

  jitRealm->initialize(zone()->allocNurseryStrings());

  jitRealm_ = std::move(jitRealm);
  return true;
}

#ifdef JSGC_HASH_TABLE_CHECKS

void js::DtoaCache::checkCacheAfterMovingGC() {
  MOZ_ASSERT(!str || !IsForwarded(str));
}

#endif  // JSGC_HASH_TABLE_CHECKS

NonSyntacticLexicalEnvironmentObject*
ObjectRealm::getOrCreateNonSyntacticLexicalEnvironment(JSContext* cx,
                                                       HandleObject enclosing,
                                                       HandleObject key,
                                                       HandleObject thisv) {
  MOZ_ASSERT(&ObjectRealm::get(enclosing) == this);

  if (!nonSyntacticLexicalEnvironments_) {
    auto map = cx->make_unique<ObjectWeakMap>(cx);
    if (!map) {
      return nullptr;
    }

    nonSyntacticLexicalEnvironments_ = std::move(map);
  }

  RootedObject lexicalEnv(cx, nonSyntacticLexicalEnvironments_->lookup(key));

  if (!lexicalEnv) {
    MOZ_ASSERT(key->is<NonSyntacticVariablesObject>() ||
               !key->is<EnvironmentObject>());
    lexicalEnv =
        NonSyntacticLexicalEnvironmentObject::create(cx, enclosing, thisv);
    if (!lexicalEnv) {
      return nullptr;
    }
    if (!nonSyntacticLexicalEnvironments_->add(cx, key, lexicalEnv)) {
      return nullptr;
    }
  }

  return &lexicalEnv->as<NonSyntacticLexicalEnvironmentObject>();
}

NonSyntacticLexicalEnvironmentObject*
ObjectRealm::getOrCreateNonSyntacticLexicalEnvironment(JSContext* cx,
                                                       HandleObject enclosing) {
  // If a wrapped WithEnvironmentObject was passed in, unwrap it, as we may
  // be creating different WithEnvironmentObject wrappers each time.
  RootedObject key(cx, enclosing);
  if (enclosing->is<WithEnvironmentObject>()) {
    MOZ_ASSERT(!enclosing->as<WithEnvironmentObject>().isSyntactic());
    key = &enclosing->as<WithEnvironmentObject>().object();
  }

  // NOTE: The default global |this| value is set to key for compatibility
  // with existing users of the lexical environment cache.
  //  - When used by shared-global JSM loader, |this| must be the
  //    NonSyntacticVariablesObject passed as enclosing.
  //  - When used by SubscriptLoader, |this| must be the target object of
  //    the WithEnvironmentObject wrapper.
  //  - When used by XBL/DOM Events, we execute directly as a function and
  //    do not access the |this| value.
  // See js::GetFunctionThis / js::GetNonSyntacticGlobalThis
  return getOrCreateNonSyntacticLexicalEnvironment(cx, enclosing, key,
                                                   /*thisv = */ key);
}

NonSyntacticLexicalEnvironmentObject*
ObjectRealm::getNonSyntacticLexicalEnvironment(JSObject* key) const {
  MOZ_ASSERT(&ObjectRealm::get(key) == this);

  if (!nonSyntacticLexicalEnvironments_) {
    return nullptr;
  }
  // If a wrapped WithEnvironmentObject was passed in, unwrap it as in
  // getOrCreateNonSyntacticLexicalEnvironment.
  if (key->is<WithEnvironmentObject>()) {
    MOZ_ASSERT(!key->as<WithEnvironmentObject>().isSyntactic());
    key = &key->as<WithEnvironmentObject>().object();
  }
  JSObject* lexicalEnv = nonSyntacticLexicalEnvironments_->lookup(key);
  if (!lexicalEnv) {
    return nullptr;
  }
  return &lexicalEnv->as<NonSyntacticLexicalEnvironmentObject>();
}

void Realm::traceGlobalData(JSTracer* trc) {
  // Trace things reachable from the realm's global. Note that these edges
  // must be swept too in case the realm is live but the global is not.

  savedStacks_.trace(trc);

  DebugAPI::traceFromRealm(trc, this);
}

void ObjectRealm::trace(JSTracer* trc) {
  if (objectMetadataTable) {
    objectMetadataTable->trace(trc);
  }

  if (nonSyntacticLexicalEnvironments_) {
    nonSyntacticLexicalEnvironments_->trace(trc);
  }
}

void Realm::traceRoots(JSTracer* trc,
                       js::gc::GCRuntime::TraceOrMarkRuntime traceOrMark) {
  // It's not possible to trigger a GC between allocating the pending object and
  // setting its meta data in ~AutoSetNewObjectMetadata.
  MOZ_RELEASE_ASSERT(!objectPendingMetadata_);

  if (!JS::RuntimeHeapIsMinorCollecting()) {
    // The global is never nursery allocated, so we don't need to
    // trace it when doing a minor collection.
    //
    // If a realm is on-stack, we mark its global so that
    // JSContext::global() remains valid.
    if (shouldTraceGlobal() && global_) {
      TraceRoot(trc, global_.unbarrieredAddress(), "on-stack realm global");
    }
  }

  // Nothing below here needs to be treated as a root if we aren't marking
  // this zone for a collection.
  if (traceOrMark == js::gc::GCRuntime::MarkRuntime &&
      !zone()->isCollectingFromAnyThread()) {
    return;
  }

  /* Mark debug scopes, if present */
  if (debugEnvs_) {
    debugEnvs_->trace(trc);
  }

  objects_.trace(trc);
}

void ObjectRealm::finishRoots() {
  if (objectMetadataTable) {
    objectMetadataTable->clear();
  }

  if (nonSyntacticLexicalEnvironments_) {
    nonSyntacticLexicalEnvironments_->clear();
  }
}

void Realm::finishRoots() {
  if (debugEnvs_) {
    debugEnvs_->finish();
  }

  objects_.finishRoots();
}

void ObjectRealm::sweepAfterMinorGC(JSTracer* trc) {
  InnerViewTable& table = innerViews.get();
  if (table.needsSweepAfterMinorGC()) {
    table.sweepAfterMinorGC(trc);
  }
}

void Realm::sweepAfterMinorGC(JSTracer* trc) {
  globalWriteBarriered = 0;
  dtoaCache.purge();
  objects_.sweepAfterMinorGC(trc);
}

void Realm::traceWeakSavedStacks(JSTracer* trc) { savedStacks_.traceWeak(trc); }

void Realm::traceWeakGlobalEdge(JSTracer* trc) {
  // If the global is dead, free its GlobalObjectData.
  auto result = TraceWeakEdge(trc, &global_, "Realm::global_");
  if (result.isDead()) {
    result.initialTarget()->releaseData(runtime_->gcContext());
  }
}

void Realm::traceWeakEdgesInJitRealm(JSTracer* trc) {
  if (jitRealm_) {
    jitRealm_->traceWeak(trc, this);
  }
}

void Realm::traceWeakRegExps(JSTracer* trc) {
  /*
   * JIT code increments activeWarmUpCounter for any RegExpShared used by jit
   * code for the lifetime of the JIT script. Thus, we must perform
   * sweeping after clearing jit code.
   */
  regExps.traceWeak(trc);
}

void Realm::traceWeakDebugEnvironmentEdges(JSTracer* trc) {
  if (debugEnvs_) {
    debugEnvs_->traceWeak(trc);
  }
}

void Realm::fixupAfterMovingGC(JSTracer* trc) {
  purge();
  traceWeakGlobalEdge(trc);
}

void Realm::purge() {
  dtoaCache.purge();
  newProxyCache.purge();
  newPlainObjectWithPropsCache.purge();
  objects_.iteratorCache.clearAndCompact();
  arraySpeciesLookup.purge();
  promiseLookup.purge();

  if (zone()->isGCPreparing()) {
    purgeForOfPicChain();
  }
}

void Realm::purgeForOfPicChain() {
  if (GlobalObject* global = global_.unbarrieredGet()) {
    if (NativeObject* object = global->getForOfPICObject()) {
      ForOfPIC::Chain* chain = ForOfPIC::fromJSObject(object);
      chain->freeAllStubs(runtime_->gcContext());
    }
  }
}

// Check to see if this individual realm is recording allocations. Debuggers or
// runtimes can try and record allocations, so this method can check to see if
// any initialization is needed.
bool Realm::isRecordingAllocations() { return !!allocationMetadataBuilder_; }

void Realm::setAllocationMetadataBuilder(
    const js::AllocationMetadataBuilder* builder) {
  // Clear any jitcode in the runtime, which behaves differently depending on
  // whether there is a creation callback.
  ReleaseAllJITCode(runtime_->gcContext());

  allocationMetadataBuilder_ = builder;
}

void Realm::forgetAllocationMetadataBuilder() {
  // Unlike setAllocationMetadataBuilder, we don't have to discard all JIT
  // code here (code is still valid, just a bit slower because it doesn't do
  // inline GC allocations when a metadata builder is present), but we do want
  // to cancel off-thread Ion compilations to avoid races when Ion calls
  // hasAllocationMetadataBuilder off-thread.
  CancelOffThreadIonCompile(this);

  allocationMetadataBuilder_ = nullptr;
}

void Realm::setNewObjectMetadata(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(obj->maybeCCWRealm() == this);
  cx->check(compartment(), obj);

  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (JSObject* metadata =
          allocationMetadataBuilder_->build(cx, obj, oomUnsafe)) {
    MOZ_ASSERT(metadata->maybeCCWRealm() == obj->maybeCCWRealm());
    cx->check(metadata);

    if (!objects_.objectMetadataTable) {
      auto table = cx->make_unique<ObjectWeakMap>(cx);
      if (!table) {
        oomUnsafe.crash("setNewObjectMetadata");
      }

      objects_.objectMetadataTable = std::move(table);
    }

    if (!objects_.objectMetadataTable->add(cx, obj, metadata)) {
      oomUnsafe.crash("setNewObjectMetadata");
    }
  }
}

void Realm::updateDebuggerObservesFlag(unsigned flag) {
  MOZ_ASSERT(isDebuggee());
  MOZ_ASSERT(flag == DebuggerObservesAllExecution ||
             flag == DebuggerObservesCoverage ||
             flag == DebuggerObservesAsmJS || flag == DebuggerObservesWasm);

  GlobalObject* global =
      zone()->runtimeFromMainThread()->gc.isForegroundSweeping()
          ? unsafeUnbarrieredMaybeGlobal()
          : maybeGlobal();
  bool observes = false;
  if (flag == DebuggerObservesAllExecution) {
    observes = DebugAPI::debuggerObservesAllExecution(global);
  } else if (flag == DebuggerObservesCoverage) {
    observes = DebugAPI::debuggerObservesCoverage(global);
  } else if (flag == DebuggerObservesAsmJS) {
    observes = DebugAPI::debuggerObservesAsmJS(global);
  } else if (flag == DebuggerObservesWasm) {
    observes = DebugAPI::debuggerObservesWasm(global);
  }

  if (observes) {
    debugModeBits_ |= flag;
  } else {
    debugModeBits_ &= ~flag;
  }
}

void Realm::setIsDebuggee() {
  if (!isDebuggee()) {
    debugModeBits_ |= IsDebuggee;
    runtimeFromMainThread()->incrementNumDebuggeeRealms();
  }
}

void Realm::unsetIsDebuggee() {
  if (isDebuggee()) {
    if (debuggerObservesCoverage()) {
      runtime_->decrementNumDebuggeeRealmsObservingCoverage();
    }
    debugModeBits_ = 0;
    DebugEnvironments::onRealmUnsetIsDebuggee(this);
    runtimeFromMainThread()->decrementNumDebuggeeRealms();
  }
}

void Realm::updateDebuggerObservesCoverage() {
  bool previousState = debuggerObservesCoverage();
  updateDebuggerObservesFlag(DebuggerObservesCoverage);
  if (previousState == debuggerObservesCoverage()) {
    return;
  }

  if (debuggerObservesCoverage()) {
    // Interrupt any running interpreter frame. The scriptCounts are
    // allocated on demand when a script resumes its execution.
    JSContext* cx = TlsContext.get();
    for (ActivationIterator iter(cx); !iter.done(); ++iter) {
      if (iter->isInterpreter()) {
        iter->asInterpreter()->enableInterruptsUnconditionally();
      }
    }
    runtime_->incrementNumDebuggeeRealmsObservingCoverage();
    return;
  }

  runtime_->decrementNumDebuggeeRealmsObservingCoverage();

  // If code coverage is enabled by any other means, keep it.
  if (collectCoverageForDebug()) {
    return;
  }

  clearScriptCounts();
  clearScriptLCov();
}

coverage::LCovRealm* Realm::lcovRealm() {
  if (!lcovRealm_) {
    lcovRealm_ = js::MakeUnique<coverage::LCovRealm>(this);
  }
  return lcovRealm_.get();
}

bool Realm::collectCoverageForDebug() const {
  return debuggerObservesCoverage() || coverage::IsLCovEnabled();
}

void Realm::clearScriptCounts() { zone()->clearScriptCounts(this); }

void Realm::clearScriptLCov() { zone()->clearScriptLCov(this); }

void ObjectRealm::addSizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf, size_t* innerViewsArg,
    size_t* objectMetadataTablesArg,
    size_t* nonSyntacticLexicalEnvironmentsArg) {
  *innerViewsArg += innerViews.sizeOfExcludingThis(mallocSizeOf);

  if (objectMetadataTable) {
    *objectMetadataTablesArg +=
        objectMetadataTable->sizeOfIncludingThis(mallocSizeOf);
  }

  if (auto& map = nonSyntacticLexicalEnvironments_) {
    *nonSyntacticLexicalEnvironmentsArg +=
        map->sizeOfIncludingThis(mallocSizeOf);
  }
}

void Realm::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                   size_t* realmObject, size_t* realmTables,
                                   size_t* innerViewsArg,
                                   size_t* objectMetadataTablesArg,
                                   size_t* savedStacksSet,
                                   size_t* nonSyntacticLexicalEnvironmentsArg,
                                   size_t* jitRealm) {
  *realmObject += mallocSizeOf(this);
  wasm.addSizeOfExcludingThis(mallocSizeOf, realmTables);

  objects_.addSizeOfExcludingThis(mallocSizeOf, innerViewsArg,
                                  objectMetadataTablesArg,
                                  nonSyntacticLexicalEnvironmentsArg);

  *savedStacksSet += savedStacks_.sizeOfExcludingThis(mallocSizeOf);

  if (jitRealm_) {
    *jitRealm += jitRealm_->sizeOfIncludingThis(mallocSizeOf);
  }
}

bool Realm::shouldCaptureStackForThrow() {
  // Determine whether a stack trace should be captured for throw-statements (or
  // similar) in JS code in this realm. We don't want to do this unconditionally
  // because capturing stacks is slow and some scripts throw a lot of
  // exceptions.
  //
  // Note: this is unrelated to Error.stack! That property is observable from
  // JS code so we can't use these heuristics there. The code here is mostly
  // relevant for uncaught exceptions that are not Error objects.

  // To match other browsers, we always capture a stack trace if the realm is a
  // debuggee (this includes the devtools console being open) or if unlimited
  // stack traces have been enabled for this realm (used in automation).
  if (isDebuggee() || isUnlimitedStacksCapturingEnabled) {
    return true;
  }

  // Also always capture for chrome code. This is code we control and this helps
  // debugging.
  if (principals() &&
      principals() == runtimeFromMainThread()->trustedPrincipals()) {
    return true;
  }

  // Else, capture the stack only for the first N exceptions so that we can
  // still show stack traces for scripts that don't throw a lot of exceptions
  // (if the console is opened later).
  static constexpr uint16_t MaxStacksCapturedForThrow = 50;
  if (numStacksCapturedForThrow_ > MaxStacksCapturedForThrow) {
    return false;
  }
  numStacksCapturedForThrow_++;
  return true;
}

mozilla::HashCodeScrambler Realm::randomHashCodeScrambler() {
  return mozilla::HashCodeScrambler(randomKeyGenerator_.next(),
                                    randomKeyGenerator_.next());
}

void AutoSetNewObjectMetadata::setPendingMetadata() {
  JSObject* obj = cx_->realm()->getAndClearObjectPendingMetadata();
  if (!obj) {
    return;
  }

  MOZ_ASSERT(obj->getClass()->shouldDelayMetadataBuilder());

  if (cx_->isExceptionPending()) {
    return;
  }

  // This function is called from a destructor that often runs upon exit from
  // a function that is returning an unrooted pointer to a Cell. The
  // allocation metadata callback often allocates; if it causes a GC, then the
  // Cell pointer being returned won't be traced or relocated.
  //
  // The only extant callbacks are those internal to SpiderMonkey that
  // capture the JS stack. In fact, we're considering removing general
  // callbacks altogther in bug 1236748. Since it's not running arbitrary
  // code, it's adequate to simply suppress GC while we run the callback.
  gc::AutoSuppressGC autoSuppressGC(cx_);

  (void)SetNewObjectMetadata(cx_, obj);
}

JS_PUBLIC_API void gc::TraceRealm(JSTracer* trc, JS::Realm* realm,
                                  const char* name) {
  // The way GC works with compartments is basically incomprehensible.
  // For Realms, what we want is very simple: each Realm has a strong
  // reference to its GlobalObject, and vice versa.
  //
  // Here we simply trace our side of that edge. During GC,
  // GCRuntime::traceRuntimeCommon() marks all other realm roots, for
  // all realms.
  realm->traceGlobalData(trc);
}

JS_PUBLIC_API JS::Realm* JS::GetCurrentRealmOrNull(JSContext* cx) {
  return cx->realm();
}

JS_PUBLIC_API JS::Realm* JS::GetObjectRealmOrNull(JSObject* obj) {
  return IsCrossCompartmentWrapper(obj) ? nullptr : obj->nonCCWRealm();
}

JS_PUBLIC_API void* JS::GetRealmPrivate(JS::Realm* realm) {
  return realm->realmPrivate();
}

JS_PUBLIC_API void JS::SetRealmPrivate(JS::Realm* realm, void* data) {
  realm->setRealmPrivate(data);
}

JS_PUBLIC_API void JS::SetDestroyRealmCallback(
    JSContext* cx, JS::DestroyRealmCallback callback) {
  cx->runtime()->destroyRealmCallback = callback;
}

JS_PUBLIC_API void JS::SetRealmNameCallback(JSContext* cx,
                                            JS::RealmNameCallback callback) {
  cx->runtime()->realmNameCallback = callback;
}

JS_PUBLIC_API JSObject* JS::GetRealmGlobalOrNull(JS::Realm* realm) {
  return realm->maybeGlobal();
}

JS_PUBLIC_API bool JS::InitRealmStandardClasses(JSContext* cx) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return GlobalObject::initStandardClasses(cx, cx->global());
}

JS_PUBLIC_API bool JS::MaybeFreezeCtorAndPrototype(JSContext* cx,
                                                   HandleObject ctor,
                                                   HandleObject maybeProto) {
  if (MOZ_LIKELY(!cx->realm()->creationOptions().freezeBuiltins())) {
    return true;
  }
  if (!SetIntegrityLevel(cx, ctor, IntegrityLevel::Frozen)) {
    return false;
  }
  if (maybeProto) {
    if (!SetIntegrityLevel(cx, maybeProto, IntegrityLevel::Sealed)) {
      return false;
    }
  }
  return true;
}

JS_PUBLIC_API JSObject* JS::GetRealmObjectPrototype(JSContext* cx) {
  CHECK_THREAD(cx);
  return &cx->global()->getObjectPrototype();
}

JS_PUBLIC_API JS::Handle<JSObject*> JS::GetRealmObjectPrototypeHandle(
    JSContext* cx) {
  return cx->global()->getObjectPrototypeHandle();
}

JS_PUBLIC_API JSObject* JS::GetRealmFunctionPrototype(JSContext* cx) {
  CHECK_THREAD(cx);
  return &cx->global()->getFunctionPrototype();
}

JS_PUBLIC_API JSObject* JS::GetRealmArrayPrototype(JSContext* cx) {
  CHECK_THREAD(cx);
  return GlobalObject::getOrCreateArrayPrototype(cx, cx->global());
}

JS_PUBLIC_API JSObject* JS::GetRealmErrorPrototype(JSContext* cx) {
  CHECK_THREAD(cx);
  return GlobalObject::getOrCreateCustomErrorPrototype(cx, cx->global(),
                                                       JSEXN_ERR);
}

JS_PUBLIC_API JSObject* JS::GetRealmIteratorPrototype(JSContext* cx) {
  CHECK_THREAD(cx);
  return GlobalObject::getOrCreateIteratorPrototype(cx, cx->global());
}

JS_PUBLIC_API JSObject* JS::GetRealmAsyncIteratorPrototype(JSContext* cx) {
  CHECK_THREAD(cx);
  return GlobalObject::getOrCreateAsyncIteratorPrototype(cx, cx->global());
}

JS_PUBLIC_API JSObject* JS::GetRealmKeyObject(JSContext* cx) {
  return GlobalObject::getOrCreateRealmKeyObject(cx, cx->global());
}

JS_PUBLIC_API Realm* JS::GetFunctionRealm(JSContext* cx, HandleObject objArg) {
  // https://tc39.github.io/ecma262/#sec-getfunctionrealm
  // 7.3.22 GetFunctionRealm ( obj )

  CHECK_THREAD(cx);
  cx->check(objArg);

  RootedObject obj(cx, objArg);
  while (true) {
    obj = CheckedUnwrapStatic(obj);
    if (!obj) {
      ReportAccessDenied(cx);
      return nullptr;
    }

    // Step 1.
    MOZ_ASSERT(IsCallable(obj));

    // Steps 2 and 3. We use a loop instead of recursion to unwrap bound
    // functions.
    if (obj->is<JSFunction>()) {
      return obj->as<JSFunction>().realm();
    }
    if (obj->is<BoundFunctionObject>()) {
      obj = obj->as<BoundFunctionObject>().getTarget();
      continue;
    }

    // WrappedFunctionObjects also have a [[Realm]] internal slot,
    // which is the nonCCWRealm by construction.
    if (obj->is<WrappedFunctionObject>()) {
      return obj->nonCCWRealm();
    }

    // Step 4.
    if (IsScriptedProxy(obj)) {
      // Steps 4.a-b.
      JSObject* proxyTarget = GetProxyTargetObject(obj);
      if (!proxyTarget) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_PROXY_REVOKED);
        return nullptr;
      }

      // Step 4.c.
      obj = proxyTarget;
      continue;
    }

    // Step 5.
    return cx->realm();
  }
}
