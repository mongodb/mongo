/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_Sort_h
#define ds_Sort_h

#include "jstypes.h"

namespace js {

namespace detail {

template<typename T>
MOZ_ALWAYS_INLINE void
CopyNonEmptyArray(T* dst, const T* src, size_t nelems)
{
    MOZ_ASSERT(nelems != 0);
    const T* end = src + nelems;
    do {
        *dst++ = *src++;
    } while (src != end);
}

/* Helper function for MergeSort. */
template<typename T, typename Comparator>
MOZ_ALWAYS_INLINE bool
MergeArrayRuns(T* dst, const T* src, size_t run1, size_t run2, Comparator c)
{
    MOZ_ASSERT(run1 >= 1);
    MOZ_ASSERT(run2 >= 1);

    /* Copy runs already in sorted order. */
    const T* b = src + run1;
    bool lessOrEqual;
    if (!c(b[-1],  b[0], &lessOrEqual))
        return false;

    if (!lessOrEqual) {
        /* Runs are not already sorted, merge them. */
        for (const T* a = src;;) {
            if (!c(*a, *b, &lessOrEqual))
                return false;
            if (lessOrEqual) {
                *dst++ = *a++;
                if (!--run1) {
                    src = b;
                    break;
                }
            } else {
                *dst++ = *b++;
                if (!--run2) {
                    src = a;
                    break;
                }
            }
        }
    }
    CopyNonEmptyArray(dst, src, run1 + run2);
    return true;
}

} /* namespace detail */

/*
 * Sort the array using the merge sort algorithm. The scratch should point to
 * a temporary storage that can hold nelems elements.
 *
 * The comparator must provide the () operator with the following signature:
 *
 *     bool operator()(const T& a, const T& a, bool* lessOrEqualp);
 *
 * It should return true on success and set *lessOrEqualp to the result of
 * a <= b operation. If it returns false, the sort terminates immediately with
 * the false result. In this case the content of the array and scratch is
 * arbitrary.
 */
template<typename T, typename Comparator>
bool
MergeSort(T* array, size_t nelems, T* scratch, Comparator c)
{
    const size_t INS_SORT_LIMIT = 3;

    if (nelems <= 1)
        return true;

    /*
     * Apply insertion sort to small chunks to reduce the number of merge
     * passes needed.
     */
    for (size_t lo = 0; lo < nelems; lo += INS_SORT_LIMIT) {
        size_t hi = lo + INS_SORT_LIMIT;
        if (hi >= nelems)
            hi = nelems;
        for (size_t i = lo + 1; i != hi; i++) {
            for (size_t j = i; ;) {
                bool lessOrEqual;
                if (!c(array[j - 1], array[j], &lessOrEqual))
                    return false;
                if (lessOrEqual)
                    break;
                T tmp = array[j - 1];
                array[j - 1] = array[j];
                array[j] = tmp;
                if (--j == lo)
                    break;
            }
        }
    }

    T* vec1 = array;
    T* vec2 = scratch;
    for (size_t run = INS_SORT_LIMIT; run < nelems; run *= 2) {
        for (size_t lo = 0; lo < nelems; lo += 2 * run) {
            size_t hi = lo + run;
            if (hi >= nelems) {
                detail::CopyNonEmptyArray(vec2 + lo, vec1 + lo, nelems - lo);
                break;
            }
            size_t run2 = (run <= nelems - hi) ? run : nelems - hi;
            if (!detail::MergeArrayRuns(vec2 + lo, vec1 + lo, run, run2, c))
                return false;
        }
        T* swap = vec1;
        vec1 = vec2;
        vec2 = swap;
    }
    if (vec1 == scratch)
        detail::CopyNonEmptyArray(array, scratch, nelems);
    return true;
}

} /* namespace js */

#endif /* ds_Sort_h */
