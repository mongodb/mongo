/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_WeakMap_h
#define gc_WeakMap_h

#include "mozilla/LinkedList.h"

#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "gc/ZoneAllocator.h"
#include "js/HashTable.h"
#include "js/HeapAPI.h"

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
  virtual ~WeakMapBase();

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

  // Add zone edges for weakmaps with key delegates in a different zone.
  [[nodiscard]] static bool findSweepGroupEdgesForZone(JS::Zone* zone);

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

 protected:
  // Instance member functions called by the above. Instantiations of WeakMap
  // override these with definitions appropriate for their Key and Value types.
  virtual void trace(JSTracer* tracer) = 0;
  virtual bool findSweepGroupEdges() = 0;
  virtual void traceWeakEdges(JSTracer* trc) = 0;
  virtual void traceMappings(WeakMapTracer* tracer) = 0;
  virtual void clearAndCompact() = 0;

  // We have a key that, if it or its delegate is marked, may lead to a WeakMap
  // value getting marked. Insert it or its delegate (if any) into the
  // appropriate zone's gcEphemeronEdges or gcNurseryEphemeronEdges.
  inline bool addImplicitEdges(gc::Cell* key, gc::Cell* delegate,
                               gc::TenuredCell* value);

  virtual bool markEntries(GCMarker* marker) = 0;

#ifdef JS_GC_ZEAL
  virtual bool checkMarking() const = 0;
  virtual bool allowKeysInOtherZones() const { return false; }
  friend bool gc::CheckWeakMapEntryMarking(const WeakMapBase*, gc::Cell*,
                                           gc::Cell*);
#endif

  // Object that this weak map is part of, if any.
  HeapPtr<JSObject*> memberOf;

  // Zone containing this weak map.
  JS::Zone* zone_;

  // Whether this object has been marked during garbage collection and which
  // color it was marked.
  gc::CellColor mapColor;

  friend class JS::Zone;
};

template <class Key, class Value>
class WeakMap
    : private HashMap<Key, Value, StableCellHasher<Key>, ZoneAllocPolicy>,
      public WeakMapBase {
 public:
  using Base = HashMap<Key, Value, StableCellHasher<Key>, ZoneAllocPolicy>;

  using Lookup = typename Base::Lookup;
  using Entry = typename Base::Entry;
  using Range = typename Base::Range;
  using Ptr = typename Base::Ptr;
  using AddPtr = typename Base::AddPtr;

  struct Enum : public Base::Enum {
    explicit Enum(WeakMap& map) : Base::Enum(static_cast<Base&>(map)) {}
  };

  using Base::all;
  using Base::clear;
  using Base::count;
  using Base::empty;
  using Base::has;
  using Base::shallowSizeOfExcludingThis;

  // Resolve ambiguity with LinkedListElement<>::remove.
  using Base::remove;

  using UnbarrieredKey = typename RemoveBarrier<Key>::Type;

  explicit WeakMap(JSContext* cx, JSObject* memOf = nullptr);
  explicit WeakMap(JS::Zone* zone, JSObject* memOf = nullptr);

  // Add a read barrier to prevent an incorrectly gray value from escaping the
  // weak map. See the UnmarkGrayTracer::onChild comment in gc/Marking.cpp.
  Ptr lookup(const Lookup& l) const {
    Ptr p = Base::lookup(l);
    if (p) {
      exposeGCThingToActiveJS(p->value());
    }
    return p;
  }

  Ptr lookupUnbarriered(const Lookup& l) const { return Base::lookup(l); }

  AddPtr lookupForAdd(const Lookup& l) {
    AddPtr p = Base::lookupForAdd(l);
    if (p) {
      exposeGCThingToActiveJS(p->value());
    }
    return p;
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool add(AddPtr& p, KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(k);
    return Base::add(p, std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool relookupOrAdd(AddPtr& p, KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(k);
    return Base::relookupOrAdd(p, std::forward<KeyInput>(k),
                               std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool put(KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(k);
    return Base::put(std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool putNew(KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(k);
    return Base::putNew(std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  void putNewInfallible(KeyInput&& k, ValueInput&& v) {
    MOZ_ASSERT(k);
    Base::putNewInfallible(std::forward(k), std::forward<KeyInput>(k));
  }

#ifdef DEBUG
  template <typename KeyInput, typename ValueInput>
  bool hasEntry(KeyInput&& key, ValueInput&& value) {
    Ptr p = Base::lookup(std::forward<KeyInput>(key));
    return p && p->value() == value;
  }
#endif

  bool markEntry(GCMarker* marker, Key& key, Value& value,
                 bool populateWeakKeysTable);

  void trace(JSTracer* trc) override;

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf);

 protected:
  inline void assertMapIsSameZoneWithValue(const Value& v);

  bool markEntries(GCMarker* marker) override;

 protected:
  // Find sweep group edges for delegates, if the key type has delegates. (If
  // not, the optimizer should make this a nop.)
  bool findSweepGroupEdges() override;

  /**
   * If a wrapper is used as a key in a weakmap, the garbage collector should
   * keep that object around longer than it otherwise would. We want to avoid
   * collecting the wrapper (and removing the weakmap entry) as long as the
   * wrapped object is alive (because the object can be rewrapped and looked up
   * again). As long as the wrapper is used as a weakmap key, it will not be
   * collected (and remain in the weakmap) until the wrapped object is
   * collected.
   */
 private:
  void exposeGCThingToActiveJS(const JS::Value& v) const {
    JS::ExposeValueToActiveJS(v);
  }
  void exposeGCThingToActiveJS(JSObject* obj) const {
    JS::ExposeObjectToActiveJS(obj);
  }

  void traceWeakEdges(JSTracer* trc) override;

  void clearAndCompact() override {
    Base::clear();
    Base::compact();
  }

  // memberOf can be nullptr, which means that the map is not part of a
  // JSObject.
  void traceMappings(WeakMapTracer* tracer) override;

 protected:
#if DEBUG
  void assertEntriesNotAboutToBeFinalized();
#endif

#ifdef JS_GC_ZEAL
  bool checkMarking() const override;
#endif
};

using ObjectValueWeakMap = WeakMap<HeapPtr<JSObject*>, HeapPtr<Value>>;

// Generic weak map for mapping objects to other objects.
class ObjectWeakMap {
  ObjectValueWeakMap map;

 public:
  explicit ObjectWeakMap(JSContext* cx);

  JS::Zone* zone() const { return map.zone(); }

  JSObject* lookup(const JSObject* obj);
  bool add(JSContext* cx, JSObject* obj, JSObject* target);
  void remove(JSObject* key);
  void clear();

  void trace(JSTracer* trc);
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }

  ObjectValueWeakMap& valueMap() { return map; }

#ifdef JSGC_HASH_TABLE_CHECKS
  void checkAfterMovingGC();
#endif
};

} /* namespace js */

#endif /* gc_WeakMap_h */
