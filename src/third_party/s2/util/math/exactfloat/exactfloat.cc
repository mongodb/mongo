// Copyright 2009 Google Inc. All Rights Reserved.

#include "util/math/exactfloat/exactfloat.h"
#include <cstring>

#include <math.h>
#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <limits>
using std::numeric_limits;

#include "base/integral_types.h"
#include "base/logging.h"
#include "openssl/crypto.h"

// Define storage for constants.
const int ExactFloat::kMinExp;
const int ExactFloat::kMaxExp;
const int ExactFloat::kMaxPrec;
const int32 ExactFloat::kExpNaN;
const int32 ExactFloat::kExpInfinity;
const int32 ExactFloat::kExpZero;
const int ExactFloat::kDoubleMantissaBits;

// To simplify the overflow/underflow logic, we limit the exponent and
// precision range so that (2 * bn_exp_) does not overflow an "int".  We take
// advantage of this, for example, by only checking for overflow/underflow
// *after* multiplying two numbers.
COMPILE_ASSERT(
    ExactFloat::kMaxExp <= INT_MAX / 2 &&
    ExactFloat::kMinExp - ExactFloat::kMaxPrec >= INT_MIN / 2,
    exactfloat_exponent_might_overflow);

// We define a few simple extensions to the BIGNUM interface.  In some cases
// these depend on BIGNUM internal fields, so they might require tweaking if
// the BIGNUM implementation changes significantly.

// Set a BIGNUM to the given unsigned 64-bit value.
inline static void BN_ext_set_uint64(BIGNUM* bn, uint64 v) {
#if BN_BITS2 == 64
  CHECK(BN_set_word(bn, v));
#else
  COMPILE_ASSERT(BN_BITS2 == 32, at_least_32_bit_openssl_build_needed);
  CHECK(BN_set_word(bn, static_cast<uint32>(v >> 32)));
  CHECK(BN_lshift(bn, bn, 32));
  CHECK(BN_add_word(bn, static_cast<uint32>(v)));
#endif
}

// Return the absolute value of a BIGNUM as a 64-bit unsigned integer.
// Requires that BIGNUM fits into 64 bits.
inline static uint64 BN_ext_get_uint64(const BIGNUM* bn) {
  DCHECK_LE(BN_num_bytes(bn), sizeof(uint64));
#if BN_BITS2 == 64
  return BN_get_word(bn);
#else
  COMPILE_ASSERT(BN_BITS2 == 32, at_least_32_bit_openssl_build_needed);
  if (bn->top == 0) return 0;
  if (bn->top == 1) return BN_get_word(bn);
  DCHECK_EQ(bn->top, 2);
  return (static_cast<uint64>(bn->d[1]) << 32) + bn->d[0];
#endif
}

// Count the number of low-order zero bits in the given BIGNUM (ignoring its
// sign).  Returns 0 if the argument is zero.
static int BN_ext_count_low_zero_bits(const BIGNUM* bn) {
  int count = 0;
  for (int i = 0; i < bn->top; ++i) {
    BN_ULONG w = bn->d[i];
    if (w == 0) {
      count += 8 * sizeof(BN_ULONG);
    } else {
      for (; (w & 1) == 0; w >>= 1) {
        ++count;
      }
      break;
    }
  }
  return count;
}

ExactFloat::ExactFloat(double v) {
  BN_init(&bn_);
  sign_ = signbit(v) ? -1 : 1;
  if (isnan(v)) {
    set_nan();
  } else if (isinf(v)) {
    set_inf(sign_);
  } else {
    // The following code is much simpler than messing about with bit masks,
    // has the advantage of handling denormalized numbers and zero correctly,
    // and is actually quite efficient (at least compared to the rest of this
    // code).  "f" is a fraction in the range [0.5, 1), so if we shift it left
    // by the number of mantissa bits in a double (53, including the leading
    // "1") then the result is always an integer.
    int exp;
    double f = frexp(fabs(v), &exp);
    uint64 m = static_cast<uint64>(ldexp(f, kDoubleMantissaBits));
    BN_ext_set_uint64(&bn_, m);
    bn_exp_ = exp - kDoubleMantissaBits;
    Canonicalize();
  }
}

ExactFloat::ExactFloat(int v) {
  BN_init(&bn_);
  sign_ = (v >= 0) ? 1 : -1;
  // Note that this works even for INT_MIN because the parameter type for
  // BN_set_word() is unsigned.
  CHECK(BN_set_word(&bn_, abs(v)));
  bn_exp_ = 0;
  Canonicalize();
}

ExactFloat::ExactFloat(const ExactFloat& b)
    : sign_(b.sign_),
      bn_exp_(b.bn_exp_) {
  BN_init(&bn_);
  BN_copy(&bn_, &b.bn_);
}

ExactFloat ExactFloat::SignedZero(int sign) {
  ExactFloat r;
  r.set_zero(sign);
  return r;
}

ExactFloat ExactFloat::Infinity(int sign) {
  ExactFloat r;
  r.set_inf(sign);
  return r;
}

ExactFloat ExactFloat::NaN() {
  ExactFloat r;
  r.set_nan();
  return r;
}

int ExactFloat::prec() const {
  return BN_num_bits(&bn_);
}

int ExactFloat::exp() const {
  DCHECK(is_normal());
  return bn_exp_ + BN_num_bits(&bn_);
}

void ExactFloat::set_zero(int sign) {
  sign_ = sign;
  bn_exp_ = kExpZero;
  if (!BN_is_zero(&bn_)) BN_zero(&bn_);
}

void ExactFloat::set_inf(int sign) {
  sign_ = sign;
  bn_exp_ = kExpInfinity;
  if (!BN_is_zero(&bn_)) BN_zero(&bn_);
}

void ExactFloat::set_nan() {
  sign_ = 1;
  bn_exp_ = kExpNaN;
  if (!BN_is_zero(&bn_)) BN_zero(&bn_);
}

double ExactFloat::ToDouble() const {
  // If the mantissa has too many bits, we need to round it.
  if (prec() <= kDoubleMantissaBits) {
    return ToDoubleHelper();
  } else {
    ExactFloat r = RoundToMaxPrec(kDoubleMantissaBits, kRoundTiesToEven);
    return r.ToDoubleHelper();
  }
}

double ExactFloat::ToDoubleHelper() const {
  DCHECK_LE(BN_num_bits(&bn_), kDoubleMantissaBits);
  if (!is_normal()) {
    if (is_zero()) return copysign(0, sign_);
    if (is_inf()) return copysign(INFINITY, sign_);
    return copysign(NAN, sign_);
  }
  uint64 d_mantissa = BN_ext_get_uint64(&bn_);
  // We rely on ldexp() to handle overflow and underflow.  (It will return a
  // signed zero or infinity if the result is too small or too large.)
  return sign_ * ldexp(static_cast<double>(d_mantissa), bn_exp_);
}

ExactFloat ExactFloat::RoundToMaxPrec(int max_prec, RoundingMode mode) const {
  // The "kRoundTiesToEven" mode requires at least 2 bits of precision
  // (otherwise both adjacent representable values may be odd).
  DCHECK_GE(max_prec, 2);
  DCHECK_LE(max_prec, kMaxPrec);

  // The following test also catches zero, infinity, and NaN.
  int shift = prec() - max_prec;
  if (shift <= 0) return *this;

  // Round by removing the appropriate number of bits from the mantissa.  Note
  // that if the value is rounded up to a power of 2, the high-order bit
  // position may increase, but in that case Canonicalize() will remove at
  // least one zero bit and so the output will still have prec() <= max_prec.
  return RoundToPowerOf2(bn_exp_ + shift, mode);
}

ExactFloat ExactFloat::RoundToPowerOf2(int bit_exp, RoundingMode mode) const {
  DCHECK_GE(bit_exp, kMinExp - kMaxPrec);
  DCHECK_LE(bit_exp, kMaxExp);

  // If the exponent is already large enough, or the value is zero, infinity,
  // or NaN, then there is nothing to do.
  int shift = bit_exp - bn_exp_;
  if (shift <= 0) return *this;
  DCHECK(is_normal());

  // Convert rounding up/down to toward/away from zero, so that we don't need
  // to consider the sign of the number from this point onward.
  if (mode == kRoundTowardPositive) {
    mode = (sign_ > 0) ? kRoundAwayFromZero : kRoundTowardZero;
  } else if (mode == kRoundTowardNegative) {
    mode = (sign_ > 0) ? kRoundTowardZero : kRoundAwayFromZero;
  }

  // Rounding consists of right-shifting the mantissa by "shift", and then
  // possibly incrementing the result (depending on the rounding mode, the
  // bits that were discarded, and sometimes the lowest kept bit).  The
  // following code figures out whether we need to increment.
  ExactFloat r;
  bool increment = false;
  if (mode == kRoundTowardZero) {
    // Never increment.
  } else if (mode == kRoundTiesAwayFromZero) {
    // Increment if the highest discarded bit is 1.
    if (BN_is_bit_set(&bn_, shift - 1))
      increment = true;
  } else if (mode == kRoundAwayFromZero) {
    // Increment unless all discarded bits are zero.
    if (BN_ext_count_low_zero_bits(&bn_) < shift)
      increment = true;
  } else {
    DCHECK_EQ(mode, kRoundTiesToEven);
    // Let "w/xyz" denote a mantissa where "w" is the lowest kept bit and
    // "xyz" are the discarded bits.  Then using regexp notation:
    //    ./0.*       ->    Don't increment (fraction < 1/2)
    //    0/10*       ->    Don't increment (fraction = 1/2, kept part even)
    //    1/10*       ->    Increment (fraction = 1/2, kept part odd)
    //    ./1.*1.*    ->    Increment (fraction > 1/2)
    if (BN_is_bit_set(&bn_, shift - 1) &&
        ((BN_is_bit_set(&bn_, shift) ||
          BN_ext_count_low_zero_bits(&bn_) < shift - 1))) {
      increment = true;
    }
  }
  r.bn_exp_ = bn_exp_ + shift;
  CHECK(BN_rshift(&r.bn_, &bn_, shift));
  if (increment) {
    CHECK(BN_add_word(&r.bn_, 1));
  }
  r.sign_ = sign_;
  r.Canonicalize();
  return r;
}

int ExactFloat::NumSignificantDigitsForPrec(int prec) {
  // The simplest bound is
  //
  //    d <= 1 + ceil(prec * log10(2))
  //
  // The following bound is tighter by 0.5 digits on average, but requires
  // the exponent to be known as well:
  //
  //    d <= ceil(exp * log10(2)) - floor((exp - prec) * log10(2))
  //
  // Since either of these bounds can be too large by 0, 1, or 2 digits, we
  // stick with the simpler first bound.
  return static_cast<int>(1 + ceil(prec * (M_LN2 / M_LN10)));
}

// Numbers are always formatted with at least this many significant digits.
// This prevents small integers from being formatted in exponential notation
// (e.g. 1024 formatted as 1e+03), and also avoids the confusion of having
// supposedly "high precision" numbers formatted with just 1 or 2 digits
// (e.g. 1/512 == 0.001953125 formatted as 0.002).
static const int kMinSignificantDigits = 10;

string ExactFloat::ToString() const {
  int max_digits = max(kMinSignificantDigits,
                       NumSignificantDigitsForPrec(prec()));
  return ToStringWithMaxDigits(max_digits);
}

string ExactFloat::ToStringWithMaxDigits(int max_digits) const {
  DCHECK_GT(max_digits, 0);
  if (!is_normal()) {
    if (is_nan()) return "nan";
    if (is_zero()) return (sign_ < 0) ? "-0" : "0";
    return (sign_ < 0) ? "-inf" : "inf";
  }
  string digits;
  int exp10 = GetDecimalDigits(max_digits, &digits);
  string str;
  if (sign_ < 0) str.push_back('-');

  // We use the standard '%g' formatting rules.  If the exponent is less than
  // -4 or greater than or equal to the requested precision (i.e., max_digits)
  // then we use exponential notation.
  //
  // But since "exp10" is the base-10 exponent corresponding to a mantissa in
  // the range [0.1, 1), whereas the '%g' rules assume a mantissa in the range
  // [1.0, 10), we need to adjust these parameters by 1.
  if (exp10 <= -4 || exp10 > max_digits) {
    // Use exponential format.
    str.push_back(digits[0]);
    if (digits.size() > 1) {
      str.push_back('.');
      str.append(digits.begin() + 1, digits.end());
    }
    char exp_buf[20];
    sprintf(exp_buf, "e%+02d", exp10 - 1);
    str += exp_buf;
  } else {
    // Use fixed format.  We split this into two cases depending on whether
    // the integer portion is non-zero or not.
    if (exp10 > 0) {
      if ((size_t)exp10 >= digits.size()) {
        str += digits;
        for (int i = exp10 - digits.size(); i > 0; --i) {
          str.push_back('0');
        }
      } else {
        str.append(digits.begin(), digits.begin() + exp10);
        str.push_back('.');
        str.append(digits.begin() + exp10, digits.end());
      }
    } else {
      str += "0.";
      for (int i = exp10; i < 0; ++i) {
        str.push_back('0');
      }
      str += digits;
    }
  }
  return str;
}

// Increment an unsigned integer represented as a string of ASCII digits.
static void IncrementDecimalDigits(string* digits) {
  string::iterator pos = digits->end();
  while (--pos >= digits->begin()) {
    if (*pos < '9') { ++*pos; return; }
    *pos = '0';
  }
  digits->insert(0, "1");
}

int ExactFloat::GetDecimalDigits(int max_digits, string* digits) const {
  DCHECK(is_normal());
  // Convert the value to the form (bn * (10 ** bn_exp10)) where "bn" is a
  // positive integer (BIGNUM).
  BIGNUM* bn = BN_new();
  int bn_exp10;
  if (bn_exp_ >= 0) {
    // The easy case: bn = bn_ * (2 ** bn_exp_)), bn_exp10 = 0.
    CHECK(BN_lshift(bn, &bn_, bn_exp_));
    bn_exp10 = 0;
  } else {
    // Set bn = bn_ * (5 ** -bn_exp_) and bn_exp10 = bn_exp_.  This is
    // equivalent to the original value of (bn_ * (2 ** bn_exp_)).
    BIGNUM* power = BN_new();
    CHECK(BN_set_word(power, -bn_exp_));
    CHECK(BN_set_word(bn, 5));
    BN_CTX* ctx = BN_CTX_new();
    CHECK(BN_exp(bn, bn, power, ctx));
    CHECK(BN_mul(bn, bn, &bn_, ctx));
    BN_CTX_free(ctx);
    BN_free(power);
    bn_exp10 = bn_exp_;
  }
  // Now convert "bn" to a decimal string.
  char* all_digits = BN_bn2dec(bn);
  DCHECK(all_digits != NULL);
  BN_free(bn);
  // Check whether we have too many digits and round if necessary.
  int num_digits = strlen(all_digits);
  if (num_digits <= max_digits) {
    *digits = all_digits;
  } else {
    digits->assign(all_digits, max_digits);
    // Standard "printf" formatting rounds ties to an even number.  This means
    // that we round up (away from zero) if highest discarded digit is '5' or
    // more, unless all other discarded digits are zero in which case we round
    // up only if the lowest kept digit is odd.
    if (all_digits[max_digits] >= '5' &&
        ((all_digits[max_digits-1] & 1) == 1 ||
         strpbrk(all_digits + max_digits + 1, "123456789") != NULL)) {
      // This can increase the number of digits by 1, but in that case at
      // least one trailing zero will be stripped off below.
      IncrementDecimalDigits(digits);
    }
    // Adjust the base-10 exponent to reflect the digits we have removed.
    bn_exp10 += num_digits - max_digits;
  }
  OPENSSL_free(all_digits);

  // Now strip any trailing zeros.
  DCHECK_NE((*digits)[0], '0');
  string::iterator pos = digits->end();
  while (pos[-1] == '0') --pos;
  if (pos < digits->end()) {
    bn_exp10 += digits->end() - pos;
    digits->erase(pos, digits->end());
  }
  DCHECK_LE(digits->size(), max_digits);

  // Finally, we adjust the base-10 exponent so that the mantissa is a
  // fraction in the range [0.1, 1) rather than an integer.
  return bn_exp10 + digits->size();
}

string ExactFloat::ToUniqueString() const {
  char prec_buf[20];
  sprintf(prec_buf, "<%d>", prec());
  return ToString() + prec_buf;
}

ExactFloat& ExactFloat::operator=(const ExactFloat& b) {
  if (this != &b) {
    sign_ = b.sign_;
    bn_exp_ = b.bn_exp_;
    BN_copy(&bn_, &b.bn_);
  }
  return *this;
}

ExactFloat ExactFloat::operator-() const {
  return CopyWithSign(-sign_);
}

ExactFloat operator+(const ExactFloat& a, const ExactFloat& b) {
  return ExactFloat::SignedSum(a.sign_, &a, b.sign_, &b);
}

ExactFloat operator-(const ExactFloat& a, const ExactFloat& b) {
  return ExactFloat::SignedSum(a.sign_, &a, -b.sign_, &b);
}

ExactFloat ExactFloat::SignedSum(int a_sign, const ExactFloat* a,
                                 int b_sign, const ExactFloat* b) {
  if (!a->is_normal() || !b->is_normal()) {
    // Handle zero, infinity, and NaN according to IEEE 754-2008.
    if (a->is_nan()) return *a;
    if (b->is_nan()) return *b;
    if (a->is_inf()) {
      // Adding two infinities with opposite sign yields NaN.
      if (b->is_inf() && a_sign != b_sign) return NaN();
      return Infinity(a_sign);
    }
    if (b->is_inf()) return Infinity(b_sign);
    if (a->is_zero()) {
      if (!b->is_zero()) return b->CopyWithSign(b_sign);
      // Adding two zeros with the same sign preserves the sign.
      if (a_sign == b_sign) return SignedZero(a_sign);
      // Adding two zeros of opposite sign produces +0.
      return SignedZero(+1);
    }
    DCHECK(b->is_zero());
    return a->CopyWithSign(a_sign);
  }
  // Swap the numbers if necessary so that "a" has the larger bn_exp_.
  if (a->bn_exp_ < b->bn_exp_) {
    swap(a_sign, b_sign);
    swap(a, b);
  }
  // Shift "a" if necessary so that both values have the same bn_exp_.
  ExactFloat r;
  if (a->bn_exp_ > b->bn_exp_) {
    CHECK(BN_lshift(&r.bn_, &a->bn_, a->bn_exp_ - b->bn_exp_));
    a = &r;  // The only field of "a" used below is bn_.
  }
  r.bn_exp_ = b->bn_exp_;
  if (a_sign == b_sign) {
    CHECK(BN_add(&r.bn_, &a->bn_, &b->bn_));
    r.sign_ = a_sign;
  } else {
    // Note that the BIGNUM documentation is out of date -- all methods now
    // allow the result to be the same as any input argument, so it is okay if
    // (a == &r) due to the shift above.
    CHECK(BN_sub(&r.bn_, &a->bn_, &b->bn_));
    if (BN_is_zero(&r.bn_)) {
      r.sign_ = +1;
    } else if (BN_is_negative(&r.bn_)) {
      // The magnitude of "b" was larger.
      r.sign_ = b_sign;
      BN_set_negative(&r.bn_, false);
    } else {
      // They were equal, or the magnitude of "a" was larger.
      r.sign_ = a_sign;
    }
  }
  r.Canonicalize();
  return r;
}

void ExactFloat::Canonicalize() {
  if (!is_normal()) return;

  // Underflow/overflow occurs if exp() is not in [kMinExp, kMaxExp].
  // We also convert a zero mantissa to signed zero.
  int my_exp = exp();
  if (my_exp < kMinExp || BN_is_zero(&bn_)) {
    set_zero(sign_);
  } else if (my_exp > kMaxExp) {
    set_inf(sign_);
  } else if (!BN_is_odd(&bn_)) {
    // Remove any low-order zero bits from the mantissa.
    DCHECK(!BN_is_zero(&bn_));
    int shift = BN_ext_count_low_zero_bits(&bn_);
    if (shift > 0) {
      CHECK(BN_rshift(&bn_, &bn_, shift));
      bn_exp_ += shift;
    }
  }
  // If the mantissa has too many bits, we replace it by NaN to indicate
  // that an inexact calculation has occurred.
  if (prec() > kMaxPrec) {
    set_nan();
  }
}

ExactFloat operator*(const ExactFloat& a, const ExactFloat& b) {
  int result_sign = a.sign_ * b.sign_;
  if (!a.is_normal() || !b.is_normal()) {
    // Handle zero, infinity, and NaN according to IEEE 754-2008.
    if (a.is_nan()) return a;
    if (b.is_nan()) return b;
    if (a.is_inf()) {
      // Infinity times zero yields NaN.
      if (b.is_zero()) return ExactFloat::NaN();
      return ExactFloat::Infinity(result_sign);
    }
    if (b.is_inf()) {
      if (a.is_zero()) return ExactFloat::NaN();
      return ExactFloat::Infinity(result_sign);
    }
    DCHECK(a.is_zero() || b.is_zero());
    return ExactFloat::SignedZero(result_sign);
  }
  ExactFloat r;
  r.sign_ = result_sign;
  r.bn_exp_ = a.bn_exp_ + b.bn_exp_;
  BN_CTX* ctx = BN_CTX_new();
  CHECK(BN_mul(&r.bn_, &a.bn_, &b.bn_, ctx));
  BN_CTX_free(ctx);
  r.Canonicalize();
  return r;
}

bool operator==(const ExactFloat& a, const ExactFloat& b) {
  // NaN is not equal to anything, not even itself.
  if (a.is_nan() || b.is_nan()) return false;

  // Since Canonicalize() strips low-order zero bits, all other cases
  // (including non-normal values) require bn_exp_ to be equal.
  if (a.bn_exp_ != b.bn_exp_) return false;

  // Positive and negative zero are equal.
  if (a.is_zero() && b.is_zero()) return true;

  // Otherwise, the signs and mantissas must match.  Note that non-normal
  // values such as infinity have a mantissa of zero.
  return a.sign_ == b.sign_ && BN_ucmp(&a.bn_, &b.bn_) == 0;
}

int ExactFloat::ScaleAndCompare(const ExactFloat& b) const {
  DCHECK(is_normal() && b.is_normal() && bn_exp_ >= b.bn_exp_);
  ExactFloat tmp = *this;
  CHECK(BN_lshift(&tmp.bn_, &tmp.bn_, bn_exp_ - b.bn_exp_));
  return BN_ucmp(&tmp.bn_, &b.bn_);
}

bool ExactFloat::UnsignedLess(const ExactFloat& b) const {
  // Handle the zero/infinity cases (NaN has already been done).
  if (is_inf() || b.is_zero()) return false;
  if (is_zero() || b.is_inf()) return true;
  // If the high-order bit positions differ, we are done.
  int cmp = exp() - b.exp();
  if (cmp != 0) return cmp < 0;
  // Otherwise shift one of the two values so that they both have the same
  // bn_exp_ and then compare the mantissas.
  return (bn_exp_ >= b.bn_exp_ ?
          ScaleAndCompare(b) < 0 : b.ScaleAndCompare(*this) > 0);
}

bool operator<(const ExactFloat& a, const ExactFloat& b) {
  // NaN is unordered compared to everything, including itself.
  if (a.is_nan() || b.is_nan()) return false;
  // Positive and negative zero are equal.
  if (a.is_zero() && b.is_zero()) return false;
  // Otherwise, anything negative is less than anything positive.
  if (a.sign_ != b.sign_) return a.sign_ < b.sign_;
  // Now we just compare absolute values.
  return (a.sign_ > 0) ? a.UnsignedLess(b) : b.UnsignedLess(a);
}

ExactFloat fabs(const ExactFloat& a) {
  return a.CopyWithSign(+1);
}

ExactFloat fmax(const ExactFloat& a, const ExactFloat& b) {
  // If one argument is NaN, return the other argument.
  if (a.is_nan()) return b;
  if (b.is_nan()) return a;
  // Not required by IEEE 754, but we prefer +0 over -0.
  if (a.sign_ != b.sign_) {
    return (a.sign_ < b.sign_) ? b : a;
  }
  return (a < b) ? b : a;
}

ExactFloat fmin(const ExactFloat& a, const ExactFloat& b) {
  // If one argument is NaN, return the other argument.
  if (a.is_nan()) return b;
  if (b.is_nan()) return a;
  // Not required by IEEE 754, but we prefer -0 over +0.
  if (a.sign_ != b.sign_) {
    return (a.sign_ < b.sign_) ? a : b;
  }
  return (a < b) ? a : b;
}

ExactFloat fdim(const ExactFloat& a, const ExactFloat& b) {
  // This formulation has the correct behavior for NaNs.
  return (a <= b) ? 0 : (a - b);
}

ExactFloat ceil(const ExactFloat& a) {
  return a.RoundToPowerOf2(0, ExactFloat::kRoundTowardPositive);
}

ExactFloat floor(const ExactFloat& a) {
  return a.RoundToPowerOf2(0, ExactFloat::kRoundTowardNegative);
}

ExactFloat trunc(const ExactFloat& a) {
  return a.RoundToPowerOf2(0, ExactFloat::kRoundTowardZero);
}

ExactFloat round(const ExactFloat& a) {
  return a.RoundToPowerOf2(0, ExactFloat::kRoundTiesAwayFromZero);
}

ExactFloat rint(const ExactFloat& a) {
  return a.RoundToPowerOf2(0, ExactFloat::kRoundTiesToEven);
}

template <class T>
T ExactFloat::ToInteger(RoundingMode mode) const {
  COMPILE_ASSERT(sizeof(T) <= sizeof(uint64), max_64_bits_supported);
  COMPILE_ASSERT(numeric_limits<T>::is_signed, only_signed_types_supported);
  const int64 kMinValue = numeric_limits<T>::min();
  const int64 kMaxValue = numeric_limits<T>::max();

  ExactFloat r = RoundToPowerOf2(0, mode);
  if (r.is_nan()) return kMaxValue;
  if (r.is_zero()) return 0;
  if (!r.is_inf()) {
    // If the unsigned value has more than 63 bits it is always clamped.
    if (r.exp() < 64) {
      int64 value = BN_ext_get_uint64(&r.bn_) << r.bn_exp_;
      if (r.sign_ < 0) value = -value;
      return max(kMinValue, min(kMaxValue, value));
    }
  }
  return (r.sign_ < 0) ? kMinValue : kMaxValue;
}

long lrint(const ExactFloat& a) {
  return a.ToInteger<long>(ExactFloat::kRoundTiesToEven);
}

long long llrint(const ExactFloat& a) {
  return a.ToInteger<long long>(ExactFloat::kRoundTiesToEven);
}

long lround(const ExactFloat& a) {
  return a.ToInteger<long>(ExactFloat::kRoundTiesAwayFromZero);
}

long long llround(const ExactFloat& a) {
  return a.ToInteger<long long>(ExactFloat::kRoundTiesAwayFromZero);
}

ExactFloat copysign(const ExactFloat& a, const ExactFloat& b) {
  return a.CopyWithSign(b.sign_);
}

ExactFloat frexp(const ExactFloat& a, int* exp) {
  if (!a.is_normal()) {
    // If a == 0, exp should be zero.  If a.is_inf() or a.is_nan(), exp is not
    // defined but the glibc implementation returns zero.
    *exp = 0;
    return a;
  }
  *exp = a.exp();
  return ldexp(a, -a.exp());
}

ExactFloat ldexp(const ExactFloat& a, int exp) {
  if (!a.is_normal()) return a;

  // To prevent integer overflow, we first clamp "exp" so that
  // (kMinExp - 1) <= (a_exp + exp) <= (kMaxExp + 1).
  int a_exp = a.exp();
  exp = min(ExactFloat::kMaxExp + 1 - a_exp,
            max(ExactFloat::kMinExp - 1 + a_exp, exp));

  // Now modify the exponent and check for overflow/underflow.
  ExactFloat r = a;
  r.bn_exp_ += exp;
  r.Canonicalize();
  return r;
}

int ilogb(const ExactFloat& a) {
  if (a.is_zero()) return FP_ILOGB0;
  if (a.is_inf()) return INT_MAX;
  if (a.is_nan()) return FP_ILOGBNAN;
  // a.exp() assumes the significand is in the range [0.5, 1).
  return a.exp() - 1;
}

ExactFloat logb(const ExactFloat& a) {
  if (a.is_zero()) return ExactFloat::Infinity(-1);
  if (a.is_inf()) return ExactFloat::Infinity(+1);  // Even if a < 0.
  if (a.is_nan()) return a;
  // exp() assumes the significand is in the range [0.5,1).
  return ExactFloat(a.exp() - 1);
}

ExactFloat ExactFloat::Unimplemented() {
  LOG(FATAL) << "Unimplemented ExactFloat method called";
  return NaN();
}
