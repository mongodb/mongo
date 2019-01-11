/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implements a higher-order macro for iteratively calling another macro with
 * fixed leading arguments, plus a trailing element picked from a second list
 * of arguments.
 */

#ifndef mozilla_MacroForEach_h
#define mozilla_MacroForEach_h

#include "mozilla/MacroArgs.h"

/*
 * MOZ_FOR_EACH(aMacro, aFixedArgs, aArgs) expands to N calls to the macro
 * |aMacro| where N is equal the number of items in the list |aArgs|. The
 * arguments for each |aMacro| call are composed of *all* arguments in the list
 * |aFixedArgs| as well as a single argument in the list |aArgs|. For example:
 *
 *   #define MACRO_A(x) x +
 *   int a = MOZ_FOR_EACH(MACRO_A, (), (1, 2, 3)) 0;
 *   // Expands to:     MACRO_A(1) MACRO_A(2) MACRO_A(3) 0;
 *   // And further to: 1 + 2 + 3 + 0;
 *
 *   #define MACRO_B(k, x) (k + x) +
 *   int b = MOZ_FOR_EACH(MACRO_B, (5,), (1, 2)) 0;
 *   // Expands to: MACRO_B(5, 1) MACRO_B(5, 2) 0;
 *
 *   #define MACRO_C(k1, k2, x) (k1 + k2 + x) +
 *   int c = MOZ_FOR_EACH(MACRO_C, (5, 8,), (1, 2)) 0;
 *   // Expands to: MACRO_B(5, 8, 1) MACRO_B(5, 8, 2) 0;
 *
 * MOZ_FOR_EACH_SEPARATED(aMacro, aSeparator, aFixedArgs, aArgs) is identical
 * to MOZ_FOR_EACH except that it inserts |aSeparator| between each call to
 * the macro. |aSeparator| must be wrapped by parens. For example:
 *
 *   #define MACRO_A(x) x
 *   int a = MOZ_FOR_EACH_SEPARATED(MACRO_A, (+), (), (1, 2, 3));
 *   // Expands to: MACRO_A(1) + MACRO_A(2) + MACRO_A(3);
 *   // And further to: 1 + 2 + 3
 *
 *   #define MACRO_B(t, n) t n
 *   void test(MOZ_FOR_EACH_SEPARATED(MACRO_B, (,), (int,), (a, b)));
 *   // Expands to: void test(MACRO_B(int, a) , MACRO_B(int, b));
 *   // And further to: void test(int a , int b);
 *
 * If the |aFixedArgs| list is not empty, a trailing comma must be included.
 *
 * The |aArgs| list may be up to 50 items long.
 */
#define MOZ_FOR_EACH_EXPAND_HELPER(...) __VA_ARGS__
#define MOZ_FOR_EACH_GLUE(a, b) a b
#define MOZ_FOR_EACH_SEPARATED(aMacro, aSeparator, aFixedArgs, aArgs) \
  MOZ_FOR_EACH_GLUE( \
    MOZ_PASTE_PREFIX_AND_ARG_COUNT(MOZ_FOR_EACH_, \
                                   MOZ_FOR_EACH_EXPAND_HELPER aArgs), \
    (aMacro, aSeparator, aFixedArgs, aArgs))
#define MOZ_FOR_EACH(aMacro, aFixedArgs, aArgs) \
  MOZ_FOR_EACH_SEPARATED(aMacro, (), aFixedArgs, aArgs)

#define MOZ_FOR_EACH_HELPER_GLUE(a, b) a b
#define MOZ_FOR_EACH_HELPER(aMacro, aFixedArgs, aArgs) \
  MOZ_FOR_EACH_HELPER_GLUE( \
    aMacro, \
    (MOZ_FOR_EACH_EXPAND_HELPER aFixedArgs MOZ_ARG_1 aArgs))

#define MOZ_FOR_EACH_0(m, s, fa, a)
#define MOZ_FOR_EACH_1(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a)
#define MOZ_FOR_EACH_2(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_1(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_3(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_2(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_4(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_3(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_5(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_4(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_6(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_5(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_7(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_6(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_8(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_7(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_9(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_8(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_10(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_9(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_11(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_10(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_12(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_11(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_13(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_12(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_14(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_13(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_15(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_14(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_16(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_15(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_17(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_16(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_18(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_17(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_19(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_18(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_20(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_19(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_21(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_20(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_22(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_21(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_23(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_22(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_24(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_23(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_25(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_24(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_26(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_25(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_27(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_26(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_28(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_27(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_29(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_28(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_30(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_29(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_31(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_30(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_32(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_31(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_33(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_32(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_34(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_33(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_35(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_34(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_36(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_35(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_37(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_36(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_38(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_37(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_39(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_38(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_40(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_39(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_41(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_40(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_42(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_41(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_43(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_42(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_44(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_43(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_45(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_44(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_46(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_45(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_47(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_46(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_48(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_47(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_49(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_48(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_FOR_EACH_50(m, s, fa, a) \
  MOZ_FOR_EACH_HELPER(m, fa, a) MOZ_FOR_EACH_EXPAND_HELPER s \
  MOZ_FOR_EACH_49(m, s, fa, (MOZ_ARGS_AFTER_1 a))

#endif /* mozilla_MacroForEach_h */
