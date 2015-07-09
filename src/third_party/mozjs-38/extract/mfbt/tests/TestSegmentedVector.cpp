/* -*- Mode: C++; tab-width: 9; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// This is included first to ensure it doesn't implicitly depend on anything
// else.
#include "mozilla/SegmentedVector.h"

#include "mozilla/Alignment.h"
#include "mozilla/Assertions.h"

using mozilla::SegmentedVector;

// It would be nice if we could use the InfallibleAllocPolicy from mozalloc,
// but MFBT cannot use mozalloc.
class InfallibleAllocPolicy
{
public:
  template <typename T>
  T* pod_malloc(size_t aNumElems)
  {
    if (aNumElems & mozilla::tl::MulOverflowMask<sizeof(T)>::value) {
      MOZ_CRASH("TestSegmentedVector.cpp: overflow");
    }
    T* rv = static_cast<T*>(malloc(aNumElems * sizeof(T)));
    if (!rv) {
      MOZ_CRASH("TestSegmentedVector.cpp: out of memory");
    }
    return rv;
  }

  void free_(void* aPtr) { free(aPtr); }
};

// We want to test Append(), which is fallible and marked with
// MOZ_WARN_UNUSED_RESULT. But we're using an infallible alloc policy, and so
// don't really need to check the result. Casting to |void| works with clang
// but not GCC, so we instead use this dummy variable which works with both
// compilers.
static int gDummy;

// This tests basic segmented vector construction and iteration.
void TestBasics()
{
  // A SegmentedVector with a POD element type.
  typedef SegmentedVector<int, 1024, InfallibleAllocPolicy> MyVector;
  MyVector v;
  int i, n;

  MOZ_RELEASE_ASSERT(v.IsEmpty());

  // Add 100 elements, then check various things.
  i = 0;
  for ( ; i < 100; i++) {
    gDummy = v.Append(mozilla::Move(i));
  }
  MOZ_RELEASE_ASSERT(!v.IsEmpty());
  MOZ_RELEASE_ASSERT(v.Length() == 100);

  n = 0;
  for (auto iter = v.Iter(); !iter.Done(); iter.Next()) {
    MOZ_RELEASE_ASSERT(iter.Get() == n);
    n++;
  }
  MOZ_RELEASE_ASSERT(n == 100);

  // Add another 900 elements, then re-check.
  for ( ; i < 1000; i++) {
    v.InfallibleAppend(mozilla::Move(i));
  }
  MOZ_RELEASE_ASSERT(!v.IsEmpty());
  MOZ_RELEASE_ASSERT(v.Length() == 1000);

  n = 0;
  for (auto iter = v.Iter(); !iter.Done(); iter.Next()) {
    MOZ_RELEASE_ASSERT(iter.Get() == n);
    n++;
  }
  MOZ_RELEASE_ASSERT(n == 1000);

  v.Clear();
  MOZ_RELEASE_ASSERT(v.IsEmpty());
  MOZ_RELEASE_ASSERT(v.Length() == 0);
}

static size_t gNumDefaultCtors;
static size_t gNumExplicitCtors;
static size_t gNumCopyCtors;
static size_t gNumMoveCtors;
static size_t gNumDtors;

struct NonPOD
{
  NonPOD()                { gNumDefaultCtors++; }
  explicit NonPOD(int x)  { gNumExplicitCtors++; }
  NonPOD(NonPOD&)         { gNumCopyCtors++; }
  NonPOD(NonPOD&&)        { gNumMoveCtors++; }
  ~NonPOD()               { gNumDtors++; }
};

// This tests how segmented vectors with non-POD elements construct and
// destruct those elements.
void TestConstructorsAndDestructors()
{
  {
    // A SegmentedVector with a non-POD element type.
    NonPOD x(1);                          // explicit constructor called
    SegmentedVector<NonPOD, 64, InfallibleAllocPolicy> v;
                                          // default constructor called 0 times
    MOZ_RELEASE_ASSERT(v.IsEmpty());
    gDummy = v.Append(x);                 // copy constructor called
    NonPOD y(1);                          // explicit constructor called
    gDummy = v.Append(mozilla::Move(y));  // move constructor called
    NonPOD z(1);                          // explicit constructor called
    v.InfallibleAppend(mozilla::Move(z)); // move constructor called
    v.Clear();                            // destructor called 3 times

    MOZ_RELEASE_ASSERT(gNumDefaultCtors  == 0);
    MOZ_RELEASE_ASSERT(gNumExplicitCtors == 3);
    MOZ_RELEASE_ASSERT(gNumCopyCtors     == 1);
    MOZ_RELEASE_ASSERT(gNumMoveCtors     == 2);
    MOZ_RELEASE_ASSERT(gNumDtors         == 3);
  }                                       // destructor called for x, y, z
  MOZ_RELEASE_ASSERT(gNumDtors == 6);
}

struct A { int mX; int mY; };
struct B { int mX; char mY; double mZ; };
struct C { A mA; B mB; };
struct D { char mBuf[101]; };
struct E { };

// This tests that we get the right segment capacities for specified segment
// sizes, and that the elements are aligned appropriately.
void TestSegmentCapacitiesAndAlignments()
{
  // When SegmentedVector's constructor is passed a size, it asserts that the
  // vector's segment capacity results in a segment size equal to (or very
  // close to) the passed size.
  //
  // Also, SegmentedVector has a static assertion that elements are
  // appropriately aligned.
  SegmentedVector<double, 512> v1(512);
  SegmentedVector<A, 1024> v2(1024);
  SegmentedVector<B, 999> v3(999);
  SegmentedVector<C, 10> v4(10);
  SegmentedVector<D, 1234> v5(1234);
  SegmentedVector<E> v6(4096);  // 4096 is the default segment size
  SegmentedVector<mozilla::AlignedElem<16>, 100> v7(100);
}

int main(void)
{
  TestBasics();
  TestConstructorsAndDestructors();
  TestSegmentCapacitiesAndAlignments();

  return 0;
}
