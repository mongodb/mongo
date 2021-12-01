/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Portable double to alphanumeric string and back converters.
 */

#include "util/DoubleToString.h"

#include "mozilla/EndianUtils.h"

#include "jstypes.h"
#include "jsutil.h"

using namespace js;

#if MOZ_LITTLE_ENDIAN
#define IEEE_8087
#else
#define IEEE_MC68k
#endif

#ifndef Long
#define Long int32_t
#endif

#ifndef ULong
#define ULong uint32_t
#endif

/*
#ifndef Llong
#define Llong int64_t
#endif

#ifndef ULlong
#define ULlong uint64_t
#endif
*/

// dtoa.c requires that MALLOC be infallible. Furthermore, its allocations are
// few and small. So AutoEnterOOMUnsafeRegion is appropriate here.
static inline void* dtoa_malloc(size_t size)
{
    AutoEnterOOMUnsafeRegion oomUnsafe;
    void* p = js_malloc(size);
    if (!p)
        oomUnsafe.crash("dtoa_malloc");

    return p;
}

static inline void dtoa_free(void* p)
{
  return js_free(p);
}

#define NO_GLOBAL_STATE
#define NO_ERRNO
#define Omit_Private_Memory // This saves memory for the workloads we see.
#define MALLOC dtoa_malloc
#define FREE dtoa_free
#include "dtoa.c"

/* Mapping of JSDToStrMode -> js_dtoa mode */
static const uint8_t dtoaModes[] = {
    0,   /* DTOSTR_STANDARD */
    0,   /* DTOSTR_STANDARD_EXPONENTIAL, */
    3,   /* DTOSTR_FIXED, */
    2,   /* DTOSTR_EXPONENTIAL, */
    2};  /* DTOSTR_PRECISION */

double
js_strtod_harder(DtoaState* state, const char* s00, char** se)
{
    return _strtod(state, s00, se);
}

char*
js_dtostr(DtoaState* state, char* buffer, size_t bufferSize, JSDToStrMode mode, int precision,
          double dinput)
{
    U d;
    int decPt;        /* Offset of decimal point from first digit */
    int sign;         /* Nonzero if the sign bit was set in d */
    int nDigits;      /* Number of significand digits returned by js_dtoa */
    char* numBegin;   /* Pointer to the digits returned by js_dtoa */
    char* numEnd = 0; /* Pointer past the digits returned by js_dtoa */

    MOZ_ASSERT(bufferSize >= (size_t)(mode <= DTOSTR_STANDARD_EXPONENTIAL
                                     ? DTOSTR_STANDARD_BUFFER_SIZE
                                     : DTOSTR_VARIABLE_BUFFER_SIZE(precision)));

    /*
     * Change mode here rather than below because the buffer may not be large
     * enough to hold a large integer.
     */
    if (mode == DTOSTR_FIXED && (dinput >= 1e21 || dinput <= -1e21))
        mode = DTOSTR_STANDARD;

    dval(d) = dinput;
    numBegin = dtoa(PASS_STATE d, dtoaModes[mode], precision, &decPt, &sign, &numEnd);
    if (!numBegin) {
        return nullptr;
    }

    nDigits = numEnd - numBegin;
    MOZ_ASSERT((size_t) nDigits <= bufferSize - 2);
    if ((size_t) nDigits > bufferSize - 2) {
        return nullptr;
    }

    js_memcpy(buffer + 2, numBegin, nDigits);
    freedtoa(PASS_STATE numBegin);
    numBegin = buffer + 2; /* +2 leaves space for sign and/or decimal point */
    numEnd = numBegin + nDigits;
    *numEnd = '\0';

    /* If Infinity, -Infinity, or NaN, return the string regardless of mode. */
    if (decPt != 9999) {
        bool exponentialNotation = false;
        int minNDigits = 0;  /* Min number of significant digits required */
        char* p;
        char* q;

        switch (mode) {
            case DTOSTR_STANDARD:
                if (decPt < -5 || decPt > 21)
                    exponentialNotation = true;
                else
                    minNDigits = decPt;
                break;

            case DTOSTR_FIXED:
                if (precision >= 0)
                    minNDigits = decPt + precision;
                else
                    minNDigits = decPt;
                break;

            case DTOSTR_EXPONENTIAL:
                MOZ_ASSERT(precision > 0);
                minNDigits = precision;
                MOZ_FALLTHROUGH;
            case DTOSTR_STANDARD_EXPONENTIAL:
                exponentialNotation = true;
                break;

            case DTOSTR_PRECISION:
                MOZ_ASSERT(precision > 0);
                minNDigits = precision;
                if (decPt < -5 || decPt > precision)
                    exponentialNotation = true;
                break;
        }

        /* If the number has fewer than minNDigits, end-pad it with zeros. */
        if (nDigits < minNDigits) {
            p = numBegin + minNDigits;
            nDigits = minNDigits;
            do {
                *numEnd++ = '0';
            } while (numEnd != p);
            *numEnd = '\0';
        }

        if (exponentialNotation) {
            /* Insert a decimal point if more than one significand digit */
            if (nDigits != 1) {
                numBegin--;
                numBegin[0] = numBegin[1];
                numBegin[1] = '.';
            }
            snprintf(numEnd, bufferSize - (numEnd - buffer), "e%+d", decPt-1);
        } else if (decPt != nDigits) {
            /* Some kind of a fraction in fixed notation */
            MOZ_ASSERT(decPt <= nDigits);
            if (decPt > 0) {
                /* dd...dd . dd...dd */
                p = --numBegin;
                do {
                    *p = p[1];
                    p++;
                } while (--decPt);
                *p = '.';
            } else {
                /* 0 . 00...00dd...dd */
                p = numEnd;
                numEnd += 1 - decPt;
                q = numEnd;
                MOZ_ASSERT(numEnd < buffer + bufferSize);
                *numEnd = '\0';
                while (p != numBegin)
                    *--q = *--p;
                for (p = numBegin + 1; p != q; p++)
                    *p = '0';
                *numBegin = '.';
                *--numBegin = '0';
            }
        }
    }

    /* If negative and neither -0.0 nor NaN, output a leading '-'. */
    if (sign &&
            !(word0(d) == Sign_bit && word1(d) == 0) &&
            !((word0(d) & Exp_mask) == Exp_mask &&
              (word1(d) || (word0(d) & Frac_mask)))) {
        *--numBegin = '-';
    }
    return numBegin;
}


/* Let b = floor(b / divisor), and return the remainder.  b must be nonnegative.
 * divisor must be between 1 and 65536.
 * This function cannot run out of memory. */
static uint32_t
divrem(Bigint* b, uint32_t divisor)
{
    int32_t n = b->wds;
    uint32_t remainder = 0;
    ULong* bx;
    ULong* bp;

    MOZ_ASSERT(divisor > 0 && divisor <= 65536);

    if (!n)
        return 0; /* b is zero */
    bx = b->x;
    bp = bx + n;
    do {
        ULong a = *--bp;
        ULong dividend = remainder << 16 | a >> 16;
        ULong quotientHi = dividend / divisor;
        ULong quotientLo;

        remainder = dividend - quotientHi*divisor;
        MOZ_ASSERT(quotientHi <= 0xFFFF && remainder < divisor);
        dividend = remainder << 16 | (a & 0xFFFF);
        quotientLo = dividend / divisor;
        remainder = dividend - quotientLo*divisor;
        MOZ_ASSERT(quotientLo <= 0xFFFF && remainder < divisor);
        *bp = quotientHi << 16 | quotientLo;
    } while (bp != bx);
    /* Decrease the size of the number if its most significant word is now zero. */
    if (bx[n-1] == 0)
        b->wds--;
    return remainder;
}

/* Return floor(b/2^k) and set b to be the remainder.  The returned quotient must be less than 2^32. */
static uint32_t quorem2(Bigint* b, int32_t k)
{
    ULong mask;
    ULong result;
    ULong* bx;
    ULong* bxe;
    int32_t w;
    int32_t n = k >> 5;
    k &= 0x1F;
    mask = (ULong(1)<<k) - 1;

    w = b->wds - n;
    if (w <= 0)
        return 0;
    MOZ_ASSERT(w <= 2);
    bx = b->x;
    bxe = bx + n;
    result = *bxe >> k;
    *bxe &= mask;
    if (w == 2) {
        MOZ_ASSERT(!(bxe[1] & ~mask));
        if (k)
            result |= bxe[1] << (32 - k);
    }
    n++;
    while (!*bxe && bxe != bx) {
        n--;
        bxe--;
    }
    b->wds = n;
    return result;
}


/* "-0.0000...(1073 zeros after decimal point)...0001\0" is the longest string that we could produce,
 * which occurs when printing -5e-324 in binary.  We could compute a better estimate of the size of
 * the output string and malloc fewer bytes depending on d and base, but why bother? */
#define DTOBASESTR_BUFFER_SIZE 1078
#define BASEDIGIT(digit) ((char)(((digit) >= 10) ? 'a' - 10 + (digit) : '0' + (digit)))

char*
js_dtobasestr(DtoaState* state, int base, double dinput)
{
    U d;
    char* buffer;        /* The output string */
    char* p;             /* Pointer to current position in the buffer */
    char* pInt;          /* Pointer to the beginning of the integer part of the string */
    char* q;
    uint32_t digit;
    U di;                /* d truncated to an integer */
    U df;                /* The fractional part of d */

    MOZ_ASSERT(base >= 2 && base <= 36);

    dval(d) = dinput;
    buffer = (char*) js_malloc(DTOBASESTR_BUFFER_SIZE);
    if (!buffer)
        return nullptr;
    p = buffer;

    if (dval(d) < 0.0
#if defined(XP_WIN)
        && !((word0(d) & Exp_mask) == Exp_mask && ((word0(d) & Frac_mask) || word1(d))) /* Visual C++ doesn't know how to compare against NaN */
#endif
       ) {
        *p++ = '-';
        dval(d) = -dval(d);
    }

    /* Check for Infinity and NaN */
    if ((word0(d) & Exp_mask) == Exp_mask) {
        strcpy(p, !word1(d) && !(word0(d) & Frac_mask) ? "Infinity" : "NaN");
        return buffer;
    }

    /* Output the integer part of d with the digits in reverse order. */
    pInt = p;
    dval(di) = floor(dval(d));
    if (dval(di) <= 4294967295.0) {
        uint32_t n = (uint32_t)dval(di);
        if (n)
            do {
                uint32_t m = n / base;
                digit = n - m*base;
                n = m;
                MOZ_ASSERT(digit < (uint32_t)base);
                *p++ = BASEDIGIT(digit);
            } while (n);
        else *p++ = '0';
    } else {
        int e;
        int bits;  /* Number of significant bits in di; not used. */
        Bigint* b = d2b(PASS_STATE di, &e, &bits);
        if (!b)
            goto nomem1;
        b = lshift(PASS_STATE b, e);
        if (!b) {
          nomem1:
            Bfree(PASS_STATE b);
            js_free(buffer);
            return nullptr;
        }
        do {
            digit = divrem(b, base);
            MOZ_ASSERT(digit < (uint32_t)base);
            *p++ = BASEDIGIT(digit);
        } while (b->wds);
        Bfree(PASS_STATE b);
    }
    /* Reverse the digits of the integer part of d. */
    q = p-1;
    while (q > pInt) {
        char ch = *pInt;
        *pInt++ = *q;
        *q-- = ch;
    }

    dval(df) = dval(d) - dval(di);
    if (dval(df) != 0.0) {
        /* We have a fraction. */
        int e, bbits;
        int32_t s2, done;
        Bigint* b = nullptr;
        Bigint* s = nullptr;
        Bigint* mlo = nullptr;
        Bigint* mhi = nullptr;

        *p++ = '.';
        b = d2b(PASS_STATE df, &e, &bbits);
        if (!b) {
          nomem2:
            Bfree(PASS_STATE b);
            Bfree(PASS_STATE s);
            if (mlo != mhi)
                Bfree(PASS_STATE mlo);
            Bfree(PASS_STATE mhi);
            js_free(buffer);
            return nullptr;
        }
        MOZ_ASSERT(e < 0);
        /* At this point df = b * 2^e.  e must be less than zero because 0 < df < 1. */

        s2 = -(int32_t)(word0(d) >> Exp_shift1 & Exp_mask>>Exp_shift1);
#ifndef Sudden_Underflow
        if (!s2)
            s2 = -1;
#endif
        s2 += Bias + P;
        /* 1/2^s2 = (nextDouble(d) - d)/2 */
        MOZ_ASSERT(-s2 < e);
        mlo = i2b(PASS_STATE 1);
        if (!mlo)
            goto nomem2;
        mhi = mlo;
        if (!word1(d) && !(word0(d) & Bndry_mask)
#ifndef Sudden_Underflow
            && word0(d) & (Exp_mask & Exp_mask << 1)
#endif
            ) {
            /* The special case.  Here we want to be within a quarter of the last input
               significant digit instead of one half of it when the output string's value is less than d.  */
            s2 += Log2P;
            mhi = i2b(PASS_STATE 1<<Log2P);
            if (!mhi)
                goto nomem2;
        }
        b = lshift(PASS_STATE b, e + s2);
        if (!b)
            goto nomem2;
        s = i2b(PASS_STATE 1);
        if (!s)
            goto nomem2;
        s = lshift(PASS_STATE s, s2);
        if (!s)
            goto nomem2;
        /* At this point we have the following:
         *   s = 2^s2;
         *   1 > df = b/2^s2 > 0;
         *   (d - prevDouble(d))/2 = mlo/2^s2;
         *   (nextDouble(d) - d)/2 = mhi/2^s2. */

        done = false;
        do {
            int32_t j, j1;
            Bigint* delta;

            b = multadd(PASS_STATE b, base, 0);
            if (!b)
                goto nomem2;
            digit = quorem2(b, s2);
            if (mlo == mhi) {
                mlo = mhi = multadd(PASS_STATE mlo, base, 0);
                if (!mhi)
                    goto nomem2;
            }
            else {
                mlo = multadd(PASS_STATE mlo, base, 0);
                if (!mlo)
                    goto nomem2;
                mhi = multadd(PASS_STATE mhi, base, 0);
                if (!mhi)
                    goto nomem2;
            }

            /* Do we yet have the shortest string that will round to d? */
            j = cmp(b, mlo);
            /* j is b/2^s2 compared with mlo/2^s2. */
            delta = diff(PASS_STATE s, mhi);
            if (!delta)
                goto nomem2;
            j1 = delta->sign ? 1 : cmp(b, delta);
            Bfree(PASS_STATE delta);
            /* j1 is b/2^s2 compared with 1 - mhi/2^s2. */

#ifndef ROUND_BIASED
            if (j1 == 0 && !(word1(d) & 1)) {
                if (j > 0)
                    digit++;
                done = true;
            } else
#endif
            if (j < 0 || (j == 0
#ifndef ROUND_BIASED
                && !(word1(d) & 1)
#endif
                )) {
                if (j1 > 0) {
                    /* Either dig or dig+1 would work here as the least significant digit.
                       Use whichever would produce an output value closer to d. */
                    b = lshift(PASS_STATE b, 1);
                    if (!b)
                        goto nomem2;
                    j1 = cmp(b, s);
                    if (j1 > 0) /* The even test (|| (j1 == 0 && (digit & 1))) is not here because it messes up odd base output
                                 * such as 3.5 in base 3.  */
                        digit++;
                }
                done = true;
            } else if (j1 > 0) {
                digit++;
                done = true;
            }
            MOZ_ASSERT(digit < (uint32_t)base);
            *p++ = BASEDIGIT(digit);
        } while (!done);
        Bfree(PASS_STATE b);
        Bfree(PASS_STATE s);
        if (mlo != mhi)
            Bfree(PASS_STATE mlo);
        Bfree(PASS_STATE mhi);
    }
    MOZ_ASSERT(p < buffer + DTOBASESTR_BUFFER_SIZE);
    *p = '\0';
    return buffer;
}

DtoaState*
js::NewDtoaState()
{
    return newdtoa();
}

void
js::DestroyDtoaState(DtoaState* state)
{
    destroydtoa(state);
}

/* Cleanup pollution from dtoa.c */
#undef Bias
