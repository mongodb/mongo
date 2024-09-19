/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBitTestPosition(ArityType arity) {
    invariant(arity == 3);

    auto [ownedMask, maskTag, maskValue] = getFromStack(0);
    auto [ownedInput, valueTag, value] = getFromStack(1);

    // Carries a flag to indicate the desired testing behavior this was invoked under. The testing
    // behavior is used to determine if we need to bail out of the bit position comparison early in
    // the depending if a bit is found to be set or unset.
    auto [_, tagBitTestBehavior, valueBitTestBehavior] = getFromStack(2);
    invariant(tagBitTestBehavior == value::TypeTags::NumberInt32);

    if (!value::isArray(maskTag) || !value::isBinData(valueTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto bitPositions = value::getArrayView(maskValue);
    auto binDataSize = static_cast<int64_t>(value::getBSONBinDataSize(valueTag, value));
    auto binData = value::getBSONBinData(valueTag, value);
    auto bitTestBehavior = BitTestBehavior{value::bitcastTo<int32_t>(valueBitTestBehavior)};

    auto isBitSet = false;
    for (size_t idx = 0; idx < bitPositions->size(); ++idx) {
        auto [tagBitPosition, valueBitPosition] = bitPositions->getAt(idx);
        auto bitPosition = value::bitcastTo<int64_t>(valueBitPosition);
        if (bitPosition >= binDataSize * 8) {
            // If position to test is longer than the data to test against, zero-extend.
            isBitSet = false;
        } else {
            // Convert the bit position to a byte position within a byte. Note that byte positions
            // start at position 0 in the document's value BinData array representation, and bit
            // positions start at the least significant bit.
            auto byteIdx = bitPosition / 8;
            auto currentBit = bitPosition % 8;
            auto currentByte = binData[byteIdx];

            isBitSet = currentByte & (1 << currentBit);
        }

        // Bail out early if we succeed with the any case or fail with the all case. To do this, we
        // negate a test to determine if we need to continue looping over the bit position list. So
        // the first part of the disjunction checks when a bit is set and the test is invoked by the
        // AllSet or AnyClear expressions. The second test checks if a bit isn't set and we are
        // checking the AllClear or the AnySet cases.
        if (!((isBitSet &&
               (bitTestBehavior == BitTestBehavior::AllSet ||
                bitTestBehavior == BitTestBehavior::AnyClear)) ||
              (!isBitSet &&
               (bitTestBehavior == BitTestBehavior::AllClear ||
                bitTestBehavior == BitTestBehavior::AnySet)))) {
            return {false,
                    value::TypeTags::Boolean,
                    value::bitcastFrom<bool>(bitTestBehavior == BitTestBehavior::AnyClear ||
                                             bitTestBehavior == BitTestBehavior::AnySet)};
        }
    }
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(bitTestBehavior == BitTestBehavior::AllSet ||
                                     bitTestBehavior == BitTestBehavior::AllClear)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBitTestZero(ArityType arity) {
    invariant(arity == 2);
    auto [maskOwned, maskTag, maskValue] = getFromStack(0);
    auto [inputOwned, inputTag, inputValue] = getFromStack(1);

    if ((maskTag != value::TypeTags::NumberInt32 && maskTag != value::TypeTags::NumberInt64) ||
        (inputTag != value::TypeTags::NumberInt32 && inputTag != value::TypeTags::NumberInt64)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto maskNum = value::numericCast<int64_t>(maskTag, maskValue);
    auto inputNum = value::numericCast<int64_t>(inputTag, inputValue);
    auto result = (maskNum & inputNum) == 0;
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBitTestMask(ArityType arity) {
    invariant(arity == 2);
    auto [maskOwned, maskTag, maskValue] = getFromStack(0);
    auto [inputOwned, inputTag, inputValue] = getFromStack(1);

    if ((maskTag != value::TypeTags::NumberInt32 && maskTag != value::TypeTags::NumberInt64) ||
        (inputTag != value::TypeTags::NumberInt32 && inputTag != value::TypeTags::NumberInt64)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto maskNum = value::numericCast<int64_t>(maskTag, maskValue);
    auto inputNum = value::numericCast<int64_t>(inputTag, inputValue);
    auto result = (maskNum & inputNum) == maskNum;
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
