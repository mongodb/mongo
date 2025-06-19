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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/pipeline/accumulator.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_avg.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/platform/decimal128.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(avg, genericParseSingleExpressionAccumulator<AccumulatorAvg>);
REGISTER_STABLE_EXPRESSION(avg, ExpressionFromAccumulator<AccumulatorAvg>::parse);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(avg, AccumulatorAvg, WindowFunctionAvg);

namespace {
// TODO SERVER-64227 Remove 'subTotal' and 'subTotalError' fields when we branch for 6.1 because all
// nodes in a sharded cluster would use the new data format.
const char subTotalName[] = "subTotal";
const char subTotalErrorName[] = "subTotalError";  // Used for extra precision
const char partialSumName[] = "ps";                // Used for the full state of partial sum
const char countName[] = "count";
}  // namespace

void applyPartialSum(const std::vector<Value>& arr,
                     BSONType& nonDecimalTotalType,
                     BSONType& totalType,
                     DoubleDoubleSummation& nonDecimalTotal,
                     Decimal128& decimalTotal);

Value serializePartialSum(BSONType nonDecimalTotalType,
                          BSONType totalType,
                          const DoubleDoubleSummation& nonDecimalTotal,
                          const Decimal128& decimalTotal);

void AccumulatorAvg::processInternal(const Value& input, bool merging) {
    if (merging) {
        // We expect an object that contains both a partial sum and a count.
        assertMergingInputType(input, Object);

        // TODO SERVER-64227 Remove 'if' block when we branch for 6.1 because all nodes in a sharded
        // cluster would use the new data format.
        if (auto partialSumVal = input[partialSumName]; partialSumVal.missing()) {
            // We're recursively adding the subtotal to get the proper type treatment, but this only
            // increments the count by one, so adjust the count afterwards. Similarly for 'error'.
            processInternal(input[subTotalName], false);
            _count += input[countName].getLong() - 1;
            Value error = input[subTotalErrorName];
            if (!error.missing()) {
                processInternal(error, false);
                _count--;  // The error correction only adjusts the total, not the number of items.
            }
        } else {
            // The merge-side must be ready to process the full state of a partial sum from a
            // shard-side if a shard chooses to do so. See Accumulator::getValue() for details.
            applyPartialSum(partialSumVal.getArray(),
                            _nonDecimalTotalType,
                            _totalType,
                            _nonDecimalTotal,
                            _decimalTotal);
            _count += input[countName].getLong();
        }

        return;
    }

    if (!input.numeric()) {
        return;
    }

    _totalType = Value::getWidestNumeric(_totalType, input.getType());

    // Keep the nonDecimalTotal's type so that the type information can be serialized too for
    // 'toBeMerged' scenarios.
    if (input.getType() != NumberDecimal) {
        _nonDecimalTotalType = Value::getWidestNumeric(_nonDecimalTotalType, input.getType());
    }

    switch (input.getType()) {
        case NumberDecimal:
            _decimalTotal = _decimalTotal.add(input.getDecimal());
            break;
        case NumberLong:
            // Avoid summation using double as that loses precision.
            _nonDecimalTotal.addLong(input.getLong());
            break;
        case NumberInt:
            _nonDecimalTotal.addInt(input.getInt());
            break;
        case NumberDouble:
            _nonDecimalTotal.addDouble(input.getDouble());
            break;
        default:
            MONGO_UNREACHABLE;
    }
    _count++;
}

intrusive_ptr<AccumulatorState> AccumulatorAvg::create(ExpressionContext* const expCtx) {
    return new AccumulatorAvg(expCtx);
}

Decimal128 AccumulatorAvg::_getDecimalTotal() const {
    return _decimalTotal.add(_nonDecimalTotal.getDecimal());
}

Value AccumulatorAvg::getValue(bool toBeMerged) {
    if (toBeMerged) {
        auto partialSumVal =
            serializePartialSum(_nonDecimalTotalType, _totalType, _nonDecimalTotal, _decimalTotal);
        if (_totalType == NumberDecimal) {
            return Value(Document{{subTotalName, _getDecimalTotal()},
                                  {countName, _count},
                                  {partialSumName, partialSumVal}});
        }

        auto [total, error] = _nonDecimalTotal.getDoubleDouble();
        return Value(Document{{subTotalName, total},
                              {countName, _count},
                              {subTotalErrorName, error},
                              {partialSumName, partialSumVal}});
    }

    if (_count == 0)
        return Value(BSONNULL);

    if (_totalType == NumberDecimal)
        return Value(_getDecimalTotal().divide(Decimal128(static_cast<int64_t>(_count))));

    return Value(_nonDecimalTotal.getDouble() / static_cast<double>(_count));
}

AccumulatorAvg::AccumulatorAvg(ExpressionContext* const expCtx)
    : AccumulatorState(expCtx), _count(0) {
    // This is a fixed size AccumulatorState so we never need to update this
    _memUsageBytes = sizeof(*this);
}

void AccumulatorAvg::reset() {
    _totalType = NumberInt;
    _nonDecimalTotalType = NumberInt;
    _nonDecimalTotal = {};
    _decimalTotal = {};
    _count = 0;
}
}  // namespace mongo
