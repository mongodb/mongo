/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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
#include "js/TracingAPI.h"

namespace JS {

// Define a reasonable default GC policy for GC-aware Maps.
template <typename Key, typename Value>
struct DefaultMapSweepPolicy {
    static bool needsSweep(Key* key, Value* value) {
        return GCPolicy<Key>::needsSweep(key) || GCPolicy<Value>::needsSweep(value);
    }
};

// A GCHashMap is a GC-aware HashMap, meaning that it has additional trace and
// sweep methods that know how to visit all keys and values in the table.
// HashMaps that contain GC pointers will generally want to use this GCHashMap
// specialization instead of HashMap, because this conveniently supports tracing
// keys and values, and cleaning up weak entries.
//
// GCHashMap::trace applies GCPolicy<T>::trace to each entry's key and value.
// Most types of GC pointers already have appropriate specializations of
// GCPolicy, so they should just work as keys and values. Any struct type with a
// default constructor and trace and sweep functions should work as well. If you
// need to define your own GCPolicy specialization, generic helpers can be found
// in js/public/TracingAPI.h.
//
// The MapSweepPolicy template parameter controls how the table drops entries
// when swept. GCHashMap::sweep applies MapSweepPolicy::needsSweep to each table
// entry; if it returns true, the entry is dropped. The default MapSweepPolicy
// drops the entry if either the key or value is about to be finalized,
// according to its GCPolicy<T>::needsSweep method. (This default is almost
// always fine: it's hard to imagine keeping such an entry around anyway.)
//
// Note that this HashMap only knows *how* to trace and sweep, but it does not
// itself cause tracing or sweeping to be invoked. For tracing, it must be used
// with Rooted or PersistentRooted, or barriered and traced manually. For
// sweeping, currently it requires an explicit call to <map>.sweep().
template <typename Key,
          typename Value,
          typename HashPolicy = js::DefaultHasher<Key>,
          typename AllocPolicy = js::TempAllocPolicy,
          typename MapSweepPolicy = DefaultMapSweepPolicy<Key, Value>>
class GCHashMap : public js::HashMap<Key, Value, HashPolicy, AllocPolicy>
{
    using Base = js::HashMap<Key, Value, HashPolicy, AllocPolicy>;

  public:
    explicit GCHashMap(AllocPolicy a = AllocPolicy()) : Base(a)  {}

    static void trace(GCHashMap* map, JSTracer* trc) { map->trace(trc); }
    void trace(JSTracer* trc) {
        if (!this->initialized())
            return;
        for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
            GCPolicy<Value>::trace(trc, &e.front().value(), "hashmap value");
            GCPolicy<Key>::trace(trc, &e.front().mutableKey(), "hashmap key");
        }
    }

    bool needsSweep() const {
        return this->initialized() && !this->empty();
    }

    void sweep() {
        if (!this->initialized())
            return;

        for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
            if (MapSweepPolicy::needsSweep(&e.front().mutableKey(), &e.front().value()))
                e.removeFront();
        }
    }

    // GCHashMap is movable
    GCHashMap(GCHashMap&& rhs) : Base(mozilla::Move(rhs)) {}
    void operator=(GCHashMap&& rhs) {
        MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
        Base::operator=(mozilla::Move(rhs));
    }

  private:
    // GCHashMap is not copyable or assignable
    GCHashMap(const GCHashMap& hm) = delete;
    GCHashMap& operator=(const GCHashMap& hm) = delete;
};

} // namespace JS

namespace js {

// HashMap that supports rekeying.
//
// If your keys are pointers to something like JSObject that can be tenured or
// compacted, prefer to use GCHashMap with MovableCellHasher, which takes
// advantage of the Zone's stable id table to make rekeying unnecessary.
template <typename Key,
          typename Value,
          typename HashPolicy = DefaultHasher<Key>,
          typename AllocPolicy = TempAllocPolicy,
          typename MapSweepPolicy = JS::DefaultMapSweepPolicy<Key, Value>>
class GCRekeyableHashMap : public JS::GCHashMap<Key, Value, HashPolicy, AllocPolicy, MapSweepPolicy>
{
    using Base = JS::GCHashMap<Key, Value, HashPolicy, AllocPolicy>;

  public:
    explicit GCRekeyableHashMap(AllocPolicy a = AllocPolicy()) : Base(a)  {}

    void sweep() {
        if (!this->initialized())
            return;

        for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
            Key key(e.front().key());
            if (MapSweepPolicy::needsSweep(&key, &e.front().value()))
                e.removeFront();
            else if (!HashPolicy::match(key, e.front().key()))
                e.rekeyFront(key);
        }
    }

    // GCRekeyableHashMap is movable
    GCRekeyableHashMap(GCRekeyableHashMap&& rhs) : Base(mozilla::Move(rhs)) {}
    void operator=(GCRekeyableHashMap&& rhs) {
        MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
        Base::operator=(mozilla::Move(rhs));
    }
};

template <typename Wrapper, typename... Args>
class WrappedPtrOperations<JS::GCHashMap<Args...>, Wrapper>
{
    using Map = JS::GCHashMap<Args...>;
    using Lookup = typename Map::Lookup;

    const Map& map() const { return static_cast<const Wrapper*>(this)->get(); }

  public:
    using AddPtr = typename Map::AddPtr;
    using Ptr = typename Map::Ptr;
    using Range = typename Map::Range;

    bool initialized() const                   { return map().initialized(); }
    Ptr lookup(const Lookup& l) const          { return map().lookup(l); }
    AddPtr lookupForAdd(const Lookup& l) const { return map().lookupForAdd(l); }
    Range all() const                          { return map().all(); }
    bool empty() const                         { return map().empty(); }
    uint32_t count() const                     { return map().count(); }
    size_t capacity() const                    { return map().capacity(); }
    bool has(const Lookup& l) const            { return map().lookup(l).found(); }
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return map().sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + map().sizeOfExcludingThis(mallocSizeOf);
    }
};

template <typename Wrapper, typename... Args>
class MutableWrappedPtrOperations<JS::GCHashMap<Args...>, Wrapper>
  : public WrappedPtrOperations<JS::GCHashMap<Args...>, Wrapper>
{
    using Map = JS::GCHashMap<Args...>;
    using Lookup = typename Map::Lookup;

    Map& map() { return static_cast<Wrapper*>(this)->get(); }

  public:
    using AddPtr = typename Map::AddPtr;
    struct Enum : public Map::Enum { explicit Enum(Wrapper& o) : Map::Enum(o.map()) {} };
    using Ptr = typename Map::Ptr;
    using Range = typename Map::Range;

    bool init(uint32_t len = 16) { return map().init(len); }
    void clear()                 { map().clear(); }
    void finish()                { map().finish(); }
    void remove(Ptr p)           { map().remove(p); }

    template<typename KeyInput, typename ValueInput>
    bool add(AddPtr& p, KeyInput&& k, ValueInput&& v) {
        return map().add(p, mozilla::Forward<KeyInput>(k), mozilla::Forward<ValueInput>(v));
    }

    template<typename KeyInput>
    bool add(AddPtr& p, KeyInput&& k) {
        return map().add(p, mozilla::Forward<KeyInput>(k), Map::Value());
    }

    template<typename KeyInput, typename ValueInput>
    bool relookupOrAdd(AddPtr& p, KeyInput&& k, ValueInput&& v) {
        return map().relookupOrAdd(p, k,
                                   mozilla::Forward<KeyInput>(k),
                                   mozilla::Forward<ValueInput>(v));
    }

    template<typename KeyInput, typename ValueInput>
    bool put(KeyInput&& k, ValueInput&& v) {
        return map().put(mozilla::Forward<KeyInput>(k), mozilla::Forward<ValueInput>(v));
    }

    template<typename KeyInput, typename ValueInput>
    bool putNew(KeyInput&& k, ValueInput&& v) {
        return map().putNew(mozilla::Forward<KeyInput>(k), mozilla::Forward<ValueInput>(v));
    }
};

} // namespace js

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
template <typename T,
          typename HashPolicy = js::DefaultHasher<T>,
          typename AllocPolicy = js::TempAllocPolicy>
class GCHashSet : public js::HashSet<T, HashPolicy, AllocPolicy>
{
    using Base = js::HashSet<T, HashPolicy, AllocPolicy>;

  public:
    explicit GCHashSet(AllocPolicy a = AllocPolicy()) : Base(a)  {}

    static void trace(GCHashSet* set, JSTracer* trc) { set->trace(trc); }
    void trace(JSTracer* trc) {
        if (!this->initialized())
            return;
        for (typename Base::Enum e(*this); !e.empty(); e.popFront())
            GCPolicy<T>::trace(trc, &e.mutableFront(), "hashset element");
    }

    bool needsSweep() const {
        return this->initialized() && !this->empty();
    }

    void sweep() {
        if (!this->initialized())
            return;
        for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
            if (GCPolicy<T>::needsSweep(&e.mutableFront()))
                e.removeFront();
        }
    }

    // GCHashSet is movable
    GCHashSet(GCHashSet&& rhs) : Base(mozilla::Move(rhs)) {}
    void operator=(GCHashSet&& rhs) {
        MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
        Base::operator=(mozilla::Move(rhs));
    }

  private:
    // GCHashSet is not copyable or assignable
    GCHashSet(const GCHashSet& hs) = delete;
    GCHashSet& operator=(const GCHashSet& hs) = delete;
};

} // namespace JS

namespace js {

template <typename Wrapper, typename... Args>
class WrappedPtrOperations<JS::GCHashSet<Args...>, Wrapper>
{
    using Set = JS::GCHashSet<Args...>;

    const Set& set() const { return static_cast<const Wrapper*>(this)->get(); }

  public:
    using Lookup = typename Set::Lookup;
    using AddPtr = typename Set::AddPtr;
    using Entry = typename Set::Entry;
    using Ptr = typename Set::Ptr;
    using Range = typename Set::Range;

    bool initialized() const                   { return set().initialized(); }
    Ptr lookup(const Lookup& l) const          { return set().lookup(l); }
    AddPtr lookupForAdd(const Lookup& l) const { return set().lookupForAdd(l); }
    Range all() const                          { return set().all(); }
    bool empty() const                         { return set().empty(); }
    uint32_t count() const                     { return set().count(); }
    size_t capacity() const                    { return set().capacity(); }
    bool has(const Lookup& l) const            { return set().lookup(l).found(); }
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return set().sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + set().sizeOfExcludingThis(mallocSizeOf);
    }
};

template <typename Wrapper, typename... Args>
class MutableWrappedPtrOperations<JS::GCHashSet<Args...>, Wrapper>
  : public WrappedPtrOperations<JS::GCHashSet<Args...>, Wrapper>
{
    using Set = JS::GCHashSet<Args...>;
    using Lookup = typename Set::Lookup;

    Set& set() { return static_cast<Wrapper*>(this)->get(); }

  public:
    using AddPtr = typename Set::AddPtr;
    using Entry = typename Set::Entry;
    struct Enum : public Set::Enum { explicit Enum(Wrapper& o) : Set::Enum(o.set()) {} };
    using Ptr = typename Set::Ptr;
    using Range = typename Set::Range;

    bool init(uint32_t len = 16) { return set().init(len); }
    void clear()                 { set().clear(); }
    void finish()                { set().finish(); }
    void remove(Ptr p)           { set().remove(p); }
    void remove(const Lookup& l) { set().remove(l); }

    template<typename TInput>
    bool add(AddPtr& p, TInput&& t) {
        return set().add(p, mozilla::Forward<TInput>(t));
    }

    template<typename TInput>
    bool relookupOrAdd(AddPtr& p, const Lookup& l, TInput&& t) {
        return set().relookupOrAdd(p, l, mozilla::Forward<TInput>(t));
    }

    template<typename TInput>
    bool put(TInput&& t) {
        return set().put(mozilla::Forward<TInput>(t));
    }

    template<typename TInput>
    bool putNew(TInput&& t) {
        return set().putNew(mozilla::Forward<TInput>(t));
    }

    template<typename TInput>
    bool putNew(const Lookup& l, TInput&& t) {
        return set().putNew(l, mozilla::Forward<TInput>(t));
    }
};

} /* namespace js */

namespace JS {

// Specialize WeakCache for GCHashMap to provide a barriered map that does not
// need to be swept immediately.
template <typename Key, typename Value,
          typename HashPolicy, typename AllocPolicy, typename MapSweepPolicy>
class WeakCache<GCHashMap<Key, Value, HashPolicy, AllocPolicy, MapSweepPolicy>>
  : protected detail::WeakCacheBase
{
    using Map = GCHashMap<Key, Value, HashPolicy, AllocPolicy, MapSweepPolicy>;
    using Self = WeakCache<Map>;

    Map map;
    bool needsBarrier;

  public:
    template <typename... Args>
    explicit WeakCache(Zone* zone, Args&&... args)
      : WeakCacheBase(zone), map(mozilla::Forward<Args>(args)...), needsBarrier(false)
    {}
    template <typename... Args>
    explicit WeakCache(JSRuntime* rt, Args&&... args)
      : WeakCacheBase(rt), map(mozilla::Forward<Args>(args)...), needsBarrier(false)
    {}
    ~WeakCache() {
        MOZ_ASSERT(!needsBarrier);
    }

    bool needsSweep() override {
        return map.needsSweep();
    }

    size_t sweep() override {
        if (!this->initialized())
            return 0;

        size_t steps = map.count();
        map.sweep();
        return steps;
    }

    bool setNeedsIncrementalBarrier(bool needs) override {
        MOZ_ASSERT(needsBarrier != needs);
        needsBarrier = needs;
        return true;
    }

    bool needsIncrementalBarrier() const override {
        return needsBarrier;
    }

  private:
    using Entry = typename Map::Entry;

    static bool entryNeedsSweep(const Entry& prior) {
        Key key(prior.key());
        Value value(prior.value());
        bool result = MapSweepPolicy::needsSweep(&key, &value);
        MOZ_ASSERT(prior.key() == key); // We shouldn't update here.
        MOZ_ASSERT(prior.value() == value); // We shouldn't update here.
        return result;
    }

  public:
    using Lookup = typename Map::Lookup;
    using Ptr = typename Map::Ptr;
    using AddPtr = typename Map::AddPtr;

    struct Range
    {
        explicit Range(const typename Map::Range& r)
          : range(r)
        {
            settle();
        }
        Range() {}

        bool empty() const { return range.empty(); }
        const Entry& front() const { return range.front(); }

        void popFront() {
            range.popFront();
            settle();
        }

      private:
        typename Map::Range range;

        void settle() {
            while (!empty() && entryNeedsSweep(front()))
                popFront();
        }
    };

    struct Enum : public Map::Enum
    {
        explicit Enum(Self& cache)
          : Map::Enum(cache.map)
        {
            // This operation is not allowed while barriers are in place as we
            // may also need to enumerate the set for sweeping.
            MOZ_ASSERT(!cache.needsBarrier);
        }
    };

    bool initialized() const {
        return map.initialized();
    }

    Ptr lookup(const Lookup& l) const {
        Ptr ptr = map.lookup(l);
        if (needsBarrier && ptr && entryNeedsSweep(*ptr)) {
            const_cast<Map&>(map).remove(ptr);
            return Ptr();
        }
        return ptr;
    }

    AddPtr lookupForAdd(const Lookup& l) const {
        AddPtr ptr = map.lookupForAdd(l);
        if (needsBarrier && ptr && entryNeedsSweep(*ptr)) {
            const_cast<Map&>(map).remove(ptr);
            return map.lookupForAdd(l);
        }
        return ptr;
    }

    Range all() const {
        return Range(map.all());
    }

    bool empty() const {
        // This operation is not currently allowed while barriers are in place
        // as it would require iterating the map and the caller expects a
        // constant time operation.
        MOZ_ASSERT(!needsBarrier);
        return map.empty();
    }

    uint32_t count() const {
        // This operation is not currently allowed while barriers are in place
        // as it would require iterating the set and the caller expects a
        // constant time operation.
        MOZ_ASSERT(!needsBarrier);
        return map.count();
    }

    size_t capacity() const {
        return map.capacity();
    }

    bool has(const Lookup& l) const {
        return lookup(l).found();
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return map.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + map.sizeOfExcludingThis(mallocSizeOf);
    }

    bool init(uint32_t len = 16) {
        MOZ_ASSERT(!needsBarrier);
        return map.init(len);
    }

    void clear() {
        // This operation is not currently allowed while barriers are in place
        // since it doesn't make sense to clear a cache while it is being swept.
        MOZ_ASSERT(!needsBarrier);
        map.clear();
    }

    void finish() {
        // This operation is not currently allowed while barriers are in place
        // since it doesn't make sense to destroy a cache while it is being swept.
        MOZ_ASSERT(!needsBarrier);
        map.finish();
    }

    void remove(Ptr p) {
        // This currently supports removing entries during incremental
        // sweeping. If we allow these tables to be swept incrementally this may
        // no longer be possible.
        map.remove(p);
    }

    void remove(const Lookup& l) {
        Ptr p = lookup(l);
        if (p)
            remove(p);
    }

    template<typename KeyInput>
    bool add(AddPtr& p, KeyInput&& k) {
        using mozilla::Forward;
        return map.add(p, Forward<KeyInput>(k));
    }

    template<typename KeyInput, typename ValueInput>
    bool add(AddPtr& p, KeyInput&& k, ValueInput&& v) {
        using mozilla::Forward;
        return map.add(p, Forward<KeyInput>(k), Forward<ValueInput>(v));
    }

    template<typename KeyInput, typename ValueInput>
    bool relookupOrAdd(AddPtr& p, KeyInput&& k, ValueInput&& v) {
        using mozilla::Forward;
        return map.relookupOrAdd(p, Forward<KeyInput>(k), Forward<ValueInput>(v));
    }

    template<typename KeyInput, typename ValueInput>
    bool put(KeyInput&& k, ValueInput&& v) {
        using mozilla::Forward;
        return map.put(Forward<KeyInput>(k), Forward<ValueInput>(v));
    }

    template<typename KeyInput, typename ValueInput>
    bool putNew(KeyInput&& k, ValueInput&& v) {
        using mozilla::Forward;
        return map.putNew(Forward<KeyInput>(k), Forward<ValueInput>(v));
    }
};

// Specialize WeakCache for GCHashSet to provide a barriered set that does not
// need to be swept immediately.
template <typename T, typename HashPolicy, typename AllocPolicy>
class WeakCache<GCHashSet<T, HashPolicy, AllocPolicy>>
    : protected detail::WeakCacheBase
{
    using Set = GCHashSet<T, HashPolicy, AllocPolicy>;
    using Self = WeakCache<Set>;

    Set set;
    bool needsBarrier;

  public:
    using Entry = typename Set::Entry;

    template <typename... Args>
    explicit WeakCache(Zone* zone, Args&&... args)
      : WeakCacheBase(zone), set(mozilla::Forward<Args>(args)...), needsBarrier(false)
    {}
    template <typename... Args>
    explicit WeakCache(JSRuntime* rt, Args&&... args)
      : WeakCacheBase(rt), set(mozilla::Forward<Args>(args)...), needsBarrier(false)
    {}

    size_t sweep() override {
        if (!this->initialized())
            return 0;

        size_t steps = set.count();
        set.sweep();
        return steps;
    }

    bool needsSweep() override {
        return set.needsSweep();
    }

    bool setNeedsIncrementalBarrier(bool needs) override {
        MOZ_ASSERT(needsBarrier != needs);
        needsBarrier = needs;
        return true;
    }

    bool needsIncrementalBarrier() const override {
        return needsBarrier;
    }

  private:
   static bool entryNeedsSweep(const Entry& prior) {
        Entry entry(prior);
        bool result = GCPolicy<T>::needsSweep(&entry);
        MOZ_ASSERT(prior == entry); // We shouldn't update here.
        return result;
    }

  public:
    using Lookup = typename Set::Lookup;
    using Ptr = typename Set::Ptr;
    using AddPtr = typename Set::AddPtr;

    struct Range
    {
        explicit Range(const typename Set::Range& r)
          : range(r)
        {
            settle();
        }
        Range() {}

        bool empty() const { return range.empty(); }
        const Entry& front() const { return range.front(); }

        void popFront() {
            range.popFront();
            settle();
        }

      private:
        typename Set::Range range;

        void settle() {
            while (!empty() && entryNeedsSweep(front()))
                popFront();
        }
    };

    struct Enum : public Set::Enum
    {
        explicit Enum(Self& cache)
          : Set::Enum(cache.set)
        {
            // This operation is not allowed while barriers are in place as we
            // may also need to enumerate the set for sweeping.
            MOZ_ASSERT(!cache.needsBarrier);
        }
    };

    bool initialized() const {
        return set.initialized();
    }

    Ptr lookup(const Lookup& l) const {
        Ptr ptr = set.lookup(l);
        if (needsBarrier && ptr && entryNeedsSweep(*ptr)) {
            const_cast<Set&>(set).remove(ptr);
            return Ptr();
        }
        return ptr;
    }

    AddPtr lookupForAdd(const Lookup& l) const {
        AddPtr ptr = set.lookupForAdd(l);
        if (needsBarrier && ptr && entryNeedsSweep(*ptr)) {
            const_cast<Set&>(set).remove(ptr);
            return set.lookupForAdd(l);
        }
        return ptr;
    }

    Range all() const {
        return Range(set.all());
    }

    bool empty() const {
        // This operation is not currently allowed while barriers are in place
        // as it would require iterating the set and the caller expects a
        // constant time operation.
        MOZ_ASSERT(!needsBarrier);
        return set.empty();
    }

    uint32_t count() const {
        // This operation is not currently allowed while barriers are in place
        // as it would require iterating the set and the caller expects a
        // constant time operation.
        MOZ_ASSERT(!needsBarrier);
        return set.count();
    }

    size_t capacity() const {
        return set.capacity();
    }

    bool has(const Lookup& l) const {
        return lookup(l).found();
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return set.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + set.sizeOfExcludingThis(mallocSizeOf);
    }

    bool init(uint32_t len = 16) {
        MOZ_ASSERT(!needsBarrier);
        return set.init(len);
    }

    void clear() {
        // This operation is not currently allowed while barriers are in place
        // since it doesn't make sense to clear a cache while it is being swept.
        MOZ_ASSERT(!needsBarrier);
        set.clear();
    }

    void finish() {
        // This operation is not currently allowed while barriers are in place
        // since it doesn't make sense to destroy a cache while it is being swept.
        MOZ_ASSERT(!needsBarrier);
        set.finish();
    }

    void remove(Ptr p) {
        // This currently supports removing entries during incremental
        // sweeping. If we allow these tables to be swept incrementally this may
        // no longer be possible.
        set.remove(p);
    }

    void remove(const Lookup& l) {
        Ptr p = lookup(l);
        if (p)
            remove(p);
    }

    template<typename TInput>
    bool add(AddPtr& p, TInput&& t) {
        return set.add(p, mozilla::Forward<TInput>(t));
    }

    template<typename TInput>
    bool relookupOrAdd(AddPtr& p, const Lookup& l, TInput&& t) {
        return set.relookupOrAdd(p, l, mozilla::Forward<TInput>(t));
    }

    template<typename TInput>
    bool put(TInput&& t) {
        return set.put(mozilla::Forward<TInput>(t));
    }

    template<typename TInput>
    bool putNew(TInput&& t) {
        return set.putNew(mozilla::Forward<TInput>(t));
    }

    template<typename TInput>
    bool putNew(const Lookup& l, TInput&& t) {
        return set.putNew(l, mozilla::Forward<TInput>(t));
    }
};

} // namespace JS

#endif /* GCHashTable_h */
