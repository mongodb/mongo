// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_integral.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_STABLE_WINDOW_FUNCTION(integral, mongo::window_function::ExpressionIntegral::parse);

AccumulatorIntegral::AccumulatorIntegral(ExpressionContext* const expCtx,
                                         boost::optional<long long> unitMillis)
    : AccumulatorForWindowFunctions(expCtx),
      _integralWF(expCtx, unitMillis, true /* isNonremovable */) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorIntegral::processInternal(const Value& input, bool merging) {
    tassert(5558800, "$integral can't be merged", !merging);

    _integralWF.add(input);
    _memUsageTracker.set(sizeof(*this) + _integralWF.getApproximateSize() - sizeof(_integralWF));
}

Value AccumulatorIntegral::getValue(bool toBeMerged) {
    return _integralWF.getValue();
}

void AccumulatorIntegral::reset() {
    _integralWF.reset();
    _memUsageTracker.set(sizeof(*this));
}

}  // namespace mongo
