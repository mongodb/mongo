/*-
 * Copyright (c) 1992, 1993
 *    The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "wt_internal.h"

#define WT_QSORT_INSERT_THRESHOLD 7
#define WT_QSORT_LARGE_THRESHOLD 40

#ifdef WT_QSORT_R_DEF
typedef int wt_cmp_t(const void *, const void *, void *);
#else
typedef int wt_cmp_t(const void *, const void *);
#endif

#ifdef WT_QSORT_R_DEF
#define CMP(x, y, t) (cmp((x), (y), (t)))
#else
#define CMP(x, y, t) (cmp((x), (y)))
#endif

/*
 * __swap_bytes --
 *     Swap the contents of two arbitrary values of a given size.
 */
static inline void
__swap_bytes(uint8_t *a, uint8_t *b, size_t size)
{
    uint8_t tmp;

    while (size > 0) {
        tmp = *a;
        *a++ = *b;
        *b++ = tmp;
        --size;
    }
}

/*
 * __med3 --
 *     Find the median of three values, using the user-supplied comparator.
 */
static inline void *
__med3(void *a, void *b, void *c, wt_cmp_t cmp, void *context)
{
#ifndef WT_QSORT_R_DEF
    WT_UNUSED(context);
#endif

    if (CMP(a, b, context) < 0) {
        if (CMP(b, c, context) < 0)
            return (b);
        else
            return (CMP(a, c, context) < 0 ? c : a);
    } else {
        if (CMP(b, c, context) > 0)
            return (b);
        else
            return (CMP(a, c, context) < 0 ? a : c);
    }
}

/*
 * __qsort --
 *     Sort an array using a user-supplied comparator function and a context argument.
 */
static void
__qsort(void *arr, size_t nmemb, size_t elem_sz, wt_cmp_t cmp, void *context)
{
    /*
     * This is very similar to the version in FreeBSD, which itself is originally from "Engineering
     * a Sort Function", by Jon L. Bentley and Douglas M. McIlroy ("Software - Practice and
     * Experience, Vol. 23(11)", November 1993). From there it was massaged into a form suitable for
     * WiredTiger.
     *
     * It's not simple, so some description of where all these variables point is in order:
     * -----------------------------------------------
     * | = |        <         | ? |      >       | = |
     * -----------------------------------------------
     *      ^                  ^ ^              ^
     *      |                  | |              |
     *      lowest_lt_median   | hi_unknown     highest_gt_median
     *                         lo_unknown
     */
    bool swapped;
    int cmp_result;
    size_t lhs_unsorted, rhs_unsorted;
    uint8_t *a, *hi_pseudomedian, *hi_unknown, *highest_gt_median, *lo_pseudomedian, *lo_unknown,
      *lowest_lt_median, *pseudomedian;

    WT_ASSERT(NULL, cmp != NULL);
    if (UNLIKELY(nmemb < 2))
        return;

    a = arr; /* Avoid arithmetic on void pointers. */

    for (;;) {
        swapped = false;

        /* Insertion sort for small arrays. */
        if (nmemb < WT_QSORT_INSERT_THRESHOLD) {
            for (hi_unknown = a + elem_sz; hi_unknown < a + nmemb * elem_sz; hi_unknown += elem_sz)
                for (lo_unknown = hi_unknown;
                     lo_unknown > a && CMP(lo_unknown - elem_sz, lo_unknown, context) > 0;
                     lo_unknown -= elem_sz)
                    __swap_bytes(lo_unknown, lo_unknown - elem_sz, elem_sz);
            return;
        }

        /* Take middle element as median for small arrays. */
        pseudomedian = a + (nmemb / 2) * elem_sz;
        if (nmemb > WT_QSORT_INSERT_THRESHOLD) {
            lo_pseudomedian = a;
            hi_pseudomedian = a + (nmemb - 1) * elem_sz;
            /*
             * For larger arrays, do more work to find a good median - here, it's a "pseudo-median"
             * of nine samples.
             */
            if (nmemb > WT_QSORT_LARGE_THRESHOLD) {
                size_t median_dist = (nmemb / 8) * elem_sz;

                lo_pseudomedian = __med3(lo_pseudomedian, lo_pseudomedian + median_dist,
                  lo_pseudomedian + 2 * median_dist, cmp, context);
                pseudomedian = __med3(pseudomedian - median_dist, pseudomedian,
                  pseudomedian + median_dist, cmp, context);
                hi_pseudomedian = __med3(hi_pseudomedian - 2 * median_dist,
                  hi_pseudomedian - median_dist, hi_pseudomedian, cmp, context);
            }
            pseudomedian = __med3(lo_pseudomedian, pseudomedian, hi_pseudomedian, cmp, context);
        }

        __swap_bytes(a, pseudomedian, elem_sz);
        lowest_lt_median = lo_unknown = a + elem_sz;
        hi_unknown = highest_gt_median = a + (nmemb - 1) * elem_sz;
        for (;;) {
            while (lo_unknown <= hi_unknown && (cmp_result = CMP(lo_unknown, a, context)) <= 0) {
                if (cmp_result == 0) {
                    swapped = true;
                    __swap_bytes(lowest_lt_median, lo_unknown, elem_sz);
                    lowest_lt_median += elem_sz;
                }
                lo_unknown += elem_sz;
            }

            while (lo_unknown <= hi_unknown && (cmp_result = CMP(hi_unknown, a, context)) >= 0) {
                if (cmp_result == 0) {
                    swapped = true;
                    __swap_bytes(hi_unknown, highest_gt_median, elem_sz);
                    highest_gt_median -= elem_sz;
                }
                hi_unknown -= elem_sz;
            }

            if (lo_unknown > hi_unknown)
                break;

            __swap_bytes(lo_unknown, hi_unknown, elem_sz);
            swapped = true;
            lo_unknown += elem_sz;
            hi_unknown -= elem_sz;
        }

        if (!swapped) {
            /* Switch to insertion sort. */
            for (hi_unknown = a + elem_sz; hi_unknown < a + nmemb * elem_sz; hi_unknown += elem_sz)
                for (lo_unknown = hi_unknown;
                     lo_unknown > a && CMP(lo_unknown - elem_sz, lo_unknown, context) > 0;
                     lo_unknown -= elem_sz)
                    __swap_bytes(lo_unknown, lo_unknown - elem_sz, elem_sz);
            return;
        }

        hi_pseudomedian = a + nmemb * elem_sz;

        WT_ASSERT(NULL, lowest_lt_median >= a);
        WT_ASSERT(NULL, lo_unknown >= lowest_lt_median);
        WT_ASSERT(NULL, highest_gt_median >= hi_unknown);
        WT_ASSERT(NULL, hi_pseudomedian >= (highest_gt_median + elem_sz));

        /*
         * Swap our pivot back into the correct place. We need the size_t casts because subtracting
         * pointers yields a ptrdiff_t, which is signed, but we're assigning to a size_t (because
         * we're measuring the size of an object) and having this signed makes no logical sense. It
         * would also break the arithmetic we do later on. The compiler (correctly) complains about
         * this, but we know a little bit more. Specifically, we know that the right hand side is
         * always less than the left-hand side (modulo bugs - see assertions above), so we can get
         * away with a cast.
         */
        lhs_unsorted =
          WT_MIN((size_t)(lowest_lt_median - a), (size_t)(lo_unknown - lowest_lt_median));
        __swap_bytes(a, lo_unknown - lhs_unsorted, lhs_unsorted);
        lhs_unsorted = WT_MIN((size_t)(highest_gt_median - hi_unknown),
          (size_t)(hi_pseudomedian - highest_gt_median) - elem_sz);
        __swap_bytes(lo_unknown, hi_pseudomedian - lhs_unsorted, lhs_unsorted);

        lhs_unsorted = (size_t)(lo_unknown - lowest_lt_median);
        rhs_unsorted = (size_t)(highest_gt_median - hi_unknown);
        if (lhs_unsorted <= rhs_unsorted) {
            /* Recurse on left partition, then iterate on right partition. */
            if (lhs_unsorted > elem_sz)
                __qsort(a, lhs_unsorted / elem_sz, elem_sz, cmp, context);

            if (rhs_unsorted > elem_sz) {
                a = hi_pseudomedian - rhs_unsorted;
                nmemb = rhs_unsorted / elem_sz;
                continue;
            } else
                break;
        } else {
            /* Recurse on right partition, then iterate on left partition. */
            if (rhs_unsorted > elem_sz)
                __qsort(
                  hi_pseudomedian - rhs_unsorted, rhs_unsorted / elem_sz, elem_sz, cmp, context);

            if (lhs_unsorted > elem_sz) {
                nmemb = lhs_unsorted / elem_sz;
                continue;
            } else
                break;
        }
    }
}

#ifdef WT_QSORT_R_DEF
/*
 * __wt_qsort_r --
 *     Sort an array using a user-supplied comparator function and context argument.
 */
void
__wt_qsort_r(void *a, size_t nmemb, size_t elem_sz, int (*cmp)(const void *, const void *, void *),
  void *context)
{
    __qsort(a, nmemb, elem_sz, cmp, context);
}
#else
/*
 * __wt_qsort --
 *     Sort an array using a user-supplied comparator function.
 */
void
__wt_qsort(void *a, size_t nmemb, size_t elem_sz, int (*cmp)(const void *, const void *))
{
    __qsort(a, nmemb, elem_sz, cmp, NULL);
}
#endif
