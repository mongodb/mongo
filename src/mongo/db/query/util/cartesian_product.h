// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <tuple>
#include <vector>

namespace mongo::utils {
/**
 * Base case; cartesian product of no inputs is defined as a single output value
 * of an empty tuple.
 * This is consistent with C++23's std::ranges::cartesian_product, and is a required
 * base case for implementing the cartesian product recursively.
 */
inline auto cartesian_product() {
    return std::vector{std::tuple{}};
}

using std::cbegin;

// macOS builders do not currently use a new enough clang to have std::iter_value_t.
// This is a trivial shim.
template <class T>
using iter_value_t = typename std::iterator_traits<std::remove_cvref_t<T>>::value_type;

// Helper to find the (const) value type of a range - i.e., the result of dereferencing an iterator
// from the range. This is used as the <ranges> header is currently forbidden.
template <class Range>
using range_value_t = iter_value_t<decltype(cbegin(std::declval<Range>()))>;

template <class... Inputs>
using tuple_of_results_t = std::tuple<range_value_t<Inputs>...>;

/**
 * Compute the cartesian product of N input containers.
 *
 * That is,
 *
 *  cartesian_product(std::vector{1, 2}, std::array{'A', 'B'})
 *
 * produces
 *
 *  std::vector{
 *      std::tuple{1, 'A'},
 *      std::tuple{1, 'A'},
 *      std::tuple{2, 'B'},
 *      std::tuple{2, 'B'},
 *  };
 *
 *  cartesian_product(std::vector{1, 2})
 *
 * produces
 *
 *  std::vector{
 *      std::tuple{1},
 *      std::tuple{2},
 *  };
 *
 *  cartesian_product()
 *
 * produces
 *
 *  std::vector{
 *      std::tuple{},
 *  };
 *
 * Any repeatedly iterable input may be provided.
 */
template <class FirstRange, class... TailRanges>
auto cartesian_product(const FirstRange& firstRange, const TailRanges&... inputRanges) {
    // A vector of results will be built at each recursive call; this is inefficient
    // in terms of allocations, but was chosen as it is reasonably "readable".
    std::vector<tuple_of_results_t<FirstRange, TailRanges...>> result;

    // Called with N inputs, compute the cartesian product of the last N-1.
    auto tail = cartesian_product(inputRanges...);

    // Now, for each of the above results, output a copy for each of the values
    // in the _first_ input.
    for (const auto& value : firstRange) {
        for (const auto& rest : tail) {
            result.push_back(std::tuple_cat(std::tuple(value), rest));
        }
    }
    return result;
}
}  // namespace mongo::utils
