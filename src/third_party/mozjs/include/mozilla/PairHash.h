/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Utilities for hashing pairs. */

#ifndef mozilla_PairHash_h
#define mozilla_PairHash_h

#include "mozilla/CompactPair.h"
#include "mozilla/HashFunctions.h"

#include <utility>  // std::pair

namespace mozilla {

/**
 * The HashPair overloads below do just what you'd expect.
 *
 * These functions support hash of std::pair<T,U> and mozilla::CompactPair<T,u>
 * where type T and U both support AddToHash.
 */
template <typename U, typename V>
[[nodiscard]] inline HashNumber HashPair(const std::pair<U, V>& pair) {
  // Pair hash combines the hash of each member
  return HashGeneric(pair.first, pair.second);
}

template <typename U, typename V>
[[nodiscard]] inline HashNumber HashCompactPair(const CompactPair<U, V>& pair) {
  // Pair hash combines the hash of each member
  return HashGeneric(pair.first(), pair.second());
}

/**
 * Hash policy for std::pair compatible with HashTable
 */
template <typename T, typename U>
struct PairHasher {
  using Key = std::pair<T, U>;
  using Lookup = Key;

  static HashNumber hash(const Lookup& aLookup) { return HashPair(aLookup); }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    return aKey == aLookup;
  }

  static void rekey(Key& aKey, const Key& aNewKey) { aKey = aNewKey; }
};

/**
 * Hash policy for mozilla::CompactPair compatible with HashTable
 */
template <typename T, typename U>
struct CompactPairHasher {
  using Key = CompactPair<T, U>;
  using Lookup = Key;

  static HashNumber hash(const Lookup& aLookup) {
    return HashCompactPair(aLookup);
  }

  static bool match(const Key& aKey, const Lookup& aLookup) {
    return aKey == aLookup;
  }

  static void rekey(Key& aKey, const Key& aNewKey) { aKey = aNewKey; }
};

}  // namespace mozilla

#endif /* mozilla_PairHash_h */
