/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/BinarySearch.h"
#include "mozilla/Vector.h"

using mozilla::ArrayLength;
using mozilla::BinarySearch;
using mozilla::BinarySearchIf;
using mozilla::Vector;

#define A(a) MOZ_RELEASE_ASSERT(a)

struct Person
{
  int mAge;
  int mId;
  Person(int aAge, int aId) : mAge(aAge), mId(aId) {}
};

struct GetAge
{
  Vector<Person>& mV;
  explicit GetAge(Vector<Person>& aV) : mV(aV) {}
  int operator[](size_t index) const { return mV[index].mAge; }
};

struct RangeFinder {
  const int mLower, mUpper;
  RangeFinder(int lower, int upper) : mLower(lower), mUpper(upper) {}
  int operator()(int val) const {
    if (val >= mUpper) return -1;
    if (val < mLower) return 1;
    return 0;
  }
};

static void
TestBinarySearch()
{
  size_t m;

  Vector<int> v1;
  v1.append(2);
  v1.append(4);
  v1.append(6);
  v1.append(8);

  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 0, v1.length(), 1, &m) && m == 0);
  MOZ_RELEASE_ASSERT( BinarySearch(v1, 0, v1.length(), 2, &m) && m == 0);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 0, v1.length(), 3, &m) && m == 1);
  MOZ_RELEASE_ASSERT( BinarySearch(v1, 0, v1.length(), 4, &m) && m == 1);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 0, v1.length(), 5, &m) && m == 2);
  MOZ_RELEASE_ASSERT( BinarySearch(v1, 0, v1.length(), 6, &m) && m == 2);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 0, v1.length(), 7, &m) && m == 3);
  MOZ_RELEASE_ASSERT( BinarySearch(v1, 0, v1.length(), 8, &m) && m == 3);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 0, v1.length(), 9, &m) && m == 4);

  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 1, 3, 1, &m) && m == 1);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 1, 3, 2, &m) && m == 1);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 1, 3, 3, &m) && m == 1);
  MOZ_RELEASE_ASSERT( BinarySearch(v1, 1, 3, 4, &m) && m == 1);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 1, 3, 5, &m) && m == 2);
  MOZ_RELEASE_ASSERT( BinarySearch(v1, 1, 3, 6, &m) && m == 2);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 1, 3, 7, &m) && m == 3);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 1, 3, 8, &m) && m == 3);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 1, 3, 9, &m) && m == 3);

  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 0, 0, 0, &m) && m == 0);
  MOZ_RELEASE_ASSERT(!BinarySearch(v1, 0, 0, 9, &m) && m == 0);

  Vector<int> v2;
  MOZ_RELEASE_ASSERT(!BinarySearch(v2, 0, 0, 0, &m) && m == 0);
  MOZ_RELEASE_ASSERT(!BinarySearch(v2, 0, 0, 9, &m) && m == 0);

  Vector<Person> v3;
  v3.append(Person(2, 42));
  v3.append(Person(4, 13));
  v3.append(Person(6, 360));

  A(!BinarySearch(GetAge(v3), 0, v3.length(), 1, &m) && m == 0);
  A( BinarySearch(GetAge(v3), 0, v3.length(), 2, &m) && m == 0);
  A(!BinarySearch(GetAge(v3), 0, v3.length(), 3, &m) && m == 1);
  A( BinarySearch(GetAge(v3), 0, v3.length(), 4, &m) && m == 1);
  A(!BinarySearch(GetAge(v3), 0, v3.length(), 5, &m) && m == 2);
  A( BinarySearch(GetAge(v3), 0, v3.length(), 6, &m) && m == 2);
  A(!BinarySearch(GetAge(v3), 0, v3.length(), 7, &m) && m == 3);
}

static void
TestBinarySearchIf()
{
  const int v1[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  const size_t len = ArrayLength(v1);
  size_t m;

  A( BinarySearchIf(v1, 0, len, RangeFinder( 2,  3), &m) && m == 2);
  A(!BinarySearchIf(v1, 0, len, RangeFinder(-5, -2), &m) && m == 0);
  A( BinarySearchIf(v1, 0, len, RangeFinder( 3,  5), &m) && m >= 3 && m < 5);
  A(!BinarySearchIf(v1, 0, len, RangeFinder(10, 12), &m) && m == 10);
}

int
main()
{
  TestBinarySearch();
  TestBinarySearchIf();
  return 0;
}
