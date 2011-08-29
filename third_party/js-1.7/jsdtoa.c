/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * Portable double to alphanumeric string and back converters.
 */
#include "jsstddef.h"
#include "jslibmath.h"
#include "jstypes.h"
#include "jsdtoa.h"
#include "jsprf.h"
#include "jsutil.h" /* Added by JSIFY */
#include "jspubtd.h"
#include "jsnum.h"

#ifdef JS_THREADSAFE
#include "prlock.h"
#endif

/****************************************************************
 *
 * The author of this software is David M. Gay.
 *
 * Copyright (c) 1991 by Lucent Technologies.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose without fee is hereby granted, provided that this entire notice
 * is included in all copies of any software which is or includes a copy
 * or modification of this software and in all copies of the supporting
 * documentation for such software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY.  IN PARTICULAR, NEITHER THE AUTHOR NOR LUCENT MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
 * OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 ***************************************************************/

/* Please send bug reports to
    David M. Gay
    Bell Laboratories, Room 2C-463
    600 Mountain Avenue
    Murray Hill, NJ 07974-0636
    U.S.A.
    dmg@bell-labs.com
 */

/* On a machine with IEEE extended-precision registers, it is
 * necessary to specify double-precision (53-bit) rounding precision
 * before invoking strtod or dtoa.  If the machine uses (the equivalent
 * of) Intel 80x87 arithmetic, the call
 *  _control87(PC_53, MCW_PC);
 * does this with many compilers.  Whether this or another call is
 * appropriate depends on the compiler; for this to work, it may be
 * necessary to #include "float.h" or another system-dependent header
 * file.
 */

/* strtod for IEEE-arithmetic machines.
 *
 * This strtod returns a nearest machine number to the input decimal
 * string (or sets err to JS_DTOA_ERANGE or JS_DTOA_ENOMEM).  With IEEE
 * arithmetic, ties are broken by the IEEE round-even rule.  Otherwise
 * ties are broken by biased rounding (add half and chop).
 *
 * Inspired loosely by William D. Clinger's paper "How to Read Floating
 * Point Numbers Accurately" [Proc. ACM SIGPLAN '90, pp. 92-101].
 *
 * Modifications:
 *
 *  1. We only require IEEE double-precision
 *      arithmetic (not IEEE double-extended).
 *  2. We get by with floating-point arithmetic in a case that
 *      Clinger missed -- when we're computing d * 10^n
 *      for a small integer d and the integer n is not too
 *      much larger than 22 (the maximum integer k for which
 *      we can represent 10^k exactly), we may be able to
 *      compute (d*10^k) * 10^(e-k) with just one roundoff.
 *  3. Rather than a bit-at-a-time adjustment of the binary
 *      result in the hard case, we use floating-point
 *      arithmetic to determine the adjustment to within
 *      one bit; only in really hard cases do we need to
 *      compute a second residual.
 *  4. Because of 3., we don't need a large table of powers of 10
 *      for ten-to-e (just some small tables, e.g. of 10^k
 *      for 0 <= k <= 22).
 */

/*
 * #define IEEE_8087 for IEEE-arithmetic machines where the least
 *  significant byte has the lowest address.
 * #define IEEE_MC68k for IEEE-arithmetic machines where the most
 *  significant byte has the lowest address.
 * #define Long int on machines with 32-bit ints and 64-bit longs.
 * #define Sudden_Underflow for IEEE-format machines without gradual
 *  underflow (i.e., that flush to zero on underflow).
 * #define No_leftright to omit left-right logic in fast floating-point
 *  computation of js_dtoa.
 * #define Check_FLT_ROUNDS if FLT_ROUNDS can assume the values 2 or 3.
 * #define RND_PRODQUOT to use rnd_prod and rnd_quot (assembly routines
 *  that use extended-precision instructions to compute rounded
 *  products and quotients) with IBM.
 * #define ROUND_BIASED for IEEE-format with biased rounding.
 * #define Inaccurate_Divide for IEEE-format with correctly rounded
 *  products but inaccurate quotients, e.g., for Intel i860.
 * #define JS_HAVE_LONG_LONG on machines that have a "long long"
 *  integer type (of >= 64 bits).  If long long is available and the name is
 *  something other than "long long", #define Llong to be the name,
 *  and if "unsigned Llong" does not work as an unsigned version of
 *  Llong, #define #ULLong to be the corresponding unsigned type.
 * #define Bad_float_h if your system lacks a float.h or if it does not
 *  define some or all of DBL_DIG, DBL_MAX_10_EXP, DBL_MAX_EXP,
 *  FLT_RADIX, FLT_ROUNDS, and DBL_MAX.
 * #define MALLOC your_malloc, where your_malloc(n) acts like malloc(n)
 *  if memory is available and otherwise does something you deem
 *  appropriate.  If MALLOC is undefined, malloc will be invoked
 *  directly -- and assumed always to succeed.
 * #define Omit_Private_Memory to omit logic (added Jan. 1998) for making
 *  memory allocations from a private pool of memory when possible.
 *  When used, the private pool is PRIVATE_MEM bytes long: 2000 bytes,
 *  unless #defined to be a different length.  This default length
 *  suffices to get rid of MALLOC calls except for unusual cases,
 *  such as decimal-to-binary conversion of a very long string of
 *  digits.
 * #define INFNAN_CHECK on IEEE systems to cause strtod to check for
 *  Infinity and NaN (case insensitively).  On some systems (e.g.,
 *  some HP systems), it may be necessary to #define NAN_WORD0
 *  appropriately -- to the most significant word of a quiet NaN.
 *  (On HP Series 700/800 machines, -DNAN_WORD0=0x7ff40000 works.)
 * #define MULTIPLE_THREADS if the system offers preemptively scheduled
 *  multiple threads.  In this case, you must provide (or suitably
 *  #define) two locks, acquired by ACQUIRE_DTOA_LOCK() and released
 *  by RELEASE_DTOA_LOCK().  (The second lock, accessed
 *  in pow5mult, ensures lazy evaluation of only one copy of high
 *  powers of 5; omitting this lock would introduce a small
 *  probability of wasting memory, but would otherwise be harmless.)
 *  You must also invoke freedtoa(s) to free the value s returned by
 *  dtoa.  You may do so whether or not MULTIPLE_THREADS is #defined.
 * #define NO_IEEE_Scale to disable new (Feb. 1997) logic in strtod that
 *  avoids underflows on inputs whose result does not underflow.
 */
#ifdef IS_LITTLE_ENDIAN
#define IEEE_8087
#else
#define IEEE_MC68k
#endif

#ifndef Long
#define Long int32
#endif

#ifndef ULong
#define ULong uint32
#endif

#define Bug(errorMessageString) JS_ASSERT(!errorMessageString)

#include "stdlib.h"
#include "string.h"

#ifdef MALLOC
extern void *MALLOC(size_t);
#else
#define MALLOC malloc
#endif

#define Omit_Private_Memory
/* Private memory currently doesn't work with JS_THREADSAFE */
#ifndef Omit_Private_Memory
#ifndef PRIVATE_MEM
#define PRIVATE_MEM 2000
#endif
#define PRIVATE_mem ((PRIVATE_MEM+sizeof(double)-1)/sizeof(double))
static double private_mem[PRIVATE_mem], *pmem_next = private_mem;
#endif

#ifdef Bad_float_h
#undef __STDC__

#define DBL_DIG 15
#define DBL_MAX_10_EXP 308
#define DBL_MAX_EXP 1024
#define FLT_RADIX 2
#define FLT_ROUNDS 1
#define DBL_MAX 1.7976931348623157e+308



#ifndef LONG_MAX
#define LONG_MAX 2147483647
#endif

#else /* ifndef Bad_float_h */
#include "float.h"
#endif /* Bad_float_h */

#ifndef __MATH_H__
#include "math.h"
#endif

#ifndef CONST
#define CONST const
#endif

#if defined(IEEE_8087) + defined(IEEE_MC68k) != 1
Exactly one of IEEE_8087 or IEEE_MC68k should be defined.
#endif

#define word0(x)        JSDOUBLE_HI32(x)
#define set_word0(x, y) JSDOUBLE_SET_HI32(x, y)
#define word1(x)        JSDOUBLE_LO32(x)
#define set_word1(x, y) JSDOUBLE_SET_LO32(x, y)

#define Storeinc(a,b,c) (*(a)++ = (b) << 16 | (c) & 0xffff)

/* #define P DBL_MANT_DIG */
/* Ten_pmax = floor(P*log(2)/log(5)) */
/* Bletch = (highest power of 2 < DBL_MAX_10_EXP) / 16 */
/* Quick_max = floor((P-1)*log(FLT_RADIX)/log(10) - 1) */
/* Int_max = floor(P*log(FLT_RADIX)/log(10) - 1) */

#define Exp_shift  20
#define Exp_shift1 20
#define Exp_msk1    0x100000
#define Exp_msk11   0x100000
#define Exp_mask  0x7ff00000
#define P 53
#define Bias 1023
#define Emin (-1022)
#define Exp_1  0x3ff00000
#define Exp_11 0x3ff00000
#define Ebits 11
#define Frac_mask  0xfffff
#define Frac_mask1 0xfffff
#define Ten_pmax 22
#define Bletch 0x10
#define Bndry_mask  0xfffff
#define Bndry_mask1 0xfffff
#define LSB 1
#define Sign_bit 0x80000000
#define Log2P 1
#define Tiny0 0
#define Tiny1 1
#define Quick_max 14
#define Int_max 14
#define Infinite(x) (word0(x) == 0x7ff00000) /* sufficient test for here */
#ifndef NO_IEEE_Scale
#define Avoid_Underflow
#endif



#ifdef RND_PRODQUOT
#define rounded_product(a,b) a = rnd_prod(a, b)
#define rounded_quotient(a,b) a = rnd_quot(a, b)
extern double rnd_prod(double, double), rnd_quot(double, double);
#else
#define rounded_product(a,b) a *= b
#define rounded_quotient(a,b) a /= b
#endif

#define Big0 (Frac_mask1 | Exp_msk1*(DBL_MAX_EXP+Bias-1))
#define Big1 0xffffffff

#ifndef JS_HAVE_LONG_LONG
#undef ULLong
#else   /* long long available */
#ifndef Llong
#define Llong JSInt64
#endif
#ifndef ULLong
#define ULLong JSUint64
#endif
#endif /* JS_HAVE_LONG_LONG */

#ifdef JS_THREADSAFE
#define MULTIPLE_THREADS
static PRLock *freelist_lock;
#define ACQUIRE_DTOA_LOCK()                                                   \
    JS_BEGIN_MACRO                                                            \
        if (!initialized)                                                     \
            InitDtoa();                                                       \
        PR_Lock(freelist_lock);                                               \
    JS_END_MACRO
#define RELEASE_DTOA_LOCK() PR_Unlock(freelist_lock)
#else
#undef MULTIPLE_THREADS
#define ACQUIRE_DTOA_LOCK()   /*nothing*/
#define RELEASE_DTOA_LOCK()   /*nothing*/
#endif

#define Kmax 15

struct Bigint {
    struct Bigint *next;  /* Free list link */
    int32 k;              /* lg2(maxwds) */
    int32 maxwds;         /* Number of words allocated for x */
    int32 sign;           /* Zero if positive, 1 if negative.  Ignored by most Bigint routines! */
    int32 wds;            /* Actual number of words.  If value is nonzero, the most significant word must be nonzero. */
    ULong x[1];           /* wds words of number in little endian order */
};

#ifdef ENABLE_OOM_TESTING
/* Out-of-memory testing.  Use a good testcase (over and over) and then use
 * these routines to cause a memory failure on every possible Balloc allocation,
 * to make sure that all out-of-memory paths can be followed.  See bug 14044.
 */

static int allocationNum;               /* which allocation is next? */
static int desiredFailure;              /* which allocation should fail? */

/**
 * js_BigintTestingReset
 *
 * Call at the beginning of a test run to set the allocation failure position.
 * (Set to 0 to just have the engine count allocations without failing.)
 */
JS_PUBLIC_API(void)
js_BigintTestingReset(int newFailure)
{
    allocationNum = 0;
    desiredFailure = newFailure;
}

/**
 * js_BigintTestingWhere
 *
 * Report the current allocation position.  This is really only useful when you
 * want to learn how many allocations a test run has.
 */
JS_PUBLIC_API(int)
js_BigintTestingWhere()
{
    return allocationNum;
}


/*
 * So here's what you do: Set up a fantastic test case that exercises the
 * elements of the code you wish.  Set the failure point at 0 and run the test,
 * then get the allocation position.  This number is the number of allocations
 * your test makes.  Now loop from 1 to that number, setting the failure point
 * at each loop count, and run the test over and over, causing failures at each
 * step.  Any memory failure *should* cause a Out-Of-Memory exception; if it
 * doesn't, then there's still an error here.
 */
#endif

typedef struct Bigint Bigint;

static Bigint *freelist[Kmax+1];

/*
 * Allocate a Bigint with 2^k words.
 * This is not threadsafe. The caller must use thread locks
 */
static Bigint *Balloc(int32 k)
{
    int32 x;
    Bigint *rv;
#ifndef Omit_Private_Memory
    uint32 len;
#endif

#ifdef ENABLE_OOM_TESTING
    if (++allocationNum == desiredFailure) {
        printf("Forced Failing Allocation number %d\n", allocationNum);
        return NULL;
    }
#endif

    if ((rv = freelist[k]) != NULL)
        freelist[k] = rv->next;
    if (rv == NULL) {
        x = 1 << k;
#ifdef Omit_Private_Memory
        rv = (Bigint *)MALLOC(sizeof(Bigint) + (x-1)*sizeof(ULong));
#else
        len = (sizeof(Bigint) + (x-1)*sizeof(ULong) + sizeof(double) - 1)
            /sizeof(double);
        if (pmem_next - private_mem + len <= PRIVATE_mem) {
            rv = (Bigint*)pmem_next;
            pmem_next += len;
            }
        else
            rv = (Bigint*)MALLOC(len*sizeof(double));
#endif
        if (!rv)
            return NULL;
        rv->k = k;
        rv->maxwds = x;
    }
    rv->sign = rv->wds = 0;
    return rv;
}

static void Bfree(Bigint *v)
{
    if (v) {
        v->next = freelist[v->k];
        freelist[v->k] = v;
    }
}

#define Bcopy(x,y) memcpy((char *)&x->sign, (char *)&y->sign, \
                          y->wds*sizeof(Long) + 2*sizeof(int32))

/* Return b*m + a.  Deallocate the old b.  Both a and m must be between 0 and
 * 65535 inclusive.  NOTE: old b is deallocated on memory failure.
 */
static Bigint *multadd(Bigint *b, int32 m, int32 a)
{
    int32 i, wds;
#ifdef ULLong
    ULong *x;
    ULLong carry, y;
#else
    ULong carry, *x, y;
    ULong xi, z;
#endif
    Bigint *b1;

#ifdef ENABLE_OOM_TESTING
    if (++allocationNum == desiredFailure) {
        /* Faux allocation, because I'm not getting all of the failure paths
         * without it.
         */
        printf("Forced Failing Allocation number %d\n", allocationNum);
        Bfree(b);
        return NULL;
    }
#endif

    wds = b->wds;
    x = b->x;
    i = 0;
    carry = a;
    do {
#ifdef ULLong
        y = *x * (ULLong)m + carry;
        carry = y >> 32;
        *x++ = (ULong)(y & 0xffffffffUL);
#else
        xi = *x;
        y = (xi & 0xffff) * m + carry;
        z = (xi >> 16) * m + (y >> 16);
        carry = z >> 16;
        *x++ = (z << 16) + (y & 0xffff);
#endif
    }
    while(++i < wds);
    if (carry) {
        if (wds >= b->maxwds) {
            b1 = Balloc(b->k+1);
            if (!b1) {
                Bfree(b);
                return NULL;
            }
            Bcopy(b1, b);
            Bfree(b);
            b = b1;
        }
        b->x[wds++] = (ULong)carry;
        b->wds = wds;
    }
    return b;
}

static Bigint *s2b(CONST char *s, int32 nd0, int32 nd, ULong y9)
{
    Bigint *b;
    int32 i, k;
    Long x, y;

    x = (nd + 8) / 9;
    for(k = 0, y = 1; x > y; y <<= 1, k++) ;
    b = Balloc(k);
    if (!b)
        return NULL;
    b->x[0] = y9;
    b->wds = 1;

    i = 9;
    if (9 < nd0) {
        s += 9;
        do {
            b = multadd(b, 10, *s++ - '0');
            if (!b)
                return NULL;
        } while(++i < nd0);
        s++;
    }
    else
        s += 10;
    for(; i < nd; i++) {
        b = multadd(b, 10, *s++ - '0');
        if (!b)
            return NULL;
    }
    return b;
}


/* Return the number (0 through 32) of most significant zero bits in x. */
static int32 hi0bits(register ULong x)
{
    register int32 k = 0;

    if (!(x & 0xffff0000)) {
        k = 16;
        x <<= 16;
    }
    if (!(x & 0xff000000)) {
        k += 8;
        x <<= 8;
    }
    if (!(x & 0xf0000000)) {
        k += 4;
        x <<= 4;
    }
    if (!(x & 0xc0000000)) {
        k += 2;
        x <<= 2;
    }
    if (!(x & 0x80000000)) {
        k++;
        if (!(x & 0x40000000))
            return 32;
    }
    return k;
}


/* Return the number (0 through 32) of least significant zero bits in y.
 * Also shift y to the right past these 0 through 32 zeros so that y's
 * least significant bit will be set unless y was originally zero. */
static int32 lo0bits(ULong *y)
{
    register int32 k;
    register ULong x = *y;

    if (x & 7) {
        if (x & 1)
            return 0;
        if (x & 2) {
            *y = x >> 1;
            return 1;
        }
        *y = x >> 2;
        return 2;
    }
    k = 0;
    if (!(x & 0xffff)) {
        k = 16;
        x >>= 16;
    }
    if (!(x & 0xff)) {
        k += 8;
        x >>= 8;
    }
    if (!(x & 0xf)) {
        k += 4;
        x >>= 4;
    }
    if (!(x & 0x3)) {
        k += 2;
        x >>= 2;
    }
    if (!(x & 1)) {
        k++;
        x >>= 1;
        if (!x & 1)
            return 32;
    }
    *y = x;
    return k;
}

/* Return a new Bigint with the given integer value, which must be nonnegative. */
static Bigint *i2b(int32 i)
{
    Bigint *b;

    b = Balloc(1);
    if (!b)
        return NULL;
    b->x[0] = i;
    b->wds = 1;
    return b;
}

/* Return a newly allocated product of a and b. */
static Bigint *mult(CONST Bigint *a, CONST Bigint *b)
{
    CONST Bigint *t;
    Bigint *c;
    int32 k, wa, wb, wc;
    ULong y;
    ULong *xc, *xc0, *xce;
    CONST ULong *x, *xa, *xae, *xb, *xbe;
#ifdef ULLong
    ULLong carry, z;
#else
    ULong carry, z;
    ULong z2;
#endif

    if (a->wds < b->wds) {
        t = a;
        a = b;
        b = t;
    }
    k = a->k;
    wa = a->wds;
    wb = b->wds;
    wc = wa + wb;
    if (wc > a->maxwds)
        k++;
    c = Balloc(k);
    if (!c)
        return NULL;
    for(xc = c->x, xce = xc + wc; xc < xce; xc++)
        *xc = 0;
    xa = a->x;
    xae = xa + wa;
    xb = b->x;
    xbe = xb + wb;
    xc0 = c->x;
#ifdef ULLong
    for(; xb < xbe; xc0++) {
        if ((y = *xb++) != 0) {
            x = xa;
            xc = xc0;
            carry = 0;
            do {
                z = *x++ * (ULLong)y + *xc + carry;
                carry = z >> 32;
                *xc++ = (ULong)(z & 0xffffffffUL);
                }
                while(x < xae);
            *xc = (ULong)carry;
            }
        }
#else
    for(; xb < xbe; xb++, xc0++) {
        if ((y = *xb & 0xffff) != 0) {
            x = xa;
            xc = xc0;
            carry = 0;
            do {
                z = (*x & 0xffff) * y + (*xc & 0xffff) + carry;
                carry = z >> 16;
                z2 = (*x++ >> 16) * y + (*xc >> 16) + carry;
                carry = z2 >> 16;
                Storeinc(xc, z2, z);
            }
            while(x < xae);
            *xc = carry;
        }
        if ((y = *xb >> 16) != 0) {
            x = xa;
            xc = xc0;
            carry = 0;
            z2 = *xc;
            do {
                z = (*x & 0xffff) * y + (*xc >> 16) + carry;
                carry = z >> 16;
                Storeinc(xc, z, z2);
                z2 = (*x++ >> 16) * y + (*xc & 0xffff) + carry;
                carry = z2 >> 16;
            }
            while(x < xae);
            *xc = z2;
        }
    }
#endif
    for(xc0 = c->x, xc = xc0 + wc; wc > 0 && !*--xc; --wc) ;
    c->wds = wc;
    return c;
}

/*
 * 'p5s' points to a linked list of Bigints that are powers of 5.
 * This list grows on demand, and it can only grow: it won't change
 * in any other way.  So if we read 'p5s' or the 'next' field of
 * some Bigint on the list, and it is not NULL, we know it won't
 * change to NULL or some other value.  Only when the value of
 * 'p5s' or 'next' is NULL do we need to acquire the lock and add
 * a new Bigint to the list.
 */

static Bigint *p5s;

#ifdef JS_THREADSAFE
static PRLock *p5s_lock;
#endif

/* Return b * 5^k.  Deallocate the old b.  k must be nonnegative. */
/* NOTE: old b is deallocated on memory failure. */
static Bigint *pow5mult(Bigint *b, int32 k)
{
    Bigint *b1, *p5, *p51;
    int32 i;
    static CONST int32 p05[3] = { 5, 25, 125 };

    if ((i = k & 3) != 0) {
        b = multadd(b, p05[i-1], 0);
        if (!b)
            return NULL;
    }

    if (!(k >>= 2))
        return b;
    if (!(p5 = p5s)) {
#ifdef JS_THREADSAFE
        /*
         * We take great care to not call i2b() and Bfree()
         * while holding the lock.
         */
        Bigint *wasted_effort = NULL;
        p5 = i2b(625);
        if (!p5) {
            Bfree(b);
            return NULL;
        }
        /* lock and check again */
        PR_Lock(p5s_lock);
        if (!p5s) {
            /* first time */
            p5s = p5;
            p5->next = 0;
        } else {
            /* some other thread just beat us */
            wasted_effort = p5;
            p5 = p5s;
        }
        PR_Unlock(p5s_lock);
        if (wasted_effort) {
            Bfree(wasted_effort);
        }
#else
        /* first time */
        p5 = p5s = i2b(625);
        if (!p5) {
            Bfree(b);
            return NULL;
        }
        p5->next = 0;
#endif
    }
    for(;;) {
        if (k & 1) {
            b1 = mult(b, p5);
            Bfree(b);
            if (!b1)
                return NULL;
            b = b1;
        }
        if (!(k >>= 1))
            break;
        if (!(p51 = p5->next)) {
#ifdef JS_THREADSAFE
            Bigint *wasted_effort = NULL;
            p51 = mult(p5, p5);
            if (!p51) {
                Bfree(b);
                return NULL;
            }
            PR_Lock(p5s_lock);
            if (!p5->next) {
                p5->next = p51;
                p51->next = 0;
            } else {
                wasted_effort = p51;
                p51 = p5->next;
            }
            PR_Unlock(p5s_lock);
            if (wasted_effort) {
                Bfree(wasted_effort);
            }
#else
            p51 = mult(p5,p5);
            if (!p51) {
                Bfree(b);
                return NULL;
            }
            p51->next = 0;
            p5->next = p51;
#endif
        }
        p5 = p51;
    }
    return b;
}

/* Return b * 2^k.  Deallocate the old b.  k must be nonnegative.
 * NOTE: on memory failure, old b is deallocated. */
static Bigint *lshift(Bigint *b, int32 k)
{
    int32 i, k1, n, n1;
    Bigint *b1;
    ULong *x, *x1, *xe, z;

    n = k >> 5;
    k1 = b->k;
    n1 = n + b->wds + 1;
    for(i = b->maxwds; n1 > i; i <<= 1)
        k1++;
    b1 = Balloc(k1);
    if (!b1)
        goto done;
    x1 = b1->x;
    for(i = 0; i < n; i++)
        *x1++ = 0;
    x = b->x;
    xe = x + b->wds;
    if (k &= 0x1f) {
        k1 = 32 - k;
        z = 0;
        do {
            *x1++ = *x << k | z;
            z = *x++ >> k1;
        }
        while(x < xe);
        if ((*x1 = z) != 0)
            ++n1;
    }
    else do
        *x1++ = *x++;
         while(x < xe);
    b1->wds = n1 - 1;
done:
    Bfree(b);
    return b1;
}

/* Return -1, 0, or 1 depending on whether a<b, a==b, or a>b, respectively. */
static int32 cmp(Bigint *a, Bigint *b)
{
    ULong *xa, *xa0, *xb, *xb0;
    int32 i, j;

    i = a->wds;
    j = b->wds;
#ifdef DEBUG
    if (i > 1 && !a->x[i-1])
        Bug("cmp called with a->x[a->wds-1] == 0");
    if (j > 1 && !b->x[j-1])
        Bug("cmp called with b->x[b->wds-1] == 0");
#endif
    if (i -= j)
        return i;
    xa0 = a->x;
    xa = xa0 + j;
    xb0 = b->x;
    xb = xb0 + j;
    for(;;) {
        if (*--xa != *--xb)
            return *xa < *xb ? -1 : 1;
        if (xa <= xa0)
            break;
    }
    return 0;
}

static Bigint *diff(Bigint *a, Bigint *b)
{
    Bigint *c;
    int32 i, wa, wb;
    ULong *xa, *xae, *xb, *xbe, *xc;
#ifdef ULLong
    ULLong borrow, y;
#else
    ULong borrow, y;
    ULong z;
#endif

    i = cmp(a,b);
    if (!i) {
        c = Balloc(0);
        if (!c)
            return NULL;
        c->wds = 1;
        c->x[0] = 0;
        return c;
    }
    if (i < 0) {
        c = a;
        a = b;
        b = c;
        i = 1;
    }
    else
        i = 0;
    c = Balloc(a->k);
    if (!c)
        return NULL;
    c->sign = i;
    wa = a->wds;
    xa = a->x;
    xae = xa + wa;
    wb = b->wds;
    xb = b->x;
    xbe = xb + wb;
    xc = c->x;
    borrow = 0;
#ifdef ULLong
    do {
        y = (ULLong)*xa++ - *xb++ - borrow;
        borrow = y >> 32 & 1UL;
        *xc++ = (ULong)(y & 0xffffffffUL);
        }
        while(xb < xbe);
    while(xa < xae) {
        y = *xa++ - borrow;
        borrow = y >> 32 & 1UL;
        *xc++ = (ULong)(y & 0xffffffffUL);
        }
#else
    do {
        y = (*xa & 0xffff) - (*xb & 0xffff) - borrow;
        borrow = (y & 0x10000) >> 16;
        z = (*xa++ >> 16) - (*xb++ >> 16) - borrow;
        borrow = (z & 0x10000) >> 16;
        Storeinc(xc, z, y);
        }
        while(xb < xbe);
    while(xa < xae) {
        y = (*xa & 0xffff) - borrow;
        borrow = (y & 0x10000) >> 16;
        z = (*xa++ >> 16) - borrow;
        borrow = (z & 0x10000) >> 16;
        Storeinc(xc, z, y);
        }
#endif
    while(!*--xc)
        wa--;
    c->wds = wa;
    return c;
}

/* Return the absolute difference between x and the adjacent greater-magnitude double number (ignoring exponent overflows). */
static double ulp(double x)
{
    register Long L;
    double a = 0;

    L = (word0(x) & Exp_mask) - (P-1)*Exp_msk1;
#ifndef Sudden_Underflow
    if (L > 0) {
#endif
        set_word0(a, L);
        set_word1(a, 0);
#ifndef Sudden_Underflow
    }
    else {
        L = -L >> Exp_shift;
        if (L < Exp_shift) {
            set_word0(a, 0x80000 >> L);
            set_word1(a, 0);
        }
        else {
            set_word0(a, 0);
            L -= Exp_shift;
            set_word1(a, L >= 31 ? 1 : 1 << (31 - L));
        }
    }
#endif
    return a;
}


static double b2d(Bigint *a, int32 *e)
{
    ULong *xa, *xa0, w, y, z;
    int32 k;
    double d = 0;
#define d0 word0(d)
#define d1 word1(d)
#define set_d0(x) set_word0(d, x)
#define set_d1(x) set_word1(d, x)

    xa0 = a->x;
    xa = xa0 + a->wds;
    y = *--xa;
#ifdef DEBUG
    if (!y) Bug("zero y in b2d");
#endif
    k = hi0bits(y);
    *e = 32 - k;
    if (k < Ebits) {
        set_d0(Exp_1 | y >> (Ebits - k));
        w = xa > xa0 ? *--xa : 0;
        set_d1(y << (32-Ebits + k) | w >> (Ebits - k));
        goto ret_d;
    }
    z = xa > xa0 ? *--xa : 0;
    if (k -= Ebits) {
        set_d0(Exp_1 | y << k | z >> (32 - k));
        y = xa > xa0 ? *--xa : 0;
        set_d1(z << k | y >> (32 - k));
    }
    else {
        set_d0(Exp_1 | y);
        set_d1(z);
    }
  ret_d:
#undef d0
#undef d1
#undef set_d0
#undef set_d1
    return d;
}


/* Convert d into the form b*2^e, where b is an odd integer.  b is the returned
 * Bigint and e is the returned binary exponent.  Return the number of significant
 * bits in b in bits.  d must be finite and nonzero. */
static Bigint *d2b(double d, int32 *e, int32 *bits)
{
    Bigint *b;
    int32 de, i, k;
    ULong *x, y, z;
#define d0 word0(d)
#define d1 word1(d)
#define set_d0(x) set_word0(d, x)
#define set_d1(x) set_word1(d, x)

    b = Balloc(1);
    if (!b)
        return NULL;
    x = b->x;

    z = d0 & Frac_mask;
    set_d0(d0 & 0x7fffffff);  /* clear sign bit, which we ignore */
#ifdef Sudden_Underflow
    de = (int32)(d0 >> Exp_shift);
    z |= Exp_msk11;
#else
    if ((de = (int32)(d0 >> Exp_shift)) != 0)
        z |= Exp_msk1;
#endif
    if ((y = d1) != 0) {
        if ((k = lo0bits(&y)) != 0) {
            x[0] = y | z << (32 - k);
            z >>= k;
        }
        else
            x[0] = y;
        i = b->wds = (x[1] = z) ? 2 : 1;
    }
    else {
        JS_ASSERT(z);
        k = lo0bits(&z);
        x[0] = z;
        i = b->wds = 1;
        k += 32;
    }
#ifndef Sudden_Underflow
    if (de) {
#endif
        *e = de - Bias - (P-1) + k;
        *bits = P - k;
#ifndef Sudden_Underflow
    }
    else {
        *e = de - Bias - (P-1) + 1 + k;
        *bits = 32*i - hi0bits(x[i-1]);
    }
#endif
    return b;
}
#undef d0
#undef d1
#undef set_d0
#undef set_d1


static double ratio(Bigint *a, Bigint *b)
{
    double da, db;
    int32 k, ka, kb;

    da = b2d(a, &ka);
    db = b2d(b, &kb);
    k = ka - kb + 32*(a->wds - b->wds);
    if (k > 0)
        set_word0(da, word0(da) + k*Exp_msk1);
    else {
        k = -k;
        set_word0(db, word0(db) + k*Exp_msk1);
    }
    return da / db;
}

static CONST double
tens[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
    1e20, 1e21, 1e22
};

static CONST double bigtens[] = { 1e16, 1e32, 1e64, 1e128, 1e256 };
static CONST double tinytens[] = { 1e-16, 1e-32, 1e-64, 1e-128,
#ifdef Avoid_Underflow
        9007199254740992.e-256
#else
        1e-256
#endif
        };
/* The factor of 2^53 in tinytens[4] helps us avoid setting the underflow */
/* flag unnecessarily.  It leads to a song and dance at the end of strtod. */
#define Scale_Bit 0x10
#define n_bigtens 5


#ifdef INFNAN_CHECK

#ifndef NAN_WORD0
#define NAN_WORD0 0x7ff80000
#endif

#ifndef NAN_WORD1
#define NAN_WORD1 0
#endif

static int match(CONST char **sp, char *t)
{
    int c, d;
    CONST char *s = *sp;

    while(d = *t++) {
        if ((c = *++s) >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != d)
            return 0;
        }
    *sp = s + 1;
    return 1;
    }
#endif /* INFNAN_CHECK */


#ifdef JS_THREADSAFE
static JSBool initialized = JS_FALSE;

/* hacked replica of nspr _PR_InitDtoa */
static void InitDtoa(void)
{
    freelist_lock = PR_NewLock();
        p5s_lock = PR_NewLock();
    initialized = JS_TRUE;
}
#endif

void js_FinishDtoa(void)
{
    int count;
    Bigint *temp;

#ifdef JS_THREADSAFE
    if (initialized == JS_TRUE) {
        PR_DestroyLock(freelist_lock);
        PR_DestroyLock(p5s_lock);
        initialized = JS_FALSE;
    }
#endif

    /* clear down the freelist array and p5s */

    /* static Bigint *freelist[Kmax+1]; */
    for (count = 0; count <= Kmax; count++) {
        Bigint **listp = &freelist[count];
        while ((temp = *listp) != NULL) {
            *listp = temp->next;
            free(temp);
        }
        freelist[count] = NULL;
    }

    /* static Bigint *p5s; */
    while (p5s) {
        temp = p5s;
        p5s = p5s->next;
        free(temp);
    }
}

/* nspr2 watcom bug ifdef omitted */

JS_FRIEND_API(double)
JS_strtod(CONST char *s00, char **se, int *err)
{
    int32 scale;
    int32 bb2, bb5, bbe, bd2, bd5, bbbits, bs2, c, dsign,
        e, e1, esign, i, j, k, nd, nd0, nf, nz, nz0, sign;
    CONST char *s, *s0, *s1;
    double aadj, aadj1, adj, rv, rv0;
    Long L;
    ULong y, z;
    Bigint *bb, *bb1, *bd, *bd0, *bs, *delta;

    *err = 0;

    bb = bd = bs = delta = NULL;
    sign = nz0 = nz = 0;
    rv = 0.;

    /* Locking for Balloc's shared buffers that will be used in this block */
    ACQUIRE_DTOA_LOCK();

    for(s = s00;;s++) switch(*s) {
    case '-':
        sign = 1;
        /* no break */
    case '+':
        if (*++s)
            goto break2;
        /* no break */
    case 0:
        s = s00;
        goto ret;
    case '\t':
    case '\n':
    case '\v':
    case '\f':
    case '\r':
    case ' ':
        continue;
    default:
        goto break2;
    }
break2:

    if (*s == '0') {
        nz0 = 1;
        while(*++s == '0') ;
        if (!*s)
            goto ret;
    }
    s0 = s;
    y = z = 0;
    for(nd = nf = 0; (c = *s) >= '0' && c <= '9'; nd++, s++)
        if (nd < 9)
            y = 10*y + c - '0';
        else if (nd < 16)
            z = 10*z + c - '0';
    nd0 = nd;
    if (c == '.') {
        c = *++s;
        if (!nd) {
            for(; c == '0'; c = *++s)
                nz++;
            if (c > '0' && c <= '9') {
                s0 = s;
                nf += nz;
                nz = 0;
                goto have_dig;
            }
            goto dig_done;
        }
        for(; c >= '0' && c <= '9'; c = *++s) {
        have_dig:
            nz++;
            if (c -= '0') {
                nf += nz;
                for(i = 1; i < nz; i++)
                    if (nd++ < 9)
                        y *= 10;
                    else if (nd <= DBL_DIG + 1)
                        z *= 10;
                if (nd++ < 9)
                    y = 10*y + c;
                else if (nd <= DBL_DIG + 1)
                    z = 10*z + c;
                nz = 0;
            }
        }
    }
dig_done:
    e = 0;
    if (c == 'e' || c == 'E') {
        if (!nd && !nz && !nz0) {
            s = s00;
            goto ret;
        }
        s00 = s;
        esign = 0;
        switch(c = *++s) {
        case '-':
            esign = 1;
        case '+':
            c = *++s;
        }
        if (c >= '0' && c <= '9') {
            while(c == '0')
                c = *++s;
            if (c > '0' && c <= '9') {
                L = c - '0';
                s1 = s;
                while((c = *++s) >= '0' && c <= '9')
                    L = 10*L + c - '0';
                if (s - s1 > 8 || L > 19999)
                    /* Avoid confusion from exponents
                     * so large that e might overflow.
                     */
                    e = 19999; /* safe for 16 bit ints */
                else
                    e = (int32)L;
                if (esign)
                    e = -e;
            }
            else
                e = 0;
        }
        else
            s = s00;
    }
    if (!nd) {
        if (!nz && !nz0) {
#ifdef INFNAN_CHECK
            /* Check for Nan and Infinity */
            switch(c) {
              case 'i':
              case 'I':
                if (match(&s,"nfinity")) {
                    set_word0(rv, 0x7ff00000);
                    set_word1(rv, 0);
                    goto ret;
                    }
                break;
              case 'n':
              case 'N':
                if (match(&s, "an")) {
                    set_word0(rv, NAN_WORD0);
                    set_word1(rv, NAN_WORD1);
                    goto ret;
                    }
              }
#endif /* INFNAN_CHECK */
            s = s00;
            }
        goto ret;
    }
    e1 = e -= nf;

    /* Now we have nd0 digits, starting at s0, followed by a
     * decimal point, followed by nd-nd0 digits.  The number we're
     * after is the integer represented by those digits times
     * 10**e */

    if (!nd0)
        nd0 = nd;
    k = nd < DBL_DIG + 1 ? nd : DBL_DIG + 1;
    rv = y;
    if (k > 9)
        rv = tens[k - 9] * rv + z;
    bd0 = 0;
    if (nd <= DBL_DIG
#ifndef RND_PRODQUOT
        && FLT_ROUNDS == 1
#endif
        ) {
        if (!e)
            goto ret;
        if (e > 0) {
            if (e <= Ten_pmax) {
                /* rv = */ rounded_product(rv, tens[e]);
                goto ret;
            }
            i = DBL_DIG - nd;
            if (e <= Ten_pmax + i) {
                /* A fancier test would sometimes let us do
                 * this for larger i values.
                 */
                e -= i;
                rv *= tens[i];
                /* rv = */ rounded_product(rv, tens[e]);
                goto ret;
            }
        }
#ifndef Inaccurate_Divide
        else if (e >= -Ten_pmax) {
            /* rv = */ rounded_quotient(rv, tens[-e]);
            goto ret;
        }
#endif
    }
    e1 += nd - k;

    scale = 0;

    /* Get starting approximation = rv * 10**e1 */

    if (e1 > 0) {
        if ((i = e1 & 15) != 0)
            rv *= tens[i];
        if (e1 &= ~15) {
            if (e1 > DBL_MAX_10_EXP) {
            ovfl:
                *err = JS_DTOA_ERANGE;
#ifdef __STDC__
                rv = HUGE_VAL;
#else
                /* Can't trust HUGE_VAL */
                set_word0(rv, Exp_mask);
                set_word1(rv, 0);
#endif
                if (bd0)
                    goto retfree;
                goto ret;
            }
            e1 >>= 4;
            for(j = 0; e1 > 1; j++, e1 >>= 1)
                if (e1 & 1)
                    rv *= bigtens[j];
            /* The last multiplication could overflow. */
            set_word0(rv, word0(rv) - P*Exp_msk1);
            rv *= bigtens[j];
            if ((z = word0(rv) & Exp_mask) > Exp_msk1*(DBL_MAX_EXP+Bias-P))
                goto ovfl;
            if (z > Exp_msk1*(DBL_MAX_EXP+Bias-1-P)) {
                /* set to largest number */
                /* (Can't trust DBL_MAX) */
                set_word0(rv, Big0);
                set_word1(rv, Big1);
                }
            else
                set_word0(rv, word0(rv) + P*Exp_msk1);
            }
    }
    else if (e1 < 0) {
        e1 = -e1;
        if ((i = e1 & 15) != 0)
            rv /= tens[i];
        if (e1 &= ~15) {
            e1 >>= 4;
            if (e1 >= 1 << n_bigtens)
                goto undfl;
#ifdef Avoid_Underflow
            if (e1 & Scale_Bit)
                scale = P;
            for(j = 0; e1 > 0; j++, e1 >>= 1)
                if (e1 & 1)
                    rv *= tinytens[j];
            if (scale && (j = P + 1 - ((word0(rv) & Exp_mask)
                        >> Exp_shift)) > 0) {
                /* scaled rv is denormal; zap j low bits */
                if (j >= 32) {
                    set_word1(rv, 0);
                    set_word0(rv, word0(rv) & (0xffffffff << (j-32)));
                    if (!word0(rv))
                        set_word0(rv, 1);
                    }
                else
                    set_word1(rv, word1(rv) & (0xffffffff << j));
                }
#else
            for(j = 0; e1 > 1; j++, e1 >>= 1)
                if (e1 & 1)
                    rv *= tinytens[j];
            /* The last multiplication could underflow. */
            rv0 = rv;
            rv *= tinytens[j];
            if (!rv) {
                rv = 2.*rv0;
                rv *= tinytens[j];
#endif
                if (!rv) {
                undfl:
                    rv = 0.;
                    *err = JS_DTOA_ERANGE;
                    if (bd0)
                        goto retfree;
                    goto ret;
                }
#ifndef Avoid_Underflow
                set_word0(rv, Tiny0);
                set_word1(rv, Tiny1);
                /* The refinement below will clean
                 * this approximation up.
                 */
            }
#endif
        }
    }

    /* Now the hard part -- adjusting rv to the correct value.*/

    /* Put digits into bd: true value = bd * 10^e */

    bd0 = s2b(s0, nd0, nd, y);
    if (!bd0)
        goto nomem;

    for(;;) {
        bd = Balloc(bd0->k);
        if (!bd)
            goto nomem;
        Bcopy(bd, bd0);
        bb = d2b(rv, &bbe, &bbbits);    /* rv = bb * 2^bbe */
        if (!bb)
            goto nomem;
        bs = i2b(1);
        if (!bs)
            goto nomem;

        if (e >= 0) {
            bb2 = bb5 = 0;
            bd2 = bd5 = e;
        }
        else {
            bb2 = bb5 = -e;
            bd2 = bd5 = 0;
        }
        if (bbe >= 0)
            bb2 += bbe;
        else
            bd2 -= bbe;
        bs2 = bb2;
#ifdef Sudden_Underflow
        j = P + 1 - bbbits;
#else
#ifdef Avoid_Underflow
        j = bbe - scale;
#else
        j = bbe;
#endif
        i = j + bbbits - 1; /* logb(rv) */
        if (i < Emin)   /* denormal */
            j += P - Emin;
        else
            j = P + 1 - bbbits;
#endif
        bb2 += j;
        bd2 += j;
#ifdef Avoid_Underflow
        bd2 += scale;
#endif
        i = bb2 < bd2 ? bb2 : bd2;
        if (i > bs2)
            i = bs2;
        if (i > 0) {
            bb2 -= i;
            bd2 -= i;
            bs2 -= i;
        }
        if (bb5 > 0) {
            bs = pow5mult(bs, bb5);
            if (!bs)
                goto nomem;
            bb1 = mult(bs, bb);
            if (!bb1)
                goto nomem;
            Bfree(bb);
            bb = bb1;
        }
        if (bb2 > 0) {
            bb = lshift(bb, bb2);
            if (!bb)
                goto nomem;
        }
        if (bd5 > 0) {
            bd = pow5mult(bd, bd5);
            if (!bd)
                goto nomem;
        }
        if (bd2 > 0) {
            bd = lshift(bd, bd2);
            if (!bd)
                goto nomem;
        }
        if (bs2 > 0) {
            bs = lshift(bs, bs2);
            if (!bs)
                goto nomem;
        }
        delta = diff(bb, bd);
        if (!delta)
            goto nomem;
        dsign = delta->sign;
        delta->sign = 0;
        i = cmp(delta, bs);
        if (i < 0) {
            /* Error is less than half an ulp -- check for
             * special case of mantissa a power of two.
             */
            if (dsign || word1(rv) || word0(rv) & Bndry_mask
#ifdef Avoid_Underflow
             || (word0(rv) & Exp_mask) <= Exp_msk1 + P*Exp_msk1
#else
             || (word0(rv) & Exp_mask) <= Exp_msk1
#endif
                ) {
#ifdef Avoid_Underflow
                if (!delta->x[0] && delta->wds == 1)
                    dsign = 2;
#endif
                break;
                }
            delta = lshift(delta,Log2P);
            if (!delta)
                goto nomem;
            if (cmp(delta, bs) > 0)
                goto drop_down;
            break;
        }
        if (i == 0) {
            /* exactly half-way between */
            if (dsign) {
                if ((word0(rv) & Bndry_mask1) == Bndry_mask1
                    &&  word1(rv) == 0xffffffff) {
                    /*boundary case -- increment exponent*/
                    set_word0(rv, (word0(rv) & Exp_mask) + Exp_msk1);
                    set_word1(rv, 0);
#ifdef Avoid_Underflow
                    dsign = 0;
#endif
                    break;
                }
            }
            else if (!(word0(rv) & Bndry_mask) && !word1(rv)) {
#ifdef Avoid_Underflow
                dsign = 2;
#endif
            drop_down:
                /* boundary case -- decrement exponent */
#ifdef Sudden_Underflow
                L = word0(rv) & Exp_mask;
                if (L <= Exp_msk1)
                    goto undfl;
                L -= Exp_msk1;
#else
                L = (word0(rv) & Exp_mask) - Exp_msk1;
#endif
                set_word0(rv, L | Bndry_mask1);
                set_word1(rv, 0xffffffff);
                break;
            }
#ifndef ROUND_BIASED
            if (!(word1(rv) & LSB))
                break;
#endif
            if (dsign)
                rv += ulp(rv);
#ifndef ROUND_BIASED
            else {
                rv -= ulp(rv);
#ifndef Sudden_Underflow
                if (!rv)
                    goto undfl;
#endif
            }
#ifdef Avoid_Underflow
            dsign = 1 - dsign;
#endif
#endif
            break;
        }
        if ((aadj = ratio(delta, bs)) <= 2.) {
            if (dsign)
                aadj = aadj1 = 1.;
            else if (word1(rv) || word0(rv) & Bndry_mask) {
#ifndef Sudden_Underflow
                if (word1(rv) == Tiny1 && !word0(rv))
                    goto undfl;
#endif
                aadj = 1.;
                aadj1 = -1.;
            }
            else {
                /* special case -- power of FLT_RADIX to be */
                /* rounded down... */

                if (aadj < 2./FLT_RADIX)
                    aadj = 1./FLT_RADIX;
                else
                    aadj *= 0.5;
                aadj1 = -aadj;
            }
        }
        else {
            aadj *= 0.5;
            aadj1 = dsign ? aadj : -aadj;
#ifdef Check_FLT_ROUNDS
            switch(FLT_ROUNDS) {
            case 2: /* towards +infinity */
                aadj1 -= 0.5;
                break;
            case 0: /* towards 0 */
            case 3: /* towards -infinity */
                aadj1 += 0.5;
            }
#else
            if (FLT_ROUNDS == 0)
                aadj1 += 0.5;
#endif
        }
        y = word0(rv) & Exp_mask;

        /* Check for overflow */

        if (y == Exp_msk1*(DBL_MAX_EXP+Bias-1)) {
            rv0 = rv;
            set_word0(rv, word0(rv) - P*Exp_msk1);
            adj = aadj1 * ulp(rv);
            rv += adj;
            if ((word0(rv) & Exp_mask) >=
                Exp_msk1*(DBL_MAX_EXP+Bias-P)) {
                if (word0(rv0) == Big0 && word1(rv0) == Big1)
                    goto ovfl;
                set_word0(rv, Big0);
                set_word1(rv, Big1);
                goto cont;
            }
            else
                set_word0(rv, word0(rv) + P*Exp_msk1);
        }
        else {
#ifdef Sudden_Underflow
            if ((word0(rv) & Exp_mask) <= P*Exp_msk1) {
                rv0 = rv;
                set_word0(rv, word0(rv) + P*Exp_msk1);
                adj = aadj1 * ulp(rv);
                rv += adj;
                    if ((word0(rv) & Exp_mask) <= P*Exp_msk1)
                        {
                            if (word0(rv0) == Tiny0
                                && word1(rv0) == Tiny1)
                                goto undfl;
                            set_word0(rv, Tiny0);
                            set_word1(rv, Tiny1);
                            goto cont;
                        }
                    else
                        set_word0(rv, word0(rv) - P*Exp_msk1);
            }
            else {
                adj = aadj1 * ulp(rv);
                rv += adj;
            }
#else
            /* Compute adj so that the IEEE rounding rules will
             * correctly round rv + adj in some half-way cases.
             * If rv * ulp(rv) is denormalized (i.e.,
             * y <= (P-1)*Exp_msk1), we must adjust aadj to avoid
             * trouble from bits lost to denormalization;
             * example: 1.2e-307 .
             */
#ifdef Avoid_Underflow
            if (y <= P*Exp_msk1 && aadj > 1.)
#else
            if (y <= (P-1)*Exp_msk1 && aadj > 1.)
#endif
                {
                aadj1 = (double)(int32)(aadj + 0.5);
                if (!dsign)
                    aadj1 = -aadj1;
            }
#ifdef Avoid_Underflow
            if (scale && y <= P*Exp_msk1)
                set_word0(aadj1, word0(aadj1) + (P+1)*Exp_msk1 - y);
#endif
            adj = aadj1 * ulp(rv);
            rv += adj;
#endif
        }
        z = word0(rv) & Exp_mask;
#ifdef Avoid_Underflow
        if (!scale)
#endif
        if (y == z) {
            /* Can we stop now? */
            L = (Long)aadj;
            aadj -= L;
            /* The tolerances below are conservative. */
            if (dsign || word1(rv) || word0(rv) & Bndry_mask) {
                if (aadj < .4999999 || aadj > .5000001)
                    break;
            }
            else if (aadj < .4999999/FLT_RADIX)
                break;
        }
    cont:
        Bfree(bb);
        Bfree(bd);
        Bfree(bs);
        Bfree(delta);
        bb = bd = bs = delta = NULL;
    }
#ifdef Avoid_Underflow
    if (scale) {
        rv0 = 0.;
        set_word0(rv0, Exp_1 - P*Exp_msk1);
        set_word1(rv0, 0);
        if ((word0(rv) & Exp_mask) <= P*Exp_msk1
              && word1(rv) & 1
              && dsign != 2) {
            if (dsign) {
#ifdef Sudden_Underflow
                /* rv will be 0, but this would give the  */
                /* right result if only rv *= rv0 worked. */
                set_word0(rv, word0(rv) + P*Exp_msk1);
                set_word0(rv0, Exp_1 - 2*P*Exp_msk1);
#endif
                rv += ulp(rv);
                }
            else
                set_word1(rv, word1(rv) & ~1);
        }
        rv *= rv0;
    }
#endif /* Avoid_Underflow */
retfree:
    Bfree(bb);
    Bfree(bd);
    Bfree(bs);
    Bfree(bd0);
    Bfree(delta);
ret:
    RELEASE_DTOA_LOCK();
    if (se)
        *se = (char *)s;
    return sign ? -rv : rv;

nomem:
    Bfree(bb);
    Bfree(bd);
    Bfree(bs);
    Bfree(bd0);
    Bfree(delta);
    RELEASE_DTOA_LOCK();
    *err = JS_DTOA_ENOMEM;
    return 0;
}


/* Return floor(b/2^k) and set b to be the remainder.  The returned quotient must be less than 2^32. */
static uint32 quorem2(Bigint *b, int32 k)
{
    ULong mask;
    ULong result;
    ULong *bx, *bxe;
    int32 w;
    int32 n = k >> 5;
    k &= 0x1F;
    mask = (1<<k) - 1;

    w = b->wds - n;
    if (w <= 0)
        return 0;
    JS_ASSERT(w <= 2);
    bx = b->x;
    bxe = bx + n;
    result = *bxe >> k;
    *bxe &= mask;
    if (w == 2) {
        JS_ASSERT(!(bxe[1] & ~mask));
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

/* Return floor(b/S) and set b to be the remainder.  As added restrictions, b must not have
 * more words than S, the most significant word of S must not start with a 1 bit, and the
 * returned quotient must be less than 36. */
static int32 quorem(Bigint *b, Bigint *S)
{
    int32 n;
    ULong *bx, *bxe, q, *sx, *sxe;
#ifdef ULLong
    ULLong borrow, carry, y, ys;
#else
    ULong borrow, carry, y, ys;
    ULong si, z, zs;
#endif

    n = S->wds;
    JS_ASSERT(b->wds <= n);
    if (b->wds < n)
        return 0;
    sx = S->x;
    sxe = sx + --n;
    bx = b->x;
    bxe = bx + n;
    JS_ASSERT(*sxe <= 0x7FFFFFFF);
    q = *bxe / (*sxe + 1);  /* ensure q <= true quotient */
    JS_ASSERT(q < 36);
    if (q) {
        borrow = 0;
        carry = 0;
        do {
#ifdef ULLong
            ys = *sx++ * (ULLong)q + carry;
            carry = ys >> 32;
            y = *bx - (ys & 0xffffffffUL) - borrow;
            borrow = y >> 32 & 1UL;
            *bx++ = (ULong)(y & 0xffffffffUL);
#else
            si = *sx++;
            ys = (si & 0xffff) * q + carry;
            zs = (si >> 16) * q + (ys >> 16);
            carry = zs >> 16;
            y = (*bx & 0xffff) - (ys & 0xffff) - borrow;
            borrow = (y & 0x10000) >> 16;
            z = (*bx >> 16) - (zs & 0xffff) - borrow;
            borrow = (z & 0x10000) >> 16;
            Storeinc(bx, z, y);
#endif
        }
        while(sx <= sxe);
        if (!*bxe) {
            bx = b->x;
            while(--bxe > bx && !*bxe)
                --n;
            b->wds = n;
        }
    }
    if (cmp(b, S) >= 0) {
        q++;
        borrow = 0;
        carry = 0;
        bx = b->x;
        sx = S->x;
        do {
#ifdef ULLong
            ys = *sx++ + carry;
            carry = ys >> 32;
            y = *bx - (ys & 0xffffffffUL) - borrow;
            borrow = y >> 32 & 1UL;
            *bx++ = (ULong)(y & 0xffffffffUL);
#else
            si = *sx++;
            ys = (si & 0xffff) + carry;
            zs = (si >> 16) + (ys >> 16);
            carry = zs >> 16;
            y = (*bx & 0xffff) - (ys & 0xffff) - borrow;
            borrow = (y & 0x10000) >> 16;
            z = (*bx >> 16) - (zs & 0xffff) - borrow;
            borrow = (z & 0x10000) >> 16;
            Storeinc(bx, z, y);
#endif
        } while(sx <= sxe);
        bx = b->x;
        bxe = bx + n;
        if (!*bxe) {
            while(--bxe > bx && !*bxe)
                --n;
            b->wds = n;
        }
    }
    return (int32)q;
}

/* dtoa for IEEE arithmetic (dmg): convert double to ASCII string.
 *
 * Inspired by "How to Print Floating-Point Numbers Accurately" by
 * Guy L. Steele, Jr. and Jon L. White [Proc. ACM SIGPLAN '90, pp. 92-101].
 *
 * Modifications:
 *  1. Rather than iterating, we use a simple numeric overestimate
 *     to determine k = floor(log10(d)).  We scale relevant
 *     quantities using O(log2(k)) rather than O(k) multiplications.
 *  2. For some modes > 2 (corresponding to ecvt and fcvt), we don't
 *     try to generate digits strictly left to right.  Instead, we
 *     compute with fewer bits and propagate the carry if necessary
 *     when rounding the final digit up.  This is often faster.
 *  3. Under the assumption that input will be rounded nearest,
 *     mode 0 renders 1e23 as 1e23 rather than 9.999999999999999e22.
 *     That is, we allow equality in stopping tests when the
 *     round-nearest rule will give the same floating-point value
 *     as would satisfaction of the stopping test with strict
 *     inequality.
 *  4. We remove common factors of powers of 2 from relevant
 *     quantities.
 *  5. When converting floating-point integers less than 1e16,
 *     we use floating-point arithmetic rather than resorting
 *     to multiple-precision integers.
 *  6. When asked to produce fewer than 15 digits, we first try
 *     to get by with floating-point arithmetic; we resort to
 *     multiple-precision integer arithmetic only if we cannot
 *     guarantee that the floating-point calculation has given
 *     the correctly rounded result.  For k requested digits and
 *     "uniformly" distributed input, the probability is
 *     something like 10^(k-15) that we must resort to the Long
 *     calculation.
 */

/* Always emits at least one digit. */
/* If biasUp is set, then rounding in modes 2 and 3 will round away from zero
 * when the number is exactly halfway between two representable values.  For example,
 * rounding 2.5 to zero digits after the decimal point will return 3 and not 2.
 * 2.49 will still round to 2, and 2.51 will still round to 3. */
/* bufsize should be at least 20 for modes 0 and 1.  For the other modes,
 * bufsize should be two greater than the maximum number of output characters expected. */
static JSBool
js_dtoa(double d, int mode, JSBool biasUp, int ndigits,
    int *decpt, int *sign, char **rve, char *buf, size_t bufsize)
{
    /*  Arguments ndigits, decpt, sign are similar to those
        of ecvt and fcvt; trailing zeros are suppressed from
        the returned string.  If not null, *rve is set to point
        to the end of the return value.  If d is +-Infinity or NaN,
        then *decpt is set to 9999.

        mode:
        0 ==> shortest string that yields d when read in
        and rounded to nearest.
        1 ==> like 0, but with Steele & White stopping rule;
        e.g. with IEEE P754 arithmetic , mode 0 gives
        1e23 whereas mode 1 gives 9.999999999999999e22.
        2 ==> max(1,ndigits) significant digits.  This gives a
        return value similar to that of ecvt, except
        that trailing zeros are suppressed.
        3 ==> through ndigits past the decimal point.  This
        gives a return value similar to that from fcvt,
        except that trailing zeros are suppressed, and
        ndigits can be negative.
        4-9 should give the same return values as 2-3, i.e.,
        4 <= mode <= 9 ==> same return as mode
        2 + (mode & 1).  These modes are mainly for
        debugging; often they run slower but sometimes
        faster than modes 2-3.
        4,5,8,9 ==> left-to-right digit generation.
        6-9 ==> don't try fast floating-point estimate
        (if applicable).

        Values of mode other than 0-9 are treated as mode 0.

        Sufficient space is allocated to the return value
        to hold the suppressed trailing zeros.
    */

    int32 bbits, b2, b5, be, dig, i, ieps, ilim, ilim0, ilim1,
        j, j1, k, k0, k_check, leftright, m2, m5, s2, s5,
        spec_case, try_quick;
    Long L;
#ifndef Sudden_Underflow
    int32 denorm;
    ULong x;
#endif
    Bigint *b, *b1, *delta, *mlo, *mhi, *S;
    double d2, ds, eps;
    char *s;

    if (word0(d) & Sign_bit) {
        /* set sign for everything, including 0's and NaNs */
        *sign = 1;
        set_word0(d, word0(d) & ~Sign_bit);  /* clear sign bit */
    }
    else
        *sign = 0;

    if ((word0(d) & Exp_mask) == Exp_mask) {
        /* Infinity or NaN */
        *decpt = 9999;
        s = !word1(d) && !(word0(d) & Frac_mask) ? "Infinity" : "NaN";
        if ((s[0] == 'I' && bufsize < 9) || (s[0] == 'N' && bufsize < 4)) {
            JS_ASSERT(JS_FALSE);
/*          JS_SetError(JS_BUFFER_OVERFLOW_ERROR, 0); */
            return JS_FALSE;
        }
        strcpy(buf, s);
        if (rve) {
            *rve = buf[3] ? buf + 8 : buf + 3;
            JS_ASSERT(**rve == '\0');
        }
        return JS_TRUE;
    }

    b = NULL;                           /* initialize for abort protection */
    S = NULL;
    mlo = mhi = NULL;

    if (!d) {
      no_digits:
        *decpt = 1;
        if (bufsize < 2) {
            JS_ASSERT(JS_FALSE);
/*          JS_SetError(JS_BUFFER_OVERFLOW_ERROR, 0); */
            return JS_FALSE;
        }
        buf[0] = '0'; buf[1] = '\0';  /* copy "0" to buffer */
        if (rve)
            *rve = buf + 1;
        /* We might have jumped to "no_digits" from below, so we need
         * to be sure to free the potentially allocated Bigints to avoid
         * memory leaks. */
        Bfree(b);
        Bfree(S);
        if (mlo != mhi)
            Bfree(mlo);
        Bfree(mhi);
        return JS_TRUE;
    }

    b = d2b(d, &be, &bbits);
    if (!b)
        goto nomem;
#ifdef Sudden_Underflow
    i = (int32)(word0(d) >> Exp_shift1 & (Exp_mask>>Exp_shift1));
#else
    if ((i = (int32)(word0(d) >> Exp_shift1 & (Exp_mask>>Exp_shift1))) != 0) {
#endif
        d2 = d;
        set_word0(d2, word0(d2) & Frac_mask1);
        set_word0(d2, word0(d2) | Exp_11);

        /* log(x)   ~=~ log(1.5) + (x-1.5)/1.5
         * log10(x)  =  log(x) / log(10)
         *      ~=~ log(1.5)/log(10) + (x-1.5)/(1.5*log(10))
         * log10(d) = (i-Bias)*log(2)/log(10) + log10(d2)
         *
         * This suggests computing an approximation k to log10(d) by
         *
         * k = (i - Bias)*0.301029995663981
         *  + ( (d2-1.5)*0.289529654602168 + 0.176091259055681 );
         *
         * We want k to be too large rather than too small.
         * The error in the first-order Taylor series approximation
         * is in our favor, so we just round up the constant enough
         * to compensate for any error in the multiplication of
         * (i - Bias) by 0.301029995663981; since |i - Bias| <= 1077,
         * and 1077 * 0.30103 * 2^-52 ~=~ 7.2e-14,
         * adding 1e-13 to the constant term more than suffices.
         * Hence we adjust the constant term to 0.1760912590558.
         * (We could get a more accurate k by invoking log10,
         *  but this is probably not worthwhile.)
         */

        i -= Bias;
#ifndef Sudden_Underflow
        denorm = 0;
    }
    else {
        /* d is denormalized */

        i = bbits + be + (Bias + (P-1) - 1);
        x = i > 32 ? word0(d) << (64 - i) | word1(d) >> (i - 32) : word1(d) << (32 - i);
        d2 = x;
        set_word0(d2, word0(d2) - 31*Exp_msk1); /* adjust exponent */
        i -= (Bias + (P-1) - 1) + 1;
        denorm = 1;
    }
#endif
    /* At this point d = f*2^i, where 1 <= f < 2.  d2 is an approximation of f. */
    ds = (d2-1.5)*0.289529654602168 + 0.1760912590558 + i*0.301029995663981;
    k = (int32)ds;
    if (ds < 0. && ds != k)
        k--;    /* want k = floor(ds) */
    k_check = 1;
    if (k >= 0 && k <= Ten_pmax) {
        if (d < tens[k])
            k--;
        k_check = 0;
    }
    /* At this point floor(log10(d)) <= k <= floor(log10(d))+1.
       If k_check is zero, we're guaranteed that k = floor(log10(d)). */
    j = bbits - i - 1;
    /* At this point d = b/2^j, where b is an odd integer. */
    if (j >= 0) {
        b2 = 0;
        s2 = j;
    }
    else {
        b2 = -j;
        s2 = 0;
    }
    if (k >= 0) {
        b5 = 0;
        s5 = k;
        s2 += k;
    }
    else {
        b2 -= k;
        b5 = -k;
        s5 = 0;
    }
    /* At this point d/10^k = (b * 2^b2 * 5^b5) / (2^s2 * 5^s5), where b is an odd integer,
       b2 >= 0, b5 >= 0, s2 >= 0, and s5 >= 0. */
    if (mode < 0 || mode > 9)
        mode = 0;
    try_quick = 1;
    if (mode > 5) {
        mode -= 4;
        try_quick = 0;
    }
    leftright = 1;
    ilim = ilim1 = 0;
    switch(mode) {
    case 0:
    case 1:
        ilim = ilim1 = -1;
        i = 18;
        ndigits = 0;
        break;
    case 2:
        leftright = 0;
        /* no break */
    case 4:
        if (ndigits <= 0)
            ndigits = 1;
        ilim = ilim1 = i = ndigits;
        break;
    case 3:
        leftright = 0;
        /* no break */
    case 5:
        i = ndigits + k + 1;
        ilim = i;
        ilim1 = i - 1;
        if (i <= 0)
            i = 1;
    }
    /* ilim is the maximum number of significant digits we want, based on k and ndigits. */
    /* ilim1 is the maximum number of significant digits we want, based on k and ndigits,
       when it turns out that k was computed too high by one. */

    /* Ensure space for at least i+1 characters, including trailing null. */
    if (bufsize <= (size_t)i) {
        Bfree(b);
        JS_ASSERT(JS_FALSE);
        return JS_FALSE;
    }
    s = buf;

    if (ilim >= 0 && ilim <= Quick_max && try_quick) {

        /* Try to get by with floating-point arithmetic. */

        i = 0;
        d2 = d;
        k0 = k;
        ilim0 = ilim;
        ieps = 2; /* conservative */
        /* Divide d by 10^k, keeping track of the roundoff error and avoiding overflows. */
        if (k > 0) {
            ds = tens[k&0xf];
            j = k >> 4;
            if (j & Bletch) {
                /* prevent overflows */
                j &= Bletch - 1;
                d /= bigtens[n_bigtens-1];
                ieps++;
            }
            for(; j; j >>= 1, i++)
                if (j & 1) {
                    ieps++;
                    ds *= bigtens[i];
                }
            d /= ds;
        }
        else if ((j1 = -k) != 0) {
            d *= tens[j1 & 0xf];
            for(j = j1 >> 4; j; j >>= 1, i++)
                if (j & 1) {
                    ieps++;
                    d *= bigtens[i];
                }
        }
        /* Check that k was computed correctly. */
        if (k_check && d < 1. && ilim > 0) {
            if (ilim1 <= 0)
                goto fast_failed;
            ilim = ilim1;
            k--;
            d *= 10.;
            ieps++;
        }
        /* eps bounds the cumulative error. */
        eps = ieps*d + 7.;
        set_word0(eps, word0(eps) - (P-1)*Exp_msk1);
        if (ilim == 0) {
            S = mhi = 0;
            d -= 5.;
            if (d > eps)
                goto one_digit;
            if (d < -eps)
                goto no_digits;
            goto fast_failed;
        }
#ifndef No_leftright
        if (leftright) {
            /* Use Steele & White method of only
             * generating digits needed.
             */
            eps = 0.5/tens[ilim-1] - eps;
            for(i = 0;;) {
                L = (Long)d;
                d -= L;
                *s++ = '0' + (char)L;
                if (d < eps)
                    goto ret1;
                if (1. - d < eps)
                    goto bump_up;
                if (++i >= ilim)
                    break;
                eps *= 10.;
                d *= 10.;
            }
        }
        else {
#endif
            /* Generate ilim digits, then fix them up. */
            eps *= tens[ilim-1];
            for(i = 1;; i++, d *= 10.) {
                L = (Long)d;
                d -= L;
                *s++ = '0' + (char)L;
                if (i == ilim) {
                    if (d > 0.5 + eps)
                        goto bump_up;
                    else if (d < 0.5 - eps) {
                        while(*--s == '0') ;
                        s++;
                        goto ret1;
                    }
                    break;
                }
            }
#ifndef No_leftright
        }
#endif
    fast_failed:
        s = buf;
        d = d2;
        k = k0;
        ilim = ilim0;
    }

    /* Do we have a "small" integer? */

    if (be >= 0 && k <= Int_max) {
        /* Yes. */
        ds = tens[k];
        if (ndigits < 0 && ilim <= 0) {
            S = mhi = 0;
            if (ilim < 0 || d < 5*ds || (!biasUp && d == 5*ds))
                goto no_digits;
            goto one_digit;
        }

        /* Use true number of digits to limit looping. */
        for(i = 1; i<=k+1; i++) {
            L = (Long) (d / ds);
            d -= L*ds;
#ifdef Check_FLT_ROUNDS
            /* If FLT_ROUNDS == 2, L will usually be high by 1 */
            if (d < 0) {
                L--;
                d += ds;
            }
#endif
            *s++ = '0' + (char)L;
            if (i == ilim) {
                d += d;
                if ((d > ds) || (d == ds && (L & 1 || biasUp))) {
                bump_up:
                    while(*--s == '9')
                        if (s == buf) {
                            k++;
                            *s = '0';
                            break;
                        }
                    ++*s++;
                }
                break;
            }
            d *= 10.;
        }
        goto ret1;
    }

    m2 = b2;
    m5 = b5;
    if (leftright) {
        if (mode < 2) {
            i =
#ifndef Sudden_Underflow
                denorm ? be + (Bias + (P-1) - 1 + 1) :
#endif
            1 + P - bbits;
            /* i is 1 plus the number of trailing zero bits in d's significand. Thus,
               (2^m2 * 5^m5) / (2^(s2+i) * 5^s5) = (1/2 lsb of d)/10^k. */
        }
        else {
            j = ilim - 1;
            if (m5 >= j)
                m5 -= j;
            else {
                s5 += j -= m5;
                b5 += j;
                m5 = 0;
            }
            if ((i = ilim) < 0) {
                m2 -= i;
                i = 0;
            }
            /* (2^m2 * 5^m5) / (2^(s2+i) * 5^s5) = (1/2 * 10^(1-ilim))/10^k. */
        }
        b2 += i;
        s2 += i;
        mhi = i2b(1);
        if (!mhi)
            goto nomem;
        /* (mhi * 2^m2 * 5^m5) / (2^s2 * 5^s5) = one-half of last printed (when mode >= 2) or
           input (when mode < 2) significant digit, divided by 10^k. */
    }
    /* We still have d/10^k = (b * 2^b2 * 5^b5) / (2^s2 * 5^s5).  Reduce common factors in
       b2, m2, and s2 without changing the equalities. */
    if (m2 > 0 && s2 > 0) {
        i = m2 < s2 ? m2 : s2;
        b2 -= i;
        m2 -= i;
        s2 -= i;
    }

    /* Fold b5 into b and m5 into mhi. */
    if (b5 > 0) {
        if (leftright) {
            if (m5 > 0) {
                mhi = pow5mult(mhi, m5);
                if (!mhi)
                    goto nomem;
                b1 = mult(mhi, b);
                if (!b1)
                    goto nomem;
                Bfree(b);
                b = b1;
            }
            if ((j = b5 - m5) != 0) {
                b = pow5mult(b, j);
                if (!b)
                    goto nomem;
            }
        }
        else {
            b = pow5mult(b, b5);
            if (!b)
                goto nomem;
        }
    }
    /* Now we have d/10^k = (b * 2^b2) / (2^s2 * 5^s5) and
       (mhi * 2^m2) / (2^s2 * 5^s5) = one-half of last printed or input significant digit, divided by 10^k. */

    S = i2b(1);
    if (!S)
        goto nomem;
    if (s5 > 0) {
        S = pow5mult(S, s5);
        if (!S)
            goto nomem;
    }
    /* Now we have d/10^k = (b * 2^b2) / (S * 2^s2) and
       (mhi * 2^m2) / (S * 2^s2) = one-half of last printed or input significant digit, divided by 10^k. */

    /* Check for special case that d is a normalized power of 2. */
    spec_case = 0;
    if (mode < 2) {
        if (!word1(d) && !(word0(d) & Bndry_mask)
#ifndef Sudden_Underflow
            && word0(d) & (Exp_mask & Exp_mask << 1)
#endif
            ) {
            /* The special case.  Here we want to be within a quarter of the last input
               significant digit instead of one half of it when the decimal output string's value is less than d.  */
            b2 += Log2P;
            s2 += Log2P;
            spec_case = 1;
        }
    }

    /* Arrange for convenient computation of quotients:
     * shift left if necessary so divisor has 4 leading 0 bits.
     *
     * Perhaps we should just compute leading 28 bits of S once
     * and for all and pass them and a shift to quorem, so it
     * can do shifts and ors to compute the numerator for q.
     */
    if ((i = ((s5 ? 32 - hi0bits(S->x[S->wds-1]) : 1) + s2) & 0x1f) != 0)
        i = 32 - i;
    /* i is the number of leading zero bits in the most significant word of S*2^s2. */
    if (i > 4) {
        i -= 4;
        b2 += i;
        m2 += i;
        s2 += i;
    }
    else if (i < 4) {
        i += 28;
        b2 += i;
        m2 += i;
        s2 += i;
    }
    /* Now S*2^s2 has exactly four leading zero bits in its most significant word. */
    if (b2 > 0) {
        b = lshift(b, b2);
        if (!b)
            goto nomem;
    }
    if (s2 > 0) {
        S = lshift(S, s2);
        if (!S)
            goto nomem;
    }
    /* Now we have d/10^k = b/S and
       (mhi * 2^m2) / S = maximum acceptable error, divided by 10^k. */
    if (k_check) {
        if (cmp(b,S) < 0) {
            k--;
            b = multadd(b, 10, 0);  /* we botched the k estimate */
            if (!b)
                goto nomem;
            if (leftright) {
                mhi = multadd(mhi, 10, 0);
                if (!mhi)
                    goto nomem;
            }
            ilim = ilim1;
        }
    }
    /* At this point 1 <= d/10^k = b/S < 10. */

    if (ilim <= 0 && mode > 2) {
        /* We're doing fixed-mode output and d is less than the minimum nonzero output in this mode.
           Output either zero or the minimum nonzero output depending on which is closer to d. */
        if (ilim < 0)
            goto no_digits;
        S = multadd(S,5,0);
        if (!S)
            goto nomem;
        i = cmp(b,S);
        if (i < 0 || (i == 0 && !biasUp)) {
        /* Always emit at least one digit.  If the number appears to be zero
           using the current mode, then emit one '0' digit and set decpt to 1. */
        /*no_digits:
            k = -1 - ndigits;
            goto ret; */
            goto no_digits;
        }
    one_digit:
        *s++ = '1';
        k++;
        goto ret;
    }
    if (leftright) {
        if (m2 > 0) {
            mhi = lshift(mhi, m2);
            if (!mhi)
                goto nomem;
        }

        /* Compute mlo -- check for special case
         * that d is a normalized power of 2.
         */

        mlo = mhi;
        if (spec_case) {
            mhi = Balloc(mhi->k);
            if (!mhi)
                goto nomem;
            Bcopy(mhi, mlo);
            mhi = lshift(mhi, Log2P);
            if (!mhi)
                goto nomem;
        }
        /* mlo/S = maximum acceptable error, divided by 10^k, if the output is less than d. */
        /* mhi/S = maximum acceptable error, divided by 10^k, if the output is greater than d. */

        for(i = 1;;i++) {
            dig = quorem(b,S) + '0';
            /* Do we yet have the shortest decimal string
             * that will round to d?
             */
            j = cmp(b, mlo);
            /* j is b/S compared with mlo/S. */
            delta = diff(S, mhi);
            if (!delta)
                goto nomem;
            j1 = delta->sign ? 1 : cmp(b, delta);
            Bfree(delta);
            /* j1 is b/S compared with 1 - mhi/S. */
#ifndef ROUND_BIASED
            if (j1 == 0 && !mode && !(word1(d) & 1)) {
                if (dig == '9')
                    goto round_9_up;
                if (j > 0)
                    dig++;
                *s++ = (char)dig;
                goto ret;
            }
#endif
            if ((j < 0) || (j == 0 && !mode
#ifndef ROUND_BIASED
                && !(word1(d) & 1)
#endif
                )) {
                if (j1 > 0) {
                    /* Either dig or dig+1 would work here as the least significant decimal digit.
                       Use whichever would produce a decimal value closer to d. */
                    b = lshift(b, 1);
                    if (!b)
                        goto nomem;
                    j1 = cmp(b, S);
                    if (((j1 > 0) || (j1 == 0 && (dig & 1 || biasUp)))
                        && (dig++ == '9'))
                        goto round_9_up;
                }
                *s++ = (char)dig;
                goto ret;
            }
            if (j1 > 0) {
                if (dig == '9') { /* possible if i == 1 */
                round_9_up:
                    *s++ = '9';
                    goto roundoff;
                }
                *s++ = (char)dig + 1;
                goto ret;
            }
            *s++ = (char)dig;
            if (i == ilim)
                break;
            b = multadd(b, 10, 0);
            if (!b)
                goto nomem;
            if (mlo == mhi) {
                mlo = mhi = multadd(mhi, 10, 0);
                if (!mhi)
                    goto nomem;
            }
            else {
                mlo = multadd(mlo, 10, 0);
                if (!mlo)
                    goto nomem;
                mhi = multadd(mhi, 10, 0);
                if (!mhi)
                    goto nomem;
            }
        }
    }
    else
        for(i = 1;; i++) {
            *s++ = (char)(dig = quorem(b,S) + '0');
            if (i >= ilim)
                break;
            b = multadd(b, 10, 0);
            if (!b)
                goto nomem;
        }

    /* Round off last digit */

    b = lshift(b, 1);
    if (!b)
        goto nomem;
    j = cmp(b, S);
    if ((j > 0) || (j == 0 && (dig & 1 || biasUp))) {
    roundoff:
        while(*--s == '9')
            if (s == buf) {
                k++;
                *s++ = '1';
                goto ret;
            }
        ++*s++;
    }
    else {
        /* Strip trailing zeros */
        while(*--s == '0') ;
        s++;
    }
  ret:
    Bfree(S);
    if (mhi) {
        if (mlo && mlo != mhi)
            Bfree(mlo);
        Bfree(mhi);
    }
  ret1:
    Bfree(b);
    JS_ASSERT(s < buf + bufsize);
    *s = '\0';
    if (rve)
        *rve = s;
    *decpt = k + 1;
    return JS_TRUE;

nomem:
    Bfree(S);
    if (mhi) {
        if (mlo && mlo != mhi)
            Bfree(mlo);
        Bfree(mhi);
    }
    Bfree(b);
    return JS_FALSE;
}


/* Mapping of JSDToStrMode -> js_dtoa mode */
static const int dtoaModes[] = {
    0,   /* DTOSTR_STANDARD */
    0,   /* DTOSTR_STANDARD_EXPONENTIAL, */
    3,   /* DTOSTR_FIXED, */
    2,   /* DTOSTR_EXPONENTIAL, */
    2};  /* DTOSTR_PRECISION */

JS_FRIEND_API(char *)
JS_dtostr(char *buffer, size_t bufferSize, JSDToStrMode mode, int precision, double d)
{
    int decPt;                  /* Position of decimal point relative to first digit returned by js_dtoa */
    int sign;                   /* Nonzero if the sign bit was set in d */
    int nDigits;                /* Number of significand digits returned by js_dtoa */
    char *numBegin = buffer+2;  /* Pointer to the digits returned by js_dtoa; the +2 leaves space for */
                                /* the sign and/or decimal point */
    char *numEnd;               /* Pointer past the digits returned by js_dtoa */
    JSBool dtoaRet;

    JS_ASSERT(bufferSize >= (size_t)(mode <= DTOSTR_STANDARD_EXPONENTIAL ? DTOSTR_STANDARD_BUFFER_SIZE :
            DTOSTR_VARIABLE_BUFFER_SIZE(precision)));

    if (mode == DTOSTR_FIXED && (d >= 1e21 || d <= -1e21))
        mode = DTOSTR_STANDARD; /* Change mode here rather than below because the buffer may not be large enough to hold a large integer. */

    /* Locking for Balloc's shared buffers */
    ACQUIRE_DTOA_LOCK();
    dtoaRet = js_dtoa(d, dtoaModes[mode], mode >= DTOSTR_FIXED, precision, &decPt, &sign, &numEnd, numBegin, bufferSize-2);
    RELEASE_DTOA_LOCK();
    if (!dtoaRet)
        return 0;

    nDigits = numEnd - numBegin;

    /* If Infinity, -Infinity, or NaN, return the string regardless of the mode. */
    if (decPt != 9999) {
        JSBool exponentialNotation = JS_FALSE;
        int minNDigits = 0;         /* Minimum number of significand digits required by mode and precision */
        char *p;
        char *q;

        switch (mode) {
            case DTOSTR_STANDARD:
                if (decPt < -5 || decPt > 21)
                    exponentialNotation = JS_TRUE;
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
                JS_ASSERT(precision > 0);
                minNDigits = precision;
                /* Fall through */
            case DTOSTR_STANDARD_EXPONENTIAL:
                exponentialNotation = JS_TRUE;
                break;

            case DTOSTR_PRECISION:
                JS_ASSERT(precision > 0);
                minNDigits = precision;
                if (decPt < -5 || decPt > precision)
                    exponentialNotation = JS_TRUE;
                break;
        }

        /* If the number has fewer than minNDigits, pad it with zeros at the end */
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
            JS_snprintf(numEnd, bufferSize - (numEnd - buffer), "e%+d", decPt-1);
        } else if (decPt != nDigits) {
            /* Some kind of a fraction in fixed notation */
            JS_ASSERT(decPt <= nDigits);
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
                JS_ASSERT(numEnd < buffer + bufferSize);
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
static uint32
divrem(Bigint *b, uint32 divisor)
{
    int32 n = b->wds;
    uint32 remainder = 0;
    ULong *bx;
    ULong *bp;

    JS_ASSERT(divisor > 0 && divisor <= 65536);

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
        JS_ASSERT(quotientHi <= 0xFFFF && remainder < divisor);
        dividend = remainder << 16 | (a & 0xFFFF);
        quotientLo = dividend / divisor;
        remainder = dividend - quotientLo*divisor;
        JS_ASSERT(quotientLo <= 0xFFFF && remainder < divisor);
        *bp = quotientHi << 16 | quotientLo;
    } while (bp != bx);
    /* Decrease the size of the number if its most significant word is now zero. */
    if (bx[n-1] == 0)
        b->wds--;
    return remainder;
}


/* "-0.0000...(1073 zeros after decimal point)...0001\0" is the longest string that we could produce,
 * which occurs when printing -5e-324 in binary.  We could compute a better estimate of the size of
 * the output string and malloc fewer bytes depending on d and base, but why bother? */
#define DTOBASESTR_BUFFER_SIZE 1078
#define BASEDIGIT(digit) ((char)(((digit) >= 10) ? 'a' - 10 + (digit) : '0' + (digit)))

JS_FRIEND_API(char *)
JS_dtobasestr(int base, double d)
{
    char *buffer;        /* The output string */
    char *p;             /* Pointer to current position in the buffer */
    char *pInt;          /* Pointer to the beginning of the integer part of the string */
    char *q;
    uint32 digit;
    double di;           /* d truncated to an integer */
    double df;           /* The fractional part of d */

    JS_ASSERT(base >= 2 && base <= 36);

    buffer = (char*) malloc(DTOBASESTR_BUFFER_SIZE);
    if (buffer) {
        p = buffer;
        if (d < 0.0
#if defined(XP_WIN) || defined(XP_OS2)
            && !((word0(d) & Exp_mask) == Exp_mask && ((word0(d) & Frac_mask) || word1(d))) /* Visual C++ doesn't know how to compare against NaN */
#endif
           ) {
            *p++ = '-';
            d = -d;
        }

        /* Check for Infinity and NaN */
        if ((word0(d) & Exp_mask) == Exp_mask) {
            strcpy(p, !word1(d) && !(word0(d) & Frac_mask) ? "Infinity" : "NaN");
            return buffer;
        }

        /* Locking for Balloc's shared buffers */
        ACQUIRE_DTOA_LOCK();

        /* Output the integer part of d with the digits in reverse order. */
        pInt = p;
        di = fd_floor(d);
        if (di <= 4294967295.0) {
            uint32 n = (uint32)di;
            if (n)
                do {
                    uint32 m = n / base;
                    digit = n - m*base;
                    n = m;
                    JS_ASSERT(digit < (uint32)base);
                    *p++ = BASEDIGIT(digit);
                } while (n);
            else *p++ = '0';
        } else {
            int32 e;
            int32 bits;  /* Number of significant bits in di; not used. */
            Bigint *b = d2b(di, &e, &bits);
            if (!b)
                goto nomem1;
            b = lshift(b, e);
            if (!b) {
              nomem1:
                Bfree(b);
                RELEASE_DTOA_LOCK();
                free(buffer);
                return NULL;
            }
            do {
                digit = divrem(b, base);
                JS_ASSERT(digit < (uint32)base);
                *p++ = BASEDIGIT(digit);
            } while (b->wds);
            Bfree(b);
        }
        /* Reverse the digits of the integer part of d. */
        q = p-1;
        while (q > pInt) {
            char ch = *pInt;
            *pInt++ = *q;
            *q-- = ch;
        }

        df = d - di;
        if (df != 0.0) {
            /* We have a fraction. */
            int32 e, bbits, s2, done;
            Bigint *b, *s, *mlo, *mhi;

            b = s = mlo = mhi = NULL;

            *p++ = '.';
            b = d2b(df, &e, &bbits);
            if (!b) {
              nomem2:
                Bfree(b);
                Bfree(s);
                if (mlo != mhi)
                    Bfree(mlo);
                Bfree(mhi);
                RELEASE_DTOA_LOCK();
                free(buffer);
                return NULL;
            }
            JS_ASSERT(e < 0);
            /* At this point df = b * 2^e.  e must be less than zero because 0 < df < 1. */

            s2 = -(int32)(word0(d) >> Exp_shift1 & Exp_mask>>Exp_shift1);
#ifndef Sudden_Underflow
            if (!s2)
                s2 = -1;
#endif
            s2 += Bias + P;
            /* 1/2^s2 = (nextDouble(d) - d)/2 */
            JS_ASSERT(-s2 < e);
            mlo = i2b(1);
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
                mhi = i2b(1<<Log2P);
                if (!mhi)
                    goto nomem2;
            }
            b = lshift(b, e + s2);
            if (!b)
                goto nomem2;
            s = i2b(1);
            if (!s)
                goto nomem2;
            s = lshift(s, s2);
            if (!s)
                goto nomem2;
            /* At this point we have the following:
             *   s = 2^s2;
             *   1 > df = b/2^s2 > 0;
             *   (d - prevDouble(d))/2 = mlo/2^s2;
             *   (nextDouble(d) - d)/2 = mhi/2^s2. */

            done = JS_FALSE;
            do {
                int32 j, j1;
                Bigint *delta;

                b = multadd(b, base, 0);
                if (!b)
                    goto nomem2;
                digit = quorem2(b, s2);
                if (mlo == mhi) {
                    mlo = mhi = multadd(mlo, base, 0);
                    if (!mhi)
                        goto nomem2;
                }
                else {
                    mlo = multadd(mlo, base, 0);
                    if (!mlo)
                        goto nomem2;
                    mhi = multadd(mhi, base, 0);
                    if (!mhi)
                        goto nomem2;
                }

                /* Do we yet have the shortest string that will round to d? */
                j = cmp(b, mlo);
                /* j is b/2^s2 compared with mlo/2^s2. */
                delta = diff(s, mhi);
                if (!delta)
                    goto nomem2;
                j1 = delta->sign ? 1 : cmp(b, delta);
                Bfree(delta);
                /* j1 is b/2^s2 compared with 1 - mhi/2^s2. */

#ifndef ROUND_BIASED
                if (j1 == 0 && !(word1(d) & 1)) {
                    if (j > 0)
                        digit++;
                    done = JS_TRUE;
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
                        b = lshift(b, 1);
                        if (!b)
                            goto nomem2;
                        j1 = cmp(b, s);
                        if (j1 > 0) /* The even test (|| (j1 == 0 && (digit & 1))) is not here because it messes up odd base output
                                     * such as 3.5 in base 3.  */
                            digit++;
                    }
                    done = JS_TRUE;
                } else if (j1 > 0) {
                    digit++;
                    done = JS_TRUE;
                }
                JS_ASSERT(digit < (uint32)base);
                *p++ = BASEDIGIT(digit);
            } while (!done);
            Bfree(b);
            Bfree(s);
            if (mlo != mhi)
                Bfree(mlo);
            Bfree(mhi);
        }
        JS_ASSERT(p < buffer + DTOBASESTR_BUFFER_SIZE);
        *p = '\0';
        RELEASE_DTOA_LOCK();
    }
    return buffer;
}
