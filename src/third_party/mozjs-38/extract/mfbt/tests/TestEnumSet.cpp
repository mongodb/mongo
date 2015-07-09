/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/EnumSet.h"

using namespace mozilla;

enum SeaBird
{
  PENGUIN,
  ALBATROSS,
  FULMAR,
  PRION,
  SHEARWATER,
  GADFLY_PETREL,
  TRUE_PETREL,
  DIVING_PETREL,
  STORM_PETREL,
  PELICAN,
  GANNET,
  BOOBY,
  CORMORANT,
  FRIGATEBIRD,
  TROPICBIRD,
  SKUA,
  GULL,
  TERN,
  SKIMMER,
  AUK
};

class EnumSetSuite
{
public:
  EnumSetSuite()
    : mAlcidae()
    , mDiomedeidae(ALBATROSS)
    , mPetrelProcellariidae(GADFLY_PETREL, TRUE_PETREL)
    , mNonPetrelProcellariidae(FULMAR, PRION, SHEARWATER)
    , mPetrels(GADFLY_PETREL, TRUE_PETREL, DIVING_PETREL, STORM_PETREL)
  { }

  void runTests()
  {
    testSize();
    testContains();
    testAddTo();
    testAdd();
    testAddAll();
    testUnion();
    testRemoveFrom();
    testRemove();
    testRemoveAllFrom();
    testRemoveAll();
    testIntersect();
    testInsersection();
    testEquality();
    testDuplicates();
  }

private:
  void testSize()
  {
    MOZ_RELEASE_ASSERT(mAlcidae.size() == 0);
    MOZ_RELEASE_ASSERT(mDiomedeidae.size() == 1);
    MOZ_RELEASE_ASSERT(mPetrelProcellariidae.size() == 2);
    MOZ_RELEASE_ASSERT(mNonPetrelProcellariidae.size() == 3);
    MOZ_RELEASE_ASSERT(mPetrels.size() == 4);
  }

  void testContains()
  {
    MOZ_RELEASE_ASSERT(!mPetrels.contains(PENGUIN));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(ALBATROSS));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(FULMAR));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(PRION));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(SHEARWATER));
    MOZ_RELEASE_ASSERT(mPetrels.contains(GADFLY_PETREL));
    MOZ_RELEASE_ASSERT(mPetrels.contains(TRUE_PETREL));
    MOZ_RELEASE_ASSERT(mPetrels.contains(DIVING_PETREL));
    MOZ_RELEASE_ASSERT(mPetrels.contains(STORM_PETREL));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(PELICAN));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(GANNET));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(BOOBY));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(CORMORANT));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(FRIGATEBIRD));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(TROPICBIRD));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(SKUA));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(GULL));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(TERN));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(SKIMMER));
    MOZ_RELEASE_ASSERT(!mPetrels.contains(AUK));
  }

  void testCopy()
  {
    EnumSet<SeaBird> likes = mPetrels;
    likes -= TRUE_PETREL;
    MOZ_RELEASE_ASSERT(mPetrels.size() == 4);
    MOZ_RELEASE_ASSERT(mPetrels.contains(TRUE_PETREL));

    MOZ_RELEASE_ASSERT(likes.size() == 3);
    MOZ_RELEASE_ASSERT(likes.contains(GADFLY_PETREL));
    MOZ_RELEASE_ASSERT(likes.contains(DIVING_PETREL));
    MOZ_RELEASE_ASSERT(likes.contains(STORM_PETREL));
  }

  void testAddTo()
  {
    EnumSet<SeaBird> seen = mPetrels;
    seen += CORMORANT;
    seen += TRUE_PETREL;
    MOZ_RELEASE_ASSERT(mPetrels.size() == 4);
    MOZ_RELEASE_ASSERT(!mPetrels.contains(CORMORANT));
    MOZ_RELEASE_ASSERT(seen.size() == 5);
    MOZ_RELEASE_ASSERT(seen.contains(GADFLY_PETREL));
    MOZ_RELEASE_ASSERT(seen.contains(TRUE_PETREL));
    MOZ_RELEASE_ASSERT(seen.contains(DIVING_PETREL));
    MOZ_RELEASE_ASSERT(seen.contains(STORM_PETREL));
    MOZ_RELEASE_ASSERT(seen.contains(CORMORANT));
  }

  void testAdd()
  {
    EnumSet<SeaBird> seen = mPetrels + CORMORANT + STORM_PETREL;
    MOZ_RELEASE_ASSERT(mPetrels.size() == 4);
    MOZ_RELEASE_ASSERT(!mPetrels.contains(CORMORANT));
    MOZ_RELEASE_ASSERT(seen.size() == 5);
    MOZ_RELEASE_ASSERT(seen.contains(GADFLY_PETREL));
    MOZ_RELEASE_ASSERT(seen.contains(TRUE_PETREL));
    MOZ_RELEASE_ASSERT(seen.contains(DIVING_PETREL));
    MOZ_RELEASE_ASSERT(seen.contains(STORM_PETREL));
    MOZ_RELEASE_ASSERT(seen.contains(CORMORANT));
  }

  void testAddAll()
  {
    EnumSet<SeaBird> procellariidae;
    procellariidae += mPetrelProcellariidae;
    procellariidae += mNonPetrelProcellariidae;
    MOZ_RELEASE_ASSERT(procellariidae.size() == 5);

    // Both procellariidae and mPetrels include GADFLY_PERTEL and TRUE_PETREL
    EnumSet<SeaBird> procellariiformes;
    procellariiformes += mDiomedeidae;
    procellariiformes += procellariidae;
    procellariiformes += mPetrels;
    MOZ_RELEASE_ASSERT(procellariiformes.size() == 8);
  }

  void testUnion()
  {
    EnumSet<SeaBird> procellariidae = mPetrelProcellariidae +
                                      mNonPetrelProcellariidae;
    MOZ_RELEASE_ASSERT(procellariidae.size() == 5);

    // Both procellariidae and mPetrels include GADFLY_PETREL and TRUE_PETREL
    EnumSet<SeaBird> procellariiformes = mDiomedeidae + procellariidae +
                                         mPetrels;
    MOZ_RELEASE_ASSERT(procellariiformes.size() == 8);
  }

  void testRemoveFrom()
  {
    EnumSet<SeaBird> likes = mPetrels;
    likes -= TRUE_PETREL;
    likes -= DIVING_PETREL;
    MOZ_RELEASE_ASSERT(likes.size() == 2);
    MOZ_RELEASE_ASSERT(likes.contains(GADFLY_PETREL));
    MOZ_RELEASE_ASSERT(likes.contains(STORM_PETREL));
  }

  void testRemove()
  {
    EnumSet<SeaBird> likes = mPetrels - TRUE_PETREL - DIVING_PETREL;
    MOZ_RELEASE_ASSERT(likes.size() == 2);
    MOZ_RELEASE_ASSERT(likes.contains(GADFLY_PETREL));
    MOZ_RELEASE_ASSERT(likes.contains(STORM_PETREL));
  }

  void testRemoveAllFrom()
  {
    EnumSet<SeaBird> likes = mPetrels;
    likes -= mPetrelProcellariidae;
    MOZ_RELEASE_ASSERT(likes.size() == 2);
    MOZ_RELEASE_ASSERT(likes.contains(DIVING_PETREL));
    MOZ_RELEASE_ASSERT(likes.contains(STORM_PETREL));
  }

  void testRemoveAll()
  {
    EnumSet<SeaBird> likes = mPetrels - mPetrelProcellariidae;
    MOZ_RELEASE_ASSERT(likes.size() == 2);
    MOZ_RELEASE_ASSERT(likes.contains(DIVING_PETREL));
    MOZ_RELEASE_ASSERT(likes.contains(STORM_PETREL));
  }

  void testIntersect()
  {
    EnumSet<SeaBird> likes = mPetrels;
    likes &= mPetrelProcellariidae;
    MOZ_RELEASE_ASSERT(likes.size() == 2);
    MOZ_RELEASE_ASSERT(likes.contains(GADFLY_PETREL));
    MOZ_RELEASE_ASSERT(likes.contains(TRUE_PETREL));
  }

  void testInsersection()
  {
    EnumSet<SeaBird> likes = mPetrels & mPetrelProcellariidae;
    MOZ_RELEASE_ASSERT(likes.size() == 2);
    MOZ_RELEASE_ASSERT(likes.contains(GADFLY_PETREL));
    MOZ_RELEASE_ASSERT(likes.contains(TRUE_PETREL));
  }

  void testEquality()
  {
    EnumSet<SeaBird> likes = mPetrels & mPetrelProcellariidae;
    MOZ_RELEASE_ASSERT(likes == EnumSet<SeaBird>(GADFLY_PETREL,
                                         TRUE_PETREL));
  }

  void testDuplicates()
  {
    EnumSet<SeaBird> likes = mPetrels;
    likes += GADFLY_PETREL;
    likes += TRUE_PETREL;
    likes += DIVING_PETREL;
    likes += STORM_PETREL;
    MOZ_RELEASE_ASSERT(likes.size() == 4);
    MOZ_RELEASE_ASSERT(likes == mPetrels);
  }

  EnumSet<SeaBird> mAlcidae;
  EnumSet<SeaBird> mDiomedeidae;
  EnumSet<SeaBird> mPetrelProcellariidae;
  EnumSet<SeaBird> mNonPetrelProcellariidae;
  EnumSet<SeaBird> mPetrels;
};

int
main()
{
  EnumSetSuite suite;
  suite.runTests();
  return 0;
}
