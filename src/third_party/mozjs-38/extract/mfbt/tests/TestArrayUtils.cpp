/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"

using mozilla::IsInRange;

static void
TestIsInRangeNonClass()
{
  void* nul = nullptr;
  int* intBegin = nullptr;
  int* intEnd = intBegin + 1;
  int* intEnd2 = intBegin + 2;

  MOZ_RELEASE_ASSERT(IsInRange(nul, intBegin, intEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, intEnd, intEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(intBegin, intBegin, intEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(intEnd, intBegin, intEnd));

  MOZ_RELEASE_ASSERT(IsInRange(intBegin, intBegin, intEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(intEnd, intBegin, intEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(intEnd2, intBegin, intEnd2));

  uintptr_t uintBegin = uintptr_t(intBegin);
  uintptr_t uintEnd = uintptr_t(intEnd);
  uintptr_t uintEnd2 = uintptr_t(intEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(nul, uintBegin, uintEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, uintEnd, uintEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(intBegin, uintBegin, uintEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(intEnd, uintBegin, uintEnd));

  MOZ_RELEASE_ASSERT(IsInRange(intBegin, uintBegin, uintEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(intEnd, uintBegin, uintEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(intEnd2, uintBegin, uintEnd2));
}

static void
TestIsInRangeVoid()
{
  int* intBegin = nullptr;
  int* intEnd = intBegin + 1;
  int* intEnd2 = intBegin + 2;

  void* voidBegin = intBegin;
  void* voidEnd = intEnd;
  void* voidEnd2 = intEnd2;

  MOZ_RELEASE_ASSERT(IsInRange(voidBegin, intBegin, intEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(voidEnd, intBegin, intEnd));

  MOZ_RELEASE_ASSERT(IsInRange(voidBegin, voidBegin, voidEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(voidEnd, voidBegin, voidEnd));

  MOZ_RELEASE_ASSERT(IsInRange(voidBegin, intBegin, intEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(voidEnd, intBegin, intEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(voidEnd2, intBegin, intEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(voidBegin, voidBegin, voidEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(voidEnd, voidBegin, voidEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(voidEnd2, voidBegin, voidEnd2));

  uintptr_t uintBegin = uintptr_t(intBegin);
  uintptr_t uintEnd = uintptr_t(intEnd);
  uintptr_t uintEnd2 = uintptr_t(intEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(voidBegin, uintBegin, uintEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(voidEnd, uintBegin, uintEnd));

  MOZ_RELEASE_ASSERT(IsInRange(voidBegin, uintBegin, uintEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(voidEnd, uintBegin, uintEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(voidEnd2, uintBegin, uintEnd2));
}

struct Base { int mX; };

static void
TestIsInRangeClass()
{
  void* nul = nullptr;
  Base* baseBegin = nullptr;
  Base* baseEnd = baseBegin + 1;
  Base* baseEnd2 = baseBegin + 2;

  MOZ_RELEASE_ASSERT(IsInRange(nul, baseBegin, baseEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, baseEnd, baseEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, baseBegin, baseEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd, baseBegin, baseEnd));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, baseBegin, baseEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(baseEnd, baseBegin, baseEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd2, baseBegin, baseEnd2));

  uintptr_t ubaseBegin = uintptr_t(baseBegin);
  uintptr_t ubaseEnd = uintptr_t(baseEnd);
  uintptr_t ubaseEnd2 = uintptr_t(baseEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(nul, ubaseBegin, ubaseEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, ubaseEnd, ubaseEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, ubaseBegin, ubaseEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd, ubaseBegin, ubaseEnd));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, ubaseBegin, ubaseEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(baseEnd, ubaseBegin, ubaseEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd2, ubaseBegin, ubaseEnd2));
}

struct EmptyBase {};

static void
TestIsInRangeEmptyClass()
{
  void* nul = nullptr;
  EmptyBase* baseBegin = nullptr;
  EmptyBase* baseEnd = baseBegin + 1;
  EmptyBase* baseEnd2 = baseBegin + 2;

  MOZ_RELEASE_ASSERT(IsInRange(nul, baseBegin, baseEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, baseEnd, baseEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, baseBegin, baseEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd, baseBegin, baseEnd));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, baseBegin, baseEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(baseEnd, baseBegin, baseEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd2, baseBegin, baseEnd2));

  uintptr_t ubaseBegin = uintptr_t(baseBegin);
  uintptr_t ubaseEnd = uintptr_t(baseEnd);
  uintptr_t ubaseEnd2 = uintptr_t(baseEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(nul, ubaseBegin, ubaseEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, ubaseEnd, ubaseEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, ubaseBegin, ubaseEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd, ubaseBegin, ubaseEnd));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, ubaseBegin, ubaseEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(baseEnd, ubaseBegin, ubaseEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd2, ubaseBegin, ubaseEnd2));
}

struct Derived : Base {};

static void
TestIsInRangeClassDerived()
{
  void* nul = nullptr;
  Derived* derivedBegin = nullptr;
  Derived* derivedEnd = derivedBegin + 1;
  Derived* derivedEnd2 = derivedBegin + 2;

  Base* baseBegin = static_cast<Base*>(derivedBegin);
  Base* baseEnd = static_cast<Base*>(derivedEnd);
  Base* baseEnd2 = static_cast<Base*>(derivedEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(nul, derivedBegin, derivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, derivedEnd, derivedEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, derivedBegin, derivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd, derivedBegin, derivedEnd));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, derivedBegin, derivedEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(baseEnd, derivedBegin, derivedEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd2, derivedBegin, derivedEnd2));

  uintptr_t uderivedBegin = uintptr_t(derivedBegin);
  uintptr_t uderivedEnd = uintptr_t(derivedEnd);
  uintptr_t uderivedEnd2 = uintptr_t(derivedEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(derivedBegin, uderivedBegin, uderivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(derivedEnd, uderivedBegin, uderivedEnd));

  MOZ_RELEASE_ASSERT(IsInRange(derivedBegin, uderivedBegin, uderivedEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(derivedEnd, uderivedBegin, uderivedEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(derivedEnd2, uderivedBegin, uderivedEnd2));
}

struct DerivedEmpty : EmptyBase {};

static void
TestIsInRangeClassDerivedEmpty()
{
  void* nul = nullptr;
  DerivedEmpty* derivedEmptyBegin = nullptr;
  DerivedEmpty* derivedEmptyEnd = derivedEmptyBegin + 1;
  DerivedEmpty* derivedEmptyEnd2 = derivedEmptyBegin + 2;

  EmptyBase* baseBegin = static_cast<EmptyBase*>(derivedEmptyBegin);
  EmptyBase* baseEnd = static_cast<EmptyBase*>(derivedEmptyEnd);
  EmptyBase* baseEnd2 = static_cast<EmptyBase*>(derivedEmptyEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(nul, derivedEmptyBegin, derivedEmptyEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, derivedEmptyEnd, derivedEmptyEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, derivedEmptyBegin, derivedEmptyEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd, derivedEmptyBegin, derivedEmptyEnd));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, derivedEmptyBegin, derivedEmptyEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(baseEnd, derivedEmptyBegin, derivedEmptyEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd2, derivedEmptyBegin, derivedEmptyEnd2));

  uintptr_t uderivedEmptyBegin = uintptr_t(derivedEmptyBegin);
  uintptr_t uderivedEmptyEnd = uintptr_t(derivedEmptyEnd);
  uintptr_t uderivedEmptyEnd2 = uintptr_t(derivedEmptyEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(derivedEmptyBegin, uderivedEmptyBegin,
                               uderivedEmptyEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(derivedEmptyEnd, uderivedEmptyBegin,
                                uderivedEmptyEnd));

  MOZ_RELEASE_ASSERT(IsInRange(derivedEmptyBegin, uderivedEmptyBegin,
                               uderivedEmptyEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(derivedEmptyEnd, uderivedEmptyBegin,
                               uderivedEmptyEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(derivedEmptyEnd2, uderivedEmptyBegin,
                                uderivedEmptyEnd2));
}

struct ExtraDerived : Base { int y; };

static void
TestIsInRangeClassExtraDerived()
{
  void* nul = nullptr;
  ExtraDerived* derivedBegin = nullptr;
  ExtraDerived* derivedEnd = derivedBegin + 1;
  ExtraDerived* derivedEnd2 = derivedBegin + 2;

  Base* baseBegin = static_cast<Base*>(derivedBegin);
  Base* baseEnd = static_cast<Base*>(derivedEnd);
  Base* baseEnd2 = static_cast<Base*>(derivedEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(nul, derivedBegin, derivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, derivedEnd, derivedEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, derivedBegin, derivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd, derivedBegin, derivedEnd));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, derivedBegin, derivedEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(baseEnd, derivedBegin, derivedEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd2, derivedBegin, derivedEnd2));

  uintptr_t uderivedBegin = uintptr_t(derivedBegin);
  uintptr_t uderivedEnd = uintptr_t(derivedEnd);
  uintptr_t uderivedEnd2 = uintptr_t(derivedEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(derivedBegin, uderivedBegin, uderivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(derivedEnd, uderivedBegin, uderivedEnd));

  MOZ_RELEASE_ASSERT(IsInRange(derivedBegin, uderivedBegin, uderivedEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(derivedEnd, uderivedBegin, uderivedEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(derivedEnd2, uderivedBegin, uderivedEnd2));
}

struct ExtraDerivedEmpty : EmptyBase { int y; };

static void
TestIsInRangeClassExtraDerivedEmpty()
{
  void* nul = nullptr;
  ExtraDerivedEmpty* derivedBegin = nullptr;
  ExtraDerivedEmpty* derivedEnd = derivedBegin + 1;
  ExtraDerivedEmpty* derivedEnd2 = derivedBegin + 2;

  EmptyBase* baseBegin = static_cast<EmptyBase*>(derivedBegin);
  EmptyBase* baseEnd = static_cast<EmptyBase*>(derivedEnd);
  EmptyBase* baseEnd2 = static_cast<EmptyBase*>(derivedEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(nul, derivedBegin, derivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(nul, derivedEnd, derivedEnd2));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, derivedBegin, derivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd, derivedBegin, derivedEnd));

  MOZ_RELEASE_ASSERT(IsInRange(baseBegin, derivedBegin, derivedEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(baseEnd, derivedBegin, derivedEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(baseEnd2, derivedBegin, derivedEnd2));

  uintptr_t uderivedBegin = uintptr_t(derivedBegin);
  uintptr_t uderivedEnd = uintptr_t(derivedEnd);
  uintptr_t uderivedEnd2 = uintptr_t(derivedEnd2);

  MOZ_RELEASE_ASSERT(IsInRange(derivedBegin, uderivedBegin, uderivedEnd));
  MOZ_RELEASE_ASSERT(!IsInRange(derivedEnd, uderivedBegin, uderivedEnd));

  MOZ_RELEASE_ASSERT(IsInRange(derivedBegin, uderivedBegin, uderivedEnd2));
  MOZ_RELEASE_ASSERT(IsInRange(derivedEnd, uderivedBegin, uderivedEnd2));
  MOZ_RELEASE_ASSERT(!IsInRange(derivedEnd2, uderivedBegin, uderivedEnd2));
}

int
main()
{
  TestIsInRangeNonClass();
  TestIsInRangeVoid();
  TestIsInRangeClass();
  TestIsInRangeEmptyClass();
  TestIsInRangeClassDerived();
  TestIsInRangeClassDerivedEmpty();
  TestIsInRangeClassExtraDerived();
  TestIsInRangeClassExtraDerivedEmpty();
  return 0;
}
