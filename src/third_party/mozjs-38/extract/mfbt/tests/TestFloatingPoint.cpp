/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/FloatingPoint.h"

#include <math.h>

using mozilla::ExponentComponent;
using mozilla::FloatingPoint;
using mozilla::FuzzyEqualsAdditive;
using mozilla::FuzzyEqualsMultiplicative;
using mozilla::IsFinite;
using mozilla::IsInfinite;
using mozilla::IsNaN;
using mozilla::IsNegative;
using mozilla::IsNegativeZero;
using mozilla::NegativeInfinity;
using mozilla::NumberEqualsInt32;
using mozilla::NumberIsInt32;
using mozilla::NumbersAreIdentical;
using mozilla::PositiveInfinity;
using mozilla::SpecificNaN;
using mozilla::UnspecifiedNaN;

#define A(a) MOZ_RELEASE_ASSERT(a)

template<typename T>
static void
ShouldBeIdentical(T aD1, T aD2)
{
  A(NumbersAreIdentical(aD1, aD2));
  A(NumbersAreIdentical(aD2, aD1));
}

template<typename T>
static void
ShouldNotBeIdentical(T aD1, T aD2)
{
  A(!NumbersAreIdentical(aD1, aD2));
  A(!NumbersAreIdentical(aD2, aD1));
}

static void
TestDoublesAreIdentical()
{
  ShouldBeIdentical(+0.0, +0.0);
  ShouldBeIdentical(-0.0, -0.0);
  ShouldNotBeIdentical(+0.0, -0.0);

  ShouldBeIdentical(1.0, 1.0);
  ShouldNotBeIdentical(-1.0, 1.0);
  ShouldBeIdentical(4294967295.0, 4294967295.0);
  ShouldNotBeIdentical(-4294967295.0, 4294967295.0);
  ShouldBeIdentical(4294967296.0, 4294967296.0);
  ShouldBeIdentical(4294967297.0, 4294967297.0);
  ShouldBeIdentical(1e300, 1e300);

  ShouldBeIdentical(PositiveInfinity<double>(), PositiveInfinity<double>());
  ShouldBeIdentical(NegativeInfinity<double>(), NegativeInfinity<double>());
  ShouldNotBeIdentical(PositiveInfinity<double>(), NegativeInfinity<double>());

  ShouldNotBeIdentical(-0.0, NegativeInfinity<double>());
  ShouldNotBeIdentical(+0.0, NegativeInfinity<double>());
  ShouldNotBeIdentical(1e300, NegativeInfinity<double>());
  ShouldNotBeIdentical(3.141592654, NegativeInfinity<double>());

  ShouldBeIdentical(UnspecifiedNaN<double>(), UnspecifiedNaN<double>());
  ShouldBeIdentical(-UnspecifiedNaN<double>(), UnspecifiedNaN<double>());
  ShouldBeIdentical(UnspecifiedNaN<double>(), -UnspecifiedNaN<double>());

  ShouldBeIdentical(SpecificNaN<double>(0, 17), SpecificNaN<double>(0, 42));
  ShouldBeIdentical(SpecificNaN<double>(1, 17), SpecificNaN<double>(1, 42));
  ShouldBeIdentical(SpecificNaN<double>(0, 17), SpecificNaN<double>(1, 42));
  ShouldBeIdentical(SpecificNaN<double>(1, 17), SpecificNaN<double>(0, 42));

  const uint64_t Mask = 0xfffffffffffffULL;
  for (unsigned i = 0; i < 52; i++) {
    for (unsigned j = 0; j < 52; j++) {
      for (unsigned sign = 0; i < 2; i++) {
        ShouldBeIdentical(SpecificNaN<double>(0, 1ULL << i),
                          SpecificNaN<double>(sign, 1ULL << j));
        ShouldBeIdentical(SpecificNaN<double>(1, 1ULL << i),
                          SpecificNaN<double>(sign, 1ULL << j));

        ShouldBeIdentical(SpecificNaN<double>(0, Mask & ~(1ULL << i)),
                          SpecificNaN<double>(sign, Mask & ~(1ULL << j)));
        ShouldBeIdentical(SpecificNaN<double>(1, Mask & ~(1ULL << i)),
                          SpecificNaN<double>(sign, Mask & ~(1ULL << j)));
      }
    }
  }
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x8000000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x4000000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x2000000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x1000000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x0800000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x0400000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x0200000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x0100000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x0080000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x0040000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x0020000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(0, 17),
                    SpecificNaN<double>(0, 0x0010000000000ULL));
  ShouldBeIdentical(SpecificNaN<double>(1, 17),
                    SpecificNaN<double>(0, 0xff0ffffffffffULL));
  ShouldBeIdentical(SpecificNaN<double>(1, 17),
                    SpecificNaN<double>(0, 0xfffffffffff0fULL));

  ShouldNotBeIdentical(UnspecifiedNaN<double>(), +0.0);
  ShouldNotBeIdentical(UnspecifiedNaN<double>(), -0.0);
  ShouldNotBeIdentical(UnspecifiedNaN<double>(), 1.0);
  ShouldNotBeIdentical(UnspecifiedNaN<double>(), -1.0);
  ShouldNotBeIdentical(UnspecifiedNaN<double>(), PositiveInfinity<double>());
  ShouldNotBeIdentical(UnspecifiedNaN<double>(), NegativeInfinity<double>());
}

static void
TestFloatsAreIdentical()
{
  ShouldBeIdentical(+0.0f, +0.0f);
  ShouldBeIdentical(-0.0f, -0.0f);
  ShouldNotBeIdentical(+0.0f, -0.0f);

  ShouldBeIdentical(1.0f, 1.0f);
  ShouldNotBeIdentical(-1.0f, 1.0f);
  ShouldBeIdentical(8388607.0f, 8388607.0f);
  ShouldNotBeIdentical(-8388607.0f, 8388607.0f);
  ShouldBeIdentical(8388608.0f, 8388608.0f);
  ShouldBeIdentical(8388609.0f, 8388609.0f);
  ShouldBeIdentical(1e36f, 1e36f);

  ShouldBeIdentical(PositiveInfinity<float>(), PositiveInfinity<float>());
  ShouldBeIdentical(NegativeInfinity<float>(), NegativeInfinity<float>());
  ShouldNotBeIdentical(PositiveInfinity<float>(), NegativeInfinity<float>());

  ShouldNotBeIdentical(-0.0f, NegativeInfinity<float>());
  ShouldNotBeIdentical(+0.0f, NegativeInfinity<float>());
  ShouldNotBeIdentical(1e36f, NegativeInfinity<float>());
  ShouldNotBeIdentical(3.141592654f, NegativeInfinity<float>());

  ShouldBeIdentical(UnspecifiedNaN<float>(), UnspecifiedNaN<float>());
  ShouldBeIdentical(-UnspecifiedNaN<float>(), UnspecifiedNaN<float>());
  ShouldBeIdentical(UnspecifiedNaN<float>(), -UnspecifiedNaN<float>());

  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 42));
  ShouldBeIdentical(SpecificNaN<float>(1, 17), SpecificNaN<float>(1, 42));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(1, 42));
  ShouldBeIdentical(SpecificNaN<float>(1, 17), SpecificNaN<float>(0, 42));

  const uint32_t Mask = 0x7fffffUL;
  for (unsigned i = 0; i < 23; i++) {
    for (unsigned j = 0; j < 23; j++) {
      for (unsigned sign = 0; i < 2; i++) {
        ShouldBeIdentical(SpecificNaN<float>(0, 1UL << i),
                          SpecificNaN<float>(sign, 1UL << j));
        ShouldBeIdentical(SpecificNaN<float>(1, 1UL << i),
                          SpecificNaN<float>(sign, 1UL << j));

        ShouldBeIdentical(SpecificNaN<float>(0, Mask & ~(1UL << i)),
                          SpecificNaN<float>(sign, Mask & ~(1UL << j)));
        ShouldBeIdentical(SpecificNaN<float>(1, Mask & ~(1UL << i)),
                          SpecificNaN<float>(sign, Mask & ~(1UL << j)));
      }
    }
  }
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x700000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x400000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x200000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x100000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x080000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x040000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x020000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x010000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x008000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x004000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x002000));
  ShouldBeIdentical(SpecificNaN<float>(0, 17), SpecificNaN<float>(0, 0x001000));
  ShouldBeIdentical(SpecificNaN<float>(1, 17), SpecificNaN<float>(0, 0x7f0fff));
  ShouldBeIdentical(SpecificNaN<float>(1, 17), SpecificNaN<float>(0, 0x7fff0f));

  ShouldNotBeIdentical(UnspecifiedNaN<float>(), +0.0f);
  ShouldNotBeIdentical(UnspecifiedNaN<float>(), -0.0f);
  ShouldNotBeIdentical(UnspecifiedNaN<float>(), 1.0f);
  ShouldNotBeIdentical(UnspecifiedNaN<float>(), -1.0f);
  ShouldNotBeIdentical(UnspecifiedNaN<float>(), PositiveInfinity<float>());
  ShouldNotBeIdentical(UnspecifiedNaN<float>(), NegativeInfinity<float>());
}

static void
TestAreIdentical()
{
  TestDoublesAreIdentical();
  TestFloatsAreIdentical();
}

static void
TestDoubleExponentComponent()
{
  A(ExponentComponent(0.0) ==
    -int_fast16_t(FloatingPoint<double>::kExponentBias));
  A(ExponentComponent(-0.0) ==
    -int_fast16_t(FloatingPoint<double>::kExponentBias));
  A(ExponentComponent(0.125) == -3);
  A(ExponentComponent(0.5) == -1);
  A(ExponentComponent(1.0) == 0);
  A(ExponentComponent(1.5) == 0);
  A(ExponentComponent(2.0) == 1);
  A(ExponentComponent(7.0) == 2);
  A(ExponentComponent(PositiveInfinity<double>()) ==
    FloatingPoint<double>::kExponentBias + 1);
  A(ExponentComponent(NegativeInfinity<double>()) ==
    FloatingPoint<double>::kExponentBias + 1);
  A(ExponentComponent(UnspecifiedNaN<double>()) ==
    FloatingPoint<double>::kExponentBias + 1);
}

static void
TestFloatExponentComponent()
{
  A(ExponentComponent(0.0f) ==
    -int_fast16_t(FloatingPoint<float>::kExponentBias));
  A(ExponentComponent(-0.0f) ==
    -int_fast16_t(FloatingPoint<float>::kExponentBias));
  A(ExponentComponent(0.125f) == -3);
  A(ExponentComponent(0.5f) == -1);
  A(ExponentComponent(1.0f) == 0);
  A(ExponentComponent(1.5f) == 0);
  A(ExponentComponent(2.0f) == 1);
  A(ExponentComponent(7.0f) == 2);
  A(ExponentComponent(PositiveInfinity<float>()) ==
    FloatingPoint<float>::kExponentBias + 1);
  A(ExponentComponent(NegativeInfinity<float>()) ==
    FloatingPoint<float>::kExponentBias + 1);
  A(ExponentComponent(UnspecifiedNaN<float>()) ==
    FloatingPoint<float>::kExponentBias + 1);
}

static void
TestExponentComponent()
{
  TestDoubleExponentComponent();
  TestFloatExponentComponent();
}

static void
TestDoublesPredicates()
{
  A(IsNaN(UnspecifiedNaN<double>()));
  A(IsNaN(SpecificNaN<double>(1, 17)));;
  A(IsNaN(SpecificNaN<double>(0, 0xfffffffffff0fULL)));
  A(!IsNaN(0.0));
  A(!IsNaN(-0.0));
  A(!IsNaN(1.0));
  A(!IsNaN(PositiveInfinity<double>()));
  A(!IsNaN(NegativeInfinity<double>()));

  A(IsInfinite(PositiveInfinity<double>()));
  A(IsInfinite(NegativeInfinity<double>()));
  A(!IsInfinite(UnspecifiedNaN<double>()));
  A(!IsInfinite(0.0));
  A(!IsInfinite(-0.0));
  A(!IsInfinite(1.0));

  A(!IsFinite(PositiveInfinity<double>()));
  A(!IsFinite(NegativeInfinity<double>()));
  A(!IsFinite(UnspecifiedNaN<double>()));
  A(IsFinite(0.0));
  A(IsFinite(-0.0));
  A(IsFinite(1.0));

  A(!IsNegative(PositiveInfinity<double>()));
  A(IsNegative(NegativeInfinity<double>()));
  A(IsNegative(-0.0));
  A(!IsNegative(0.0));
  A(IsNegative(-1.0));
  A(!IsNegative(1.0));

  A(!IsNegativeZero(PositiveInfinity<double>()));
  A(!IsNegativeZero(NegativeInfinity<double>()));
  A(!IsNegativeZero(SpecificNaN<double>(1, 17)));;
  A(!IsNegativeZero(SpecificNaN<double>(1, 0xfffffffffff0fULL)));
  A(!IsNegativeZero(SpecificNaN<double>(0, 17)));;
  A(!IsNegativeZero(SpecificNaN<double>(0, 0xfffffffffff0fULL)));
  A(!IsNegativeZero(UnspecifiedNaN<double>()));
  A(IsNegativeZero(-0.0));
  A(!IsNegativeZero(0.0));
  A(!IsNegativeZero(-1.0));
  A(!IsNegativeZero(1.0));

  int32_t i;
  A(NumberIsInt32(0.0, &i));
  A(i == 0);
  A(!NumberIsInt32(-0.0, &i));
  A(NumberEqualsInt32(0.0, &i));
  A(i == 0);
  A(NumberEqualsInt32(-0.0, &i));
  A(i == 0);
  A(NumberIsInt32(double(INT32_MIN), &i));
  A(i == INT32_MIN);
  A(NumberIsInt32(double(INT32_MAX), &i));
  A(i == INT32_MAX);
  A(NumberEqualsInt32(double(INT32_MIN), &i));
  A(i == INT32_MIN);
  A(NumberEqualsInt32(double(INT32_MAX), &i));
  A(i == INT32_MAX);
  A(!NumberIsInt32(0.5, &i));
  A(!NumberIsInt32(double(INT32_MAX) + 0.1, &i));
  A(!NumberIsInt32(double(INT32_MIN) - 0.1, &i));
  A(!NumberIsInt32(NegativeInfinity<double>(), &i));
  A(!NumberIsInt32(PositiveInfinity<double>(), &i));
  A(!NumberIsInt32(UnspecifiedNaN<double>(), &i));
  A(!NumberEqualsInt32(0.5, &i));
  A(!NumberEqualsInt32(double(INT32_MAX) + 0.1, &i));
  A(!NumberEqualsInt32(double(INT32_MIN) - 0.1, &i));
  A(!NumberEqualsInt32(NegativeInfinity<double>(), &i));
  A(!NumberEqualsInt32(PositiveInfinity<double>(), &i));
  A(!NumberEqualsInt32(UnspecifiedNaN<double>(), &i));
}

static void
TestFloatsPredicates()
{
  A(IsNaN(UnspecifiedNaN<float>()));
  A(IsNaN(SpecificNaN<float>(1, 17)));;
  A(IsNaN(SpecificNaN<float>(0, 0x7fff0fUL)));
  A(!IsNaN(0.0f));
  A(!IsNaN(-0.0f));
  A(!IsNaN(1.0f));
  A(!IsNaN(PositiveInfinity<float>()));
  A(!IsNaN(NegativeInfinity<float>()));

  A(IsInfinite(PositiveInfinity<float>()));
  A(IsInfinite(NegativeInfinity<float>()));
  A(!IsInfinite(UnspecifiedNaN<float>()));
  A(!IsInfinite(0.0f));
  A(!IsInfinite(-0.0f));
  A(!IsInfinite(1.0f));

  A(!IsFinite(PositiveInfinity<float>()));
  A(!IsFinite(NegativeInfinity<float>()));
  A(!IsFinite(UnspecifiedNaN<float>()));
  A(IsFinite(0.0f));
  A(IsFinite(-0.0f));
  A(IsFinite(1.0f));

  A(!IsNegative(PositiveInfinity<float>()));
  A(IsNegative(NegativeInfinity<float>()));
  A(IsNegative(-0.0f));
  A(!IsNegative(0.0f));
  A(IsNegative(-1.0f));
  A(!IsNegative(1.0f));

  A(!IsNegativeZero(PositiveInfinity<float>()));
  A(!IsNegativeZero(NegativeInfinity<float>()));
  A(!IsNegativeZero(SpecificNaN<float>(1, 17)));;
  A(!IsNegativeZero(SpecificNaN<float>(1, 0x7fff0fUL)));
  A(!IsNegativeZero(SpecificNaN<float>(0, 17)));;
  A(!IsNegativeZero(SpecificNaN<float>(0, 0x7fff0fUL)));
  A(!IsNegativeZero(UnspecifiedNaN<float>()));
  A(IsNegativeZero(-0.0f));
  A(!IsNegativeZero(0.0f));
  A(!IsNegativeZero(-1.0f));
  A(!IsNegativeZero(1.0f));

  int32_t i;
  const int32_t BIG = 2097151;
  A(NumberIsInt32(0.0f, &i));
  A(i == 0);
  A(!NumberIsInt32(-0.0f, &i));
  A(NumberEqualsInt32(0.0f, &i));
  A(i == 0);
  A(NumberEqualsInt32(-0.0f, &i));
  A(i == 0);
  A(NumberIsInt32(float(INT32_MIN), &i));
  A(i == INT32_MIN);
  A(NumberIsInt32(float(BIG), &i));
  A(i == BIG);
  A(NumberEqualsInt32(float(INT32_MIN), &i));
  A(i == INT32_MIN);
  A(NumberEqualsInt32(float(BIG), &i));
  A(i == BIG);
  A(!NumberIsInt32(0.5f, &i));
  A(!NumberIsInt32(float(BIG) + 0.1f, &i));
  A(!NumberIsInt32(NegativeInfinity<float>(), &i));
  A(!NumberIsInt32(PositiveInfinity<float>(), &i));
  A(!NumberIsInt32(UnspecifiedNaN<float>(), &i));
  A(!NumberEqualsInt32(0.5f, &i));
  A(!NumberEqualsInt32(float(BIG) + 0.1f, &i));
  A(!NumberEqualsInt32(NegativeInfinity<float>(), &i));
  A(!NumberEqualsInt32(PositiveInfinity<float>(), &i));
  A(!NumberEqualsInt32(UnspecifiedNaN<float>(), &i));
}

static void
TestPredicates()
{
  TestFloatsPredicates();
  TestDoublesPredicates();
}

static void
TestFloatsAreApproximatelyEqual()
{
  float epsilon = mozilla::detail::FuzzyEqualsEpsilon<float>::value();
  float lessThanEpsilon = epsilon / 2.0f;
  float moreThanEpsilon = epsilon * 2.0f;

  // Additive tests using the default epsilon
  // ... around 1.0
  A(FuzzyEqualsAdditive(1.0f, 1.0f + lessThanEpsilon));
  A(FuzzyEqualsAdditive(1.0f, 1.0f - lessThanEpsilon));
  A(FuzzyEqualsAdditive(1.0f, 1.0f + epsilon));
  A(FuzzyEqualsAdditive(1.0f, 1.0f - epsilon));
  A(!FuzzyEqualsAdditive(1.0f, 1.0f + moreThanEpsilon));
  A(!FuzzyEqualsAdditive(1.0f, 1.0f - moreThanEpsilon));
  // ... around 1.0e2 (this is near the upper bound of the range where
  // adding moreThanEpsilon will still be representable and return false)
  A(FuzzyEqualsAdditive(1.0e2f, 1.0e2f + lessThanEpsilon));
  A(FuzzyEqualsAdditive(1.0e2f, 1.0e2f + epsilon));
  A(!FuzzyEqualsAdditive(1.0e2f, 1.0e2f + moreThanEpsilon));
  // ... around 1.0e-10
  A(FuzzyEqualsAdditive(1.0e-10f, 1.0e-10f + lessThanEpsilon));
  A(FuzzyEqualsAdditive(1.0e-10f, 1.0e-10f + epsilon));
  A(!FuzzyEqualsAdditive(1.0e-10f, 1.0e-10f + moreThanEpsilon));
  // ... straddling 0
  A(FuzzyEqualsAdditive(1.0e-6f, -1.0e-6f));
  A(!FuzzyEqualsAdditive(1.0e-5f, -1.0e-5f));
  // Using a small epsilon
  A(FuzzyEqualsAdditive(1.0e-5f, 1.0e-5f + 1.0e-10f, 1.0e-9f));
  A(!FuzzyEqualsAdditive(1.0e-5f, 1.0e-5f + 1.0e-10f, 1.0e-11f));
  // Using a big epsilon
  A(FuzzyEqualsAdditive(1.0e20f, 1.0e20f + 1.0e15f, 1.0e16f));
  A(!FuzzyEqualsAdditive(1.0e20f, 1.0e20f + 1.0e15f, 1.0e14f));

  // Multiplicative tests using the default epsilon
  // ... around 1.0
  A(FuzzyEqualsMultiplicative(1.0f, 1.0f + lessThanEpsilon));
  A(FuzzyEqualsMultiplicative(1.0f, 1.0f - lessThanEpsilon));
  A(FuzzyEqualsMultiplicative(1.0f, 1.0f + epsilon));
  A(!FuzzyEqualsMultiplicative(1.0f, 1.0f - epsilon));
  A(!FuzzyEqualsMultiplicative(1.0f, 1.0f + moreThanEpsilon));
  A(!FuzzyEqualsMultiplicative(1.0f, 1.0f - moreThanEpsilon));
  // ... around 1.0e10
  A(FuzzyEqualsMultiplicative(1.0e10f, 1.0e10f + (lessThanEpsilon * 1.0e10f)));
  A(!FuzzyEqualsMultiplicative(1.0e10f, 1.0e10f + (moreThanEpsilon * 1.0e10f)));
  // ... around 1.0e-10
  A(FuzzyEqualsMultiplicative(1.0e-10f,
                              1.0e-10f + (lessThanEpsilon * 1.0e-10f)));
  A(!FuzzyEqualsMultiplicative(1.0e-10f,
                               1.0e-10f + (moreThanEpsilon * 1.0e-10f)));
  // ... straddling 0
  A(!FuzzyEqualsMultiplicative(1.0e-6f, -1.0e-6f));
  A(FuzzyEqualsMultiplicative(1.0e-6f, -1.0e-6f, 1.0e2f));
  // Using a small epsilon
  A(FuzzyEqualsMultiplicative(1.0e-5f, 1.0e-5f + 1.0e-10f, 1.0e-4f));
  A(!FuzzyEqualsMultiplicative(1.0e-5f, 1.0e-5f + 1.0e-10f, 1.0e-5f));
  // Using a big epsilon
  A(FuzzyEqualsMultiplicative(1.0f, 2.0f, 1.0f));
  A(!FuzzyEqualsMultiplicative(1.0f, 2.0f, 0.1f));

  // "real world case"
  float oneThird = 10.0f / 3.0f;
  A(FuzzyEqualsAdditive(10.0f, 3.0f * oneThird));
  A(FuzzyEqualsMultiplicative(10.0f, 3.0f * oneThird));
  // NaN check
  A(!FuzzyEqualsAdditive(SpecificNaN<float>(1, 1), SpecificNaN<float>(1, 1)));
  A(!FuzzyEqualsAdditive(SpecificNaN<float>(1, 2), SpecificNaN<float>(0, 8)));
  A(!FuzzyEqualsMultiplicative(SpecificNaN<float>(1, 1),
                               SpecificNaN<float>(1, 1)));
  A(!FuzzyEqualsMultiplicative(SpecificNaN<float>(1, 2),
                               SpecificNaN<float>(0, 200)));
}

static void
TestDoublesAreApproximatelyEqual()
{
  double epsilon = mozilla::detail::FuzzyEqualsEpsilon<double>::value();
  double lessThanEpsilon = epsilon / 2.0;
  double moreThanEpsilon = epsilon * 2.0;

  // Additive tests using the default epsilon
  // ... around 1.0
  A(FuzzyEqualsAdditive(1.0, 1.0 + lessThanEpsilon));
  A(FuzzyEqualsAdditive(1.0, 1.0 - lessThanEpsilon));
  A(FuzzyEqualsAdditive(1.0, 1.0 + epsilon));
  A(FuzzyEqualsAdditive(1.0, 1.0 - epsilon));
  A(!FuzzyEqualsAdditive(1.0, 1.0 + moreThanEpsilon));
  A(!FuzzyEqualsAdditive(1.0, 1.0 - moreThanEpsilon));
  // ... around 1.0e4 (this is near the upper bound of the range where
  // adding moreThanEpsilon will still be representable and return false)
  A(FuzzyEqualsAdditive(1.0e4, 1.0e4 + lessThanEpsilon));
  A(FuzzyEqualsAdditive(1.0e4, 1.0e4 + epsilon));
  A(!FuzzyEqualsAdditive(1.0e4, 1.0e4 + moreThanEpsilon));
  // ... around 1.0e-25
  A(FuzzyEqualsAdditive(1.0e-25, 1.0e-25 + lessThanEpsilon));
  A(FuzzyEqualsAdditive(1.0e-25, 1.0e-25 + epsilon));
  A(!FuzzyEqualsAdditive(1.0e-25, 1.0e-25 + moreThanEpsilon));
  // ... straddling 0
  A(FuzzyEqualsAdditive(1.0e-13, -1.0e-13));
  A(!FuzzyEqualsAdditive(1.0e-12, -1.0e-12));
  // Using a small epsilon
  A(FuzzyEqualsAdditive(1.0e-15, 1.0e-15 + 1.0e-30, 1.0e-29));
  A(!FuzzyEqualsAdditive(1.0e-15, 1.0e-15 + 1.0e-30, 1.0e-31));
  // Using a big epsilon
  A(FuzzyEqualsAdditive(1.0e40, 1.0e40 + 1.0e25, 1.0e26));
  A(!FuzzyEqualsAdditive(1.0e40, 1.0e40 + 1.0e25, 1.0e24));

  // Multiplicative tests using the default epsilon
  // ... around 1.0
  A(FuzzyEqualsMultiplicative(1.0, 1.0 + lessThanEpsilon));
  A(FuzzyEqualsMultiplicative(1.0, 1.0 - lessThanEpsilon));
  A(FuzzyEqualsMultiplicative(1.0, 1.0 + epsilon));
  A(!FuzzyEqualsMultiplicative(1.0, 1.0 - epsilon));
  A(!FuzzyEqualsMultiplicative(1.0, 1.0 + moreThanEpsilon));
  A(!FuzzyEqualsMultiplicative(1.0, 1.0 - moreThanEpsilon));
  // ... around 1.0e30
  A(FuzzyEqualsMultiplicative(1.0e30, 1.0e30 + (lessThanEpsilon * 1.0e30)));
  A(!FuzzyEqualsMultiplicative(1.0e30, 1.0e30 + (moreThanEpsilon * 1.0e30)));
  // ... around 1.0e-30
  A(FuzzyEqualsMultiplicative(1.0e-30, 1.0e-30 + (lessThanEpsilon * 1.0e-30)));
  A(!FuzzyEqualsMultiplicative(1.0e-30, 1.0e-30 + (moreThanEpsilon * 1.0e-30)));
  // ... straddling 0
  A(!FuzzyEqualsMultiplicative(1.0e-6, -1.0e-6));
  A(FuzzyEqualsMultiplicative(1.0e-6, -1.0e-6, 1.0e2));
  // Using a small epsilon
  A(FuzzyEqualsMultiplicative(1.0e-15, 1.0e-15 + 1.0e-30, 1.0e-15));
  A(!FuzzyEqualsMultiplicative(1.0e-15, 1.0e-15 + 1.0e-30, 1.0e-16));
  // Using a big epsilon
  A(FuzzyEqualsMultiplicative(1.0e40, 2.0e40, 1.0));
  A(!FuzzyEqualsMultiplicative(1.0e40, 2.0e40, 0.1));

  // "real world case"
  double oneThird = 10.0 / 3.0;
  A(FuzzyEqualsAdditive(10.0, 3.0 * oneThird));
  A(FuzzyEqualsMultiplicative(10.0, 3.0 * oneThird));
  // NaN check
  A(!FuzzyEqualsAdditive(SpecificNaN<double>(1, 1),
                         SpecificNaN<double>(1, 1)));
  A(!FuzzyEqualsAdditive(SpecificNaN<double>(1, 2),
                         SpecificNaN<double>(0, 8)));
  A(!FuzzyEqualsMultiplicative(SpecificNaN<double>(1, 1),
                               SpecificNaN<double>(1, 1)));
  A(!FuzzyEqualsMultiplicative(SpecificNaN<double>(1, 2),
                               SpecificNaN<double>(0, 200)));
}

static void
TestAreApproximatelyEqual()
{
  TestFloatsAreApproximatelyEqual();
  TestDoublesAreApproximatelyEqual();
}

#undef A

int
main()
{
  TestAreIdentical();
  TestExponentComponent();
  TestPredicates();
  TestAreApproximatelyEqual();
  return 0;
}
