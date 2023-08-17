#ifndef MC_DEC128_H_INCLUDED
#define MC_DEC128_H_INCLUDED

#include <bson/bson.h>

#include <mlib/endian.h>
#include <mlib/int128.h>
#include <mlib/macros.h>

// Conditional preprocessor definition set by the usage of an intel_dfp from
// the ImportDFP.cmake script:
#ifndef MONGOCRYPT_INTELDFP
// Notify includers that Decimal128 is not available:
#define MONGOCRYPT_HAVE_DECIMAL128_SUPPORT 0

#else // With IntelDFP:
// Tell includers that Decimal128 is okay:
#define MONGOCRYPT_HAVE_DECIMAL128_SUPPORT 1

// Include the header that declares the DFP functions, which may be macros that
// expand to renamed symbols:
#include <bid_conf.h>
#include <bid_functions.h>

#include <float.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

MLIB_C_LINKAGE_BEGIN

/// Rounding controls for Decimal128 operations
typedef enum mc_dec128_rounding_mode {
    MC_DEC128_ROUND_NEAREST_EVEN = 0,
    MC_DEC128_ROUND_DOWNWARD = 1,
    MC_DEC128_ROUND_UPWARD = 2,
    MC_DEC128_ROUND_TOWARD_ZERO = 3,
    MC_DEC128_ROUND_NEAREST_AWAY = 4,
    MC_DEC128_ROUND_DEFAULT = MC_DEC128_ROUND_NEAREST_EVEN,
} mc_dec128_rounding_mode;

typedef struct mc_dec128_flagset {
    _IDEC_flags bits;
} mc_dec128_flagset;

// This alignment conditional is the same conditions used in Intel's DFP
// library, ensuring we match the ABI of the library without pulling the header
#if defined _MSC_VER
#if defined _M_IX86 && !defined __INTEL_COMPILER
#define _mcDec128Align(n)
#else
#define _mcDec128Align(n) __declspec(align(n))
#endif
#else
#if !defined HPUX_OS
#define _mcDec128Align(n) __attribute__((aligned(n)))
#else
#define _mcDec128Align(n)
#endif
#endif

typedef union _mcDec128Align(16) {
    uint64_t _words[2];
#if !defined(__INTELLISENSE__) && defined(__GNUC__) && defined(__amd64) && !defined(__APPLE__) && !defined(__clang__)
    // If supported by the compiler, emit a field that can be used to visualize
    // the value in a debugger.
    float value_ __attribute__((mode(TD)));
#endif
}

mc_dec128;

#undef _mcDec128Align

/// Expands to a dec128 constant value.
#ifdef __cplusplus
#define MC_DEC128_C(N) mc_dec128 _mcDec128Const(((N) < 0 ? -(N) : (N)), ((N) < 0 ? 1 : 0))
#else
#define MC_DEC128_C(N) _mcDec128Const(((N) < 0 ? -(N) : (N)), ((N) < 0 ? 1 : 0))
#endif

#define MC_DEC128(N) MLIB_INIT(mc_dec128) MC_DEC128_C(N)

#define _mcDec128Combination(Bits) ((uint64_t)(Bits) << (47))
#define _mcDec128ZeroExpCombo _mcDec128Combination(1 << 7 | 1 << 13 | 1 << 14)
#define _mcDec128Const(N, Negate) _mcDec128ConstFromParts(N, (_mcDec128ZeroExpCombo | ((uint64_t)(Negate) << 63)))
#define _mcDec128ConstFromParts(CoeffLow, HighWord)                                                                    \
    {                                                                                                                  \
        {                                                                                                              \
            MLIB_IS_LITTLE_ENDIAN ? (uint64_t)(CoeffLow) : (uint64_t)(HighWord),                                       \
            MLIB_IS_LITTLE_ENDIAN ? (uint64_t)(HighWord) : (uint64_t)(CoeffLow),                                       \
        },                                                                                                             \
    }

static const mc_dec128 MC_DEC128_ZERO = MC_DEC128_C(0);
static const mc_dec128 MC_DEC128_ONE = MC_DEC128_C(1);
static const mc_dec128 MC_DEC128_MINUSONE = MC_DEC128_C(-1);

/// The greatest-magnitude finite negative value representable in a Decimal128
#define MC_DEC128_LARGEST_NEGATIVE mc_dec128_from_string("-9999999999999999999999999999999999E6111")
/// The least-magnitude non-zero negative value representable in a Decimal128
#define MC_DEC128_SMALLEST_NEGATIVE mc_dec128_from_string("-1E-6176")
/// The greatest-magnitude finite positive value representable in a Decimal128
#define MC_DEC128_LARGEST_POSITIVE mc_dec128_from_string("9999999999999999999999999999999999E6111")
/// The least-magnitude non-zero positive value representable in a Decimal128
#define MC_DEC128_SMALLEST_POSITIVE mc_dec128_from_string("1E-6176")
/// The normalized zero of Decimal128
#define MC_DEC128_NORMALIZED_ZERO MC_DEC128_C(0)
/// A zero of Decimal128 with the least exponent
#define MC_DEC128_NEGATIVE_EXPONENT_ZERO mc_dec128_from_string("0E-6176")
#define _mcDec128InfCombo _mcDec128Combination(1 << 15 | 1 << 14 | 1 << 13 | 1 << 12)
#define _mcDec128QuietNaNCombo _mcDec128Combination(1 << 15 | 1 << 14 | 1 << 13 | 1 << 12 | 1 << 11)

/// Positive infinity of Decimal128
#define MC_DEC128_POSITIVE_INFINITY _mcDec128ConstFromParts(0, _mcDec128InfCombo)
/// Negative infinity of Decimal128
#define MC_DEC128_NEGATIVE_INFINITY _mcDec128ConstFromParts(0, _mcDec128InfCombo | 1ull << 63)
/// Positve quiet NaN of Decimal128
#define MC_DEC128_POSITIVE_NAN _mcDec128ConstFromParts(0, _mcDec128QuietNaNCombo)
/// Negative quiet NaN of Decimal128
#define MC_DEC128_NEGATIVE_NAN _mcDec128ConstFromParts(0, _mcDec128QuietNaNCombo | 1ull << 63)

/// Convert an mc_dec128 value to a DFP 128-bit object
static inline BID_UINT128 _mc_to_bid128(mc_dec128 d) {
    BID_UINT128 r;
    memcpy(&r, &d, sizeof d);
    return r;
}

/// Convert a DFP 128-bit object to a mc_dec128 value
static inline mc_dec128 _bid128_to_mc(BID_UINT128 d) {
    mc_dec128 r;
    memcpy(&r, &d, sizeof d);
    return r;
}

/**
 * @brief Convert a double-precision binary floating point value into the
 * nearest Decimal128 value
 *
 * @param d The number to conver
 * @param rnd The rounding mode in case the value is not exactly representable
 * @param flags Out param for exception/error flags (Optional)
 */
static inline mc_dec128 mc_dec128_from_double_ex(double d, mc_dec128_rounding_mode rnd, mc_dec128_flagset *flags) {
    mc_dec128_flagset zero_flags = {0};
    return _bid128_to_mc(binary64_to_bid128(d, rnd, flags ? &flags->bits : &zero_flags.bits));
}

/**
 * @brief Convert a double-precision binary floating point value into the
 * nearest Decimal128 value
 */
static inline mc_dec128 mc_dec128_from_double(double d) {
    return mc_dec128_from_double_ex(d, MC_DEC128_ROUND_DEFAULT, NULL);
}

/**
 * @brief Convert a string representation of a number into the nearest
 * Decimal128 value
 *
 * @param s The string to parse. MUST be null-terminated
 * @param rnd The rounding mode to use if the result is not representable
 * @param flags Out param for exception/error flags (Optional)
 */
static inline mc_dec128 mc_dec128_from_string_ex(const char *s, mc_dec128_rounding_mode rnd, mc_dec128_flagset *flags) {
    mc_dec128_flagset zero_flags = {0};
    return _bid128_to_mc(bid128_from_string((char *)s, rnd, flags ? &flags->bits : &zero_flags.bits));
}

/**
 * @brief Convert a string representation of a number into the nearest
 * Decimal128 value
 */
static inline mc_dec128 mc_dec128_from_string(const char *s) {
    return mc_dec128_from_string_ex(s, MC_DEC128_ROUND_DEFAULT, NULL);
}

/**
 * @brief A type capable of holding a string rendering of a Decimal128 in
 * engineering notation.
 */
typedef struct mc_dec128_string {
    /// The character array of the rendered value. Null-terminated
    char str[48];
} mc_dec128_string;

/**
 * @brief Render a Decimal128 value as a string (in engineering notation)
 *
 * @param d The number to represent
 * @param flags Output parameter for exception/error flags (optional)
 */
static inline mc_dec128_string mc_dec128_to_string_ex(mc_dec128 d, mc_dec128_flagset *flags) {
    mc_dec128_flagset zero_flags = {0};
    mc_dec128_string out = {{0}};
    bid128_to_string(out.str, _mc_to_bid128(d), flags ? &flags->bits : &zero_flags.bits);
    return out;
}

/**
 * @brief Render a Decimal128 value as a string (in engineering notation)
 */
static inline mc_dec128_string mc_dec128_to_string(mc_dec128 d) {
    return mc_dec128_to_string_ex(d, NULL);
}

/// Compare two dec128 numbers
#define DECL_IDF_COMPARE_1(Oper)                                                                                       \
    static inline bool mc_dec128_##Oper##_ex(mc_dec128 left, mc_dec128 right, mc_dec128_flagset *flags) {              \
        mc_dec128_flagset zero_flags = {0};                                                                            \
        return 0                                                                                                       \
            != bid128_quiet_##Oper(_mc_to_bid128(left),                                                                \
                                   _mc_to_bid128(right),                                                               \
                                   flags ? &flags->bits : &zero_flags.bits);                                           \
    }                                                                                                                  \
                                                                                                                       \
    static inline bool mc_dec128_##Oper(mc_dec128 left, mc_dec128 right) {                                             \
        return mc_dec128_##Oper##_ex(left, right, NULL);                                                               \
    }

#define DECL_IDF_COMPARE(Op) DECL_IDF_COMPARE_1(Op)

DECL_IDF_COMPARE(equal)
DECL_IDF_COMPARE(not_equal)
DECL_IDF_COMPARE(greater)
DECL_IDF_COMPARE(greater_equal)
DECL_IDF_COMPARE(less)
DECL_IDF_COMPARE(less_equal)

#undef DECL_IDF_COMPARE
#undef DECL_IDF_COMPARE_1

/// Test properties of Decimal128 numbers
#define DECL_PREDICATE(Name, BIDName)                                                                                  \
    static inline bool mc_dec128_##Name(mc_dec128 d) { return 0 != bid128_##BIDName(_mc_to_bid128(d)); }

DECL_PREDICATE(is_zero, isZero)
DECL_PREDICATE(is_negative, isSigned)
DECL_PREDICATE(is_inf, isInf)
DECL_PREDICATE(is_finite, isFinite)
DECL_PREDICATE(is_nan, isNaN)

#undef DECL_PREDICATE

/// Binary arithmetic operations on two Decimal128 numbers
#define DECL_IDF_BINOP_WRAPPER(Oper)                                                                                   \
    static inline mc_dec128 mc_dec128_##Oper##_ex(mc_dec128 left,                                                      \
                                                  mc_dec128 right,                                                     \
                                                  mc_dec128_rounding_mode mode,                                        \
                                                  mc_dec128_flagset *flags) {                                          \
        mc_dec128_flagset zero_flags = {0};                                                                            \
        return _bid128_to_mc(                                                                                          \
            bid128_##Oper(_mc_to_bid128(left), _mc_to_bid128(right), mode, flags ? &flags->bits : &zero_flags.bits));  \
    }                                                                                                                  \
                                                                                                                       \
    static inline mc_dec128 mc_dec128_##Oper(mc_dec128 left, mc_dec128 right) {                                        \
        return mc_dec128_##Oper##_ex(left, right, MC_DEC128_ROUND_DEFAULT, NULL);                                      \
    }

DECL_IDF_BINOP_WRAPPER(add)
DECL_IDF_BINOP_WRAPPER(mul)
DECL_IDF_BINOP_WRAPPER(div)
DECL_IDF_BINOP_WRAPPER(sub)
DECL_IDF_BINOP_WRAPPER(pow)

#undef DECL_IDF_BINOP_WRAPPER

/// Unary arithmetic operations on two Decimal128 numbers
#define DECL_IDF_UNOP_WRAPPER(Oper)                                                                                    \
    static inline mc_dec128 mc_dec128_##Oper##_ex(mc_dec128 operand, mc_dec128_flagset *flags) {                       \
        mc_dec128_flagset zero_flags = {0};                                                                            \
        return _bid128_to_mc(                                                                                          \
            bid128_##Oper(_mc_to_bid128(operand), MC_DEC128_ROUND_DEFAULT, flags ? &flags->bits : &zero_flags.bits));  \
    }                                                                                                                  \
                                                                                                                       \
    static inline mc_dec128 mc_dec128_##Oper(mc_dec128 operand) { return mc_dec128_##Oper##_ex(operand, NULL); }

DECL_IDF_UNOP_WRAPPER(log2)
DECL_IDF_UNOP_WRAPPER(log10)
#undef DECL_IDF_UNOP_WRAPPER

static inline mc_dec128
mc_dec128_round_integral_ex(mc_dec128 value, mc_dec128_rounding_mode direction, mc_dec128_flagset *flags) {
    BID_UINT128 bid = _mc_to_bid128(value);
    mc_dec128_flagset zero_flags = {0};
    _IDEC_flags *fl = flags ? &flags->bits : &zero_flags.bits;
    switch (direction) {
    case MC_DEC128_ROUND_TOWARD_ZERO: return _bid128_to_mc(bid128_round_integral_zero(bid, fl));
    case MC_DEC128_ROUND_NEAREST_AWAY: return _bid128_to_mc(bid128_round_integral_nearest_away(bid, fl));
    case MC_DEC128_ROUND_NEAREST_EVEN: return _bid128_to_mc(bid128_round_integral_nearest_even(bid, fl));
    case MC_DEC128_ROUND_DOWNWARD: return _bid128_to_mc(bid128_round_integral_negative(bid, fl));
    case MC_DEC128_ROUND_UPWARD: return _bid128_to_mc(bid128_round_integral_positive(bid, fl));
    default: abort();
    }
}

static inline mc_dec128 mc_dec128_negate(mc_dec128 operand) {
    return _bid128_to_mc(bid128_negate(_mc_to_bid128(operand)));
}

static inline mc_dec128 mc_dec128_abs(mc_dec128 operand) {
    return _bid128_to_mc(bid128_abs(_mc_to_bid128(operand)));
}

/**
 * @brief Scale the given Decimal128 by a power of ten
 *
 * @param fac The value being scaled
 * @param exp The exponent: 10^exp is the scale factor
 * @param rounding Rounding behavior
 * @param flags Control/result flags
 * @return mc_dec128 The product
 */
static inline mc_dec128
mc_dec128_scale_ex(mc_dec128 fac, long int exp, mc_dec128_rounding_mode rounding, mc_dec128_flagset *flags) {
    mc_dec128_flagset zero_flags = {0};
    return _bid128_to_mc(bid128_scalbln(_mc_to_bid128(fac), exp, rounding, flags ? &flags->bits : &zero_flags.bits));
}

/**
 * @brief Scale the given Decimal128 by a power of ten
 *
 * @param fac The value being scaled
 * @param exp The exponent: 10^exp is the scale factor
 * @return mc_dec128 The product
 */
static inline mc_dec128 mc_dec128_scale(mc_dec128 fac, long int exp) {
    return mc_dec128_scale_ex(fac, exp, MC_DEC128_ROUND_DEFAULT, NULL);
}

/// The result of a dec_128 modf operation
typedef struct mc_dec128_modf_result {
    /// The whole part of the result
    mc_dec128 whole;
    /// The fractional part of the result
    mc_dec128 frac;
} mc_dec128_modf_result;

/**
 * @brief Split a Decimal128 into its whole and fractional parts.
 *
 * The "whole" value is the integral value of the Decimal128 rounded towards
 * zero. The "frac" part is the remainder after removing the whole.
 *
 * @param d The value to inspect
 * @param flags Results status flags
 */
static inline mc_dec128_modf_result mc_dec128_modf_ex(mc_dec128 d, mc_dec128_flagset *flags) {
    mc_dec128_flagset zero_flags = {0};
    mc_dec128_modf_result res;
    BID_UINT128 whole;
    res.frac = _bid128_to_mc(bid128_modf(_mc_to_bid128(d), &whole, flags ? &flags->bits : &zero_flags.bits));
    res.whole = _bid128_to_mc(whole);
    return res;
}

/**
 * @brief Split a Decimal128 into its whole and fractional parts.
 *
 * The "whole" value is the integral value of the Decimal128 rounded towards
 * zero. The "frac" part is the remainder after removing the whole.
 *
 * @param d The value to inspect
 */
static inline mc_dec128_modf_result mc_dec128_modf(mc_dec128 d) {
    return mc_dec128_modf_ex(d, NULL);
}

/**
 * @brief Compute the "fmod", the remainder after decimal division rounding
 * towards zero.
 *
 * @param numer The dividend of the modulus
 * @param denom The divisor of the modulus
 * @param flags Control/status flags
 */
static inline mc_dec128 mc_dec128_fmod_ex(mc_dec128 numer, mc_dec128 denom, mc_dec128_flagset *flags) {
    mc_dec128_flagset zero_flags = {0};
    return _bid128_to_mc(
        bid128_fmod(_mc_to_bid128(numer), _mc_to_bid128(denom), flags ? &flags->bits : &zero_flags.bits));
}

/**
 * @brief Compute the "fmod", the remainder after decimal division rounding
 * towards zero.
 *
 * @param numer The dividend of the modulus
 * @param denom The divisor of the modulus
 */
static inline mc_dec128 mc_dec128_fmod(mc_dec128 numer, mc_dec128 denom) {
    return mc_dec128_fmod_ex(numer, denom, NULL);
}

/**
 * @brief Obtain the a 64-bit binary integer value for the given Decimal128
 * value, nearest rounding toward zero.
 *
 * @param d The value to inspect
 * @param flags Control/status flags
 */
static inline int64_t mc_dec128_to_int64_ex(mc_dec128 d, mc_dec128_flagset *flags) {
    mc_dec128_flagset zero_flags = {0};
    return bid128_to_int64_int(_mc_to_bid128(d), flags ? &flags->bits : &zero_flags.bits);
}

/**
 * @brief Obtain the a 64-bit binary integer value for the given Decimal128
 * value, nearest rounding toward zero.
 *
 * @param d The value to inspect
 */
static inline int64_t mc_dec128_to_int64(mc_dec128 d) {
    return mc_dec128_to_int64_ex(d, NULL);
}

/// Constants for inspection/deconstruction of Decimal128
enum {
    /// Least non-canonical combination bits value
    MC_DEC128_COMBO_NONCANONICAL = 3 << 15,
    /// Least combination value indicating infinity
    MC_DEC128_COMBO_INFINITY = 0x1e << 12,
    /// The greatest Decimal128 biased exponent
    MC_DEC128_MAX_BIASED_EXPONENT = 6143 + 6144,
    /// The exponent bias of Decimal128
    MC_DEC128_EXPONENT_BIAS = 6143 + 33, // +33 to include the 34 decimal digits
    /// Minimum exponent value (without bias)
    MC_DEC_MIN_EXPONENT = -6143,
    /// Maximum exponent value (without bias)
    MC_DEC_MAX_EXPONENT = 6144,
};

/// Obtain the value of the combination bits of a decimal128
static inline uint32_t mc_dec128_combination(mc_dec128 d) {
    // Grab the high 64 bits:
    uint64_t hi = d._words[MLIB_IS_LITTLE_ENDIAN ? 1 : 0];
    // Sign is the 64th bit:
    int signpos = 64 - 1;
    // Combo is the next 16 bits:
    int fieldpos = signpos - 17;
    int fieldmask = (1 << 17) - 1;
    return (uint32_t)((hi >> fieldpos) & (uint32_t)fieldmask);
}

/**
 * @brief Obtain the value of the high 49 bits of the Decimal128 coefficient
 */
static inline uint64_t mc_dec128_coeff_high(mc_dec128 d) {
    uint64_t hi_field_mask = (1ull << 49) - 1;
    uint32_t combo = mc_dec128_combination(d);
    if (combo < MC_DEC128_COMBO_NONCANONICAL) {
        uint64_t hi = d._words[MLIB_IS_LITTLE_ENDIAN ? 1 : 0];
        return hi & hi_field_mask;
    } else {
        return 0;
    }
}

/**
 * @brief Obtain the value of the low 64 bits of the Decimal128 coefficient
 */
static inline uint64_t mc_dec128_coeff_low(mc_dec128 d) {
    uint32_t combo = mc_dec128_combination(d);
    if (combo < MC_DEC128_COMBO_NONCANONICAL) {
        uint64_t lo = d._words[MLIB_IS_LITTLE_ENDIAN ? 0 : 1];
        return lo;
    } else {
        return 0;
    }
}

/**
 * @brief Obtain the full coefficient of the given Decimal128 number. Requires a
 * 128-bit integer.
 */
static inline mlib_int128 mc_dec128_coeff(mc_dec128 d) {
    // Hi bits
    uint64_t hi = mc_dec128_coeff_high(d);
    // Lo bits
    uint64_t lo = mc_dec128_coeff_low(d);
    // Shift and add
    mlib_int128 hi_128 = mlib_int128_lshift(MLIB_INT128_CAST(hi), 64);
    return mlib_int128_add(hi_128, MLIB_INT128_CAST(lo));
}

/**
 * @brief Obtain the biased value of the Decimal128 exponent.
 *
 * The value is "biased" in that its binary value not actually zero for 10^0. It
 * is offset by half the exponent range (the "bias") so it can encode the full
 * positive and negative exponent range. The bias value is defined as
 * MC_DEC128_EXPONENT_BIAS.
 */
static inline uint32_t mc_dec128_get_biased_exp(mc_dec128 d) {
    uint32_t combo = mc_dec128_combination(d);
    if (combo < MC_DEC128_COMBO_NONCANONICAL) {
        return combo >> 3;
    }
    if (combo >= MC_DEC128_COMBO_INFINITY) {
        return MC_DEC128_MAX_BIASED_EXPONENT + 1;
    } else {
        return (combo >> 1) & ((1 << 14) - 1);
    }
}

/// Create a decimal string from a dec128 number. The result must be freed.
static inline char *mc_dec128_to_new_decimal_string(mc_dec128 d) {
    if (mc_dec128_is_zero(d)) {
        // Just return "0"
        char *s = (char *)calloc(2, 1);
        if (s) {
            s[0] = '0';
        }
        return s;
    }

    if (mc_dec128_is_negative(d)) {
        // Negate the result, return a string with a '-' prefix
        d = mc_dec128_negate(d);
        char *s = mc_dec128_to_new_decimal_string(d);
        if (!s) {
            return NULL;
        }
        char *s1 = (char *)calloc(strlen(s) + 2, 1);
        if (s1) {
            s1[0] = '-';
            strcpy(s1 + 1, s);
        }
        free(s);
        return s1;
    }

    if (mc_dec128_is_inf(d) || mc_dec128_is_nan(d)) {
        const char *r = mc_dec128_is_inf(d) ? "Infinity" : "NaN";
        char *c = (char *)calloc(strlen(r) + 1, 1);
        if (c) {
            strcpy(c, r);
        }
        return c;
    }

    const char DIGITS[] = "0123456789";
    const mc_dec128 TEN = MC_DEC128_C(10);

    // Format the whole and fractional part separately.
    mc_dec128_modf_result modf = mc_dec128_modf(d);

    if (mc_dec128_is_zero(modf.frac)) {
        // This is a non-zero integer
        // Allocate enough digits:
        mc_dec128 log10 = mc_dec128_modf(mc_dec128_log10(d)).whole;
        int64_t ndigits = mc_dec128_to_int64(log10) + 1;
        // +1 for null
        char *strbuf = (char *)calloc((size_t)(ndigits + 1), 1);
        if (strbuf) {
            // Write the string backwards:
            char *optr = strbuf + ndigits - 1;
            while (!mc_dec128_is_zero(modf.whole)) {
                mc_dec128 rem = mc_dec128_fmod(modf.whole, TEN);
                int64_t remi = mc_dec128_to_int64(rem);
                *optr-- = DIGITS[remi];
                // Divide ten
                modf = mc_dec128_modf(mc_dec128_div(modf.whole, TEN));
            }
        }
        return strbuf;
    } else if (mc_dec128_is_zero(modf.whole)) {
        // This is only a fraction (less than one, but more than zero)
        while (!mc_dec128_is_zero(mc_dec128_modf(d).frac)) {
            d = mc_dec128_mul(d, TEN);
        }
        // 'd' is now a whole number
        char *part = mc_dec128_to_new_decimal_string(d);
        if (!part) {
            return NULL;
        }
        char *buf = (char *)calloc(strlen(part) + 3, 1);
        if (buf) {
            buf[0] = '0';
            buf[1] = '.';
            strcpy(buf + 2, part);
        }
        free(part);
        return buf;
    } else {
        // We have both a whole part and a fractional part
        char *whole = mc_dec128_to_new_decimal_string(modf.whole);
        if (!whole) {
            return NULL;
        }
        char *frac = mc_dec128_to_new_decimal_string(modf.frac);
        if (!frac) {
            free(whole);
            return NULL;
        }
        char *ret = (char *)calloc(strlen(whole) + strlen(frac) + 1, 1);
        if (ret) {
            char *out = ret;
            strcpy(out, whole);
            out += strlen(whole);
            // "frac" contains a leading zero, which we don't want
            strcpy(out, frac + 1);
        }
        free(whole);
        free(frac);
        return ret;
    }
}

static inline mc_dec128 mc_dec128_from_bson_iter(bson_iter_t *it) {
    bson_decimal128_t b;
    if (!bson_iter_decimal128(it, &b)) {
        mc_dec128 nan = MC_DEC128_POSITIVE_NAN;
        return nan;
    }
    mc_dec128 ret;
    memcpy(&ret, &b, sizeof b);
    return ret;
}

static inline bson_decimal128_t mc_dec128_to_bson_decimal128(mc_dec128 v) {
    bson_decimal128_t ret;
    memcpy(&ret, &v, sizeof ret);
    return ret;
}

MLIB_C_LINKAGE_END

#endif /// defined MONGOCRYPT_INTELDFP

#endif // MC_DEC128_H_INCLUDED
