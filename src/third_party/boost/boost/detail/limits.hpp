// Copyright 2001 John Maddock
// Distributed under the Boost Software License, Version 1.0. (See accompany-
// ing file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

/*
 * Copyright (c) 1997
 * Silicon Graphics Computer Systems, Inc.
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Silicon Graphics makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 */

/* NOTE: This is not portable code.  Parts of numeric_limits<> are
 * inherently machine-dependent, and this file is written for the MIPS
 * architecture and the SGI MIPSpro C++ compiler.  Parts of it (in
 * particular, some of the characteristics of floating-point types)
 * are almost certainly incorrect for any other platform.
 */

/* The above comment is almost certainly out of date. This file works
 * on systems other than SGI MIPSpro C++ now.
 */

/*
 * Revision history:
 * 21 Sep 2001:
 *    Only include <cwchar> if BOOST_NO_CWCHAR is defined. (Darin Adler)
 * 10 Aug 2001:
 *    Added MIPS (big endian) to the big endian family. (Jens Maurer)
 * 13 Apr 2001:
 *    Added powerpc to the big endian family. (Jeremy Siek)
 * 5 Apr 2001:
 *    Added sparc (big endian) processor support (John Maddock).
 * Initial sub:
 *      Modified by Jens Maurer for gcc 2.95 on x86.
 */

#ifndef BOOST_SGI_CPP_LIMITS
#define BOOST_SGI_CPP_LIMITS

#include <climits>
#include <cfloat>
#include <boost/config.hpp>
#include <boost/detail/endian.hpp>

#ifndef BOOST_NO_CWCHAR
#include <cwchar> // for WCHAR_MIN and WCHAR_MAX
#endif

namespace std {

enum float_round_style {
  round_indeterminate       = -1,
  round_toward_zero         =  0,
  round_to_nearest          =  1,
  round_toward_infinity     =  2,
  round_toward_neg_infinity =  3
};

enum float_denorm_style {
  denorm_indeterminate = -1,
  denorm_absent        =  0,
  denorm_present       =  1
};

// The C++ standard (section 18.2.1) requires that some of the members of
// numeric_limits be static const data members that are given constant-
// initializers within the class declaration.  On compilers where the
// BOOST_NO_INCLASS_MEMBER_INITIALIZATION macro is defined, it is impossible to write
// a standard-conforming numeric_limits class.
//
// There are two possible workarounds: either initialize the data
// members outside the class, or change them from data members to
// enums.  Neither workaround is satisfactory: the former makes it
// impossible to use the data members in constant-expressions, and the
// latter means they have the wrong type and that it is impossible to
// take their addresses.  We choose the former workaround.

#ifdef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
# define BOOST_STL_DECLARE_LIMITS_MEMBER(__mem_type, __mem_name, __mem_value) \
  enum { __mem_name = __mem_value }
#else /* BOOST_NO_INCLASS_MEMBER_INITIALIZATION */
# define BOOST_STL_DECLARE_LIMITS_MEMBER(__mem_type, __mem_name, __mem_value) \
  static const __mem_type __mem_name = __mem_value
#endif /* BOOST_NO_INCLASS_MEMBER_INITIALIZATION */

// Base class for all specializations of numeric_limits.
template <class __number>
class _Numeric_limits_base {
public:
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_specialized, false);

  static __number min BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return __number(); }
  static __number max BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return __number(); }

  BOOST_STL_DECLARE_LIMITS_MEMBER(int, digits,   0);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, digits10, 0);

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_signed,  false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_integer, false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_exact,   false);

  BOOST_STL_DECLARE_LIMITS_MEMBER(int, radix, 0);

  static __number epsilon() throw()     { return __number(); }
  static __number round_error() throw() { return __number(); }

  BOOST_STL_DECLARE_LIMITS_MEMBER(int, min_exponent,   0);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, min_exponent10, 0);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, max_exponent,   0);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, max_exponent10, 0);

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, has_infinity,      false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, has_quiet_NaN,     false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, has_signaling_NaN, false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(float_denorm_style,
                              has_denorm,
                              denorm_absent);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, has_denorm_loss,   false);

  static __number infinity() throw()      { return __number(); }
  static __number quiet_NaN() throw()     { return __number(); }
  static __number signaling_NaN() throw() { return __number(); }
  static __number denorm_min() throw()    { return __number(); }

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_iec559,  false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_bounded, false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_modulo,  false);

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, traps,            false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, tinyness_before,  false);
  BOOST_STL_DECLARE_LIMITS_MEMBER(float_round_style,
                              round_style,
                              round_toward_zero);
};

// Base class for integers.

template <class _Int,
          _Int __imin,
          _Int __imax,
          int __idigits = -1>
class _Integer_limits : public _Numeric_limits_base<_Int> 
{
public:
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_specialized, true);

  static _Int min BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return __imin; }
  static _Int max BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return __imax; }

  BOOST_STL_DECLARE_LIMITS_MEMBER(int,
                              digits,
                              (__idigits < 0) ? (int)(sizeof(_Int) * CHAR_BIT)
                                                   - (__imin == 0 ? 0 : 1) 
                                              : __idigits);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, digits10, (digits * 301) / 1000); 
                                // log 2 = 0.301029995664...

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_signed,  __imin != 0);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_integer, true);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_exact,   true);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int,  radix,      2);

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_bounded, true);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_modulo, true);
};

#if defined(BOOST_BIG_ENDIAN)

 template<class Number, unsigned int Word>
 struct float_helper{
  static Number get_word() throw() {
    // sizeof(long double) == 16
    const unsigned int _S_word[4] = { Word, 0, 0, 0 };
    return *reinterpret_cast<const Number*>(&_S_word);
  } 
};

#else

 template<class Number, unsigned int Word>
 struct float_helper{
  static Number get_word() throw() {
    // sizeof(long double) == 12, but only 10 bytes significant
    const unsigned int _S_word[4] = { 0, 0, 0, Word };
    return *reinterpret_cast<const Number*>(
        reinterpret_cast<const char *>(&_S_word)+16-
                (sizeof(Number) == 12 ? 10 : sizeof(Number)));
  } 
};

#endif

// Base class for floating-point numbers.
template <class __number,
         int __Digits, int __Digits10,
         int __MinExp, int __MaxExp,
         int __MinExp10, int __MaxExp10,
         unsigned int __InfinityWord,
         unsigned int __QNaNWord, unsigned int __SNaNWord,
         bool __IsIEC559,
         float_round_style __RoundStyle>
class _Floating_limits : public _Numeric_limits_base<__number>
{
public:
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_specialized, true);

  BOOST_STL_DECLARE_LIMITS_MEMBER(int, digits,   __Digits);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, digits10, __Digits10);

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_signed, true);

  BOOST_STL_DECLARE_LIMITS_MEMBER(int, radix, 2);

  BOOST_STL_DECLARE_LIMITS_MEMBER(int, min_exponent,   __MinExp);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, max_exponent,   __MaxExp);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, min_exponent10, __MinExp10);
  BOOST_STL_DECLARE_LIMITS_MEMBER(int, max_exponent10, __MaxExp10);

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, has_infinity,      true);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, has_quiet_NaN,     true);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, has_signaling_NaN, true);
  BOOST_STL_DECLARE_LIMITS_MEMBER(float_denorm_style,
                              has_denorm,
                              denorm_indeterminate);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, has_denorm_loss,   false);

 
  static __number infinity() throw() {
    return float_helper<__number, __InfinityWord>::get_word();
  }
  static __number quiet_NaN() throw() {
    return float_helper<__number,__QNaNWord>::get_word();
  }
  static __number signaling_NaN() throw() {
    return float_helper<__number,__SNaNWord>::get_word();
  }

  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_iec559,       __IsIEC559);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, is_bounded,      true);
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, traps,           false /* was: true */ );
  BOOST_STL_DECLARE_LIMITS_MEMBER(bool, tinyness_before, false);

  BOOST_STL_DECLARE_LIMITS_MEMBER(float_round_style, round_style, __RoundStyle);
};

// Class numeric_limits

// The unspecialized class.

template<class T> 
class numeric_limits : public _Numeric_limits_base<T> {};

// Specializations for all built-in integral types.

template<>
class numeric_limits<bool>
  : public _Integer_limits<bool, false, true, 0>
{};

template<>
class numeric_limits<char>
  : public _Integer_limits<char, CHAR_MIN, CHAR_MAX>
{};

template<>
class numeric_limits<signed char>
  : public _Integer_limits<signed char, SCHAR_MIN, SCHAR_MAX>
{};

template<>
class numeric_limits<unsigned char>
  : public _Integer_limits<unsigned char, 0, UCHAR_MAX>
{};

#ifndef BOOST_NO_INTRINSIC_WCHAR_T
template<>
class numeric_limits<wchar_t>
#if !defined(WCHAR_MAX) || !defined(WCHAR_MIN)
#if defined(_WIN32) || defined(__CYGWIN__)
  : public _Integer_limits<wchar_t, 0, USHRT_MAX>
#elif defined(__hppa)
// wchar_t has "unsigned int" as the underlying type
  : public _Integer_limits<wchar_t, 0, UINT_MAX>
#else
// assume that wchar_t has "int" as the underlying type
  : public _Integer_limits<wchar_t, INT_MIN, INT_MAX>
#endif
#else
// we have WCHAR_MIN and WCHAR_MAX defined, so use it
  : public _Integer_limits<wchar_t, WCHAR_MIN, WCHAR_MAX>
#endif
{};
#endif

template<>
class numeric_limits<short>
  : public _Integer_limits<short, SHRT_MIN, SHRT_MAX>
{};

template<>
class numeric_limits<unsigned short>
  : public _Integer_limits<unsigned short, 0, USHRT_MAX>
{};

template<>
class numeric_limits<int>
  : public _Integer_limits<int, INT_MIN, INT_MAX>
{};

template<>
class numeric_limits<unsigned int>
  : public _Integer_limits<unsigned int, 0, UINT_MAX>
{};

template<>
class numeric_limits<long>
  : public _Integer_limits<long, LONG_MIN, LONG_MAX>
{};

template<>
class numeric_limits<unsigned long>
  : public _Integer_limits<unsigned long, 0, ULONG_MAX>
{};

#ifdef __GNUC__

// Some compilers have long long, but don't define the
// LONGLONG_MIN and LONGLONG_MAX macros in limits.h.  This
// assumes that long long is 64 bits.
#if !defined(LONGLONG_MAX) && !defined(ULONGLONG_MAX)

# define ULONGLONG_MAX 0xffffffffffffffffLLU
# define LONGLONG_MAX 0x7fffffffffffffffLL

#endif

#if !defined(LONGLONG_MIN)
# define LONGLONG_MIN (-LONGLONG_MAX - 1)
#endif 


#if !defined(ULONGLONG_MIN)
# define ULONGLONG_MIN 0
#endif 

#endif /* __GNUC__ */

// Specializations for all built-in floating-point type.

template<> class numeric_limits<float>
  : public _Floating_limits<float, 
                            FLT_MANT_DIG,   // Binary digits of precision
                            FLT_DIG,        // Decimal digits of precision
                            FLT_MIN_EXP,    // Minimum exponent
                            FLT_MAX_EXP,    // Maximum exponent
                            FLT_MIN_10_EXP, // Minimum base 10 exponent
                            FLT_MAX_10_EXP, // Maximum base 10 exponent
#if defined(BOOST_BIG_ENDIAN)
                            0x7f80 << (sizeof(int)*CHAR_BIT-16),    // Last word of +infinity
                            0x7f81 << (sizeof(int)*CHAR_BIT-16),    // Last word of quiet NaN
                            0x7fc1 << (sizeof(int)*CHAR_BIT-16),    // Last word of signaling NaN
#else
                            0x7f800000u,    // Last word of +infinity
                            0x7f810000u,    // Last word of quiet NaN
                            0x7fc10000u,    // Last word of signaling NaN
#endif
                            true,           // conforms to iec559
                            round_to_nearest>
{
public:
  static float min BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return FLT_MIN; }
  static float denorm_min() throw() { return FLT_MIN; }
  static float max BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return FLT_MAX; }
  static float epsilon() throw() { return FLT_EPSILON; }
  static float round_error() throw() { return 0.5f; } // Units: ulps.
};

template<> class numeric_limits<double>
  : public _Floating_limits<double, 
                            DBL_MANT_DIG,   // Binary digits of precision
                            DBL_DIG,        // Decimal digits of precision
                            DBL_MIN_EXP,    // Minimum exponent
                            DBL_MAX_EXP,    // Maximum exponent
                            DBL_MIN_10_EXP, // Minimum base 10 exponent
                            DBL_MAX_10_EXP, // Maximum base 10 exponent
#if defined(BOOST_BIG_ENDIAN)
                            0x7ff0 << (sizeof(int)*CHAR_BIT-16),    // Last word of +infinity
                            0x7ff1 << (sizeof(int)*CHAR_BIT-16),    // Last word of quiet NaN
                            0x7ff9 << (sizeof(int)*CHAR_BIT-16),    // Last word of signaling NaN
#else
                            0x7ff00000u,    // Last word of +infinity
                            0x7ff10000u,    // Last word of quiet NaN
                            0x7ff90000u,    // Last word of signaling NaN
#endif
                            true,           // conforms to iec559
                            round_to_nearest>
{
public:
  static double min BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return DBL_MIN; }
  static double denorm_min() throw() { return DBL_MIN; }
  static double max BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return DBL_MAX; }
  static double epsilon() throw() { return DBL_EPSILON; }
  static double round_error() throw() { return 0.5; } // Units: ulps.
};

template<> class numeric_limits<long double>
  : public _Floating_limits<long double, 
                            LDBL_MANT_DIG,  // Binary digits of precision
                            LDBL_DIG,       // Decimal digits of precision
                            LDBL_MIN_EXP,   // Minimum exponent
                            LDBL_MAX_EXP,   // Maximum exponent
                            LDBL_MIN_10_EXP,// Minimum base 10 exponent
                            LDBL_MAX_10_EXP,// Maximum base 10 exponent
#if defined(BOOST_BIG_ENDIAN)
                            0x7ff0 << (sizeof(int)*CHAR_BIT-16),    // Last word of +infinity
                            0x7ff1 << (sizeof(int)*CHAR_BIT-16),    // Last word of quiet NaN
                            0x7ff9 << (sizeof(int)*CHAR_BIT-16),    // Last word of signaling NaN
#else
                            0x7fff8000u,    // Last word of +infinity
                            0x7fffc000u,    // Last word of quiet NaN
                            0x7fff9000u,    // Last word of signaling NaN
#endif
                            false,          // Doesn't conform to iec559
                            round_to_nearest>
{
public:
  static long double min BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return LDBL_MIN; }
  static long double denorm_min() throw() { return LDBL_MIN; }
  static long double max BOOST_PREVENT_MACRO_SUBSTITUTION () throw() { return LDBL_MAX; }
  static long double epsilon() throw() { return LDBL_EPSILON; }
  static long double round_error() throw() { return 4; } // Units: ulps.
};

} // namespace std

#endif /* BOOST_SGI_CPP_LIMITS */

// Local Variables:
// mode:C++
// End:



