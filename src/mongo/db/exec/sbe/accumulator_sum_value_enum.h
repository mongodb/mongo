// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * This enum defines indices into an 'Array' that accumulates $sum results.
 *
 * The array might contain up to four elements:
 * - The element at index `kNonDecimalTotalTag` keeps track of the widest type of the sum of
 * non-decimal values. Only the tag half of this entry is used and represents the tag of the
 * widest data type seen so far. The value half of the entry is garbage (nominally set to 0).
 * - The elements at indices `kNonDecimalTotalSum` and `kNonDecimalTotalAddend` together represent
 * non-decimal value which is the sum of all non-decimal values.
 * - The element at index `kDecimalTotal` is optional and represents the sum of all decimal values
 * if any such values are encountered.
 */
enum AggSumValueElems {
    kNonDecimalTotalTag,     // starts life as NumberInt32, the smallest numeric type
    kNonDecimalTotalSum,     // must be NumberDouble per tassert 5755312
    kNonDecimalTotalAddend,  // must be NumberDouble per tassert 5755312
    kDecimalTotal,
    // This is actually not an index but represents the maximum number of elements.
    kMaxSizeOfArray
};

namespace stage_builder {
using namespace std::literals::string_view_literals;

constexpr std::string_view partialSumName = "ps"sv;  // Used for the full state of partial sum
constexpr std::string_view countName = "count"sv;    // Used for number of elements in average

}  // namespace stage_builder

}  // namespace mongo
