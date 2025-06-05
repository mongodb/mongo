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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_avg.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/summation.h"

#include <cstdint>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

template <>
Value ExpressionFromAccumulator<AccumulatorAvg>::evaluate(const Document& root,
                                                          Variables* variables) const {
    return evaluateAccumulator(*this, root, variables);
}

REGISTER_ACCUMULATOR(avg, genericParseSingleExpressionAccumulator<AccumulatorAvg>);
REGISTER_STABLE_EXPRESSION(avg, ExpressionFromAccumulator<AccumulatorAvg>::parse);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(avg, AccumulatorAvg, WindowFunctionAvg);

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
        assertMergingInputType(input, BSONType::object);

        auto partialSumVal = input[stage_builder::partialSumName];
        tassert(6422700, "'ps' field must be present", !partialSumVal.missing());
        tassert(6422701, "'ps' field must be an array", partialSumVal.isArray());

        // The merge-side must be ready to process the full state of a partial sum from a
        // shard-side if a shard chooses to do so. See Accumulator::getValue() for details.
        applyPartialSum(partialSumVal.getArray(),
                        _nonDecimalTotalType,
                        _totalType,
                        _nonDecimalTotal,
                        _decimalTotal);
        _count += input[stage_builder::countName].getLong();

        return;
    }

    if (!input.numeric()) {
        return;
    }

    _totalType = Value::getWidestNumeric(_totalType, input.getType());

    // Keep the nonDecimalTotal's type so that the type information can be serialized too for
    // 'toBeMerged' scenarios.
    if (input.getType() != BSONType::numberDecimal) {
        _nonDecimalTotalType = Value::getWidestNumeric(_nonDecimalTotalType, input.getType());
    }

    switch (input.getType()) {
        case BSONType::numberDecimal:
            _decimalTotal = _decimalTotal.add(input.getDecimal());
            break;
        case BSONType::numberLong:
            // Avoid summation using double as that loses precision.
            _nonDecimalTotal.addLong(input.getLong());
            break;
        case BSONType::numberInt:
            _nonDecimalTotal.addInt(input.getInt());
            break;
        case BSONType::numberDouble:
            _nonDecimalTotal.addDouble(input.getDouble());
            break;
        default:
            MONGO_UNREACHABLE;
    }
    _count++;
}

Decimal128 AccumulatorAvg::_getDecimalTotal() const {
    return _decimalTotal.add(_nonDecimalTotal.getDecimal());
}

Value AccumulatorAvg::getValue(bool toBeMerged) {
    if (toBeMerged) {
        auto partialSumVal =
            serializePartialSum(_nonDecimalTotalType, _totalType, _nonDecimalTotal, _decimalTotal);
        return Value(Document{{stage_builder::countName, _count},
                              {stage_builder::partialSumName, partialSumVal}});
    }

    if (_count == 0)
        return Value(BSONNULL);

    if (_totalType == BSONType::numberDecimal)
        return Value(_getDecimalTotal().divide(Decimal128(static_cast<int64_t>(_count))));

    return Value(_nonDecimalTotal.getDouble() / static_cast<double>(_count));
}

AccumulatorAvg::AccumulatorAvg(ExpressionContext* const expCtx)
    : AccumulatorState(expCtx), _count(0) {
    // This is a fixed size AccumulatorState so we never need to update this
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorAvg::reset() {
    _totalType = BSONType::numberInt;
    _nonDecimalTotalType = BSONType::numberInt;
    _nonDecimalTotal = {};
    _decimalTotal = {};
    _count = 0;
}
}  // namespace mongo
