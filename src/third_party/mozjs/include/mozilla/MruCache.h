/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_MruCache_h
#define mozilla_MruCache_h

#include <cstdint>
#include <type_traits>
#include <utility>

#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"

namespace mozilla {

namespace detail {

// Helper struct for checking if a value is empty.
//
// `IsNotEmpty` will return true if `Value` is not a pointer type or if the
// pointer value is not null.
template <typename Value, bool IsPtr = std::is_pointer<Value>::value>
struct EmptyChecker {
  static bool IsNotEmpty(const Value&) { return true; }
};
// Template specialization for the `IsPtr == true` case.
template <typename Value>
struct EmptyChecker<Value, true> {
  static bool IsNotEmpty(const Value& aVal) { return aVal != nullptr; }
};

}  // namespace detail

// Provides a most recently used cache that can be used as a layer on top of
// a larger container where lookups can be expensive. The default size is 31,
// which as a prime number provides a better distrubution of cached entries.
//
// Users are expected to provide a `Cache` class that defines two required
// methods:
//   - A method for providing the hash of a key:
//
//     static HashNumber Hash(const KeyType& aKey)
//
//   - A method for matching a key to a value, for pointer types the value
//     is guaranteed not to be null.
//
//     static bool Match(const KeyType& aKey, const ValueType& aVal)
//
// For example:
//    class MruExample : public MruCache<void*, PtrInfo*, MruExample>
//    {
//      static HashNumber Hash(const KeyType& aKey)
//      {
//        return HashGeneric(aKey);
//      }
//      static Match(const KeyType& aKey, const ValueType& aVal)
//      {
//        return aVal->mPtr == aKey;
//      }
//    };
template <class Key, class Value, class Cache, size_t Size = 31>
class MruCache {
  // Best distribution is achieved with a prime number. Ideally the closest
  // to a power of two will be the most efficient use of memory. This
  // assertion is pretty weak, but should catch the common inclination to
  // use a power-of-two.
  static_assert(Size % 2 != 0, "Use a prime number");

  // This is a stronger assertion but significantly limits the values to just
  // those close to a power-of-two value.
  // static_assert(Size == 7 || Size == 13 || Size == 31 || Size == 61 ||
  //              Size == 127 || Size == 251 || Size == 509 || Size == 1021,
  //              "Use a prime number less than 1024");

 public:
  using KeyType = Key;
  using ValueType = Value;

  MruCache() = default;
  MruCache(const MruCache&) = delete;
  MruCache(const MruCache&&) = delete;

  // Inserts the given value into the cache. Potentially overwrites an
  // existing entry.
  template <typename U>
  void Put(const KeyType& aKey, U&& aVal) {
    *RawEntry(aKey) = std::forward<U>(aVal);
  }

  // Removes the given entry if it is in the cache.
  void Remove(const KeyType& aKey) { Lookup(aKey).Remove(); }

  // Clears all cached entries and resets them to a default value.
  void Clear() {
    for (ValueType& val : mCache) {
      val = ValueType{};
    }
  }

  // Helper that holds an entry that matched a lookup key. Usage:
  //
  //    auto p = mCache.Lookup(aKey);
  //    if (p) {
  //      return p.Data();
  //    }
  //
  //    auto foo = new Foo();
  //    p.Set(foo);
  //    return foo;
  class Entry {
   public:
    Entry(ValueType* aEntry, bool aMatch) : mEntry(aEntry), mMatch(aMatch) {
      MOZ_ASSERT(mEntry);
    }

    explicit operator bool() const { return mMatch; }

    ValueType& Data() const {
      MOZ_ASSERT(mMatch);
      return *mEntry;
    }

    template <typename U>
    void Set(U&& aValue) {
      mMatch = true;
      Data() = std::forward<U>(aValue);
    }

    void Remove() {
      if (mMatch) {
        Data() = ValueType{};
        mMatch = false;
      }
    }

   private:
    ValueType* mEntry;  // Location of the entry in the cache.
    bool mMatch;        // Whether the value matched.
  };

  // Retrieves an entry from the cache. Can be used to test if an entry is
  // present, update the entry to a new value, or remove the entry if one was
  // matched.
  Entry Lookup(const KeyType& aKey) {
    using EmptyChecker = detail::EmptyChecker<ValueType>;

    auto entry = RawEntry(aKey);
    bool match = EmptyChecker::IsNotEmpty(*entry) && Cache::Match(aKey, *entry);
    return Entry(entry, match);
  }

 private:
  MOZ_ALWAYS_INLINE ValueType* RawEntry(const KeyType& aKey) {
    return &mCache[Cache::Hash(aKey) % Size];
  }

  ValueType mCache[Size] = {};
};

}  // namespace mozilla

#endif  // mozilla_mrucache_h
