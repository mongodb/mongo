/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Move.h"
#include "mozilla/Vector.h"

using mozilla::detail::VectorTesting;
using mozilla::Move;
using mozilla::Vector;

struct mozilla::detail::VectorTesting
{
  static void testReserved();
};

void
mozilla::detail::VectorTesting::testReserved()
{
#ifdef DEBUG
  Vector<bool> bv;
  MOZ_RELEASE_ASSERT(bv.reserved() == 0);

  MOZ_RELEASE_ASSERT(bv.append(true));
  MOZ_RELEASE_ASSERT(bv.reserved() == 1);

  Vector<bool> otherbv;
  MOZ_RELEASE_ASSERT(otherbv.append(false));
  MOZ_RELEASE_ASSERT(otherbv.append(true));
  MOZ_RELEASE_ASSERT(bv.appendAll(otherbv));
  MOZ_RELEASE_ASSERT(bv.reserved() == 3);

  MOZ_RELEASE_ASSERT(bv.reserve(5));
  MOZ_RELEASE_ASSERT(bv.reserved() == 5);

  MOZ_RELEASE_ASSERT(bv.reserve(1));
  MOZ_RELEASE_ASSERT(bv.reserved() == 5);

  Vector<bool> bv2(Move(bv));
  MOZ_RELEASE_ASSERT(bv.reserved() == 0);
  MOZ_RELEASE_ASSERT(bv2.reserved() == 5);

  bv2.clearAndFree();
  MOZ_RELEASE_ASSERT(bv2.reserved() == 0);

  Vector<int, 42> iv;
  MOZ_RELEASE_ASSERT(iv.reserved() == 0);

  MOZ_RELEASE_ASSERT(iv.append(17));
  MOZ_RELEASE_ASSERT(iv.reserved() == 1);

  Vector<int, 42> otheriv;
  MOZ_RELEASE_ASSERT(otheriv.append(42));
  MOZ_RELEASE_ASSERT(otheriv.append(37));
  MOZ_RELEASE_ASSERT(iv.appendAll(otheriv));
  MOZ_RELEASE_ASSERT(iv.reserved() == 3);

  MOZ_RELEASE_ASSERT(iv.reserve(5));
  MOZ_RELEASE_ASSERT(iv.reserved() == 5);

  MOZ_RELEASE_ASSERT(iv.reserve(1));
  MOZ_RELEASE_ASSERT(iv.reserved() == 5);

  MOZ_RELEASE_ASSERT(iv.reserve(55));
  MOZ_RELEASE_ASSERT(iv.reserved() == 55);

  Vector<int, 42> iv2(Move(iv));
  MOZ_RELEASE_ASSERT(iv.reserved() == 0);
  MOZ_RELEASE_ASSERT(iv2.reserved() == 55);

  iv2.clearAndFree();
  MOZ_RELEASE_ASSERT(iv2.reserved() == 0);
#endif
}

int
main()
{
  VectorTesting::testReserved();
}
