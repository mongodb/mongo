/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_set_union.h"

namespace mongo {

REGISTER_ACCUMULATOR_WITH_FEATURE_FLAG(setUnion,
                                       genericParseSingleExpressionAccumulator<AccumulatorSetUnion>,
                                       &feature_flags::gFeatureFlagArrayAccumulators);

REGISTER_WINDOW_FUNCTION_WITH_FEATURE_FLAG(
    setUnion,
    (mongo::window_function::ExpressionRemovable<AccumulatorSetUnion,
                                                 WindowFunctionSetUnion>::parse),
    &feature_flags::gFeatureFlagArrayAccumulators,
    AllowedWithApiStrict::kAlways);

AccumulatorSetUnion::AccumulatorSetUnion(ExpressionContext* const expCtx,
                                         boost::optional<int> maxMemoryUsageBytes)
    : AccumulatorState(expCtx, maxMemoryUsageBytes.value_or(internalQueryMaxSetUnionBytes.load())),
      _set(expCtx->getValueComparator().makeFlatUnorderedValueSet()) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorSetUnion::addValues(const std::vector<Value>& values) {
    for (auto&& val : values) {
        bool inserted = _set.insert(val).second;
        if (inserted) {
            _memUsageTracker.add(val.getApproximateSize());
            uassert(ErrorCodes::ExceededMemoryLimit,
                    str::stream() << "$setUnion used too much memory and spilling to disk will not "
                                     "reduce memory usage. Used: "
                                  << _memUsageTracker.inUseTrackedMemoryBytes()
                                  << "bytes. Memory limit: "
                                  << _memUsageTracker.maxAllowedMemoryUsageBytes() << " bytes",
                    _memUsageTracker.withinMemoryLimit());
        }
    }
}

void AccumulatorSetUnion::processInternal(const Value& input, bool merging) {
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
                str::stream() << "$setUnion requires array inputs, but input "
                              << redact(input.toString()) << " is of type "
                              << typeName(input.getType()),
                input.isArray());
    } else {
        tassert(ErrorCodes::TypeMismatch,
                str::stream() << "$setUnion requires array inputs, but input "
                              << redact(input.toString()) << " is of type "
                              << typeName(input.getType()),
                input.isArray());
    }

    // In both cases (merging and non-merging), we take apart the arrays we receive and put their
    // elements into the array we are collecting. Otherwise, we would get an array of arrays.
    addValues(input.getArray());
}

Value AccumulatorSetUnion::getValue(bool) {
    return Value(std::vector<Value>(_set.begin(), _set.end()));
}

void AccumulatorSetUnion::reset() {
    _set = getExpressionContext()->getValueComparator().makeFlatUnorderedValueSet();
    _memUsageTracker.set(sizeof(*this));
}

}  // namespace mongo
