// Copyright 2005 Google Inc.
// All Rights Reserved.
//
//

#include "util/math/mathlimits.h"
#include "base/integral_types.h"

// MSVC++ 2005 thinks the header declaration was a definition, and
// erroneously flags these as a duplicate definition.
#ifdef COMPILER_MSVC

#define DEF_COMMON_LIMITS(Type)
#define DEF_UNSIGNED_INT_LIMITS(Type)
#define DEF_SIGNED_INT_LIMITS(Type)
#define DEF_PRECISION_LIMITS(Type)

#else

#define DEF_COMMON_LIMITS(Type) \
const bool MathLimits<Type>::kIsSigned; \
const bool MathLimits<Type>::kIsInteger; \
const int MathLimits<Type>::kMin10Exp; \
const int MathLimits<Type>::kMax10Exp;

#define DEF_UNSIGNED_INT_LIMITS(Type) \
DEF_COMMON_LIMITS(Type) \
const Type MathLimits<Type>::kPosMin; \
const Type MathLimits<Type>::kPosMax; \
const Type MathLimits<Type>::kMin; \
const Type MathLimits<Type>::kMax; \
const Type MathLimits<Type>::kEpsilon; \
const Type MathLimits<Type>::kStdError;

#define DEF_SIGNED_INT_LIMITS(Type) \
DEF_UNSIGNED_INT_LIMITS(Type) \
const Type MathLimits<Type>::kNegMin; \
const Type MathLimits<Type>::kNegMax;

#define DEF_PRECISION_LIMITS(Type) \
const int MathLimits<Type>::kPrecisionDigits;

#endif  // not COMPILER_MSVC

#define DEF_FP_LIMITS(Type, PREFIX) \
DEF_COMMON_LIMITS(Type) \
const Type MathLimits<Type>::kPosMin = PREFIX##_MIN; \
const Type MathLimits<Type>::kPosMax = PREFIX##_MAX; \
const Type MathLimits<Type>::kMin = -MathLimits<Type>::kPosMax; \
const Type MathLimits<Type>::kMax = MathLimits<Type>::kPosMax; \
const Type MathLimits<Type>::kNegMin = -MathLimits<Type>::kPosMin; \
const Type MathLimits<Type>::kNegMax = -MathLimits<Type>::kPosMax; \
const Type MathLimits<Type>::kEpsilon = PREFIX##_EPSILON; \
/* 32 is 5 bits of mantissa error; should be adequate for common errors */ \
const Type MathLimits<Type>::kStdError = MathLimits<Type>::kEpsilon * 32; \
DEF_PRECISION_LIMITS(Type) \
const Type MathLimits<Type>::kNaN = HUGE_VAL - HUGE_VAL; \
const Type MathLimits<Type>::kPosInf = HUGE_VAL; \
const Type MathLimits<Type>::kNegInf = -HUGE_VAL;

DEF_SIGNED_INT_LIMITS(int8)
DEF_SIGNED_INT_LIMITS(int16)
DEF_SIGNED_INT_LIMITS(int32)
DEF_SIGNED_INT_LIMITS(int64)
DEF_UNSIGNED_INT_LIMITS(uint8)
DEF_UNSIGNED_INT_LIMITS(uint16)
DEF_UNSIGNED_INT_LIMITS(uint32)
DEF_UNSIGNED_INT_LIMITS(uint64)

DEF_SIGNED_INT_LIMITS(long int)
DEF_UNSIGNED_INT_LIMITS(unsigned long int)

DEF_FP_LIMITS(float, FLT)
DEF_FP_LIMITS(double, DBL)
DEF_FP_LIMITS(long double, LDBL);

#undef DEF_COMMON_LIMITS
#undef DEF_SIGNED_INT_LIMITS
#undef DEF_UNSIGNED_INT_LIMITS
#undef DEF_FP_LIMITS
#undef DEF_PRECISION_LIMITS
