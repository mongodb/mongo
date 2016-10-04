/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_HashTable_h
#define js_HashTable_h

#include "mozilla/Alignment.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"
#include "mozilla/PodOperations.h"
#include "mozilla/ReentrancyGuard.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/TypeTraits.h"
#include "mozilla/UniquePtr.h"

#include "js/Utility.h"

namespace js {

class TempAllocPolicy;
template <class> struct DefaultHasher;
template <class, class> class HashMapEntry;
namespace detail {
    template <class T> class HashTableEntry;
    template <class T, class HashPolicy, class AllocPolicy> class HashTable;
} // namespace detail

/*****************************************************************************/

// A JS-friendly, STL-like container providing a hash-based map from keys to
// values. In particular, HashMap calls constructors and destructors of all
// objects added so non-PODs may be used safely.
//
// Key/Value requirements:
//  - movable, destructible, assignable
// HashPolicy requirements:
//  - see Hash Policy section below
// AllocPolicy:
//  - see jsalloc.h
//
// Note:
// - HashMap is not reentrant: Key/Value/HashPolicy/AllocPolicy members
//   called by HashMap must not call back into the same HashMap object.
// - Due to the lack of exception handling, the user must call |init()|.
template <class Key,
          class Value,
          class HashPolicy = DefaultHasher<Key>,
          class AllocPolicy = TempAllocPolicy>
class HashMap
{
    typedef HashMapEntry<Key, Value> TableEntry;

    struct MapHashPolicy : HashPolicy
    {
        typedef Key KeyType;
        static const Key& getKey(TableEntry& e) { return e.key(); }
        static void setKey(TableEntry& e, Key& k) { HashPolicy::rekey(e.mutableKey(), k); }
    };

    typedef detail::HashTable<TableEntry, MapHashPolicy, AllocPolicy> Impl;
    Impl impl;

  public:
    typedef typename HashPolicy::Lookup Lookup;
    typedef TableEntry Entry;

    // HashMap construction is fallible (due to OOM); thus the user must call
    // init after constructing a HashMap and check the return value.
    explicit HashMap(AllocPolicy a = AllocPolicy()) : impl(a)  {}
    bool init(uint32_t len = 16)                      { return impl.init(len); }
    bool initialized() const                          { return impl.initialized(); }

    // Return whether the given lookup value is present in the map. E.g.:
    //
    //   typedef HashMap<int,char> HM;
    //   HM h;
    //   if (HM::Ptr p = h.lookup(3)) {
    //     const HM::Entry& e = *p; // p acts like a pointer to Entry
    //     assert(p->key == 3);     // Entry contains the key
    //     char val = p->value;     // and value
    //   }
    //
    // Also see the definition of Ptr in HashTable above (with T = Entry).
    typedef typename Impl::Ptr Ptr;
    Ptr lookup(const Lookup& l) const                 { return impl.lookup(l); }

    // Like lookup, but does not assert if two threads call lookup at the same
    // time. Only use this method when none of the threads will modify the map.
    Ptr readonlyThreadsafeLookup(const Lookup& l) const { return impl.readonlyThreadsafeLookup(l); }

    // Assuming |p.found()|, remove |*p|.
    void remove(Ptr p)                                { impl.remove(p); }

    // Like |lookup(l)|, but on miss, |p = lookupForAdd(l)| allows efficient
    // insertion of Key |k| (where |HashPolicy::match(k,l) == true|) using
    // |add(p,k,v)|. After |add(p,k,v)|, |p| points to the new Entry. E.g.:
    //
    //   typedef HashMap<int,char> HM;
    //   HM h;
    //   HM::AddPtr p = h.lookupForAdd(3);
    //   if (!p) {
    //     if (!h.add(p, 3, 'a'))
    //       return false;
    //   }
    //   const HM::Entry& e = *p;   // p acts like a pointer to Entry
    //   assert(p->key == 3);       // Entry contains the key
    //   char val = p->value;       // and value
    //
    // Also see the definition of AddPtr in HashTable above (with T = Entry).
    //
    // N.B. The caller must ensure that no mutating hash table operations
    // occur between a pair of |lookupForAdd| and |add| calls. To avoid
    // looking up the key a second time, the caller may use the more efficient
    // relookupOrAdd method. This method reuses part of the hashing computation
    // to more efficiently insert the key if it has not been added. For
    // example, a mutation-handling version of the previous example:
    //
    //    HM::AddPtr p = h.lookupForAdd(3);
    //    if (!p) {
    //      call_that_may_mutate_h();
    //      if (!h.relookupOrAdd(p, 3, 'a'))
    //        return false;
    //    }
    //    const HM::Entry& e = *p;
    //    assert(p->key == 3);
    //    char val = p->value;
    typedef typename Impl::AddPtr AddPtr;
    AddPtr lookupForAdd(const Lookup& l) const {
        return impl.lookupForAdd(l);
    }

    template<typename KeyInput, typename ValueInput>
    bool add(AddPtr& p, KeyInput&& k, ValueInput&& v) {
        return impl.add(p,
                        mozilla::Forward<KeyInput>(k),
                        mozilla::Forward<ValueInput>(v));
    }

    template<typename KeyInput>
    bool add(AddPtr& p, KeyInput&& k) {
        return impl.add(p, mozilla::Forward<KeyInput>(k), Value());
    }

    template<typename KeyInput, typename ValueInput>
    bool relookupOrAdd(AddPtr& p, KeyInput&& k, ValueInput&& v) {
        return impl.relookupOrAdd(p, k,
                                  mozilla::Forward<KeyInput>(k),
                                  mozilla::Forward<ValueInput>(v));
    }

    // |all()| returns a Range containing |count()| elements. E.g.:
    //
    //   typedef HashMap<int,char> HM;
    //   HM h;
    //   for (HM::Range r = h.all(); !r.empty(); r.popFront())
    //     char c = r.front().value();
    //
    // Also see the definition of Range in HashTable above (with T = Entry).
    typedef typename Impl::Range Range;
    Range all() const                                 { return impl.all(); }

    // Typedef for the enumeration class. An Enum may be used to examine and
    // remove table entries:
    //
    //   typedef HashMap<int,char> HM;
    //   HM s;
    //   for (HM::Enum e(s); !e.empty(); e.popFront())
    //     if (e.front().value() == 'l')
    //       e.removeFront();
    //
    // Table resize may occur in Enum's destructor. Also see the definition of
    // Enum in HashTable above (with T = Entry).
    typedef typename Impl::Enum Enum;

    // Remove all entries. This does not shrink the table. For that consider
    // using the finish() method.
    void clear()                                      { impl.clear(); }

    // Remove all the entries and release all internal buffers. The map must
    // be initialized again before any use.
    void finish()                                     { impl.finish(); }

    // Does the table contain any entries?
    bool empty() const                                { return impl.empty(); }

    // Number of live elements in the map.
    uint32_t count() const                            { return impl.count(); }

    // Total number of allocation in the dynamic table. Note: resize will
    // happen well before count() == capacity().
    size_t capacity() const                           { return impl.capacity(); }

    // Don't just call |impl.sizeOfExcludingThis()| because there's no
    // guarantee that |impl| is the first field in HashMap.
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return impl.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + impl.sizeOfExcludingThis(mallocSizeOf);
    }

    // If |generation()| is the same before and after a HashMap operation,
    // pointers into the table remain valid.
    uint32_t generation() const                       { return impl.generation(); }

    /************************************************** Shorthand operations */

    bool has(const Lookup& l) const {
        return impl.lookup(l).found();
    }

    // Overwrite existing value with v. Return false on oom.
    template<typename KeyInput, typename ValueInput>
    bool put(KeyInput&& k, ValueInput&& v) {
        AddPtr p = lookupForAdd(k);
        if (p) {
            p->value() = mozilla::Forward<ValueInput>(v);
            return true;
        }
        return add(p, mozilla::Forward<KeyInput>(k), mozilla::Forward<ValueInput>(v));
    }

    // Like put, but assert that the given key is not already present.
    template<typename KeyInput, typename ValueInput>
    bool putNew(KeyInput&& k, ValueInput&& v) {
        return impl.putNew(k, mozilla::Forward<KeyInput>(k), mozilla::Forward<ValueInput>(v));
    }

    // Only call this to populate an empty map after reserving space with init().
    template<typename KeyInput, typename ValueInput>
    void putNewInfallible(KeyInput&& k, ValueInput&& v) {
        impl.putNewInfallible(k, mozilla::Forward<KeyInput>(k), mozilla::Forward<ValueInput>(v));
    }

    // Add (k,defaultValue) if |k| is not found. Return a false-y Ptr on oom.
    Ptr lookupWithDefault(const Key& k, const Value& defaultValue) {
        AddPtr p = lookupForAdd(k);
        if (p)
            return p;
        (void)add(p, k, defaultValue);  // p is left false-y on oom.
        return p;
    }

    // Remove if present.
    void remove(const Lookup& l) {
        if (Ptr p = lookup(l))
            remove(p);
    }

    // Infallibly rekey one entry, if necessary.
    // Requires template parameters Key and HashPolicy::Lookup to be the same type.
    void rekeyIfMoved(const Key& old_key, const Key& new_key) {
        if (old_key != new_key)
            rekeyAs(old_key, new_key, new_key);
    }

    // Infallibly rekey one entry if present, and return whether that happened.
    bool rekeyAs(const Lookup& old_lookup, const Lookup& new_lookup, const Key& new_key) {
        if (Ptr p = lookup(old_lookup)) {
            impl.rekeyAndMaybeRehash(p, new_lookup, new_key);
            return true;
        }
        return false;
    }

    // HashMap is movable
    HashMap(HashMap&& rhs) : impl(mozilla::Move(rhs.impl)) {}
    void operator=(HashMap&& rhs) {
        MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
        impl = mozilla::Move(rhs.impl);
    }

  private:
    // HashMap is not copyable or assignable
    HashMap(const HashMap& hm) = delete;
    HashMap& operator=(const HashMap& hm) = delete;

    friend class Impl::Enum;
};

/*****************************************************************************/

// A JS-friendly, STL-like container providing a hash-based set of values. In
// particular, HashSet calls constructors and destructors of all objects added
// so non-PODs may be used safely.
//
// T requirements:
//  - movable, destructible, assignable
// HashPolicy requirements:
//  - see Hash Policy section below
// AllocPolicy:
//  - see jsalloc.h
//
// Note:
// - HashSet is not reentrant: T/HashPolicy/AllocPolicy members called by
//   HashSet must not call back into the same HashSet object.
// - Due to the lack of exception handling, the user must call |init()|.
template <class T,
          class HashPolicy = DefaultHasher<T>,
          class AllocPolicy = TempAllocPolicy>
class HashSet
{
    struct SetOps : HashPolicy
    {
        typedef T KeyType;
        static const KeyType& getKey(const T& t) { return t; }
        static void setKey(T& t, KeyType& k) { HashPolicy::rekey(t, k); }
    };

    typedef detail::HashTable<const T, SetOps, AllocPolicy> Impl;
    Impl impl;

  public:
    typedef typename HashPolicy::Lookup Lookup;
    typedef T Entry;

    // HashSet construction is fallible (due to OOM); thus the user must call
    // init after constructing a HashSet and check the return value.
    explicit HashSet(AllocPolicy a = AllocPolicy()) : impl(a)  {}
    bool init(uint32_t len = 16)                      { return impl.init(len); }
    bool initialized() const                          { return impl.initialized(); }

    // Return whether the given lookup value is present in the map. E.g.:
    //
    //   typedef HashSet<int> HS;
    //   HS h;
    //   if (HS::Ptr p = h.lookup(3)) {
    //     assert(*p == 3);   // p acts like a pointer to int
    //   }
    //
    // Also see the definition of Ptr in HashTable above.
    typedef typename Impl::Ptr Ptr;
    Ptr lookup(const Lookup& l) const                 { return impl.lookup(l); }

    // Like lookup, but does not assert if two threads call lookup at the same
    // time. Only use this method when none of the threads will modify the map.
    Ptr readonlyThreadsafeLookup(const Lookup& l) const { return impl.readonlyThreadsafeLookup(l); }

    // Assuming |p.found()|, remove |*p|.
    void remove(Ptr p)                                { impl.remove(p); }

    // Like |lookup(l)|, but on miss, |p = lookupForAdd(l)| allows efficient
    // insertion of T value |t| (where |HashPolicy::match(t,l) == true|) using
    // |add(p,t)|. After |add(p,t)|, |p| points to the new element. E.g.:
    //
    //   typedef HashSet<int> HS;
    //   HS h;
    //   HS::AddPtr p = h.lookupForAdd(3);
    //   if (!p) {
    //     if (!h.add(p, 3))
    //       return false;
    //   }
    //   assert(*p == 3);   // p acts like a pointer to int
    //
    // Also see the definition of AddPtr in HashTable above.
    //
    // N.B. The caller must ensure that no mutating hash table operations
    // occur between a pair of |lookupForAdd| and |add| calls. To avoid
    // looking up the key a second time, the caller may use the more efficient
    // relookupOrAdd method. This method reuses part of the hashing computation
    // to more efficiently insert the key if it has not been added. For
    // example, a mutation-handling version of the previous example:
    //
    //    HS::AddPtr p = h.lookupForAdd(3);
    //    if (!p) {
    //      call_that_may_mutate_h();
    //      if (!h.relookupOrAdd(p, 3, 3))
    //        return false;
    //    }
    //    assert(*p == 3);
    //
    // Note that relookupOrAdd(p,l,t) performs Lookup using |l| and adds the
    // entry |t|, where the caller ensures match(l,t).
    typedef typename Impl::AddPtr AddPtr;
    AddPtr lookupForAdd(const Lookup& l) const        { return impl.lookupForAdd(l); }

    template <typename U>
    bool add(AddPtr& p, U&& u) {
        return impl.add(p, mozilla::Forward<U>(u));
    }

    template <typename U>
    bool relookupOrAdd(AddPtr& p, const Lookup& l, U&& u) {
        return impl.relookupOrAdd(p, l, mozilla::Forward<U>(u));
    }

    // |all()| returns a Range containing |count()| elements:
    //
    //   typedef HashSet<int> HS;
    //   HS h;
    //   for (HS::Range r = h.all(); !r.empty(); r.popFront())
    //     int i = r.front();
    //
    // Also see the definition of Range in HashTable above.
    typedef typename Impl::Range Range;
    Range all() const                                 { return impl.all(); }

    // Typedef for the enumeration class. An Enum may be used to examine and
    // remove table entries:
    //
    //   typedef HashSet<int> HS;
    //   HS s;
    //   for (HS::Enum e(s); !e.empty(); e.popFront())
    //     if (e.front() == 42)
    //       e.removeFront();
    //
    // Table resize may occur in Enum's destructor. Also see the definition of
    // Enum in HashTable above.
    typedef typename Impl::Enum Enum;

    // Remove all entries. This does not shrink the table. For that consider
    // using the finish() method.
    void clear()                                      { impl.clear(); }

    // Remove all the entries and release all internal buffers. The set must
    // be initialized again before any use.
    void finish()                                     { impl.finish(); }

    // Does the table contain any entries?
    bool empty() const                                { return impl.empty(); }

    // Number of live elements in the map.
    uint32_t count() const                            { return impl.count(); }

    // Total number of allocation in the dynamic table. Note: resize will
    // happen well before count() == capacity().
    size_t capacity() const                           { return impl.capacity(); }

    // Don't just call |impl.sizeOfExcludingThis()| because there's no
    // guarantee that |impl| is the first field in HashSet.
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return impl.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + impl.sizeOfExcludingThis(mallocSizeOf);
    }

    // If |generation()| is the same before and after a HashSet operation,
    // pointers into the table remain valid.
    uint32_t generation() const                       { return impl.generation(); }

    /************************************************** Shorthand operations */

    bool has(const Lookup& l) const {
        return impl.lookup(l).found();
    }

    // Add |u| if it is not present already. Return false on oom.
    template <typename U>
    bool put(U&& u) {
        AddPtr p = lookupForAdd(u);
        return p ? true : add(p, mozilla::Forward<U>(u));
    }

    // Like put, but assert that the given key is not already present.
    template <typename U>
    bool putNew(U&& u) {
        return impl.putNew(u, mozilla::Forward<U>(u));
    }

    template <typename U>
    bool putNew(const Lookup& l, U&& u) {
        return impl.putNew(l, mozilla::Forward<U>(u));
    }

    // Only call this to populate an empty set after reserving space with init().
    template <typename U>
    void putNewInfallible(const Lookup& l, U&& u) {
        impl.putNewInfallible(l, mozilla::Forward<U>(u));
    }

    void remove(const Lookup& l) {
        if (Ptr p = lookup(l))
            remove(p);
    }

    // Infallibly rekey one entry, if present.
    // Requires template parameters T and HashPolicy::Lookup to be the same type.
    void rekeyIfMoved(const Lookup& old_value, const T& new_value) {
        if (old_value != new_value)
            rekeyAs(old_value, new_value, new_value);
    }

    // Infallibly rekey one entry if present, and return whether that happened.
    bool rekeyAs(const Lookup& old_lookup, const Lookup& new_lookup, const T& new_value) {
        if (Ptr p = lookup(old_lookup)) {
            impl.rekeyAndMaybeRehash(p, new_lookup, new_value);
            return true;
        }
        return false;
    }

    // Infallibly rekey one entry with a new key that is equivalent.
    void rekeyInPlace(Ptr p, const T& new_value)
    {
        MOZ_ASSERT(HashPolicy::match(*p, new_value));
        impl.rekeyInPlace(p, new_value);
    }

    // HashSet is movable
    HashSet(HashSet&& rhs) : impl(mozilla::Move(rhs.impl)) {}
    void operator=(HashSet&& rhs) {
        MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
        impl = mozilla::Move(rhs.impl);
    }

  private:
    // HashSet is not copyable or assignable
    HashSet(const HashSet& hs) = delete;
    HashSet& operator=(const HashSet& hs) = delete;

    friend class Impl::Enum;
};

/*****************************************************************************/

// Hash Policy
//
// A hash policy P for a hash table with key-type Key must provide:
//  - a type |P::Lookup| to use to lookup table entries;
//  - a static member function |P::hash| with signature
//
//      static js::HashNumber hash(Lookup)
//
//    to use to hash the lookup type; and
//  - a static member function |P::match| with signature
//
//      static bool match(Key, Lookup)
//
//    to use to test equality of key and lookup values.
//
// Normally, Lookup = Key. In general, though, different values and types of
// values can be used to lookup and store. If a Lookup value |l| is != to the
// added Key value |k|, the user must ensure that |P::match(k,l)|. E.g.:
//
//   js::HashSet<Key, P>::AddPtr p = h.lookup(l);
//   if (!p) {
//     assert(P::match(k, l));  // must hold
//     h.add(p, k);
//   }

// Pointer hashing policy that strips the lowest zeroBits when calculating the
// hash to improve key distribution.
template <typename Key, size_t zeroBits>
struct PointerHasher
{
    typedef Key Lookup;
    static HashNumber hash(const Lookup& l) {
        size_t word = reinterpret_cast<size_t>(l) >> zeroBits;
        static_assert(sizeof(HashNumber) == 4,
                      "subsequent code assumes a four-byte hash");
#if JS_BITS_PER_WORD == 32
        return HashNumber(word);
#else
        static_assert(sizeof(word) == 8,
                      "unexpected word size, new hashing strategy required to "
                      "properly incorporate all bits");
        return HashNumber((word >> 32) ^ word);
#endif
    }
    static bool match(const Key& k, const Lookup& l) {
        return k == l;
    }
    static void rekey(Key& k, const Key& newKey) {
        k = newKey;
    }
};

// Default hash policy: just use the 'lookup' value. This of course only
// works if the lookup value is integral. HashTable applies ScrambleHashCode to
// the result of the 'hash' which means that it is 'ok' if the lookup value is
// not well distributed over the HashNumber domain.
template <class Key>
struct DefaultHasher
{
    typedef Key Lookup;
    static HashNumber hash(const Lookup& l) {
        // Hash if can implicitly cast to hash number type.
        return l;
    }
    static bool match(const Key& k, const Lookup& l) {
        // Use builtin or overloaded operator==.
        return k == l;
    }
    static void rekey(Key& k, const Key& newKey) {
        k = newKey;
    }
};

// Specialize hashing policy for pointer types. It assumes that the type is
// at least word-aligned. For types with smaller size use PointerHasher.
template <class T>
struct DefaultHasher<T*> : PointerHasher<T*, mozilla::tl::FloorLog2<sizeof(void*)>::value>
{};

// Specialize hashing policy for mozilla::UniquePtr<T> to proxy the UniquePtr's
// raw pointer to PointerHasher.
template <class T>
struct DefaultHasher<mozilla::UniquePtr<T>>
{
    using Lookup = mozilla::UniquePtr<T>;
    using PtrHasher = PointerHasher<T*, mozilla::tl::FloorLog2<sizeof(void*)>::value>;

    static HashNumber hash(const Lookup& l) {
        return PtrHasher::hash(l.get());
    }
    static bool match(const mozilla::UniquePtr<T>& k, const Lookup& l) {
        return PtrHasher::match(k.get(), l.get());
    }
    static void rekey(mozilla::UniquePtr<T>& k, mozilla::UniquePtr<T>&& newKey) {
        k = mozilla::Move(newKey);
    }
};

// For doubles, we can xor the two uint32s.
template <>
struct DefaultHasher<double>
{
    typedef double Lookup;
    static HashNumber hash(double d) {
        static_assert(sizeof(HashNumber) == 4,
                      "subsequent code assumes a four-byte hash");
        uint64_t u = mozilla::BitwiseCast<uint64_t>(d);
        return HashNumber(u ^ (u >> 32));
    }
    static bool match(double lhs, double rhs) {
        return mozilla::BitwiseCast<uint64_t>(lhs) == mozilla::BitwiseCast<uint64_t>(rhs);
    }
};

template <>
struct DefaultHasher<float>
{
    typedef float Lookup;
    static HashNumber hash(float f) {
        static_assert(sizeof(HashNumber) == 4,
                      "subsequent code assumes a four-byte hash");
        return HashNumber(mozilla::BitwiseCast<uint32_t>(f));
    }
    static bool match(float lhs, float rhs) {
        return mozilla::BitwiseCast<uint32_t>(lhs) == mozilla::BitwiseCast<uint32_t>(rhs);
    }
};

/*****************************************************************************/

// Both HashMap and HashSet are implemented by a single HashTable that is even
// more heavily parameterized than the other two. This leaves HashTable gnarly
// and extremely coupled to HashMap and HashSet; thus code should not use
// HashTable directly.

template <class Key, class Value>
class HashMapEntry
{
    Key key_;
    Value value_;

    template <class, class, class> friend class detail::HashTable;
    template <class> friend class detail::HashTableEntry;
    template <class, class, class, class> friend class HashMap;

  public:
    template<typename KeyInput, typename ValueInput>
    HashMapEntry(KeyInput&& k, ValueInput&& v)
      : key_(mozilla::Forward<KeyInput>(k)),
        value_(mozilla::Forward<ValueInput>(v))
    {}

    HashMapEntry(HashMapEntry&& rhs)
      : key_(mozilla::Move(rhs.key_)),
        value_(mozilla::Move(rhs.value_))
    {}

    typedef Key KeyType;
    typedef Value ValueType;

    const Key& key() const { return key_; }
    Key& mutableKey() { return key_; }
    const Value& value() const { return value_; }
    Value& value() { return value_; }

  private:
    HashMapEntry(const HashMapEntry&) = delete;
    void operator=(const HashMapEntry&) = delete;
};

} // namespace js

namespace mozilla {

template <typename T>
struct IsPod<js::detail::HashTableEntry<T> > : IsPod<T> {};

template <typename K, typename V>
struct IsPod<js::HashMapEntry<K, V> >
  : IntegralConstant<bool, IsPod<K>::value && IsPod<V>::value>
{};

} // namespace mozilla

namespace js {

namespace detail {

template <class T, class HashPolicy, class AllocPolicy>
class HashTable;

template <class T>
class HashTableEntry
{
    template <class, class, class> friend class HashTable;
    typedef typename mozilla::RemoveConst<T>::Type NonConstT;

    HashNumber keyHash;
    mozilla::AlignedStorage2<NonConstT> mem;

    static const HashNumber sFreeKey = 0;
    static const HashNumber sRemovedKey = 1;
    static const HashNumber sCollisionBit = 1;

    static bool isLiveHash(HashNumber hash)
    {
        return hash > sRemovedKey;
    }

    HashTableEntry(const HashTableEntry&) = delete;
    void operator=(const HashTableEntry&) = delete;
    ~HashTableEntry() = delete;

  public:
    // NB: HashTableEntry is treated as a POD: no constructor or destructor calls.

    void destroyIfLive() {
        if (isLive())
            mem.addr()->~T();
    }

    void destroy() {
        MOZ_ASSERT(isLive());
        mem.addr()->~T();
    }

    void swap(HashTableEntry* other) {
        mozilla::Swap(keyHash, other->keyHash);
        mozilla::Swap(mem, other->mem);
    }

    T& get() { MOZ_ASSERT(isLive()); return *mem.addr(); }
    NonConstT& getMutable() { MOZ_ASSERT(isLive()); return *mem.addr(); }

    bool isFree() const    { return keyHash == sFreeKey; }
    void clearLive()       { MOZ_ASSERT(isLive()); keyHash = sFreeKey; mem.addr()->~T(); }
    void clear()           { if (isLive()) mem.addr()->~T(); keyHash = sFreeKey; }
    bool isRemoved() const { return keyHash == sRemovedKey; }
    void removeLive()      { MOZ_ASSERT(isLive()); keyHash = sRemovedKey; mem.addr()->~T(); }
    bool isLive() const    { return isLiveHash(keyHash); }
    void setCollision()               { MOZ_ASSERT(isLive()); keyHash |= sCollisionBit; }
    void unsetCollision()             { keyHash &= ~sCollisionBit; }
    bool hasCollision() const         { return keyHash & sCollisionBit; }
    bool matchHash(HashNumber hn)     { return (keyHash & ~sCollisionBit) == hn; }
    HashNumber getKeyHash() const     { return keyHash & ~sCollisionBit; }

    template <typename... Args>
    void setLive(HashNumber hn, Args&&... args)
    {
        MOZ_ASSERT(!isLive());
        keyHash = hn;
        new(mem.addr()) T(mozilla::Forward<Args>(args)...);
        MOZ_ASSERT(isLive());
    }
};

template <class T, class HashPolicy, class AllocPolicy>
class HashTable : private AllocPolicy
{
    friend class mozilla::ReentrancyGuard;

    typedef typename mozilla::RemoveConst<T>::Type NonConstT;
    typedef typename HashPolicy::KeyType Key;
    typedef typename HashPolicy::Lookup Lookup;

  public:
    typedef HashTableEntry<T> Entry;

    // A nullable pointer to a hash table element. A Ptr |p| can be tested
    // either explicitly |if (p.found()) p->...| or using boolean conversion
    // |if (p) p->...|. Ptr objects must not be used after any mutating hash
    // table operations unless |generation()| is tested.
    class Ptr
    {
        friend class HashTable;

        Entry* entry_;
#ifdef JS_DEBUG
        const HashTable* table_;
        uint32_t generation;
#endif

      protected:
        Ptr(Entry& entry, const HashTable& tableArg)
          : entry_(&entry)
#ifdef JS_DEBUG
          , table_(&tableArg)
          , generation(tableArg.generation())
#endif
        {}

      public:
        // Leaves Ptr uninitialized.
        Ptr() {
#ifdef JS_DEBUG
            entry_ = (Entry*)0xbad;
#endif
        }

        bool found() const {
#ifdef JS_DEBUG
            MOZ_ASSERT(generation == table_->generation());
#endif
            return entry_->isLive();
        }

        explicit operator bool() const {
            return found();
        }

        bool operator==(const Ptr& rhs) const {
            MOZ_ASSERT(found() && rhs.found());
            return entry_ == rhs.entry_;
        }

        bool operator!=(const Ptr& rhs) const {
#ifdef JS_DEBUG
            MOZ_ASSERT(generation == table_->generation());
#endif
            return !(*this == rhs);
        }

        T& operator*() const {
#ifdef JS_DEBUG
            MOZ_ASSERT(generation == table_->generation());
#endif
            return entry_->get();
        }

        T* operator->() const {
#ifdef JS_DEBUG
            MOZ_ASSERT(generation == table_->generation());
#endif
            return &entry_->get();
        }
    };

    // A Ptr that can be used to add a key after a failed lookup.
    class AddPtr : public Ptr
    {
        friend class HashTable;
        HashNumber keyHash;
#ifdef JS_DEBUG
        uint64_t mutationCount;
#endif

        AddPtr(Entry& entry, const HashTable& tableArg, HashNumber hn)
          : Ptr(entry, tableArg)
          , keyHash(hn)
#ifdef JS_DEBUG
          , mutationCount(tableArg.mutationCount)
#endif
        {}

      public:
        // Leaves AddPtr uninitialized.
        AddPtr() {}
    };

    // A collection of hash table entries. The collection is enumerated by
    // calling |front()| followed by |popFront()| as long as |!empty()|. As
    // with Ptr/AddPtr, Range objects must not be used after any mutating hash
    // table operation unless the |generation()| is tested.
    class Range
    {
      protected:
        friend class HashTable;

        Range(const HashTable& tableArg, Entry* c, Entry* e)
          : cur(c)
          , end(e)
#ifdef JS_DEBUG
          , table_(&tableArg)
          , mutationCount(tableArg.mutationCount)
          , generation(tableArg.generation())
          , validEntry(true)
#endif
        {
            while (cur < end && !cur->isLive())
                ++cur;
        }

        Entry* cur;
        Entry* end;
#ifdef JS_DEBUG
        const HashTable* table_;
        uint64_t mutationCount;
        uint32_t generation;
        bool validEntry;
#endif

      public:
        Range()
          : cur(nullptr)
          , end(nullptr)
#ifdef JS_DEBUG
          , table_(nullptr)
          , mutationCount(0)
          , generation(0)
          , validEntry(false)
#endif
        {}

        bool empty() const {
#ifdef JS_DEBUG
            MOZ_ASSERT(generation == table_->generation());
            MOZ_ASSERT(mutationCount == table_->mutationCount);
#endif
            return cur == end;
        }

        T& front() const {
            MOZ_ASSERT(!empty());
#ifdef JS_DEBUG
            MOZ_ASSERT(validEntry);
            MOZ_ASSERT(generation == table_->generation());
            MOZ_ASSERT(mutationCount == table_->mutationCount);
#endif
            return cur->get();
        }

        void popFront() {
            MOZ_ASSERT(!empty());
#ifdef JS_DEBUG
            MOZ_ASSERT(generation == table_->generation());
            MOZ_ASSERT(mutationCount == table_->mutationCount);
#endif
            while (++cur < end && !cur->isLive())
                continue;
#ifdef JS_DEBUG
            validEntry = true;
#endif
        }
    };

    // A Range whose lifetime delimits a mutating enumeration of a hash table.
    // Since rehashing when elements were removed during enumeration would be
    // bad, it is postponed until the Enum is destructed.  Since the Enum's
    // destructor touches the hash table, the user must ensure that the hash
    // table is still alive when the destructor runs.
    class Enum : public Range
    {
        friend class HashTable;

        HashTable& table_;
        bool rekeyed;
        bool removed;

        /* Not copyable. */
        Enum(const Enum&) = delete;
        void operator=(const Enum&) = delete;

      public:
        template<class Map> explicit
        Enum(Map& map) : Range(map.all()), table_(map.impl), rekeyed(false), removed(false) {}

        // Removes the |front()| element from the table, leaving |front()|
        // invalid until the next call to |popFront()|. For example:
        //
        //   HashSet<int> s;
        //   for (HashSet<int>::Enum e(s); !e.empty(); e.popFront())
        //     if (e.front() == 42)
        //       e.removeFront();
        void removeFront() {
            table_.remove(*this->cur);
            removed = true;
#ifdef JS_DEBUG
            this->validEntry = false;
            this->mutationCount = table_.mutationCount;
#endif
        }

        NonConstT& mutableFront() {
            MOZ_ASSERT(!this->empty());
#ifdef JS_DEBUG
            MOZ_ASSERT(this->validEntry);
            MOZ_ASSERT(this->generation == this->Range::table_->generation());
            MOZ_ASSERT(this->mutationCount == this->Range::table_->mutationCount);
#endif
            return this->cur->getMutable();
        }

        // Removes the |front()| element and re-inserts it into the table with
        // a new key at the new Lookup position.  |front()| is invalid after
        // this operation until the next call to |popFront()|.
        void rekeyFront(const Lookup& l, const Key& k) {
            MOZ_ASSERT(&k != &HashPolicy::getKey(this->cur->get()));
            Ptr p(*this->cur, table_);
            table_.rekeyWithoutRehash(p, l, k);
            rekeyed = true;
#ifdef JS_DEBUG
            this->validEntry = false;
            this->mutationCount = table_.mutationCount;
#endif
        }

        void rekeyFront(const Key& k) {
            rekeyFront(k, k);
        }

        // Potentially rehashes the table.
        ~Enum() {
            if (rekeyed) {
                table_.gen++;
                table_.checkOverRemoved();
            }

            if (removed)
                table_.compactIfUnderloaded();
        }
    };

    // HashTable is movable
    HashTable(HashTable&& rhs)
      : AllocPolicy(rhs)
    {
        mozilla::PodAssign(this, &rhs);
        rhs.table = nullptr;
    }
    void operator=(HashTable&& rhs) {
        MOZ_ASSERT(this != &rhs, "self-move assignment is prohibited");
        if (table)
            destroyTable(*this, table, capacity());
        mozilla::PodAssign(this, &rhs);
        rhs.table = nullptr;
    }

  private:
    // HashTable is not copyable or assignable
    HashTable(const HashTable&) = delete;
    void operator=(const HashTable&) = delete;

  private:
    static const size_t CAP_BITS = 30;

  public:
    Entry*      table;                 // entry storage
    uint32_t    gen:24;                 // entry storage generation number
    uint32_t    hashShift:8;            // multiplicative hash shift
    uint32_t    entryCount;             // number of entries in table
    uint32_t    removedCount;           // removed entry sentinels in table

#ifdef JS_DEBUG
    uint64_t     mutationCount;
    mutable bool mEntered;
    // Note that some updates to these stats are not thread-safe. See the
    // comment on the three-argument overloading of HashTable::lookup().
    mutable struct Stats
    {
        uint32_t        searches;       // total number of table searches
        uint32_t        steps;          // hash chain links traversed
        uint32_t        hits;           // searches that found key
        uint32_t        misses;         // searches that didn't find key
        uint32_t        addOverRemoved; // adds that recycled a removed entry
        uint32_t        removes;        // calls to remove
        uint32_t        removeFrees;    // calls to remove that freed the entry
        uint32_t        grows;          // table expansions
        uint32_t        shrinks;        // table contractions
        uint32_t        compresses;     // table compressions
        uint32_t        rehashes;       // tombstone decontaminations
    } stats;
#   define METER(x) x
#else
#   define METER(x)
#endif

    // The default initial capacity is 32 (enough to hold 16 elements), but it
    // can be as low as 4.
    static const unsigned sMinCapacityLog2 = 2;
    static const unsigned sMinCapacity  = 1 << sMinCapacityLog2;
    static const unsigned sMaxInit      = JS_BIT(CAP_BITS - 1);
    static const unsigned sMaxCapacity  = JS_BIT(CAP_BITS);
    static const unsigned sHashBits     = mozilla::tl::BitSize<HashNumber>::value;

    // Hash-table alpha is conceptually a fraction, but to avoid floating-point
    // math we implement it as a ratio of integers.
    static const uint8_t sAlphaDenominator = 4;
    static const uint8_t sMinAlphaNumerator = 1; // min alpha: 1/4
    static const uint8_t sMaxAlphaNumerator = 3; // max alpha: 3/4

    static const HashNumber sFreeKey = Entry::sFreeKey;
    static const HashNumber sRemovedKey = Entry::sRemovedKey;
    static const HashNumber sCollisionBit = Entry::sCollisionBit;

    void setTableSizeLog2(unsigned sizeLog2)
    {
        hashShift = sHashBits - sizeLog2;
    }

    static bool isLiveHash(HashNumber hash)
    {
        return Entry::isLiveHash(hash);
    }

    static HashNumber prepareHash(const Lookup& l)
    {
        HashNumber keyHash = ScrambleHashCode(HashPolicy::hash(l));

        // Avoid reserved hash codes.
        if (!isLiveHash(keyHash))
            keyHash -= (sRemovedKey + 1);
        return keyHash & ~sCollisionBit;
    }

    enum FailureBehavior { DontReportFailure = false, ReportFailure = true };

    static Entry* createTable(AllocPolicy& alloc, uint32_t capacity,
                              FailureBehavior reportFailure = ReportFailure)
    {
        static_assert(sFreeKey == 0,
                      "newly-calloc'd tables have to be considered empty");
        if (reportFailure)
            return alloc.template pod_calloc<Entry>(capacity);

        return alloc.template maybe_pod_calloc<Entry>(capacity);
    }

    static Entry* maybeCreateTable(AllocPolicy& alloc, uint32_t capacity)
    {
        static_assert(sFreeKey == 0,
                      "newly-calloc'd tables have to be considered empty");
        return alloc.template maybe_pod_calloc<Entry>(capacity);
    }

    static void destroyTable(AllocPolicy& alloc, Entry* oldTable, uint32_t capacity)
    {
        Entry* end = oldTable + capacity;
        for (Entry* e = oldTable; e < end; ++e)
            e->destroyIfLive();
        alloc.free_(oldTable);
    }

  public:
    explicit HashTable(AllocPolicy ap)
      : AllocPolicy(ap)
      , table(nullptr)
      , gen(0)
      , hashShift(sHashBits)
      , entryCount(0)
      , removedCount(0)
#ifdef JS_DEBUG
      , mutationCount(0)
      , mEntered(false)
#endif
    {}

    MOZ_WARN_UNUSED_RESULT bool init(uint32_t length)
    {
        MOZ_ASSERT(!initialized());

        // Reject all lengths whose initial computed capacity would exceed
        // sMaxCapacity.  Round that maximum length down to the nearest power
        // of two for speedier code.
        if (MOZ_UNLIKELY(length > sMaxInit)) {
            this->reportAllocOverflow();
            return false;
        }

        static_assert((sMaxInit * sAlphaDenominator) / sAlphaDenominator == sMaxInit,
                      "multiplication in numerator below could overflow");
        static_assert(sMaxInit * sAlphaDenominator <= UINT32_MAX - sMaxAlphaNumerator,
                      "numerator calculation below could potentially overflow");

        // Compute the smallest capacity allowing |length| elements to be
        // inserted without rehashing: ceil(length / max-alpha).  (Ceiling
        // integral division: <http://stackoverflow.com/a/2745086>.)
        uint32_t newCapacity =
            (length * sAlphaDenominator + sMaxAlphaNumerator - 1) / sMaxAlphaNumerator;
        if (newCapacity < sMinCapacity)
            newCapacity = sMinCapacity;

        // FIXME: use JS_CEILING_LOG2 when PGO stops crashing (bug 543034).
        uint32_t roundUp = sMinCapacity, roundUpLog2 = sMinCapacityLog2;
        while (roundUp < newCapacity) {
            roundUp <<= 1;
            ++roundUpLog2;
        }

        newCapacity = roundUp;
        MOZ_ASSERT(newCapacity >= length);
        MOZ_ASSERT(newCapacity <= sMaxCapacity);

        table = createTable(*this, newCapacity);
        if (!table)
            return false;

        setTableSizeLog2(roundUpLog2);
        METER(memset(&stats, 0, sizeof(stats)));
        return true;
    }

    bool initialized() const
    {
        return !!table;
    }

    ~HashTable()
    {
        if (table)
            destroyTable(*this, table, capacity());
    }

  private:
    HashNumber hash1(HashNumber hash0) const
    {
        return hash0 >> hashShift;
    }

    struct DoubleHash
    {
        HashNumber h2;
        HashNumber sizeMask;
    };

    DoubleHash hash2(HashNumber curKeyHash) const
    {
        unsigned sizeLog2 = sHashBits - hashShift;
        DoubleHash dh = {
            ((curKeyHash << sizeLog2) >> hashShift) | 1,
            (HashNumber(1) << sizeLog2) - 1
        };
        return dh;
    }

    static HashNumber applyDoubleHash(HashNumber h1, const DoubleHash& dh)
    {
        return (h1 - dh.h2) & dh.sizeMask;
    }

    bool overloaded()
    {
        static_assert(sMaxCapacity <= UINT32_MAX / sMaxAlphaNumerator,
                      "multiplication below could overflow");
        return entryCount + removedCount >=
               capacity() * sMaxAlphaNumerator / sAlphaDenominator;
    }

    // Would the table be underloaded if it had the given capacity and entryCount?
    static bool wouldBeUnderloaded(uint32_t capacity, uint32_t entryCount)
    {
        static_assert(sMaxCapacity <= UINT32_MAX / sMinAlphaNumerator,
                      "multiplication below could overflow");
        return capacity > sMinCapacity &&
               entryCount <= capacity * sMinAlphaNumerator / sAlphaDenominator;
    }

    bool underloaded()
    {
        return wouldBeUnderloaded(capacity(), entryCount);
    }

    static bool match(Entry& e, const Lookup& l)
    {
        return HashPolicy::match(HashPolicy::getKey(e.get()), l);
    }

    // Warning: in order for readonlyThreadsafeLookup() to be safe this
    // function must not modify the table in any way when |collisionBit| is 0.
    // (The use of the METER() macro to increment stats violates this
    // restriction but we will live with that for now because it's enabled so
    // rarely.)
    Entry& lookup(const Lookup& l, HashNumber keyHash, unsigned collisionBit) const
    {
        MOZ_ASSERT(isLiveHash(keyHash));
        MOZ_ASSERT(!(keyHash & sCollisionBit));
        MOZ_ASSERT(collisionBit == 0 || collisionBit == sCollisionBit);
        MOZ_ASSERT(table);
        METER(stats.searches++);

        // Compute the primary hash address.
        HashNumber h1 = hash1(keyHash);
        Entry* entry = &table[h1];

        // Miss: return space for a new entry.
        if (entry->isFree()) {
            METER(stats.misses++);
            return *entry;
        }

        // Hit: return entry.
        if (entry->matchHash(keyHash) && match(*entry, l)) {
            METER(stats.hits++);
            return *entry;
        }

        // Collision: double hash.
        DoubleHash dh = hash2(keyHash);

        // Save the first removed entry pointer so we can recycle later.
        Entry* firstRemoved = nullptr;

        while (true) {
            if (MOZ_UNLIKELY(entry->isRemoved())) {
                if (!firstRemoved)
                    firstRemoved = entry;
            } else {
                if (collisionBit == sCollisionBit)
                    entry->setCollision();
            }

            METER(stats.steps++);
            h1 = applyDoubleHash(h1, dh);

            entry = &table[h1];
            if (entry->isFree()) {
                METER(stats.misses++);
                return firstRemoved ? *firstRemoved : *entry;
            }

            if (entry->matchHash(keyHash) && match(*entry, l)) {
                METER(stats.hits++);
                return *entry;
            }
        }
    }

    // This is a copy of lookup hardcoded to the assumptions:
    //   1. the lookup is a lookupForAdd
    //   2. the key, whose |keyHash| has been passed is not in the table,
    //   3. no entries have been removed from the table.
    // This specialized search avoids the need for recovering lookup values
    // from entries, which allows more flexible Lookup/Key types.
    Entry& findFreeEntry(HashNumber keyHash)
    {
        MOZ_ASSERT(!(keyHash & sCollisionBit));
        MOZ_ASSERT(table);
        METER(stats.searches++);

        // We assume 'keyHash' has already been distributed.

        // Compute the primary hash address.
        HashNumber h1 = hash1(keyHash);
        Entry* entry = &table[h1];

        // Miss: return space for a new entry.
        if (!entry->isLive()) {
            METER(stats.misses++);
            return *entry;
        }

        // Collision: double hash.
        DoubleHash dh = hash2(keyHash);

        while (true) {
            MOZ_ASSERT(!entry->isRemoved());
            entry->setCollision();

            METER(stats.steps++);
            h1 = applyDoubleHash(h1, dh);

            entry = &table[h1];
            if (!entry->isLive()) {
                METER(stats.misses++);
                return *entry;
            }
        }
    }

    enum RebuildStatus { NotOverloaded, Rehashed, RehashFailed };

    RebuildStatus changeTableSize(int deltaLog2, FailureBehavior reportFailure = ReportFailure)
    {
        // Look, but don't touch, until we succeed in getting new entry store.
        Entry* oldTable = table;
        uint32_t oldCap = capacity();
        uint32_t newLog2 = sHashBits - hashShift + deltaLog2;
        uint32_t newCapacity = JS_BIT(newLog2);
        if (MOZ_UNLIKELY(newCapacity > sMaxCapacity)) {
            if (reportFailure)
                this->reportAllocOverflow();
            return RehashFailed;
        }

        Entry* newTable = createTable(*this, newCapacity, reportFailure);
        if (!newTable)
            return RehashFailed;

        // We can't fail from here on, so update table parameters.
        setTableSizeLog2(newLog2);
        removedCount = 0;
        gen++;
        table = newTable;

        // Copy only live entries, leaving removed ones behind.
        Entry* end = oldTable + oldCap;
        for (Entry* src = oldTable; src < end; ++src) {
            if (src->isLive()) {
                HashNumber hn = src->getKeyHash();
                findFreeEntry(hn).setLive(
                    hn, mozilla::Move(const_cast<typename Entry::NonConstT&>(src->get())));
                src->destroy();
            }
        }

        // All entries have been destroyed, no need to destroyTable.
        this->free_(oldTable);
        return Rehashed;
    }

    bool shouldCompressTable()
    {
        // Compress if a quarter or more of all entries are removed.
        return removedCount >= (capacity() >> 2);
    }

    RebuildStatus checkOverloaded(FailureBehavior reportFailure = ReportFailure)
    {
        if (!overloaded())
            return NotOverloaded;

        int deltaLog2;
        if (shouldCompressTable()) {
            METER(stats.compresses++);
            deltaLog2 = 0;
        } else {
            METER(stats.grows++);
            deltaLog2 = 1;
        }

        return changeTableSize(deltaLog2, reportFailure);
    }

    // Infallibly rehash the table if we are overloaded with removals.
    void checkOverRemoved()
    {
        if (overloaded()) {
            if (checkOverloaded(DontReportFailure) == RehashFailed)
                rehashTableInPlace();
        }
    }

    void remove(Entry& e)
    {
        MOZ_ASSERT(table);
        METER(stats.removes++);

        if (e.hasCollision()) {
            e.removeLive();
            removedCount++;
        } else {
            METER(stats.removeFrees++);
            e.clearLive();
        }
        entryCount--;
#ifdef JS_DEBUG
        mutationCount++;
#endif
    }

    void checkUnderloaded()
    {
        if (underloaded()) {
            METER(stats.shrinks++);
            (void) changeTableSize(-1, DontReportFailure);
        }
    }

    // Resize the table down to the largest capacity which doesn't underload the
    // table.  Since we call checkUnderloaded() on every remove, you only need
    // to call this after a bulk removal of items done without calling remove().
    void compactIfUnderloaded()
    {
        int32_t resizeLog2 = 0;
        uint32_t newCapacity = capacity();
        while (wouldBeUnderloaded(newCapacity, entryCount)) {
            newCapacity = newCapacity >> 1;
            resizeLog2--;
        }

        if (resizeLog2 != 0)
            (void) changeTableSize(resizeLog2, DontReportFailure);
    }

    // This is identical to changeTableSize(currentSize), but without requiring
    // a second table.  We do this by recycling the collision bits to tell us if
    // the element is already inserted or still waiting to be inserted.  Since
    // already-inserted elements win any conflicts, we get the same table as we
    // would have gotten through random insertion order.
    void rehashTableInPlace()
    {
        METER(stats.rehashes++);
        removedCount = 0;
        for (size_t i = 0; i < capacity(); ++i)
            table[i].unsetCollision();

        for (size_t i = 0; i < capacity();) {
            Entry* src = &table[i];

            if (!src->isLive() || src->hasCollision()) {
                ++i;
                continue;
            }

            HashNumber keyHash = src->getKeyHash();
            HashNumber h1 = hash1(keyHash);
            DoubleHash dh = hash2(keyHash);
            Entry* tgt = &table[h1];
            while (true) {
                if (!tgt->hasCollision()) {
                    src->swap(tgt);
                    tgt->setCollision();
                    break;
                }

                h1 = applyDoubleHash(h1, dh);
                tgt = &table[h1];
            }
        }

        // TODO: this algorithm leaves collision bits on *all* elements, even if
        // they are on no collision path. We have the option of setting the
        // collision bits correctly on a subsequent pass or skipping the rehash
        // unless we are totally filled with tombstones: benchmark to find out
        // which approach is best.
    }

  public:
    void clear()
    {
        if (mozilla::IsPod<Entry>::value) {
            memset(table, 0, sizeof(*table) * capacity());
        } else {
            uint32_t tableCapacity = capacity();
            Entry* end = table + tableCapacity;
            for (Entry* e = table; e < end; ++e)
                e->clear();
        }
        removedCount = 0;
        entryCount = 0;
#ifdef JS_DEBUG
        mutationCount++;
#endif
    }

    void finish()
    {
#ifdef JS_DEBUG
        MOZ_ASSERT(!mEntered);
#endif
        if (!table)
            return;

        destroyTable(*this, table, capacity());
        table = nullptr;
        gen++;
        entryCount = 0;
        removedCount = 0;
#ifdef JS_DEBUG
        mutationCount++;
#endif
    }

    Range all() const
    {
        MOZ_ASSERT(table);
        return Range(*this, table, table + capacity());
    }

    bool empty() const
    {
        MOZ_ASSERT(table);
        return !entryCount;
    }

    uint32_t count() const
    {
        MOZ_ASSERT(table);
        return entryCount;
    }

    uint32_t capacity() const
    {
        MOZ_ASSERT(table);
        return JS_BIT(sHashBits - hashShift);
    }

    uint32_t generation() const
    {
        MOZ_ASSERT(table);
        return gen;
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const
    {
        return mallocSizeOf(table);
    }

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const
    {
        return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
    }

    Ptr lookup(const Lookup& l) const
    {
        mozilla::ReentrancyGuard g(*this);
        HashNumber keyHash = prepareHash(l);
        return Ptr(lookup(l, keyHash, 0), *this);
    }

    Ptr readonlyThreadsafeLookup(const Lookup& l) const
    {
        HashNumber keyHash = prepareHash(l);
        return Ptr(lookup(l, keyHash, 0), *this);
    }

    AddPtr lookupForAdd(const Lookup& l) const
    {
        mozilla::ReentrancyGuard g(*this);
        HashNumber keyHash = prepareHash(l);
        Entry& entry = lookup(l, keyHash, sCollisionBit);
        AddPtr p(entry, *this, keyHash);
        return p;
    }

    template <typename... Args>
    bool add(AddPtr& p, Args&&... args)
    {
        mozilla::ReentrancyGuard g(*this);
        MOZ_ASSERT(table);
        MOZ_ASSERT(!p.found());
        MOZ_ASSERT(!(p.keyHash & sCollisionBit));

        // Changing an entry from removed to live does not affect whether we
        // are overloaded and can be handled separately.
        if (p.entry_->isRemoved()) {
            if (!this->checkSimulatedOOM())
                return false;
            METER(stats.addOverRemoved++);
            removedCount--;
            p.keyHash |= sCollisionBit;
        } else {
            // Preserve the validity of |p.entry_|.
            RebuildStatus status = checkOverloaded();
            if (status == RehashFailed)
                return false;
            if (!this->checkSimulatedOOM())
                return false;
            if (status == Rehashed)
                p.entry_ = &findFreeEntry(p.keyHash);
        }

        p.entry_->setLive(p.keyHash, mozilla::Forward<Args>(args)...);
        entryCount++;
#ifdef JS_DEBUG
        mutationCount++;
        p.generation = generation();
        p.mutationCount = mutationCount;
#endif
        return true;
    }

    // Note: |l| may be a reference to a piece of |u|, so this function
    // must take care not to use |l| after moving |u|.
    template <typename... Args>
    void putNewInfallible(const Lookup& l, Args&&... args)
    {
        MOZ_ASSERT(table);

        HashNumber keyHash = prepareHash(l);
        Entry* entry = &findFreeEntry(keyHash);
        MOZ_ASSERT(entry);

        if (entry->isRemoved()) {
            METER(stats.addOverRemoved++);
            removedCount--;
            keyHash |= sCollisionBit;
        }

        entry->setLive(keyHash, mozilla::Forward<Args>(args)...);
        entryCount++;
#ifdef JS_DEBUG
        mutationCount++;
#endif
    }

    // Note: |l| may be alias arguments in |args|, so this function must take
    // care not to use |l| after moving |args|.
    template <typename... Args>
    bool putNew(const Lookup& l, Args&&... args)
    {
        if (!this->checkSimulatedOOM())
            return false;

        if (checkOverloaded() == RehashFailed)
            return false;

        putNewInfallible(l, mozilla::Forward<Args>(args)...);
        return true;
    }

    // Note: |l| may be a reference to a piece of |u|, so this function
    // must take care not to use |l| after moving |u|.
    template <typename... Args>
    bool relookupOrAdd(AddPtr& p, const Lookup& l, Args&&... args)
    {
#ifdef JS_DEBUG
        p.generation = generation();
        p.mutationCount = mutationCount;
#endif
        {
            mozilla::ReentrancyGuard g(*this);
            MOZ_ASSERT(prepareHash(l) == p.keyHash); // l has not been destroyed
            p.entry_ = &lookup(l, p.keyHash, sCollisionBit);
        }
        return p.found() || add(p, mozilla::Forward<Args>(args)...);
    }

    void remove(Ptr p)
    {
        MOZ_ASSERT(table);
        mozilla::ReentrancyGuard g(*this);
        MOZ_ASSERT(p.found());
        remove(*p.entry_);
        checkUnderloaded();
    }

    void rekeyWithoutRehash(Ptr p, const Lookup& l, const Key& k)
    {
        MOZ_ASSERT(table);
        mozilla::ReentrancyGuard g(*this);
        MOZ_ASSERT(p.found());
        typename HashTableEntry<T>::NonConstT t(mozilla::Move(*p));
        HashPolicy::setKey(t, const_cast<Key&>(k));
        remove(*p.entry_);
        putNewInfallible(l, mozilla::Move(t));
    }

    void rekeyAndMaybeRehash(Ptr p, const Lookup& l, const Key& k)
    {
        rekeyWithoutRehash(p, l, k);
        checkOverRemoved();
    }

    void rekeyInPlace(Ptr p, const Key& k)
    {
        MOZ_ASSERT(table);
        mozilla::ReentrancyGuard g(*this);
        MOZ_ASSERT(p.found());
        HashPolicy::rekey(const_cast<Key&>(*p), const_cast<Key&>(k));
    }

#undef METER
};

} // namespace detail
} // namespace js

#endif  /* js_HashTable_h */
