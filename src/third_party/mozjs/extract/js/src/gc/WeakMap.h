/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_WeakMap_h
#define gc_WeakMap_h

#include "mozilla/Atomics.h"
#include "mozilla/LinkedList.h"

#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "gc/Tracer.h"
#include "gc/ZoneAllocator.h"
#include "js/HashTable.h"
#include "js/HeapAPI.h"
#include "vm/JSObject.h"

namespace JS {
class Zone;
}

namespace js {

class GCMarker;
class WeakMapBase;
struct WeakMapTracer;

extern void DumpWeakMapLog(JSRuntime* rt);

namespace gc {

struct WeakMarkable;

#if defined(JS_GC_ZEAL) || defined(DEBUG)
// Check whether a weak map entry is marked correctly.
bool CheckWeakMapEntryMarking(const WeakMapBase* map, Cell* key, Cell* value);
#endif

}  // namespace gc

// A subclass template of js::HashMap whose keys and values may be
// garbage-collected. When a key is collected, the table entry disappears,
// dropping its reference to the value.
//
// More precisely:
//
//     A WeakMap entry is live if and only if both the WeakMap and the entry's
//     key are live. An entry holds a strong reference to its value.
//
// You must call this table's 'trace' method when its owning object is reached
// by the garbage collection tracer. Once a table is known to be live, the
// implementation takes care of the special weak marking (ie, marking through
// the implicit edges stored in the map) and of removing (sweeping) table
// entries when collection is complete.

// WeakMaps are marked with an incremental linear-time algorithm that handles
// all orderings of map and key marking. The basic algorithm is:
//
// At first while marking, do nothing special when marking WeakMap keys (there
// is no straightforward way to know whether a particular object is being used
// as a key in some weakmap.) When a WeakMap is marked, scan through it to mark
// all entries with live keys, and collect all unmarked keys into a "weak keys"
// table.
//
// At some point, everything reachable has been marked. At this point, enter
// "weak marking mode". In this mode, whenever any object is marked, look it up
// in the weak keys table to see if it is the key for any WeakMap entry and if
// so, mark the value. When entering weak marking mode, scan the weak key table
// to find all keys that have been marked since we added them to the table, and
// mark those entries.
//
// In addition, we want weakmap marking to work incrementally. So WeakMap
// mutations are barriered to keep the weak keys table up to date: entries are
// removed if their key is removed from the table, etc.
//
// You can break down various ways that WeakMap values get marked based on the
// order that the map and key are marked. All of these assume the map and key
// get marked at some point:
//
//   key marked, then map marked:
//    - value was marked with map in `markEntries()`
//   map marked, key already in map, key marked before weak marking mode:
//    - key added to gcEphemeronEdges when map marked in `markEntries()`
//    - value marked during `enterWeakMarkingMode`
//   map marked, key already in map, key marked after weak marking mode:
//    - when key is marked, gcEphemeronEdges[key] triggers marking of value in
//      `markImplicitEdges()`
//   map marked, key inserted into map, key marked:
//    - value was live when inserted and must get marked at some point
//

using WeakMapColors = HashMap<WeakMapBase*, js::gc::CellColor,
                              DefaultHasher<WeakMapBase*>, SystemAllocPolicy>;

// Common base class for all WeakMap specializations, used for calling
// subclasses' GC-related methods.
class WeakMapBase : public mozilla::LinkedListElement<WeakMapBase> {
  friend class js::GCMarker;

 public:
  using CellColor = js::gc::CellColor;

  WeakMapBase(JSObject* memOf, JS::Zone* zone);
  virtual ~WeakMapBase() {}

  JS::Zone* zone() const { return zone_; }

  // Garbage collector entry points.

  // Unmark all weak maps in a zone.
  static void unmarkZone(JS::Zone* zone);

  // Check all weak maps in a zone that have been marked as live in this garbage
  // collection, and mark the values of all entries that have become strong
  // references to them. Return true if we marked any new values, indicating
  // that we need to make another pass. In other words, mark my marked maps'
  // marked members' mid-collection.
  static bool markZoneIteratively(JS::Zone* zone, GCMarker* marker);

  // Add zone edges for weakmaps in zone |mapZone| with key delegates in a
  // different zone.
  [[nodiscard]] static bool findSweepGroupEdgesForZone(JS::Zone* atomsZone,
                                                       JS::Zone* mapZone);

  // Sweep the marked weak maps in a zone, updating moved keys.
  static void sweepZoneAfterMinorGC(JS::Zone* zone);

  // Trace all weak map bindings. Used by the cycle collector.
  static void traceAllMappings(WeakMapTracer* tracer);

  // Save information about which weak maps are marked for a zone.
  static bool saveZoneMarkedWeakMaps(JS::Zone* zone,
                                     WeakMapColors& markedWeakMaps);

  // Restore information about which weak maps are marked for many zones.
  static void restoreMarkedWeakMaps(WeakMapColors& markedWeakMaps);

#if defined(JS_GC_ZEAL) || defined(DEBUG)
  static bool checkMarkingForZone(JS::Zone* zone);
#endif

#ifdef JSGC_HASH_TABLE_CHECKS
  static void checkWeakMapsAfterMovingGC(JS::Zone* zone);
#endif

 protected:
  // Instance member functions called by the above. Instantiations of WeakMap
  // override these with definitions appropriate for their Key and Value types.
  virtual void trace(JSTracer* tracer) = 0;
  virtual bool findSweepGroupEdges(Zone* atomsZone) = 0;
  virtual void traceWeakEdges(JSTracer* trc) = 0;
  virtual void traceMappings(WeakMapTracer* tracer) = 0;
  virtual void clearAndCompact() = 0;

  // We have a key that, if it or its delegate is marked, may lead to a WeakMap
  // value getting marked. Insert the necessary edges into the appropriate
  // zone's gcEphemeronEdges or gcNurseryEphemeronEdges tables.
  [[nodiscard]] bool addEphemeronEdgesForEntry(gc::MarkColor mapColor,
                                               gc::Cell* key,
                                               gc::Cell* delegate,
                                               gc::TenuredCell* value);
  [[nodiscard]] bool addEphemeronEdge(gc::MarkColor color, gc::Cell* src,
                                      gc::Cell* dst);

  virtual bool markEntries(GCMarker* marker) = 0;

  gc::CellColor mapColor() const { return gc::CellColor(uint32_t(mapColor_)); }
  void setMapColor(gc::CellColor newColor) { mapColor_ = uint32_t(newColor); }
  bool markMap(gc::MarkColor markColor);

#ifdef JS_GC_ZEAL
  virtual bool checkMarking() const = 0;
  virtual bool allowKeysInOtherZones() const { return false; }
  friend bool gc::CheckWeakMapEntryMarking(const WeakMapBase*, gc::Cell*,
                                           gc::Cell*);
#endif

#ifdef JSGC_HASH_TABLE_CHECKS
  virtual void checkAfterMovingGC() const = 0;
#endif

  // Object that this weak map is part of, if any.
  HeapPtr<JSObject*> memberOf;

  // Zone containing this weak map.
  JS::Zone* zone_;

  // Whether this object has been marked during garbage collection and which
  // color it was marked.
  mozilla::Atomic<uint32_t, mozilla::Relaxed> mapColor_;

  // Cached information about keys to speed up findSweepGroupEdges.
  bool mayHaveKeyDelegates = false;
  bool mayHaveSymbolKeys = false;

  friend class JS::Zone;
};

template <class Key, class Value>
class WeakMap : public WeakMapBase {
  using BarrieredKey = HeapPtr<Key>;
  using BarrieredValue = HeapPtr<Value>;

  using Map = HashMap<HeapPtr<Key>, HeapPtr<Value>,
                      StableCellHasher<HeapPtr<Key>>, ZoneAllocPolicy>;
  using UnbarrieredMap =
      HashMap<Key, Value, StableCellHasher<Key>, ZoneAllocPolicy>;

  UnbarrieredMap map_;  // Barriers are added by |map()| accessor.

 public:
  using Lookup = typename Map::Lookup;
  using Entry = typename Map::Entry;
  using Range = typename Map::Range;
  using Ptr = typename Map::Ptr;
  using AddPtr = typename Map::AddPtr;

  struct Enum : public Map::Enum {
    explicit Enum(WeakMap& map) : Map::Enum(map.map()) {}
  };

  explicit WeakMap(JSContext* cx, JSObject* memOf = nullptr);
  explicit WeakMap(JS::Zone* zone, JSObject* memOf = nullptr);
  ~WeakMap() override;

  Range all() const { return map().all(); }
  uint32_t count() const { return map().count(); }
  bool empty() const { return map().empty(); }
  bool has(const Lookup& lookup) const { return map().has(lookup); }
  void remove(const Lookup& lookup) { return map().remove(lookup); }
  void remove(Ptr ptr) { return map().remove(ptr); }

  size_t shallowSizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf) const {
    return map().shallowSizeOfExcludingThis(aMallocSizeOf);
  }
  size_t shallowSizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const {
    return aMallocSizeOf(this) + shallowSizeOfExcludingThis(aMallocSizeOf);
  }

  // Get the value associated with a key, or a default constructed Value if the
  // key is not present in the map.
  Value get(const Lookup& l) {
    Ptr ptr = lookup(l);
    if (!ptr) {
      return Value();
    }
    return ptr->value();
  }

  // Add a read barrier to prevent a gray value from escaping the weak map. This
  // is necessary because we don't unmark gray through weak maps.
  Ptr lookup(const Lookup& l) const {
    Ptr p = map().lookup(l);
    if (p) {
      valueReadBarrier(p->value());
    }
    return p;
  }

  Ptr lookupUnbarriered(const Lookup& l) const { return map().lookup(l); }

  AddPtr lookupForAdd(const Lookup& l) {
    AddPtr p = map().lookupForAdd(l);
    if (p) {
      valueReadBarrier(p->value());
    }
    return p;
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool add(AddPtr& p, KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(gc::ToMarkable(k));
    keyWriteBarrier(std::forward<KeyInput>(k));
    return map().add(p, std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool relookupOrAdd(AddPtr& p, KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(gc::ToMarkable(k));
    keyWriteBarrier(std::forward<KeyInput>(k));
    return map().relookupOrAdd(p, std::forward<KeyInput>(k),
                               std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool put(KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(gc::ToMarkable(k));
    keyWriteBarrier(std::forward<KeyInput>(k));
    return map().put(std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool putNew(KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(gc::ToMarkable(k));
    keyWriteBarrier(std::forward<KeyInput>(k));
    return map().putNew(std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  void putNewInfallible(KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(gc::ToMarkable(k));
    keyWriteBarrier(std::forward<KeyInput>(k));
    map().putNewInfallible(std::forward(k), std::forward<KeyInput>(k));
  }

  void clear() {
    map().clear();
    mayHaveSymbolKeys = false;
    mayHaveKeyDelegates = false;
  }

#ifdef DEBUG
  template <typename KeyInput, typename ValueInput>
  bool hasEntry(KeyInput&& key, ValueInput&& value) {
    Ptr p = map().lookup(std::forward<KeyInput>(key));
    return p && p->value() == value;
  }
#endif

  bool markEntry(GCMarker* marker, gc::CellColor mapColor, BarrieredKey& key,
                 BarrieredValue& value, bool populateWeakKeysTable);

  void trace(JSTracer* trc) override;

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf);

 protected:
  inline void assertMapIsSameZoneWithValue(const BarrieredValue& v);

  bool markEntries(GCMarker* marker) override;

  // Find sweep group edges for delegates, if the key type has delegates. (If
  // not, the optimizer should make this a nop.)
  bool findSweepGroupEdges(Zone* atomsZone) override;

#if DEBUG
  void assertEntriesNotAboutToBeFinalized();
#endif

#ifdef JS_GC_ZEAL
  bool checkMarking() const override;
#endif

#ifdef JSGC_HASH_TABLE_CHECKS
  void checkAfterMovingGC() const override;
#endif

 private:
  // Map accessor uses a cast to add barriers.
  Map& map() { return reinterpret_cast<Map&>(map_); }
  const Map& map() const { return reinterpret_cast<const Map&>(map_); }

  static void valueReadBarrier(const JS::Value& v) {
    JS::ExposeValueToActiveJS(v);
  }
  static void valueReadBarrier(JSObject* obj) {
    JS::ExposeObjectToActiveJS(obj);
  }

  void keyWriteBarrier(const JS::Value& v) {
    if (v.isSymbol()) {
      mayHaveSymbolKeys = true;
    }
    if (v.isObject()) {
      keyWriteBarrier(&v.toObject());
    }
  }
  void keyWriteBarrier(JSObject* key) {
    JSObject* delegate = UncheckedUnwrapWithoutExpose(key);
    if (delegate != key || ObjectMayBeSwapped(key)) {
      mayHaveKeyDelegates = true;
    }
  }
  void keyWriteBarrier(BaseScript* key) {}

  void traceWeakEdges(JSTracer* trc) override;

  void clearAndCompact() override {
    map().clear();
    map().compact();
  }

  // memberOf can be nullptr, which means that the map is not part of a
  // JSObject.
  void traceMappings(WeakMapTracer* tracer) override;
};

using ObjectValueWeakMap = WeakMap<JSObject*, Value>;
using ValueValueWeakMap = WeakMap<Value, Value>;

// Generic weak map for mapping objects to other objects.
using ObjectWeakMap = WeakMap<JSObject*, JSObject*>;

// Get the hash from the Symbol.
HashNumber GetSymbolHash(JS::Symbol* sym);

// NB: The specialization works based on pointer equality and not on JS Value
// semantics, and it will assert if the Value's isGCThing() is false.
//
// When the JS Value is of type JS::Symbol, we cannot access uniqueIds when it
// runs on the worker thread, so we get the hashes from the Symbols directly
// instead.
template <>
struct StableCellHasher<HeapPtr<Value>> {
  using Key = HeapPtr<Value>;
  using Lookup = Value;

  static bool maybeGetHash(const Lookup& l, HashNumber* hashOut) {
    if (l.isSymbol()) {
      *hashOut = GetSymbolHash(l.toSymbol());
      return true;
    }
    return StableCellHasher<gc::Cell*>::maybeGetHash(l.toGCThing(), hashOut);
  }
  static bool ensureHash(const Lookup& l, HashNumber* hashOut) {
    if (l.isSymbol()) {
      *hashOut = GetSymbolHash(l.toSymbol());
      return true;
    }
    return StableCellHasher<gc::Cell*>::ensureHash(l.toGCThing(), hashOut);
  }
  static HashNumber hash(const Lookup& l) {
    if (l.isSymbol()) {
      return GetSymbolHash(l.toSymbol());
    }
    return StableCellHasher<gc::Cell*>::hash(l.toGCThing());
  }
  static bool match(const Key& k, const Lookup& l) {
    if (l.isSymbol()) {
      return k.toSymbol() == l.toSymbol();
    }
    return StableCellHasher<gc::Cell*>::match(k.toGCThing(), l.toGCThing());
  }
};

} /* namespace js */

#endif /* gc_WeakMap_h */
