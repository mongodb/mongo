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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/util/modules.h"

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

/**
 * Helper struct to indicate semantics of an equality predicate on the given FieldPath.
 */
struct FieldPathAndEqSemantics {
    FieldPath path;
    // Indicates the type of equality:
    //  - true => $expr eq, strict equality, null != missing.
    //  - false => regular eq, null == missing.
    bool isExprEq = false;

    // Used for logging.
    BSONObj toBSON() const;
};

/**
 * Returns the number of distinct values of tuples of the given field names in the input documents.
 * For example, given documents [{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 2}, {a: 2, b: 2}], the NDV of
 * ["a","b"] is 3.
 *
 * If bounds are provided, the sample will first be filtered to values meeting those bounds. There
 * must be the same number of bounds as fields, and they must be in the same order. For the above
 * example, the NDV of ["a", "b"] given bounds [[1,1]], [[1,2]] is 2.
 *
 * Important notes on what is treated as "distinct":
 * - Null and missing are treated as distinct only for fields marked as isExprEq=true: e.g. given
 *   documents [{a: null}, {}], the NDV of "a" is 2 if it is specified as a $expr field- otherwise,
 *   the NDV is 1.
 * - When values are objects, field order matters: given documents [{a: {b: 1, c: 1}}, {a: {c: 1, b:
 *   1}}], the NDV of "a" is 2.
 * - The field order of fields in 'fieldNames' in the documents in 'docs' is not significant: given
 *   documents [{a: 1, b: 2}, {b: 2, a: 1}], the NDV of ["a","b"] is 1.
 *
 * Does not support counting NDV over array-valued fields; tasserts if any of 'fieldNames' are
 * array-valued in 'docs'.
 */
size_t countNDV(const std::vector<FieldPathAndEqSemantics>& fields,
                const std::vector<BSONObj>& docs,
                boost::optional<std::span<const OrderedIntervalList>> bounds = boost::none);

struct KeyCountResult {
    // Total number of keys projected from the sample (>= number of documents for multikey fields).
    size_t totalSampleKeys;
    // Number of keys that satisfy the filter (e.g. fall within the index bounds).
    size_t uniqueMatchingKeys;
    // Number of distinct key tuples among all projected keys (regardless of filter).
    size_t sampleUniqueKeys;
};

/**
 * Version of countNDV which handles array-valued fields by flattening into multiple values, before
 * determining the count of distinct values.
 */
KeyCountResult countNDVMultiKey(
    const std::vector<FieldPathAndEqSemantics>& fields,
    const std::vector<BSONObj>& docs,
    boost::optional<std::span<const OrderedIntervalList>> bounds = boost::none);

/**
 * Returns the number of unique documents in the given vector by counting distinct _id values.
 * This detects duplicate documents produced by sampling with replacement, where the same
 * physical document may appear multiple times.
 * This function assumes that every document passed to it contains an _id field.
 */
size_t countUniqueDocuments(const std::vector<BSONObj>& docs);

/**
 * This helper checks if an element is within the given Interval.
 */
bool matchesInterval(const Interval& interval, BSONElement val);

/**
 * This helper checks if an element is within any of the list of Interval.
 */
bool matchesInterval(const OrderedIntervalList& oil, BSONElement val);

}  // namespace mongo::ce
