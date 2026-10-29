// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_sum.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/window_function/window_function_state_serialization.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Value RemovableSum::getValue(boost::optional<Value> current) const {
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
    if (val.getType() == BSONType::numberDecimal && _decimalCount == 0) {
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
    if (val.getType() == BSONType::numberDouble && _doubleCount == 0 &&
        val.getDouble() >= std::numeric_limits<long long>::min() &&
        val.getDouble() < static_cast<double>(std::numeric_limits<long long>::max())) {
        return Value::createIntOrLong(llround(val.getDouble()));  // Narrow double to integral.
    }
    if (val.getType() == BSONType::numberLong) {  // Narrow long to int
        return Value::createIntOrLong(val.getLong());
    }
    return val;
}

namespace {

constexpr std::string_view kSumField = "sum";
constexpr std::string_view kPosInfCountField = "posInfCount";
constexpr std::string_view kNegInfCountField = "negInfCount";
constexpr std::string_view kNanCountField = "nanCount";
constexpr std::string_view kDoubleCountField = "doubleCount";
constexpr std::string_view kDecimalCountField = "decimalCount";
constexpr int kStateVersion = 1;

}  // namespace

Value RemovableSum::serializeState() const {
    // The inner sum is stored in AccumulatorSum's mergeable form so it can be loaded back with a
    // merging process() call, which preserves the running total type.
    return Value(Document{{window_function_state::kVersionField, kStateVersion},
                          {kSumField, _sumAcc->getValue(true /* toBeMerged */)},
                          {kPosInfCountField, _posInfiniteValueCount},
                          {kNegInfCountField, _negInfiniteValueCount},
                          {kNanCountField, _nanCount},
                          {kDoubleCountField, _doubleCount},
                          {kDecimalCountField, _decimalCount}});
}

void RemovableSum::restoreState(const Value& state) {
    auto doc =
        window_function_state::validateVersionedStateDoc(state, kStateVersion, "RemovableSum");
    _sumAcc->reset();
    _sumAcc->process(doc[kSumField], true /* merging */);
    _posInfiniteValueCount = window_function_state::getIntField(doc, kPosInfCountField);
    _negInfiniteValueCount = window_function_state::getIntField(doc, kNegInfCountField);
    _nanCount = window_function_state::getIntField(doc, kNanCountField);
    _doubleCount = window_function_state::getLongField(doc, kDoubleCountField);
    _decimalCount = window_function_state::getLongField(doc, kDecimalCountField);
}
}  // namespace mongo
