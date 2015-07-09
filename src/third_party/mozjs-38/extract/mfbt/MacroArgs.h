/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implements various macros meant to ease the use of variadic macros.
 */

#ifndef mozilla_MacroArgs_h
#define mozilla_MacroArgs_h

/*
 * MOZ_PASTE_PREFIX_AND_ARG_COUNT(aPrefix, ...) counts the number of variadic
 * arguments and prefixes it with |aPrefix|. For example:
 *
 *   MOZ_PASTE_PREFIX_AND_ARG_COUNT(, foo, 42) expands to 2
 *   MOZ_PASTE_PREFIX_AND_ARG_COUNT(A, foo, 42, bar) expands to A3
 *
 * You must pass in between 1 and 50 (inclusive) variadic arguments, past
 * |aPrefix|. It is not legal to do
 *
 *   MOZ_PASTE_PREFIX_AND_ARG_COUNT(prefix)
 *
 * (that is, pass in 0 variadic arguments). To ensure that a compile-time
 * error occurs when these constraints are violated, use the
 * MOZ_STATIC_ASSERT_VALID_ARG_COUNT macro with the same variaidc arguments
 * wherever this macro is used.
 *
 * Passing (__VA_ARGS__, <rest of arguments>) rather than simply calling
 * MOZ_MACROARGS_ARG_COUNT_HELPER2(__VA_ARGS__, <rest of arguments>) very
 * carefully tiptoes around a MSVC bug where it improperly expands __VA_ARGS__
 * as a single token in argument lists. For details, see:
 *
 *   http://connect.microsoft.com/VisualStudio/feedback/details/380090/variadic-macro-replacement
 *   http://cplusplus.co.il/2010/07/17/variadic-macro-to-count-number-of-arguments/#comment-644
 */
#define MOZ_PASTE_PREFIX_AND_ARG_COUNT(aPrefix, ...) \
  MOZ_MACROARGS_ARG_COUNT_HELPER((__VA_ARGS__, \
    aPrefix##50, aPrefix##49, aPrefix##48, aPrefix##47, aPrefix##46, \
    aPrefix##45, aPrefix##44, aPrefix##43, aPrefix##42, aPrefix##41, \
    aPrefix##40, aPrefix##39, aPrefix##38, aPrefix##37, aPrefix##36, \
    aPrefix##35, aPrefix##34, aPrefix##33, aPrefix##32, aPrefix##31, \
    aPrefix##30, aPrefix##29, aPrefix##28, aPrefix##27, aPrefix##26, \
    aPrefix##25, aPrefix##24, aPrefix##23, aPrefix##22, aPrefix##21, \
    aPrefix##20, aPrefix##19, aPrefix##18, aPrefix##17, aPrefix##16, \
    aPrefix##15, aPrefix##14, aPrefix##13, aPrefix##12, aPrefix##11, \
    aPrefix##10, aPrefix##9,  aPrefix##8,  aPrefix##7,  aPrefix##6,  \
    aPrefix##5,  aPrefix##4,  aPrefix##3,  aPrefix##2,  aPrefix##1, aPrefix##0))

#define MOZ_MACROARGS_ARG_COUNT_HELPER(aArgs) \
  MOZ_MACROARGS_ARG_COUNT_HELPER2 aArgs

#define MOZ_MACROARGS_ARG_COUNT_HELPER2( \
   a1,  a2,  a3,  a4,  a5,  a6,  a7,  a8,  a9, a10, \
  a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, \
  a21, a22, a23, a24, a25, a26, a27, a28, a29, a30, \
  a31, a32, a33, a34, a35, a36, a37, a38, a39, a40, \
  a41, a42, a43, a44, a45, a46, a47, a48, a49, a50, \
  a51, ...) a51

/*
 * MOZ_STATIC_ASSERT_VALID_ARG_COUNT ensures that a compile-time error occurs
 * when the argument count constraints of MOZ_PASTE_PREFIX_AND_ARG_COUNT are
 * violated. Use this macro wherever MOZ_PASTE_PREFIX_AND_ARG_COUNT is used
 * and pass it the same variadic arguments.
 *
 * This macro employs a few dirty tricks to function. To detect the zero
 * argument case, |(__VA_ARGS__)| is stringified, sizeof-ed, and compared to
 * what it should be in the absence of arguments.
 *
 * Detecting too many arguments is a little trickier. With a valid argument
 * count and a prefix of 1, MOZ_PASTE_PREFIX_AND_ARG_COUNT expands to e.g. 14.
 * With a prefix of 0.0, it expands to e.g. 0.04. If there are too many
 * arguments, it expands to the first argument over the limit. If this
 * exceeding argument is a number, the assertion will fail as there is no
 * number than can simultaneously be both > 10 and == 0. If the exceeding
 * argument is not a number, a compile-time error should still occur due to
 * the operations performed on it.
 */
#define MOZ_MACROARGS_STRINGIFY_HELPER(x) #x
#define MOZ_STATIC_ASSERT_VALID_ARG_COUNT(...) \
  static_assert( \
    sizeof(MOZ_MACROARGS_STRINGIFY_HELPER((__VA_ARGS__))) != sizeof("()") && \
      (MOZ_PASTE_PREFIX_AND_ARG_COUNT(1, __VA_ARGS__)) > 10 && \
      (int)(MOZ_PASTE_PREFIX_AND_ARG_COUNT(0.0, __VA_ARGS__)) == 0, \
    "MOZ_STATIC_ASSERT_VALID_ARG_COUNT requires 1 to 50 arguments") /* ; */

/*
 * MOZ_ARGS_AFTER_N expands to its arguments excluding the first |N|
 * arguments. For example:
 *
 *   MOZ_ARGS_AFTER_2(a, b, c, d) expands to: c, d
 */
#define MOZ_ARGS_AFTER_1(a1, ...) __VA_ARGS__
#define MOZ_ARGS_AFTER_2(a1, a2, ...) __VA_ARGS__

/*
 * MOZ_ARG_N expands to its |N|th argument.
 */
#define MOZ_ARG_1(a1, ...) a1
#define MOZ_ARG_2(a1, a2, ...) a2

#endif /* mozilla_MacroArgs_h */
