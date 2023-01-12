/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"

namespace mongo::optimizer::ce {
// Invalid estimate - an arbitrary negative value used for initialization.
constexpr SelectivityType kInvalidSel{-1.0};
constexpr CEType kInvalidEstimate{-1.0};

constexpr SelectivityType kDefaultFilterSel{0.1};
constexpr SelectivityType kDefaultExistsSel{0.70};

// The selectivities used in the piece-wise function for open-range intervals.
// Note that we assume a smaller input cardinality will result in a less selective range.
constexpr SelectivityType kSmallCardOpenRangeSel{0.70};
constexpr SelectivityType kMediumCardOpenRangeSel{0.45};
constexpr SelectivityType kLargeCardOpenRangeSel{0.33};

// The selectivities used in the piece-wise function for closed-range intervals.
// Note that we assume a smaller input cardinality will result in a less selective range.
constexpr SelectivityType kSmallCardClosedRangeSel{0.50};
constexpr SelectivityType kMediumCardClosedRangeSel{0.33};
constexpr SelectivityType kLargeCardClosedRangeSel{0.20};

// Global and Local selectivity should multiply to the Complete selectivity.
constexpr SelectivityType kDefaultCompleteGroupSel{0.01};
constexpr SelectivityType kDefaultLocalGroupSel{0.02};
constexpr SelectivityType kDefaultGlobalGroupSel{0.5};

// Since there are only two boolean values, when we have a count of arrays containing booleans, we
// estimate the default selectivity of booleans in arrays as 1/2.
constexpr double kDefaultArrayBoolSel{0.5};

// The following constants are the steps used in the piece-wise functions that select selectivies
// based on input cardinality.
constexpr CEType kSmallLimit{20.0};
constexpr CEType kMediumLimit{100.0};

// Assumed average number of elements in an array. This is a unitless constant.
constexpr double kDefaultAverageArraySize{10.0};

/**
 * Returns the heuristic selectivity of equalities. To avoid super small selectivities for small
 * cardinalities, that would result in 0 cardinality for many small inputs, the estimate is scaled
 * as inputCard grows. The bigger inputCard, the smaller the selectivity.
 */
SelectivityType heuristicEqualitySel(CEType inputCard);

/**
 * Returns the heuristic selectivity of intervals with bounds on both ends. These intervals are
 * considered less selective than equalities.
 * Examples: (a > 'abc' AND a < 'hta'), (0 < b <= 13)
 */
SelectivityType heuristicClosedRangeSel(CEType inputCard);

/**
 * Returns the heuristic selectivity of intervals open on one end. These intervals are considered
 * less selective than those with both ends specified by the user query.
 * Examples: (a > 'xyz'), (b <= 13)
 */
SelectivityType heuristicOpenRangeSel(CEType inputCard);

/**
 * Returns the heuristic selectivity based on the kind of operation in a predicate, e.g. $eq, $lt.
 */
SelectivityType heuristicOperationSel(Operations op, CEType inputCard);

/**
 * Returns the heuristic selectivity of the given 'interval'.
 */
SelectivityType heuristicIntervalSel(const IntervalRequirement& interval, CEType inputCard);

/**
 * Returns the heuristically estimated cardinality of the given 'interval'.
 */
CEType heuristicIntervalCard(const IntervalRequirement& interval, CEType inputCard);

/**
 * Returns the heuristic selectivity of an interval expressed as two PathCompare nodes.
 */
SelectivityType heuristicIntervalSel(const PathCompare& left,
                                     const PathCompare& right,
                                     CEType inputCard);

}  // namespace mongo::optimizer::ce
