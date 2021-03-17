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

#include "mongo/db/pipeline/window_function/window_function_sum.h"

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

Value RemovableSum::getValue() const {
    if (_nanCount > 0) {
        return _decimalCount > 0 ? Value(Decimal128::kPositiveNaN)
                                 : Value(std::numeric_limits<double>::quiet_NaN());
    }
    if (_posInfiniteValueCount > 0 && _negInfiniteValueCount > 0) {
        return _decimalCount > 0 ? Value(Decimal128::kPositiveNaN)
                                 : Value(std::numeric_limits<double>::quiet_NaN());
    }
    if (_posInfiniteValueCount > 0) {
        return _decimalCount > 0 ? Value(Decimal128::kPositiveInfinity)
                                 : Value(std::numeric_limits<double>::infinity());
    }
    if (_negInfiniteValueCount > 0) {
        return _decimalCount > 0 ? Value(Decimal128::kNegativeInfinity)
                                 : Value(-std::numeric_limits<double>::infinity());
    }
    Value val = _sumAcc->getValue(false);
    if (val.getType() == NumberDecimal && _decimalCount == 0) {
        Decimal128 decVal = val.getDecimal();
        if (_doubleCount > 0) {  // Narrow Decimal128 to double.
            return Value(decVal.toDouble());
        }
        std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
        long long longVal = decVal.toLong(&signalingFlags);  // Narrow Decimal128 to integral.
        if (signalingFlags == Decimal128::SignalingFlag::kNoFlag) {
            return Value::createIntOrLong(longVal);
        }
        return Value(decVal.toDouble());  // Narrow Decimal128 to double if overflows long.
    }
    if (val.getType() == NumberDouble && _doubleCount == 0 &&
        val.getDouble() > std::numeric_limits<long long>::min() &&
        val.getDouble() < std::numeric_limits<long long>::max()) {  // Narrow double to integral
        return Value::createIntOrLong(llround(val.getDouble()));
    }
    if (val.getType() == NumberLong) {  // Narrow long to int
        return Value::createIntOrLong(val.getLong());
    }
    return val;
}
}  // namespace mongo
