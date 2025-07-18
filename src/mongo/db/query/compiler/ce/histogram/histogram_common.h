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

#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"

namespace mongo::ce {

/**
 * Distinguish cardinality estimation algorithms for range queries over array values.
 * Given an example interval a: [x, y]
 * - kConjunctArrayCE estimates cardinality for queries that translate the interval into
 * conjunctions during planning. e.g, [x,y] is translated into
 * {$and: [ {a: { $lte: y }}, {a: { $gte: x }}]} i.e., (-inf, y] AND [x, +inf)
 * This approach will estimate the number of arrays that values collectively satisfy the
 * conjunction i.e., at least on that satisfies (-inf, y] and at least one that satisfies [x, +inf).
 * - kExactArrayCE estimates the cardinality of arrays that have at least one value that falls in
 * the given interval.
 */
enum class ArrayRangeEstimationAlgo { kConjunctArrayCE, kExactArrayCE };

}  // namespace mongo::ce
