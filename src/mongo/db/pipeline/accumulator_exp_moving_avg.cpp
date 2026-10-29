// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"


namespace mongo {

REGISTER_STABLE_WINDOW_FUNCTION(expMovingAvg,
                                mongo::window_function::ExpressionExpMovingAvg::parse);

void AccumulatorExpMovingAvg::processInternal(const Value& input, bool merging) {
    tassert(5433600, "$expMovingAvg can't be merged", !merging);
    if (!input.numeric()) {
        return;
    }
    if (input.getType() == BSONType::numberDecimal) {
        _isDecimal = true;
    }
    auto decimalVal = input.coerceToDecimal();
    if (!_init) {
        _currentResult = decimalVal;
        _init = true;
    } else {
        _currentResult = decimalVal.multiply(_alpha).add(
            _currentResult.multiply(Decimal128(1).subtract(_alpha)));
    }
}

Value AccumulatorExpMovingAvg::getValue(bool toBeMerged) {
    tassert(5433601, "$expMovingAvg can't be merged", !toBeMerged);
    if (!_init) {
        return Value(BSONNULL);
    }
    if (!_isDecimal) {
        return Value(_currentResult.toDouble());
    }
    return Value(_currentResult);
}

AccumulatorExpMovingAvg::AccumulatorExpMovingAvg(ExpressionContext* const expCtx, Decimal128 alpha)
    : AccumulatorState(expCtx), _alpha(alpha) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorExpMovingAvg::reset() {
    _memUsageTracker.set(sizeof(*this));
    _init = false;
}
}  // namespace mongo
