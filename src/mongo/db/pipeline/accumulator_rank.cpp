// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/util/assert_util.h"

#include <boost/none.hpp>


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
