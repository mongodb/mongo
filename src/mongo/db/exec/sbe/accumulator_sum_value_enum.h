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

#include "mongo/base/string_data.h"

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

constexpr StringData partialSumName = "ps"_sd;  // Used for the full state of partial sum
constexpr StringData countName = "count"_sd;    // Used for number of elements in average

}  // namespace stage_builder

}  // namespace mongo
