// Copyright 2009 Google Inc. All Rights Reserved.
//
// ExactFloat is a multiple-precision floating point type based on the OpenSSL
// Bignum library.  It has the same interface as the built-in "float" and
// "double" types, but only supports the subset of operators and intrinsics
// where it is possible to compute the result exactly.  So for example,
// ExactFloat supports addition and multiplication but not division (since in
// general, the quotient of two floating-point numbers cannot be represented
// exactly).  Exact arithmetic is useful for geometric algorithms, especially
// for disambiguating cases where ordinary double-precision arithmetic yields
// an uncertain result.
//
// ExactFloat is a subset of the faster and more capable MPFloat class (which
// is based on the GNU MPFR library).  The main reason to use this class
// rather than MPFloat is that it is subject to a BSD-style license rather
// than the much restrictive LGPL license.
//
// It has the following features:
//
//  - ExactFloat uses the same syntax as the built-in "float" and "double"
//    types, for example: x += 4 + fabs(2*y*y - z*z).  There are a few
//    differences (see below), but the syntax is compatible enough so that
//    ExactFloat can be used as a template argument to templatized classes
//    such as Vector2, VectorN, Matrix3x3, etc.
//
//  - Results are not rounded; instead, precision is increased so that the
//    result can be represented exactly.  An inexact result is returned only
//    in the case of underflow or overflow (yielding signed zero or infinity
//    respectively), or if the maximum allowed precision is exceeded (yielding
//    NaN).  ExactFloat uses IEEE 754-2008 rules for handling infinities, NaN,
//    rounding to integers, etc.
//
//  - ExactFloat only supports calculations where the result can be
//    represented exactly.  Therefore it supports intrinsics such as fabs()
//    but not transcendentals such as sin(), sqrt(), etc.
//
// Syntax Compatibility with "float" and "double"
// ----------------------------------------------
//
// ExactFloat supports a subset of the operators and intrinsics for the
// built-in "double" type.  (Thus it supports fabs() but not fabsf(), for
// example.)  The syntax is different only in the following cases:
//
//  - Casts and implicit conversions to built-in types (including "bool") are
//    not supported.  So for example, the following will not compile:
//
//      ExactFloat x = 7.5;
//      double y = x;            // ERROR: use x.ToDouble() instead
//      long z = x;              // ERROR: use x.ToDouble() or lround(trunc(x))
//      q = static_cast<int>(x); // ERROR: use x.ToDouble() or lround(trunc(x))
//      if (x) { ... }           // ERROR: use (x != 0) instead
//
//  - The glibc floating-point classification macros (fpclassify, isfinite,
//    isnormal, isnan, isinf) are not supported.  Instead there are
//    zero-argument methods:
//
//      ExactFloat x;
//      if (isnan(x)) { ... }  // ERROR: use (x.is_nan()) instead
//      if (isinf(x)) { ... }  // ERROR: use (x.is_inf()) instead
//
// Using ExactFloat with Vector3, etc.
// -----------------------------------
//
// ExactFloat can be used with templatized classes such as Vector2 and Vector3
// (see "util/math/vector3-inl.h"), with the following limitations:
//
//  - Cast() can be used to convert other vector types to an ExactFloat vector
//    type, but not the other way around.  This is because there are no
//    implicit conversions from ExactFloat to built-in types.  You can work
//    around this by calling an explicit conversion method such as
//    ToDouble().  For example:
//
//      typedef Vector3<ExactFloat> Vector3_xf;
//      Vector3_xf x;
//      Vector3_d y;
//      x = Vector3_xf::Cast(y);   // This works.
//      y = Vector3_d::Cast(x);    // This doesn't.
//      y = Vector3_d(x[0].ToDouble(), x[1].ToDouble(), x[2].ToDouble()); // OK
//
//  - IsNaN() is not supported because it calls isnan(), which is defined as a
//    macro in <math.h> and therefore can't easily be overrided.
//
// Precision Semantics
// -------------------
//
// Unlike MPFloat, ExactFloat does not allow a maximum precision to be
// specified (it is always unbounded).  Therefore it does not have any of the
// corresponding constructors.
//
// The current precision of an ExactFloat (i.e., the number of bits in its
// mantissa) is returned by prec().  The precision is increased as necessary
// so that the result of every operation can be represented exactly.

#ifndef UTIL_MATH_EXACTFLOAT_EXACTFLOAT_H_
#define UTIL_MATH_EXACTFLOAT_EXACTFLOAT_H_

#include <math.h>
#include <limits.h>
#include <iostream>
using std::ostream;
using std::cout;
using std::endl;

#include <string>
using std::string;

#include "base/logging.h"
#include "base/integral_types.h"
#include "openssl/bn.h"

class ExactFloat {
 public:
  // The following limits are imposed by OpenSSL.

  // The maximum exponent supported.  If a value has an exponent larger than
  // this, it is replaced by infinity (with the appropriate sign).
  static const int kMaxExp = 200*1000*1000;  // About 10**(60 million)

  // The minimum exponent supported.  If a value has an exponent less than
  // this, it is replaced by zero (with the appropriate sign).
  static const int kMinExp = -kMaxExp;   // About 10**(-60 million)

  // The maximum number of mantissa bits supported.  If a value has more
  // mantissa bits than this, it is replaced with NaN.  (It is expected that
  // users of this class will never want this much precision.)
  static const int kMaxPrec = 64 << 20;  // About 20 million digits

  // Rounding modes.  kRoundTiesToEven and kRoundTiesAwayFromZero both round
  // to the nearest representable value unless two values are equally close.
  // In that case kRoundTiesToEven rounds to the nearest even value, while
  // kRoundTiesAwayFromZero always rounds away from zero.
  enum RoundingMode {
    kRoundTiesToEven,
    kRoundTiesAwayFromZero,
    kRoundTowardZero,
    kRoundAwayFromZero,
    kRoundTowardPositive,
    kRoundTowardNegative
  };

  /////////////////////////////////////////////////////////////////////////////
  // Constructors

  // The default constructor initializes the value to zero.  (The initial
  // value must be zero rather than NaN for compatibility with the built-in
  // float types.)
  inline ExactFloat();

  // Construct an ExactFloat from a "double".  The constructor is implicit so
  // that this class can be used as a replacement for "float" or "double" in
  // templatized libraries.  (With an explicit constructor, code such as
  // "ExactFloat f = 2.5;" would not compile.)  All double-precision values are
  // supported, including denormalized numbers, infinities, and NaNs.
  ExactFloat(double v);

  // Construct an ExactFloat from an "int".  Note that in general, ints are
  // automatically converted to doubles and so would be handled by the
  // constructor above.  However, the particular argument (0) is ambiguous; the
  // compiler doesn't know whether to treat it as a "double" or "NULL"
  // (invoking the const char* constructor below).
  //
  // We do not provide constructors for "unsigned", "long", "unsigned long",
  // "long long", or "unsigned long long", since these types are not typically
  // used in floating-point calculations and it is safer to require them to be
  // explicitly cast.
  ExactFloat(int v);

  // Construct an ExactFloat from a string (such as "1.2e50").  Requires that
  // the value is exactly representable as a floating-point number (so for
  // example, "0.125" is allowed but "0.1" is not).
  explicit ExactFloat(const char* s) { Unimplemented(); }

  // Copy constructor.
  ExactFloat(const ExactFloat& b);

  // The destructor is not virtual for efficiency reasons.  Therefore no
  // subclass should declare additional fields that require destruction.
  inline ~ExactFloat();

  /////////////////////////////////////////////////////////////////////
  // Constants
  //
  // As an alternative to the constants below, you can also just use the
  // constants defined in <math.h>, for example:
  //
  //   ExactFloat x = NAN, y = -INFINITY;

  // Return an ExactFloat equal to positive zero (if sign >= 0) or
  // negative zero (if sign < 0).
  static ExactFloat SignedZero(int sign);

  // Return an ExactFloat equal to positive infinity (if sign >= 0) or
  // negative infinity (if sign < 0).
  static ExactFloat Infinity(int sign);

  // Return an ExactFloat that is NaN (Not-a-Number).
  static ExactFloat NaN();

  /////////////////////////////////////////////////////////////////////////////
  // Accessor Methods

  // Return the maximum precision of the ExactFloat.  This method exists only
  // for compatibility with MPFloat.
  int max_prec() const { return kMaxPrec; }

  // Return the actual precision of this ExactFloat (the current number of
  // bits in its mantissa).  Returns 0 for non-normal numbers such as NaN.
  int prec() const;

  // Return the exponent of this ExactFloat given that the mantissa is in the
  // range [0.5, 1).  It is an error to call this method if the value is zero,
  // infinity, or NaN.
  int exp() const;

  // Set the value of the ExactFloat to +0 (if sign >= 0) or -0 (if sign < 0).
  void set_zero(int sign);

  // Set the value of the ExactFloat to positive infinity (if sign >= 0) or
  // negative infinity (if sign < 0).
  void set_inf(int sign);

  // Set the value of the ExactFloat to NaN (Not-a-Number).
  void set_nan();

  // Unfortunately, isinf(x), isnan(x), isnormal(x), and isfinite(x) are
  // defined as macros in <math.h>.  Therefore we can't easily extend them
  // here.  Instead we provide methods with underscores in their names that do
  // the same thing: x.is_inf(), etc.
  //
  // These macros are not implemented: signbit(x), fpclassify(x).

  // Return true if this value is zero (including negative zero).
  inline bool is_zero() const;

  // Return true if this value is infinity (positive or negative).
  inline bool is_inf() const;

  // Return true if this value is NaN (Not-a-Number).
  inline bool is_nan() const;

  // Return true if this value is a normal floating-point number.  Non-normal
  // values (zero, infinity, and NaN) often need to be handled separately
  // because they are represented using special exponent values and their
  // mantissa is not defined.
  inline bool is_normal() const;

  // Return true if this value is a normal floating-point number or zero,
  // i.e. it is not infinity or NaN.
  inline bool is_finite() const;

  // Return true if the sign bit is set (this includes negative zero).
  inline bool sign_bit() const;

  // Return +1 if this ExactFloat is positive, -1 if it is negative, and 0
  // if it is zero or NaN.  Note that unlike sign_bit(), sgn() returns 0 for
  // both positive and negative zero.
  inline int sgn() const;

  /////////////////////////////////////////////////////////////////////////////
  // Conversion Methods
  //
  // Note that some conversions are defined as functions further below,
  // e.g. to convert to an integer you can use lround(), llrint(), etc.

  // Round to double precision.  Note that since doubles have a much smaller
  // exponent range than ExactFloats, very small values may be rounded to
  // (positive or negative) zero, and very large values may be rounded to
  // infinity.
  //
  // It is very important to make this a named method rather than an implicit
  // conversion, because otherwise there would be a silent loss of precision
  // whenever some desired operator or function happens not to be implemented.
  // For example, if fabs() were not implemented and "x" and "y" were
  // ExactFloats, then x = fabs(y) would silently convert "y" to a "double",
  // take its absolute value, and convert it back to an ExactFloat.
  double ToDouble() const;

  // Return a human-readable string such that if two values with the same
  // precision are different, then their string representations are different.
  // The format is similar to printf("%g"), except that the number of
  // significant digits depends on the precision (with a minimum of 10).
  // Trailing zeros are stripped (just like "%g").
  //
  // Note that if two values have different precisions, they may have the same
  // ToString() value even though their values are slightly different.  If you
  // need to distinguish such values, use ToUniqueString() intead.
  string ToString() const;

  // Return a string formatted according to printf("%Ng") where N is the given
  // maximum number of significant digits.
  string ToStringWithMaxDigits(int max_digits) const;

  // Return a human-readable string such that if two ExactFloats have different
  // values, then their string representations are always different.  This
  // method is useful for debugging.  The string has the form "value<prec>",
  // where "prec" is the actual precision of the ExactFloat (e.g., "0.215<50>").
  string ToUniqueString() const;

  // Return an upper bound on the number of significant digits required to
  // distinguish any two floating-point numbers with the given precision when
  // they are formatted as decimal strings in exponential format.
  static int NumSignificantDigitsForPrec(int prec);

  // Output the ExactFloat in human-readable format, e.g. for logging.
  friend ostream& operator<<(ostream& o, ExactFloat const& f) {
    return o << f.ToString();
  }

  /////////////////////////////////////////////////////////////////////////////
  // Other Methods

  // Round the ExactFloat so that its mantissa has at most "max_prec" bits
  // using the given rounding mode.  Requires "max_prec" to be at least 2
  // (since kRoundTiesToEven doesn't make sense with fewer bits than this).
  ExactFloat RoundToMaxPrec(int max_prec, RoundingMode mode) const;

  /////////////////////////////////////////////////////////////////////////////
  // Operators

  // Assignment operator.
  ExactFloat& operator=(const ExactFloat& b);

  // Unary plus.
  ExactFloat operator+() const { return *this; }

  // Unary minus.
  ExactFloat operator-() const;

  // Addition.
  friend ExactFloat operator+(const ExactFloat& a, const ExactFloat& b);

  // Subtraction.
  friend ExactFloat operator-(const ExactFloat& a, const ExactFloat& b);

  // Multiplication.
  friend ExactFloat operator*(const ExactFloat& a, const ExactFloat& b);

  // Division is not implemented because the result cannot be represented
  // exactly in general.  Doing this properly would require extending all the
  // operations to support rounding to a specified precision.

  // Arithmetic assignment operators (+=, -=, *=).
  ExactFloat& operator+=(const ExactFloat& b) { return (*this = *this + b); }
  ExactFloat& operator-=(const ExactFloat& b) { return (*this = *this - b); }
  ExactFloat& operator*=(const ExactFloat& b) { return (*this = *this * b); }

  // Comparison operators (==, !=, <, <=, >, >=).
  friend bool operator==(const ExactFloat& a, const ExactFloat& b);
  friend bool operator<(const ExactFloat& a, const ExactFloat& b);
  // These don't need to be friends but are declared here for completeness.
  inline friend bool operator!=(const ExactFloat& a, const ExactFloat& b);
  inline friend bool operator<=(const ExactFloat& a, const ExactFloat& b);
  inline friend bool operator>(const ExactFloat& a, const ExactFloat& b);
  inline friend bool operator>=(const ExactFloat& a, const ExactFloat& b);

  /////////////////////////////////////////////////////////////////////
  // Math Intrinsics
  //
  // The math intrinsics currently supported by ExactFloat are listed below.
  // Except as noted, they behave identically to the usual glibc intrinsics
  // except that they have greater precision.  See the man pages for more
  // information.

  //////// Miscellaneous simple arithmetic functions.

  // Absolute value.
  friend ExactFloat fabs(const ExactFloat& a);

  // Maximum of two values.
  friend ExactFloat fmax(const ExactFloat& a, const ExactFloat& b);

  // Minimum of two values.
  friend ExactFloat fmin(const ExactFloat& a, const ExactFloat& b);

  // Positive difference: max(a - b, 0).
  friend ExactFloat fdim(const ExactFloat& a, const ExactFloat& b);

  //////// Integer rounding functions that return ExactFloat values.

  // Round up to the nearest integer.
  friend ExactFloat ceil(const ExactFloat& a);

  // Round down to the nearest integer.
  friend ExactFloat floor(const ExactFloat& a);

  // Round to the nearest integer not larger in absolute value.
  // For example: f(-1.9) = -1, f(2.9) = 2.
  friend ExactFloat trunc(const ExactFloat& a);

  // Round to the nearest integer, rounding halfway cases away from zero.
  // For example: f(-0.5) = -1, f(0.5) = 1, f(1.5) = 2, f(2.5) = 3.
  friend ExactFloat round(const ExactFloat& a);

  // Round to the nearest integer, rounding halfway cases to an even integer.
  // For example: f(-0.5) = 0, f(0.5) = 0, f(1.5) = 2, f(2.5) = 2.
  friend ExactFloat rint(const ExactFloat& a);

  // A synonym for rint().
  friend ExactFloat nearbyint(const ExactFloat& a) { return rint(a); }

  //////// Integer rounding functions that return C++ integer types.

  // Like rint(), but rounds to the nearest "long" value.  Returns the
  // minimum/maximum possible integer if the value is out of range.
  friend long lrint(const ExactFloat& a);

  // Like rint(), but rounds to the nearest "long long" value.  Returns the
  // minimum/maximum possible integer if the value is out of range.
  friend long long llrint(const ExactFloat& a);

  // Like round(), but rounds to the nearest "long" value.  Returns the
  // minimum/maximum possible integer if the value is out of range.
  friend long lround(const ExactFloat& a);

  // Like round(), but rounds to the nearest "long long" value.  Returns the
  // minimum/maximum possible integer if the value is out of range.
  friend long long llround(const ExactFloat& a);

  //////// Remainder functions.

  // The remainder of dividing "a" by "b", where the quotient is rounded toward
  // zero to the nearest integer.  Similar to (a - trunc(a / b) * b).
  friend ExactFloat fmod(const ExactFloat& a, const ExactFloat& b) {
    // Note that it is possible to implement this operation exactly, it just
    // hasn't been done.
    return Unimplemented();
  }

  // The remainder of dividing "a" by "b", where the quotient is rounded to the
  // nearest integer, rounding halfway cases to an even integer.  Similar to
  // (a - rint(a / b) * b).
  friend ExactFloat remainder(const ExactFloat& a, const ExactFloat& b) {
    // Note that it is possible to implement this operation exactly, it just
    // hasn't been done.
    return Unimplemented();
  }

  // A synonym for remainder().
  friend ExactFloat drem(const ExactFloat& a, const ExactFloat& b) {
    return remainder(a, b);
  }

  // Break the argument "a" into integer and fractional parts, each of which
  // has the same sign as "a".  The fractional part is returned, and the
  // integer part is stored in the output parameter "i_ptr".  Both output
  // values are set to have the same maximum precision as "a".
  friend ExactFloat modf(const ExactFloat& a, ExactFloat* i_ptr) {
    // Note that it is possible to implement this operation exactly, it just
    // hasn't been done.
    return Unimplemented();
  }

  //////// Floating-point manipulation functions.

  // Return an ExactFloat with the magnitude of "a" and the sign bit of "b".
  // (Note that an IEEE zero can be either positive or negative.)
  friend ExactFloat copysign(const ExactFloat& a, const ExactFloat& b);

  // Convert "a" to a normalized fraction in the range [0.5, 1) times a power
  // of two.  Return the fraction and set "exp" to the exponent.  If "a" is
  // zero, infinity, or NaN then return "a" and set "exp" to zero.
  friend ExactFloat frexp(const ExactFloat& a, int* exp);

  // Return "a" multiplied by 2 raised to the power "exp".
  friend ExactFloat ldexp(const ExactFloat& a, int exp);

  // A synonym for ldexp().
  friend ExactFloat scalbn(const ExactFloat& a, int exp) {
    return ldexp(a, exp);
  }

  // A version of ldexp() where "exp" is a long integer.
  friend ExactFloat scalbln(const ExactFloat& a, long exp) {
    return ldexp(a, exp);
  }

  // Convert "a" to a normalized fraction in the range [1,2) times a power of
  // two, and return the exponent value as an integer.  This is equivalent to
  // lrint(floor(log2(fabs(a)))) but it is computed more efficiently.  Returns
  // the constants documented in the man page for zero, infinity, or NaN.
  friend int ilogb(const ExactFloat& a);

  // Convert "a" to a normalized fraction in the range [1,2) times a power of
  // two, and return the exponent value as an ExactFloat.  This is equivalent to
  // floor(log2(fabs(a))) but it is computed more efficiently.
  friend ExactFloat logb(const ExactFloat& a);

 protected:
  // Non-normal numbers are represented using special exponent values and a
  // mantissa of zero.  Do not change these values; methods such as
  // is_normal() make assumptions about their ordering.  Non-normal numbers
  // can have either a positive or negative sign (including zero and NaN).
  static const int32 kExpNaN = INT_MAX;
  static const int32 kExpInfinity = INT_MAX - 1;
  static const int32 kExpZero = INT_MAX - 2;

  // Normal numbers are represented as (sign_ * bn_ * (2 ** bn_exp_)), where:
  //  - sign_ is either +1 or -1
  //  - bn_ is a BIGNUM with a positive value
  //  - bn_exp_ is the base-2 exponent applied to bn_.
  int32 sign_;
  int32 bn_exp_;
  BIGNUM bn_;

  // A standard IEEE "double" has a 53-bit mantissa consisting of a 52-bit
  // fraction plus an implicit leading "1" bit.
  static const int kDoubleMantissaBits = 53;

  // Convert an ExactFloat with no more than 53 bits in its mantissa to a
  // "double".  This method handles non-normal values (NaN, etc).
  double ToDoubleHelper() const;

  // Round an ExactFloat so that it is a multiple of (2 ** bit_exp), using the
  // given rounding mode.
  ExactFloat RoundToPowerOf2(int bit_exp, RoundingMode mode) const;

  // Convert the ExactFloat to a decimal value of the form 0.ddd * (10 ** x),
  // with at most "max_digits" significant digits (trailing zeros are removed).
  // Set (*digits) to the ASCII digits and return the decimal exponent "x".
  int GetDecimalDigits(int max_digits, string* digits) const;

  // Return a_sign * fabs(a) + b_sign * fabs(b).  Used to implement addition
  // and subtraction.
  static ExactFloat SignedSum(int a_sign, const ExactFloat* a,
                              int b_sign, const ExactFloat* b);

  // Convert an ExactFloat to its canonical form.  Underflow results in signed
  // zero, overflow results in signed infinity, and precision overflow results
  // in NaN.  A zero mantissa is converted to the canonical zero value with
  // the given sign; otherwise the mantissa is normalized so that its low bit
  // is 1.  Non-normal numbers are left unchanged.
  void Canonicalize();

  // Scale the mantissa of this ExactFloat so that it has the same bn_exp_ as
  // "b", then return -1, 0, or 1 according to whether the scaled mantissa is
  // less, equal, or greater than the mantissa of "b".  Requires that both
  // values are normal.
  int ScaleAndCompare(const ExactFloat& b) const;

  // Return true if the magnitude of this ExactFloat is less than the
  // magnitude of "b".  Requires that neither value is NaN.
  bool UnsignedLess(const ExactFloat& b) const;

  // Return an ExactFloat with the magnitude of this ExactFloat and the given
  // sign.  (Similar to copysign, except that the sign is given explicitly
  // rather than being copied from another ExactFloat.)
  inline ExactFloat CopyWithSign(int sign) const;

  // Convert an ExactFloat to an integer of type "T" using the given rounding
  // mode.  The type "T" must be signed.  Returns the largest possible integer
  // for NaN, and clamps out of range values to the largest or smallest
  // possible values.
  template <class T> T ToInteger(RoundingMode mode) const;

  // Log a fatal error message (used for unimplemented methods).
  static ExactFloat Unimplemented();
};

/////////////////////////////////////////////////////////////////////////
// Implementation details follow:

inline ExactFloat::ExactFloat() : sign_(1), bn_exp_(kExpZero) {
  BN_init(&bn_);
}

inline ExactFloat::~ExactFloat() {
  BN_free(&bn_);
}

inline bool ExactFloat::is_zero() const { return bn_exp_ == kExpZero; }
inline bool ExactFloat::is_inf() const { return bn_exp_ == kExpInfinity; }
inline bool ExactFloat::is_nan() const { return bn_exp_ == kExpNaN; }
inline bool ExactFloat::is_normal() const { return bn_exp_ < kExpZero; }
inline bool ExactFloat::is_finite() const { return bn_exp_ <= kExpZero; }
inline bool ExactFloat::sign_bit() const { return sign_ < 0; }

inline int ExactFloat::sgn() const {
  return (is_nan() || is_zero()) ? 0 : sign_;
}

inline bool operator!=(const ExactFloat& a, const ExactFloat& b) {
  return !(a == b);
}

inline bool operator<=(const ExactFloat& a, const ExactFloat& b) {
  // NaN is unordered compared to everything, including itself.
  if (a.is_nan() || b.is_nan()) return false;
  return !(b < a);
}

inline bool operator>(const ExactFloat& a, const ExactFloat& b) {
  return b < a;
}

inline bool operator>=(const ExactFloat& a, const ExactFloat& b) {
  return b <= a;
}

inline ExactFloat ExactFloat::CopyWithSign(int sign) const {
  ExactFloat r = *this;
  r.sign_ = sign;
  return r;
}

#endif  // UTIL_MATH_EXACTFLOAT_EXACTFLOAT_H_
