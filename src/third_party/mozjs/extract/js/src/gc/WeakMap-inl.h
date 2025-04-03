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
#include "js/TraceKind.h"
#include "vm/JSContext.h"

#include "gc/StableCellHasher-inl.h"

namespace js {
namespace gc {

namespace detail {

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

// Only objects have delegates, so default to returning nullptr. Note that some
// compilation units will only ever use the object version.
static MOZ_MAYBE_UNUSED JSObject* GetDelegateInternal(gc::Cell* key) {
  return nullptr;
}

static MOZ_MAYBE_UNUSED JSObject* GetDelegateInternal(JSObject* key) {
  JSObject* delegate = UncheckedUnwrapWithoutExpose(key);
  return (key == delegate) ? nullptr : delegate;
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

} /* namespace detail */
} /* namespace gc */

// Weakmap entry -> value edges are only visible if the map is traced, which
// only happens if the map zone is being collected. If the map and the value
// were in different zones, then we could have a case where the map zone is not
// collecting but the value zone is, and incorrectly free a value that is
// reachable solely through weakmaps.
template <class K, class V>
void WeakMap<K, V>::assertMapIsSameZoneWithValue(const V& v) {
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
    : Base(zone), WeakMapBase(memOf, zone) {
  using ElemType = typename K::ElementType;
  using NonPtrType = std::remove_pointer_t<ElemType>;

  // The object's TraceKind needs to be added to CC graph if this object is
  // used as a WeakMap key, otherwise the key is considered to be pointed from
  // somewhere unknown, and results in leaking the subgraph which contains the
  // key. See the comments in NoteWeakMapsTracer::trace for more details.
  static_assert(JS::IsCCTraceKind(NonPtrType::TraceKind),
                "Object's TraceKind should be added to CC graph.");

  zone->gcWeakMapList().insertFront(this);
  if (zone->gcState() > Zone::Prepare) {
    mapColor = CellColor::Black;
  }
}

// If the entry is live, ensure its key and value are marked. Also make sure the
// key is at least as marked as min(map, delegate), so it cannot get discarded
// and then recreated by rewrapping the delegate.
//
// Optionally adds edges to the ephemeron edges table for any keys (or
// delegates) where future changes to their mark color would require marking the
// value (or the key).
template <class K, class V>
bool WeakMap<K, V>::markEntry(GCMarker* marker, K& key, V& value,
                              bool populateWeakKeysTable) {
#ifdef DEBUG
  MOZ_ASSERT(mapColor);
  if (marker->isParallelMarking()) {
    marker->runtime()->gc.assertCurrentThreadHasLockedGC();
  }
#endif

  bool marked = false;
  CellColor markColor = marker->markColor();
  CellColor keyColor = gc::detail::GetEffectiveColor(marker, key);
  JSObject* delegate = gc::detail::GetDelegate(key);
  JSTracer* trc = marker->tracer();

  if (delegate) {
    CellColor delegateColor = gc::detail::GetEffectiveColor(marker, delegate);
    // The key needs to stay alive while both the delegate and map are live.
    CellColor proxyPreserveColor = std::min(delegateColor, mapColor);
    if (keyColor < proxyPreserveColor) {
      MOZ_ASSERT(markColor >= proxyPreserveColor);
      if (markColor == proxyPreserveColor) {
        TraceWeakMapKeyEdge(trc, zone(), &key,
                            "proxy-preserved WeakMap entry key");
        MOZ_ASSERT(key->color() >= proxyPreserveColor);
        marked = true;
        keyColor = proxyPreserveColor;
      }
    }
  }

  gc::Cell* cellValue = gc::ToMarkable(value);
  if (keyColor) {
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
    // Note that delegateColor >= keyColor because marking a key marks its
    // delegate, so we only need to check whether keyColor < mapColor to tell
    // this.

    if (keyColor < mapColor) {
      MOZ_ASSERT(trc->weakMapAction() == JS::WeakMapTraceAction::Expand);
      // The final color of the key is not yet known. Record this weakmap and
      // the lookup key in the list of weak keys. If the key has a delegate,
      // then the lookup key is the delegate (because marking the key will end
      // up marking the delegate and thereby mark the entry.)
      gc::TenuredCell* tenuredValue = nullptr;
      if (cellValue && cellValue->isTenured()) {
        tenuredValue = &cellValue->asTenured();
      }

      if (!this->addImplicitEdges(key, delegate, tenuredValue)) {
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
    auto marker = GCMarker::fromTracer(trc);

    // Lock if we are marking in parallel to synchronize updates to:
    //  - the weak map's color
    //  - the ephemeron edges table
    mozilla::Maybe<AutoLockGC> lock;
    if (marker->isParallelMarking()) {
      lock.emplace(marker->runtime());
    }

    // Don't downgrade the map color from black to gray. This can happen when a
    // barrier pushes the map object onto the black mark stack when it's
    // already present on the gray mark stack, which is marked later.
    if (mapColor < marker->markColor()) {
      mapColor = marker->markColor();
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
  for (Range r = Base::all(); !r.empty(); r.popFront()) {
    TraceEdge(trc, &r.front().value(), "WeakMap entry value");
  }
}

bool WeakMapBase::addImplicitEdges(gc::Cell* key, gc::Cell* delegate,
                                   gc::TenuredCell* value) {
  if (delegate) {
    auto& edgeTable = delegate->zone()->gcEphemeronEdges(delegate);
    auto* p = edgeTable.get(delegate);

    gc::EphemeronEdgeVector newVector;
    gc::EphemeronEdgeVector& edges = p ? p->value : newVector;

    // Add a <weakmap, delegate> -> key edge: the key must be preserved for
    // future lookups until either the weakmap or the delegate dies.
    gc::EphemeronEdge keyEdge{mapColor, key};
    if (!edges.append(keyEdge)) {
      return false;
    }

    if (value) {
      gc::EphemeronEdge valueEdge{mapColor, value};
      if (!edges.append(valueEdge)) {
        return false;
      }
    }

    if (!p) {
      return edgeTable.put(delegate, std::move(newVector));
    }

    return true;
  }

  // No delegate. Insert just the key -> value edge.

  if (!value) {
    return true;
  }

  auto& edgeTable = key->zone()->gcEphemeronEdges(key);
  auto* p = edgeTable.get(key);
  gc::EphemeronEdge valueEdge{mapColor, value};
  if (p) {
    return p->value.append(valueEdge);
  } else {
    gc::EphemeronEdgeVector edges;
    MOZ_ALWAYS_TRUE(edges.append(valueEdge));
    return edgeTable.put(key, std::move(edges));
  }
}

template <class K, class V>
bool WeakMap<K, V>::markEntries(GCMarker* marker) {
  // This method is called whenever the map's mark color changes. Mark values
  // (and keys with delegates) as required for the new color and populate the
  // ephemeron edges if we're in incremental marking mode.

#ifdef DEBUG
  if (marker->isParallelMarking()) {
    marker->runtime()->gc.assertCurrentThreadHasLockedGC();
  }
#endif

  MOZ_ASSERT(mapColor);
  bool markedAny = false;

  // If we don't populate the weak keys table now then we do it when we enter
  // weak marking mode.
  bool populateWeakKeysTable =
      marker->incrementalWeakMapMarkingEnabled || marker->isWeakMarking();

  for (Enum e(*this); !e.empty(); e.popFront()) {
    if (markEntry(marker, e.front().mutableKey(), e.front().value(),
                  populateWeakKeysTable)) {
      markedAny = true;
    }
  }

  return markedAny;
}

template <class K, class V>
void WeakMap<K, V>::traceWeakEdges(JSTracer* trc) {
  // Remove all entries whose keys remain unmarked.
  for (Enum e(*this); !e.empty(); e.popFront()) {
    if (!TraceWeakEdge(trc, &e.front().mutableKey(), "WeakMap key")) {
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
  for (Range r = Base::all(); !r.empty(); r.popFront()) {
    gc::Cell* key = gc::ToMarkable(r.front().key());
    gc::Cell* value = gc::ToMarkable(r.front().value());
    if (key && value) {
      tracer->trace(memberOf, JS::GCCellPtr(r.front().key().get()),
                    JS::GCCellPtr(r.front().value().get()));
    }
  }
}

template <class K, class V>
bool WeakMap<K, V>::findSweepGroupEdges() {
  // For weakmap keys with delegates in a different zone, add a zone edge to
  // ensure that the delegate zone finishes marking before the key zone.
  JS::AutoSuppressGCAnalysis nogc;
  for (Range r = all(); !r.empty(); r.popFront()) {
    const K& key = r.front().key();

    // If the key type doesn't have delegates, then this will always return
    // nullptr and the optimizer can remove the entire body of this function.
    JSObject* delegate = gc::detail::GetDelegate(key);
    if (!delegate) {
      continue;
    }

    // Marking a WeakMap key's delegate will mark the key, so process the
    // delegate zone no later than the key zone.
    Zone* delegateZone = delegate->zone();
    Zone* keyZone = key->zone();
    if (delegateZone != keyZone && delegateZone->isGCMarking() &&
        keyZone->isGCMarking()) {
      if (!delegateZone->addSweepGroupEdgeTo(keyZone)) {
        return false;
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
  for (Range r = Base::all(); !r.empty(); r.popFront()) {
    UnbarrieredKey k = r.front().key();
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
  for (Range r = Base::all(); !r.empty(); r.popFront()) {
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

} /* namespace js */

#endif /* gc_WeakMap_inl_h */
