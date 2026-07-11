// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"


namespace mongo {

REGISTER_ACCUMULATOR(first, genericParseSingleExpressionAccumulator<AccumulatorFirst>);

void AccumulatorFirst::processInternal(const Value& input, bool merging) {
    /* only remember the first value seen */
    if (!_haveFirst) {
        // can't use pValue.missing() since we want the first value even if missing
        _haveFirst = true;
        _first = input;
        _memUsageTracker.set(sizeof(*this) + input.getApproximateSize() - sizeof(Value));
        _needsInput = false;
    }
}

Value AccumulatorFirst::getValue(bool toBeMerged) {
    return _first;
}

AccumulatorFirst::AccumulatorFirst(ExpressionContext* const expCtx)
    : AccumulatorState(expCtx), _haveFirst(false) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorFirst::reset() {
    _haveFirst = false;
    _first = Value();
    _memUsageTracker.set(sizeof(*this));
}

}  // namespace mongo
