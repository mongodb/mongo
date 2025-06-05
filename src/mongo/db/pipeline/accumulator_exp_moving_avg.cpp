/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
