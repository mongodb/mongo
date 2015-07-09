/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Pair.h"

using mozilla::Pair;

// Sizes aren't part of the guaranteed Pair interface, but we want to verify our
// attempts at compactness through EBO are moderately functional, *somewhere*.
#define INSTANTIATE(T1, T2, name, size) \
  Pair<T1, T2> name##_1(T1(0), T2(0)); \
  static_assert(sizeof(name##_1.first()) > 0, \
                "first method should work on Pair<" #T1 ", " #T2 ">"); \
  static_assert(sizeof(name##_1.second()) > 0, \
                "second method should work on Pair<" #T1 ", " #T2 ">"); \
  static_assert(sizeof(name##_1) == (size), \
                "Pair<" #T1 ", " #T2 "> has an unexpected size"); \
  Pair<T2, T1> name##_2(T2(0), T1(0)); \
  static_assert(sizeof(name##_2.first()) > 0, \
                "first method should work on Pair<" #T2 ", " #T1 ">"); \
  static_assert(sizeof(name##_2.second()) > 0, \
                "second method should work on Pair<" #T2 ", " #T1 ">"); \
  static_assert(sizeof(name##_2) == (size), \
                "Pair<" #T2 ", " #T1 "> has an unexpected size");

INSTANTIATE(int, int, prim1, 2 * sizeof(int));
INSTANTIATE(int, long, prim2, 2 * sizeof(long));

struct EmptyClass { explicit EmptyClass(int) {} };
struct NonEmpty { char mC; explicit NonEmpty(int) {} };

INSTANTIATE(int, EmptyClass, both1, sizeof(int));
INSTANTIATE(int, NonEmpty, both2, 2 * sizeof(int));
INSTANTIATE(EmptyClass, NonEmpty, both3, 1);

struct A { char dummy; explicit A(int) {} };
struct B : A { explicit B(int aI) : A(aI) {} };

INSTANTIATE(A, A, class1, 2);
INSTANTIATE(A, B, class2, 2);
INSTANTIATE(A, EmptyClass, class3, 1);

struct OtherEmpty : EmptyClass { explicit OtherEmpty(int aI) : EmptyClass(aI) {} };

// C++11 requires distinct objects of the same type, within the same "most
// derived object", to have different addresses.  Pair allocates its elements as
// two bases, a base and a member, or two members.  If the two elements have
// non-zero size or are unrelated, no big deal.  But if they're both empty and
// related, something -- possibly both -- must be inflated.  Exactly which are
// inflated depends which PairHelper specialization is used.  We could
// potentially assert something about size for this case, but whatever we could
// assert would be very finicky.  Plus it's two empty classes -- hardly likely.
// So don't bother trying to assert anything about this case.
//INSTANTIATE(EmptyClass, OtherEmpty, class4, ...something finicky...);

int
main()
{
  return 0;
}
