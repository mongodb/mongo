/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/Compiler.h"
#include "mozilla/Move.h"
#include "mozilla/TypeTraits.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"

#include <stddef.h>

using mozilla::DefaultDelete;
using mozilla::IsSame;
using mozilla::MakeUnique;
using mozilla::Swap;
using mozilla::UniquePtr;
using mozilla::Vector;

#define CHECK(c) \
  do { \
    bool cond = !!(c); \
    MOZ_ASSERT(cond, "Failed assertion: " #c); \
    if (!cond) { \
      return false; \
    } \
  } while (false)

typedef UniquePtr<int> NewInt;
static_assert(sizeof(NewInt) == sizeof(int*), "stored most efficiently");

static size_t gADestructorCalls = 0;

struct A
{
public:
  A() : mX(0) {}
  virtual ~A() { gADestructorCalls++; }

  int mX;
};

static size_t gBDestructorCalls = 0;

struct B : public A
{
public:
  B() : mY(1) {}
  ~B() { gBDestructorCalls++; }

  int mY;
};

typedef UniquePtr<A> UniqueA;
typedef UniquePtr<B, UniqueA::DeleterType> UniqueB; // permit interconversion

static_assert(sizeof(UniqueA) == sizeof(A*), "stored most efficiently");
static_assert(sizeof(UniqueB) == sizeof(B*), "stored most efficiently");

struct DeleterSubclass : UniqueA::DeleterType {};

typedef UniquePtr<B, DeleterSubclass> UniqueC;
static_assert(sizeof(UniqueC) == sizeof(B*), "stored most efficiently");

static UniqueA
ReturnUniqueA()
{
  return UniqueA(new B);
}

static UniqueA
ReturnLocalA()
{
  UniqueA a(new A);
  return Move(a);
}

static bool
TestDefaultFreeGuts()
{
  static_assert(IsSame<NewInt::DeleterType, DefaultDelete<int> >::value,
                "weird deleter?");

  NewInt n1(new int);
  CHECK(n1);
  CHECK(n1.get() != nullptr);

  n1 = nullptr;
  CHECK(!n1);
  CHECK(n1.get() == nullptr);

  int* p1 = new int;
  n1.reset(p1);
  CHECK(n1);
  NewInt n2(Move(n1));
  CHECK(!n1);
  CHECK(n1.get() == nullptr);
  CHECK(n2.get() == p1);

  Swap(n1, n2);
  CHECK(n1.get() == p1);
  CHECK(n2.get() == nullptr);

  n1.swap(n2);
  CHECK(n1.get() == nullptr);
  CHECK(n2.get() == p1);
  delete n2.release();

  CHECK(n1.get() == nullptr);
  CHECK(n2 == nullptr);
  CHECK(nullptr == n2);

  int* p2 = new int;
  int* p3 = new int;
  n1.reset(p2);
  n2.reset(p3);
  CHECK(n1.get() == p2);
  CHECK(n2.get() == p3);

  n1.swap(n2);
  CHECK(n2 != nullptr);
  CHECK(nullptr != n2);
  CHECK(n2.get() == p2);
  CHECK(n1.get() == p3);

  UniqueA a1;
  CHECK(a1 == nullptr);
  a1.reset(new A);
  CHECK(gADestructorCalls == 0);
  CHECK(a1->mX == 0);

  B* bp1 = new B;
  bp1->mX = 5;
  CHECK(gBDestructorCalls == 0);
  a1.reset(bp1);
  CHECK(gADestructorCalls == 1);
  CHECK(a1->mX == 5);
  a1.reset(nullptr);
  CHECK(gADestructorCalls == 2);
  CHECK(gBDestructorCalls == 1);

  B* bp2 = new B;
  UniqueB b1(bp2);
  UniqueA a2(nullptr);
  a2 = Move(b1);
  CHECK(gADestructorCalls == 2);
  CHECK(gBDestructorCalls == 1);

  UniqueA a3(Move(a2));
  a3 = nullptr;
  CHECK(gADestructorCalls == 3);
  CHECK(gBDestructorCalls == 2);

  B* bp3 = new B;
  bp3->mX = 42;
  UniqueB b2(bp3);
  UniqueA a4(Move(b2));
  CHECK(b2.get() == nullptr);
  CHECK((*a4).mX == 42);
  CHECK(gADestructorCalls == 3);
  CHECK(gBDestructorCalls == 2);

  UniqueA a5(new A);
  UniqueB b3(new B);
  a5 = Move(b3);
  CHECK(gADestructorCalls == 4);
  CHECK(gBDestructorCalls == 2);

  ReturnUniqueA();
  CHECK(gADestructorCalls == 5);
  CHECK(gBDestructorCalls == 3);

  ReturnLocalA();
  CHECK(gADestructorCalls == 6);
  CHECK(gBDestructorCalls == 3);

  UniqueA a6(ReturnLocalA());
  a6 = nullptr;
  CHECK(gADestructorCalls == 7);
  CHECK(gBDestructorCalls == 3);

  UniqueC c1(new B);
  UniqueA a7(new B);
  a7 = Move(c1);
  CHECK(gADestructorCalls == 8);
  CHECK(gBDestructorCalls == 4);

  c1.reset(new B);

  UniqueA a8(Move(c1));
  CHECK(gADestructorCalls == 8);
  CHECK(gBDestructorCalls == 4);

  // These smart pointers still own B resources.
  CHECK(a4);
  CHECK(a5);
  CHECK(a7);
  CHECK(a8);
  return true;
}

static bool
TestDefaultFree()
{
  CHECK(TestDefaultFreeGuts());
  CHECK(gADestructorCalls == 12);
  CHECK(gBDestructorCalls == 8);
  return true;
}

static size_t FreeClassCounter = 0;

struct FreeClass
{
public:
  FreeClass() {}

  void operator()(int* aPtr)
  {
    FreeClassCounter++;
    delete aPtr;
  }
};

typedef UniquePtr<int, FreeClass> NewIntCustom;
static_assert(sizeof(NewIntCustom) == sizeof(int*),
              "stored most efficiently");

static bool
TestFreeClass()
{
  CHECK(FreeClassCounter == 0);
  {
    NewIntCustom n1(new int);
    CHECK(FreeClassCounter == 0);
  }
  CHECK(FreeClassCounter == 1);

  NewIntCustom n2;
  {
    NewIntCustom n3(new int);
    CHECK(FreeClassCounter == 1);
    n2 = Move(n3);
  }
  CHECK(FreeClassCounter == 1);
  n2 = nullptr;
  CHECK(FreeClassCounter == 2);

  n2.reset(nullptr);
  CHECK(FreeClassCounter == 2);
  n2.reset(new int);
  n2.reset();
  CHECK(FreeClassCounter == 3);

  NewIntCustom n4(new int, FreeClass());
  CHECK(FreeClassCounter == 3);
  n4.reset(new int);
  CHECK(FreeClassCounter == 4);
  n4.reset();
  CHECK(FreeClassCounter == 5);

  FreeClass f;
  NewIntCustom n5(new int, f);
  CHECK(FreeClassCounter == 5);
  int* p = n5.release();
  CHECK(FreeClassCounter == 5);
  delete p;

  return true;
}

typedef UniquePtr<int, DefaultDelete<int>&> IntDeleterRef;
typedef UniquePtr<A, DefaultDelete<A>&> ADeleterRef;
typedef UniquePtr<B, DefaultDelete<A>&> BDeleterRef;

static_assert(sizeof(IntDeleterRef) > sizeof(int*),
              "has to be heavier than an int* to store the reference");
static_assert(sizeof(ADeleterRef) > sizeof(A*),
              "has to be heavier than an A* to store the reference");
static_assert(sizeof(BDeleterRef) > sizeof(int*),
              "has to be heavier than a B* to store the reference");

static bool
TestReferenceDeleterGuts()
{
  DefaultDelete<int> delInt;
  IntDeleterRef id1(new int, delInt);

  IntDeleterRef id2(Move(id1));
  CHECK(id1 == nullptr);
  CHECK(nullptr != id2);
  CHECK(&id1.getDeleter() == &id2.getDeleter());

  IntDeleterRef id3(Move(id2));

  DefaultDelete<A> delA;
  ADeleterRef a1(new A, delA);
  a1.reset(nullptr);
  a1.reset(new B);
  a1 = nullptr;

  BDeleterRef b1(new B, delA);
  a1 = Move(b1);

  BDeleterRef b2(new B, delA);

  ADeleterRef a2(Move(b2));

  return true;
}

static bool
TestReferenceDeleter()
{
  gADestructorCalls = 0;
  gBDestructorCalls = 0;

  CHECK(TestReferenceDeleterGuts());

  CHECK(gADestructorCalls == 4);
  CHECK(gBDestructorCalls == 3);

  gADestructorCalls = 0;
  gBDestructorCalls = 0;
  return true;
}

typedef void (&FreeSignature)(void*);

static size_t DeleteIntFunctionCallCount = 0;

static void
DeleteIntFunction(void* aPtr)
{
  DeleteIntFunctionCallCount++;
  delete static_cast<int*>(aPtr);
}

static void
SetMallocedInt(UniquePtr<int, FreeSignature>& aPtr, int aI)
{
  int* newPtr = static_cast<int*>(malloc(sizeof(int)));
  *newPtr = aI;
  aPtr.reset(newPtr);
}

static UniquePtr<int, FreeSignature>
MallocedInt(int aI)
{
  UniquePtr<int, FreeSignature>
    ptr(static_cast<int*>(malloc(sizeof(int))), free);
  *ptr = aI;
  return Move(ptr);
}
static bool
TestFunctionReferenceDeleter()
{
  // Look for allocator mismatches and leaks to verify these bits
  UniquePtr<int, FreeSignature> i1(MallocedInt(17));
  CHECK(*i1 == 17);

  SetMallocedInt(i1, 42);
  CHECK(*i1 == 42);

  // These bits use a custom deleter so we can instrument deletion.
  {
    UniquePtr<int, FreeSignature> i2 =
      UniquePtr<int, FreeSignature>(new int(42), DeleteIntFunction);
    CHECK(DeleteIntFunctionCallCount == 0);

    i2.reset(new int(76));
    CHECK(DeleteIntFunctionCallCount == 1);
  }

  CHECK(DeleteIntFunctionCallCount == 2);

  return true;
}

template<typename T>
struct AppendNullptrTwice
{
  AppendNullptrTwice() {}

  bool operator()(Vector<T>& vec)
  {
    CHECK(vec.append(nullptr));
    CHECK(vec.append(nullptr));
    return true;
  }
};

static size_t AAfter;
static size_t BAfter;

static bool
TestVectorGuts()
{
  Vector<UniqueA> vec;
  CHECK(vec.append(new B));
  CHECK(vec.append(new A));
  CHECK(AppendNullptrTwice<UniqueA>()(vec));
  CHECK(vec.append(new B));

  size_t initialLength = vec.length();

  UniqueA* begin = vec.begin();
  bool appendA = true;
  do {
    CHECK(appendA ? vec.append(new A) : vec.append(new B));
    appendA = !appendA;
  } while (begin == vec.begin());

  size_t numAppended = vec.length() - initialLength;

  BAfter = numAppended / 2;
  AAfter = numAppended - numAppended / 2;

  CHECK(gADestructorCalls == 0);
  CHECK(gBDestructorCalls == 0);
  return true;
}

static bool
TestVector()
{
  gADestructorCalls = 0;
  gBDestructorCalls = 0;

  CHECK(TestVectorGuts());

  CHECK(gADestructorCalls == 3 + AAfter + BAfter);
  CHECK(gBDestructorCalls == 2 + BAfter);
  return true;
}

typedef UniquePtr<int[]> IntArray;
static_assert(sizeof(IntArray) == sizeof(int*),
              "stored most efficiently");

static bool
TestArray()
{
  static_assert(IsSame<IntArray::DeleterType, DefaultDelete<int[]> >::value,
                "weird deleter?");

  IntArray n1(new int[5]);
  CHECK(n1);
  CHECK(n1.get() != nullptr);

  n1 = nullptr;
  CHECK(!n1);
  CHECK(n1.get() == nullptr);

  int* p1 = new int[42];
  n1.reset(p1);
  CHECK(n1);
  IntArray n2(Move(n1));
  CHECK(!n1);
  CHECK(n1.get() == nullptr);
  CHECK(n2.get() == p1);

  Swap(n1, n2);
  CHECK(n1.get() == p1);
  CHECK(n2.get() == nullptr);

  n1.swap(n2);
  CHECK(n1.get() == nullptr);
  CHECK(n2.get() == p1);
  delete[] n2.release();

  CHECK(n1.get() == nullptr);
  CHECK(n2.get() == nullptr);

  int* p2 = new int[7];
  int* p3 = new int[42];
  n1.reset(p2);
  n2.reset(p3);
  CHECK(n1.get() == p2);
  CHECK(n2.get() == p3);

  n1.swap(n2);
  CHECK(n2.get() == p2);
  CHECK(n1.get() == p3);

  n1 = Move(n2);
  CHECK(n1.get() == p2);
  n1 = Move(n2);
  CHECK(n1.get() == nullptr);

  UniquePtr<A[]> a1(new A[17]);
  static_assert(sizeof(a1) == sizeof(A*),
                "stored most efficiently");

  UniquePtr<A[]> a2(new A[5], DefaultDelete<A[]>());
  a2.reset(nullptr);
  a2.reset(new A[17]);
  a2 = nullptr;

  UniquePtr<A[]> a3(nullptr);
  a3.reset(new A[7]);

  return true;
}

struct Q
{
  Q() {}
  Q(const Q&) {}

  Q(Q&, char) {}

  template<typename T>
  Q(Q, T&&, int) {}

  Q(int, long, double, void*) {}
};

static int randomInt() { return 4; }

static bool
TestMakeUnique()
{
  UniquePtr<int> a1(MakeUnique<int>());
  UniquePtr<long> a2(MakeUnique<long>(4));

  // no args, easy
  UniquePtr<Q> q0(MakeUnique<Q>());

  // temporary bound to const lval ref
  UniquePtr<Q> q1(MakeUnique<Q>(Q()));

  // passing through a non-const lval ref
  UniquePtr<Q> q2(MakeUnique<Q>(*q1, 'c'));

  // pass by copying, forward a temporary, pass by value
  UniquePtr<Q> q3(MakeUnique<Q>(Q(), UniquePtr<int>(), randomInt()));

  // various type mismatching to test "fuzzy" forwarding
  UniquePtr<Q> q4(MakeUnique<Q>('s', 66LL, 3.141592654, &q3));

  UniquePtr<char[]> c1(MakeUnique<char[]>(5));

  return true;
}

int
main()
{
  if (!TestDefaultFree()) {
    return 1;
  }
  if (!TestFreeClass()) {
    return 1;
  }
  if (!TestReferenceDeleter()) {
    return 1;
  }
  if (!TestFunctionReferenceDeleter()) {
    return 1;
  }
  if (!TestVector()) {
    return 1;
  }
  if (!TestArray()) {
    return 1;
  }
  if (!TestMakeUnique()) {
    return 1;
  }
  return 0;
}
