// Copyright 2001 and onwards Google Inc.
//
// This class is intended to contain a collection of useful (static)
// mathematical functions, properly coded (by consulting numerical
// recipes or another authoritative source first).

#ifndef UTIL_MATH_MATHUTIL_H__
#define UTIL_MATH_MATHUTIL_H__

#include <math.h>
#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <vector>
using std::vector;

#include "base/basictypes.h"
#include "base/logging.h"

// Returns the sign of x:
//   -1 if x < 0,
//   +1 if x > 0,
//    0 if x = 0.
// Consider instead using MathUtil::Sign below for readability
// and floating-point correctness.
template <class T>
inline T sgn(const T x) {
  return (x == 0 ? 0 : (x < 0 ? -1 : 1));
}
// HACK ALERT
// So here's the deal: There's a ton of junk defined in mathlimits.h that should be moved into
// mathlimits.cc.  But the only thing that uses mathlimits.cc/h is this, mathutil.cc/h.
// So I moved a class definition into this .h file before the stuff that references it and stuck
// mathutil.cc into mathlimits.cc.  Voila!
#ifndef UTIL_MATH_MATHLIMITS_H__
#define UTIL_MATH_MATHLIMITS_H__
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
// END HACK ALERT
#endif  //UTIL_MATH_MATHLIMITS_H
// ========================================================================= //

class MathUtil {
 public:

  // Return type of RealRootsForQuadratic (below).  The enum values are
  // chosen to be sensible if converted to bool or int, and should not be
  // changed lightly.
  enum QuadraticRootType {kNoRealRoots = 0, kAmbiguous = 1, kTwoRealRoots = 2};

  // Returns the QuadraticRootType of the equation a * x^2 + b * x + c = 0.
  // Normal cases are kNoRealRoots, in which case *r1 and *r2 are not
  // changed; and kTwoRealRoots, in which case the root(s) are placed in
  // *r1 and *r2, order not specified.  The kAmbiguous return value
  // indicates that the disciminant is zero, within floating-point error
  // (i.e. that changing an input by epsilon<double> would change the sign
  // of the discriminant). The resulting roots are equal, as if the
  // discriminant were exactly zero.
  //
  // A special case occurs when a==0; see DegenerateQuadraticRoots().
  // See also QuadraticIsAmbiguous() and RealQuadraticRoots().
  static QuadraticRootType RealRootsForQuadratic(long double a,
                                                        long double b,
                                                        long double c,
                                                        long double *r1,
                                                        long double *r2);

  // Returns the discriminant of the quadratic equation a * x^2 + b * x + c = 0.
  static inline long double QuadraticDiscriminant(long double a,
                                                  long double b,
                                                  long double c) {
    return b * b - 4 * a * c;
  }

  // Returns true if the discriminant is zero within floating-point error,
  // in the sense that changing one of the coefficients by epsilon (e.g. a
  // -> a + a*epsilon) could change the sign of the discriminant. [When the
  // discriminant is exactly 0 the quadratic is (2*a*x + b)^2 = 0 and the
  // root is - b / (2*a).]
  static inline bool QuadraticIsAmbiguous(long double a,
                                          long double b,
                                          long double c,
                                          long double discriminant,
                                          long double epsilon) {
    // Discriminants below kTolerance in absolute value are considered zero
    // because changing the final bit of one of the inputs can change the
    // sign of the discriminant.
    const double kTolerance = epsilon * max(fabs(2 * b * b), fabs(4 * a * c));
    return (fabs(discriminant) <= kTolerance);
  }

  // Returns in *r1 and *r2 the roots of a "normal" quadratic equation
  // whose discriminant (b*b - 4*a*c) is known and positive.  Preconditions
  // (will DCHECK and return false if not satisfied): a != 0, discriminant > 0.
  static inline bool RealQuadraticRoots(long double a,
                                        long double b,
                                        long double c,
                                        long double discriminant,
                                        long double *r1,
                                        long double *r2) {
    if (discriminant <= 0 || a == 0) {
      // A case that should have been excluded by the caller.
      DCHECK(false);
      return false;
    }

    // The discriminant is positive so there are two distinct roots.
    // According to Numerical Recipes (p. 184), it would be a mistake to
    // solve for the roots using
    //
    //     r1 = 2c / (-b + sqrt(b^2 - 4ac)),
    //     r2 = 2c / (-b - sqrt(b^2 - 4ac)).
    //
    // If a*c is small, then one of the roots above will involve the
    // subtraction of b from a very nearly equal quantity (the discriminant),
    // producing a very inaccurate root.  Avoid the risk of cancellation with
    // the following rearrangement.  (Note we don't use sgn(b) because we
    // need sgn(0) = +1 or -1.)
    long double const q = -0.5 *
        (b + ((b >= 0) ? sqrt(discriminant) : -sqrt(discriminant)));
    *r1 = q / a;  // If a is very small this produces +/- HUGE_VAL.
    *r2 = c / q;  // q cannot be too small.
    return true;
  }

  // Returns the root of the degenerate quadratic equation b * x + c = 0,
  // following the interface of RealRootsForQuadratic. To be consistent
  // with that function as a->0, the degenerate quadratic is considered to
  // have two real roots, one of which is +/- HUGE_VAL and one of which is
  // -c / b.  If both a and b are 0, so the equation is c = 0, the response
  // is kNoRealRoots if c != 0 or kAmbiguous if c == 0 (since the
  // discriminant is zero).
  static QuadraticRootType DegenerateQuadraticRoots(long double b,
                                                    long double c,
                                                    long double *r1,
                                                    long double *r2);

  // Solves for the real roots of x^3+ax^2+bx+c=0, returns true iff
  // all three are real, in which case the roots are stored (in any
  // order) in r1, r2, r3; otherwise, exactly one real root exists and
  // it is stored in r1.
  static bool RealRootsForCubic(long double a,
                                long double b,
                                long double c,
                                long double *r1,
                                long double *r2,
                                long double *r3);


  // ----------------------------------------------------------------------
  // Sigmoid
  //   A sigmoid function is a differentiably s curve that ranges between
  //   0 and 1:
  //   f(x) = 1/(1+e^(-lambda x))
  // --------------------------------------------------------------------
  static double Sigmoid(double x, double lambda) {
    return 1/(1+exp(-lambda*x));
  }

  // ----------------------------------------------------------------------
  // InverseSigmoid
  //   Inverts Sigmoid such that InverseSigmoid(Sigmoid(x)) == x for all x
  //   Note that all inputs must be in (-1, 1)
  // ----------------------------------------------------------------------
  static double InverseSigmoid(double const x, double const lambda) {
    return -log((1.0 / x - 1)) / lambda;
  }

  // ----------------------------------------------------------------------
  // Sigmoid2
  //   A nicer way of specifying a sigmoid. A sigmoid is a smooth s curve
  //   that ranges from 0 to 1. We describe a sigmoid  with three values:
  //
  //   start: the x value at which f(x) = tolerance
  //   finish: the x value at which f(x) = 1-tolerance
  //
  // So if we was a smoothly transitioning function from, say, x=1 to
  // x=10 with the property that anything outside the domain [1, 10]
  // will still be within 10% of either f(1) or f(10) then we set: start
  // = 1 finish = 10 tolerance = 0.1
  // --------------------------------------------------------------------
  static double Sigmoid2(double x, double start_x,
                         double finish_x, double tolerance) {
    DCHECK_GT(tolerance, 0);
    DCHECK_LT(tolerance, 1);
    DCHECK_NE(finish_x - start_x, 0);
    double lambda = log((1-tolerance)/tolerance)*2/(finish_x - start_x);
    return Sigmoid(x - 0.5 * (start_x + finish_x), lambda);
  }

  // Returns the greatest common divisor of two unsigned integers x and y
  static unsigned int GCD(unsigned int x, unsigned int y) {
    while (y != 0) {
      unsigned int r = x % y;
      x = y;
      y = r;
    }
    return x;
  }

  // Returns the greatest common divisor of two unsigned integers x and y,
  // and assigns a, and b such that a*x + b*y = gcd(x, y).
  static unsigned int ExtendedGCD(unsigned int x, unsigned int y,
                                  int* a, int* b);

  // Returns the least common multiple of two unsigned integers.  Returns zero
  // if either is zero.
  static unsigned int LeastCommonMultiple(unsigned int a, unsigned int b) {
    if (a > b) {
      return (a / MathUtil::GCD(a, b)) * b;
    } else if (a < b) {
      return (b / MathUtil::GCD(b, a)) * a;
    } else {
      return a;
    }
  }

  // Converts a non-zero double value representing an odds into its
  // probability value.
  static double OddsToProbability(double odds) {
    DCHECK_GE(odds, 0.0);
    return odds / (1.0 + odds);
  }

  // Converts a probability with range [0-1.0) into its odds value.
  static double ProbabilityToOdds(double prob) {
    DCHECK_GE(prob, 0.0);
    DCHECK_LT(prob, 1.0);
    return prob / (1.0 - prob);
  }

  // --------------------------------------------------------------------
  // ShardsToRead
  //   Resharding helper.  Suppose we have N input shards and M output
  //   shards sharded by modulo of the same hash function.  If we want
  //   to write a subset of the output shards, which input shards should
  //   we read?
  //
  // Inputs:
  //   shards_to_write gives the desired subset of the M output shards.
  //   shards_to_read gives the number N of the input shards.
  // Outputs:
  //   shards_to_read gives the subset of the N input shards to read.
  // --------------------------------------------------------------------
  static void ShardsToRead(const vector<bool>& shards_to_write,
                           vector<bool>* shards_to_read);

  // --------------------------------------------------------------------
  // Round, IntRound
  //   These functions round a floating-point number to an integer.  They
  //   work for positive or negative numbers.
  //
  //   Values that are halfway between two integers may be rounded up or
  //   down, for example IntRound(0.5) == 0 and IntRound(1.5) == 2.  This
  //   allows these functions to be implemented efficiently on Intel
  //   processors (see the template specializations at the bottom of this
  //   file).  You should not use these functions if you care about which
  //   way such half-integers are rounded.
  //
  //   Example usage:
  //     double y, z;
  //     int x = IntRound(y + 3.7);
  //     int64 b = Round<int64>(0.3 * z);
  //
  //   Note that the floating-point template parameter is typically inferred
  //   from the argument type, i.e. there is no need to specify it explicitly.
  // --------------------------------------------------------------------
  template <class IntOut, class FloatIn>
  static IntOut Round(FloatIn x);

  // Example usage: IntRound(3.6) (no need for IntRound<double>(3.6)).
  template <class FloatIn>
  static int IntRound(FloatIn x) { return Round<int>(x); }

  // --------------------------------------------------------------------
  // FastIntRound, FastInt64Round
  //   Fast routines for converting floating-point numbers to integers.
  //
  //   These routines are approximately 6 times faster than the default
  //   implementation of IntRound() on Intel processors (12 times faster on
  //   the Pentium 3).  They are also more than 5 times faster than simply
  //   casting a "double" to an "int" using static_cast<int>.  This is
  //   because casts are defined to truncate towards zero, which on Intel
  //   processors requires changing the rounding mode and flushing the
  //   floating-point pipeline (unless programs are compiled specifically
  //   for the Pentium 4, which has a new instruction to avoid this).
  //
  //   Numbers that are halfway between two integers may be rounded up or
  //   down.  This is because the conversion is done using the default
  //   rounding mode, which rounds towards the closest even number in case
  //   of ties.  So for example, FastIntRound(0.5) == 0, but
  //   FastIntRound(1.5) == 2.  These functions should only be used with
  //   applications that don't care about which way such half-integers are
  //   rounded.
  //
  //   There are template specializations of Round() which call these
  //   functions (for "int" and "int64" only), but it's safer to call them
  //   directly.
  //
  //   This functions are equivalent to lrint() and llrint() as defined in
  //   the ISO C99 standard.  Unfortunately this standard does not seem to
  //   widely adopted yet and these functions are not available by default.
  //   --------------------------------------------------------------------

  static int32 FastIntRound(double x) {
    // This function is not templatized because gcc doesn't seem to be able
    // to deal with inline assembly code in templatized functions, and there
    // is no advantage to passing an argument type of "float" on Intel
    // architectures anyway.

#if defined __GNUC__ && (defined __i386__ || defined __SSE2__)
#if defined __SSE2__
    // SSE2.
    int32 result;
    __asm__ __volatile__
        ("cvtsd2si %1, %0"
         : "=r" (result)    // Output operand is a register
         : "x" (x));        // Input operand is an xmm register
    return result;
#elif defined __i386__
    // FPU stack.  Adapted from /usr/include/bits/mathinline.h.
    int32 result;
    __asm__ __volatile__
        ("fistpl %0"
         : "=m" (result)    // Output operand is a memory location
         : "t" (x)          // Input operand is top of FP stack
         : "st");           // Clobbers (pops) top of FP stack
    return result;
#endif  // if defined __x86_64__ || ...
#else
    return Round<int32, double>(x);
#endif  // if defined __GNUC__ && ...
  }

  static int64 FastInt64Round(double x) {
#if defined __GNUC__ && (defined __i386__ || defined __x86_64__)
#if defined __x86_64__
    // SSE2.
    int64 result;
    __asm__ __volatile__
        ("cvtsd2si %1, %0"
         : "=r" (result)    // Output operand is a register
         : "x" (x));        // Input operand is an xmm register
    return result;
#elif defined __i386__
    // There is no CVTSD2SI in i386 to produce a 64 bit int, even with SSE2.
    // FPU stack.  Adapted from /usr/include/bits/mathinline.h.
    int64 result;
    __asm__ __volatile__
        ("fistpll %0"
         : "=m" (result)    // Output operand is a memory location
         : "t" (x)          // Input operand is top of FP stack
         : "st");           // Clobbers (pops) top of FP stack
    return result;
#endif  // if defined __i386__
#else
    return Round<int64, double>(x);
#endif  // if defined __GNUC__ && ...
  }

  // Return Not a Number.
  // Consider instead using MathLimits<double>::kNaN directly.
  static double NaN() { return MathLimits<double>::kNaN; }

  // the sine cardinal function
  static double Sinc(double x) {
    if (fabs(x) < 1E-8) return 1.0;
    return sin(x) / x;
  }

  // Returns an approximation An for the n-th element of the harmonic
  // serices Hn = 1 + ... + 1/n.  Sets error e such that |An-Hn| < e.
  static double Harmonic(int64 n, double *e);

  // Returns Stirling's Approximation for log(n!) which has an error
  // of at worst 1/(1260*n^5).
  static double Stirling(double n);

  // Returns the log of the binomial coefficient C(n, k), known in the
  // vernacular as "N choose K".  Why log?  Because the integer number
  // for non-trivial N and K would overflow.
  // Note that if k > 15, this uses Stirling's approximation of log(n!).
  // The relative error is about 1/(1260*k^5) (which is 7.6e-10 when k=16).
  static double LogCombinations(int n, int k);

  // Rounds "f" to the nearest float which has its last "bits" bits of
  // the mantissa set to zero.  This rounding will introduce a
  // fractional error of at most 2^(bits - 24).  Useful for values
  // stored in compressed files, when super-accurate numbers aren't
  // needed and the random-looking low-order bits foil compressors.
  // This routine should be really fast when inlined with "bits" set
  // to a constant.
  // Precondition: 1 <= bits <= 23, f != NaN
  static float RoundOffBits(const float f, const int bits) {
    const int32 f_rep = bit_cast<int32>(f);

    // Set low-order "bits" bits to zero.
    int32 g_rep = f_rep & ~((1 << bits) - 1);

    // Round mantissa up if we need to.  Note that we do round-to-even,
    // a.k.a. round-up-if-odd.
    const int32 lowbits = f_rep & ((1 << bits) - 1);
    if (lowbits > (1 << (bits - 1)) ||
        (lowbits == (1 << (bits - 1)) && (f_rep & (1 << bits)))) {
      g_rep += (1 << bits);
      // NOTE: overflow does a really nice thing here - if all the
      // rest of the mantissa bits are 1, the carry carries over into
      // the exponent and increments it by 1, which is exactly what we
      // want.  It even gets to +/-INF properly.
    }
    return bit_cast<float>(g_rep);
  }
  // Same, but for doubles.  1 <= bits <= 52, error at most 2^(bits - 53).
  static double RoundOffBits(const double f, const int bits) {
    const int64 f_rep = bit_cast<int64>(f);
    int64 g_rep = f_rep & ~((1LL << bits) - 1);
    const int64 lowbits = f_rep & ((1LL << bits) - 1);
    if (lowbits > (1LL << (bits - 1)) ||
        (lowbits == (1LL << (bits - 1)) && (f_rep & (1LL << bits)))) {
      g_rep += (1LL << bits);
    }
    return bit_cast<double>(g_rep);
  }

  // Largest of two values.
  // Works correctly for special floating point values.
  // Note: 0.0 and -0.0 are not differentiated by Max (Max(0.0, -0.0) is -0.0),
  // which should be OK because, although they (can) have different
  // bit representation, they are observably the same when examined
  // with arithmetic and (in)equality operators.
  template<typename T>
  static T Max(const T x, const T y) {
    return MathLimits<T>::IsNaN(x) || x > y ? x : y;
  }

  // Smallest of two values
  // Works correctly for special floating point values.
  // Note: 0.0 and -0.0 are not differentiated by Min (Min(-0.0, 0.0) is 0.0),
  // which should be OK: see the comment for Max above.
  template<typename T>
  static T Min(const T x, const T y) {
    return MathLimits<T>::IsNaN(x) || x < y ? x : y;
  }

  // Absolute value of x
  // Works correctly for unsigned types and
  // for special floating point values.
  // Note: 0.0 and -0.0 are not differentiated by Abs (Abs(0.0) is -0.0),
  // which should be OK: see the comment for Max above.
  template<typename T>
  static T Abs(const T x) {
    return x > 0 ? x : -x;
  }

  // The sign of x
  // (works for unsigned types and special floating point values as well):
  //   -1 if x < 0,
  //   +1 if x > 0,
  //    0 if x = 0.
  //  nan if x is nan.
  template<typename T>
  static T Sign(const T x) {
    return MathLimits<T>::IsNaN(x) ? x : (x == 0 ? 0 : (x > 0 ? 1 : -1));
  }

  // Returns the square of x
  template <typename T>
  static T Square(const T x) {
    return x * x;
  }

  // Absolute value of the difference between two numbers.
  // Works correctly for signed types and special floating point values.
  template<typename T>
  static typename MathLimits<T>::UnsignedType AbsDiff(const T x, const T y) {
    return x > y ? x - y : y - x;
  }

  // CAVEAT: Floating point computation instability for x86 CPUs
  // can frequently stem from the difference of when floating point values
  // are transferred from registers to memory and back,
  // which can depend simply on the level of optimization.
  // The reason is that the registers use a higher-precision representation.
  // Hence, instead of relying on approximate floating point equality below
  // it might be better to reorganize the code with volatile modifiers
  // for the floating point variables so as to control when
  // the loss of precision occurs.

  // If two (usually floating point) numbers are within a certain
  // absolute margin of error.
  // NOTE: this "misbehaves" is one is trying to capture provisons for errors
  // that are relative, i.e. larger if the numbers involved are larger.
  // Consider using WithinFraction or WithinFractionOrMargin below.
  //
  // This and other Within* NearBy* functions below
  // work correctly for signed types and special floating point values.
  template<typename T>
  static bool WithinMargin(const T x, const T y, const T margin) {
    DCHECK_GE(margin, 0);
    // this is a little faster than x <= y + margin  &&  x >= y - margin
    return AbsDiff(x, y) <= margin;
  }

  // If two (usually floating point) numbers are within a certain
  // fraction of their magnitude.
  // CAVEAT: Although this works well if computation errors are relative
  // both for large magnitude numbers > 1 and for small magnitude numbers < 1,
  // zero is never within a fraction of any
  // non-zero finite number (fraction is required to be < 1).
  template<typename T>
  static bool WithinFraction(const T x, const T y, const T fraction);

  // If two (usually floating point) numbers are within a certain
  // fraction of their magnitude or within a certain absolute margin of error.
  // This is the same as the following but faster:
  //   WithinFraction(x, y, fraction)  ||  WithinMargin(x, y, margin)
  // E.g. WithinFraction(0.0, 1e-10, 1e-5) is false but
  //      WithinFractionOrMargin(0.0, 1e-10, 1e-5, 1e-5) is true.
  template<typename T>
  static bool WithinFractionOrMargin(const T x, const T y,
                                     const T fraction, const T margin);

  // NearBy* functions below are geared as replacements for CHECK_EQ()
  // over floating-point numbers.

  // Same as WithinMargin(x, y, MathLimits<T>::kStdError)
  // Works as == for integer types.
  template<typename T>
  static bool NearByMargin(const T x, const T y) {
    return AbsDiff(x, y) <= MathLimits<T>::kStdError;
  }

  // Same as WithinFraction(x, y, MathLimits<T>::kStdError)
  // Works as == for integer types.
  template<typename T>
  static bool NearByFraction(const T x, const T y) {
    return WithinFraction(x, y, MathLimits<T>::kStdError);
  }

  // Same as WithinFractionOrMargin(x, y, MathLimits<T>::kStdError,
  //                                      MathLimits<T>::kStdError)
  // Works as == for integer types.
  template<typename T>
  static bool NearByFractionOrMargin(const T x, const T y) {
    return WithinFractionOrMargin(x, y, MathLimits<T>::kStdError,
                                        MathLimits<T>::kStdError);
  }

  // Tests whether two values are close enough to each other to be considered
  // equal. This function is intended to be used mainly as a replacement for
  // equality tests of floating point values in CHECK()s, and as a replacement
  // for equality comparison against golden files. It is the same as == for
  // integer types. The purpose of AlmostEquals() is to avoid false positive
  // error reports in unit tests and regression tests due to minute differences
  // in floating point arithmetic (for example, due to a different compiler).
  //
  // We cannot use simple equality to compare floating point values
  // because floating point expressions often accumulate inaccuracies, and
  // new compilers may introduce further variations in the values.
  //
  // Two values x and y are considered "almost equals" if:
  // (a) Both values are very close to zero: x and y are in the range
  //     [-standard_error, standard_error]
  //     Normal calculations producing these values are likely to be dealing
  //     with meaningless residual values.
  // -or-
  // (b) The difference between the values is small:
  //     abs(x - y) <= standard_error
  // -or-
  // (c) The values are finite and the relative difference between them is
  //     small:
  //     abs (x - y) <= standard_error * max(abs(x), abs(y))
  // -or-
  // (d) The values are both positive infinity or both negative infinity.
  //
  // Cases (b) and (c) are the same as MathUtils::NearByFractionOrMargin(x, y),
  // for finite values of x and y.
  //
  // standard_error is the corresponding MathLimits<T>::kStdError constant.
  // It is equivalent to 5 bits of mantissa error. See
  // google3/util/math/mathlimits.cc.
  //
  // Caveat:
  // AlmostEquals() is not appropriate for checking long sequences of
  // operations where errors may cascade (like extended sums or matrix
  // computations), or where significant cancellation may occur
  // (e.g., the expression (x+y)-x, where x is much larger than y).
  // Both cases may produce errors in excess of standard_error.
  // In any case, you should not test the results of calculations which have
  // not been vetted for possible cancellation errors and the like.
  template<typename T>
  static bool AlmostEquals(const T x, const T y) {
    if (x == y)  // Covers +inf and -inf, and is a shortcut for finite values.
      return true;
    if (!MathLimits<T>::IsFinite(x) || !MathLimits<T>::IsFinite(y))
      return false;

    if (MathUtil::Abs<T>(x) <= MathLimits<T>::kStdError &&
        MathUtil::Abs<T>(y) <= MathLimits<T>::kStdError)
      return true;

    return MathUtil::NearByFractionOrMargin<T>(x, y);
  }

  // Returns the clamped value to be between low and high inclusively.
  template<typename T>
  static const T& Clamp(const T& low, const T& high, const T& value) {
    return std::max(low, std::min(value, high));
  }

  // Clamps value to be between min and max inclusively.
  template<typename T>
  static void ClampValue(const T& low, const T& high, T* value) {
    *value = Clamp(low, high, *value);
  }
};

// ========================================================================= //

#if (defined __i386__ || defined __x86_64__) && defined __GNUC__

// We define template specializations of Round() to get the more efficient
// Intel versions when possible.  Note that gcc does not currently support
// partial specialization of templatized functions.
template<>
inline int32 MathUtil::Round<int32, double>(double x) {
  return FastIntRound(x);
}

template<>
inline int32 MathUtil::Round<int32, float>(float x) {
  return FastIntRound(x);
}

template<>
inline int64 MathUtil::Round<int64, double>(double x) {
  return FastInt64Round(x);
}

template<>
inline int64 MathUtil::Round<int64, float>(float x) {
  return FastInt64Round(x);
}

#endif

template<typename T>
bool MathUtil::WithinFraction(const T x, const T y, const T fraction) {
  // not just "0 <= fraction" to fool the compiler for unsigned types
  DCHECK((0 < fraction || 0 == fraction)  &&  fraction < 1);

  // Template specialization will convert the if() condition to a constant,
  // which will cause the compiler to generate code for either the "if" part
  // or the "then" part.  In this way we avoid a compiler warning
  // about a potential integer overflow in crosstool v12 (gcc 4.3.1).
  if (MathLimits<T>::kIsInteger) {
    return x == y;
  } else {
    // IsFinite checks are to make kPosInf and kNegInf not within fraction
    return (MathLimits<T>::IsFinite(x) || MathLimits<T>::IsFinite(y)) &&
           (AbsDiff(x, y) <= fraction * Max(Abs(x), Abs(y)));
  }
}

template<typename T>
bool MathUtil::WithinFractionOrMargin(const T x, const T y,
                                      const T fraction, const T margin) {
  // not just "0 <= fraction" to fool the compiler for unsigned types
  DCHECK((0 < fraction || 0 == fraction)  &&  fraction < 1  &&  margin >= 0);

  // Template specialization will convert the if() condition to a constant,
  // which will cause the compiler to generate code for either the "if" part
  // or the "then" part.  In this way we avoid a compiler warning
  // about a potential integer overflow in crosstool v12 (gcc 4.3.1).
  if (MathLimits<T>::kIsInteger) {
    return x == y;
  } else {
    // IsFinite checks are to make kPosInf and kNegInf not within fraction
    return (MathLimits<T>::IsFinite(x) || MathLimits<T>::IsFinite(y)) &&
           (AbsDiff(x, y) <= Max(margin, fraction * Max(Abs(x), Abs(y))));
  }
}

#endif  // UTIL_MATH_MATHUTIL_H__
