/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/WeakPtr.h"

using mozilla::SupportsWeakPtr;
using mozilla::WeakPtr;

// To have a class C support weak pointers, inherit from SupportsWeakPtr<C>.
class C : public SupportsWeakPtr<C>
{
public:
  MOZ_DECLARE_WEAKREFERENCE_TYPENAME(C)

  int mNum;

  C()
    : mNum(0)
  {}

  ~C()
  {
    // Setting mNum in the destructor allows us to test against use-after-free below
    mNum = 0xDEAD;
  }

  void act() {}

  bool isConst() {
    return false;
  }

  bool isConst() const {
    return true;
  }
};

bool isConst(C*)
{
  return false;
}

bool isConst(const C*)
{
  return true;
}

int
main()
{
  C* c1 = new C;
  MOZ_RELEASE_ASSERT(c1->mNum == 0);

  // Get weak pointers to c1. The first time,
  // a reference-counted WeakReference object is created that
  // can live beyond the lifetime of 'c1'. The WeakReference
  // object will be notified of 'c1's destruction.
  WeakPtr<C> w1 = c1;
  // Test a weak pointer for validity before using it.
  MOZ_RELEASE_ASSERT(w1);
  MOZ_RELEASE_ASSERT(w1 == c1);
  w1->mNum = 1;
  w1->act();

  // Test taking another WeakPtr<C> to c1
  WeakPtr<C> w2 = c1;
  MOZ_RELEASE_ASSERT(w2);
  MOZ_RELEASE_ASSERT(w2 == c1);
  MOZ_RELEASE_ASSERT(w2 == w1);
  MOZ_RELEASE_ASSERT(w2->mNum == 1);

  // Test a WeakPtr<const C>
  WeakPtr<const C> w3const = c1;
  MOZ_RELEASE_ASSERT(w3const);
  MOZ_RELEASE_ASSERT(w3const == c1);
  MOZ_RELEASE_ASSERT(w3const == w1);
  MOZ_RELEASE_ASSERT(w3const == w2);
  MOZ_RELEASE_ASSERT(w3const->mNum == 1);

  // Test const-correctness of operator-> and operator T*
  MOZ_RELEASE_ASSERT(!w1->isConst());
  MOZ_RELEASE_ASSERT(w3const->isConst());
  MOZ_RELEASE_ASSERT(!isConst(w1));
  MOZ_RELEASE_ASSERT(isConst(w3const));

  // Test that when a WeakPtr is destroyed, it does not destroy the object that it points to,
  // and it does not affect other WeakPtrs pointing to the same object (e.g. it does not
  // destroy the WeakReference object).
  {
    WeakPtr<C> w4local = c1;
    MOZ_RELEASE_ASSERT(w4local == c1);
  }
  // Now w4local has gone out of scope. If that had destroyed c1, then the following would fail
  // for sure (see C::~C()).
  MOZ_RELEASE_ASSERT(c1->mNum == 1);
  // Check that w4local going out of scope hasn't affected other WeakPtr's pointing to c1
  MOZ_RELEASE_ASSERT(w1 == c1);
  MOZ_RELEASE_ASSERT(w2 == c1);

  // Now construct another C object and test changing what object a WeakPtr points to
  C* c2 = new C;
  c2->mNum = 2;
  MOZ_RELEASE_ASSERT(w2->mNum == 1); // w2 was pointing to c1
  w2 = c2;
  MOZ_RELEASE_ASSERT(w2);
  MOZ_RELEASE_ASSERT(w2 == c2);
  MOZ_RELEASE_ASSERT(w2 != c1);
  MOZ_RELEASE_ASSERT(w2 != w1);
  MOZ_RELEASE_ASSERT(w2->mNum == 2);

  // Destroying the underlying object clears weak pointers to it.
  // It should not affect pointers that are not currently pointing to it.
  delete c1;
  MOZ_RELEASE_ASSERT(!w1, "Deleting an object should clear WeakPtr's to it.");
  MOZ_RELEASE_ASSERT(!w3const, "Deleting an object should clear WeakPtr's to it.");
  MOZ_RELEASE_ASSERT(w2, "Deleting an object should not clear WeakPtr that are not pointing to it.");

  delete c2;
  MOZ_RELEASE_ASSERT(!w2, "Deleting an object should clear WeakPtr's to it.");
}
