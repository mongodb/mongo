// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/double_double_sum_state.h"

#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/summation.h"

#include <cstdint>
#include <tuple>

namespace mongo::sbe::value {

DoubleDoubleSumState DoubleDoubleSumState::read(const Array& array) {
    tassert(5755310,
            str::stream() << "The result slot must have at least "
                          << AggSumValueElems::kMaxSizeOfArray - 1
                          << " elements but got: " << array.size(),
            array.size() >= AggSumValueElems::kMaxSizeOfArray - 1);
    tassert(5755314,
            str::stream() << "The result slot must have at most "
                          << AggSumValueElems::kMaxSizeOfArray
                          << " elements but got: " << array.size(),
            array.size() <= AggSumValueElems::kMaxSizeOfArray);

    DoubleDoubleSumState state;
    // Only uses tag information from the kNonDecimalTotalTag element.
    state.nonDecimalTag = array.getAt(AggSumValueElems::kNonDecimalTotalTag).tag;
    tassert(5755311,
            "The nonDecimalTag can't be NumberDecimal",
            state.nonDecimalTag != TypeTags::NumberDecimal);

    // Only uses values from the kNonDecimalTotalSum/kNonDecimalTotalAddend elements.
    auto sumElement = array.getAt(AggSumValueElems::kNonDecimalTotalSum);
    auto addendElement = array.getAt(AggSumValueElems::kNonDecimalTotalAddend);
    tassert(5755312,
            "The sum and addend must be NumberDouble",
            sumElement.tag == addendElement.tag && sumElement.tag == TypeTags::NumberDouble);
    state.sum = bitcastTo<double>(sumElement.value);
    state.addend = bitcastTo<double>(addendElement.value);

    // An absent 'kDecimalTotal' element means that we've not seen any decimal value.
    state.hasDecimal = array.size() == AggSumValueElems::kMaxSizeOfArray;
    if (state.hasDecimal) {
        auto decimalElement = array.getAt(AggSumValueElems::kDecimalTotal);
        tassert(5755315,
                "The decimalTotal must be NumberDecimal",
                decimalElement.tag == TypeTags::NumberDecimal);
        state.decimalTotal = bitcastTo<Decimal128>(decimalElement.value);
    }
    return state;
}

void DoubleDoubleSumState::add(TypeTags tag, Value val) {
    if (!isNumber(tag)) {
        return;
    }

    // We're guaranteed to always have a valid nonDecimalTotal value.
    auto total = DoubleDoubleSummation::create(sum, addend);
    if (tag == TypeTags::NumberDecimal) {
        auto decimal = bitcastTo<Decimal128>(val);
        decimalTotal = hasDecimal ? decimalTotal.add(decimal) : decimal;
        // Hit the first decimal.
        hasDecimal = true;
    } else {
        nonDecimalTag = getWidestNumericalType(nonDecimalTag, tag);
        switch (tag) {
            case TypeTags::NumberInt32:
                total.addInt(bitcastTo<int32_t>(val));
                break;
            case TypeTags::NumberInt64:
                total.addLong(bitcastTo<int64_t>(val));
                break;
            case TypeTags::NumberDouble:
                total.addDouble(bitcastTo<double>(val));
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(5755316);
        }
    }
    std::tie(sum, addend) = total.getDoubleDouble();
}

void DoubleDoubleSumState::write(Array& array) const {
    array.setAt(AggSumValueElems::kNonDecimalTotalTag, nonDecimalTag, bitcastFrom<int32_t>(0));
    array.setAt(
        AggSumValueElems::kNonDecimalTotalSum, TypeTags::NumberDouble, bitcastFrom<double>(sum));
    array.setAt(AggSumValueElems::kNonDecimalTotalAddend,
                TypeTags::NumberDouble,
                bitcastFrom<double>(addend));
    if (hasDecimal) {
        auto [tag, val] = makeCopyDecimal(decimalTotal);
        if (array.size() < AggSumValueElems::kMaxSizeOfArray) {
            array.push_back_raw(tag, val);
        } else {
            array.setAt(AggSumValueElems::kDecimalTotal, tag, val);
        }
    }
}

TagValueOwned DoubleDoubleSumState::finalize() const {
    // We're guaranteed to always have a valid nonDecimalTotal value.
    auto total = DoubleDoubleSummation::create(sum, addend);
    if (hasDecimal) {
        // We've seen a decimal value.
        return TagValueOwned::fromRaw(makeCopyDecimal(decimalTotal.add(total.getDecimal())));
    }

    // We've not seen any decimal value.
    switch (nonDecimalTag) {
        case TypeTags::NumberInt32:
        case TypeTags::NumberInt64:
            if (total.fitsLong()) {
                auto longVal = total.getLong();
                if (auto intVal = static_cast<int32_t>(longVal);
                    nonDecimalTag == TypeTags::NumberInt32 && intVal == longVal) {
                    return TagValueOwned::numberInt32(intVal);
                }
                return TagValueOwned::numberInt64(longVal);
            }

            // Sum doesn't fit a NumberLong, so return a NumberDouble instead.
            [[fallthrough]];
        case TypeTags::NumberDouble:
            return TagValueOwned::numberDouble(total.getDouble());
        default:
            MONGO_UNREACHABLE_TASSERT(5755324);
    }
}

}  // namespace mongo::sbe::value
