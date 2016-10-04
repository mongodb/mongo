// Copyright 2005 Google Inc.
// All Rights Reserved.
//
//
// Useful integer and floating point limits and type traits.
//
// This partially replaces/duplictes numeric_limits<> from <limits>.
// We get a Google-style class that we have a greater control over
// and thus can add new features to it or fix whatever happens to be broken in
// numeric_limits for the compilers we use.
//

#ifndef UTIL_MATH_MATHLIMITS_H__
#define UTIL_MATH_MATHLIMITS_H__

#include <string.h>
#include <math.h>
#include <cfloat>
#include "base/basictypes.h"

// ========================================================================= //

// Useful integer and floating point limits and type traits.
// This is just for the documentation;
// real members are defined in our specializations below.
template<typename T> struct MathLimits {
  // Type name.
  typedef T Type;
  // Unsigned version of the Type with the same byte size.
  // Same as Type for floating point and unsigned types.
  typedef T UnsignedType;
  // If the type supports negative values.
  static const bool kIsSigned;
  // If the type supports only integer values.
  static const bool kIsInteger;
  // Magnitude-wise smallest representable positive value.
  static const Type kPosMin;
  // Magnitude-wise largest representable positive value.
  static const Type kPosMax;
  // Smallest representable value.
  static const Type kMin;
  // Largest representable value.
  static const Type kMax;
  // Magnitude-wise smallest representable negative value.
  // Present only if kIsSigned.
  static const Type kNegMin;
  // Magnitude-wise largest representable negative value.
  // Present only if kIsSigned.
  static const Type kNegMax;
  // Smallest integer x such that 10^x is representable.
  static const int kMin10Exp;
  // Largest integer x such that 10^x is representable.
  static const int kMax10Exp;
  // Smallest positive value such that Type(1) + kEpsilon != Type(1)
  static const Type kEpsilon;
  // Typical rounding error that is enough to cover
  // a few simple floating-point operations.
  // Slightly larger than kEpsilon to account for a few rounding errors.
  // Is zero if kIsInteger.
  static const Type kStdError;
  // Number of decimal digits of mantissa precision.
  // Present only if !kIsInteger.
  static const int kPrecisionDigits;
  // Not a number, i.e. result of 0/0.
  // Present only if !kIsInteger.
  static const Type kNaN;
  // Positive infinity, i.e. result of 1/0.
  // Present only if !kIsInteger.
  static const Type kPosInf;
  // Negative infinity, i.e. result of -1/0.
  // Present only if !kIsInteger.
  static const Type kNegInf;

  // NOTE: Special floating point values behave
  // in a special (but mathematically-logical) way
  // in terms of (in)equalty comparison and mathematical operations
  // -- see out unittest for examples.

  // Special floating point value testers.
  // Present in integer types for convenience.
  static bool IsFinite(const Type x);
  static bool IsNaN(const Type x);
  static bool IsInf(const Type x);
  static bool IsPosInf(const Type x);
  static bool IsNegInf(const Type x);
};

// ========================================================================= //

// All #define-s below are simply to refactor the declarations of
// MathLimits template specializations.
// They are all #undef-ined below.

// The hoop-jumping in *_INT_(MAX|MIN) below is so that the compiler does not
// get an overflow while computing the constants.

#define SIGNED_INT_MAX(Type) \
  (((Type(1) << (sizeof(Type)*8 - 2)) - 1) + (Type(1) << (sizeof(Type)*8 - 2)))

#define SIGNED_INT_MIN(Type) \
  (-(Type(1) << (sizeof(Type)*8 - 2)) - (Type(1) << (sizeof(Type)*8 - 2)))

#define UNSIGNED_INT_MAX(Type) \
  (((Type(1) << (sizeof(Type)*8 - 1)) - 1) + (Type(1) << (sizeof(Type)*8 - 1)))

// Compile-time selected log10-related constants for integer types.
#define SIGNED_MAX_10_EXP(Type) \
  (sizeof(Type) == 1 ? 2 : ( \
   sizeof(Type) == 2 ? 4 : ( \
   sizeof(Type) == 4 ? 9 : ( \
   sizeof(Type) == 8 ? 18 : -1))))

#define UNSIGNED_MAX_10_EXP(Type) \
  (sizeof(Type) == 1 ? 2 : ( \
   sizeof(Type) == 2 ? 4 : ( \
   sizeof(Type) == 4 ? 9 : ( \
   sizeof(Type) == 8 ? 19 : -1))))

#define DECL_INT_LIMIT_FUNCS \
  static bool IsFinite(const Type x) { return true; } \
  static bool IsNaN(const Type x) { return false; } \
  static bool IsInf(const Type x) { return false; } \
  static bool IsPosInf(const Type x) { return false; } \
  static bool IsNegInf(const Type x) { return false; }

#define DECL_SIGNED_INT_LIMITS(IntType, UnsignedIntType) \
template<> \
struct MathLimits<IntType> { \
  typedef IntType Type; \
  typedef UnsignedIntType UnsignedType; \
  static const bool kIsSigned = true; \
  static const bool kIsInteger = true; \
  static const Type kPosMin = 1; \
  static const Type kPosMax = SIGNED_INT_MAX(Type); \
  static const Type kMin = SIGNED_INT_MIN(Type); \
  static const Type kMax = kPosMax; \
  static const Type kNegMin = -1; \
  static const Type kNegMax = kMin; \
  static const int kMin10Exp = 0; \
  static const int kMax10Exp = SIGNED_MAX_10_EXP(Type); \
  static const Type kEpsilon = 1; \
  static const Type kStdError = 0; \
  DECL_INT_LIMIT_FUNCS \
};

#define DECL_UNSIGNED_INT_LIMITS(IntType) \
template<> \
struct MathLimits<IntType> { \
  typedef IntType Type; \
  typedef IntType UnsignedType; \
  static const bool kIsSigned = false; \
  static const bool kIsInteger = true; \
  static const Type kPosMin = 1; \
  static const Type kPosMax = UNSIGNED_INT_MAX(Type); \
  static const Type kMin = 0; \
  static const Type kMax = kPosMax; \
  static const int kMin10Exp = 0; \
  static const int kMax10Exp = UNSIGNED_MAX_10_EXP(Type); \
  static const Type kEpsilon = 1; \
  static const Type kStdError = 0; \
  DECL_INT_LIMIT_FUNCS \
};

DECL_SIGNED_INT_LIMITS(signed char, unsigned char)
DECL_SIGNED_INT_LIMITS(signed short int, unsigned short int)
DECL_SIGNED_INT_LIMITS(signed int, unsigned int)
DECL_SIGNED_INT_LIMITS(signed long int, unsigned long int)
DECL_SIGNED_INT_LIMITS(signed long long int, unsigned long long int)
DECL_UNSIGNED_INT_LIMITS(unsigned char)
DECL_UNSIGNED_INT_LIMITS(unsigned short int)
DECL_UNSIGNED_INT_LIMITS(unsigned int)
DECL_UNSIGNED_INT_LIMITS(unsigned long int)
DECL_UNSIGNED_INT_LIMITS(unsigned long long int)

#undef DECL_SIGNED_INT_LIMITS
#undef DECL_UNSIGNED_INT_LIMITS
#undef SIGNED_INT_MAX
#undef SIGNED_INT_MIN
#undef UNSIGNED_INT_MAX
#undef SIGNED_MAX_10_EXP
#undef UNSIGNED_MAX_10_EXP
#undef DECL_INT_LIMIT_FUNCS

// ========================================================================= //
#ifdef WIN32  // Lacks built-in isnan() and isinf()
#define DECL_FP_LIMIT_FUNCS \
  static bool IsFinite(const Type x) { return _finite(x); } \
  static bool IsNaN(const Type x) { return _isnan(x); } \
  static bool IsInf(const Type x) { return (_fpclass(x) & (_FPCLASS_NINF | _FPCLASS_PINF)) != 0; } \
  static bool IsPosInf(const Type x) { return _fpclass(x) == _FPCLASS_PINF; } \
  static bool IsNegInf(const Type x) { return _fpclass(x) == _FPCLASS_NINF; }
#else
#define DECL_FP_LIMIT_FUNCS \
  static bool IsFinite(const Type x) { return !isinf(x)  &&  !isnan(x); } \
  static bool IsNaN(const Type x) { return isnan(x); } \
  static bool IsInf(const Type x) { return isinf(x); } \
  static bool IsPosInf(const Type x) { return isinf(x)  &&  x > 0; } \
  static bool IsNegInf(const Type x) { return isinf(x)  &&  x < 0; }
#endif

// We can't put floating-point constant values in the header here because
// such constants are not considered to be primitive-type constants by gcc.
// CAVEAT: Hence, they are going to be initialized only during
// the global objects construction time.
#define DECL_FP_LIMITS(FP_Type, PREFIX) \
template<> \
struct MathLimits<FP_Type> { \
  typedef FP_Type Type; \
  typedef FP_Type UnsignedType; \
  static const bool kIsSigned = true; \
  static const bool kIsInteger = false; \
  static const Type kPosMin; \
  static const Type kPosMax; \
  static const Type kMin; \
  static const Type kMax; \
  static const Type kNegMin; \
  static const Type kNegMax; \
  static const int kMin10Exp = PREFIX##_MIN_10_EXP; \
  static const int kMax10Exp = PREFIX##_MAX_10_EXP; \
  static const Type kEpsilon; \
  static const Type kStdError; \
  static const int kPrecisionDigits = PREFIX##_DIG; \
  static const Type kNaN; \
  static const Type kPosInf; \
  static const Type kNegInf; \
  DECL_FP_LIMIT_FUNCS \
};

DECL_FP_LIMITS(float, FLT)
DECL_FP_LIMITS(double, DBL)
DECL_FP_LIMITS(long double, LDBL)

#undef DECL_FP_LIMITS
#undef DECL_FP_LIMIT_FUNCS

// ========================================================================= //

#endif  // UTIL_MATH_MATHLIMITS_H__
