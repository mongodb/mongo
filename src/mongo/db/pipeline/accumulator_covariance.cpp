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

#include "mongo/platform/basic.h"

#include <cmath>

#include "mongo/db/pipeline/accumulator.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"

namespace mongo {

REGISTER_NON_REMOVABLE_WINDOW_FUNCTION(
    covarianceSamp, window_function::ExpressionFromAccumulator<AccumulatorCovarianceSamp>::parse);

REGISTER_NON_REMOVABLE_WINDOW_FUNCTION(
    covariancePop, window_function::ExpressionFromAccumulator<AccumulatorCovariancePop>::parse);

void AccumulatorCovariance::processInternal(const Value& input, bool merging) {
    tassert(5424000, "$covariance can't be merged", !merging);

    // Currently we only support array with 2 numeric values as input value. Other types of input or
    // non-numeric arrays have no impact on covariance.
    if (!input.isArray())
        return;
    const auto& arr = input.getArray();
    if (arr.size() != 2)
        return;
    if (!arr[0].numeric() || !arr[1].numeric())
        return;

    // This is an implementation of the following algorithm:
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online
    double x = arr[0].coerceToDouble();
    double y = arr[1].coerceToDouble();
    double dx = x - _meanX;

    _count++;
    _meanX += (dx / _count);
    _meanY += (y - _meanY) / _count;
    _cXY += dx * (y - _meanY);
}

AccumulatorCovariance::AccumulatorCovariance(ExpressionContext* const expCtx, bool isSamp)
    : AccumulatorState(expCtx), _isSamp(isSamp) {
    _memUsageBytes = sizeof(*this);
}

void AccumulatorCovariance::reset() {
    _count = 0;
    _meanX = 0;
    _meanY = 0;
    _cXY = 0;  // 0 only makes sense if "_count" > 0.
}

Value AccumulatorCovariance::getValue(bool toBeMerged) {
    const double adjustedCount = (_isSamp ? _count - 1 : _count);

    if (adjustedCount <= 0)
        return Value(BSONNULL);  // Covariance not well defined in this case.

    return Value(_cXY / adjustedCount);
}

boost::intrusive_ptr<AccumulatorState> AccumulatorCovarianceSamp::create(
    ExpressionContext* const expCtx) {
    return new AccumulatorCovarianceSamp(expCtx);
}

boost::intrusive_ptr<AccumulatorState> AccumulatorCovariancePop::create(
    ExpressionContext* const expCtx) {
    return new AccumulatorCovariancePop(expCtx);
}

}  // namespace mongo
