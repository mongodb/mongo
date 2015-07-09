/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A class holding a pair of objects that tries to conserve storage space. */

#ifndef mozilla_Pair_h
#define mozilla_Pair_h

#include "mozilla/Attributes.h"
#include "mozilla/Move.h"
#include "mozilla/TypeTraits.h"

namespace mozilla {

namespace detail {

enum StorageType { AsBase, AsMember };

// Optimize storage using the Empty Base Optimization -- that empty base classes
// don't take up space -- to optimize size when one or the other class is
// stateless and can be used as a base class.
//
// The extra conditions on storage for B are necessary so that PairHelper won't
// ambiguously inherit from either A or B, such that one or the other base class
// would be inaccessible.
template<typename A, typename B,
         detail::StorageType =
           IsEmpty<A>::value ? detail::AsBase : detail::AsMember,
         detail::StorageType =
           IsEmpty<B>::value && !IsBaseOf<A, B>::value && !IsBaseOf<B, A>::value
           ? detail::AsBase
           : detail::AsMember>
struct PairHelper;

template<typename A, typename B>
struct PairHelper<A, B, AsMember, AsMember>
{
protected:
  template<typename AArg, typename BArg>
  PairHelper(AArg&& aA, BArg&& aB)
    : mFirstA(Forward<AArg>(aA)),
      mSecondB(Forward<BArg>(aB))
  {}

  A& first() { return mFirstA; }
  const A& first() const { return mFirstA; }
  B& second() { return mSecondB; }
  const B& second() const { return mSecondB; }

  void swap(PairHelper& aOther)
  {
    Swap(mFirstA, aOther.mFirstA);
    Swap(mSecondB, aOther.mSecondB);
  }

private:
  A mFirstA;
  B mSecondB;
};

template<typename A, typename B>
struct PairHelper<A, B, AsMember, AsBase> : private B
{
protected:
  template<typename AArg, typename BArg>
  PairHelper(AArg&& aA, BArg&& aB)
    : B(Forward<BArg>(aB)),
      mFirstA(Forward<AArg>(aA))
  {}

  A& first() { return mFirstA; }
  const A& first() const { return mFirstA; }
  B& second() { return *this; }
  const B& second() const { return *this; }

  void swap(PairHelper& aOther)
  {
    Swap(mFirstA, aOther.mFirstA);
    Swap(static_cast<B&>(*this), static_cast<B&>(aOther));
  }

private:
  A mFirstA;
};

template<typename A, typename B>
struct PairHelper<A, B, AsBase, AsMember> : private A
{
protected:
  template<typename AArg, typename BArg>
  PairHelper(AArg&& aA, BArg&& aB)
    : A(Forward<AArg>(aA)),
      mSecondB(Forward<BArg>(aB))
  {}

  A& first() { return *this; }
  const A& first() const { return *this; }
  B& second() { return mSecondB; }
  const B& second() const { return mSecondB; }

  void swap(PairHelper& aOther)
  {
    Swap(static_cast<A&>(*this), static_cast<A&>(aOther));
    Swap(mSecondB, aOther.mSecondB);
  }

private:
  B mSecondB;
};

template<typename A, typename B>
struct PairHelper<A, B, AsBase, AsBase> : private A, private B
{
protected:
  template<typename AArg, typename BArg>
  PairHelper(AArg&& aA, BArg&& aB)
    : A(Forward<AArg>(aA)),
      B(Forward<BArg>(aB))
  {}

  A& first() { return static_cast<A&>(*this); }
  const A& first() const { return static_cast<A&>(*this); }
  B& second() { return static_cast<B&>(*this); }
  const B& second() const { return static_cast<B&>(*this); }

  void swap(PairHelper& aOther)
  {
    Swap(static_cast<A&>(*this), static_cast<A&>(aOther));
    Swap(static_cast<B&>(*this), static_cast<B&>(aOther));
  }
};

} // namespace detail

/**
 * Pair is the logical concatenation of an instance of A with an instance B.
 * Space is conserved when possible.  Neither A nor B may be a final class.
 *
 * It's typically clearer to have individual A and B member fields.  Except if
 * you want the space-conserving qualities of Pair, you're probably better off
 * not using this!
 *
 * No guarantees are provided about the memory layout of A and B, the order of
 * initialization or destruction of A and B, and so on.  (This is approximately
 * required to optimize space usage.)  The first/second names are merely
 * conceptual!
 */
template<typename A, typename B>
struct Pair
  : private detail::PairHelper<A, B>
{
  typedef typename detail::PairHelper<A, B> Base;

public:
  template<typename AArg, typename BArg>
  Pair(AArg&& aA, BArg&& aB)
    : Base(Forward<AArg>(aA), Forward<BArg>(aB))
  {}

  /** The A instance. */
  using Base::first;
  /** The B instance. */
  using Base::second;

  /** Swap this pair with another pair. */
  void swap(Pair& aOther) { Base::swap(aOther); }

private:
  Pair(const Pair&) = delete;
};

template<typename A, class B>
void
Swap(Pair<A, B>& aX, Pair<A, B>& aY)
{
  aX.swap(aY);
}

} // namespace mozilla

#endif /* mozilla_Pair_h */
