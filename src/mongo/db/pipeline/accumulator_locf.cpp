// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"


namespace mongo {

REGISTER_WINDOW_FUNCTION(
    locf,
    mongo::window_function::ExpressionFromLeftUnboundedWindowFunction<AccumulatorLocf>::parse,
    AllowedWithApiStrict::kAlways);

AccumulatorLocf::AccumulatorLocf(ExpressionContext* const expCtx)
    : AccumulatorForWindowFunctions(expCtx) {
    _memUsageTracker.set(sizeof(*this) + _lastNonNull.getApproximateSize());
}

void AccumulatorLocf::processInternal(const Value& input, bool merging) {
    tassert(6050100, "$locf can't be merged", !merging);

    if (!input.nullish()) {
        _lastNonNull = input;
        _memUsageTracker.set(sizeof(*this) + _lastNonNull.getApproximateSize());
    }
}

Value AccumulatorLocf::getValue(bool toBeMerged) {
    tassert(6050102, "$locf can't be merged", !toBeMerged);
    return _lastNonNull;
}

void AccumulatorLocf::reset() {
    _lastNonNull = Value(BSONNULL);
    _memUsageTracker.set(sizeof(*this) + _lastNonNull.getApproximateSize());
}

}  // namespace mongo
