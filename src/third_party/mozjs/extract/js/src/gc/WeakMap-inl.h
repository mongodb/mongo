/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_WeakMap_inl_h
#define gc_WeakMap_inl_h

#include "gc/WeakMap.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include <algorithm>
#include <type_traits>

#include "gc/GCLock.h"
#include "gc/Marking.h"
#include "gc/Zone.h"
#include "js/Prefs.h"
#include "js/TraceKind.h"
#include "vm/JSContext.h"

#include "gc/Marking-inl.h"
#include "gc/StableCellHasher-inl.h"

namespace js {

namespace gc::detail {

// Return the effective cell color given the current marking state.
// This must be kept in sync with ShouldMark in Marking.cpp.
template <typename T>
static CellColor GetEffectiveColor(GCMarker* marker, const T& item) {
  Cell* cell = ToMarkable(item);
  if (!cell->isTenured()) {
    return CellColor::Black;
  }
  const TenuredCell& t = cell->asTenured();
  if (!t.zoneFromAnyThread()->shouldMarkInZone(marker->markColor())) {
    return CellColor::Black;
  }
  MOZ_ASSERT(t.runtimeFromAnyThread() == marker->runtime());
  return t.color();
}

// If a wrapper is used as a key in a weakmap, the garbage collector should
// keep that object around longer than it otherwise would. We want to avoid
// collecting the wrapper (and removing the weakmap entry) as long as the
// wrapped object is alive (because the object can be rewrapped and looked up
// again). As long as the wrapper is used as a weakmap key, it will not be
// collected (and remain in the weakmap) until the wrapped object is
// collected.

// Only objects have delegates, so default to returning nullptr. Note that some
// compilation units will only ever use the object version.
static MOZ_MAYBE_UNUSED JSObject* GetDelegateInternal(gc::Cell* key) {
  return nullptr;
}

static MOZ_MAYBE_UNUSED JSObject* GetDelegateInternal(JSObject* key) {
  JSObject* delegate = UncheckedUnwrapWithoutExpose(key);
  return (key == delegate) ? nullptr : delegate;
}
static MOZ_MAYBE_UNUSED JSObject* GetDelegateInternal(const Value& key) {
  if (key.isObject()) {
    return GetDelegateInternal(&key.toObject());
  }
  return nullptr;
}

// Use a helper function to do overload resolution to handle cases like
// Heap<ObjectSubclass*>: find everything that is convertible to JSObject* (and
// avoid calling barriers).
template <typename T>
static inline JSObject* GetDelegate(const T& key) {
  return GetDelegateInternal(key);
}

template <>
inline JSObject* GetDelegate(gc::Cell* const&) = delete;

template <typename T>
static inline bool IsSymbol(const T& key) {
  return false;
}

template <>
inline bool IsSymbol(const HeapPtr<JS::Value>& key) {
  return key.isSymbol();
}

}  // namespace gc::detail

// Weakmap entry -> value edges are only visible if the map is traced, which
// only happens if the map zone is being collected. If the map and the value
// were in different zones, then we could have a case where the map zone is not
// collecting but the value zone is, and incorrectly free a value that is
// reachable solely through weakmaps.
template <class K, class V>
void WeakMap<K, V>::assertMapIsSameZoneWithValue(const BarrieredValue& v) {
#ifdef DEBUG
  gc::Cell* cell = gc::ToMarkable(v);
  if (cell) {
    Zone* cellZone = cell->zoneFromAnyThread();
    MOZ_ASSERT(zone() == cellZone || cellZone->isAtomsZone());
  }
#endif
}

template <class K, class V>
WeakMap<K, V>::WeakMap(JSContext* cx, JSObject* memOf)
    : WeakMap(cx->zone(), memOf) {}

template <class K, class V>
WeakMap<K, V>::WeakMap(JS::Zone* zone, JSObject* memOf)
    : WeakMapBase(memOf, zone), map_(zone) {
  static_assert(std::is_same_v<typename RemoveBarrier<K>::Type, K>);
  static_assert(std::is_same_v<typename RemoveBarrier<V>::Type, V>);

  // The object's TraceKind needs to be added to CC graph if this object is
  // used as a WeakMap key, otherwise the key is considered to be pointed from
  // somewhere unknown, and results in leaking the subgraph which contains the
  // key. See the comments in NoteWeakMapsTracer::trace for more details.
  if constexpr (std::is_pointer_v<K>) {
    using NonPtrType = std::remove_pointer_t<K>;
    static_assert(JS::IsCCTraceKind(NonPtrType::TraceKind),
                  "Object's TraceKind should be added to CC graph.");
  }

  zone->gcWeakMapList().insertFront(this);
  if (zone->gcState() > Zone::Prepare) {
    setMapColor(CellColor::Black);
  }
}

template <class K, class V>
WeakMap<K, V>::~WeakMap() {
#ifdef DEBUG
  // Weak maps store their data in an unbarriered map (|map_|) meaning that no
  // barriers are run on destruction. This is safe because:

  // 1. Weak maps have GC lifetime except on construction failure, therefore no
  // prebarrier is required.
  MOZ_ASSERT_IF(!empty(),
                CurrentThreadIsGCSweeping() || CurrentThreadIsGCFinalizing());

  // 2. If we're finalizing a weak map due to GC then it cannot contain nursery
  // things, because we evicted the nursery at the start of collection and
  // writing a nursery thing into the table would require the map to be
  // live. Therefore no postbarrier is required.
  size_t i = 0;
  for (auto r = all(); !r.empty() && i < 1000; r.popFront(), i++) {
    K key = r.front().key();
    MOZ_ASSERT_IF(gc::ToMarkable(key), !IsInsideNursery(gc::ToMarkable(key)));
    V value = r.front().value();
    MOZ_ASSERT_IF(gc::ToMarkable(value),
                  !IsInsideNursery(gc::ToMarkable(value)));
  }
#endif
}

// If the entry is live, ensure its key and value are marked. Also make sure the
// key is at least as marked as min(map, delegate), so it cannot get discarded
// and then recreated by rewrapping the delegate.
//
// Optionally adds edges to the ephemeron edges table for any keys (or
// delegates) where future changes to their mark color would require marking the
// value (or the key).
template <class K, class V>
bool WeakMap<K, V>::markEntry(GCMarker* marker, gc::CellColor mapColor,
                              BarrieredKey& key, BarrieredValue& value,
                              bool populateWeakKeysTable) {
#ifdef DEBUG
  MOZ_ASSERT(IsMarked(mapColor));
  if (marker->isParallelMarking()) {
    marker->runtime()->gc.assertCurrentThreadHasLockedGC();
  }
#endif

  bool marked = false;
  CellColor markColor = AsCellColor(marker->markColor());
  CellColor keyColor = gc::detail::GetEffectiveColor(marker, key);
  JSObject* delegate = gc::detail::GetDelegate(key);
  JSTracer* trc = marker->tracer();

  gc::Cell* keyCell = gc::ToMarkable(key);
  MOZ_ASSERT(keyCell);

  if (delegate) {
    CellColor delegateColor = gc::detail::GetEffectiveColor(marker, delegate);
    // The key needs to stay alive while both the delegate and map are live.
    CellColor proxyPreserveColor = std::min(delegateColor, mapColor);
    if (keyColor < proxyPreserveColor) {
      MOZ_ASSERT(markColor >= proxyPreserveColor);
      if (markColor == proxyPreserveColor) {
        TraceWeakMapKeyEdge(trc, zone(), &key,
                            "proxy-preserved WeakMap entry key");
        MOZ_ASSERT(keyCell->color() >= proxyPreserveColor);
        marked = true;
        keyColor = proxyPreserveColor;
      }
    }
  }

  gc::Cell* cellValue = gc::ToMarkable(value);
  if (IsMarked(keyColor)) {
    if (cellValue) {
      CellColor targetColor = std::min(mapColor, keyColor);
      CellColor valueColor = gc::detail::GetEffectiveColor(marker, cellValue);
      if (valueColor < targetColor) {
        MOZ_ASSERT(markColor >= targetColor);
        if (markColor == targetColor) {
          TraceEdge(trc, &value, "WeakMap entry value");
          MOZ_ASSERT(cellValue->color() >= targetColor);
          marked = true;
        }
      }
    }
  }

  if (populateWeakKeysTable) {
    MOZ_ASSERT(trc->weakMapAction() == JS::WeakMapTraceAction::Expand);

    // Note that delegateColor >= keyColor because marking a key marks its
    // delegate, so we only need to check whether keyColor < mapColor to tell
    // this.
    if (keyColor < mapColor) {
      // The final color of the key is not yet known. Add an edge to the
      // relevant ephemerons table to ensure that the value will be marked if
      // the key is marked. If the key has a delegate, also add an edge to
      // ensure the key is marked if the delegate is marked.

      gc::TenuredCell* tenuredValue = nullptr;
      if (cellValue && cellValue->isTenured()) {
        tenuredValue = &cellValue->asTenured();
      }

      if (!this->addEphemeronEdgesForEntry(AsMarkColor(mapColor), keyCell,
                                           delegate, tenuredValue)) {
        marker->abortLinearWeakMarking();
      }
    }
  }

  return marked;
}

template <class K, class V>
void WeakMap<K, V>::trace(JSTracer* trc) {
  MOZ_ASSERT(isInList());

  TraceNullableEdge(trc, &memberOf, "WeakMap owner");

  if (trc->isMarkingTracer()) {
    MOZ_ASSERT(trc->weakMapAction() == JS::WeakMapTraceAction::Expand);
    GCMarker* marker = GCMarker::fromTracer(trc);
    if (markMap(marker->markColor())) {
      (void)markEntries(marker);
    }
    return;
  }

  if (trc->weakMapAction() == JS::WeakMapTraceAction::Skip) {
    return;
  }

  // Trace keys only if weakMapAction() says to.
  if (trc->weakMapAction() == JS::WeakMapTraceAction::TraceKeysAndValues) {
    for (Enum e(*this); !e.empty(); e.popFront()) {
      TraceWeakMapKeyEdge(trc, zone(), &e.front().mutableKey(),
                          "WeakMap entry key");
    }
  }

  // Always trace all values (unless weakMapAction() is Skip).
  for (Range r = all(); !r.empty(); r.popFront()) {
    TraceEdge(trc, &r.front().value(), "WeakMap entry value");
  }
}

template <class K, class V>
bool WeakMap<K, V>::markEntries(GCMarker* marker) {
  // This method is called whenever the map's mark color changes. Mark values
  // (and keys with delegates) as required for the new color and populate the
  // ephemeron edges if we're in incremental marking mode.

  // Lock during parallel marking to synchronize updates to the ephemeron edges
  // table.
  mozilla::Maybe<AutoLockGC> lock;
  if (marker->isParallelMarking()) {
    lock.emplace(marker->runtime());
  }

  MOZ_ASSERT(IsMarked(mapColor()));
  bool markedAny = false;

  // If we don't populate the weak keys table now then we do it when we enter
  // weak marking mode.
  bool populateWeakKeysTable =
      marker->incrementalWeakMapMarkingEnabled || marker->isWeakMarking();

  // Read the atomic color into a local variable so the compiler doesn't load it
  // every time.
  gc::CellColor mapColor = this->mapColor();

  for (Enum e(*this); !e.empty(); e.popFront()) {
    if (markEntry(marker, mapColor, e.front().mutableKey(), e.front().value(),
                  populateWeakKeysTable)) {
      markedAny = true;
    }
  }

  return markedAny;
}

template <class K, class V>
void WeakMap<K, V>::traceWeakEdges(JSTracer* trc) {
  // Scan the map, removing all entries whose keys remain unmarked. Rebuild
  // cached key state at the same time.
  mayHaveSymbolKeys = false;
  mayHaveKeyDelegates = false;
  for (Enum e(*this); !e.empty(); e.popFront()) {
    if (TraceWeakEdge(trc, &e.front().mutableKey(), "WeakMap key")) {
      keyWriteBarrier(e.front().key());
    } else {
      e.removeFront();
    }
  }

#if DEBUG
  // Once we've swept, all remaining edges should stay within the known-live
  // part of the graph.
  assertEntriesNotAboutToBeFinalized();
#endif
}

// memberOf can be nullptr, which means that the map is not part of a JSObject.
template <class K, class V>
void WeakMap<K, V>::traceMappings(WeakMapTracer* tracer) {
  for (Range r = all(); !r.empty(); r.popFront()) {
    gc::Cell* key = gc::ToMarkable(r.front().key());
    gc::Cell* value = gc::ToMarkable(r.front().value());
    if (key && value) {
      tracer->trace(memberOf, JS::GCCellPtr(r.front().key().get()),
                    JS::GCCellPtr(r.front().value().get()));
    }
  }
}

template <class K, class V>
bool WeakMap<K, V>::findSweepGroupEdges(Zone* atomsZone) {
  // For weakmap keys with delegates in a different zone, add a zone edge to
  // ensure that the delegate zone finishes marking before the key zone.

#ifdef DEBUG
  if (!mayHaveSymbolKeys || !mayHaveKeyDelegates) {
    for (Range r = all(); !r.empty(); r.popFront()) {
      const K& key = r.front().key();
      MOZ_ASSERT_IF(!mayHaveKeyDelegates, !gc::detail::GetDelegate(key));
      MOZ_ASSERT_IF(!mayHaveSymbolKeys, !gc::detail::IsSymbol(key));
    }
  }
#endif

#ifdef NIGHTLY_BUILD
  if (mayHaveSymbolKeys) {
    MOZ_ASSERT(JS::Prefs::experimental_symbols_as_weakmap_keys());
    if (atomsZone->isGCMarking()) {
      if (!atomsZone->addSweepGroupEdgeTo(zone())) {
        return false;
      }
    }
  }
#endif

  if (mayHaveKeyDelegates) {
    for (Range r = all(); !r.empty(); r.popFront()) {
      const K& key = r.front().key();

      JSObject* delegate = gc::detail::GetDelegate(key);
      if (delegate) {
        // Marking a WeakMap key's delegate will mark the key, so process the
        // delegate zone no later than the key zone.
        Zone* delegateZone = delegate->zone();
        gc::Cell* keyCell = gc::ToMarkable(key);
        MOZ_ASSERT(keyCell);
        Zone* keyZone = keyCell->zone();
        if (delegateZone != keyZone && delegateZone->isGCMarking() &&
            keyZone->isGCMarking()) {
          if (!delegateZone->addSweepGroupEdgeTo(keyZone)) {
            return false;
          }
        }
      }
    }
  }

  return true;
}

template <class K, class V>
size_t WeakMap<K, V>::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  return mallocSizeOf(this) + shallowSizeOfExcludingThis(mallocSizeOf);
}

#if DEBUG
template <class K, class V>
void WeakMap<K, V>::assertEntriesNotAboutToBeFinalized() {
  for (Range r = all(); !r.empty(); r.popFront()) {
    K k = r.front().key();
    MOZ_ASSERT(!gc::IsAboutToBeFinalizedUnbarriered(k));
    JSObject* delegate = gc::detail::GetDelegate(k);
    if (delegate) {
      MOZ_ASSERT(!gc::IsAboutToBeFinalizedUnbarriered(delegate),
                 "weakmap marking depends on a key tracing its delegate");
    }
    MOZ_ASSERT(!gc::IsAboutToBeFinalized(r.front().value()));
  }
}
#endif

#ifdef JS_GC_ZEAL
template <class K, class V>
bool WeakMap<K, V>::checkMarking() const {
  bool ok = true;
  for (Range r = all(); !r.empty(); r.popFront()) {
    gc::Cell* key = gc::ToMarkable(r.front().key());
    gc::Cell* value = gc::ToMarkable(r.front().value());
    if (key && value) {
      if (!gc::CheckWeakMapEntryMarking(this, key, value)) {
        ok = false;
      }
    }
  }
  return ok;
}
#endif

#ifdef JSGC_HASH_TABLE_CHECKS
template <class K, class V>
void WeakMap<K, V>::checkAfterMovingGC() const {
  for (Range r = all(); !r.empty(); r.popFront()) {
    gc::Cell* key = gc::ToMarkable(r.front().key());
    gc::Cell* value = gc::ToMarkable(r.front().value());
    CheckGCThingAfterMovingGC(key);
    if (!allowKeysInOtherZones()) {
      Zone* keyZone = key->zoneFromAnyThread();
      MOZ_RELEASE_ASSERT(keyZone == zone() || keyZone->isAtomsZone());
    }
    CheckGCThingAfterMovingGC(value, zone());
    auto ptr = lookupUnbarriered(r.front().key());
    MOZ_RELEASE_ASSERT(ptr.found() && &*ptr == &r.front());
  }
}
#endif  // JSGC_HASH_TABLE_CHECKS

inline HashNumber GetSymbolHash(JS::Symbol* sym) { return sym->hash(); }

}  // namespace js

#endif /* gc_WeakMap_inl_h */
