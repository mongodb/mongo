/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_WeakMap_inl_h
#define gc_WeakMap_inl_h

#include "gc/WeakMap.h"

#include "mozilla/DebugOnly.h"

#include <algorithm>
#include <type_traits>

#include "gc/Zone.h"
#include "js/TraceKind.h"
#include "vm/JSContext.h"

namespace js {
namespace gc {

namespace detail {

// Return the effective cell color given the current marking state.
// This must be kept in sync with ShouldMark in Marking.cpp.
template <typename T>
static CellColor GetEffectiveColor(JSRuntime* rt, const T& item) {
  Cell* cell = ToMarkable(item);
  if (!cell->isTenured()) {
    return CellColor::Black;
  }
  const TenuredCell& t = cell->asTenured();
  if (rt != t.runtimeFromAnyThread()) {
    return CellColor::Black;
  }
  if (!t.zoneFromAnyThread()->shouldMarkInZone()) {
    return CellColor::Black;
  }
  return cell->color();
}

// Only objects have delegates, so default to returning nullptr. Note that some
// compilation units will only ever use the object version.
static MOZ_MAYBE_UNUSED JSObject* GetDelegateInternal(gc::Cell* key) {
  return nullptr;
}

static JSObject* GetDelegateInternal(JSObject* key) {
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
    : Base(cx->zone()), WeakMapBase(memOf, cx->zone()) {
  using ElemType = typename K::ElementType;
  using NonPtrType = std::remove_pointer_t<ElemType>;

  // The object's TraceKind needs to be added to CC graph if this object is
  // used as a WeakMap key, otherwise the key is considered to be pointed from
  // somewhere unknown, and results in leaking the subgraph which contains the
  // key. See the comments in NoteWeakMapsTracer::trace for more details.
  static_assert(JS::IsCCTraceKind(NonPtrType::TraceKind),
                "Object's TraceKind should be added to CC graph.");

  zone()->gcWeakMapList().insertFront(this);
  if (zone()->gcState() > Zone::Prepare) {
    mapColor = CellColor::Black;
  }
}

// Trace a WeakMap entry based on 'markedCell' getting marked, where 'origKey'
// is the key in the weakmap. In the absence of delegates, these will be the
// same, but when a delegate is marked then origKey will be its wrapper.
// `markedCell` is only used for an assertion.
template <class K, class V>
void WeakMap<K, V>::markKey(GCMarker* marker, gc::Cell* markedCell,
                            gc::Cell* origKey) {
#if DEBUG
  if (!mapColor) {
    fprintf(stderr, "markKey called on an unmarked map %p", this);
    Zone* zone = markedCell->asTenured().zoneFromAnyThread();
    fprintf(stderr, "  markedCell=%p from zone %p state %d mark %d\n",
            markedCell, zone, zone->gcState(),
            int(debug::GetMarkInfo(markedCell)));
    zone = origKey->asTenured().zoneFromAnyThread();
    fprintf(stderr, "  origKey=%p from zone %p state %d mark %d\n", origKey,
            zone, zone->gcState(), int(debug::GetMarkInfo(markedCell)));
    if (memberOf) {
      zone = memberOf->asTenured().zoneFromAnyThread();
      fprintf(stderr, "  memberOf=%p from zone %p state %d mark %d\n",
              memberOf.get(), zone, zone->gcState(),
              int(debug::GetMarkInfo(memberOf.get())));
    }
  }
#endif
  MOZ_ASSERT(mapColor);

  Ptr p = Base::lookup(static_cast<Lookup>(origKey));
  // We should only be processing <weakmap,key> pairs where the key exists in
  // the weakmap. Such pairs are inserted when a weakmap is marked, and are
  // removed by barriers if the key is removed from the weakmap. Failure here
  // probably means gcEphemeronEdges is not being properly traced during a minor
  // GC, or the weakmap keys are not being updated when tenured.
  MOZ_ASSERT(p.found());

  mozilla::DebugOnly<gc::Cell*> oldKey = gc::ToMarkable(p->key());
  MOZ_ASSERT((markedCell == oldKey) ||
             (markedCell == gc::detail::GetDelegate(p->key())));

  markEntry(marker, p->mutableKey(), p->value());
  MOZ_ASSERT(oldKey == gc::ToMarkable(p->key()), "no moving GC");
}

// If the entry is live, ensure its key and value are marked. Also make sure
// the key is at least as marked as the delegate, so it cannot get discarded
// and then recreated by rewrapping the delegate.
template <class K, class V>
bool WeakMap<K, V>::markEntry(GCMarker* marker, K& key, V& value) {
  bool marked = false;
  JSRuntime* rt = zone()->runtimeFromAnyThread();
  CellColor keyColor = gc::detail::GetEffectiveColor(rt, key);
  JSObject* delegate = gc::detail::GetDelegate(key);

  if (delegate) {
    CellColor delegateColor = gc::detail::GetEffectiveColor(rt, delegate);
    MOZ_ASSERT(mapColor);
    // The delegate color should propagate to the key, assuming the map is
    // potentially alive at all (its color doesn't matter).
    if (keyColor < delegateColor) {
      gc::AutoSetMarkColor autoColor(*marker, delegateColor);
      TraceWeakMapKeyEdge(marker, zone(), &key,
                          "proxy-preserved WeakMap entry key");
      MOZ_ASSERT(key->color() >= delegateColor);
      marked = true;
      keyColor = delegateColor;
    }
  }

  if (keyColor) {
    gc::Cell* cellValue = gc::ToMarkable(value);
    if (cellValue) {
      gc::AutoSetMarkColor autoColor(*marker, std::min(mapColor, keyColor));
      CellColor valueColor = gc::detail::GetEffectiveColor(rt, cellValue);
      if (valueColor < marker->markColor()) {
        TraceEdge(marker, &value, "WeakMap entry value");
        MOZ_ASSERT(cellValue->color() >= std::min(mapColor, keyColor));
        marked = true;
      }
    }
  }

  return marked;
}

template <class K, class V>
void WeakMap<K, V>::trace(JSTracer* trc) {
  MOZ_ASSERT_IF(JS::RuntimeHeapIsBusy(), isInList());

  TraceNullableEdge(trc, &memberOf, "WeakMap owner");

  if (trc->isMarkingTracer()) {
    MOZ_ASSERT(trc->weakMapAction() == JS::WeakMapTraceAction::Expand);
    auto marker = GCMarker::fromTracer(trc);

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

    // Add a delegate -> key edge, where the key is marked the color of the
    // delegate no matter what the weakmap's mark color is. This is implemented
    // as if it were a regular ephemeron <weakmap, key> -> value edge, where
    // the value color is the minimum of the two source colors, but in place of
    // the weakmap we use the constant color Black, which results in the
    // delegate's color propagating unchanged.
    gc::EphemeronEdge keyEdge{CellColor::Black, key};
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
  MOZ_ASSERT(mapColor);
  bool markedAny = false;

  for (Enum e(*this); !e.empty(); e.popFront()) {
    if (markEntry(marker, e.front().mutableKey(), e.front().value())) {
      markedAny = true;
    }
    if (!marker->incrementalWeakMapMarkingEnabled && !marker->isWeakMarking()) {
      // Populate weak keys table when we enter weak marking mode.
      continue;
    }

    JSRuntime* rt = zone()->runtimeFromAnyThread();
    CellColor keyColor =
        gc::detail::GetEffectiveColor(rt, e.front().key().get());

    // Changes in the map's mark color will be handled in this code, but
    // changes in the key's mark color are handled through the weak keys table.
    // So we only need to populate the table if the key is less marked than the
    // map, to catch later updates in the key's mark color.
    if (keyColor < mapColor) {
      MOZ_ASSERT(marker->weakMapAction() == JS::WeakMapTraceAction::Expand);
      // The final color of the key is not yet known. Record this weakmap and
      // the lookup key in the list of weak keys. If the key has a delegate,
      // then the lookup key is the delegate (because marking the key will end
      // up marking the delegate and thereby mark the entry.)
      gc::Cell* weakKey = e.front().key();
      gc::Cell* value = gc::ToMarkable(e.front().value());
      gc::Cell* delegate = gc::detail::GetDelegate(e.front().key());

      gc::TenuredCell* tenuredValue = nullptr;
      if (value) {
        if (value->isTenured()) {
          tenuredValue = &value->asTenured();
        } else {
          // The nursery is collected at the beginning of an incremental GC. If
          // the value is in the nursery, we know it was allocated after the GC
          // started and sometime later was inserted into the map, which should
          // be a fairly rare case. To avoid needing to sweep through the
          // ephemeron edge tables on a minor GC, just mark the value
          // immediately.
          TraceEdge(marker, &e.front().value(), "WeakMap entry value");
        }
      }

      if (!addImplicitEdges(weakKey, delegate, tenuredValue)) {
        marker->abortLinearWeakMarking();
      }
    }
  }

  return markedAny;
}

template <class K, class V>
void WeakMap<K, V>::sweep() {
  /* Remove all entries whose keys remain unmarked. */
  for (Enum e(*this); !e.empty(); e.popFront()) {
    if (gc::IsAboutToBeFinalized(&e.front().mutableKey())) {
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

#if DEBUG
template <class K, class V>
void WeakMap<K, V>::assertEntriesNotAboutToBeFinalized() {
  for (Range r = Base::all(); !r.empty(); r.popFront()) {
    UnbarrieredKey k = r.front().key();
    MOZ_ASSERT(!gc::IsAboutToBeFinalizedUnbarriered(&k));
    JSObject* delegate = gc::detail::GetDelegate(k);
    if (delegate) {
      MOZ_ASSERT(!gc::IsAboutToBeFinalizedUnbarriered(&delegate),
                 "weakmap marking depends on a key tracing its delegate");
    }
    MOZ_ASSERT(!gc::IsAboutToBeFinalized(&r.front().value()));
    MOZ_ASSERT(k == r.front().key());
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
