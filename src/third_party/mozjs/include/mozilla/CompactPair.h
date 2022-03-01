/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A class holding a pair of objects that tries to conserve storage space. */

#ifndef mozilla_CompactPair_h
#define mozilla_CompactPair_h

#include <type_traits>
#include <tuple>
#include <utility>

#include "mozilla/Attributes.h"

namespace mozilla {

namespace detail {

enum StorageType { AsBase, AsMember };

// Optimize storage using the Empty Base Optimization -- that empty base classes
// don't take up space -- to optimize size when one or the other class is
// stateless and can be used as a base class.
//
// The extra conditions on storage for B are necessary so that CompactPairHelper
// won't ambiguously inherit from either A or B, such that one or the other base
// class would be inaccessible.
template <typename A, typename B,
          detail::StorageType =
              std::is_empty_v<A> ? detail::AsBase : detail::AsMember,
          detail::StorageType = std::is_empty_v<B> &&
                                        !std::is_base_of<A, B>::value &&
                                        !std::is_base_of<B, A>::value
                                    ? detail::AsBase
                                    : detail::AsMember>
struct CompactPairHelper;

template <typename A, typename B>
struct CompactPairHelper<A, B, AsMember, AsMember> {
 protected:
  template <typename... AArgs, std::size_t... AIndexes, typename... BArgs,
            std::size_t... BIndexes>
  constexpr CompactPairHelper(std::tuple<AArgs...>& aATuple,
                              std::tuple<BArgs...>& aBTuple,
                              std::index_sequence<AIndexes...>,
                              std::index_sequence<BIndexes...>)
      : mFirstA(std::forward<AArgs>(std::get<AIndexes>(aATuple))...),
        mSecondB(std::forward<BArgs>(std::get<BIndexes>(aBTuple))...) {}

 public:
  template <typename AArg, typename BArg>
  constexpr CompactPairHelper(AArg&& aA, BArg&& aB)
      : mFirstA(std::forward<AArg>(aA)), mSecondB(std::forward<BArg>(aB)) {}

  constexpr A& first() { return mFirstA; }
  constexpr const A& first() const { return mFirstA; }
  constexpr B& second() { return mSecondB; }
  constexpr const B& second() const { return mSecondB; }

  void swap(CompactPairHelper& aOther) {
    std::swap(mFirstA, aOther.mFirstA);
    std::swap(mSecondB, aOther.mSecondB);
  }

 private:
  A mFirstA;
  B mSecondB;
};

template <typename A, typename B>
struct CompactPairHelper<A, B, AsMember, AsBase> : private B {
 protected:
  template <typename... AArgs, std::size_t... AIndexes, typename... BArgs,
            std::size_t... BIndexes>
  constexpr CompactPairHelper(std::tuple<AArgs...>& aATuple,
                              std::tuple<BArgs...>& aBTuple,
                              std::index_sequence<AIndexes...>,
                              std::index_sequence<BIndexes...>)
      : B(std::forward<BArgs>(std::get<BIndexes>(aBTuple))...),
        mFirstA(std::forward<AArgs>(std::get<AIndexes>(aATuple))...) {}

 public:
  template <typename AArg, typename BArg>
  constexpr CompactPairHelper(AArg&& aA, BArg&& aB)
      : B(std::forward<BArg>(aB)), mFirstA(std::forward<AArg>(aA)) {}

  constexpr A& first() { return mFirstA; }
  constexpr const A& first() const { return mFirstA; }
  constexpr B& second() { return *this; }
  constexpr const B& second() const { return *this; }

  void swap(CompactPairHelper& aOther) {
    std::swap(mFirstA, aOther.mFirstA);
    std::swap(static_cast<B&>(*this), static_cast<B&>(aOther));
  }

 private:
  A mFirstA;
};

template <typename A, typename B>
struct CompactPairHelper<A, B, AsBase, AsMember> : private A {
 protected:
  template <typename... AArgs, std::size_t... AIndexes, typename... BArgs,
            std::size_t... BIndexes>
  constexpr CompactPairHelper(std::tuple<AArgs...>& aATuple,
                              std::tuple<BArgs...>& aBTuple,
                              std::index_sequence<AIndexes...>,
                              std::index_sequence<BIndexes...>)
      : A(std::forward<AArgs>(std::get<AIndexes>(aATuple))...),
        mSecondB(std::forward<BArgs>(std::get<BIndexes>(aBTuple))...) {}

 public:
  template <typename AArg, typename BArg>
  constexpr CompactPairHelper(AArg&& aA, BArg&& aB)
      : A(std::forward<AArg>(aA)), mSecondB(std::forward<BArg>(aB)) {}

  constexpr A& first() { return *this; }
  constexpr const A& first() const { return *this; }
  constexpr B& second() { return mSecondB; }
  constexpr const B& second() const { return mSecondB; }

  void swap(CompactPairHelper& aOther) {
    std::swap(static_cast<A&>(*this), static_cast<A&>(aOther));
    std::swap(mSecondB, aOther.mSecondB);
  }

 private:
  B mSecondB;
};

template <typename A, typename B>
struct CompactPairHelper<A, B, AsBase, AsBase> : private A, private B {
 protected:
  template <typename... AArgs, std::size_t... AIndexes, typename... BArgs,
            std::size_t... BIndexes>
  constexpr CompactPairHelper(std::tuple<AArgs...>& aATuple,
                              std::tuple<BArgs...>& aBTuple,
                              std::index_sequence<AIndexes...>,
                              std::index_sequence<BIndexes...>)
      : A(std::forward<AArgs>(std::get<AIndexes>(aATuple))...),
        B(std::forward<BArgs>(std::get<BIndexes>(aBTuple))...) {}

 public:
  template <typename AArg, typename BArg>
  constexpr CompactPairHelper(AArg&& aA, BArg&& aB)
      : A(std::forward<AArg>(aA)), B(std::forward<BArg>(aB)) {}

  constexpr A& first() { return static_cast<A&>(*this); }
  constexpr const A& first() const { return static_cast<A&>(*this); }
  constexpr B& second() { return static_cast<B&>(*this); }
  constexpr const B& second() const { return static_cast<B&>(*this); }

  void swap(CompactPairHelper& aOther) {
    std::swap(static_cast<A&>(*this), static_cast<A&>(aOther));
    std::swap(static_cast<B&>(*this), static_cast<B&>(aOther));
  }
};

}  // namespace detail

/**
 * CompactPair is the logical concatenation of an instance of A with an instance
 * B. Space is conserved when possible.  Neither A nor B may be a final class.
 *
 * In general if space conservation is not critical is preferred to use
 * std::pair.
 *
 * It's typically clearer to have individual A and B member fields.  Except if
 * you want the space-conserving qualities of CompactPair, you're probably
 * better off not using this!
 *
 * No guarantees are provided about the memory layout of A and B, the order of
 * initialization or destruction of A and B, and so on.  (This is approximately
 * required to optimize space usage.)  The first/second names are merely
 * conceptual!
 */
template <typename A, typename B>
struct CompactPair : private detail::CompactPairHelper<A, B> {
  typedef typename detail::CompactPairHelper<A, B> Base;

  using Base::Base;

  template <typename... AArgs, typename... BArgs>
  constexpr CompactPair(std::piecewise_construct_t, std::tuple<AArgs...> aFirst,
                        std::tuple<BArgs...> aSecond)
      : Base(aFirst, aSecond, std::index_sequence_for<AArgs...>(),
             std::index_sequence_for<BArgs...>()) {}

  CompactPair(CompactPair&& aOther) = default;
  CompactPair(const CompactPair& aOther) = default;

  CompactPair& operator=(CompactPair&& aOther) = default;
  CompactPair& operator=(const CompactPair& aOther) = default;

  /** The A instance. */
  using Base::first;
  /** The B instance. */
  using Base::second;

  /** Swap this pair with another pair. */
  void swap(CompactPair& aOther) { Base::swap(aOther); }
};

/**
 * MakeCompactPair allows you to construct a CompactPair instance using type
 * inference. A call like this:
 *
 *   MakeCompactPair(Foo(), Bar())
 *
 * will return a CompactPair<Foo, Bar>.
 */
template <typename A, typename B>
CompactPair<std::remove_cv_t<std::remove_reference_t<A>>,
            std::remove_cv_t<std::remove_reference_t<B>>>
MakeCompactPair(A&& aA, B&& aB) {
  return CompactPair<std::remove_cv_t<std::remove_reference_t<A>>,
                     std::remove_cv_t<std::remove_reference_t<B>>>(
      std::forward<A>(aA), std::forward<B>(aB));
}

/**
 * CompactPair equality comparison
 */
template <typename A, typename B>
bool operator==(const CompactPair<A, B>& aLhs, const CompactPair<A, B>& aRhs) {
  return aLhs.first() == aRhs.first() && aLhs.second() == aRhs.second();
}

}  // namespace mozilla

namespace std {

template <typename A, class B>
void swap(mozilla::CompactPair<A, B>& aX, mozilla::CompactPair<A, B>& aY) {
  aX.swap(aY);
}

}  // namespace std

#endif /* mozilla_CompactPair_h */
