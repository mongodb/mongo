/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Reusable template meta-functions on types and compile-time values.  Meta-
 * functions are placed inside the 'tl' namespace to avoid conflict with non-
 * meta functions of the same name (e.g., mozilla::tl::FloorLog2 vs.
 * mozilla::FloorLog2).
 *
 * When constexpr support becomes universal, we should probably use that instead
 * of some of these templates, for simplicity.
 */

#ifndef mozilla_TemplateLib_h
#define mozilla_TemplateLib_h

#include <limits.h>
#include <stddef.h>
#include <type_traits>

namespace mozilla {

namespace tl {

/** Compute min/max. */
template <size_t Size, size_t... Rest>
struct Min {
  static constexpr size_t value =
      Size < Min<Rest...>::value ? Size : Min<Rest...>::value;
};

template <size_t Size>
struct Min<Size> {
  static constexpr size_t value = Size;
};

template <size_t Size, size_t... Rest>
struct Max {
  static constexpr size_t value =
      Size > Max<Rest...>::value ? Size : Max<Rest...>::value;
};

template <size_t Size>
struct Max<Size> {
  static constexpr size_t value = Size;
};

/** Compute floor(log2(i)). */
template <size_t I>
struct FloorLog2 {
  static const size_t value = 1 + FloorLog2<I / 2>::value;
};
template <>
struct FloorLog2<0> { /* Error */
};
template <>
struct FloorLog2<1> {
  static const size_t value = 0;
};

/** Compute ceiling(log2(i)). */
template <size_t I>
struct CeilingLog2 {
  static const size_t value = FloorLog2<2 * I - 1>::value;
};

/** Round up to the nearest power of 2. */
template <size_t I>
struct RoundUpPow2 {
  static const size_t value = size_t(1) << CeilingLog2<I>::value;
};
template <>
struct RoundUpPow2<0> {
  static const size_t value = 1;
};

/** Compute the number of bits in the given unsigned type. */
template <typename T>
struct BitSize {
  static const size_t value = sizeof(T) * CHAR_BIT;
};

/**
 * Produce an N-bit mask, where N <= BitSize<size_t>::value.  Handle the
 * language-undefined edge case when N = BitSize<size_t>::value.
 */
template <size_t N>
struct NBitMask {
  // Assert the precondition.  On success this evaluates to 0.  Otherwise it
  // triggers divide-by-zero at compile time: a guaranteed compile error in
  // C++11, and usually one in C++98.  Add this value to |value| to assure
  // its computation.
  static const size_t checkPrecondition =
      0 / size_t(N < BitSize<size_t>::value);
  static const size_t value = (size_t(1) << N) - 1 + checkPrecondition;
};
template <>
struct NBitMask<BitSize<size_t>::value> {
  static const size_t value = size_t(-1);
};

/**
 * For the unsigned integral type size_t, compute a mask M for N such that
 * for all X, !(X & M) implies X * N will not overflow (w.r.t size_t)
 */
template <size_t N>
struct MulOverflowMask {
  static const size_t value =
      ~NBitMask<BitSize<size_t>::value - CeilingLog2<N>::value>::value;
};
template <>
struct MulOverflowMask<0> { /* Error */
};
template <>
struct MulOverflowMask<1> {
  static const size_t value = 0;
};

/**
 * And<bool...> computes the logical 'and' of its argument booleans.
 *
 * Examples:
 *   mozilla::t1::And<true, true>::value is true.
 *   mozilla::t1::And<true, false>::value is false.
 *   mozilla::t1::And<>::value is true.
 */

template <bool... C>
struct And : std::integral_constant<bool, (C && ...)> {};

}  // namespace tl

}  // namespace mozilla

#endif /* mozilla_TemplateLib_h */
