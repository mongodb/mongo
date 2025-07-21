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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

namespace mongo::ce {

using TypeCounts = std::map<sbe::value::TypeTags, int64_t>;

using CardinalityType = cost_based_ranker::CardinalityType;
using CardinalityEstimate = cost_based_ranker::CardinalityEstimate;
using EstimationSource = cost_based_ranker::EstimationSource;

struct EstimationResult {
    double card;
    double ndv;

    EstimationResult operator-(const EstimationResult& other) const {
        return {card - other.card, ndv - other.ndv};
    }

    EstimationResult& operator+=(const EstimationResult& other) {
        this->card += other.card;
        this->ndv += other.ndv;
        return *this;
    }
};

enum class EstimationType { kEqual, kLess, kLessOrEqual, kGreater, kGreaterOrEqual };

/**
 * Checks if an interval is in descending direction.
 */
inline bool reversedInterval(sbe::value::TypeTags tagLow,
                             sbe::value::Value valLow,
                             sbe::value::TypeTags tagHigh,
                             sbe::value::Value valHigh) {
    auto [cmpTag, cmpVal] = sbe::value::compareValue(tagLow, valLow, tagHigh, valHigh);

    // Compares 'cmpTag' to check if the comparison is successful.
    if (cmpTag == sbe::value::TypeTags::NumberInt32) {
        if (sbe::value::bitcastTo<int32_t>(cmpVal) == 1) {
            return true;
        }
    }

    // The comparison fails to tell which one is smaller or larger. This is not expected because we
    // do not expect to see 'Nothing', 'ArraySet', 'ArrayMultiSet', 'MultiMap' in interval bounds.
    uassert(8870501, "Unable to compare bounds", cmpTag == sbe::value::TypeTags::NumberInt32);
    return false;
}
}  // namespace mongo::ce
