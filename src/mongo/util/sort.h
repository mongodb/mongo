// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <algorithm>
#include <functional>

namespace mongo {

/**
 * Work-alike to C++20 constexpr std::is_sorted_until.
 *
 * Implementation from: https://en.cppreference.com/w/cpp/algorithm/is_sorted_until
 */
template <class ForwardIt, class Compare>
constexpr ForwardIt constexprIsSortedUntil(ForwardIt first, ForwardIt last, Compare comp) {
    // TODO(SERVER-105042): GCC 14.2 will not constant-evaluate std::is_sorted_until in debug mode.
    // Revisit this when we next upgrade GCC.
#if !defined(_GLIBCXX_DEBUG) && __cpp_lib_constexpr_algorithms >= 201806L
    return std::is_sorted_until(first, last, comp);
#else
    if (first != last) {
        ForwardIt next = first;
        while (++next != last) {
            if (comp(*next, *first))
                return next;
            first = next;
        }
    }
    return last;
#endif
}

/**
 * Work-alike to C++20 constexpr std::is_sorted_until.
 */
template <typename ForwardIt>
constexpr bool constexprIsSortedUntil(ForwardIt first, ForwardIt last) {
    return constexprIsSortedUntil(first, last, std::less<>{});
}

/**
 * Work-alike to C++20 constexpr std::is_sorted.
 *
 * Implementation from: https://en.cppreference.com/w/cpp/algorithm/is_sorted
 */
template <typename ForwardIt, typename Compare>
constexpr bool constexprIsSorted(ForwardIt first, ForwardIt last, Compare comp) {
    // TODO(SERVER-105042): GCC 14.2 will not constant-evaluate std::is_sorted_until in debug mode.
    // Revisit this when we next upgrade GCC.
#if !defined(_GLIBCXX_DEBUG) && __cpp_lib_constexpr_algorithms >= 201806L
    return std::is_sorted(first, last, comp);
#else
    return constexprIsSortedUntil(first, last, comp) == last;
#endif
}

/**
 * Work-alike to C++20 constexpr std::is_sorted.
 */
template <typename ForwardIt>
constexpr bool constexprIsSorted(ForwardIt first, ForwardIt last) {
    return constexprIsSorted(first, last, std::less<>{});
}

}  // namespace mongo
