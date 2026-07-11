// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_covariance.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/util/assert_util.h"


namespace mongo {

REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(covarianceSamp,
                                          AccumulatorCovarianceSamp,
                                          WindowFunctionCovarianceSamp);

REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(covariancePop,
                                          AccumulatorCovariancePop,
                                          WindowFunctionCovariancePop);

void AccumulatorCovariance::processInternal(const Value& input, bool merging) {
    tassert(5424000, "$covariance can't be merged", !merging);

    _covarianceWF.add(input);
}

AccumulatorCovariance::AccumulatorCovariance(ExpressionContext* const expCtx, bool isSamp)
    : AccumulatorForWindowFunctions(expCtx), _covarianceWF(expCtx, isSamp) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorCovariance::reset() {
    _covarianceWF.reset();
}

Value AccumulatorCovariance::getValue(bool toBeMerged) {
    return _covarianceWF.getValue();
}

}  // namespace mongo
