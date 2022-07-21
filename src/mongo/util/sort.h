/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
#if __cpp_lib_constexpr_algorithms >= 201806L
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
#if __cpp_lib_constexpr_algorithms >= 201806L
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
