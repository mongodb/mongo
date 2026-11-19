// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"


namespace mongo {

REGISTER_ACCUMULATOR(last, genericParseSingleExpressionAccumulator<AccumulatorLast>);

void AccumulatorLast::processInternal(const Value& input, bool merging) {
    /* always remember the last value seen */
    _last = input;
    _memUsage = sizeof(*this) + _last.getApproximateSize() - sizeof(Value);
}

Value AccumulatorLast::getValue(bool toBeMerged) {
    return _last;
}

AccumulatorLast::AccumulatorLast(ExpressionContext* const expCtx) : AccumulatorState(expCtx) {
    _memUsage = sizeof(*this);
}

void AccumulatorLast::reset() {
    _memUsage = sizeof(*this);
    _last = Value();
}

}  // namespace mongo
