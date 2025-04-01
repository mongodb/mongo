/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * API functions and methods used by the rest of SpiderMonkey and by embeddings.
 */

#include "mozilla/TimeStamp.h"

#include "jsapi.h"
#include "jsfriendapi.h"

#include "gc/GC.h"
#include "gc/PublicIterators.h"
#include "jit/JitZone.h"
#include "js/HeapAPI.h"
#include "js/Value.h"
#include "util/DifferentialTesting.h"
#include "vm/HelperThreads.h"
#include "vm/Realm.h"
#include "vm/Scope.h"

#include "gc/Marking-inl.h"
#include "gc/StableCellHasher-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::TimeStamp;

extern JS_PUBLIC_API bool js::AddRawValueRoot(JSContext* cx, Value* vp,
                                              const char* name) {
  MOZ_ASSERT(vp);
  MOZ_ASSERT(name);
  bool ok = cx->runtime()->gc.addRoot(vp, name);
  if (!ok) {
    JS_ReportOutOfMemory(cx);
  }
  return ok;
}

extern JS_PUBLIC_API void js::RemoveRawValueRoot(JSContext* cx, Value* vp) {
  cx->runtime()->gc.removeRoot(vp);
}

JS_PUBLIC_API JS::HeapState JS::RuntimeHeapState() {
  return TlsContext.get()->runtime()->gc.heapState();
}

JS::AutoDisableGenerationalGC::AutoDisableGenerationalGC(JSContext* cx)
    : cx(cx) {
  if (!cx->generationalDisabled) {
    cx->runtime()->gc.evictNursery(JS::GCReason::DISABLE_GENERATIONAL_GC);
    cx->nursery().disable();
  }
  ++cx->generationalDisabled;
  MOZ_ASSERT(cx->nursery().isEmpty());
}

JS::AutoDisableGenerationalGC::~AutoDisableGenerationalGC() {
  if (--cx->generationalDisabled == 0 &&
      cx->runtime()->gc.tunables.gcMaxNurseryBytes() > 0) {
    cx->nursery().enable();
  }
}

JS_PUBLIC_API bool JS::IsGenerationalGCEnabled(JSRuntime* rt) {
  return !rt->mainContextFromOwnThread()->generationalDisabled;
}

AutoDisableCompactingGC::AutoDisableCompactingGC(JSContext* cx) : cx(cx) {
  ++cx->compactingDisabledCount;
  if (cx->runtime()->gc.isIncrementalGCInProgress() &&
      cx->runtime()->gc.isCompactingGc()) {
    FinishGC(cx);
  }
}

AutoDisableCompactingGC::~AutoDisableCompactingGC() {
  MOZ_ASSERT(cx->compactingDisabledCount > 0);
  --cx->compactingDisabledCount;
}

#ifdef DEBUG

/* Should only be called manually under gdb */
void PreventGCDuringInteractiveDebug() { TlsContext.get()->suppressGC++; }

#endif

void js::ReleaseAllJITCode(JS::GCContext* gcx) {
  js::CancelOffThreadIonCompile(gcx->runtime());

  for (ZonesIter zone(gcx->runtime(), SkipAtoms); !zone.done(); zone.next()) {
    zone->forceDiscardJitCode(gcx);
    if (jit::JitZone* jitZone = zone->jitZone()) {
      jitZone->discardStubs();
    }
  }
}

AutoSuppressGC::AutoSuppressGC(JSContext* cx)
    : suppressGC_(cx->suppressGC.ref()) {
  suppressGC_++;
}

#ifdef DEBUG
AutoDisableProxyCheck::AutoDisableProxyCheck() {
  TlsContext.get()->disableStrictProxyChecking();
}

AutoDisableProxyCheck::~AutoDisableProxyCheck() {
  TlsContext.get()->enableStrictProxyChecking();
}

JS_PUBLIC_API void JS::AssertGCThingMustBeTenured(JSObject* obj) {
  MOZ_ASSERT(obj->isTenured());
  MOZ_ASSERT(obj->getClass()->hasFinalize() &&
             !(obj->getClass()->flags & JSCLASS_SKIP_NURSERY_FINALIZE));
}

JS_PUBLIC_API void JS::AssertGCThingIsNotNurseryAllocable(Cell* cell) {
  MOZ_ASSERT(cell);
  MOZ_ASSERT(!cell->is<JSObject>() && !cell->is<JSString>() &&
             !cell->is<JS::BigInt>());
}

JS_PUBLIC_API void js::gc::AssertGCThingHasType(js::gc::Cell* cell,
                                                JS::TraceKind kind) {
  if (!cell) {
    MOZ_ASSERT(kind == JS::TraceKind::Null);
    return;
  }

  MOZ_ASSERT(IsCellPointerValid(cell));
  MOZ_ASSERT(cell->getTraceKind() == kind);
}
#endif

#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED

JS::AutoAssertNoGC::AutoAssertNoGC(JSContext* maybecx) {
  if (maybecx) {
    cx_ = maybecx;
  } else if (TlsContext.initialized()) {
    cx_ = TlsContext.get();
  } else {
    cx_ = nullptr;
  }
  if (cx_) {
    cx_->inUnsafeRegion++;
  }
}

JS::AutoAssertNoGC::~AutoAssertNoGC() { reset(); }

void JS::AutoAssertNoGC::reset() {
  if (cx_) {
    MOZ_ASSERT(cx_->inUnsafeRegion > 0);
    cx_->inUnsafeRegion--;
    cx_ = nullptr;
  }
}

#endif  // MOZ_DIAGNOSTIC_ASSERT_ENABLED

#ifdef DEBUG

JS::AutoEnterCycleCollection::AutoEnterCycleCollection(JSRuntime* rt)
    : runtime_(rt) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(rt));
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());
  runtime_->gc.heapState_ = HeapState::CycleCollecting;
}

JS::AutoEnterCycleCollection::~AutoEnterCycleCollection() {
  MOZ_ASSERT(JS::RuntimeHeapIsCycleCollecting());
  runtime_->gc.heapState_ = HeapState::Idle;
}

JS::AutoAssertGCCallback::AutoAssertGCCallback() : AutoSuppressGCAnalysis() {
  MOZ_ASSERT(JS::RuntimeHeapIsCollecting());
}

#endif  // DEBUG

JS_PUBLIC_API const char* JS::GCTraceKindToAscii(JS::TraceKind kind) {
  switch (kind) {
#define MAP_NAME(name, _0, _1, _2) \
  case JS::TraceKind::name:        \
    return "JS " #name;
    JS_FOR_EACH_TRACEKIND(MAP_NAME);
#undef MAP_NAME
    default:
      return "Invalid";
  }
}

JS_PUBLIC_API size_t JS::GCTraceKindSize(JS::TraceKind kind) {
  switch (kind) {
#define MAP_SIZE(name, type, _0, _1) \
  case JS::TraceKind::name:          \
    return sizeof(type);
    JS_FOR_EACH_TRACEKIND(MAP_SIZE);
#undef MAP_SIZE
    default:
      return 0;
  }
}

JS::GCCellPtr::GCCellPtr(const Value& v)
    : GCCellPtr(v.toGCThing(), v.traceKind()) {}

JS::TraceKind JS::GCCellPtr::outOfLineKind() const {
  MOZ_ASSERT((ptr & OutOfLineTraceKindMask) == OutOfLineTraceKindMask);
  MOZ_ASSERT(asCell()->isTenured());
  return MapAllocToTraceKind(asCell()->asTenured().getAllocKind());
}

JS_PUBLIC_API void JS::PrepareZoneForGC(JSContext* cx, Zone* zone) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  // If we got the zone from a shared atom, we may have the wrong atoms zone
  // here.
  if (zone->isAtomsZone()) {
    zone = cx->runtime()->atomsZone();
  }

  MOZ_ASSERT(cx->runtime()->gc.hasZone(zone));
  zone->scheduleGC();
}

JS_PUBLIC_API void JS::PrepareForFullGC(JSContext* cx) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  cx->runtime()->gc.fullGCRequested = true;
  for (ZonesIter zone(cx->runtime(), WithAtoms); !zone.done(); zone.next()) {
    zone->scheduleGC();
  }
}

JS_PUBLIC_API void JS::PrepareForIncrementalGC(JSContext* cx) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  if (!JS::IsIncrementalGCInProgress(cx)) {
    return;
  }

  for (ZonesIter zone(cx->runtime(), WithAtoms); !zone.done(); zone.next()) {
    if (zone->wasGCStarted()) {
      zone->scheduleGC();
    }
  }
}

JS_PUBLIC_API bool JS::IsGCScheduled(JSContext* cx) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  for (ZonesIter zone(cx->runtime(), WithAtoms); !zone.done(); zone.next()) {
    if (zone->isGCScheduled()) {
      return true;
    }
  }

  return false;
}

JS_PUBLIC_API void JS::SkipZoneForGC(JSContext* cx, Zone* zone) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_ASSERT(cx->runtime()->gc.hasZone(zone));

  cx->runtime()->gc.fullGCRequested = false;
  zone->unscheduleGC();
}

static inline void CheckGCOptions(JS::GCOptions options) {
  MOZ_ASSERT(options == JS::GCOptions::Normal ||
             options == JS::GCOptions::Shrink ||
             options == JS::GCOptions::Shutdown);
}

JS_PUBLIC_API void JS::NonIncrementalGC(JSContext* cx, JS::GCOptions options,
                                        GCReason reason) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  CheckGCOptions(options);

  cx->runtime()->gc.gc(options, reason);

  MOZ_ASSERT(!IsIncrementalGCInProgress(cx));
}

JS_PUBLIC_API void JS::StartIncrementalGC(JSContext* cx, JS::GCOptions options,
                                          GCReason reason,
                                          const js::SliceBudget& budget) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  CheckGCOptions(options);

  cx->runtime()->gc.startGC(options, reason, budget);
}

JS_PUBLIC_API void JS::IncrementalGCSlice(JSContext* cx, GCReason reason,
                                          const js::SliceBudget& budget) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  cx->runtime()->gc.gcSlice(reason, budget);
}

JS_PUBLIC_API bool JS::IncrementalGCHasForegroundWork(JSContext* cx) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return cx->runtime()->gc.hasForegroundWork();
}

JS_PUBLIC_API void JS::FinishIncrementalGC(JSContext* cx, GCReason reason) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  cx->runtime()->gc.finishGC(reason);
}

JS_PUBLIC_API void JS::AbortIncrementalGC(JSContext* cx) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  if (IsIncrementalGCInProgress(cx)) {
    cx->runtime()->gc.abortGC();
  }
}

char16_t* JS::GCDescription::formatSliceMessage(JSContext* cx) const {
  UniqueChars cstr = cx->runtime()->gc.stats().formatCompactSliceMessage();

  size_t nchars = strlen(cstr.get());
  UniqueTwoByteChars out(js_pod_malloc<char16_t>(nchars + 1));
  if (!out) {
    return nullptr;
  }
  out.get()[nchars] = 0;

  CopyAndInflateChars(out.get(), cstr.get(), nchars);
  return out.release();
}

char16_t* JS::GCDescription::formatSummaryMessage(JSContext* cx) const {
  UniqueChars cstr = cx->runtime()->gc.stats().formatCompactSummaryMessage();

  size_t nchars = strlen(cstr.get());
  UniqueTwoByteChars out(js_pod_malloc<char16_t>(nchars + 1));
  if (!out) {
    return nullptr;
  }
  out.get()[nchars] = 0;

  CopyAndInflateChars(out.get(), cstr.get(), nchars);
  return out.release();
}

JS::dbg::GarbageCollectionEvent::Ptr JS::GCDescription::toGCEvent(
    JSContext* cx) const {
  return JS::dbg::GarbageCollectionEvent::Create(
      cx->runtime(), cx->runtime()->gc.stats(),
      cx->runtime()->gc.majorGCCount());
}

TimeStamp JS::GCDescription::startTime(JSContext* cx) const {
  return cx->runtime()->gc.stats().start();
}

TimeStamp JS::GCDescription::endTime(JSContext* cx) const {
  return cx->runtime()->gc.stats().end();
}

TimeStamp JS::GCDescription::lastSliceStart(JSContext* cx) const {
  return cx->runtime()->gc.stats().slices().back().start;
}

TimeStamp JS::GCDescription::lastSliceEnd(JSContext* cx) const {
  return cx->runtime()->gc.stats().slices().back().end;
}

JS::UniqueChars JS::GCDescription::sliceToJSONProfiler(JSContext* cx) const {
  size_t slices = cx->runtime()->gc.stats().slices().length();
  MOZ_ASSERT(slices > 0);
  return cx->runtime()->gc.stats().renderJsonSlice(slices - 1);
}

JS::UniqueChars JS::GCDescription::formatJSONProfiler(JSContext* cx) const {
  return cx->runtime()->gc.stats().renderJsonMessage();
}

JS_PUBLIC_API JS::UniqueChars JS::MinorGcToJSON(JSContext* cx) {
  JSRuntime* rt = cx->runtime();
  return rt->gc.stats().renderNurseryJson();
}

JS_PUBLIC_API JS::GCSliceCallback JS::SetGCSliceCallback(
    JSContext* cx, GCSliceCallback callback) {
  return cx->runtime()->gc.setSliceCallback(callback);
}

JS_PUBLIC_API JS::DoCycleCollectionCallback JS::SetDoCycleCollectionCallback(
    JSContext* cx, JS::DoCycleCollectionCallback callback) {
  return cx->runtime()->gc.setDoCycleCollectionCallback(callback);
}

JS_PUBLIC_API bool JS::AddGCNurseryCollectionCallback(
    JSContext* cx, GCNurseryCollectionCallback callback, void* data) {
  return cx->runtime()->gc.addNurseryCollectionCallback(callback, data);
}

JS_PUBLIC_API void JS::RemoveGCNurseryCollectionCallback(
    JSContext* cx, GCNurseryCollectionCallback callback, void* data) {
  return cx->runtime()->gc.removeNurseryCollectionCallback(callback, data);
}

JS_PUBLIC_API void JS::SetLowMemoryState(JSContext* cx, bool newState) {
  return cx->runtime()->gc.setLowMemoryState(newState);
}

JS_PUBLIC_API void JS::DisableIncrementalGC(JSContext* cx) {
  cx->runtime()->gc.disallowIncrementalGC();
}

JS_PUBLIC_API bool JS::IsIncrementalGCEnabled(JSContext* cx) {
  GCRuntime& gc = cx->runtime()->gc;
  return gc.isIncrementalGCEnabled() && gc.isIncrementalGCAllowed();
}

JS_PUBLIC_API bool JS::IsIncrementalGCInProgress(JSContext* cx) {
  return cx->runtime()->gc.isIncrementalGCInProgress();
}

JS_PUBLIC_API bool JS::IsIncrementalGCInProgress(JSRuntime* rt) {
  return rt->gc.isIncrementalGCInProgress() &&
         !rt->gc.isVerifyPreBarriersEnabled();
}

JS_PUBLIC_API bool JS::IsIncrementalBarrierNeeded(JSContext* cx) {
  if (JS::RuntimeHeapIsBusy()) {
    return false;
  }

  auto state = cx->runtime()->gc.state();
  return state != gc::State::NotActive && state <= gc::State::Sweep;
}

JS_PUBLIC_API void JS::IncrementalPreWriteBarrier(JSObject* obj) {
  if (!obj) {
    return;
  }

  AutoGeckoProfilerEntry profilingStackFrame(
      TlsContext.get(), "IncrementalPreWriteBarrier(JSObject*)",
      JS::ProfilingCategoryPair::GCCC_Barrier);
  PreWriteBarrier(obj);
}

JS_PUBLIC_API void JS::IncrementalPreWriteBarrier(GCCellPtr thing) {
  if (!thing) {
    return;
  }

  AutoGeckoProfilerEntry profilingStackFrame(
      TlsContext.get(), "IncrementalPreWriteBarrier(GCCellPtr)",
      JS::ProfilingCategoryPair::GCCC_Barrier);
  CellPtrPreWriteBarrier(thing);
}

JS_PUBLIC_API bool JS::WasIncrementalGC(JSRuntime* rt) {
  return rt->gc.isIncrementalGc();
}

bool js::gc::CreateUniqueIdForNativeObject(NativeObject* nobj, uint64_t* uidp) {
  JSRuntime* runtime = nobj->runtimeFromMainThread();
  *uidp = NextCellUniqueId(runtime);
  return nobj->setUniqueId(runtime, *uidp);
}

bool js::gc::CreateUniqueIdForNonNativeObject(Cell* cell,
                                              UniqueIdMap::AddPtr ptr,
                                              uint64_t* uidp) {
  // If the cell is in the nursery, hopefully unlikely, then we need to tell the
  // nursery about it so that it can sweep the uid if the thing does not get
  // tenured.
  JSRuntime* runtime = cell->runtimeFromMainThread();
  if (IsInsideNursery(cell) &&
      !runtime->gc.nursery().addedUniqueIdToCell(cell)) {
    return false;
  }

  // Set a new uid on the cell.
  *uidp = NextCellUniqueId(runtime);
  return cell->zone()->uniqueIds().add(ptr, cell, *uidp);
}

uint64_t js::gc::NextCellUniqueId(JSRuntime* rt) {
  return rt->gc.nextCellUniqueId();
}

namespace js {

static const struct GCParamInfo {
  const char* name;
  JSGCParamKey key;
  bool writable;
} GCParameters[] = {
#define DEFINE_PARAM_INFO(name, key, writable) {name, key, writable},
    FOR_EACH_GC_PARAM(DEFINE_PARAM_INFO)
#undef DEFINE_PARAM_INFO
};

bool GetGCParameterInfo(const char* name, JSGCParamKey* keyOut,
                        bool* writableOut) {
  MOZ_ASSERT(keyOut);
  MOZ_ASSERT(writableOut);

  for (const GCParamInfo& info : GCParameters) {
    if (strcmp(name, info.name) == 0) {
      *keyOut = info.key;
      *writableOut = info.writable;
      return true;
    }
  }

  return false;
}

namespace gc {
namespace MemInfo {

static bool GCBytesGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->runtime()->gc.heapSize.bytes()));
  return true;
}

static bool MallocBytesGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  size_t bytes = 0;
  for (ZonesIter zone(cx->runtime(), WithAtoms); !zone.done(); zone.next()) {
    bytes += zone->mallocHeapSize.bytes();
  }
  args.rval().setNumber(bytes);
  return true;
}

static bool GCMaxBytesGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->runtime()->gc.tunables.gcMaxBytes()));
  return true;
}

static bool GCHighFreqGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(
      cx->runtime()->gc.schedulingState.inHighFrequencyGCMode());
  return true;
}

static bool GCNumberGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->runtime()->gc.gcNumber()));
  return true;
}

static bool MajorGCCountGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->runtime()->gc.majorGCCount()));
  return true;
}

static bool MinorGCCountGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->runtime()->gc.minorGCCount()));
  return true;
}

static bool GCSliceCountGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->runtime()->gc.gcSliceCount()));
  return true;
}

static bool GCCompartmentCount(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  size_t count = 0;
  for (ZonesIter zone(cx->runtime(), WithAtoms); !zone.done(); zone.next()) {
    count += zone->compartments().length();
  }

  args.rval().setNumber(double(count));
  return true;
}

static bool GCLastStartReason(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  const char* reason = ExplainGCReason(cx->runtime()->gc.lastStartReason());
  RootedString str(cx, JS_NewStringCopyZ(cx, reason));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool ZoneGCBytesGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->zone()->gcHeapSize.bytes()));
  return true;
}

static bool ZoneGCTriggerBytesGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->zone()->gcHeapThreshold.startBytes()));
  return true;
}

static bool ZoneGCAllocTriggerGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  bool highFrequency =
      cx->runtime()->gc.schedulingState.inHighFrequencyGCMode();
  args.rval().setNumber(
      double(cx->zone()->gcHeapThreshold.eagerAllocTrigger(highFrequency)));
  return true;
}

static bool ZoneMallocBytesGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->zone()->mallocHeapSize.bytes()));
  return true;
}

static bool ZoneMallocTriggerBytesGetter(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->zone()->mallocHeapThreshold.startBytes()));
  return true;
}

static bool ZoneGCNumberGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(double(cx->runtime()->gc.gcNumber()));
  return true;
}

#ifdef DEBUG
static bool DummyGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setUndefined();
  return true;
}
#endif

} /* namespace MemInfo */

JSObject* NewMemoryInfoObject(JSContext* cx) {
  RootedObject obj(cx, JS_NewObject(cx, nullptr));
  if (!obj) {
    return nullptr;
  }

  using namespace MemInfo;
  struct NamedGetter {
    const char* name;
    JSNative getter;
  } getters[] = {{"gcBytes", GCBytesGetter},
                 {"gcMaxBytes", GCMaxBytesGetter},
                 {"mallocBytes", MallocBytesGetter},
                 {"gcIsHighFrequencyMode", GCHighFreqGetter},
                 {"gcNumber", GCNumberGetter},
                 {"majorGCCount", MajorGCCountGetter},
                 {"minorGCCount", MinorGCCountGetter},
                 {"sliceCount", GCSliceCountGetter},
                 {"compartmentCount", GCCompartmentCount},
                 {"lastStartReason", GCLastStartReason}};

  for (auto pair : getters) {
    JSNative getter = pair.getter;

#ifdef DEBUG
    if (js::SupportDifferentialTesting()) {
      getter = DummyGetter;
    }
#endif

    if (!JS_DefineProperty(cx, obj, pair.name, getter, nullptr,
                           JSPROP_ENUMERATE)) {
      return nullptr;
    }
  }

  RootedObject zoneObj(cx, JS_NewObject(cx, nullptr));
  if (!zoneObj) {
    return nullptr;
  }

  if (!JS_DefineProperty(cx, obj, "zone", zoneObj, JSPROP_ENUMERATE)) {
    return nullptr;
  }

  struct NamedZoneGetter {
    const char* name;
    JSNative getter;
  } zoneGetters[] = {{"gcBytes", ZoneGCBytesGetter},
                     {"gcTriggerBytes", ZoneGCTriggerBytesGetter},
                     {"gcAllocTrigger", ZoneGCAllocTriggerGetter},
                     {"mallocBytes", ZoneMallocBytesGetter},
                     {"mallocTriggerBytes", ZoneMallocTriggerBytesGetter},
                     {"gcNumber", ZoneGCNumberGetter}};

  for (auto pair : zoneGetters) {
    JSNative getter = pair.getter;

#ifdef DEBUG
    if (js::SupportDifferentialTesting()) {
      getter = DummyGetter;
    }
#endif

    if (!JS_DefineProperty(cx, zoneObj, pair.name, getter, nullptr,
                           JSPROP_ENUMERATE)) {
      return nullptr;
    }
  }

  return obj;
}

const char* StateName(State state) {
  switch (state) {
#define MAKE_CASE(name) \
  case State::name:     \
    return #name;
    GCSTATES(MAKE_CASE)
#undef MAKE_CASE
  }
  MOZ_CRASH("Invalid gc::State enum value");
}

const char* StateName(JS::Zone::GCState state) {
  switch (state) {
    case JS::Zone::NoGC:
      return "NoGC";
    case JS::Zone::Prepare:
      return "Prepare";
    case JS::Zone::MarkBlackOnly:
      return "MarkBlackOnly";
    case JS::Zone::MarkBlackAndGray:
      return "MarkBlackAndGray";
    case JS::Zone::Sweep:
      return "Sweep";
    case JS::Zone::Finished:
      return "Finished";
    case JS::Zone::Compact:
      return "Compact";
    case JS::Zone::VerifyPreBarriers:
      return "VerifyPreBarriers";
    case JS::Zone::Limit:
      break;
  }
  MOZ_CRASH("Invalid Zone::GCState enum value");
}

const char* CellColorName(CellColor color) {
  switch (color) {
    case CellColor::White:
      return "white";
    case CellColor::Black:
      return "black";
    case CellColor::Gray:
      return "gray";
    default:
      MOZ_CRASH("Unexpected cell color");
  }
}

} /* namespace gc */
} /* namespace js */

JS_PUBLIC_API JS::GCContext* js::gc::GetGCContext(JSContext* cx) {
  CHECK_THREAD(cx);
  return cx->gcContext();
}

JS_PUBLIC_API void js::gc::SetPerformanceHint(JSContext* cx,
                                              PerformanceHint hint) {
  CHECK_THREAD(cx);
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());

  cx->runtime()->gc.setPerformanceHint(hint);
}

AutoSelectGCHeap::AutoSelectGCHeap(JSContext* cx,
                                   size_t allowedNurseryCollections)
    : cx_(cx), allowedNurseryCollections_(allowedNurseryCollections) {
  if (!JS::AddGCNurseryCollectionCallback(cx, &NurseryCollectionCallback,
                                          this)) {
    cx_ = nullptr;
  }
}

AutoSelectGCHeap::~AutoSelectGCHeap() {
  if (cx_) {
    JS::RemoveGCNurseryCollectionCallback(cx_, &NurseryCollectionCallback,
                                          this);
  }
}

/* static */
void AutoSelectGCHeap::NurseryCollectionCallback(JSContext* cx,
                                                 JS::GCNurseryProgress progress,
                                                 JS::GCReason reason,
                                                 void* data) {
  if (progress == JS::GCNurseryProgress::GC_NURSERY_COLLECTION_END) {
    static_cast<AutoSelectGCHeap*>(data)->onNurseryCollectionEnd();
  }
}

void AutoSelectGCHeap::onNurseryCollectionEnd() {
  if (allowedNurseryCollections_ != 0) {
    allowedNurseryCollections_--;
    return;
  }

  heap_ = gc::Heap::Tenured;
}

JS_PUBLIC_API void js::gc::LockStoreBuffer(JSRuntime* runtime) {
  MOZ_ASSERT(runtime);
  runtime->gc.lockStoreBuffer();
}

JS_PUBLIC_API void js::gc::UnlockStoreBuffer(JSRuntime* runtime) {
  MOZ_ASSERT(runtime);
  runtime->gc.unlockStoreBuffer();
}

#ifdef JS_GC_ZEAL
JS_PUBLIC_API void JS::GetGCZealBits(JSContext* cx, uint32_t* zealBits,
                                     uint32_t* frequency,
                                     uint32_t* nextScheduled) {
  cx->runtime()->gc.getZealBits(zealBits, frequency, nextScheduled);
}

JS_PUBLIC_API void JS::SetGCZeal(JSContext* cx, uint8_t zeal,
                                 uint32_t frequency) {
  cx->runtime()->gc.setZeal(zeal, frequency);
}

JS_PUBLIC_API void JS::UnsetGCZeal(JSContext* cx, uint8_t zeal) {
  cx->runtime()->gc.unsetZeal(zeal);
}

JS_PUBLIC_API void JS::ScheduleGC(JSContext* cx, uint32_t count) {
  cx->runtime()->gc.setNextScheduled(count);
}
#endif
