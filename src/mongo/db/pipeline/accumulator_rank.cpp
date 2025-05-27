/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

// These don't make sense as accumulators, so only register them as window functions.
REGISTER_STABLE_WINDOW_FUNCTION(
    rank, mongo::window_function::ExpressionFromRankAccumulator<AccumulatorRank>::parse);
REGISTER_STABLE_WINDOW_FUNCTION(
    denseRank, mongo::window_function::ExpressionFromRankAccumulator<AccumulatorDenseRank>::parse);
REGISTER_STABLE_WINDOW_FUNCTION(
    documentNumber,
    mongo::window_function::ExpressionFromRankAccumulator<AccumulatorDocumentNumber>::parse);

const char* kTempSortKeyField = "sortKey";

// Define sort-order compliant comparison function which uses fast pass logic for null and missing
// and full sort key logic for arrays.
bool legacyIsSameValue(const ValueComparator& valueComparator,
                       SortKeyGenerator& sortKeyGen,
                       const Value& a,
                       const Value& b) {
    if (a.nullish() && b.nullish()) {
        return true;
    }
    if (a.isArray() || b.isArray()) {
        auto getSortKey = [&](const Value& v) {
            BSONObjBuilder builder;
            v.addToBsonObj(&builder, kTempSortKeyField);
            return sortKeyGen.computeSortKeyString(builder.obj());
        };
        auto aKey = getSortKey(a);
        auto bKey = getSortKey(b);
        return aKey.compare(bKey) == 0;
    }
    return valueComparator.compare(a, b) == 0;
}

void AccumulatorRank::processInternal(const Value& input, bool merging) {
    tassert(5417001, "$rank can't be merged", !merging);
    if (isNewValue(input)) {
        _lastRank += _numSameRank;
        _numSameRank = 1;
        _lastInput = input;
        _memUsageTracker.set(sizeof(*this) + _lastInput->getApproximateSize() - sizeof(Value));
    } else {
        ++_numSameRank;
    }
}

void AccumulatorDocumentNumber::processInternal(const Value& input, bool merging) {
    tassert(5417002, "$documentNumber can't be merged", !merging);
    // DocumentNumber doesn't need to keep track of what we just saw.
    ++_lastRank;
}

void AccumulatorDenseRank::processInternal(const Value& input, bool merging) {
    tassert(5417003, "$denseRank can't be merged", !merging);
    if (isNewValue(input)) {
        ++_lastRank;
        _lastInput = input;
        _memUsageTracker.set(sizeof(*this) + _lastInput->getApproximateSize() - sizeof(Value));
    }
}

AccumulatorRankBase::AccumulatorRankBase(ExpressionContext* const expCtx)
    : AccumulatorForWindowFunctions(expCtx), _legacySortKeyGen(boost::none) {
    _memUsageTracker.set(sizeof(*this));
}

AccumulatorRankBase::AccumulatorRankBase(ExpressionContext* const expCtx, bool isAscending)
    : AccumulatorForWindowFunctions(expCtx),
      _legacySortKeyGen(SortKeyGenerator{
          SortPattern({SortPattern::SortPatternPart{isAscending, FieldPath{kTempSortKeyField}}}),
          expCtx->getCollator()}) {
    _memUsageTracker.set(sizeof(*this));
}

bool AccumulatorRankBase::isNewValue(Value thisInput) {
    if (!_lastInput) {
        return true;
    }

    if (_legacySortKeyGen.has_value()) {
        return !legacyIsSameValue(getExpressionContext()->getValueComparator(),
                                  *_legacySortKeyGen,
                                  _lastInput.value(),
                                  thisInput);
    }
    // Modern expectation is that the input values are sort keys, which can be directly compared.
    // This comparison should ignore the collation, since that was already taken into account when
    // generating the sort keys.
    return ValueComparator::kInstance.evaluate(_lastInput.value() != thisInput);
}

void AccumulatorRankBase::reset() {
    _lastInput = boost::none;
    _lastRank = 0;
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorRank::reset() {
    _lastInput = boost::none;
    _numSameRank = 1;
    _lastRank = 0;
    _memUsageTracker.set(sizeof(*this));
}
}  // namespace mongo
