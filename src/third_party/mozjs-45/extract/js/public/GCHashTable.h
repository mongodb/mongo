/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GCHashTable_h
#define GCHashTable_h

#include "js/HashTable.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"

namespace js {

// Define a reasonable default GC policy for GC-aware Maps.
template <typename Key, typename Value>
struct DefaultMapGCPolicy {
    using KeyPolicy = DefaultGCPolicy<Key>;
    using ValuePolicy = DefaultGCPolicy<Value>;

    static bool needsSweep(Key* key, Value* value) {
        return KeyPolicy::needsSweep(key) || ValuePolicy::needsSweep(value);
    }
};

// A GCHashMap is a GC-aware HashMap, meaning that it has additional trace and
// sweep methods that know how to visit all keys and values in the table.
// HashMaps that contain GC pointers will generally want to use this GCHashMap
// specialization in lieu of HashMap, either because those pointers must be
// traced to be kept alive -- in which case, KeyPolicy and/or ValuePolicy
// should do the appropriate tracing -- or because those pointers are weak and
// must be swept during a GC -- in which case needsSweep should be set
// appropriately.
//
// Most types of GC pointers as keys and values can be traced with no extra
// infrastructure. For structs, the DefaultGCPolicy<T> will call a trace()
// method on the struct. For other structs and non-gc-pointer members, ensure
// that there is a specialization of DefaultGCPolicy<T> with an appropriate
// trace() static method available to handle the custom type. Generic helpers
// can be found in js/public/TracingAPI.h.
//
// Note that this HashMap only knows *how* to trace and sweep (and the tracing
// can handle keys that move), but it does not itself cause tracing or sweeping
// to be invoked. For tracing, it must be used with Rooted or PersistentRooted,
// or barriered and traced manually. For sweeping, currently it requires an
// explicit call to <map>.sweep().
//
template <typename Key,
          typename Value,
          typename HashPolicy = DefaultHasher<Key>,
          typename AllocPolicy = TempAllocPolicy,
          typename GCPolicy = DefaultMapGCPolicy<Key, Value>>
class GCHashMap : public HashMap<Key, Value, HashPolicy, AllocPolicy>,
                  public JS::Traceable
{
    using Base = HashMap<Key, Value, HashPolicy, AllocPolicy>;

  public:
    explicit GCHashMap(AllocPolicy a = AllocPolicy()) : Base(a)  {}

    static void trace(GCHashMap* map, JSTracer* trc) { map->trace(trc); }
    void trace(JSTracer* trc) {
        if (!this->initialized())
            return;
        for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
            GCPolicy::ValuePolicy::trace(trc, &e.front().value(), "hashmap value");
            GCPolicy::KeyPolicy::trace(trc, &e.front().mutableKey(), "hashmap key");
        }
    }

    void sweep() {
        if (!this->initialized())
            return;

        for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
            if (GCPolicy::needsSweep(&e.front().mutableKey(), &e.front().value()))
                e.removeFront();
        }
    }

    // GCHashMap is movable
    GCHashMap(GCHashMap&& rhs) : Base(mozilla::Forward<GCHashMap>(rhs)) {}
    void operator=(GCHashMap&& rhs) {
        MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
        Base::operator=(mozilla::Forward<GCHashMap>(rhs));
    }

  private:
    // GCHashMap is not copyable or assignable
    GCHashMap(const GCHashMap& hm) = delete;
    GCHashMap& operator=(const GCHashMap& hm) = delete;
};

template <typename Outer, typename... Args>
class GCHashMapOperations
{
    using Map = GCHashMap<Args...>;
    using Lookup = typename Map::Lookup;
    using Ptr = typename Map::Ptr;
    using AddPtr = typename Map::AddPtr;
    using Range = typename Map::Range;
    using Enum = typename Map::Enum;

    const Map& map() const { return static_cast<const Outer*>(this)->get(); }

  public:
    bool initialized() const                   { return map().initialized(); }
    Ptr lookup(const Lookup& l) const          { return map().lookup(l); }
    AddPtr lookupForAdd(const Lookup& l) const { return map().lookupForAdd(l); }
    Range all() const                          { return map().all(); }
    bool empty() const                         { return map().empty(); }
    uint32_t count() const                     { return map().count(); }
    size_t capacity() const                    { return map().capacity(); }
    bool has(const Lookup& l) const            { return map().lookup(l).found(); }
};

template <typename Outer, typename... Args>
class MutableGCHashMapOperations
  : public GCHashMapOperations<Outer, Args...>
{
    using Map = GCHashMap<Args...>;
    using Lookup = typename Map::Lookup;
    using Ptr = typename Map::Ptr;
    using AddPtr = typename Map::AddPtr;
    using Range = typename Map::Range;
    using Enum = typename Map::Enum;

    Map& map() { return static_cast<Outer*>(this)->get(); }

  public:
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

template <typename A, typename B, typename C, typename D, typename E>
class RootedBase<GCHashMap<A,B,C,D,E>>
  : public MutableGCHashMapOperations<JS::Rooted<GCHashMap<A,B,C,D,E>>, A,B,C,D,E>
{};

template <typename A, typename B, typename C, typename D, typename E>
class MutableHandleBase<GCHashMap<A,B,C,D,E>>
  : public MutableGCHashMapOperations<JS::MutableHandle<GCHashMap<A,B,C,D,E>>, A,B,C,D,E>
{};

template <typename A, typename B, typename C, typename D, typename E>
class HandleBase<GCHashMap<A,B,C,D,E>>
  : public GCHashMapOperations<JS::Handle<GCHashMap<A,B,C,D,E>>, A,B,C,D,E>
{};

// A GCHashSet is a HashSet with an additional trace method that knows
// be traced to be kept alive will generally want to use this GCHashSet
// specializeation in lieu of HashSet.
//
// Most types of GC pointers can be traced with no extra infrastructure. For
// structs and non-gc-pointer members, ensure that there is a specialization of
// DefaultGCPolicy<T> with an appropriate trace method available to handle the
// custom type. Generic helpers can be found in js/public/TracingAPI.h.
//
// Note that although this HashSet's trace will deal correctly with moved
// elements, it does not itself know when to barrier or trace elements. To
// function properly it must either be used with Rooted or barriered and traced
// manually.
template <typename T,
          typename HashPolicy = DefaultHasher<T>,
          typename AllocPolicy = TempAllocPolicy,
          typename GCPolicy = DefaultGCPolicy<T>>
class GCHashSet : public HashSet<T, HashPolicy, AllocPolicy>,
                  public JS::Traceable
{
    using Base = HashSet<T, HashPolicy, AllocPolicy>;

  public:
    explicit GCHashSet(AllocPolicy a = AllocPolicy()) : Base(a)  {}

    static void trace(GCHashSet* set, JSTracer* trc) { set->trace(trc); }
    void trace(JSTracer* trc) {
        if (!this->initialized())
            return;
        for (typename Base::Enum e(*this); !e.empty(); e.popFront())
            GCPolicy::trace(trc, &e.mutableFront(), "hashset element");
    }

    void sweep() {
        if (!this->initialized())
            return;
        for (typename Base::Enum e(*this); !e.empty(); e.popFront()) {
            if (GCPolicy::needsSweep(&e.mutableFront()))
                e.removeFront();
        }
    }

    // GCHashSet is movable
    GCHashSet(GCHashSet&& rhs) : Base(mozilla::Forward<GCHashSet>(rhs)) {}
    void operator=(GCHashSet&& rhs) {
        MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
        Base::operator=(mozilla::Forward<GCHashSet>(rhs));
    }

  private:
    // GCHashSet is not copyable or assignable
    GCHashSet(const GCHashSet& hs) = delete;
    GCHashSet& operator=(const GCHashSet& hs) = delete;
};

template <typename Outer, typename... Args>
class GCHashSetOperations
{
    using Set = GCHashSet<Args...>;
    using Lookup = typename Set::Lookup;
    using Ptr = typename Set::Ptr;
    using AddPtr = typename Set::AddPtr;
    using Range = typename Set::Range;
    using Enum = typename Set::Enum;

    const Set& set() const { return static_cast<const Outer*>(this)->extract(); }

  public:
    bool initialized() const                   { return set().initialized(); }
    Ptr lookup(const Lookup& l) const          { return set().lookup(l); }
    AddPtr lookupForAdd(const Lookup& l) const { return set().lookupForAdd(l); }
    Range all() const                          { return set().all(); }
    bool empty() const                         { return set().empty(); }
    uint32_t count() const                     { return set().count(); }
    size_t capacity() const                    { return set().capacity(); }
    bool has(const Lookup& l) const            { return set().lookup(l).found(); }
};

template <typename Outer, typename... Args>
class MutableGCHashSetOperations
  : public GCHashSetOperations<Outer, Args...>
{
    using Set = GCHashSet<Args...>;
    using Lookup = typename Set::Lookup;
    using Ptr = typename Set::Ptr;
    using AddPtr = typename Set::AddPtr;
    using Range = typename Set::Range;
    using Enum = typename Set::Enum;

    Set& set() { return static_cast<Outer*>(this)->extract(); }

  public:
    bool init(uint32_t len = 16) { return set().init(len); }
    void clear()                 { set().clear(); }
    void finish()                { set().finish(); }
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

template <typename T, typename HP, typename AP, typename GP>
class RootedBase<GCHashSet<T, HP, AP, GP>>
  : public MutableGCHashSetOperations<JS::Rooted<GCHashSet<T, HP, AP, GP>>, T, HP, AP, GP>
{
    using Set = GCHashSet<T, HP, AP, GP>;

    friend class GCHashSetOperations<JS::Rooted<Set>, T, HP, AP, GP>;
    const Set& extract() const { return *static_cast<const JS::Rooted<Set>*>(this)->address(); }

    friend class MutableGCHashSetOperations<JS::Rooted<Set>, T, HP, AP, GP>;
    Set& extract() { return *static_cast<JS::Rooted<Set>*>(this)->address(); }
};

template <typename T, typename HP, typename AP, typename GP>
class MutableHandleBase<GCHashSet<T, HP, AP, GP>>
  : public MutableGCHashSetOperations<JS::MutableHandle<GCHashSet<T, HP, AP, GP>>, T, HP, AP, GP>
{
    using Set = GCHashSet<T, HP, AP, GP>;

    friend class GCHashSetOperations<JS::MutableHandle<Set>, T, HP, AP, GP>;
    const Set& extract() const {
        return *static_cast<const JS::MutableHandle<Set>*>(this)->address();
    }

    friend class MutableGCHashSetOperations<JS::MutableHandle<Set>, T, HP, AP, GP>;
    Set& extract() { return *static_cast<JS::MutableHandle<Set>*>(this)->address(); }
};

template <typename T, typename HP, typename AP, typename GP>
class HandleBase<GCHashSet<T, HP, AP, GP>>
  : public GCHashSetOperations<JS::Handle<GCHashSet<T, HP, AP, GP>>, T, HP, AP, GP>
{
    using Set = GCHashSet<T, HP, AP, GP>;
    friend class GCHashSetOperations<JS::Handle<Set>, T, HP, AP, GP>;
    const Set& extract() const { return *static_cast<const JS::Handle<Set>*>(this)->address(); }
};

} /* namespace js */

#endif /* GCHashTable_h */
