// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
value::TagValueMaybeOwned ByteCode::builtinBitTestPosition(ArityType arity) {
    tassert(11080056, "Unexpected arity value", arity == 3);

    auto mask = viewFromStack(0);
    auto input = viewFromStack(1);

    // Carries a flag to indicate the desired testing behavior this was invoked under. The testing
    // behavior is used to determine if we need to bail out of the bit position comparison early in
    // the depending if a bit is found to be set or unset.
    auto bitTestBehaviorView = viewFromStack(2);
    tassert(11086808,
            "Unexpected BitTestBehavior type",
            bitTestBehaviorView.tag == value::TypeTags::NumberInt32);

    if (!value::isArray(mask.tag) || !value::isBinData(input.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto bitPositions = value::getArrayView(mask.value);
    auto binDataSize = static_cast<int64_t>(value::getBSONBinDataSize(input.tag, input.value));
    auto binData = value::getBSONBinData(input.tag, input.value);
    auto bitTestBehavior = BitTestBehavior{value::bitcastTo<int32_t>(bitTestBehaviorView.value)};

    auto isBitSet = false;
    for (size_t idx = 0; idx < bitPositions->size(); ++idx) {
        auto bitPositionTagVal = bitPositions->getAt(idx);
        auto bitPosition = value::bitcastTo<int64_t>(bitPositionTagVal.value);
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
            return value::TagValueMaybeOwned::boolean(bitTestBehavior ==
                                                          BitTestBehavior::AnyClear ||
                                                      bitTestBehavior == BitTestBehavior::AnySet);
        }
    }
    return value::TagValueMaybeOwned::boolean(bitTestBehavior == BitTestBehavior::AllSet ||
                                              bitTestBehavior == BitTestBehavior::AllClear);
}

value::TagValueMaybeOwned ByteCode::builtinBitTestZero(ArityType arity) {
    tassert(11080055, "Unexpected arity value", arity == 2);
    auto mask = viewFromStack(0);
    auto input = viewFromStack(1);

    if ((mask.tag != value::TypeTags::NumberInt32 && mask.tag != value::TypeTags::NumberInt64) ||
        (input.tag != value::TypeTags::NumberInt32 && input.tag != value::TypeTags::NumberInt64)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto maskNum = value::numericCast<int64_t>(mask);
    auto inputNum = value::numericCast<int64_t>(input);
    auto result = (maskNum & inputNum) == 0;
    return value::TagValueMaybeOwned::boolean(result);
}

value::TagValueMaybeOwned ByteCode::builtinBitTestMask(ArityType arity) {
    tassert(11080054, "Unexpected arity value", arity == 2);
    auto mask = viewFromStack(0);
    auto input = viewFromStack(1);

    if ((mask.tag != value::TypeTags::NumberInt32 && mask.tag != value::TypeTags::NumberInt64) ||
        (input.tag != value::TypeTags::NumberInt32 && input.tag != value::TypeTags::NumberInt64)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto maskNum = value::numericCast<int64_t>(mask);
    auto inputNum = value::numericCast<int64_t>(input);
    auto result = (maskNum & inputNum) == maskNum;
    return value::TagValueMaybeOwned::boolean(result);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
