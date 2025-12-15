/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GCHashTable_h
#define GCHashTable_h

#include "mozilla/Maybe.h"

#include "js/GCPolicyAPI.h"
#include "js/HashTable.h"
#include "js/RootingAPI.h"
#include "js/SweepingAPI.h"
#include "js/TypeDecls.h"

class JSTracer;

namespace JS {

// Define a reasonable default GC policy for GC-aware Maps.
template <typename Key, typename Value>
struct DefaultMapEntryGCPolicy {
  static bool traceWeak(JSTracer* trc, Key* key, Value* value) {
    return GCPolicy<Key>::traceWeak(trc, key) &&
           GCPolicy<Value>::traceWeak(trc, value);
  }
  static bool needsSweep(JSTracer* trc, const Key* key, const Value* value) {
    // This is like a const version of the |traceWeak| method. It has the sense
    // of the return value reversed and does not mutate keys/values. Used during
    // incremental sweeping by the WeakCache specializations for maps and sets.
    return GCPolicy<Key>::needsSweep(trc, key) ||
           GCPolicy<Value>::needsSweep(trc, value);
  }
};

// A GCHashMap is a GC-aware HashMap, meaning that it has additional trace
// methods that know how to visit all keys and values in the table. HashMaps
// that contain GC pointers will generally want to use this GCHashMap
// specialization instead of HashMap, because this conveniently supports tracing
// keys and values, and cleaning up weak entries.
//
// GCHashMap::trace applies GCPolicy<T>::trace to each entry's key and value.
// Most types of GC pointers already have appropriate specializations of
// GCPolicy, so they should just work as keys and values. Any struct type with a
// default constructor and trace function should work as well. If you need to
// define your own GCPolicy specialization, generic helpers can be found in
// js/public/TracingAPI.h.
//
// The MapEntryGCPolicy template parameter controls how the table drops entries
// when edges are weakly held. GCHashMap::traceWeak applies the
// MapEntryGCPolicy's traceWeak method to each table entry; if it returns true,
// the entry is dropped. The default MapEntryGCPolicy drops the entry if either
// the key or value is about to be finalized, according to its
// GCPolicy<T>::traceWeak method. (This default is almost always fine: it's hard
// to imagine keeping such an entry around anyway.)
//
// Note that this HashMap only knows *how* to trace, but it does not itself
// cause tracing to be invoked. For tracing, it must be used as
// Rooted<GCHashMap> or PersistentRooted<GCHashMap>, or barriered and traced
// manually.
template <typename Key, typename Value,
          typename HashPolicy = js::DefaultHasher<Key>,
          typename AllocPolicy = js::TempAllocPolicy,
          typename MapEntryGCPolicy = DefaultMapEntryGCPolicy<Key, Value>>
class GCHashMap : public js::HashMap<Key, Value, HashPolicy, AllocPolicy> {
  using Base = js::HashMap<Key, Value, HashPolicy, AllocPolicy>;

 public:
  using EntryGCPolicy = MapEntryGCPolicy;

  explicit GCHashMap() : Base(AllocPolicy()) {}
  explicit GCHashMap(AllocPolicy a) : Base(std::move(a)) {}
  explicit GCHashMap(size_t length) : Base(length) {}
  GCHashMap(AllocPolicy a, size_t length) : Base(std::move(a), length) {}

  void trace(JSTracer* trc) {
    for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
      GCPolicy<Value>::trace(trc, &e.front().value(), "hashmap value");
      GCPolicy<Key>::trace(trc, &e.front().mutableKey(), "hashmap key");
    }
  }

  bool traceWeak(JSTracer* trc) {
    typename Base::Enum e(*this);
    traceWeakEntries(trc, e);
    return !this->empty();
  }

  void traceWeakEntries(JSTracer* trc, typename Base::Enum& e) {
    for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
      if (!MapEntryGCPolicy::traceWeak(trc, &e.front().mutableKey(),
                                       &e.front().value())) {
        e.removeFront();
      }
    }
  }

  bool needsSweep(JSTracer* trc) const {
    for (auto r = this->all(); !r.empty(); r.popFront()) {
      if (MapEntryGCPolicy::needsSweep(trc, &r.front().key(),
                                       &r.front().value())) {
        return true;
      }
    }
    return false;
  }

  // GCHashMap is movable
  GCHashMap(GCHashMap&& rhs) : Base(std::move(rhs)) {}
  void operator=(GCHashMap&& rhs) {
    MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
    Base::operator=(std::move(rhs));
  }

 private:
  // GCHashMap is not copyable or assignable
  GCHashMap(const GCHashMap& hm) = delete;
  GCHashMap& operator=(const GCHashMap& hm) = delete;
} MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS;

}  // namespace JS

namespace js {

// HashMap that supports rekeying.
//
// If your keys are pointers to something like JSObject that can be tenured or
// compacted, prefer to use GCHashMap with StableCellHasher, which takes
// advantage of the Zone's stable id table to make rekeying unnecessary.
template <typename Key, typename Value,
          typename HashPolicy = DefaultHasher<Key>,
          typename AllocPolicy = TempAllocPolicy,
          typename MapEntryGCPolicy = JS::DefaultMapEntryGCPolicy<Key, Value>>
class GCRekeyableHashMap : public JS::GCHashMap<Key, Value, HashPolicy,
                                                AllocPolicy, MapEntryGCPolicy> {
  using Base = JS::GCHashMap<Key, Value, HashPolicy, AllocPolicy>;

 public:
  explicit GCRekeyableHashMap(AllocPolicy a = AllocPolicy())
      : Base(std::move(a)) {}
  explicit GCRekeyableHashMap(size_t length) : Base(length) {}
  GCRekeyableHashMap(AllocPolicy a, size_t length)
      : Base(std::move(a), length) {}

  bool traceWeak(JSTracer* trc) {
    for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
      Key key(e.front().key());
      if (!MapEntryGCPolicy::traceWeak(trc, &key, &e.front().value())) {
        e.removeFront();
      } else if (!HashPolicy::match(key, e.front().key())) {
        e.rekeyFront(key);
      }
    }
    return !this->empty();
  }

  // GCRekeyableHashMap is movable
  GCRekeyableHashMap(GCRekeyableHashMap&& rhs) : Base(std::move(rhs)) {}
  void operator=(GCRekeyableHashMap&& rhs) {
    MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
    Base::operator=(std::move(rhs));
  }
} MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS;

template <typename Wrapper, typename... Args>
class WrappedPtrOperations<JS::GCHashMap<Args...>, Wrapper> {
  using Map = JS::GCHashMap<Args...>;
  using Lookup = typename Map::Lookup;

  const Map& map() const { return static_cast<const Wrapper*>(this)->get(); }

 public:
  using AddPtr = typename Map::AddPtr;
  using Ptr = typename Map::Ptr;
  using Range = typename Map::Range;

  Ptr lookup(const Lookup& l) const { return map().lookup(l); }
  Range all() const { return map().all(); }
  bool empty() const { return map().empty(); }
  uint32_t count() const { return map().count(); }
  size_t capacity() const { return map().capacity(); }
  bool has(const Lookup& l) const { return map().lookup(l).found(); }
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return map().sizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + map().sizeOfExcludingThis(mallocSizeOf);
  }
};

template <typename Wrapper, typename... Args>
class MutableWrappedPtrOperations<JS::GCHashMap<Args...>, Wrapper>
    : public WrappedPtrOperations<JS::GCHashMap<Args...>, Wrapper> {
  using Map = JS::GCHashMap<Args...>;
  using Lookup = typename Map::Lookup;

  Map& map() { return static_cast<Wrapper*>(this)->get(); }

 public:
  using AddPtr = typename Map::AddPtr;
  struct Enum : public Map::Enum {
    explicit Enum(Wrapper& o) : Map::Enum(o.map()) {}
  };
  using Ptr = typename Map::Ptr;
  using Range = typename Map::Range;

  void clear() { map().clear(); }
  void clearAndCompact() { map().clearAndCompact(); }
  void remove(Ptr p) { map().remove(p); }
  AddPtr lookupForAdd(const Lookup& l) { return map().lookupForAdd(l); }

  template <typename KeyInput, typename ValueInput>
  bool add(AddPtr& p, KeyInput&& k, ValueInput&& v) {
    return map().add(p, std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput>
  bool add(AddPtr& p, KeyInput&& k) {
    return map().add(p, std::forward<KeyInput>(k), Map::Value());
  }

  template <typename KeyInput, typename ValueInput>
  bool relookupOrAdd(AddPtr& p, KeyInput&& k, ValueInput&& v) {
    return map().relookupOrAdd(p, k, std::forward<KeyInput>(k),
                               std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  bool put(KeyInput&& k, ValueInput&& v) {
    return map().put(std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  bool putNew(KeyInput&& k, ValueInput&& v) {
    return map().putNew(std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }
};

}  // namespace js

namespace JS {

// A GCHashSet is a HashSet with an additional trace method that knows
// be traced to be kept alive will generally want to use this GCHashSet
// specialization in lieu of HashSet.
//
// Most types of GC pointers can be traced with no extra infrastructure. For
// structs and non-gc-pointer members, ensure that there is a specialization of
// GCPolicy<T> with an appropriate trace method available to handle the custom
// type. Generic helpers can be found in js/public/TracingAPI.h.
//
// Note that although this HashSet's trace will deal correctly with moved
// elements, it does not itself know when to barrier or trace elements. To
// function properly it must either be used with Rooted or barriered and traced
// manually.
template <typename T, typename HashPolicy = js::DefaultHasher<T>,
          typename AllocPolicy = js::TempAllocPolicy>
class GCHashSet : public js::HashSet<T, HashPolicy, AllocPolicy> {
  using Base = js::HashSet<T, HashPolicy, AllocPolicy>;

 public:
  explicit GCHashSet(AllocPolicy a = AllocPolicy()) : Base(std::move(a)) {}
  explicit GCHashSet(size_t length) : Base(length) {}
  GCHashSet(AllocPolicy a, size_t length) : Base(std::move(a), length) {}

  void trace(JSTracer* trc) {
    for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
      GCPolicy<T>::trace(trc, &e.mutableFront(), "hashset element");
    }
  }

  bool traceWeak(JSTracer* trc) {
    typename Base::Enum e(*this);
    traceWeakEntries(trc, e);
    return !this->empty();
  }

  void traceWeakEntries(JSTracer* trc, typename Base::Enum& e) {
    for (; !e.empty(); e.popFront()) {
      if (!GCPolicy<T>::traceWeak(trc, &e.mutableFront())) {
        e.removeFront();
      }
    }
  }

  bool needsSweep(JSTracer* trc) const {
    for (auto r = this->all(); !r.empty(); r.popFront()) {
      if (GCPolicy<T>::needsSweep(trc, &r.front())) {
        return true;
      }
    }
    return false;
  }

  // GCHashSet is movable
  GCHashSet(GCHashSet&& rhs) : Base(std::move(rhs)) {}
  void operator=(GCHashSet&& rhs) {
    MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
    Base::operator=(std::move(rhs));
  }

 private:
  // GCHashSet is not copyable or assignable
  GCHashSet(const GCHashSet& hs) = delete;
  GCHashSet& operator=(const GCHashSet& hs) = delete;
} MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS;

}  // namespace JS

namespace js {

template <typename Wrapper, typename... Args>
class WrappedPtrOperations<JS::GCHashSet<Args...>, Wrapper> {
  using Set = JS::GCHashSet<Args...>;

  const Set& set() const { return static_cast<const Wrapper*>(this)->get(); }

 public:
  using Lookup = typename Set::Lookup;
  using AddPtr = typename Set::AddPtr;
  using Entry = typename Set::Entry;
  using Ptr = typename Set::Ptr;
  using Range = typename Set::Range;

  Ptr lookup(const Lookup& l) const { return set().lookup(l); }
  Range all() const { return set().all(); }
  bool empty() const { return set().empty(); }
  uint32_t count() const { return set().count(); }
  size_t capacity() const { return set().capacity(); }
  bool has(const Lookup& l) const { return set().lookup(l).found(); }
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return set().sizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + set().sizeOfExcludingThis(mallocSizeOf);
  }
};

template <typename Wrapper, typename... Args>
class MutableWrappedPtrOperations<JS::GCHashSet<Args...>, Wrapper>
    : public WrappedPtrOperations<JS::GCHashSet<Args...>, Wrapper> {
  using Set = JS::GCHashSet<Args...>;
  using Lookup = typename Set::Lookup;

  Set& set() { return static_cast<Wrapper*>(this)->get(); }

 public:
  using AddPtr = typename Set::AddPtr;
  using Entry = typename Set::Entry;
  struct Enum : public Set::Enum {
    explicit Enum(Wrapper& o) : Set::Enum(o.set()) {}
  };
  using Ptr = typename Set::Ptr;
  using Range = typename Set::Range;

  void clear() { set().clear(); }
  void clearAndCompact() { set().clearAndCompact(); }
  [[nodiscard]] bool reserve(uint32_t len) { return set().reserve(len); }
  void remove(Ptr p) { set().remove(p); }
  void remove(const Lookup& l) { set().remove(l); }
  AddPtr lookupForAdd(const Lookup& l) { return set().lookupForAdd(l); }

  template <typename TInput>
  void replaceKey(Ptr p, const Lookup& l, TInput&& newValue) {
    set().replaceKey(p, l, std::forward<TInput>(newValue));
  }

  template <typename TInput>
  bool add(AddPtr& p, TInput&& t) {
    return set().add(p, std::forward<TInput>(t));
  }

  template <typename TInput>
  bool relookupOrAdd(AddPtr& p, const Lookup& l, TInput&& t) {
    return set().relookupOrAdd(p, l, std::forward<TInput>(t));
  }

  template <typename TInput>
  bool put(TInput&& t) {
    return set().put(std::forward<TInput>(t));
  }

  template <typename TInput>
  bool putNew(TInput&& t) {
    return set().putNew(std::forward<TInput>(t));
  }

  template <typename TInput>
  bool putNew(const Lookup& l, TInput&& t) {
    return set().putNew(l, std::forward<TInput>(t));
  }
};

} /* namespace js */

namespace JS {

// Specialize WeakCache for GCHashMap to provide a barriered map that does not
// need to be swept immediately.
template <typename Key, typename Value, typename HashPolicy,
          typename AllocPolicy, typename MapEntryGCPolicy>
class WeakCache<
    GCHashMap<Key, Value, HashPolicy, AllocPolicy, MapEntryGCPolicy>>
    final : protected detail::WeakCacheBase {
  using Map = GCHashMap<Key, Value, HashPolicy, AllocPolicy, MapEntryGCPolicy>;
  using Self = WeakCache<Map>;

  Map map;
  JSTracer* barrierTracer = nullptr;

 public:
  template <typename... Args>
  explicit WeakCache(Zone* zone, Args&&... args)
      : WeakCacheBase(zone), map(std::forward<Args>(args)...) {}
  template <typename... Args>
  explicit WeakCache(JSRuntime* rt, Args&&... args)
      : WeakCacheBase(rt), map(std::forward<Args>(args)...) {}
  ~WeakCache() { MOZ_ASSERT(!barrierTracer); }

  bool empty() override { return map.empty(); }

  size_t traceWeak(JSTracer* trc, NeedsLock needsLock) override {
    size_t steps = map.count();

    // Create an Enum and sweep the table entries.
    mozilla::Maybe<typename Map::Enum> e;
    e.emplace(map);
    map.traceWeakEntries(trc, e.ref());

    // Potentially take a lock while the Enum's destructor is called as this can
    // rehash/resize the table and access the store buffer.
    mozilla::Maybe<js::gc::AutoLockStoreBuffer> lock;
    if (needsLock) {
      lock.emplace(trc->runtime());
    }
    e.reset();

    return steps;
  }

  bool setIncrementalBarrierTracer(JSTracer* trc) override {
    MOZ_ASSERT(bool(barrierTracer) != bool(trc));
    barrierTracer = trc;
    return true;
  }

  bool needsIncrementalBarrier() const override { return barrierTracer; }

 private:
  using Entry = typename Map::Entry;

  static bool entryNeedsSweep(JSTracer* barrierTracer, const Entry& entry) {
    return MapEntryGCPolicy::needsSweep(barrierTracer, &entry.key(),
                                        &entry.value());
  }

 public:
  using Lookup = typename Map::Lookup;
  using Ptr = typename Map::Ptr;
  using AddPtr = typename Map::AddPtr;

  // Iterator over the whole collection.
  struct Range {
    explicit Range(Self& self) : cache(self), range(self.map.all()) {
      settle();
    }
    Range() = default;

    bool empty() const { return range.empty(); }
    const Entry& front() const { return range.front(); }

    void popFront() {
      range.popFront();
      settle();
    }

   private:
    Self& cache;
    typename Map::Range range;

    void settle() {
      if (JSTracer* trc = cache.barrierTracer) {
        while (!empty() && entryNeedsSweep(trc, front())) {
          popFront();
        }
      }
    }
  };

  struct Enum : public Map::Enum {
    explicit Enum(Self& cache) : Map::Enum(cache.map) {
      // This operation is not allowed while barriers are in place as we
      // may also need to enumerate the set for sweeping.
      MOZ_ASSERT(!cache.barrierTracer);
    }
  };

  Ptr lookup(const Lookup& l) const {
    Ptr ptr = map.lookup(l);
    if (barrierTracer && ptr && entryNeedsSweep(barrierTracer, *ptr)) {
      const_cast<Map&>(map).remove(ptr);
      return Ptr();
    }
    return ptr;
  }

  AddPtr lookupForAdd(const Lookup& l) {
    AddPtr ptr = map.lookupForAdd(l);
    if (barrierTracer && ptr && entryNeedsSweep(barrierTracer, *ptr)) {
      const_cast<Map&>(map).remove(ptr);
      return map.lookupForAdd(l);
    }
    return ptr;
  }

  Range all() const { return Range(*const_cast<Self*>(this)); }

  bool empty() const {
    // This operation is not currently allowed while barriers are in place
    // as it would require iterating the map and the caller expects a
    // constant time operation.
    MOZ_ASSERT(!barrierTracer);
    return map.empty();
  }

  uint32_t count() const {
    // This operation is not currently allowed while barriers are in place
    // as it would require iterating the set and the caller expects a
    // constant time operation.
    MOZ_ASSERT(!barrierTracer);
    return map.count();
  }

  size_t capacity() const { return map.capacity(); }

  bool has(const Lookup& l) const { return lookup(l).found(); }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return map.sizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + map.shallowSizeOfExcludingThis(mallocSizeOf);
  }

  void clear() {
    // This operation is not currently allowed while barriers are in place
    // since it doesn't make sense to clear a cache while it is being swept.
    MOZ_ASSERT(!barrierTracer);
    map.clear();
  }

  void clearAndCompact() {
    // This operation is not currently allowed while barriers are in place
    // since it doesn't make sense to clear a cache while it is being swept.
    MOZ_ASSERT(!barrierTracer);
    map.clearAndCompact();
  }

  void remove(Ptr p) {
    // This currently supports removing entries during incremental
    // sweeping. If we allow these tables to be swept incrementally this may
    // no longer be possible.
    map.remove(p);
  }

  void remove(const Lookup& l) {
    Ptr p = lookup(l);
    if (p) {
      remove(p);
    }
  }

  template <typename KeyInput, typename ValueInput>
  bool add(AddPtr& p, KeyInput&& k, ValueInput&& v) {
    return map.add(p, std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  bool relookupOrAdd(AddPtr& p, KeyInput&& k, ValueInput&& v) {
    return map.relookupOrAdd(p, std::forward<KeyInput>(k),
                             std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  bool put(KeyInput&& k, ValueInput&& v) {
    return map.put(std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }

  template <typename KeyInput, typename ValueInput>
  bool putNew(KeyInput&& k, ValueInput&& v) {
    return map.putNew(std::forward<KeyInput>(k), std::forward<ValueInput>(v));
  }
} JS_HAZ_NON_GC_POINTER;

// Specialize WeakCache for GCHashSet to provide a barriered set that does not
// need to be swept immediately.
template <typename T, typename HashPolicy, typename AllocPolicy>
class WeakCache<GCHashSet<T, HashPolicy, AllocPolicy>> final
    : protected detail::WeakCacheBase {
  using Set = GCHashSet<T, HashPolicy, AllocPolicy>;
  using Self = WeakCache<Set>;

  Set set;
  JSTracer* barrierTracer = nullptr;

 public:
  using Entry = typename Set::Entry;

  template <typename... Args>
  explicit WeakCache(Zone* zone, Args&&... args)
      : WeakCacheBase(zone), set(std::forward<Args>(args)...) {}
  template <typename... Args>
  explicit WeakCache(JSRuntime* rt, Args&&... args)
      : WeakCacheBase(rt), set(std::forward<Args>(args)...) {}

  size_t traceWeak(JSTracer* trc, NeedsLock needsLock) override {
    size_t steps = set.count();

    // Create an Enum and sweep the table entries. It's not necessary to take
    // the store buffer lock yet.
    mozilla::Maybe<typename Set::Enum> e;
    e.emplace(set);
    set.traceWeakEntries(trc, e.ref());

    // Destroy the Enum, potentially rehashing or resizing the table. Since this
    // can access the store buffer, we need to take a lock for this if we're
    // called off main thread.
    mozilla::Maybe<js::gc::AutoLockStoreBuffer> lock;
    if (needsLock) {
      lock.emplace(trc->runtime());
    }
    e.reset();

    return steps;
  }

  bool empty() override { return set.empty(); }

  bool setIncrementalBarrierTracer(JSTracer* trc) override {
    MOZ_ASSERT(bool(barrierTracer) != bool(trc));
    barrierTracer = trc;
    return true;
  }

  bool needsIncrementalBarrier() const override { return barrierTracer; }

  // Steal the contents of this weak cache.
  Set stealContents() {
    // This operation is not currently allowed while barriers are in place
    // since it doesn't make sense to steal the contents while we are
    // sweeping.
    MOZ_ASSERT(!barrierTracer);

    auto rval = std::move(set);
    // Ensure set is in a specified (empty) state after the move
    set.clear();

    // Return set; no move to avoid invalidating NRVO.
    return rval;
  }

 private:
  static bool entryNeedsSweep(JSTracer* barrierTracer, const Entry& prior) {
    Entry entry(prior);
    bool needsSweep = !GCPolicy<T>::traceWeak(barrierTracer, &entry);
    MOZ_ASSERT_IF(!needsSweep, prior == entry);  // We shouldn't update here.
    return needsSweep;
  }

 public:
  using Lookup = typename Set::Lookup;
  using Ptr = typename Set::Ptr;
  using AddPtr = typename Set::AddPtr;

  // Iterator over the whole collection.
  struct Range {
    explicit Range(Self& self) : cache(self), range(self.set.all()) {
      settle();
    }
    Range() = default;

    bool empty() const { return range.empty(); }
    const Entry& front() const { return range.front(); }

    void popFront() {
      range.popFront();
      settle();
    }

   private:
    Self& cache;
    typename Set::Range range;

    void settle() {
      if (JSTracer* trc = cache.barrierTracer) {
        while (!empty() && entryNeedsSweep(trc, front())) {
          popFront();
        }
      }
    }
  };

  struct Enum : public Set::Enum {
    explicit Enum(Self& cache) : Set::Enum(cache.set) {
      // This operation is not allowed while barriers are in place as we
      // may also need to enumerate the set for sweeping.
      MOZ_ASSERT(!cache.barrierTracer);
    }
  };

  Ptr lookup(const Lookup& l) const {
    Ptr ptr = set.lookup(l);
    if (barrierTracer && ptr && entryNeedsSweep(barrierTracer, *ptr)) {
      const_cast<Set&>(set).remove(ptr);
      return Ptr();
    }
    return ptr;
  }

  AddPtr lookupForAdd(const Lookup& l) {
    AddPtr ptr = set.lookupForAdd(l);
    if (barrierTracer && ptr && entryNeedsSweep(barrierTracer, *ptr)) {
      const_cast<Set&>(set).remove(ptr);
      return set.lookupForAdd(l);
    }
    return ptr;
  }

  Range all() const { return Range(*const_cast<Self*>(this)); }

  bool empty() const {
    // This operation is not currently allowed while barriers are in place
    // as it would require iterating the set and the caller expects a
    // constant time operation.
    MOZ_ASSERT(!barrierTracer);
    return set.empty();
  }

  uint32_t count() const {
    // This operation is not currently allowed while barriers are in place
    // as it would require iterating the set and the caller expects a
    // constant time operation.
    MOZ_ASSERT(!barrierTracer);
    return set.count();
  }

  size_t capacity() const { return set.capacity(); }

  bool has(const Lookup& l) const { return lookup(l).found(); }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return set.shallowSizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + set.shallowSizeOfExcludingThis(mallocSizeOf);
  }

  void clear() {
    // This operation is not currently allowed while barriers are in place
    // since it doesn't make sense to clear a cache while it is being swept.
    MOZ_ASSERT(!barrierTracer);
    set.clear();
  }

  void clearAndCompact() {
    // This operation is not currently allowed while barriers are in place
    // since it doesn't make sense to clear a cache while it is being swept.
    MOZ_ASSERT(!barrierTracer);
    set.clearAndCompact();
  }

  void remove(Ptr p) {
    // This currently supports removing entries during incremental
    // sweeping. If we allow these tables to be swept incrementally this may
    // no longer be possible.
    set.remove(p);
  }

  void remove(const Lookup& l) {
    Ptr p = lookup(l);
    if (p) {
      remove(p);
    }
  }

  template <typename TInput>
  void replaceKey(Ptr p, const Lookup& l, TInput&& newValue) {
    set.replaceKey(p, l, std::forward<TInput>(newValue));
  }

  template <typename TInput>
  bool add(AddPtr& p, TInput&& t) {
    return set.add(p, std::forward<TInput>(t));
  }

  template <typename TInput>
  bool relookupOrAdd(AddPtr& p, const Lookup& l, TInput&& t) {
    return set.relookupOrAdd(p, l, std::forward<TInput>(t));
  }

  template <typename TInput>
  bool put(TInput&& t) {
    return set.put(std::forward<TInput>(t));
  }

  template <typename TInput>
  bool putNew(TInput&& t) {
    return set.putNew(std::forward<TInput>(t));
  }

  template <typename TInput>
  bool putNew(const Lookup& l, TInput&& t) {
    return set.putNew(l, std::forward<TInput>(t));
  }
} JS_HAZ_NON_GC_POINTER;

}  // namespace JS

#endif /* GCHashTable_h */
