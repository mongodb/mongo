#ifndef MLIB_INT128_H_INCLUDED
#define MLIB_INT128_H_INCLUDED

#include "./macros.h"
#include "./str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

MLIB_C_LINKAGE_BEGIN

/**
 * @brief A 128-bit binary integer
 */
typedef union {
    struct {
        uint64_t lo;
        uint64_t hi;
    } r;
#if defined(__SIZEOF_INT128__)
    // These union members are only for the purpose of debugging visualization
    // and testing, and will only appear correctly on little-endian platforms.
    __int128_t signed_;
    __uint128_t unsigned_;
#endif
} mlib_int128;

/// Define an int128 from a literal within [INT64_MIN, INT64_MAX]
#define MLIB_INT128(N) MLIB_INIT(mlib_int128) MLIB_INT128_C(N)
/// Define an int128 from a literal within [INT64_MIN, INT64_MAX] (usable as a
/// constant init)
#define MLIB_INT128_C(N) MLIB_INT128_FROM_PARTS((uint64_t)INT64_C(N), (INT64_C(N) < 0 ? UINT64_MAX : 0))
/**
 * @brief Cast an integral value to an mlib_int128
 *
 * If the argument is signed and less-than zero, it will be sign-extended
 */
#define MLIB_INT128_CAST(N)                                                                                            \
    MLIB_INIT(mlib_int128)                                                                                             \
    MLIB_INT128_FROM_PARTS((uint64_t)(N), ((N) < 0 ? UINT64_MAX : 0))

/**
 * @brief Create an mlib_int128 from the low and high parts of the integer
 *
 * @param LowWord_u64 The low-value 64 bits of the number
 * @param HighWord_u64 The high-value 64 bits of the number
 */
#define MLIB_INT128_FROM_PARTS(LowWord_u64, HighWord_u64)                                                              \
    { {LowWord_u64, HighWord_u64}, }

/// Maximum value of int128 when treated as a signed integer
#define MLIB_INT128_SMAX MLIB_INT128_FROM_PARTS(UINT64_MAX, UINT64_MAX & ~(UINT64_C(1) << 63))

/// Minimum value of int128, when treated as a signed integer
#define MLIB_INT128_SMIN MLIB_INT128_FROM_PARTS(0, UINT64_C(1) << 63)

/// Maximum value of int128, when treated as an unsigned integer
#define MLIB_INT128_UMAX MLIB_INT128_FROM_PARTS(UINT64_MAX, UINT64_MAX)

/**
 * @brief Compare two 128-bit integers as unsigned integers
 *
 * @return (R < 0) if (left < right)
 * @return (R > 0) if (left > right)
 * @return (R = 0) if (left == right)
 */
static mlib_constexpr_fn int mlib_int128_ucmp(mlib_int128 left, mlib_int128 right) {
    if (left.r.hi > right.r.hi) {
        return 1;
    } else if (left.r.hi < right.r.hi) {
        return -1;
    } else if (left.r.lo > right.r.lo) {
        return 1;
    } else if (left.r.lo < right.r.lo) {
        return -1;
    } else {
        return 0;
    }
}

/**
 * @brief Compare two 128-bit integers as signed integers
 *
 * @return (R < 0) if (left < right)
 * @return (R > 0) if (left > right)
 * @return (R = 0) if (left == right)
 */
static mlib_constexpr_fn int mlib_int128_scmp(mlib_int128 left, mlib_int128 right) {
    if ((left.r.hi & (1ull << 63)) == (right.r.hi & (1ull << 63))) {
        // Same signed-ness, so they are as comparable as unsigned
        return mlib_int128_ucmp(left, right);
    } else if (left.r.hi & (1ull << 63)) {
        // The left is negative
        return -1;
    } else {
        // The right is negative
        return 1;
    }
}

/**
 * @brief Determine whether the two 128-bit integers are equal
 *
 * @retval true If left == right
 * @retval false Otherwise
 */
static mlib_constexpr_fn bool mlib_int128_eq(mlib_int128 left, mlib_int128 right) {
    return mlib_int128_ucmp(left, right) == 0;
}

/**
 * @brief Add two 128-bit integers together
 *
 * @return mlib_int128 The sum of the two addends. Overflow will wrap.
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_add(mlib_int128 left, mlib_int128 right) {
    uint64_t losum = left.r.lo + right.r.lo;
    // Overflow check
    unsigned carry = (losum < left.r.lo || losum < right.r.lo);
    uint64_t hisum = left.r.hi + right.r.hi + carry;
    return MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(losum, hisum);
}

/**
 * @brief Treat the given 128-bit integer as signed, and return its
 * negated value
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_negate(mlib_int128 v) {
    mlib_int128 r = MLIB_INT128_FROM_PARTS(~v.r.lo, ~v.r.hi);
    r = mlib_int128_add(r, MLIB_INT128(1));
    return r;
}

/**
 * @brief Subtract two 128-bit integers
 *
 * @return mlib_int128 The difference between `from` and `less`
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_sub(mlib_int128 from, mlib_int128 less) {
    unsigned borrow = from.r.lo < less.r.lo;
    uint64_t low = from.r.lo - less.r.lo;
    uint64_t high = from.r.hi - less.r.hi;
    high -= borrow;
    return MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(low, high);
}

/**
 * @brief Bitwise left-shift a 128-bit integer
 *
 * @param val The value to modify
 * @param off The offset to shift left. If negative, shifts right
 * @return The result of the shift operation
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_lshift(mlib_int128 val, int off) {
    if (off > 0) {
        if (off >= 64) {
            off -= 64;
            uint64_t high = val.r.lo << off;
            return MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(0, high);
        } else {
            uint64_t low = val.r.lo << off;
            uint64_t high = val.r.hi << off;
            high |= val.r.lo >> (64 - off);
            return MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(low, high);
        }
    } else if (off < 0) {
        off = -off;
        if (off >= 64) {
            off -= 64;
            uint64_t low = val.r.hi >> off;
            return MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(low, 0);
        } else {
            uint64_t high = val.r.hi >> off;
            uint64_t low = val.r.lo >> off;
            low |= val.r.hi << (64 - off);
            return MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(low, high);
        }
    } else {
        return val;
    }
}

/**
 * @brief Bitwise logical right-shift a 128-bit integer
 *
 * @param val The value to modify. No "sign bit" is respected.
 * @param off The offset to shift right. If negative, shifts left
 * @return The result of the shift operation
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_rshift(mlib_int128 val, int off) {
    return mlib_int128_lshift(val, -off);
}

/**
 * @brief Bitwise-or two 128-bit integers
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_bitor(mlib_int128 l, mlib_int128 r) {
    return MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(l.r.lo | r.r.lo, l.r.hi | r.r.hi);
}

// Multiply two 64bit integers to get a 128-bit result without overflow
static mlib_constexpr_fn mlib_int128 _mlibUnsignedMult128(uint64_t left, uint64_t right) {
    // Perform a Knuth 4.3.1M multiplication
    uint32_t u[2] = {(uint32_t)left, (uint32_t)(left >> 32)};
    uint32_t v[2] = {(uint32_t)right, (uint32_t)(right >> 32)};
    uint32_t w[4] = {0};

    for (int j = 0; j < 2; ++j) {
        uint64_t t = 0;
        for (int i = 0; i < 2; ++i) {
            t += (uint64_t)(u[i]) * v[j] + w[i + j];
            w[i + j] = (uint32_t)t;
            t >>= 32;
        }
        w[j + 2] = (uint32_t)t;
    }

    return MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(((uint64_t)w[1] << 32) | w[0], ((uint64_t)w[3] << 32) | w[2]);
}

/**
 * @brief Multiply two mlib_int128s together. Overflow will wrap.
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_mul(mlib_int128 l, mlib_int128 r) {
    // Multiply the low-order word
    mlib_int128 ret = _mlibUnsignedMult128(l.r.lo, r.r.lo);
    // Accumulate the high-order parts:
    ret.r.hi += l.r.lo * r.r.hi;
    ret.r.hi += l.r.hi * r.r.lo;
    return ret;
}

/// Get the number of leading zeros in a 64bit number.
static mlib_constexpr_fn int _mlibCountLeadingZeros_u64(uint64_t bits) {
    int n = 0;
    if (bits == 0) {
        return 64;
    }
    while (!(1ull << 63 & bits)) {
        ++n;
        bits <<= 1;
    }
    return n;
}

/// Implementation of Knuth's algorithm 4.3.1 D for unsigned integer division
static mlib_constexpr_fn void
_mlibKnuth431D(uint32_t *const u, const int ulen, const uint32_t *const v, const int vlen, uint32_t *quotient) {
    // Part D1 (normalization) is done by caller,
    // normalized in u and v (radix b is 2^32)
    typedef uint64_t u64;
    typedef int64_t i64;
    typedef uint32_t u32;
    const int m = ulen - vlen - 1;
    const int n = vlen;

    // 'd' is 2^32. Shifting left and right is equivalent to mult and division by
    // d, respectively.

    // D2
    int j = m;
    for (;;) {
        // D3: Select two u32 as a u64:
        u64 two = ((u64)(u[j + n]) << 32) | u[j + n - 1];
        // D3: Partial quotient: q̂
        u64 q = two / v[n - 1];
        // D3: Partial remainder: r̂
        u64 r = two % v[n - 1];

        // D3: Compute q̂ and r̂
        while (q >> 32 || q * (u64)v[n - 2] > (r << 32 | u[j + n - 2])) {
            q--;
            r += v[n - 1];
            if (r >> 32) {
                break;
            }
        }

        // D4: Multiply and subtract
        i64 k = 0;
        i64 t = 0;
        for (int i = 0; i < n; ++i) {
            u64 prod = (u32)q * (u64)(v[i]);
            t = u[i + j] - k - (u32)prod;
            u[i + j] = (u32)t;
            k = (i64)(prod >> 32) - (t >> 32);
        }
        t = u[j + n] - k;
        u[j + n] = (u32)t;

        quotient[j] = (u32)q;

        // D5: Test remainder
        if (t < 0) {
            // D6: Add back
            --quotient[j];
            k = 0;
            for (int i = 0; i < n; ++i) {
                t = u[i + j] + k + v[i];
                u[i + j] = (u32)(t);
                k = t >> 32;
            }
            u[j + n] += (u32)k;
        }

        // D7:
        --j;
        if (j < 0) {
            break;
        }
    }

    // Denormalization (D8) is done by caller.
}

/// The result of 128-bit division
typedef struct mlib_int128_divmod_result {
    /// The quotient of the division operation (rounds to zero)
    mlib_int128 quotient;
    /// The remainder of the division operation
    mlib_int128 remainder;
} mlib_int128_divmod_result;

/// Divide a 128-bit number by a 64bit number.
static mlib_constexpr_fn struct mlib_int128_divmod_result _mlibDivide_u128_by_u64(const mlib_int128 numer,
                                                                                  const uint64_t denom) {
    mlib_int128 adjusted = numer;
    adjusted.r.hi %= denom;
    int d = _mlibCountLeadingZeros_u64(denom);

    typedef uint32_t u32;
    typedef uint64_t u64;

    if (d >= 32) {
        // jk: We're dividing by less than UINT32_MAX: We can do a simple short
        // division of two base32 numbers.
        // Treat the denominator as a single base32 digit:
        const u32 d0 = (u32)denom;

        // And the numerator as four base32 digits:
        const u64 n0 = (u32)(numer.r.lo);
        const u64 n1 = (u32)(numer.r.lo >> 32);

        // We don't need to split n2 and n3. (n3,n2) will be the first partial
        // dividend
        const u64 n3_n2 = numer.r.hi;

        // First partial remainder: (n3,n2) % d0
        const u64 r1 = n3_n2 % d0;
        // Second partial dividend: (r1,n1)
        const u64 r1_n1 = (r1 << 32) + n1;
        // Second partial remainder: (r1,n1) % d0
        const u64 r0 = r1_n1 % d0;
        // Final partial dividend: (r0,n0)
        const u64 r0_n0 = (r0 << 32) + n0;
        // Final remainder: (r0,n0) % d0
        const u64 rem = r0_n0 % d0;

        // Form the quotient as four base32 digits:
        // Least quotient digit: (r0,n0) / d0
        const u64 q0 = r0_n0 / d0;
        // Second quotient digit: (r1,n1) / d0
        const u64 q1 = r1_n1 / d0;
        // Third and fourth quotient digit: (n3,n2) / d0
        const u64 q3_q2 = n3_n2 / d0;

        // Low word of the quotient: (q1,q0)
        const u64 q1_q0 = (q1 << 32) + q0;

        return MLIB_INIT(mlib_int128_divmod_result){
            MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(q1_q0, q3_q2),
            MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(rem, 0),
        };
    }

    // Normalize for a Knuth 4.3.1D division. Convert the integers into two
    // base-32 numbers, with u and v being arrays of digits:
    u32 u[5] = {
        (u32)(adjusted.r.lo << d),
        (u32)(adjusted.r.lo >> (32 - d)),
        (u32)(adjusted.r.hi << d),
        (u32)(adjusted.r.hi >> (32 - d)),
        0,
    };

    if (d != 0) {
        // Extra bits from overlap:
        u[2] |= (u32)(adjusted.r.lo >> (64 - d));
        u[4] |= (u32)(adjusted.r.hi >> (64 - d));
    }

    u32 v[2] = {
        (u32)(denom << d),
        (u32)(denom >> (32 - d)),
    };

    u32 qparts[3] = {0};

    _mlibKnuth431D(u, 5, v, 2, qparts);

    u64 rem = ((u64)u[1] << (32 - d)) | (u[0] >> d);
    u64 quo = ((u64)qparts[1] << 32) | qparts[0];
    return MLIB_INIT(mlib_int128_divmod_result){
        MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(quo, numer.r.hi / denom),
        MLIB_INIT(mlib_int128) MLIB_INT128_FROM_PARTS(rem, 0),
    };
}

/**
 * @brief Perform a combined division+remainder of two 128-bit numbers
 *
 * @param numer The dividend
 * @param denom The divisor
 * @return A struct with .quotient and .remainder results
 */
static mlib_constexpr_fn mlib_int128_divmod_result mlib_int128_divmod(mlib_int128 numer, mlib_int128 denom) {
    const uint64_t nhi = numer.r.hi;
    const uint64_t nlo = numer.r.lo;
    const uint64_t dhi = denom.r.hi;
    const uint64_t dlo = denom.r.lo;
    if (dhi > nhi) {
        // Denominator is definitely larger than numerator. Quotient is zero,
        // remainder is full numerator.
        return MLIB_INIT(mlib_int128_divmod_result){MLIB_INT128(0), numer};
    } else if (dhi == nhi) {
        // High words are equal
        if (nhi == 0) {
            // Both high words are zero, so this is just a division of two 64bit
            // numbers
            return MLIB_INIT(mlib_int128_divmod_result){
                MLIB_INT128_CAST(nlo / dlo),
                MLIB_INT128_CAST(nlo % dlo),
            };
        } else if (nlo > dlo) {
            // The numerator is larger than the denom and the high word on the
            // denom is non-zero, so this cannot divide to anything greater than 1.
            return MLIB_INIT(mlib_int128_divmod_result){
                MLIB_INT128(1),
                mlib_int128_sub(numer, denom),
            };
        } else if (nlo < dlo) {
            // numer.r.lo < denom.r.lo and denom.r.hi > denom.r.lo, so the
            // integer division becomes zero
            return MLIB_INIT(mlib_int128_divmod_result){
                MLIB_INT128(0),
                numer,
            };
        } else {
            // N / N is one
            return MLIB_INIT(mlib_int128_divmod_result){MLIB_INT128(1), MLIB_INT128(0)};
        }
    } else if (dhi == 0) {
        // No high in denominator. We can use a u128/u64
        return _mlibDivide_u128_by_u64(numer, denom.r.lo);
    } else {
        // We'll need to do a full u128/u128 division
        // Normalize for Knuth 4.3.1D
        int d = _mlibCountLeadingZeros_u64(denom.r.hi);
        // Does the denom have only three base32 digits?
        const bool has_three = d >= 32;
        d &= 31;

        uint32_t u[5] = {
            (uint32_t)(numer.r.lo << d),
            (uint32_t)(numer.r.lo >> (32 - d)),
            (uint32_t)(numer.r.hi << d),
            (uint32_t)(numer.r.hi >> (32 - d)),
            0,
        };
        uint32_t v[4] = {
            (uint32_t)(denom.r.lo << d),
            (uint32_t)(denom.r.lo >> (32 - d)),
            (uint32_t)(denom.r.hi << d),
            (uint32_t)(denom.r.hi >> (32 - d)),
        };
        if (d != 0) {
            u[2] |= (uint32_t)(numer.r.lo >> (64 - d));
            u[4] |= (uint32_t)(numer.r.hi >> (64 - d));
            v[2] |= (uint32_t)(denom.r.lo >> (64 - d));
        };

        uint32_t q[2] = {0};
        if (has_three) {
            _mlibKnuth431D(u, 5, v, 3, q);
        } else {
            _mlibKnuth431D(u, 5, v, 4, q);
        }

        mlib_int128 remainder = MLIB_INT128_FROM_PARTS(((uint64_t)u[1] << 32) | u[0], ((uint64_t)u[3] << 32) | u[2]);
        remainder = mlib_int128_rshift(remainder, d);

        return MLIB_INIT(mlib_int128_divmod_result){
            MLIB_INT128_CAST(q[0] | (uint64_t)q[1] << 32),
            remainder,
        };
    }
}

/**
 * @brief Perform a division of two 128-bit numbers
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_div(mlib_int128 numer, mlib_int128 denom) {
    return mlib_int128_divmod(numer, denom).quotient;
}

/**
 * @brief Perform a modulus of two 128-bit numbers
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_mod(mlib_int128 numer, mlib_int128 denom) {
    return mlib_int128_divmod(numer, denom).remainder;
}

/**
 * @brief Get the nth power of ten as a 128-bit number
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_pow10(uint8_t nth) {
    mlib_int128 r = MLIB_INT128(1);
    while (nth-- > 0) {
        r = mlib_int128_mul(r, MLIB_INT128(10));
    }
    return r;
}

/**
 * @brief Get the Nth power of two as a 128-bit number
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_pow2(uint8_t nth) {
    return mlib_int128_lshift(MLIB_INT128(1), (int)nth);
}

/**
 * @brief Read a 128-bit unsigned integer from a base-10 string
 */
static mlib_constexpr_fn mlib_int128 mlib_int128_from_string(const char *s, const char **end) {
    int radix = 10;
    if (mlib_strlen(s) > 2 && s[0] == '0') {
        // Check for a different radix
        char b = s[1];
        if (b == 'b' || b == 'B') {
            radix = 2;
            s += 2;
        } else if (b == 'c' || b == 'C') {
            radix = 8;
            s += 2;
        } else if (b == 'x' || b == 'X') {
            radix = 16;
            s += 2;
        } else {
            radix = 8;
            s += 1;
        }
    }

    mlib_int128 ret = MLIB_INT128(0);
    for (; *s; ++s) {
        char c = *s;
        if (c == '\'') {
            // Digit separator. Skip it;
            continue;
        }
        if (c >= 'a') {
            // Uppercase (if a letter, otherwise some other punct):
            c = (char)(c - ('a' - 'A'));
        }
        int digit = c - '0';
        if (c >= 'A') {
            // It's actually a letter (or garbage, which we'll catch later)
            digit = (c - 'A') + 10;
        }
        if (digit > radix || digit < 0) {
            // The digit is outside of our radix, or garbage
            break;
        }
        ret = mlib_int128_mul(ret, MLIB_INT128_CAST(radix));
        ret = mlib_int128_add(ret, MLIB_INT128_CAST(digit));
    }
    if (end) {
        *end = s;
    }
    return ret;
}

/**
 * @brief Truncate a 128-bit number to a 64-bit number
 */
static mlib_constexpr_fn uint64_t mlib_int128_to_u64(mlib_int128 v) {
    return v.r.lo;
}

/// The result type of formatting a 128-bit number
typedef struct {
    /// The character array of the number as a base10 string. Null-terminated.
    char str[40];
} mlib_int128_charbuf;

/**
 * @brief Format a 128-bit integer into a string of base10 digits.
 *
 * @return mlib_int128_charbuf a struct containing a .str character array
 */
static mlib_constexpr_fn mlib_int128_charbuf mlib_int128_format(mlib_int128 i) {
    mlib_int128_charbuf into = {{0}};
    char *out = into.str + (sizeof into) - 1;
    int len = 0;
    if (mlib_int128_eq(i, MLIB_INT128(0))) {
        *out-- = '0';
        len = 1;
    }
    while (!mlib_int128_eq(i, MLIB_INT128(0))) {
        mlib_int128_divmod_result dm = mlib_int128_divmod(i, MLIB_INT128(10));
        uint64_t v = mlib_int128_to_u64(dm.remainder);
        char digits[] = "0123456789";
        char d = digits[v];
        *out = d;
        --out;
        i = dm.quotient;
        ++len;
    }
    for (int j = 0; j < len; ++j) {
        into.str[j] = out[j + 1];
    }
    into.str[len] = 0;
    return into;
}

MLIB_C_LINKAGE_END

#endif // MLIB_INT128_H_INCLUDED
