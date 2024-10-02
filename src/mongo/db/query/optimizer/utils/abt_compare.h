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

#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/syntax/syntax.h"


namespace mongo::optimizer {

/**
 * Used to compare trees containing only Paths and Expressions.
 */
int compareExprAndPaths(const ABT& n1, const ABT& n2);

/**
 * The result of a comparison operation evaluated during constant folding.
 */
enum class CmpResult : int32_t {
    kTrue = 1,
    kFalse = 0,
    kLt = -1,
    kEq = 0,
    kGt = 1,
    kIncomparable = std::numeric_limits<int>::max()
};

/**
 * Compare ABT equality in a fast way without invoking constant folding.
 * - Returns kIncomparable if nothing can be determined about the comparison between the two
 *   arguments.
 * - Otherwise return kTrue, or kFalse depending on the comparison.
 */
CmpResult cmpEqFast(const ABT& lhs, const ABT& rhs);

/**
 * Three way comparison of the two arguments via direct value interface without using constant
 * folding. The 'op' argument signifies how to interpret the 3w comparison - either as a boolean
 * comparison function, or directly as a 3w comparison.
 * - Returns kIncomparable if nothing can be determined about the comparison between the two
 * arguments.
 * - If op is {<, <=, >, >=}, return kTrue or kFalse depending on whether it is true or false.
 * - If op is a 3-way comparison, return -1 if lhs < rhs, or 1 lhs > rhs.
 * Notice that this function is not used for equality comparison.
 */
CmpResult cmp3wFast(Operations op, const ABT& lhs, const ABT& rhs);
}  // namespace mongo::optimizer
