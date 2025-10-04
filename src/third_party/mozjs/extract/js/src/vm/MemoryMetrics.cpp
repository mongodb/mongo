/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/MemoryMetrics.h"

#include "mozilla/MathAlgorithms.h"

#include <algorithm>

#include "gc/GC.h"
#include "gc/Memory.h"
#include "gc/Nursery.h"
#include "gc/PublicIterators.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "js/HeapAPI.h"
#include "util/Text.h"
#include "vm/BigIntType.h"
#include "vm/HelperThreadState.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/PropMap.h"
#include "vm/Realm.h"
#include "vm/Runtime.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmModule.h"

#include "wasm/WasmInstance-inl.h"

using mozilla::MallocSizeOf;
using mozilla::PodCopy;

using namespace js;

using JS::ObjectPrivateVisitor;
using JS::RealmStats;
using JS::RuntimeStats;
using JS::ZoneStats;

namespace js {

JS_PUBLIC_API size_t MemoryReportingSundriesThreshold() { return 8 * 1024; }

/* static */
HashNumber InefficientNonFlatteningStringHashPolicy::hash(const Lookup& l) {
  if (l->isLinear()) {
    return HashStringChars(&l->asLinear());
  }

  // Use rope's non-copying hash function.
  uint32_t hash = 0;
  if (!l->asRope().hash(&hash)) {
    MOZ_CRASH("oom");
  }
  return hash;
}

template <typename Char1, typename Char2>
static bool EqualStringsPure(JSString* s1, JSString* s2) {
  if (s1->length() != s2->length()) {
    return false;
  }

  const Char1* c1;
  UniquePtr<Char1[], JS::FreePolicy> ownedChars1;
  JS::AutoCheckCannotGC nogc;
  if (s1->isLinear()) {
    c1 = s1->asLinear().chars<Char1>(nogc);
  } else {
    ownedChars1 =
        s1->asRope().copyChars<Char1>(/* tcx */ nullptr, js::MallocArena);
    if (!ownedChars1) {
      MOZ_CRASH("oom");
    }
    c1 = ownedChars1.get();
  }

  const Char2* c2;
  UniquePtr<Char2[], JS::FreePolicy> ownedChars2;
  if (s2->isLinear()) {
    c2 = s2->asLinear().chars<Char2>(nogc);
  } else {
    ownedChars2 =
        s2->asRope().copyChars<Char2>(/* tcx */ nullptr, js::MallocArena);
    if (!ownedChars2) {
      MOZ_CRASH("oom");
    }
    c2 = ownedChars2.get();
  }

  return EqualChars(c1, c2, s1->length());
}

/* static */
bool InefficientNonFlatteningStringHashPolicy::match(const JSString* const& k,
                                                     const Lookup& l) {
  // We can't use js::EqualStrings, because that flattens our strings.
  JSString* s1 = const_cast<JSString*>(k);
  if (k->hasLatin1Chars()) {
    return l->hasLatin1Chars() ? EqualStringsPure<Latin1Char, Latin1Char>(s1, l)
                               : EqualStringsPure<Latin1Char, char16_t>(s1, l);
  }

  return l->hasLatin1Chars() ? EqualStringsPure<char16_t, Latin1Char>(s1, l)
                             : EqualStringsPure<char16_t, char16_t>(s1, l);
}

}  // namespace js

namespace JS {

template <typename CharT>
static void StoreStringChars(char* buffer, size_t bufferSize, JSString* str) {
  const CharT* chars;
  UniquePtr<CharT[], JS::FreePolicy> ownedChars;
  JS::AutoCheckCannotGC nogc;
  if (str->isLinear()) {
    chars = str->asLinear().chars<CharT>(nogc);
  } else {
    ownedChars =
        str->asRope().copyChars<CharT>(/* tcx */ nullptr, js::MallocArena);
    if (!ownedChars) {
      MOZ_CRASH("oom");
    }
    chars = ownedChars.get();
  }

  // We might truncate |str| even if it's much shorter than 1024 chars, if
  // |str| contains unicode chars.  Since this is just for a memory reporter,
  // we don't care.
  PutEscapedString(buffer, bufferSize, chars, str->length(), /* quote */ 0);
}

NotableStringInfo::NotableStringInfo(JSString* str, const StringInfo& info)
    : StringInfo(info), length(str->length()) {
  size_t bufferSize = std::min(str->length() + 1, size_t(MAX_SAVED_CHARS));
  buffer.reset(js_pod_malloc<char>(bufferSize));
  if (!buffer) {
    MOZ_CRASH("oom");
  }

  if (str->hasLatin1Chars()) {
    StoreStringChars<Latin1Char>(buffer.get(), bufferSize, str);
  } else {
    StoreStringChars<char16_t>(buffer.get(), bufferSize, str);
  }
}

NotableClassInfo::NotableClassInfo(const char* className, const ClassInfo& info)
    : ClassInfo(info) {
  className_ = DuplicateString(className);
  if (!className_) {
    MOZ_CRASH("oom");
  }
}

NotableScriptSourceInfo::NotableScriptSourceInfo(const char* filename,
                                                 const ScriptSourceInfo& info)
    : ScriptSourceInfo(info) {
  filename_ = DuplicateString(filename);
  if (!filename_) {
    MOZ_CRASH("oom");
  }
}

}  // namespace JS

typedef HashSet<ScriptSource*, DefaultHasher<ScriptSource*>, SystemAllocPolicy>
    SourceSet;

struct StatsClosure {
  RuntimeStats* rtStats;
  ObjectPrivateVisitor* opv;
  SourceSet seenSources;
  wasm::Metadata::SeenSet wasmSeenMetadata;
  wasm::Code::SeenSet wasmSeenCode;
  wasm::Table::SeenSet wasmSeenTables;
  bool anonymize;

  StatsClosure(RuntimeStats* rt, ObjectPrivateVisitor* v, bool anon)
      : rtStats(rt), opv(v), anonymize(anon) {}
};

static void DecommittedPagesChunkCallback(JSRuntime* rt, void* data,
                                          gc::TenuredChunk* chunk,
                                          const JS::AutoRequireNoGC& nogc) {
  size_t n = 0;
  for (uint32_t word : chunk->decommittedPages.Storage()) {
    n += mozilla::CountPopulation32(word);
  }

  *static_cast<size_t*>(data) += n * gc::PageSize;
}

static void StatsZoneCallback(JSRuntime* rt, void* data, Zone* zone,
                              const JS::AutoRequireNoGC& nogc) {
  // Append a new RealmStats to the vector.
  RuntimeStats* rtStats = static_cast<StatsClosure*>(data)->rtStats;

  // CollectRuntimeStats reserves enough space.
  MOZ_ALWAYS_TRUE(rtStats->zoneStatsVector.growBy(1));
  ZoneStats& zStats = rtStats->zoneStatsVector.back();
  zStats.initStrings();
  rtStats->initExtraZoneStats(zone, &zStats, nogc);
  rtStats->currZoneStats = &zStats;

  zone->addSizeOfIncludingThis(
      rtStats->mallocSizeOf_, &zStats.zoneObject, &zStats.code,
      &zStats.regexpZone, &zStats.jitZone, &zStats.cacheIRStubs,
      &zStats.uniqueIdMap, &zStats.initialPropMapTable, &zStats.shapeTables,
      &rtStats->runtime.atomsMarkBitmaps, &zStats.compartmentObjects,
      &zStats.crossCompartmentWrappersTables, &zStats.compartmentsPrivateData,
      &zStats.scriptCountsMap);
}

static void StatsRealmCallback(JSContext* cx, void* data, Realm* realm,
                               const JS::AutoRequireNoGC& nogc) {
  // Append a new RealmStats to the vector.
  RuntimeStats* rtStats = static_cast<StatsClosure*>(data)->rtStats;

  // CollectRuntimeStats reserves enough space.
  MOZ_ALWAYS_TRUE(rtStats->realmStatsVector.growBy(1));
  RealmStats& realmStats = rtStats->realmStatsVector.back();
  realmStats.initClasses();
  rtStats->initExtraRealmStats(realm, &realmStats, nogc);

  realm->setRealmStats(&realmStats);

  // Measure the realm object itself, and things hanging off it.
  realm->addSizeOfIncludingThis(
      rtStats->mallocSizeOf_, &realmStats.realmObject, &realmStats.realmTables,
      &realmStats.innerViewsTable, &realmStats.objectMetadataTable,
      &realmStats.savedStacksSet, &realmStats.nonSyntacticLexicalScopesTable);
}

static void StatsArenaCallback(JSRuntime* rt, void* data, gc::Arena* arena,
                               JS::TraceKind traceKind, size_t thingSize,
                               const JS::AutoRequireNoGC& nogc) {
  RuntimeStats* rtStats = static_cast<StatsClosure*>(data)->rtStats;

  // The admin space includes (a) the header fields and (b) the padding
  // between the end of the header fields and the first GC thing.
  size_t allocationSpace = gc::Arena::thingsSpan(arena->getAllocKind());
  rtStats->currZoneStats->gcHeapArenaAdmin += gc::ArenaSize - allocationSpace;

  // We don't call the callback on unused things.  So we compute the
  // unused space like this:  arenaUnused = maxArenaUnused - arenaUsed.
  // We do this by setting arenaUnused to maxArenaUnused here, and then
  // subtracting thingSize for every used cell, in StatsCellCallback().
  rtStats->currZoneStats->unusedGCThings.addToKind(traceKind, allocationSpace);
}

// FineGrained is used for normal memory reporting.  CoarseGrained is used by
// AddSizeOfTab(), which aggregates all the measurements into a handful of
// high-level numbers, which means that fine-grained reporting would be a waste
// of effort.
enum Granularity { FineGrained, CoarseGrained };

static void AddClassInfo(Granularity granularity, RealmStats& realmStats,
                         const char* className, JS::ClassInfo& info) {
  if (granularity == FineGrained) {
    if (!className) {
      className = "<no class name>";
    }
    RealmStats::ClassesHashMap::AddPtr p =
        realmStats.allClasses->lookupForAdd(className);
    if (!p) {
      bool ok = realmStats.allClasses->add(p, className, info);
      // Ignore failure -- we just won't record the
      // object/shape/base-shape as notable.
      (void)ok;
    } else {
      p->value().add(info);
    }
  }
}

template <Granularity granularity>
static void CollectScriptSourceStats(StatsClosure* closure, ScriptSource* ss) {
  RuntimeStats* rtStats = closure->rtStats;

  SourceSet::AddPtr entry = closure->seenSources.lookupForAdd(ss);
  if (entry) {
    return;
  }

  bool ok = closure->seenSources.add(entry, ss);
  (void)ok;  // Not much to be done on failure.

  JS::ScriptSourceInfo info;  // This zeroes all the sizes.
  ss->addSizeOfIncludingThis(rtStats->mallocSizeOf_, &info);

  rtStats->runtime.scriptSourceInfo.add(info);

  if (granularity == FineGrained) {
    const char* filename = ss->filename();
    if (!filename) {
      filename = "<no filename>";
    }

    JS::RuntimeSizes::ScriptSourcesHashMap::AddPtr p =
        rtStats->runtime.allScriptSources->lookupForAdd(filename);
    if (!p) {
      bool ok = rtStats->runtime.allScriptSources->add(p, filename, info);
      // Ignore failure -- we just won't record the script source as notable.
      (void)ok;
    } else {
      p->value().add(info);
    }
  }
}

// The various kinds of hashing are expensive, and the results are unused when
// doing coarse-grained measurements. Skipping them more than doubles the
// profile speed for complex pages such as gmail.com.
template <Granularity granularity>
static void StatsCellCallback(JSRuntime* rt, void* data, JS::GCCellPtr cellptr,
                              size_t thingSize,
                              const JS::AutoRequireNoGC& nogc) {
  StatsClosure* closure = static_cast<StatsClosure*>(data);
  RuntimeStats* rtStats = closure->rtStats;
  ZoneStats* zStats = rtStats->currZoneStats;
  JS::TraceKind kind = cellptr.kind();
  switch (kind) {
    case JS::TraceKind::Object: {
      JSObject* obj = &cellptr.as<JSObject>();
      RealmStats& realmStats = obj->maybeCCWRealm()->realmStats();
      JS::ClassInfo info;  // This zeroes all the sizes.
      info.objectsGCHeap += thingSize;

      if (!obj->isTenured()) {
        info.objectsGCHeap += Nursery::nurseryCellHeaderSize();
      }

      obj->addSizeOfExcludingThis(rtStats->mallocSizeOf_, &info,
                                  &rtStats->runtime);

      // These classes require special handling due to shared resources which
      // we must be careful not to report twice.
      if (obj->is<WasmModuleObject>()) {
        const wasm::Module& module = obj->as<WasmModuleObject>().module();
        if (ScriptSource* ss = module.metadata().maybeScriptSource()) {
          CollectScriptSourceStats<granularity>(closure, ss);
        }
        module.addSizeOfMisc(rtStats->mallocSizeOf_, &closure->wasmSeenMetadata,
                             &closure->wasmSeenCode,
                             &info.objectsNonHeapCodeWasm,
                             &info.objectsMallocHeapMisc);
      } else if (obj->is<WasmInstanceObject>()) {
        wasm::Instance& instance = obj->as<WasmInstanceObject>().instance();
        if (ScriptSource* ss = instance.metadata().maybeScriptSource()) {
          CollectScriptSourceStats<granularity>(closure, ss);
        }
        instance.addSizeOfMisc(
            rtStats->mallocSizeOf_, &closure->wasmSeenMetadata,
            &closure->wasmSeenCode, &closure->wasmSeenTables,
            &info.objectsNonHeapCodeWasm, &info.objectsMallocHeapMisc);
      }

      realmStats.classInfo.add(info);

      const JSClass* clasp = obj->getClass();
      const char* className = clasp->name;
      AddClassInfo(granularity, realmStats, className, info);

      if (ObjectPrivateVisitor* opv = closure->opv) {
        nsISupports* iface;
        if (opv->getISupports_(obj, &iface) && iface) {
          realmStats.objectsPrivate += opv->sizeOfIncludingThis(iface);
        }
      }
      break;
    }

    case JS::TraceKind::Script: {
      BaseScript* base = &cellptr.as<BaseScript>();
      RealmStats& realmStats = base->realm()->realmStats();
      realmStats.scriptsGCHeap += thingSize;
      realmStats.scriptsMallocHeapData +=
          base->sizeOfExcludingThis(rtStats->mallocSizeOf_);
      if (base->hasJitScript()) {
        JSScript* script = static_cast<JSScript*>(base);
        script->addSizeOfJitScript(rtStats->mallocSizeOf_,
                                   &realmStats.jitScripts,
                                   &realmStats.allocSites);
        jit::AddSizeOfBaselineData(script, rtStats->mallocSizeOf_,
                                   &realmStats.baselineData);
        realmStats.ionData +=
            jit::SizeOfIonData(script, rtStats->mallocSizeOf_);
      }
      CollectScriptSourceStats<granularity>(closure, base->scriptSource());
      break;
    }

    case JS::TraceKind::String: {
      JSString* str = &cellptr.as<JSString>();
      size_t size = thingSize;
      if (!str->isTenured()) {
        size += Nursery::nurseryCellHeaderSize();
      }

      JS::StringInfo info;
      if (str->hasLatin1Chars()) {
        info.gcHeapLatin1 = size;
        info.mallocHeapLatin1 =
            str->sizeOfExcludingThis(rtStats->mallocSizeOf_);
      } else {
        info.gcHeapTwoByte = size;
        info.mallocHeapTwoByte =
            str->sizeOfExcludingThis(rtStats->mallocSizeOf_);
      }
      info.numCopies = 1;

      zStats->stringInfo.add(info);

      // The primary use case for anonymization is automated crash submission
      // (to help detect OOM crashes). In that case, we don't want to pay the
      // memory cost required to do notable string detection.
      if (granularity == FineGrained && !closure->anonymize) {
        ZoneStats::StringsHashMap::AddPtr p =
            zStats->allStrings->lookupForAdd(str);
        if (!p) {
          bool ok = zStats->allStrings->add(p, str, info);
          // Ignore failure -- we just won't record the string as notable.
          (void)ok;
        } else {
          p->value().add(info);
        }
      }
      break;
    }

    case JS::TraceKind::Symbol:
      zStats->symbolsGCHeap += thingSize;
      break;

    case JS::TraceKind::BigInt: {
      JS::BigInt* bi = &cellptr.as<BigInt>();
      size_t size = thingSize;
      if (!bi->isTenured()) {
        size += Nursery::nurseryCellHeaderSize();
      }
      zStats->bigIntsGCHeap += size;
      zStats->bigIntsMallocHeap +=
          bi->sizeOfExcludingThis(rtStats->mallocSizeOf_);
      break;
    }

    case JS::TraceKind::BaseShape: {
      JS::ShapeInfo info;  // This zeroes all the sizes.
      info.shapesGCHeapBase += thingSize;
      // No malloc-heap measurements.

      zStats->shapeInfo.add(info);
      break;
    }

    case JS::TraceKind::GetterSetter: {
      zStats->getterSettersGCHeap += thingSize;
      break;
    }

    case JS::TraceKind::PropMap: {
      PropMap* map = &cellptr.as<PropMap>();
      if (map->isDictionary()) {
        zStats->dictPropMapsGCHeap += thingSize;
      } else if (map->isCompact()) {
        zStats->compactPropMapsGCHeap += thingSize;
      } else {
        MOZ_ASSERT(map->isNormal());
        zStats->normalPropMapsGCHeap += thingSize;
      }
      map->addSizeOfExcludingThis(rtStats->mallocSizeOf_,
                                  &zStats->propMapChildren,
                                  &zStats->propMapTables);
      break;
    }

    case JS::TraceKind::JitCode: {
      zStats->jitCodesGCHeap += thingSize;
      // The code for a script is counted in ExecutableAllocator::sizeOfCode().
      break;
    }

    case JS::TraceKind::Shape: {
      Shape* shape = &cellptr.as<Shape>();

      JS::ShapeInfo info;  // This zeroes all the sizes.
      if (shape->isDictionary()) {
        info.shapesGCHeapDict += thingSize;
      } else {
        info.shapesGCHeapShared += thingSize;
      }
      shape->addSizeOfExcludingThis(rtStats->mallocSizeOf_, &info);
      zStats->shapeInfo.add(info);
      break;
    }

    case JS::TraceKind::Scope: {
      Scope* scope = &cellptr.as<Scope>();
      zStats->scopesGCHeap += thingSize;
      zStats->scopesMallocHeap +=
          scope->sizeOfExcludingThis(rtStats->mallocSizeOf_);
      break;
    }

    case JS::TraceKind::RegExpShared: {
      auto regexp = &cellptr.as<RegExpShared>();
      zStats->regExpSharedsGCHeap += thingSize;
      zStats->regExpSharedsMallocHeap +=
          regexp->sizeOfExcludingThis(rtStats->mallocSizeOf_);
      break;
    }

    default:
      MOZ_CRASH("invalid traceKind in StatsCellCallback");
  }

  // Yes, this is a subtraction:  see StatsArenaCallback() for details.
  zStats->unusedGCThings.addToKind(kind, -thingSize);
}

void ZoneStats::initStrings() {
  isTotals = false;
  allStrings.emplace();
}

void RealmStats::initClasses() {
  isTotals = false;
  allClasses.emplace();
}

static bool FindNotableStrings(ZoneStats& zStats) {
  using namespace JS;

  // We should only run FindNotableStrings once per ZoneStats object.
  MOZ_ASSERT(zStats.notableStrings.empty());

  for (ZoneStats::StringsHashMap::Range r = zStats.allStrings->all();
       !r.empty(); r.popFront()) {
    JSString* str = r.front().key();
    StringInfo& info = r.front().value();

    if (!info.isNotable()) {
      continue;
    }

    if (!zStats.notableStrings.emplaceBack(str, info)) {
      return false;
    }

    // We're moving this string from a non-notable to a notable bucket, so
    // subtract it out of the non-notable tallies.
    zStats.stringInfo.subtract(info);
  }
  // Release |allStrings| now, rather than waiting for zStats's destruction, to
  // reduce peak memory consumption during reporting.
  zStats.allStrings.reset();
  return true;
}

static bool FindNotableClasses(RealmStats& realmStats) {
  using namespace JS;

  // We should only run FindNotableClasses once per ZoneStats object.
  MOZ_ASSERT(realmStats.notableClasses.empty());

  for (RealmStats::ClassesHashMap::Range r = realmStats.allClasses->all();
       !r.empty(); r.popFront()) {
    const char* className = r.front().key();
    ClassInfo& info = r.front().value();

    // If this class isn't notable, or if we can't grow the notableStrings
    // vector, skip this string.
    if (!info.isNotable()) {
      continue;
    }

    if (!realmStats.notableClasses.emplaceBack(className, info)) {
      return false;
    }

    // We're moving this class from a non-notable to a notable bucket, so
    // subtract it out of the non-notable tallies.
    realmStats.classInfo.subtract(info);
  }
  // Release |allClasses| now, rather than waiting for zStats's destruction, to
  // reduce peak memory consumption during reporting.
  realmStats.allClasses.reset();
  return true;
}

static bool FindNotableScriptSources(JS::RuntimeSizes& runtime) {
  using namespace JS;

  // We should only run FindNotableScriptSources once per RuntimeSizes.
  MOZ_ASSERT(runtime.notableScriptSources.empty());

  for (RuntimeSizes::ScriptSourcesHashMap::Range r =
           runtime.allScriptSources->all();
       !r.empty(); r.popFront()) {
    const char* filename = r.front().key();
    ScriptSourceInfo& info = r.front().value();

    if (!info.isNotable()) {
      continue;
    }

    if (!runtime.notableScriptSources.emplaceBack(filename, info)) {
      return false;
    }

    // We're moving this script source from a non-notable to a notable
    // bucket, so subtract its sizes from the non-notable tallies.
    runtime.scriptSourceInfo.subtract(info);
  }
  // Release |allScriptSources| now, rather than waiting for zStats's
  // destruction, to reduce peak memory consumption during reporting.
  runtime.allScriptSources.reset();
  return true;
}

static bool CollectRuntimeStatsHelper(JSContext* cx, RuntimeStats* rtStats,
                                      ObjectPrivateVisitor* opv, bool anonymize,
                                      IterateCellCallback statsCellCallback) {
  // Finish any ongoing incremental GC that may change the data we're gathering
  // and ensure that we don't do anything that could start another one.
  gc::FinishGC(cx);
  JS::AutoAssertNoGC nogc(cx);

  // Wait for any background tasks to finish.
  WaitForAllHelperThreads();

  JSRuntime* rt = cx->runtime();
  if (!rtStats->realmStatsVector.reserve(rt->numRealms)) {
    return false;
  }

  size_t totalZones = rt->gc.zones().length();
  if (!rtStats->zoneStatsVector.reserve(totalZones)) {
    return false;
  }

  rtStats->gcHeapChunkTotal =
      size_t(JS_GetGCParameter(cx, JSGC_TOTAL_CHUNKS)) * gc::ChunkSize;

  rtStats->gcHeapUnusedChunks =
      size_t(JS_GetGCParameter(cx, JSGC_UNUSED_CHUNKS)) * gc::ChunkSize;

  if (js::gc::DecommitEnabled()) {
    IterateChunks(cx, &rtStats->gcHeapDecommittedPages,
                  DecommittedPagesChunkCallback);
  }

  // Take the per-compartment measurements.
  StatsClosure closure(rtStats, opv, anonymize);
  IterateHeapUnbarriered(cx, &closure, StatsZoneCallback, StatsRealmCallback,
                         StatsArenaCallback, statsCellCallback);

  // Take the "explicit/js/runtime/" measurements.
  rt->addSizeOfIncludingThis(rtStats->mallocSizeOf_, &rtStats->runtime);

  if (!FindNotableScriptSources(rtStats->runtime)) {
    return false;
  }

  JS::ZoneStatsVector& zs = rtStats->zoneStatsVector;
  ZoneStats& zTotals = rtStats->zTotals;

  // We don't look for notable strings for zTotals. So we first sum all the
  // zones' measurements to get the totals. Then we find the notable strings
  // within each zone.
  for (size_t i = 0; i < zs.length(); i++) {
    zTotals.addSizes(zs[i]);
  }

  for (size_t i = 0; i < zs.length(); i++) {
    if (!FindNotableStrings(zs[i])) {
      return false;
    }
  }

  MOZ_ASSERT(!zTotals.allStrings);

  JS::RealmStatsVector& realmStats = rtStats->realmStatsVector;
  RealmStats& realmTotals = rtStats->realmTotals;

  // As with the zones, we sum all realms first, and then get the
  // notable classes within each zone.
  for (size_t i = 0; i < realmStats.length(); i++) {
    realmTotals.addSizes(realmStats[i]);
  }

  for (size_t i = 0; i < realmStats.length(); i++) {
    if (!FindNotableClasses(realmStats[i])) {
      return false;
    }
  }

  MOZ_ASSERT(!realmTotals.allClasses);

  rtStats->gcHeapGCThings = rtStats->zTotals.sizeOfLiveGCThings() +
                            rtStats->realmTotals.sizeOfLiveGCThings();

#ifdef DEBUG
  // Check that the in-arena measurements look ok.
  size_t totalArenaSize = rtStats->zTotals.gcHeapArenaAdmin +
                          rtStats->zTotals.unusedGCThings.totalSize() +
                          rtStats->gcHeapGCThings;
  MOZ_ASSERT(totalArenaSize % gc::ArenaSize == 0);
#endif

  for (RealmsIter realm(rt); !realm.done(); realm.next()) {
    realm->nullRealmStats();
  }

  size_t numDirtyChunks =
      (rtStats->gcHeapChunkTotal - rtStats->gcHeapUnusedChunks) / gc::ChunkSize;
  size_t perChunkAdmin =
      sizeof(gc::TenuredChunk) - (sizeof(gc::Arena) * gc::ArenasPerChunk);
  rtStats->gcHeapChunkAdmin = numDirtyChunks * perChunkAdmin;

  // |gcHeapUnusedArenas| is the only thing left.  Compute it in terms of
  // all the others.  See the comment in RuntimeStats for explanation.
  rtStats->gcHeapUnusedArenas =
      rtStats->gcHeapChunkTotal - rtStats->gcHeapDecommittedPages -
      rtStats->gcHeapUnusedChunks -
      rtStats->zTotals.unusedGCThings.totalSize() - rtStats->gcHeapChunkAdmin -
      rtStats->zTotals.gcHeapArenaAdmin - rtStats->gcHeapGCThings;
  return true;
}

JS_PUBLIC_API bool JS::CollectGlobalStats(GlobalStats* gStats) {
  AutoLockHelperThreadState lock;

  // HelperThreadState holds data that is not part of a Runtime. This does
  // not include data is is currently being processed by a HelperThread.
  if (IsHelperThreadStateInitialized()) {
    HelperThreadState().addSizeOfIncludingThis(gStats, lock);
  }

  return true;
}

JS_PUBLIC_API bool JS::CollectRuntimeStats(JSContext* cx, RuntimeStats* rtStats,
                                           ObjectPrivateVisitor* opv,
                                           bool anonymize) {
  return CollectRuntimeStatsHelper(cx, rtStats, opv, anonymize,
                                   StatsCellCallback<FineGrained>);
}

JS_PUBLIC_API size_t JS::SystemCompartmentCount(JSContext* cx) {
  size_t n = 0;
  for (CompartmentsIter comp(cx->runtime()); !comp.done(); comp.next()) {
    if (IsSystemCompartment(comp)) {
      ++n;
    }
  }
  return n;
}

JS_PUBLIC_API size_t JS::UserCompartmentCount(JSContext* cx) {
  size_t n = 0;
  for (CompartmentsIter comp(cx->runtime()); !comp.done(); comp.next()) {
    if (!IsSystemCompartment(comp)) {
      ++n;
    }
  }
  return n;
}

JS_PUBLIC_API size_t JS::SystemRealmCount(JSContext* cx) {
  size_t n = 0;
  for (RealmsIter realm(cx->runtime()); !realm.done(); realm.next()) {
    if (realm->isSystem()) {
      ++n;
    }
  }
  return n;
}

JS_PUBLIC_API size_t JS::UserRealmCount(JSContext* cx) {
  size_t n = 0;
  for (RealmsIter realm(cx->runtime()); !realm.done(); realm.next()) {
    if (!realm->isSystem()) {
      ++n;
    }
  }
  return n;
}

JS_PUBLIC_API size_t JS::PeakSizeOfTemporary(const JSContext* cx) {
  return cx->tempLifoAlloc().peakSizeOfExcludingThis();
}

namespace JS {

class SimpleJSRuntimeStats : public JS::RuntimeStats {
 public:
  explicit SimpleJSRuntimeStats(MallocSizeOf mallocSizeOf)
      : JS::RuntimeStats(mallocSizeOf) {}

  virtual void initExtraZoneStats(JS::Zone* zone, JS::ZoneStats* zStats,
                                  const JS::AutoRequireNoGC& nogc) override {}

  virtual void initExtraRealmStats(Realm* realm, JS::RealmStats* realmStats,
                                   const JS::AutoRequireNoGC& nogc) override {}
};

JS_PUBLIC_API bool AddSizeOfTab(JSContext* cx, HandleObject obj,
                                MallocSizeOf mallocSizeOf,
                                ObjectPrivateVisitor* opv, TabSizes* sizes) {
  SimpleJSRuntimeStats rtStats(mallocSizeOf);

  JS::Zone* zone = GetObjectZone(obj);

  size_t numRealms = 0;
  for (CompartmentsInZoneIter comp(zone); !comp.done(); comp.next()) {
    numRealms += comp->realms().length();
  }

  if (!rtStats.realmStatsVector.reserve(numRealms)) {
    return false;
  }

  if (!rtStats.zoneStatsVector.reserve(1)) {
    return false;
  }

  // Take the per-compartment measurements. No need to anonymize because
  // these measurements will be aggregated.
  StatsClosure closure(&rtStats, opv, /* anonymize = */ false);
  IterateHeapUnbarrieredForZone(cx, zone, &closure, StatsZoneCallback,
                                StatsRealmCallback, StatsArenaCallback,
                                StatsCellCallback<CoarseGrained>);

  MOZ_ASSERT(rtStats.zoneStatsVector.length() == 1);
  rtStats.zTotals.addSizes(rtStats.zoneStatsVector[0]);

  for (size_t i = 0; i < rtStats.realmStatsVector.length(); i++) {
    rtStats.realmTotals.addSizes(rtStats.realmStatsVector[i]);
  }

  for (RealmsInZoneIter realm(zone); !realm.done(); realm.next()) {
    realm->nullRealmStats();
  }

  rtStats.zTotals.addToTabSizes(sizes);
  rtStats.realmTotals.addToTabSizes(sizes);

  return true;
}

JS_PUBLIC_API bool AddServoSizeOf(JSContext* cx, MallocSizeOf mallocSizeOf,
                                  ObjectPrivateVisitor* opv,
                                  ServoSizes* sizes) {
  SimpleJSRuntimeStats rtStats(mallocSizeOf);

  // No need to anonymize because the results will be aggregated.
  if (!CollectRuntimeStatsHelper(cx, &rtStats, opv, /* anonymize = */ false,
                                 StatsCellCallback<CoarseGrained>))
    return false;

#ifdef DEBUG
  size_t gcHeapTotalOriginal = sizes->gcHeapUsed + sizes->gcHeapUnused +
                               sizes->gcHeapAdmin + sizes->gcHeapDecommitted;
#endif

  rtStats.addToServoSizes(sizes);
  rtStats.zTotals.addToServoSizes(sizes);
  rtStats.realmTotals.addToServoSizes(sizes);

#ifdef DEBUG
  size_t gcHeapTotal = sizes->gcHeapUsed + sizes->gcHeapUnused +
                       sizes->gcHeapAdmin + sizes->gcHeapDecommitted;
  MOZ_ASSERT(rtStats.gcHeapChunkTotal == gcHeapTotal - gcHeapTotalOriginal);
#endif

  return true;
}

}  // namespace JS
