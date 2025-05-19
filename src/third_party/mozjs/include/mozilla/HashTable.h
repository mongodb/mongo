/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//---------------------------------------------------------------------------
// Overview
//---------------------------------------------------------------------------
//
// This file defines HashMap<Key, Value> and HashSet<T>, hash tables that are
// fast and have a nice API.
//
// Both hash tables have two optional template parameters.
//
// - HashPolicy. This defines the operations for hashing and matching keys. The
//   default HashPolicy is appropriate when both of the following two
//   conditions are true.
//
//   - The key type stored in the table (|Key| for |HashMap<Key, Value>|, |T|
//     for |HashSet<T>|) is an integer, pointer, UniquePtr, float, or double.
//
//   - The type used for lookups (|Lookup|) is the same as the key type. This
//     is usually the case, but not always.
//
//   There is also a |CStringHasher| policy for |char*| keys. If your keys
//   don't match any of the above cases, you must provide your own hash policy;
//   see the "Hash Policy" section below.
//
// - AllocPolicy. This defines how allocations are done by the table.
//
//   - |MallocAllocPolicy| is the default and is usually appropriate; note that
//     operations (such as insertions) that might cause allocations are
//     fallible and must be checked for OOM. These checks are enforced by the
//     use of [[nodiscard]].
//
//   - |InfallibleAllocPolicy| is another possibility; it allows the
//     abovementioned OOM checks to be done with MOZ_ALWAYS_TRUE().
//
//   Note that entry storage allocation is lazy, and not done until the first
//   lookupForAdd(), put(), or putNew() is performed.
//
//  See AllocPolicy.h for more details.
//
// Documentation on how to use HashMap and HashSet, including examples, is
// present within those classes. Search for "class HashMap" and "class
// HashSet".
//
// Both HashMap and HashSet are implemented on top of a third class, HashTable.
// You only need to look at HashTable if you want to understand the
// implementation.
//
// How does mozilla::HashTable (this file) compare with PLDHashTable (and its
// subclasses, such as nsTHashtable)?
//
// - mozilla::HashTable is a lot faster, largely because it uses templates
//   throughout *and* inlines everything. PLDHashTable inlines operations much
//   less aggressively, and also uses "virtual ops" for operations like hashing
//   and matching entries that require function calls.
//
// - Correspondingly, mozilla::HashTable use is likely to increase executable
//   size much more than PLDHashTable.
//
// - mozilla::HashTable has a nicer API, with a proper HashSet vs. HashMap
//   distinction.
//
// - mozilla::HashTable requires more explicit OOM checking. As mentioned
//   above, the use of |InfallibleAllocPolicy| can simplify things.
//
// - mozilla::HashTable has a default capacity on creation of 32 and a minimum
//   capacity of 4. PLDHashTable has a default capacity on creation of 8 and a
//   minimum capacity of 8.

#ifndef mozilla_HashTable_h
#define mozilla_HashTable_h

#include <utility>
#include <type_traits>

#include "mozilla/AllocPolicy.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Opaque.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/ReentrancyGuard.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/WrappingOperations.h"

namespace mozilla {

template <class, class = void>
struct DefaultHasher;

template <class, class>
class HashMapEntry;

namespace detail {

template <typename T>
class HashTableEntry;

template <class T, class HashPolicy, class AllocPolicy>
class HashTable;

}  // namespace detail

// The "generation" of a hash table is an opaque value indicating the state of
// modification of the hash table through its lifetime.  If the generation of
// a hash table compares equal at times T1 and T2, then lookups in the hash
// table, pointers to (or into) hash table entries, etc. at time T1 are valid
// at time T2.  If the generation compares unequal, these computations are all
// invalid and must be performed again to be used.
//
// Generations are meaningfully comparable only with respect to a single hash
// table.  It's always nonsensical to compare the generation of distinct hash
// tables H1 and H2.
using Generation = Opaque<uint64_t>;

//---------------------------------------------------------------------------
// HashMap
//---------------------------------------------------------------------------

// HashMap is a fast hash-based map from keys to values.
//
// Template parameter requirements:
// - Key/Value: movable, destructible, assignable.
// - HashPolicy: see the "Hash Policy" section below.
// - AllocPolicy: see AllocPolicy.h.
//
// Note:
// - HashMap is not reentrant: Key/Value/HashPolicy/AllocPolicy members
//   called by HashMap must not call back into the same HashMap object.
//
template <class Key, class Value, class HashPolicy = DefaultHasher<Key>,
          class AllocPolicy = MallocAllocPolicy>
class HashMap {
  // -- Implementation details -----------------------------------------------

  // HashMap is not copyable or assignable.
  HashMap(const HashMap& hm) = delete;
  HashMap& operator=(const HashMap& hm) = delete;

  using TableEntry = HashMapEntry<Key, Value>;

  struct MapHashPolicy : HashPolicy {
    using Base = HashPolicy;
    using KeyType = Key;

    static const Key& getKey(TableEntry& aEntry) { return aEntry.key(); }

    static void setKey(TableEntry& aEntry, Key& aKey) {
      HashPolicy::rekey(aEntry.mutableKey(), aKey);
    }
  };

  using Impl = detail::HashTable<TableEntry, MapHashPolicy, AllocPolicy>;
  Impl mImpl;

  friend class Impl::Enum;

 public:
  using Lookup = typename HashPolicy::Lookup;
  using Entry = TableEntry;

  // -- Initialization -------------------------------------------------------

  explicit HashMap(AllocPolicy aAllocPolicy = AllocPolicy(),
                   uint32_t aLen = Impl::sDefaultLen)
      : mImpl(std::move(aAllocPolicy), aLen) {}

  explicit HashMap(uint32_t aLen) : mImpl(AllocPolicy(), aLen) {}

  // HashMap is movable.
  HashMap(HashMap&& aRhs) = default;
  HashMap& operator=(HashMap&& aRhs) = default;

  // -- Status and sizing ----------------------------------------------------

  // The map's current generation.
  Generation generation() const { return mImpl.generation(); }

  // Is the map empty?
  bool empty() const { return mImpl.empty(); }

  // Number of keys/values in the map.
  uint32_t count() const { return mImpl.count(); }

  // Number of key/value slots in the map. Note: resize will happen well before
  // count() == capacity().
  uint32_t capacity() const { return mImpl.capacity(); }

  // The size of the map's entry storage, in bytes. If the keys/values contain
  // pointers to other heap blocks, you must iterate over the map and measure
  // them separately; hence the "shallow" prefix.
  size_t shallowSizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const {
    return mImpl.shallowSizeOfExcludingThis(aMallocSizeOf);
  }
  size_t shallowSizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
    return aMallocSizeOf(this) +
           mImpl.shallowSizeOfExcludingThis(aMallocSizeOf);
  }

  // Attempt to minimize the capacity(). If the table is empty, this will free
  // the empty storage and upon regrowth it will be given the minimum capacity.
  void compact() { mImpl.compact(); }

  // Attempt to reserve enough space to fit at least |aLen| elements. This is
  // total capacity, including elements already present. Does nothing if the
  // map already has sufficient capacity.
  [[nodiscard]] bool reserve(uint32_t aLen) { return mImpl.reserve(aLen); }

  // -- Lookups --------------------------------------------------------------

  // Does the map contain a key/value matching |aLookup|?
  bool has(const Lookup& aLookup) const {
    return mImpl.lookup(aLookup).found();
  }

  // Return a Ptr indicating whether a key/value matching |aLookup| is
  // present in the map. E.g.:
  //
  //   using HM = HashMap<int,char>;
  //   HM h;
  //   if (HM::Ptr p = h.lookup(3)) {
  //     assert(p->key() == 3);
  //     char val = p->value();
  //   }
  //
  using Ptr = typename Impl::Ptr;
  MOZ_ALWAYS_INLINE Ptr lookup(const Lookup& aLookup) const {
    return mImpl.lookup(aLookup);
  }

  // Like lookup(), but does not assert if two threads call it at the same
  // time. Only use this method when none of the threads will modify the map.
  MOZ_ALWAYS_INLINE Ptr readonlyThreadsafeLookup(const Lookup& aLookup) const {
    return mImpl.readonlyThreadsafeLookup(aLookup);
  }

  // -- Insertions -----------------------------------------------------------

  // Overwrite existing value with |aValue|, or add it if not present. Returns
  // false on OOM.
  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool put(KeyInput&& aKey, ValueInput&& aValue) {
    return put(aKey, std::forward<KeyInput>(aKey),
               std::forward<ValueInput>(aValue));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool put(const Lookup& aLookup, KeyInput&& aKey,
                         ValueInput&& aValue) {
    AddPtr p = lookupForAdd(aLookup);
    if (p) {
      p->value() = std::forward<ValueInput>(aValue);
      return true;
    }
    return add(p, std::forward<KeyInput>(aKey),
               std::forward<ValueInput>(aValue));
  }

  // Like put(), but slightly faster. Must only be used when the given key is
  // not already present. (In debug builds, assertions check this.)
  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool putNew(KeyInput&& aKey, ValueInput&& aValue) {
    return mImpl.putNew(aKey, std::forward<KeyInput>(aKey),
                        std::forward<ValueInput>(aValue));
  }

  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool putNew(const Lookup& aLookup, KeyInput&& aKey,
                            ValueInput&& aValue) {
    return mImpl.putNew(aLookup, std::forward<KeyInput>(aKey),
                        std::forward<ValueInput>(aValue));
  }

  // Like putNew(), but should be only used when the table is known to be big
  // enough for the insertion, and hashing cannot fail. Typically this is used
  // to populate an empty map with known-unique keys after reserving space with
  // reserve(), e.g.
  //
  //   using HM = HashMap<int,char>;
  //   HM h;
  //   if (!h.reserve(3)) {
  //     MOZ_CRASH("OOM");
  //   }
  //   h.putNewInfallible(1, 'a');    // unique key
  //   h.putNewInfallible(2, 'b');    // unique key
  //   h.putNewInfallible(3, 'c');    // unique key
  //
  template <typename KeyInput, typename ValueInput>
  void putNewInfallible(KeyInput&& aKey, ValueInput&& aValue) {
    mImpl.putNewInfallible(aKey, std::forward<KeyInput>(aKey),
                           std::forward<ValueInput>(aValue));
  }

  // Like |lookup(l)|, but on miss, |p = lookupForAdd(l)| allows efficient
  // insertion of Key |k| (where |HashPolicy::match(k,l) == true|) using
  // |add(p,k,v)|. After |add(p,k,v)|, |p| points to the new key/value. E.g.:
  //
  //   using HM = HashMap<int,char>;
  //   HM h;
  //   HM::AddPtr p = h.lookupForAdd(3);
  //   if (!p) {
  //     if (!h.add(p, 3, 'a')) {
  //       return false;
  //     }
  //   }
  //   assert(p->key() == 3);
  //   char val = p->value();
  //
  // N.B. The caller must ensure that no mutating hash table operations occur
  // between a pair of lookupForAdd() and add() calls. To avoid looking up the
  // key a second time, the caller may use the more efficient relookupOrAdd()
  // method. This method reuses part of the hashing computation to more
  // efficiently insert the key if it has not been added. For example, a
  // mutation-handling version of the previous example:
  //
  //    HM::AddPtr p = h.lookupForAdd(3);
  //    if (!p) {
  //      call_that_may_mutate_h();
  //      if (!h.relookupOrAdd(p, 3, 'a')) {
  //        return false;
  //      }
  //    }
  //    assert(p->key() == 3);
  //    char val = p->value();
  //
  using AddPtr = typename Impl::AddPtr;
  MOZ_ALWAYS_INLINE AddPtr lookupForAdd(const Lookup& aLookup) {
    return mImpl.lookupForAdd(aLookup);
  }

  // Add a key/value. Returns false on OOM.
  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool add(AddPtr& aPtr, KeyInput&& aKey, ValueInput&& aValue) {
    return mImpl.add(aPtr, std::forward<KeyInput>(aKey),
                     std::forward<ValueInput>(aValue));
  }

  // See the comment above lookupForAdd() for details.
  template <typename KeyInput, typename ValueInput>
  [[nodiscard]] bool relookupOrAdd(AddPtr& aPtr, KeyInput&& aKey,
                                   ValueInput&& aValue) {
    return mImpl.relookupOrAdd(aPtr, aKey, std::forward<KeyInput>(aKey),
                               std::forward<ValueInput>(aValue));
  }

  // -- Removal --------------------------------------------------------------

  // Lookup and remove the key/value matching |aLookup|, if present.
  void remove(const Lookup& aLookup) {
    if (Ptr p = lookup(aLookup)) {
      remove(p);
    }
  }

  // Remove a previously found key/value (assuming aPtr.found()). The map must
  // not have been mutated in the interim.
  void remove(Ptr aPtr) { mImpl.remove(aPtr); }

  // Remove all keys/values without changing the capacity.
  void clear() { mImpl.clear(); }

  // Like clear() followed by compact().
  void clearAndCompact() { mImpl.clearAndCompact(); }

  // -- Rekeying -------------------------------------------------------------

  // Infallibly rekey one entry, if necessary. Requires that template
  // parameters Key and HashPolicy::Lookup are the same type.
  void rekeyIfMoved(const Key& aOldKey, const Key& aNewKey) {
    if (aOldKey != aNewKey) {
      rekeyAs(aOldKey, aNewKey, aNewKey);
    }
  }

  // Infallibly rekey one entry if present, and return whether that happened.
  bool rekeyAs(const Lookup& aOldLookup, const Lookup& aNewLookup,
               const Key& aNewKey) {
    if (Ptr p = lookup(aOldLookup)) {
      mImpl.rekeyAndMaybeRehash(p, aNewLookup, aNewKey);
      return true;
    }
    return false;
  }

  // -- Iteration ------------------------------------------------------------

  // |iter()| returns an Iterator:
  //
  //   HashMap<int, char> h;
  //   for (auto iter = h.iter(); !iter.done(); iter.next()) {
  //     char c = iter.get().value();
  //   }
  //
  using Iterator = typename Impl::Iterator;
  Iterator iter() const { return mImpl.iter(); }

  // |modIter()| returns a ModIterator:
  //
  //   HashMap<int, char> h;
  //   for (auto iter = h.modIter(); !iter.done(); iter.next()) {
  //     if (iter.get().value() == 'l') {
  //       iter.remove();
  //     }
  //   }
  //
  // Table resize may occur in ModIterator's destructor.
  using ModIterator = typename Impl::ModIterator;
  ModIterator modIter() { return mImpl.modIter(); }

  // These are similar to Iterator/ModIterator/iter(), but use different
  // terminology.
  using Range = typename Impl::Range;
  using Enum = typename Impl::Enum;
  Range all() const { return mImpl.all(); }
};

//---------------------------------------------------------------------------
// HashSet
//---------------------------------------------------------------------------

// HashSet is a fast hash-based set of values.
//
// Template parameter requirements:
// - T: movable, destructible, assignable.
// - HashPolicy: see the "Hash Policy" section below.
// - AllocPolicy: see AllocPolicy.h
//
// Note:
// - HashSet is not reentrant: T/HashPolicy/AllocPolicy members called by
//   HashSet must not call back into the same HashSet object.
//
template <class T, class HashPolicy = DefaultHasher<T>,
          class AllocPolicy = MallocAllocPolicy>
class HashSet {
  // -- Implementation details -----------------------------------------------

  // HashSet is not copyable or assignable.
  HashSet(const HashSet& hs) = delete;
  HashSet& operator=(const HashSet& hs) = delete;

  struct SetHashPolicy : HashPolicy {
    using Base = HashPolicy;
    using KeyType = T;

    static const KeyType& getKey(const T& aT) { return aT; }

    static void setKey(T& aT, KeyType& aKey) { HashPolicy::rekey(aT, aKey); }
  };

  using Impl = detail::HashTable<const T, SetHashPolicy, AllocPolicy>;
  Impl mImpl;

  friend class Impl::Enum;

 public:
  using Lookup = typename HashPolicy::Lookup;
  using Entry = T;

  // -- Initialization -------------------------------------------------------

  explicit HashSet(AllocPolicy aAllocPolicy = AllocPolicy(),
                   uint32_t aLen = Impl::sDefaultLen)
      : mImpl(std::move(aAllocPolicy), aLen) {}

  explicit HashSet(uint32_t aLen) : mImpl(AllocPolicy(), aLen) {}

  // HashSet is movable.
  HashSet(HashSet&& aRhs) = default;
  HashSet& operator=(HashSet&& aRhs) = default;

  // -- Status and sizing ----------------------------------------------------

  // The set's current generation.
  Generation generation() const { return mImpl.generation(); }

  // Is the set empty?
  bool empty() const { return mImpl.empty(); }

  // Number of elements in the set.
  uint32_t count() const { return mImpl.count(); }

  // Number of element slots in the set. Note: resize will happen well before
  // count() == capacity().
  uint32_t capacity() const { return mImpl.capacity(); }

  // The size of the set's entry storage, in bytes. If the elements contain
  // pointers to other heap blocks, you must iterate over the set and measure
  // them separately; hence the "shallow" prefix.
  size_t shallowSizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const {
    return mImpl.shallowSizeOfExcludingThis(aMallocSizeOf);
  }
  size_t shallowSizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
    return aMallocSizeOf(this) +
           mImpl.shallowSizeOfExcludingThis(aMallocSizeOf);
  }

  // Attempt to minimize the capacity(). If the table is empty, this will free
  // the empty storage and upon regrowth it will be given the minimum capacity.
  void compact() { mImpl.compact(); }

  // Attempt to reserve enough space to fit at least |aLen| elements. This is
  // total capacity, including elements already present. Does nothing if the
  // map already has sufficient capacity.
  [[nodiscard]] bool reserve(uint32_t aLen) { return mImpl.reserve(aLen); }

  // -- Lookups --------------------------------------------------------------

  // Does the set contain an element matching |aLookup|?
  bool has(const Lookup& aLookup) const {
    return mImpl.lookup(aLookup).found();
  }

  // Return a Ptr indicating whether an element matching |aLookup| is present
  // in the set. E.g.:
  //
  //   using HS = HashSet<int>;
  //   HS h;
  //   if (HS::Ptr p = h.lookup(3)) {
  //     assert(*p == 3);   // p acts like a pointer to int
  //   }
  //
  using Ptr = typename Impl::Ptr;
  MOZ_ALWAYS_INLINE Ptr lookup(const Lookup& aLookup) const {
    return mImpl.lookup(aLookup);
  }

  // Like lookup(), but does not assert if two threads call it at the same
  // time. Only use this method when none of the threads will modify the set.
  MOZ_ALWAYS_INLINE Ptr readonlyThreadsafeLookup(const Lookup& aLookup) const {
    return mImpl.readonlyThreadsafeLookup(aLookup);
  }

  // -- Insertions -----------------------------------------------------------

  // Add |aU| if it is not present already. Returns false on OOM.
  template <typename U>
  [[nodiscard]] bool put(U&& aU) {
    AddPtr p = lookupForAdd(aU);
    return p ? true : add(p, std::forward<U>(aU));
  }

  // Like put(), but slightly faster. Must only be used when the given element
  // is not already present. (In debug builds, assertions check this.)
  template <typename U>
  [[nodiscard]] bool putNew(U&& aU) {
    return mImpl.putNew(aU, std::forward<U>(aU));
  }

  // Like the other putNew(), but for when |Lookup| is different to |T|.
  template <typename U>
  [[nodiscard]] bool putNew(const Lookup& aLookup, U&& aU) {
    return mImpl.putNew(aLookup, std::forward<U>(aU));
  }

  // Like putNew(), but should be only used when the table is known to be big
  // enough for the insertion, and hashing cannot fail. Typically this is used
  // to populate an empty set with known-unique elements after reserving space
  // with reserve(), e.g.
  //
  //   using HS = HashMap<int>;
  //   HS h;
  //   if (!h.reserve(3)) {
  //     MOZ_CRASH("OOM");
  //   }
  //   h.putNewInfallible(1);     // unique element
  //   h.putNewInfallible(2);     // unique element
  //   h.putNewInfallible(3);     // unique element
  //
  template <typename U>
  void putNewInfallible(const Lookup& aLookup, U&& aU) {
    mImpl.putNewInfallible(aLookup, std::forward<U>(aU));
  }

  // Like |lookup(l)|, but on miss, |p = lookupForAdd(l)| allows efficient
  // insertion of T value |t| (where |HashPolicy::match(t,l) == true|) using
  // |add(p,t)|. After |add(p,t)|, |p| points to the new element. E.g.:
  //
  //   using HS = HashSet<int>;
  //   HS h;
  //   HS::AddPtr p = h.lookupForAdd(3);
  //   if (!p) {
  //     if (!h.add(p, 3)) {
  //       return false;
  //     }
  //   }
  //   assert(*p == 3);   // p acts like a pointer to int
  //
  // N.B. The caller must ensure that no mutating hash table operations occur
  // between a pair of lookupForAdd() and add() calls. To avoid looking up the
  // key a second time, the caller may use the more efficient relookupOrAdd()
  // method. This method reuses part of the hashing computation to more
  // efficiently insert the key if it has not been added. For example, a
  // mutation-handling version of the previous example:
  //
  //    HS::AddPtr p = h.lookupForAdd(3);
  //    if (!p) {
  //      call_that_may_mutate_h();
  //      if (!h.relookupOrAdd(p, 3, 3)) {
  //        return false;
  //      }
  //    }
  //    assert(*p == 3);
  //
  // Note that relookupOrAdd(p,l,t) performs Lookup using |l| and adds the
  // entry |t|, where the caller ensures match(l,t).
  using AddPtr = typename Impl::AddPtr;
  MOZ_ALWAYS_INLINE AddPtr lookupForAdd(const Lookup& aLookup) {
    return mImpl.lookupForAdd(aLookup);
  }

  // Add an element. Returns false on OOM.
  template <typename U>
  [[nodiscard]] bool add(AddPtr& aPtr, U&& aU) {
    return mImpl.add(aPtr, std::forward<U>(aU));
  }

  // See the comment above lookupForAdd() for details.
  template <typename U>
  [[nodiscard]] bool relookupOrAdd(AddPtr& aPtr, const Lookup& aLookup,
                                   U&& aU) {
    return mImpl.relookupOrAdd(aPtr, aLookup, std::forward<U>(aU));
  }

  // -- Removal --------------------------------------------------------------

  // Lookup and remove the element matching |aLookup|, if present.
  void remove(const Lookup& aLookup) {
    if (Ptr p = lookup(aLookup)) {
      remove(p);
    }
  }

  // Remove a previously found element (assuming aPtr.found()). The set must
  // not have been mutated in the interim.
  void remove(Ptr aPtr) { mImpl.remove(aPtr); }

  // Remove all keys/values without changing the capacity.
  void clear() { mImpl.clear(); }

  // Like clear() followed by compact().
  void clearAndCompact() { mImpl.clearAndCompact(); }

  // -- Rekeying -------------------------------------------------------------

  // Infallibly rekey one entry, if present. Requires that template parameters
  // T and HashPolicy::Lookup are the same type.
  void rekeyIfMoved(const Lookup& aOldValue, const T& aNewValue) {
    if (aOldValue != aNewValue) {
      rekeyAs(aOldValue, aNewValue, aNewValue);
    }
  }

  // Infallibly rekey one entry if present, and return whether that happened.
  bool rekeyAs(const Lookup& aOldLookup, const Lookup& aNewLookup,
               const T& aNewValue) {
    if (Ptr p = lookup(aOldLookup)) {
      mImpl.rekeyAndMaybeRehash(p, aNewLookup, aNewValue);
      return true;
    }
    return false;
  }

  // Infallibly replace the current key at |aPtr| with an equivalent key.
  // Specifically, both HashPolicy::hash and HashPolicy::match must return
  // identical results for the new and old key when applied against all
  // possible matching values.
  void replaceKey(Ptr aPtr, const Lookup& aLookup, const T& aNewValue) {
    MOZ_ASSERT(aPtr.found());
    MOZ_ASSERT(*aPtr != aNewValue);
    MOZ_ASSERT(HashPolicy::match(*aPtr, aLookup));
    MOZ_ASSERT(HashPolicy::match(aNewValue, aLookup));
    const_cast<T&>(*aPtr) = aNewValue;
    MOZ_ASSERT(*lookup(aLookup) == aNewValue);
  }
  void replaceKey(Ptr aPtr, const T& aNewValue) {
    replaceKey(aPtr, aNewValue, aNewValue);
  }

  // -- Iteration ------------------------------------------------------------

  // |iter()| returns an Iterator:
  //
  //   HashSet<int> h;
  //   for (auto iter = h.iter(); !iter.done(); iter.next()) {
  //     int i = iter.get();
  //   }
  //
  using Iterator = typename Impl::Iterator;
  Iterator iter() const { return mImpl.iter(); }

  // |modIter()| returns a ModIterator:
  //
  //   HashSet<int> h;
  //   for (auto iter = h.modIter(); !iter.done(); iter.next()) {
  //     if (iter.get() == 42) {
  //       iter.remove();
  //     }
  //   }
  //
  // Table resize may occur in ModIterator's destructor.
  using ModIterator = typename Impl::ModIterator;
  ModIterator modIter() { return mImpl.modIter(); }

  // These are similar to Iterator/ModIterator/iter(), but use different
  // terminology.
  using Range = typename Impl::Range;
  using Enum = typename Impl::Enum;
  Range all() const { return mImpl.all(); }
};

//---------------------------------------------------------------------------
// Hash Policy
//---------------------------------------------------------------------------

// A hash policy |HP| for a hash table with key-type |Key| must provide:
//
//  - a type |HP::Lookup| to use to lookup table entries;
//
//  - a static member function |HP::hash| that hashes lookup values:
//
//      static mozilla::HashNumber hash(const Lookup&);
//
//  - a static member function |HP::match| that tests equality of key and
//    lookup values:
//
//      static bool match(const Key& aKey, const Lookup& aLookup);
//
//    |aKey| and |aLookup| can have different hash numbers, only when a
//    collision happens with |prepareHash| operation, which is less frequent.
//    Thus, |HP::match| shouldn't assume the hash equality in the comparison,
//    even if the hash numbers are almost always same between them.
//
// Normally, Lookup = Key. In general, though, different values and types of
// values can be used to lookup and store. If a Lookup value |l| is not equal
// to the added Key value |k|, the user must ensure that |HP::match(k,l)| is
// true. E.g.:
//
//   mozilla::HashSet<Key, HP>::AddPtr p = h.lookup(l);
//   if (!p) {
//     assert(HP::match(k, l));  // must hold
//     h.add(p, k);
//   }

// A pointer hashing policy that uses HashGeneric() to create good hashes for
// pointers. Note that we don't shift out the lowest k bits because we don't
// want to assume anything about the alignment of the pointers.
template <typename Key>
struct PointerHasher {
  using Lookup = Key;

  static HashNumber hash(const Lookup& aLookup) { return HashGeneric(aLookup); }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    return aKey == aLookup;
  }

  static void rekey(Key& aKey, const Key& aNewKey) { aKey = aNewKey; }
};

// The default hash policy, which only works with integers.
template <class Key, typename>
struct DefaultHasher {
  using Lookup = Key;

  static HashNumber hash(const Lookup& aLookup) {
    // Just convert the integer to a HashNumber and use that as is. (This
    // discards the high 32-bits of 64-bit integers!) ScrambleHashCode() is
    // subsequently called on the value to improve the distribution.
    return aLookup;
  }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    // Use builtin or overloaded operator==.
    return aKey == aLookup;
  }

  static void rekey(Key& aKey, const Key& aNewKey) { aKey = aNewKey; }
};

// A DefaultHasher specialization for enums.
template <class T>
struct DefaultHasher<T, std::enable_if_t<std::is_enum_v<T>>> {
  using Key = T;
  using Lookup = Key;

  static HashNumber hash(const Lookup& aLookup) { return HashGeneric(aLookup); }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    // Use builtin or overloaded operator==.
    return aKey == static_cast<Key>(aLookup);
  }

  static void rekey(Key& aKey, const Key& aNewKey) { aKey = aNewKey; }
};

// A DefaultHasher specialization for pointers.
template <class T>
struct DefaultHasher<T*> : PointerHasher<T*> {};

// A DefaultHasher specialization for mozilla::UniquePtr.
template <class T, class D>
struct DefaultHasher<UniquePtr<T, D>> {
  using Key = UniquePtr<T, D>;
  using Lookup = Key;
  using PtrHasher = PointerHasher<T*>;

  static HashNumber hash(const Lookup& aLookup) {
    return PtrHasher::hash(aLookup.get());
  }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    return PtrHasher::match(aKey.get(), aLookup.get());
  }

  static void rekey(UniquePtr<T, D>& aKey, UniquePtr<T, D>&& aNewKey) {
    aKey = std::move(aNewKey);
  }
};

// A DefaultHasher specialization for doubles.
template <>
struct DefaultHasher<double> {
  using Key = double;
  using Lookup = Key;

  static HashNumber hash(const Lookup& aLookup) {
    // Just xor the high bits with the low bits, and then treat the bits of the
    // result as a uint32_t.
    static_assert(sizeof(HashNumber) == 4,
                  "subsequent code assumes a four-byte hash");
    uint64_t u = BitwiseCast<uint64_t>(aLookup);
    return HashNumber(u ^ (u >> 32));
  }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    return BitwiseCast<uint64_t>(aKey) == BitwiseCast<uint64_t>(aLookup);
  }
};

// A DefaultHasher specialization for floats.
template <>
struct DefaultHasher<float> {
  using Key = float;
  using Lookup = Key;

  static HashNumber hash(const Lookup& aLookup) {
    // Just use the value as if its bits form an integer. ScrambleHashCode() is
    // subsequently called on the value to improve the distribution.
    static_assert(sizeof(HashNumber) == 4,
                  "subsequent code assumes a four-byte hash");
    return HashNumber(BitwiseCast<uint32_t>(aLookup));
  }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    return BitwiseCast<uint32_t>(aKey) == BitwiseCast<uint32_t>(aLookup);
  }
};

// A hash policy for C strings.
struct CStringHasher {
  using Key = const char*;
  using Lookup = const char*;

  static HashNumber hash(const Lookup& aLookup) { return HashString(aLookup); }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    return strcmp(aKey, aLookup) == 0;
  }
};

//---------------------------------------------------------------------------
// Fallible Hashing Interface
//---------------------------------------------------------------------------

// Most of the time generating a hash code is infallible, but sometimes it is
// necessary to generate hash codes on demand in a way that can fail. Specialize
// this class for your own hash policy to provide fallible hashing.
//
// This is used by MovableCellHasher to handle the fact that generating a unique
// ID for cell pointer may fail due to OOM.
//
// The default implementations of these methods delegate to the usual HashPolicy
// implementation and always succeed.
template <typename HashPolicy>
struct FallibleHashMethods {
  // Return true if a hashcode is already available for its argument, and
  // sets |aHashOut|. Once this succeeds for a specific argument it
  // must continue to do so.
  //
  // Return false if a hashcode is not already available. This implies that any
  // lookup must fail, as the hash code would have to have been successfully
  // created on insertion.
  template <typename Lookup>
  static bool maybeGetHash(Lookup&& aLookup, HashNumber* aHashOut) {
    *aHashOut = HashPolicy::hash(aLookup);
    return true;
  }

  // Fallible method to ensure a hashcode exists for its argument and create one
  // if not. Sets |aHashOut| to the hashcode and retuns true on success. Returns
  // false on error, e.g. out of memory.
  template <typename Lookup>
  static bool ensureHash(Lookup&& aLookup, HashNumber* aHashOut) {
    *aHashOut = HashPolicy::hash(aLookup);
    return true;
  }
};

template <typename HashPolicy, typename Lookup>
static bool MaybeGetHash(Lookup&& aLookup, HashNumber* aHashOut) {
  return FallibleHashMethods<typename HashPolicy::Base>::maybeGetHash(
      std::forward<Lookup>(aLookup), aHashOut);
}

template <typename HashPolicy, typename Lookup>
static bool EnsureHash(Lookup&& aLookup, HashNumber* aHashOut) {
  return FallibleHashMethods<typename HashPolicy::Base>::ensureHash(
      std::forward<Lookup>(aLookup), aHashOut);
}

//---------------------------------------------------------------------------
// Implementation Details (HashMapEntry, HashTableEntry, HashTable)
//---------------------------------------------------------------------------

// Both HashMap and HashSet are implemented by a single HashTable that is even
// more heavily parameterized than the other two. This leaves HashTable gnarly
// and extremely coupled to HashMap and HashSet; thus code should not use
// HashTable directly.

template <class Key, class Value>
class HashMapEntry {
  Key key_;
  Value value_;

  template <class, class, class>
  friend class detail::HashTable;
  template <class>
  friend class detail::HashTableEntry;
  template <class, class, class, class>
  friend class HashMap;

 public:
  template <typename KeyInput, typename ValueInput>
  HashMapEntry(KeyInput&& aKey, ValueInput&& aValue)
      : key_(std::forward<KeyInput>(aKey)),
        value_(std::forward<ValueInput>(aValue)) {}

  HashMapEntry(HashMapEntry&& aRhs) = default;
  HashMapEntry& operator=(HashMapEntry&& aRhs) = default;

  using KeyType = Key;
  using ValueType = Value;

  const Key& key() const { return key_; }

  // Use this method with caution! If the key is changed such that its hash
  // value also changes, the map will be left in an invalid state.
  Key& mutableKey() { return key_; }

  const Value& value() const { return value_; }
  Value& value() { return value_; }

 private:
  HashMapEntry(const HashMapEntry&) = delete;
  void operator=(const HashMapEntry&) = delete;
};

namespace detail {

template <class T, class HashPolicy, class AllocPolicy>
class HashTable;

template <typename T>
class EntrySlot;

template <typename T>
class HashTableEntry {
 private:
  using NonConstT = std::remove_const_t<T>;

  // Instead of having a hash table entry store that looks like this:
  //
  // +--------+--------+--------+--------+
  // | entry0 | entry1 |  ....  | entryN |
  // +--------+--------+--------+--------+
  //
  // where the entries contained their cached hash code, we're going to lay out
  // the entry store thusly:
  //
  // +-------+-------+-------+-------+--------+--------+--------+--------+
  // | hash0 | hash1 |  ...  | hashN | entry0 | entry1 |  ....  | entryN |
  // +-------+-------+-------+-------+--------+--------+--------+--------+
  //
  // with all the cached hashes prior to the actual entries themselves.
  //
  // We do this because implementing the first strategy requires us to make
  // HashTableEntry look roughly like:
  //
  // template <typename T>
  // class HashTableEntry {
  //   HashNumber mKeyHash;
  //   T mValue;
  // };
  //
  // The problem with this setup is that, depending on the layout of `T`, there
  // may be platform ABI-mandated padding between `mKeyHash` and the first
  // member of `T`. This ABI-mandated padding is wasted space, and can be
  // surprisingly common, e.g. when `T` is a single pointer on 64-bit platforms.
  // In such cases, we're throwing away a quarter of our entry store on padding,
  // which is undesirable.
  //
  // The second layout above, namely:
  //
  // +-------+-------+-------+-------+--------+--------+--------+--------+
  // | hash0 | hash1 |  ...  | hashN | entry0 | entry1 |  ....  | entryN |
  // +-------+-------+-------+-------+--------+--------+--------+--------+
  //
  // means there is no wasted space between the hashes themselves, and no wasted
  // space between the entries themselves.  However, we would also like there to
  // be no gap between the last hash and the first entry. The memory allocator
  // guarantees the alignment of the start of the hashes. The use of a
  // power-of-two capacity of at least 4 guarantees that the alignment of the
  // *end* of the hash array is no less than the alignment of the start.
  // Finally, the static_asserts here guarantee that the entries themselves
  // don't need to be any more aligned than the alignment of the entry store
  // itself.
  //
  // This assertion is safe for 32-bit builds because on both Windows and Linux
  // (including Android), the minimum alignment for allocations larger than 8
  // bytes is 8 bytes, and the actual data for entries in our entry store is
  // guaranteed to have that alignment as well, thanks to the power-of-two
  // number of cached hash values stored prior to the entry data.

  // The allocation policy must allocate a table with at least this much
  // alignment.
  static constexpr size_t kMinimumAlignment = 8;

  static_assert(alignof(HashNumber) <= kMinimumAlignment,
                "[N*2 hashes, N*2 T values] allocation's alignment must be "
                "enough to align each hash");
  static_assert(alignof(NonConstT) <= 2 * sizeof(HashNumber),
                "subsequent N*2 T values must not require more than an even "
                "number of HashNumbers provides");

  static const HashNumber sFreeKey = 0;
  static const HashNumber sRemovedKey = 1;
  static const HashNumber sCollisionBit = 1;

  alignas(NonConstT) unsigned char mValueData[sizeof(NonConstT)];

 private:
  template <class, class, class>
  friend class HashTable;
  template <typename>
  friend class EntrySlot;

  // Some versions of GCC treat it as a -Wstrict-aliasing violation (ergo a
  // -Werror compile error) to reinterpret_cast<> |mValueData| to |T*|, even
  // through |void*|.  Placing the latter cast in these separate functions
  // breaks the chain such that affected GCC versions no longer warn/error.
  void* rawValuePtr() { return mValueData; }

  static bool isLiveHash(HashNumber hash) { return hash > sRemovedKey; }

  HashTableEntry(const HashTableEntry&) = delete;
  void operator=(const HashTableEntry&) = delete;

  NonConstT* valuePtr() { return reinterpret_cast<NonConstT*>(rawValuePtr()); }

  void destroyStoredT() {
    NonConstT* ptr = valuePtr();
    ptr->~T();
    MOZ_MAKE_MEM_UNDEFINED(ptr, sizeof(*ptr));
  }

 public:
  HashTableEntry() = default;

  ~HashTableEntry() { MOZ_MAKE_MEM_UNDEFINED(this, sizeof(*this)); }

  void destroy() { destroyStoredT(); }

  void swap(HashTableEntry* aOther, bool aIsLive) {
    // This allows types to use Argument-Dependent-Lookup, and thus use a custom
    // std::swap, which is needed by types like JS::Heap and such.
    using std::swap;

    if (this == aOther) {
      return;
    }
    if (aIsLive) {
      swap(*valuePtr(), *aOther->valuePtr());
    } else {
      *aOther->valuePtr() = std::move(*valuePtr());
      destroy();
    }
  }

  T& get() { return *valuePtr(); }

  NonConstT& getMutable() { return *valuePtr(); }
};

// A slot represents a cached hash value and its associated entry stored
// in the hash table. These two things are not stored in contiguous memory.
template <class T>
class EntrySlot {
  using NonConstT = std::remove_const_t<T>;

  using Entry = HashTableEntry<T>;

  Entry* mEntry;
  HashNumber* mKeyHash;

  template <class, class, class>
  friend class HashTable;

  EntrySlot(Entry* aEntry, HashNumber* aKeyHash)
      : mEntry(aEntry), mKeyHash(aKeyHash) {}

 public:
  static bool isLiveHash(HashNumber hash) { return hash > Entry::sRemovedKey; }

  EntrySlot(const EntrySlot&) = default;
  EntrySlot(EntrySlot&& aOther) = default;

  EntrySlot& operator=(const EntrySlot&) = default;
  EntrySlot& operator=(EntrySlot&&) = default;

  bool operator==(const EntrySlot& aRhs) const { return mEntry == aRhs.mEntry; }

  bool operator<(const EntrySlot& aRhs) const { return mEntry < aRhs.mEntry; }

  EntrySlot& operator++() {
    ++mEntry;
    ++mKeyHash;
    return *this;
  }

  void destroy() { mEntry->destroy(); }

  void swap(EntrySlot& aOther) {
    mEntry->swap(aOther.mEntry, aOther.isLive());
    std::swap(*mKeyHash, *aOther.mKeyHash);
  }

  T& get() const { return mEntry->get(); }

  NonConstT& getMutable() { return mEntry->getMutable(); }

  bool isFree() const { return *mKeyHash == Entry::sFreeKey; }

  void clearLive() {
    MOZ_ASSERT(isLive());
    *mKeyHash = Entry::sFreeKey;
    mEntry->destroyStoredT();
  }

  void clear() {
    if (isLive()) {
      mEntry->destroyStoredT();
    }
    MOZ_MAKE_MEM_UNDEFINED(mEntry, sizeof(*mEntry));
    *mKeyHash = Entry::sFreeKey;
  }

  bool isRemoved() const { return *mKeyHash == Entry::sRemovedKey; }

  void removeLive() {
    MOZ_ASSERT(isLive());
    *mKeyHash = Entry::sRemovedKey;
    mEntry->destroyStoredT();
  }

  bool isLive() const { return isLiveHash(*mKeyHash); }

  void setCollision() {
    MOZ_ASSERT(isLive());
    *mKeyHash |= Entry::sCollisionBit;
  }
  void unsetCollision() { *mKeyHash &= ~Entry::sCollisionBit; }
  bool hasCollision() const { return *mKeyHash & Entry::sCollisionBit; }
  bool matchHash(HashNumber hn) {
    return (*mKeyHash & ~Entry::sCollisionBit) == hn;
  }
  HashNumber getKeyHash() const { return *mKeyHash & ~Entry::sCollisionBit; }

  template <typename... Args>
  void setLive(HashNumber aHashNumber, Args&&... aArgs) {
    MOZ_ASSERT(!isLive());
    *mKeyHash = aHashNumber;
    new (KnownNotNull, mEntry->valuePtr()) T(std::forward<Args>(aArgs)...);
    MOZ_ASSERT(isLive());
  }

  Entry* toEntry() const { return mEntry; }
};

template <class T, class HashPolicy, class AllocPolicy>
class HashTable : private AllocPolicy {
  friend class mozilla::ReentrancyGuard;

  using NonConstT = std::remove_const_t<T>;
  using Key = typename HashPolicy::KeyType;
  using Lookup = typename HashPolicy::Lookup;

 public:
  using Entry = HashTableEntry<T>;
  using Slot = EntrySlot<T>;

  template <typename F>
  static void forEachSlot(char* aTable, uint32_t aCapacity, F&& f) {
    auto hashes = reinterpret_cast<HashNumber*>(aTable);
    auto entries = reinterpret_cast<Entry*>(&hashes[aCapacity]);
    Slot slot(entries, hashes);
    for (size_t i = 0; i < size_t(aCapacity); ++i) {
      f(slot);
      ++slot;
    }
  }

  // A nullable pointer to a hash table element. A Ptr |p| can be tested
  // either explicitly |if (p.found()) p->...| or using boolean conversion
  // |if (p) p->...|. Ptr objects must not be used after any mutating hash
  // table operations unless |generation()| is tested.
  class Ptr {
    friend class HashTable;

    Slot mSlot;
#ifdef DEBUG
    const HashTable* mTable;
    Generation mGeneration;
#endif

   protected:
    Ptr(Slot aSlot, const HashTable& aTable)
        : mSlot(aSlot)
#ifdef DEBUG
          ,
          mTable(&aTable),
          mGeneration(aTable.generation())
#endif
    {
    }

    // This constructor is used only by AddPtr() within lookupForAdd().
    explicit Ptr(const HashTable& aTable)
        : mSlot(nullptr, nullptr)
#ifdef DEBUG
          ,
          mTable(&aTable),
          mGeneration(aTable.generation())
#endif
    {
    }

    bool isValid() const { return !!mSlot.toEntry(); }

   public:
    Ptr()
        : mSlot(nullptr, nullptr)
#ifdef DEBUG
          ,
          mTable(nullptr),
          mGeneration(0)
#endif
    {
    }

    bool found() const {
      if (!isValid()) {
        return false;
      }
#ifdef DEBUG
      MOZ_ASSERT(mGeneration == mTable->generation());
#endif
      return mSlot.isLive();
    }

    explicit operator bool() const { return found(); }

    bool operator==(const Ptr& aRhs) const {
      MOZ_ASSERT(found() && aRhs.found());
      return mSlot == aRhs.mSlot;
    }

    bool operator!=(const Ptr& aRhs) const {
#ifdef DEBUG
      MOZ_ASSERT(mGeneration == mTable->generation());
#endif
      return !(*this == aRhs);
    }

    T& operator*() const {
#ifdef DEBUG
      MOZ_ASSERT(found());
      MOZ_ASSERT(mGeneration == mTable->generation());
#endif
      return mSlot.get();
    }

    T* operator->() const {
#ifdef DEBUG
      MOZ_ASSERT(found());
      MOZ_ASSERT(mGeneration == mTable->generation());
#endif
      return &mSlot.get();
    }
  };

  // A Ptr that can be used to add a key after a failed lookup.
  class AddPtr : public Ptr {
    friend class HashTable;

    HashNumber mKeyHash;
#ifdef DEBUG
    uint64_t mMutationCount;
#endif

    AddPtr(Slot aSlot, const HashTable& aTable, HashNumber aHashNumber)
        : Ptr(aSlot, aTable),
          mKeyHash(aHashNumber)
#ifdef DEBUG
          ,
          mMutationCount(aTable.mMutationCount)
#endif
    {
    }

    // This constructor is used when lookupForAdd() is performed on a table
    // lacking entry storage; it leaves mSlot null but initializes everything
    // else.
    AddPtr(const HashTable& aTable, HashNumber aHashNumber)
        : Ptr(aTable),
          mKeyHash(aHashNumber)
#ifdef DEBUG
          ,
          mMutationCount(aTable.mMutationCount)
#endif
    {
      MOZ_ASSERT(isLive());
    }

    bool isLive() const { return isLiveHash(mKeyHash); }

   public:
    AddPtr() : mKeyHash(0) {}
  };

  // A hash table iterator that (mostly) doesn't allow table modifications.
  // As with Ptr/AddPtr, Iterator objects must not be used after any mutating
  // hash table operation unless the |generation()| is tested.
  class Iterator {
    void moveToNextLiveEntry() {
      while (++mCur < mEnd && !mCur.isLive()) {
        continue;
      }
    }

   protected:
    friend class HashTable;

    explicit Iterator(const HashTable& aTable)
        : mCur(aTable.slotForIndex(0)),
          mEnd(aTable.slotForIndex(aTable.capacity()))
#ifdef DEBUG
          ,
          mTable(aTable),
          mMutationCount(aTable.mMutationCount),
          mGeneration(aTable.generation()),
          mValidEntry(true)
#endif
    {
      if (!done() && !mCur.isLive()) {
        moveToNextLiveEntry();
      }
    }

    Slot mCur;
    Slot mEnd;
#ifdef DEBUG
    const HashTable& mTable;
    uint64_t mMutationCount;
    Generation mGeneration;
    bool mValidEntry;
#endif

   public:
    bool done() const {
      MOZ_ASSERT(mGeneration == mTable.generation());
      MOZ_ASSERT(mMutationCount == mTable.mMutationCount);
      return mCur == mEnd;
    }

    T& get() const {
      MOZ_ASSERT(!done());
      MOZ_ASSERT(mValidEntry);
      MOZ_ASSERT(mGeneration == mTable.generation());
      MOZ_ASSERT(mMutationCount == mTable.mMutationCount);
      return mCur.get();
    }

    void next() {
      MOZ_ASSERT(!done());
      MOZ_ASSERT(mGeneration == mTable.generation());
      MOZ_ASSERT(mMutationCount == mTable.mMutationCount);
      moveToNextLiveEntry();
#ifdef DEBUG
      mValidEntry = true;
#endif
    }
  };

  // A hash table iterator that permits modification, removal and rekeying.
  // Since rehashing when elements were removed during enumeration would be
  // bad, it is postponed until the ModIterator is destructed. Since the
  // ModIterator's destructor touches the hash table, the user must ensure
  // that the hash table is still alive when the destructor runs.
  class ModIterator : public Iterator {
    friend class HashTable;

    HashTable& mTable;
    bool mRekeyed;
    bool mRemoved;

    // ModIterator is movable but not copyable.
    ModIterator(const ModIterator&) = delete;
    void operator=(const ModIterator&) = delete;

   protected:
    explicit ModIterator(HashTable& aTable)
        : Iterator(aTable), mTable(aTable), mRekeyed(false), mRemoved(false) {}

   public:
    MOZ_IMPLICIT ModIterator(ModIterator&& aOther)
        : Iterator(aOther),
          mTable(aOther.mTable),
          mRekeyed(aOther.mRekeyed),
          mRemoved(aOther.mRemoved) {
      aOther.mRekeyed = false;
      aOther.mRemoved = false;
    }

    // Removes the current element from the table, leaving |get()|
    // invalid until the next call to |next()|.
    void remove() {
      mTable.remove(this->mCur);
      mRemoved = true;
#ifdef DEBUG
      this->mValidEntry = false;
      this->mMutationCount = mTable.mMutationCount;
#endif
    }

    NonConstT& getMutable() {
      MOZ_ASSERT(!this->done());
      MOZ_ASSERT(this->mValidEntry);
      MOZ_ASSERT(this->mGeneration == this->Iterator::mTable.generation());
      MOZ_ASSERT(this->mMutationCount == this->Iterator::mTable.mMutationCount);
      return this->mCur.getMutable();
    }

    // Removes the current element and re-inserts it into the table with
    // a new key at the new Lookup position.  |get()| is invalid after
    // this operation until the next call to |next()|.
    void rekey(const Lookup& l, const Key& k) {
      MOZ_ASSERT(&k != &HashPolicy::getKey(this->mCur.get()));
      Ptr p(this->mCur, mTable);
      mTable.rekeyWithoutRehash(p, l, k);
      mRekeyed = true;
#ifdef DEBUG
      this->mValidEntry = false;
      this->mMutationCount = mTable.mMutationCount;
#endif
    }

    void rekey(const Key& k) { rekey(k, k); }

    // Potentially rehashes the table.
    ~ModIterator() {
      if (mRekeyed) {
        mTable.mGen++;
        mTable.infallibleRehashIfOverloaded();
      }

      if (mRemoved) {
        mTable.compact();
      }
    }
  };

  // Range is similar to Iterator, but uses different terminology.
  class Range {
    friend class HashTable;

    Iterator mIter;

   protected:
    explicit Range(const HashTable& table) : mIter(table) {}

   public:
    bool empty() const { return mIter.done(); }

    T& front() const { return mIter.get(); }

    void popFront() { return mIter.next(); }
  };

  // Enum is similar to ModIterator, but uses different terminology.
  class Enum {
    ModIterator mIter;

    // Enum is movable but not copyable.
    Enum(const Enum&) = delete;
    void operator=(const Enum&) = delete;

   public:
    template <class Map>
    explicit Enum(Map& map) : mIter(map.mImpl) {}

    MOZ_IMPLICIT Enum(Enum&& other) : mIter(std::move(other.mIter)) {}

    bool empty() const { return mIter.done(); }

    T& front() const { return mIter.get(); }

    void popFront() { return mIter.next(); }

    void removeFront() { mIter.remove(); }

    NonConstT& mutableFront() { return mIter.getMutable(); }

    void rekeyFront(const Lookup& aLookup, const Key& aKey) {
      mIter.rekey(aLookup, aKey);
    }

    void rekeyFront(const Key& aKey) { mIter.rekey(aKey); }
  };

  // HashTable is movable
  HashTable(HashTable&& aRhs) : AllocPolicy(std::move(aRhs)) { moveFrom(aRhs); }
  HashTable& operator=(HashTable&& aRhs) {
    MOZ_ASSERT(this != &aRhs, "self-move assignment is prohibited");
    if (mTable) {
      destroyTable(*this, mTable, capacity());
    }
    AllocPolicy::operator=(std::move(aRhs));
    moveFrom(aRhs);
    return *this;
  }

 private:
  void moveFrom(HashTable& aRhs) {
    mGen = aRhs.mGen;
    mHashShift = aRhs.mHashShift;
    mTable = aRhs.mTable;
    mEntryCount = aRhs.mEntryCount;
    mRemovedCount = aRhs.mRemovedCount;
#ifdef DEBUG
    mMutationCount = aRhs.mMutationCount;
    mEntered = aRhs.mEntered;
#endif
    aRhs.mTable = nullptr;
    aRhs.clearAndCompact();
  }

  // HashTable is not copyable or assignable
  HashTable(const HashTable&) = delete;
  void operator=(const HashTable&) = delete;

  static const uint32_t CAP_BITS = 30;

 public:
  uint64_t mGen : 56;       // entry storage generation number
  uint64_t mHashShift : 8;  // multiplicative hash shift
  char* mTable;             // entry storage
  uint32_t mEntryCount;     // number of entries in mTable
  uint32_t mRemovedCount;   // removed entry sentinels in mTable

#ifdef DEBUG
  uint64_t mMutationCount;
  mutable bool mEntered;
#endif

  // The default initial capacity is 32 (enough to hold 16 elements), but it
  // can be as low as 4.
  static const uint32_t sDefaultLen = 16;
  static const uint32_t sMinCapacity = 4;
  // See the comments in HashTableEntry about this value.
  static_assert(sMinCapacity >= 4, "too-small sMinCapacity breaks assumptions");
  static const uint32_t sMaxInit = 1u << (CAP_BITS - 1);
  static const uint32_t sMaxCapacity = 1u << CAP_BITS;

  // Hash-table alpha is conceptually a fraction, but to avoid floating-point
  // math we implement it as a ratio of integers.
  static const uint8_t sAlphaDenominator = 4;
  static const uint8_t sMinAlphaNumerator = 1;  // min alpha: 1/4
  static const uint8_t sMaxAlphaNumerator = 3;  // max alpha: 3/4

  static const HashNumber sFreeKey = Entry::sFreeKey;
  static const HashNumber sRemovedKey = Entry::sRemovedKey;
  static const HashNumber sCollisionBit = Entry::sCollisionBit;

  static uint32_t bestCapacity(uint32_t aLen) {
    static_assert(
        (sMaxInit * sAlphaDenominator) / sAlphaDenominator == sMaxInit,
        "multiplication in numerator below could overflow");
    static_assert(
        sMaxInit * sAlphaDenominator <= UINT32_MAX - sMaxAlphaNumerator,
        "numerator calculation below could potentially overflow");

    // Callers should ensure this is true.
    MOZ_ASSERT(aLen <= sMaxInit);

    // Compute the smallest capacity allowing |aLen| elements to be
    // inserted without rehashing: ceil(aLen / max-alpha).  (Ceiling
    // integral division: <http://stackoverflow.com/a/2745086>.)
    uint32_t capacity = (aLen * sAlphaDenominator + sMaxAlphaNumerator - 1) /
                        sMaxAlphaNumerator;
    capacity = (capacity < sMinCapacity) ? sMinCapacity : RoundUpPow2(capacity);

    MOZ_ASSERT(capacity >= aLen);
    MOZ_ASSERT(capacity <= sMaxCapacity);

    return capacity;
  }

  static uint32_t hashShift(uint32_t aLen) {
    // Reject all lengths whose initial computed capacity would exceed
    // sMaxCapacity. Round that maximum aLen down to the nearest power of two
    // for speedier code.
    if (MOZ_UNLIKELY(aLen > sMaxInit)) {
      MOZ_CRASH("initial length is too large");
    }

    return kHashNumberBits - mozilla::CeilingLog2(bestCapacity(aLen));
  }

  static bool isLiveHash(HashNumber aHash) { return Entry::isLiveHash(aHash); }

  static HashNumber prepareHash(HashNumber aInputHash) {
    HashNumber keyHash = ScrambleHashCode(aInputHash);

    // Avoid reserved hash codes.
    if (!isLiveHash(keyHash)) {
      keyHash -= (sRemovedKey + 1);
    }
    return keyHash & ~sCollisionBit;
  }

  enum FailureBehavior { DontReportFailure = false, ReportFailure = true };

  // Fake a struct that we're going to alloc. See the comments in
  // HashTableEntry about how the table is laid out, and why it's safe.
  struct FakeSlot {
    unsigned char c[sizeof(HashNumber) + sizeof(typename Entry::NonConstT)];
  };

  static char* createTable(AllocPolicy& aAllocPolicy, uint32_t aCapacity,
                           FailureBehavior aReportFailure = ReportFailure) {
    FakeSlot* fake =
        aReportFailure
            ? aAllocPolicy.template pod_malloc<FakeSlot>(aCapacity)
            : aAllocPolicy.template maybe_pod_malloc<FakeSlot>(aCapacity);

    MOZ_ASSERT((reinterpret_cast<uintptr_t>(fake) % Entry::kMinimumAlignment) ==
               0);

    char* table = reinterpret_cast<char*>(fake);
    if (table) {
      forEachSlot(table, aCapacity, [&](Slot& slot) {
        *slot.mKeyHash = sFreeKey;
        new (KnownNotNull, slot.toEntry()) Entry();
      });
    }
    return table;
  }

  static void destroyTable(AllocPolicy& aAllocPolicy, char* aOldTable,
                           uint32_t aCapacity) {
    forEachSlot(aOldTable, aCapacity, [&](const Slot& slot) {
      if (slot.isLive()) {
        slot.toEntry()->destroyStoredT();
      }
    });
    freeTable(aAllocPolicy, aOldTable, aCapacity);
  }

  static void freeTable(AllocPolicy& aAllocPolicy, char* aOldTable,
                        uint32_t aCapacity) {
    FakeSlot* fake = reinterpret_cast<FakeSlot*>(aOldTable);
    aAllocPolicy.free_(fake, aCapacity);
  }

 public:
  HashTable(AllocPolicy aAllocPolicy, uint32_t aLen)
      : AllocPolicy(std::move(aAllocPolicy)),
        mGen(0),
        mHashShift(hashShift(aLen)),
        mTable(nullptr),
        mEntryCount(0),
        mRemovedCount(0)
#ifdef DEBUG
        ,
        mMutationCount(0),
        mEntered(false)
#endif
  {
  }

  explicit HashTable(AllocPolicy aAllocPolicy)
      : HashTable(aAllocPolicy, sDefaultLen) {}

  ~HashTable() {
    if (mTable) {
      destroyTable(*this, mTable, capacity());
    }
  }

 private:
  HashNumber hash1(HashNumber aHash0) const { return aHash0 >> mHashShift; }

  struct DoubleHash {
    HashNumber mHash2;
    HashNumber mSizeMask;
  };

  DoubleHash hash2(HashNumber aCurKeyHash) const {
    uint32_t sizeLog2 = kHashNumberBits - mHashShift;
    DoubleHash dh = {((aCurKeyHash << sizeLog2) >> mHashShift) | 1,
                     (HashNumber(1) << sizeLog2) - 1};
    return dh;
  }

  static HashNumber applyDoubleHash(HashNumber aHash1,
                                    const DoubleHash& aDoubleHash) {
    return WrappingSubtract(aHash1, aDoubleHash.mHash2) & aDoubleHash.mSizeMask;
  }

  static MOZ_ALWAYS_INLINE bool match(T& aEntry, const Lookup& aLookup) {
    return HashPolicy::match(HashPolicy::getKey(aEntry), aLookup);
  }

  enum LookupReason { ForNonAdd, ForAdd };

  Slot slotForIndex(HashNumber aIndex) const {
    auto hashes = reinterpret_cast<HashNumber*>(mTable);
    auto entries = reinterpret_cast<Entry*>(&hashes[capacity()]);
    return Slot(&entries[aIndex], &hashes[aIndex]);
  }

  // Warning: in order for readonlyThreadsafeLookup() to be safe this
  // function must not modify the table in any way when Reason==ForNonAdd.
  template <LookupReason Reason>
  MOZ_ALWAYS_INLINE Slot lookup(const Lookup& aLookup,
                                HashNumber aKeyHash) const {
    MOZ_ASSERT(isLiveHash(aKeyHash));
    MOZ_ASSERT(!(aKeyHash & sCollisionBit));
    MOZ_ASSERT(mTable);

    // Compute the primary hash address.
    HashNumber h1 = hash1(aKeyHash);
    Slot slot = slotForIndex(h1);

    // Miss: return space for a new entry.
    if (slot.isFree()) {
      return slot;
    }

    // Hit: return entry.
    if (slot.matchHash(aKeyHash) && match(slot.get(), aLookup)) {
      return slot;
    }

    // Collision: double hash.
    DoubleHash dh = hash2(aKeyHash);

    // Save the first removed entry pointer so we can recycle later.
    Maybe<Slot> firstRemoved;

    while (true) {
      if (Reason == ForAdd && !firstRemoved) {
        if (MOZ_UNLIKELY(slot.isRemoved())) {
          firstRemoved.emplace(slot);
        } else {
          slot.setCollision();
        }
      }

      h1 = applyDoubleHash(h1, dh);

      slot = slotForIndex(h1);
      if (slot.isFree()) {
        return firstRemoved.refOr(slot);
      }

      if (slot.matchHash(aKeyHash) && match(slot.get(), aLookup)) {
        return slot;
      }
    }
  }

  // This is a copy of lookup() hardcoded to the assumptions:
  //   1. the lookup is for an add;
  //   2. the key, whose |keyHash| has been passed, is not in the table.
  Slot findNonLiveSlot(HashNumber aKeyHash) {
    MOZ_ASSERT(!(aKeyHash & sCollisionBit));
    MOZ_ASSERT(mTable);

    // We assume 'aKeyHash' has already been distributed.

    // Compute the primary hash address.
    HashNumber h1 = hash1(aKeyHash);
    Slot slot = slotForIndex(h1);

    // Miss: return space for a new entry.
    if (!slot.isLive()) {
      return slot;
    }

    // Collision: double hash.
    DoubleHash dh = hash2(aKeyHash);

    while (true) {
      slot.setCollision();

      h1 = applyDoubleHash(h1, dh);

      slot = slotForIndex(h1);
      if (!slot.isLive()) {
        return slot;
      }
    }
  }

  enum RebuildStatus { NotOverloaded, Rehashed, RehashFailed };

  RebuildStatus changeTableSize(
      uint32_t newCapacity, FailureBehavior aReportFailure = ReportFailure) {
    MOZ_ASSERT(IsPowerOfTwo(newCapacity));
    MOZ_ASSERT(!!mTable == !!capacity());

    // Look, but don't touch, until we succeed in getting new entry store.
    char* oldTable = mTable;
    uint32_t oldCapacity = capacity();
    uint32_t newLog2 = mozilla::CeilingLog2(newCapacity);

    if (MOZ_UNLIKELY(newCapacity > sMaxCapacity)) {
      if (aReportFailure) {
        this->reportAllocOverflow();
      }
      return RehashFailed;
    }

    char* newTable = createTable(*this, newCapacity, aReportFailure);
    if (!newTable) {
      return RehashFailed;
    }

    // We can't fail from here on, so update table parameters.
    mHashShift = kHashNumberBits - newLog2;
    mRemovedCount = 0;
    mGen++;
    mTable = newTable;

    // Copy only live entries, leaving removed ones behind.
    forEachSlot(oldTable, oldCapacity, [&](Slot& slot) {
      if (slot.isLive()) {
        HashNumber hn = slot.getKeyHash();
        findNonLiveSlot(hn).setLive(
            hn, std::move(const_cast<typename Entry::NonConstT&>(slot.get())));
      }

      slot.clear();
    });

    // All entries have been destroyed, no need to destroyTable.
    freeTable(*this, oldTable, oldCapacity);
    return Rehashed;
  }

  RebuildStatus rehashIfOverloaded(
      FailureBehavior aReportFailure = ReportFailure) {
    static_assert(sMaxCapacity <= UINT32_MAX / sMaxAlphaNumerator,
                  "multiplication below could overflow");

    // Note: if capacity() is zero, this will always succeed, which is
    // what we want.
    bool overloaded = mEntryCount + mRemovedCount >=
                      capacity() * sMaxAlphaNumerator / sAlphaDenominator;

    if (!overloaded) {
      return NotOverloaded;
    }

    // Succeed if a quarter or more of all entries are removed. Note that this
    // always succeeds if capacity() == 0 (i.e. entry storage has not been
    // allocated), which is what we want, because it means changeTableSize()
    // will allocate the requested capacity rather than doubling it.
    bool manyRemoved = mRemovedCount >= (capacity() >> 2);
    uint32_t newCapacity = manyRemoved ? rawCapacity() : rawCapacity() * 2;
    return changeTableSize(newCapacity, aReportFailure);
  }

  void infallibleRehashIfOverloaded() {
    if (rehashIfOverloaded(DontReportFailure) == RehashFailed) {
      rehashTableInPlace();
    }
  }

  void remove(Slot& aSlot) {
    MOZ_ASSERT(mTable);

    if (aSlot.hasCollision()) {
      aSlot.removeLive();
      mRemovedCount++;
    } else {
      aSlot.clearLive();
    }
    mEntryCount--;
#ifdef DEBUG
    mMutationCount++;
#endif
  }

  void shrinkIfUnderloaded() {
    static_assert(sMaxCapacity <= UINT32_MAX / sMinAlphaNumerator,
                  "multiplication below could overflow");
    bool underloaded =
        capacity() > sMinCapacity &&
        mEntryCount <= capacity() * sMinAlphaNumerator / sAlphaDenominator;

    if (underloaded) {
      (void)changeTableSize(capacity() / 2, DontReportFailure);
    }
  }

  // This is identical to changeTableSize(currentSize), but without requiring
  // a second table.  We do this by recycling the collision bits to tell us if
  // the element is already inserted or still waiting to be inserted.  Since
  // already-inserted elements win any conflicts, we get the same table as we
  // would have gotten through random insertion order.
  void rehashTableInPlace() {
    mRemovedCount = 0;
    mGen++;
    forEachSlot(mTable, capacity(), [&](Slot& slot) { slot.unsetCollision(); });
    for (uint32_t i = 0; i < capacity();) {
      Slot src = slotForIndex(i);

      if (!src.isLive() || src.hasCollision()) {
        ++i;
        continue;
      }

      HashNumber keyHash = src.getKeyHash();
      HashNumber h1 = hash1(keyHash);
      DoubleHash dh = hash2(keyHash);
      Slot tgt = slotForIndex(h1);
      while (true) {
        if (!tgt.hasCollision()) {
          src.swap(tgt);
          tgt.setCollision();
          break;
        }

        h1 = applyDoubleHash(h1, dh);
        tgt = slotForIndex(h1);
      }
    }

    // TODO: this algorithm leaves collision bits on *all* elements, even if
    // they are on no collision path. We have the option of setting the
    // collision bits correctly on a subsequent pass or skipping the rehash
    // unless we are totally filled with tombstones: benchmark to find out
    // which approach is best.
  }

  // Prefer to use putNewInfallible; this function does not check
  // invariants.
  template <typename... Args>
  void putNewInfallibleInternal(HashNumber aKeyHash, Args&&... aArgs) {
    MOZ_ASSERT(mTable);

    Slot slot = findNonLiveSlot(aKeyHash);

    if (slot.isRemoved()) {
      mRemovedCount--;
      aKeyHash |= sCollisionBit;
    }

    slot.setLive(aKeyHash, std::forward<Args>(aArgs)...);
    mEntryCount++;
#ifdef DEBUG
    mMutationCount++;
#endif
  }

 public:
  void clear() {
    forEachSlot(mTable, capacity(), [&](Slot& slot) { slot.clear(); });
    mRemovedCount = 0;
    mEntryCount = 0;
#ifdef DEBUG
    mMutationCount++;
#endif
  }

  // Resize the table down to the smallest capacity that doesn't overload the
  // table. Since we call shrinkIfUnderloaded() on every remove, you only need
  // to call this after a bulk removal of items done without calling remove().
  void compact() {
    if (empty()) {
      // Free the entry storage.
      freeTable(*this, mTable, capacity());
      mGen++;
      mHashShift = hashShift(0);  // gives minimum capacity on regrowth
      mTable = nullptr;
      mRemovedCount = 0;
      return;
    }

    uint32_t bestCapacity = this->bestCapacity(mEntryCount);
    MOZ_ASSERT(bestCapacity <= capacity());

    if (bestCapacity < capacity()) {
      (void)changeTableSize(bestCapacity, DontReportFailure);
    }
  }

  void clearAndCompact() {
    clear();
    compact();
  }

  [[nodiscard]] bool reserve(uint32_t aLen) {
    if (aLen == 0) {
      return true;
    }

    if (MOZ_UNLIKELY(aLen > sMaxInit)) {
      this->reportAllocOverflow();
      return false;
    }

    uint32_t bestCapacity = this->bestCapacity(aLen);
    if (bestCapacity <= capacity()) {
      return true;  // Capacity is already sufficient.
    }

    RebuildStatus status = changeTableSize(bestCapacity, ReportFailure);
    MOZ_ASSERT(status != NotOverloaded);
    return status != RehashFailed;
  }

  Iterator iter() const { return Iterator(*this); }

  ModIterator modIter() { return ModIterator(*this); }

  Range all() const { return Range(*this); }

  bool empty() const { return mEntryCount == 0; }

  uint32_t count() const { return mEntryCount; }

  uint32_t rawCapacity() const { return 1u << (kHashNumberBits - mHashShift); }

  uint32_t capacity() const { return mTable ? rawCapacity() : 0; }

  Generation generation() const { return Generation(mGen); }

  size_t shallowSizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const {
    return aMallocSizeOf(mTable);
  }

  size_t shallowSizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
    return aMallocSizeOf(this) + shallowSizeOfExcludingThis(aMallocSizeOf);
  }

  MOZ_ALWAYS_INLINE Ptr readonlyThreadsafeLookup(const Lookup& aLookup) const {
    if (empty()) {
      return Ptr();
    }

    HashNumber inputHash;
    if (!MaybeGetHash<HashPolicy>(aLookup, &inputHash)) {
      return Ptr();
    }

    HashNumber keyHash = prepareHash(inputHash);
    return Ptr(lookup<ForNonAdd>(aLookup, keyHash), *this);
  }

  MOZ_ALWAYS_INLINE Ptr lookup(const Lookup& aLookup) const {
    ReentrancyGuard g(*this);
    return readonlyThreadsafeLookup(aLookup);
  }

  MOZ_ALWAYS_INLINE AddPtr lookupForAdd(const Lookup& aLookup) {
    ReentrancyGuard g(*this);

    HashNumber inputHash;
    if (!EnsureHash<HashPolicy>(aLookup, &inputHash)) {
      return AddPtr();
    }

    HashNumber keyHash = prepareHash(inputHash);

    if (!mTable) {
      return AddPtr(*this, keyHash);
    }

    // Directly call the constructor in the return statement to avoid
    // excess copying when building with Visual Studio 2017.
    // See bug 1385181.
    return AddPtr(lookup<ForAdd>(aLookup, keyHash), *this, keyHash);
  }

  template <typename... Args>
  [[nodiscard]] bool add(AddPtr& aPtr, Args&&... aArgs) {
    ReentrancyGuard g(*this);
    MOZ_ASSERT_IF(aPtr.isValid(), mTable);
    MOZ_ASSERT_IF(aPtr.isValid(), aPtr.mTable == this);
    MOZ_ASSERT(!aPtr.found());
    MOZ_ASSERT(!(aPtr.mKeyHash & sCollisionBit));

    // Check for error from ensureHash() here.
    if (!aPtr.isLive()) {
      return false;
    }

    MOZ_ASSERT(aPtr.mGeneration == generation());
#ifdef DEBUG
    MOZ_ASSERT(aPtr.mMutationCount == mMutationCount);
#endif

    if (!aPtr.isValid()) {
      MOZ_ASSERT(!mTable && mEntryCount == 0);
      uint32_t newCapacity = rawCapacity();
      RebuildStatus status = changeTableSize(newCapacity, ReportFailure);
      MOZ_ASSERT(status != NotOverloaded);
      if (status == RehashFailed) {
        return false;
      }
      aPtr.mSlot = findNonLiveSlot(aPtr.mKeyHash);

    } else if (aPtr.mSlot.isRemoved()) {
      // Changing an entry from removed to live does not affect whether we are
      // overloaded and can be handled separately.
      if (!this->checkSimulatedOOM()) {
        return false;
      }
      mRemovedCount--;
      aPtr.mKeyHash |= sCollisionBit;

    } else {
      // Preserve the validity of |aPtr.mSlot|.
      RebuildStatus status = rehashIfOverloaded();
      if (status == RehashFailed) {
        return false;
      }
      if (status == NotOverloaded && !this->checkSimulatedOOM()) {
        return false;
      }
      if (status == Rehashed) {
        aPtr.mSlot = findNonLiveSlot(aPtr.mKeyHash);
      }
    }

    aPtr.mSlot.setLive(aPtr.mKeyHash, std::forward<Args>(aArgs)...);
    mEntryCount++;
#ifdef DEBUG
    mMutationCount++;
    aPtr.mGeneration = generation();
    aPtr.mMutationCount = mMutationCount;
#endif
    return true;
  }

  // Note: |aLookup| may reference pieces of arguments in |aArgs|, so this
  // function must take care not to use |aLookup| after moving |aArgs|.
  template <typename... Args>
  void putNewInfallible(const Lookup& aLookup, Args&&... aArgs) {
    MOZ_ASSERT(!lookup(aLookup).found());
    ReentrancyGuard g(*this);
    HashNumber keyHash = prepareHash(HashPolicy::hash(aLookup));
    putNewInfallibleInternal(keyHash, std::forward<Args>(aArgs)...);
  }

  // Note: |aLookup| may alias arguments in |aArgs|, so this function must take
  // care not to use |aLookup| after moving |aArgs|.
  template <typename... Args>
  [[nodiscard]] bool putNew(const Lookup& aLookup, Args&&... aArgs) {
    MOZ_ASSERT(!lookup(aLookup).found());
    ReentrancyGuard g(*this);
    if (!this->checkSimulatedOOM()) {
      return false;
    }
    HashNumber inputHash;
    if (!EnsureHash<HashPolicy>(aLookup, &inputHash)) {
      return false;
    }
    HashNumber keyHash = prepareHash(inputHash);
    if (rehashIfOverloaded() == RehashFailed) {
      return false;
    }
    putNewInfallibleInternal(keyHash, std::forward<Args>(aArgs)...);
    return true;
  }

  // Note: |aLookup| may be a reference pieces of arguments in |aArgs|, so this
  // function must take care not to use |aLookup| after moving |aArgs|.
  template <typename... Args>
  [[nodiscard]] bool relookupOrAdd(AddPtr& aPtr, const Lookup& aLookup,
                                   Args&&... aArgs) {
    // Check for error from ensureHash() here.
    if (!aPtr.isLive()) {
      return false;
    }
#ifdef DEBUG
    aPtr.mGeneration = generation();
    aPtr.mMutationCount = mMutationCount;
#endif
    if (mTable) {
      ReentrancyGuard g(*this);
      // Check that aLookup has not been destroyed.
      MOZ_ASSERT(prepareHash(HashPolicy::hash(aLookup)) == aPtr.mKeyHash);
      aPtr.mSlot = lookup<ForAdd>(aLookup, aPtr.mKeyHash);
      if (aPtr.found()) {
        return true;
      }
    } else {
      // Clear aPtr so it's invalid; add() will allocate storage and redo the
      // lookup.
      aPtr.mSlot = Slot(nullptr, nullptr);
    }
    return add(aPtr, std::forward<Args>(aArgs)...);
  }

  void remove(Ptr aPtr) {
    MOZ_ASSERT(mTable);
    ReentrancyGuard g(*this);
    MOZ_ASSERT(aPtr.found());
    MOZ_ASSERT(aPtr.mGeneration == generation());
    remove(aPtr.mSlot);
    shrinkIfUnderloaded();
  }

  void rekeyWithoutRehash(Ptr aPtr, const Lookup& aLookup, const Key& aKey) {
    MOZ_ASSERT(mTable);
    ReentrancyGuard g(*this);
    MOZ_ASSERT(aPtr.found());
    MOZ_ASSERT(aPtr.mGeneration == generation());
    typename HashTableEntry<T>::NonConstT t(std::move(*aPtr));
    HashPolicy::setKey(t, const_cast<Key&>(aKey));
    remove(aPtr.mSlot);
    HashNumber keyHash = prepareHash(HashPolicy::hash(aLookup));
    putNewInfallibleInternal(keyHash, std::move(t));
  }

  void rekeyAndMaybeRehash(Ptr aPtr, const Lookup& aLookup, const Key& aKey) {
    rekeyWithoutRehash(aPtr, aLookup, aKey);
    infallibleRehashIfOverloaded();
  }
};

}  // namespace detail
}  // namespace mozilla

#endif /* mozilla_HashTable_h */
