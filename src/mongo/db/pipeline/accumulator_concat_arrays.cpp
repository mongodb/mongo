// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_concat_arrays.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"

namespace mongo {

REGISTER_ACCUMULATOR(concatArrays,
                     genericParseSingleExpressionAccumulator<AccumulatorConcatArrays>);

REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(concatArrays,
                                          AccumulatorConcatArrays,
                                          WindowFunctionConcatArrays);


AccumulatorConcatArrays::AccumulatorConcatArrays(
    ExpressionContext* const expCtx, boost::optional<MemoryUsageLimit> maxMemoryUsageBytes)
    : AccumulatorState(
          expCtx,
          maxMemoryUsageBytes.value_or(MemoryUsageLimit{query_knobs::kMaxConcatArraysBytes})) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorConcatArrays::processInternal(const Value& input, bool merging) {
    // The main difference here between the merging and non-merging case is that if the input is
    // malformed (i.e. not an array) in the non-merging case, that is a user error (we required that
    // the input to this accumulator is of type array). If the input is malformed in the merging
    // case, that is a programming error.
    if (!merging) {
        if (input.missing()) {
            // Do nothing if the input is missing - this indicates that there are no values to add.
            return;
        }

        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "$concatArrays requires array inputs, but input "
                              << redact(input.toString()) << " is of type "
                              << typeName(input.getType()),
                input.isArray());
    } else {
        tassert(ErrorCodes::TypeMismatch,
                str::stream() << "$concatArrays requires array inputs, but input "
                              << redact(input.toString()) << " is of type "
                              << typeName(input.getType()),
                input.isArray());
    }

    // In both cases (merging and non-merging), we take apart the arrays we receive and put their
    // elements into the array we are collecting. Otherwise, we would get an array of arrays.
    addValuesFromArray(input);
}

void AccumulatorConcatArrays::addValuesFromArray(const Value& values) {
    _memUsageTracker.add(values.getApproximateSize());

    checkMemUsage();

    _array.insert(_array.end(), values.getArray().begin(), values.getArray().end());
}

Value AccumulatorConcatArrays::getValue(bool) {
    return Value(_array);
}

void AccumulatorConcatArrays::reset() {
    std::vector<Value>().swap(_array);
    _memUsageTracker.set(sizeof(*this));
}

}  // namespace mongo
