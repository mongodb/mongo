/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/in_list.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/generic_compare.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/represent_as.h"

#include <algorithm>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe::vm {
using ColumnOpType = value::ColumnOpType;

namespace {

bool allBools(const value::TypeTags* tag, size_t sz) {
    for (size_t i = 0; i < sz; ++i) {
        if (tag[i] != value::TypeTags::Boolean) {
            return false;
        }
    }
    return true;
}

bool emptyPositionInfo(const std::vector<int32_t>& positionInfo) {
    return positionInfo.empty() ||
        std::all_of(
               positionInfo.begin(), positionInfo.end(), [](const int32_t& c) { return c == 1; });
}
}  // namespace

/*
 * Given a ValueBlock as input, returns a BoolBlock indicating whether each value in the input was
 * non-Nothing (true) or Nothing (false).
 */
value::TagValueOwned ByteCode::builtinValueBlockExists(ArityType arity) {
    tassert(11079924, "Unexpected arity value", arity == 1);
    auto input = viewFromStack(0);

    tassert(8625700,
            "Expected argument to be of valueBlock type",
            input.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(input.value);

    auto out = valueBlockIn->exists();

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(out.release()));
}

/*
 * Given a ValueBlock as input, returns a BoolBlock indicating whether each value in the input was
 * either Nothing, Null or Undefined.
 */
value::TagValueOwned ByteCode::builtinValueBlockIsNullish(ArityType arity) {
    tassert(12697000, "Unexpected arity value", arity == 1);
    auto input = viewFromStack(0);

    tassert(12697001,
            "Expected argument to be of valueBlock type",
            input.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(input.value);

    uint32_t nullUndefinedTypeMask = static_cast<uint32_t>(getBSONTypeMask(BSONType::null) |
                                                           getBSONTypeMask(BSONType::undefined));
    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> std::pair<value::TypeTags, value::Value> {
            if (tag == value::TypeTags::Nothing) {
                return {value::TypeTags::Boolean, value::bitcastFrom<bool>(true)};
            }
            return {value::TypeTags::Boolean,
                    value::bitcastFrom<bool>(
                        static_cast<bool>(getBSONTypeMask(tag) & nullUndefinedTypeMask))};
        },
        [&](value::TypeTags inTag,
            const value::Value* inVals,
            value::TypeTags* outTags,
            value::Value* outVals,
            size_t count) {
            auto [outTag, outVal] = [&]() -> std::pair<value::TypeTags, value::Value> {
                if (inTag == value::TypeTags::Nothing) {
                    return {value::TypeTags::Boolean, value::bitcastFrom<bool>(true)};
                }
                return {value::TypeTags::Boolean,
                        value::bitcastFrom<bool>(
                            static_cast<bool>(getBSONTypeMask(inTag) & nullUndefinedTypeMask))};
            }();

            for (size_t index = 0; index < count; index++) {
                outTags[index] = outTag;
                outVals[index] = outVal;
            }
        });

    auto valueBlockOut = valueBlockIn->map(cmpOp);

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(valueBlockOut.release()));
}

/* This instruction takes as input a ValueBlock and a type mask and returns a ValueBlock indicating
 * whether each value in the ValueBlock is of the type defined by the type mask. If the value is
 * Nothing then Nothing is returned. If no type mask is provided, it returns a MonoBlock with
 * Nothing.
 */
value::TagValueOwned ByteCode::builtinValueBlockTypeMatch(ArityType arity) {
    tassert(11079923, "Unexpected arity value", arity == 2);

    auto input = viewFromStack(0);
    tassert(8300800,
            "First argument of valueBlockTypeMatch must be block of values",
            input.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(input.value);

    auto typeMaskView = viewFromStack(1);
    if (typeMaskView.tag != value::TypeTags::NumberInt32) {
        return value::TagValueOwned(
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(
                value::MonoBlock::makeNothingBlock(valueBlockIn->count()).release()));
    }

    auto typeMask = static_cast<uint32_t>(value::bitcastTo<int32_t>(typeMaskView.value));

    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> std::pair<value::TypeTags, value::Value> {
            if (tag == value::TypeTags::Nothing) {
                return {value::TypeTags::Nothing, 0};
            }
            return {value::TypeTags::Boolean,
                    value::bitcastFrom<bool>(static_cast<bool>(getBSONTypeMask(tag) & typeMask))};
        },
        [&](value::TypeTags inTag,
            const value::Value* inVals,
            value::TypeTags* outTags,
            value::Value* outVals,
            size_t count) {
            auto [outTag, outVal] = [&]() -> std::pair<value::TypeTags, value::Value> {
                if (inTag == value::TypeTags::Nothing) {
                    return {value::TypeTags::Nothing, 0};
                }
                return {
                    value::TypeTags::Boolean,
                    value::bitcastFrom<bool>(static_cast<bool>(getBSONTypeMask(inTag) & typeMask))};
            }();

            for (size_t index = 0; index < count; index++) {
                outTags[index] = outTag;
                outVals[index] = outVal;
            }
        });

    auto valueBlockOut = valueBlockIn->map(cmpOp);

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(valueBlockOut.release()));
}

/* This instruction takes as input a timezoneDB and a ValueBlock and returns a ValueBlock indicating
 * whether each value in the ValueBlock is a valid timezone. If no timezoneDB is provided, it
 * returns a MonoBlock with Nothing.
 */
value::TagValueOwned ByteCode::builtinValueBlockIsTimezone(ArityType arity) {
    tassert(11079922, "Unexpected arity value", arity == 2);

    auto input = viewFromStack(1);
    tassert(8300801,
            "Second argument of valueBlockIsTimezone must be block of values",
            input.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(input.value);

    auto timezoneDBView = viewFromStack(0);
    if (timezoneDBView.tag != value::TypeTags::timeZoneDB) {
        auto nothingBlock =
            std::make_unique<value::MonoBlock>(valueBlockIn->count(), value::TypeTags::Nothing, 0);
        return value::TagValueOwned(value::TypeTags::valueBlock,
                                    value::bitcastFrom<value::ValueBlock*>(nothingBlock.release()));
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBView.value);

    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> std::pair<value::TypeTags, value::Value> {
            if (!value::isString(tag)) {
                return {value::TypeTags::Boolean, false};
            }
            auto timezoneStr = value::getStringView(tag, val);
            return {value::TypeTags::Boolean,
                    value::bitcastFrom<bool>(timezoneDB->isTimeZoneIdentifier(timezoneStr))};
        });

    auto valueBlockOut = valueBlockIn->map(cmpOp);

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(valueBlockOut.release()));
}

/**
 * Implementation of the valueBlockFillEmpty builtin. This instruction takes a block and an
 * SBE value, and produces a new block where all missing values in the block have been replaced
 * with the SBE value.
 */
value::TagValueMaybeOwned ByteCode::builtinValueBlockFillEmpty(ArityType arity) {
    tassert(11079921, "Unexpected arity value", arity == 2);
    auto fill = viewFromStack(1);
    if (fill.tag == value::TypeTags::Nothing) {
        return moveMaybeOwnedFromStack(0);
    }

    auto block = viewFromStack(0);
    tassert(8625701,
            "Expected argument to be of valueBlock type",
            block.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(block.value);

    auto out = valueBlockIn->fillEmpty(fill.tag, fill.value);
    if (!out) {
        // Input block was dense so we can just return it unmodified.
        return moveMaybeOwnedFromStack(0);
    }

    return value::TagValueMaybeOwned(
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release()));
}

/**
 * Implementation of the valueBlockFillEmptyBlock builtin. This instruction takes two blocks of the
 * same size, and produces a new block where all missing values in the first block have been
 * replaced with the correpsonding value in the second block.
 */
value::TagValueMaybeOwned ByteCode::builtinValueBlockFillEmptyBlock(ArityType arity) {
    tassert(11079920, "Unexpected arity value", arity == 2);
    auto fill = viewFromStack(1);
    if (fill.tag == value::TypeTags::Nothing) {
        return moveMaybeOwnedFromStack(0);
    }
    auto block = viewFromStack(0);
    tassert(8141618,
            "Arguments of valueBlockFillEmptyBlock must be block of values",
            fill.tag == value::TypeTags::valueBlock && block.tag == value::TypeTags::valueBlock);

    auto* fillBlockIn = value::bitcastTo<value::ValueBlock*>(fill.value);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(block.value);

    if (valueBlockIn->tryDense().get_value_or(false)) {
        return moveMaybeOwnedFromStack(0);
    }

    auto extractedFill = fillBlockIn->extract();
    auto extractedValue = valueBlockIn->extract();
    tassert(8141601,
            "Fill value and block have a different number of items",
            extractedFill.count() == extractedValue.count());

    std::vector<value::Value> valueOut(extractedValue.count());
    std::vector<value::TypeTags> tagOut(extractedValue.count(), value::TypeTags::Nothing);

    for (size_t i = 0; i < extractedValue.count(); ++i) {
        if (extractedValue.tags()[i] == value::TypeTags::Nothing) {
            std::tie(tagOut[i], valueOut[i]) =
                value::copyValue(extractedFill.tags()[i], extractedFill.vals()[i]);
        } else {
            std::tie(tagOut[i], valueOut[i]) =
                value::copyValue(extractedValue.tags()[i], extractedValue.vals()[i]);
        }
    }
    auto res = buildBlockFromStorage(std::move(tagOut), std::move(valueOut));

    return value::TagValueMaybeOwned(
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release()));
}

/**
 * Implementation of the valueBlockFillType builtin. This instruction takes a block a BSON type
 * mask, and an SBE value, and produces a new block where all values that match the type mask are
 * replaced with the SBE value. Any Nothings in the input will be preserved as Nothings in the
 * output regardless of what the type mask is.
 */
value::TagValueMaybeOwned ByteCode::builtinValueBlockFillType(ArityType arity) {
    tassert(11079919, "Unexpected arity value", arity == 3);
    auto fill = viewFromStack(2);

    auto typeMaskView = viewFromStack(1);

    auto blockView = viewFromStack(0);
    tassert(8872900,
            "Expected argument to be of valueBlock type",
            blockView.tag == value::TypeTags::valueBlock);
    auto* block = value::bitcastTo<value::ValueBlock*>(blockView.value);

    if (typeMaskView.tag != value::TypeTags::NumberInt32) {
        return value::TagValueMaybeOwned(
            true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(
                value::MonoBlock::makeNothingBlock(block->count()).release()));
    }
    uint32_t typeMask = static_cast<uint32_t>(value::bitcastTo<int32_t>(typeMaskView.value));

    auto out = block->fillType(typeMask, fill.tag, fill.value);
    if (!out) {
        // Input block didn't have any non-Nothing values that matched the type mask so we can
        // return it unmodified.
        return moveMaybeOwnedFromStack(0);
    }

    return value::TagValueMaybeOwned(
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release()));
}

template <bool less>
value::TagValueOwned ByteCode::valueBlockMinMaxImpl(value::ValueBlock* inputBlock,
                                                    value::ValueBlock* bitsetBlock) {
    if (bitsetBlock->allTrue().get_value_or(false)) {
        if constexpr (less) {
            auto [minTag, minVal] = inputBlock->tryMin();
            if (minTag != value::TypeTags::Nothing) {
                auto [minTagCpy, minValCpy] = value::copyValue(minTag, minVal);
                return value::TagValueOwned(minTagCpy, minValCpy);
            }
        } else {
            auto [maxTag, maxVal] = inputBlock->tryMax();
            if (maxTag != value::TypeTags::Nothing) {
                auto [maxTagCpy, maxValCpy] = value::copyValue(maxTag, maxVal);
                return value::TagValueOwned(maxTagCpy, maxValCpy);
            }
        }
    }

    auto block = inputBlock->extract();
    auto bitset = bitsetBlock->extract();

    value::ValueCompare<less> comp;

    tassert(
        8137400, "Expected block and bitset to be the same size", block.count() == bitset.count());
    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");

    value::TypeTags accTag = value::TypeTags::Nothing;
    value::Value accVal = 0;
    for (size_t i = 0; i < block.count(); ++i) {
        // Skip unselected and Nothing values.
        if (!value::bitcastTo<bool>(bitset[i].value) ||
            block.tags()[i] == value::TypeTags::Nothing) {
            continue;
        }
        // If our current result is nothing, set the result to be this value.
        if (accTag == value::TypeTags::Nothing) {
            accTag = block.tags()[i];
            accVal = block.vals()[i];
        } else if (comp({block.tags()[i], block.vals()[i]}, {accTag, accVal})) {
            accTag = block.tags()[i], accVal = block.vals()[i];
        }
    }

    auto [retTag, retVal] = value::copyValue(accTag, accVal);
    return value::TagValueOwned(retTag, retVal);
}

template <bool less>
value::TagValueOwned ByteCode::valueBlockAggMinMaxImpl(value::TagValueOwned acc,
                                                       value::TagValueView input,
                                                       value::TagValueView bitset) {
    tassert(8625702,
            "Expected input argument to be of valueBlock type",
            input.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(input.value);

    tassert(8625703,
            "Expected bitset argument to be of valueBlock type",
            bitset.tag == value::TypeTags::valueBlock);
    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitset.value);

    // If there is a valid accumulated value and the min(/max) value of the entire block is
    // greater(/less) than the accumulated value then we can directly return the accumulated value
    if (acc.tag() != value::TypeTags::Nothing) {
        auto [minOrMaxTag, minOrMaxVal] = less ? valueBlockIn->tryMin() : valueBlockIn->tryMax();
        if (minOrMaxTag != value::TypeTags::Nothing) {
            auto [cmpTag, cmpVal] =
                value::compare3way(minOrMaxTag, minOrMaxVal, acc.tag(), acc.value());
            if (cmpTag == value::TypeTags::NumberInt32) {
                int32_t cmp = value::bitcastTo<int32_t>(cmpVal);
                if (less ? cmp >= 0 : cmp <= 0) {
                    return acc;
                }
            }
        }
    }

    // Evaluate the min/max value in the block taking into account the bitmap
    auto result = valueBlockMinMaxImpl<less>(valueBlockIn, bitsetBlock);

    // If 'result' is not Nothing, check if we should return 'result'.
    if (result.tag() != value::TypeTags::Nothing) {
        bool returnResult = false;

        if (acc.tag() == value::TypeTags::Nothing) {
            // If 'acc' is Nothing, then we should return 'result'.
            returnResult = true;
        } else {
            // If 'acc' is not Nothing, compare 'result' with 'acc'.
            auto [tag, val] =
                value::compare3way(result.tag(), result.value(), acc.tag(), acc.value());

            if (tag == value::TypeTags::NumberInt32) {
                // If 'result' is less than or equal to 'acc' (is 'less' is true) or if 'result'
                // is greater than or equal to 'acc' (is 'less' is false), then we should return
                // 'result'.
                int32_t cmp = value::bitcastTo<int32_t>(val);
                returnResult = less ? cmp <= 0 : cmp >= 0;
            }
        }

        if (returnResult) {
            // Return result as the updated accumulator state.
            return result;
        }
    }

    // Return the current accumulator state unmodified.
    return acc;
}

/*
 * Given a ValueBlock and bitset as input, returns a tag, value pair that contains the minimum value
 * in the block based on compareValue. Values whose corresponding bit is set to false get ignored.
 * This function will return a non-Nothing value if the block contains any non-Nothing values.
 */
value::TagValueOwned ByteCode::builtinValueBlockAggMin(ArityType arity) {
    tassert(11079918, "Unexpected arity value", arity == 3);

    auto input = viewFromStack(2);
    auto bitset = viewFromStack(1);

    // Move the incoming accumulator state from the stack. We now own and have exclusive access
    // to the accumulator state and can make in-place modifications if desired.
    auto acc = moveOwnedFromStack(0);

    return valueBlockAggMinMaxImpl<true /* less */>(std::move(acc), input, bitset);
}

/*
 * Given a ValueBlock and bitset as input, returns a tag, value pair that contains the maximum value
 * in the block based on compareValue. Values whose corresponding bit is set to false get ignored.
 * This function will return a non-Nothing value if the block contains any non-Nothing values.
 */
value::TagValueOwned ByteCode::builtinValueBlockAggMax(ArityType arity) {
    tassert(11079917, "Unexpected arity value", arity == 3);

    auto input = viewFromStack(2);
    auto bitset = viewFromStack(1);

    // Move the incoming accumulator state from the stack. We now own and have exclusive access
    // to the accumulator state and can make in-place modifications if desired.
    auto acc = moveOwnedFromStack(0);

    return valueBlockAggMinMaxImpl<false /* less */>(std::move(acc), input, bitset);
}

/*
 * Given a ValueBlock bitset, count how many "true" elements there are.
 */
value::TagValueOwned ByteCode::builtinValueBlockAggCount(ArityType arity) {
    // TODO SERVER-83450 add monoblock fast path.
    tassert(11079916, "Unexpected arity value", arity == 2);

    // Move the incoming accumulator state from the stack. We now own and have exclusive access
    // to the accumulator state and can make in-place modifications if desired.
    auto acc = moveOwnedFromStack(0);

    auto bitsetView = viewFromStack(1);
    tassert(8625706,
            "Expected bitset argument to be of valueBlock type",
            bitsetView.tag == value::TypeTags::valueBlock);
    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetView.value);

    value::DeblockedTagVals bitset = bitsetBlock->extract();

    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");

    int64_t n =
        acc.tag() == value::TypeTags::NumberInt64 ? value::bitcastTo<int64_t>(acc.value()) : 0;

    int64_t count = 0;
    for (size_t i = 0; i < bitset.count(); ++i) {
        if (value::bitcastTo<bool>(bitset[i].value)) {
            ++count;
        }
    }

    return value::TagValueOwned::numberInt64(n + count);
}

/*
 * Given a ValueBlock and bitset, returns the sum of the elements of the ValueBlock where the bitset
 * indicates true. If all elements of the bitset are false, return Nothing. If there are non-Nothing
 * elements where the bitset indicates true, we return a value. If there are only Nothing elements,
 * we return Nothing.
 */
value::TagValueOwned ByteCode::builtinValueBlockAggSum(ArityType arity) {
    // TODO SERVER-83450 add monoblock fast path.
    tassert(11079915, "Unexpected arity value", arity == 3);

    // Move the incoming accumulator state from the stack. We now own and have exclusive access
    // to the accumulator state and can make in-place modifications if desired.
    value::TagValueOwned acc = moveOwnedFromStack(0);

    auto input = viewFromStack(2);
    tassert(8625707,
            "Expected input argument to be of valueBlock type",
            input.tag == value::TypeTags::valueBlock);
    value::ValueBlock* inputBlock = value::bitcastTo<value::ValueBlock*>(input.value);

    auto bitsetView = viewFromStack(1);
    tassert(8625708,
            "Expected bitset argument to be of valueBlock type",
            bitsetView.tag == value::TypeTags::valueBlock);
    value::ValueBlock* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetView.value);

    const value::DeblockedTagVals block = inputBlock->extract();
    const value::DeblockedTagVals bitset = bitsetBlock->extract();

    tassert(
        8151801, "Expected block and bitset to be the same size", block.count() == bitset.count());
    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");

    value::TagValueOwned blockRes = value::TagValueOwned::nothing();
    for (size_t i = 0; i < bitset.count(); ++i) {
        if (value::bitcastTo<bool>(bitset[i].value) && value::isNumber(block.tags()[i])) {
            // If 'blockRes' is Nothing, set 'blockRes' equal to 'block[i]'. Otherwise, compute the
            // sum of 'blockRes' and 'block[i]' and store the result back into 'blockRes'.
            if (blockRes.tag() == value::TypeTags::Nothing) {
                blockRes = value::TagValueOwned::fromRaw(
                    value::copyValue(block.tags()[i], block.vals()[i]));
            } else {
                blockRes = value::TagValueOwned::fromRaw(
                    genericAdd(blockRes.tag(), blockRes.value(), block.tags()[i], block.vals()[i])
                        .releaseToOwnedRaw());
            }
        }
    }

    if (blockRes.tag() == value::TypeTags::Nothing) {
        // Return the accumulator state unmodified.
        return acc;
    }

    if (acc.tag() == value::TypeTags::Nothing) {
        // Return 'blockRes' as the updated accumulator state.
        return blockRes;
    }

    // Compute the result of adding the accumulator state with 'blockRes'.
    auto [resultTag, resultVal] =
        genericAdd(acc.tag(), acc.value(), blockRes.tag(), blockRes.value()).releaseToOwnedRaw();

    // Return 'result' as the updated accumulator state.
    return value::TagValueOwned(resultTag, resultVal);
}  // builtinValueBlockAggSum

value::TagValueOwned ByteCode::builtinValueBlockAggDoubleDoubleSum(ArityType arity) {
    tassert(11079914, "Unexpected arity value", arity == 3);

    // Input: next block to accumulate.
    auto blockView = viewFromStack(2);
    tassert(8695107,
            "Expected input argument to be of valueBlock type",
            blockView.tag == value::TypeTags::valueBlock);
    value::ValueBlock* inputBlock = value::bitcastTo<value::ValueBlock*>(blockView.value);

    // Input: bitset matching 'inputBlock'.
    auto bitsetView = viewFromStack(1);
    tassert(8695108,
            "Expected bitset argument to be of valueBlock type",
            bitsetView.tag == value::TypeTags::valueBlock);
    value::ValueBlock* inputBitset = value::bitcastTo<value::ValueBlock*>(bitsetView.value);

    // Input-output: running accumulator result. moveOwnedFromStack() takes ownership so our local
    // copy can do in-place updates to it.
    auto accTagValue = moveOwnedFromStack(0);

    // Initialize the accumulator if this is the first use of it.
    if (accTagValue.tag() == value::TypeTags::Nothing) {
        accTagValue = value::TagValueOwned::fromRaw(genericInitializeDoubleDoubleSumState());
    }

    tassert(8695109,
            "The result slot must be Array-typed",
            accTagValue.tag() == value::TypeTags::Array);
    value::Array* accumulator = value::getArrayView(accTagValue.value());

    const value::DeblockedTagVals block = inputBlock->extract();
    const value::DeblockedTagVals bitset = inputBitset->extract();
    tassert(
        8695110, "Expected block and bitset to be the same size", block.count() == bitset.count());

    for (size_t i = 0; i < block.count(); ++i) {
        if (value::bitcastTo<bool>(bitset[i].value)) {
            aggDoubleDoubleSumImpl(accumulator, block.tags()[i], block.vals()[i]);
        }
    }

    return accTagValue;
}  // builtinValueBlockAggDoubleDoubleSum

namespace {

template <typename Less, typename T>
struct HomogeneousSortPattern {
    HomogeneousSortPattern(bool isAscending) {
        sign = isAscending ? 1 : -1;
    }

    bool operator()(value::Value lhs, value::Value rhs) {
        int cmp = 0;
        if constexpr (std::is_same_v<T, double>) {
            cmp = compareDoubles(value::bitcastTo<T>(lhs), value::bitcastTo<T>(rhs));
        } else {
            cmp = value::compareHelper(value::bitcastTo<T>(lhs), value::bitcastTo<T>(rhs));
        }
        if constexpr (std::is_same_v<Less, SortPatternLess>) {
            // Less
            return cmp * sign < 0;
        } else {
            // Greater
            return cmp * sign > 0;
        }
    }

private:
    int sign;
};

template <typename Less, typename T>
size_t homogeneousTopBottomHelper(bool isAscending,
                                  const std::span<const value::Value>& bitsetVals,
                                  const std::span<const value::Value>& keyVals) {
    size_t firstPresent = [&bitsetVals] {
        for (size_t i = 0; i < bitsetVals.size(); ++i) {
            if (value::bitcastTo<bool>(bitsetVals[i])) {
                return i;
            }
        }
        return bitsetVals.size();
    }();
    if (firstPresent == bitsetVals.size()) {
        // All values in the bitset were false, so we don't need to update the state.
        return bitsetVals.size();
    }
    auto keyLess = HomogeneousSortPattern<Less, T>(isAscending);
    size_t bestIdx = firstPresent;
    for (size_t i = firstPresent; i < keyVals.size(); ++i) {
        if (value::bitcastTo<bool>(bitsetVals[i]) && keyLess(keyVals[i], keyVals[bestIdx])) {
            bestIdx = i;
        }
    }

    return bestIdx;
}

int memAdded(value::TagValueView key, value::TagValueView output) {
    return value::getApproximateSize(key.tag, key.value) +
        value::getApproximateSize(output.tag, output.value);
}

template <typename Comp>
int addNewPair(value::Array* mergeArr,
               value::TypeTags keyTag,
               value::Value keyVal,
               value::TypeTags outTag,
               value::Value outVal,
               const PairKeyComp<Comp>& keyLess) {
    int memDelta = memAdded({keyTag, keyVal}, {outTag, outVal});

    value::TagValueOwned pairArr{value::makeNewArray()};
    auto* pairArrView = value::getArrayView(pairArr.value());
    pairArrView->reserve(2);

    // Update the sortKey with a copy.
    pairArrView->push_back_raw(value::copyValue(keyTag, keyVal));

    // Add a copy of the output value to the SBE pair array.
    pairArrView->push_back_raw(value::copyValue(outTag, outVal));

    mergeArr->push_back(std::move(pairArr));

    auto& mergeHeap = mergeArr->values();
    std::push_heap(mergeHeap.begin(), mergeHeap.end(), keyLess);

    return memDelta;
}

template <typename Comp>
int updateWorstPair(value::Array* mergeArr,
                    value::Array* worst,
                    value::TypeTags newKeyTag,
                    value::Value newKeyVal,
                    value::TypeTags newOutTag,
                    value::Value newOutVal,
                    const PairKeyComp<Comp>& keyLess) {
    auto& mergeHeap = mergeArr->values();
    std::pop_heap(mergeHeap.begin(), mergeHeap.end(), keyLess);

    int memDelta = -memAdded(worst->getAt(0), worst->getAt(1)) +
        memAdded({newKeyTag, newKeyVal}, {newOutTag, newOutVal});

    // Update the sort key. It is owned by the input sort key block so we will need to make a copy.
    auto [newKeyCpyTag, newKeyCpyVal] = value::copyValue(newKeyTag, newKeyVal);
    // The sort key from the merge heap is owned by the heap so we can safely use setAt which will
    // release the value being replaced.
    worst->setAt(0, newKeyCpyTag, newKeyCpyVal);

    // Update the output value. We will need to make a copy since the value is owned by the input
    // block of output vals.
    auto [newOutCpyTag, newOutCpyVal] = value::copyValue(newOutTag, newOutVal);
    // The current output val is owned by the merge heap, so we can safely use setAt which will
    // release the value being replaced.
    worst->setAt(1, newOutCpyTag, newOutCpyVal);

    std::push_heap(mergeHeap.begin(), mergeHeap.end(), keyLess);

    return memDelta;
}

std::pair<value::Array*, value::TagValueView> getWorst(value::Array* mergeArr) {
    auto worstTagVal = mergeArr->getAt(0);
    auto worstArr = value::getArrayView(worstTagVal.value);
    return {worstArr, worstArr->getAt(0)};
}

template <typename Less>
bool tryFullMergeArrFastPath(TopBottomSense sense,
                             bool isAscending,
                             value::Array* mergeArr,
                             size_t maxSize,
                             value::ValueBlock* sortKeyBlock,
                             const Less& less) {
    if (mergeArr->size() == maxSize) {

        value::TagValueView bestVal{value::TypeTags::Nothing, 0u};
        // topN with descending sort and bottomN with ascending sort return the same values for
        // scalar keys. This is not true for array keys, but we also check that bestTag is not an
        // array or object before using this fast path. The "best" possible key in the block cannot
        // be better than the upper bound.
        if ((sense == TopBottomSense::kTop && !isAscending) ||
            (sense == TopBottomSense::kBottom && isAscending)) {
            bestVal = sortKeyBlock->tryUpperBound();
        }
        // topN with ascending sort and bottomN with descending sort return the same values subject
        // to the same constraints described above. The "best" possible key in the block cannot be
        // better than the lower bound.
        else {
            bestVal = sortKeyBlock->tryLowerBound();
        }
        if (bestVal.tag != value::TypeTags::Nothing && !isArray(bestVal.tag) &&
            !isObject(bestVal.tag)) {
            auto [_, worstKey] = getWorst(mergeArr);

            if (!less(bestVal, worstKey)) {
                // Nothing in this block can beat the worst element in the accumulated heap, so
                // return the input state unmodified.
                return true;
            }
        }
    }
    return false;
}

template <typename Less>
bool tryArgMinMaxFastPath(TopBottomSense sense,
                          bool isAscending,
                          const ByteCode::MultiAccState& stateTuple,
                          value::ValueBlock* bitsetBlock,
                          value::ValueBlock* sortKeyBlock,
                          value::ValueBlock* valBlock,
                          const Less& less) {
    auto [state, mergeArr, startIdx, maxSize, memUsage, memLimit, isGroupAccum] = stateTuple;
    if (maxSize == 1 && bitsetBlock->allTrue().get_value_or(false)) {
        boost::optional<size_t> bestIdx;
        // topN with descending sort and bottomN with ascending sort return the same values. The
        // "best" possible key in the block cannot be better than the lower bound.
        if ((sense == TopBottomSense::kTop && !isAscending) ||
            (sense == TopBottomSense::kBottom && isAscending)) {
            bestIdx = sortKeyBlock->argMax();
        }
        // topN with ascending sort and bottomN with descending sort return the same values. The
        // "best" possible key in the block cannot be better than the upper bound.
        else {
            bestIdx = sortKeyBlock->argMin();
        }
        if (bestIdx) {
            size_t sortKeyCount = sortKeyBlock->count();
            tassert(
                8776401, "argMin/Max must be <= the size of the block", *bestIdx <= sortKeyCount);
            if (*bestIdx == sortKeyCount) {
                // Block was all Nothings, return the state unchanged.
                return true;
            }
            auto [bestTag, bestVal] = sortKeyBlock->at(*bestIdx);
            auto keyLess = PairKeyComp(less);
            if (bestTag != value::TypeTags::Nothing && !isArray(bestTag) && !isObject(bestTag)) {
                if (mergeArr->size() < maxSize) {
                    auto [bestOutTag, bestOutVal] = valBlock->at(*bestIdx);

                    int memDelta =
                        addNewPair(mergeArr, bestTag, bestVal, bestOutTag, bestOutVal, keyLess);
                    memUsage = updateAndCheckMemUsage(state, memUsage, memDelta, memLimit);

                    return true;
                } else {
                    tassert(8776402,
                            "Heap should contain same number of elements as maxSize",
                            mergeArr->size() == maxSize);

                    auto [worstArr, worstKey] = getWorst(mergeArr);

                    if (less({bestTag, bestVal}, worstKey)) {
                        auto [bestOutTag, bestOutVal] = valBlock->at(*bestIdx);

                        int memDelta = updateWorstPair(
                            mergeArr, worstArr, bestTag, bestVal, bestOutTag, bestOutVal, keyLess);
                        memUsage = updateAndCheckMemUsage(state, memUsage, memDelta, memLimit);
                    }

                    return true;
                }
            }
        }
    }

    return false;
}

// The intermediate heap will always store a tag, val pair and a value representing an array
// index, so we can use this struct instead of creating an SBE value from the index.
struct TopBottomSortKeyAndIdx {
    std::pair<value::TypeTags, value::Value> sortKey;
    size_t outIdx = 0;
};

// Comparison based on the key of a TopBottomSortKeyAndIdx.
template <typename Comp>
struct TopBottomSortKeyAndIdxComp {
    TopBottomSortKeyAndIdxComp(const Comp& comp) : _comp(comp) {}

    bool operator()(const TopBottomSortKeyAndIdx& lhs, const TopBottomSortKeyAndIdx& rhs) const {
        return _comp(lhs.sortKey, rhs.sortKey);
    }

private:
    const Comp _comp;
};

template <typename Less>
void combineBlockNativeAggTopBottomN(const ByteCode::MultiAccState& stateTuple,
                                     std::vector<TopBottomSortKeyAndIdx> newArr,
                                     value::ValueBlock* valBlock,
                                     Less less) {
    auto [state, mergeArr, startIdx, maxSize, memUsage, memLimit, isGroupAccum] = stateTuple;

    invariant(mergeArr->size() <= maxSize);

    boost::optional<value::DeblockedTagVals> deblocked;
    auto keyLess = PairKeyComp(less);

    // Once a value is inserted into mergeArr, it must be owned by mergeArr.
    for (const auto& newPair : newArr) {
        // This is an unowned view on the sort key, so it will need to be copied if it makes it into
        // the merge heap.
        auto [newSortKeyTag, newSortKeyVal] = newPair.sortKey;

        // Get the index of the corresponding output value.
        size_t outIdx = newPair.outIdx;

        if (mergeArr->size() < maxSize) {
            // Extract if we haven't done so yet.
            if (!deblocked) {
                deblocked = valBlock->extract();
            }
            invariant(outIdx < deblocked->count());

            int memDelta = addNewPair(mergeArr,
                                      newSortKeyTag,
                                      newSortKeyVal,
                                      deblocked->tags()[outIdx],
                                      deblocked->vals()[outIdx],
                                      keyLess);
            memUsage = updateAndCheckMemUsage(state, memUsage, memDelta, memLimit);
        } else {
            tassert(8794901,
                    "Heap should contain same number of elements as maxSize",
                    mergeArr->size() == maxSize);

            auto [worstArr, worstKey] = getWorst(mergeArr);

            if (less({newSortKeyTag, newSortKeyVal}, worstKey)) {
                // Extract if we haven't done so yet.
                if (!deblocked) {
                    deblocked = valBlock->extract();
                }
                invariant(outIdx < deblocked->count());

                int memDelta = updateWorstPair(mergeArr,
                                               worstArr,
                                               newSortKeyTag,
                                               newSortKeyVal,
                                               deblocked->tags()[outIdx],
                                               deblocked->vals()[outIdx],
                                               keyLess);
                memUsage = updateAndCheckMemUsage(state, memUsage, memDelta, memLimit);
            }
        }
    }
}

template <typename Less>
bool tryHomogeneousFastPath(TopBottomSense sense,
                            bool isAscending,
                            value::TypeTags stateTag,
                            value::Value stateVal,
                            const ByteCode::MultiAccState& stateTuple,
                            const std::span<const value::Value>& bitsetVals,
                            const value::DeblockedTagVals& sortKeys,
                            value::ValueBlock* valBlock,
                            const Less& less) {
    auto [state, mergeArr, startIdx, maxSize, memUsage, memLimit, isGroupAccum] = stateTuple;

    if (maxSize == 1 && sortKeys.isDense() && value::validHomogeneousType(sortKeys.tag()) &&
        sortKeys.tag() != value::TypeTags::Boolean && sortKeys.count() > 0) {

        // We will use a std::vector of TopBottomSortKeyAndIdx structs instead of a nested SBE array
        // for the intermediate heap representation to capture the semantics that these containers
        // are views on values that they do not own.
        std::vector<TopBottomSortKeyAndIdx> newArr;
        // The heap cannot be bigger than the min of the number of inputs and n/maxSize.
        newArr.reserve(std::min(sortKeys.count(), maxSize));

        auto tag = sortKeys.tag();
        auto sortKeyVals = sortKeys.valsSpan();
        size_t bestIdx = 0;
        switch (tag) {
            case value::TypeTags::NumberInt32:
                bestIdx =
                    homogeneousTopBottomHelper<Less, int32_t>(isAscending, bitsetVals, sortKeyVals);
                break;
            case value::TypeTags::NumberInt64:
            case value::TypeTags::Date:
                bestIdx =
                    homogeneousTopBottomHelper<Less, int64_t>(isAscending, bitsetVals, sortKeyVals);
                break;
            case value::TypeTags::NumberDouble:
                bestIdx =
                    homogeneousTopBottomHelper<Less, double>(isAscending, bitsetVals, sortKeyVals);
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(11122936);
        }

        if (bestIdx == bitsetVals.size()) {
            // All values in the bitset were false, so return the state unchanged.
            return true;
        }

        // Now that we have the "best" index, update the state and return it.

        // The sort key "array" for a NumberInt32, NumberInt64, Date, or NumberDouble when there is
        // only one sort field will just be the value itself with no actual array.
        newArr.push_back({std::pair{tag, sortKeyVals[bestIdx]} /* sortKey */, bestIdx});

        // Update mergeArr in-place.
        combineBlockNativeAggTopBottomN(stateTuple, newArr, valBlock, less);

        return true;
    }

    return false;
}
}  // namespace

// Currently, this is a specialized implementation for the single sort key, single output field case
// of $top[N]/$bottom[N].
template <TopBottomSense Sense, bool ValueIsDecomposedArray>
value::TagValueOwned ByteCode::blockNativeAggTopBottomNImpl(value::TagValueOwned state,
                                                            value::ValueBlock* bitsetBlock,
                                                            SortSpec* sortSpec,
                                                            size_t numKeysBlocks,
                                                            size_t numValuesBlocks) {

    static_assert(!ValueIsDecomposedArray);

    using Less =
        std::conditional_t<Sense == TopBottomSense::kTop, SortPatternLess, SortPatternGreater>;

    tassert(11086818, "Expecting number of keys blocks to be 1", numKeysBlocks == 1);
    tassert(11086814, "Expecting number of values blocks to be 1", numValuesBlocks == 1);

    // We already read numKeysBlocks (stack position 3) in builtinValueBlockAggTopBottomNImpl before
    // calling into this function.

    constexpr size_t keysBlocksStartOffset = 4;
    const size_t valuesBlocksStartOffset = keysBlocksStartOffset + numKeysBlocks;

    auto sortKeyBlockView = viewFromStack(keysBlocksStartOffset);
    tassert(8794906,
            "Expected key argument to be of valueBlock type",
            sortKeyBlockView.tag == value::TypeTags::valueBlock);
    auto* sortKeyBlock = value::getValueBlock(sortKeyBlockView.value);
    size_t sortKeyCount = sortKeyBlock->count();

    auto valBlockView = viewFromStack(valuesBlocksStartOffset);
    tassert(8794905,
            "Expected key argument to be of valueBlock type",
            valBlockView.tag == value::TypeTags::valueBlock);
    auto* valBlock = value::getValueBlock(valBlockView.value);

    MultiAccState stateTuple = getMultiAccState(state.tag(), state.value());
    auto [stateArray, mergeArr, startIdx, maxSize, memUsage, memLimit, isGroupAccum] = stateTuple;
    tassert(11093700, "maxSize must be greater than zero", maxSize > 0);

    tassert(11093701,
            fmt::format("Number of sort keys ({}) in block doesn't match the number of values in "
                        "valBlock ({})",
                        sortKeyCount,
                        valBlock->count()),
            sortKeyCount == valBlock->count());
    tassert(11093702,
            fmt::format("Number of sort keys ({}) in block doesn't match the number of values in "
                        "bitsetBlock ({})",
                        sortKeyCount,
                        bitsetBlock->count()),
            sortKeyCount == bitsetBlock->count());

    const auto sortPattern = sortSpec->getSortPattern();
    bool isAscending = sortPattern.front().isAscending;

    auto less = Less(sortSpec);

    // See if we can skip processing the entire block based on the currently accumulated heap and
    // block metadata of the current block. We don't need to check the bitset because if the best
    // possible value wouldn't make it into the accumulated heap, it doesn't matter if this value
    // passed any filters.
    if (tryFullMergeArrFastPath(Sense, isAscending, mergeArr, maxSize, sortKeyBlock, less)) {
        return state;
    }

    // Try to use the argMin/Max API if possible instead of calling extract on the sortKeyBlock.
    if (tryArgMinMaxFastPath(
            Sense, isAscending, stateTuple, bitsetBlock, sortKeyBlock, valBlock, less)) {
        return state;
    }

    auto bitset = bitsetBlock->extract();
    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");
    auto bitsetVals = bitset.valsSpan();

    auto sortKeys = sortKeyBlock->extract();

    // Fast path for $top/$bottom with homogeneous sort field input and no missing values.
    if (tryHomogeneousFastPath(Sense,
                               isAscending,
                               state.tag(),
                               state.value(),
                               stateTuple,
                               bitsetVals,
                               sortKeys,
                               valBlock,
                               less)) {
        return state;
    }

    // We will use a std::vector of TopBottomSortKeyAndIdx structs instead of a nested SBE array for
    // the intermediate heap representation to capture the semantics that these containers are views
    // on values that they do not own.
    std::vector<TopBottomSortKeyAndIdx> newArr;
    // The heap cannot be bigger than the min of the number of inputs and n/maxSize.
    newArr.reserve(std::min(sortKeyCount, maxSize));

    auto [sortKeyTags, sortKeyVals] = sortKeys.tagsValsView();
    auto keyLess = TopBottomSortKeyAndIdxComp(less);

    for (size_t i = 0; i < sortKeyVals.size(); ++i) {
        if (!value::bitcastTo<bool>(bitsetVals[i])) {
            continue;
        }
        if (newArr.size() < maxSize) {
            // We will copy the sortKey if it ever makes it into the merge heap in the combine
            // phase.
            TopBottomSortKeyAndIdx newPair{std::pair{sortKeyTags[i], sortKeyVals[i]} /* sortKey */,
                                           i /* outIdx */};

            newArr.push_back(newPair);
            std::push_heap(newArr.begin(), newArr.end(), keyLess);
        } else {
            tassert(8794902,
                    "Heap should contain same number of elements as maxSize",
                    newArr.size() == maxSize);

            if (less(std::pair{sortKeyTags[i], sortKeyVals[i]}, newArr.front().sortKey)) {
                std::pop_heap(newArr.begin(), newArr.end(), keyLess);

                auto& worstPair = newArr.back();

                // We will copy the sort key if it ever makes it into the final heap in the combine
                // phase.
                worstPair.sortKey = std::pair{sortKeyTags[i], sortKeyVals[i]};
                worstPair.outIdx = i;

                std::push_heap(newArr.begin(), newArr.end(), keyLess);
            }
        }
    }

    // Update mergeArr in-place.
    combineBlockNativeAggTopBottomN(stateTuple, newArr, valBlock, less);

    // Return the input state since mergeArr was updated in-place.
    return state;
}

class ByteCode::TopBottomArgsFromBlocks final : public ByteCode::TopBottomArgs {
public:
    TopBottomArgsFromBlocks(TopBottomSense sense,
                            SortSpec* sortSpec,
                            bool decomposedKey,
                            bool decomposedValue,
                            std::vector<value::DeblockedTagVals> keys,
                            std::vector<value::DeblockedTagVals> values)
        : TopBottomArgs(sense, sortSpec, decomposedKey, decomposedValue),
          _keys(std::move(keys)),
          _values(std::move(values)) {}

    ~TopBottomArgsFromBlocks() final = default;

    bool keySortsBeforeImpl(value::TagValueView item) final {
        tassert(8448705, "Expected item to be an Array", item.tag == value::TypeTags::Array);

        const SortPattern& sortPattern = _sortSpec->getSortPattern();
        tassert(8448706,
                "Expected numKeys to be equal to number of sort pattern parts",
                sortPattern.size() == _keys.size());

        auto itemArray = value::getArrayView(item.value);
        tassert(8448707,
                "Expected size of item array to be equal to number of sort pattern parts",
                sortPattern.size() == itemArray->size());

        if (_sense == TopBottomSense::kTop) {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                auto [keyTag, keyVal] = _keys[i][_blockIndex];
                auto itemTagVal = itemArray->getAt(i);
                int32_t cmp =
                    compare<TopBottomSense::kTop>(keyTag, keyVal, itemTagVal.tag, itemTagVal.value);

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        } else {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                auto [keyTag, keyVal] = _keys[i][_blockIndex];
                auto itemTagVal = itemArray->getAt(i);
                int32_t cmp = compare<TopBottomSense::kBottom>(
                    keyTag, keyVal, itemTagVal.tag, itemTagVal.value);

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        }

        return false;
    }

    value::TagValueOwned getOwnedKeyImpl() final {
        auto keys = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto keysArr = value::getArrayView(keys.value());

        for (size_t i = 0; i < _keys.size(); ++i) {
            auto [keyTag, keyVal] = _keys[i][_blockIndex];
            std::tie(keyTag, keyVal) = value::copyValue(keyTag, keyVal);
            keysArr->push_back_raw(keyTag, keyVal);
        }

        return keys;
    }

    value::TagValueOwned getOwnedValueImpl() final {
        auto values = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto valuesArr = value::getArrayView(values.value());

        for (size_t i = 0; i < _values.size(); ++i) {
            auto [valueTag, valueVal] = _values[i][_blockIndex];
            std::tie(valueTag, valueVal) = value::copyValue(valueTag, valueVal);
            valuesArr->push_back_raw(valueTag, valueVal);
        }

        return values;
    }

    void initForBlockIndex(size_t blockIdx) {
        _blockIndex = blockIdx;

        if (!_decomposedKey) {
            setDirectKeyArg({false, _keys[0].tags()[blockIdx], _keys[0].vals()[blockIdx]});
        }
        if (!_decomposedValue) {
            setDirectValueArg({false, _values[0].tags()[blockIdx], _values[0].vals()[blockIdx]});
        }
    }

    std::vector<value::DeblockedTagVals> _keys;
    std::vector<value::DeblockedTagVals> _values;
    size_t _blockIndex = 0;
};

template <TopBottomSense Sense, bool ValueIsDecomposedArray>
value::TagValueOwned ByteCode::builtinValueBlockAggTopBottomNImpl(ArityType arity) {

    auto bitsetView = viewFromStack(1);
    tassert(8448708,
            "Expected bitset argument to be of valueBlock type",
            bitsetView.tag == value::TypeTags::valueBlock);

    auto sortSpec = viewFromStack(2);
    tassert(
        8448709, "Argument must be of sortSpec type", sortSpec.tag == value::TypeTags::sortSpec);

    size_t numKeysBlocks = 1;
    bool keyIsDecomposed = false;
    auto numKeysBlocksView = viewFromStack(3);
    if (numKeysBlocksView.tag == value::TypeTags::NumberInt32) {
        numKeysBlocks = static_cast<size_t>(value::bitcastTo<int32_t>(numKeysBlocksView.value));
        keyIsDecomposed = true;
    } else {
        tassert(8448710,
                "Expected numKeys to be Null or Int32",
                numKeysBlocksView.tag == value::TypeTags::Null);
    }

    constexpr size_t keysBlocksStartOffset = 4;
    const size_t valuesBlocksStartOffset = keysBlocksStartOffset + numKeysBlocks;
    const size_t numValuesBlocks = ValueIsDecomposedArray ? arity - valuesBlocksStartOffset : 1;

    auto state = moveOwnedFromStack(0);

    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetView.value);
    auto ss = value::getSortSpecView(sortSpec.value);

    if constexpr (!ValueIsDecomposedArray) {
        if (!keyIsDecomposed) {
            return blockNativeAggTopBottomNImpl<Sense, ValueIsDecomposedArray>(
                std::move(state), bitsetBlock, ss, numKeysBlocks, numValuesBlocks);
        }
    }

    auto [stateArray, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(state.tag(), state.value());
    tassert(11093703, "maxSize must be greater than zero", maxSize > 0);

    value::DeblockedTagVals bitset = bitsetBlock->extract();

    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");

    std::vector<value::DeblockedTagVals> keys;
    std::vector<value::DeblockedTagVals> values;
    keys.reserve(numKeysBlocks);
    values.reserve(numValuesBlocks);

    for (size_t i = 0; i < numKeysBlocks; ++i) {
        auto keysBlock = viewFromStack(keysBlocksStartOffset + i);
        tassert(8448712,
                "Expected argument to be of valueBlock type",
                keysBlock.tag == value::TypeTags::valueBlock);

        keys.emplace_back(value::getValueBlock(keysBlock.value)->extract());

        tassert(8448713,
                "Expected block and bitset to be the same size",
                keys.back().count() == bitset.count());
    }

    for (size_t i = 0; i < numValuesBlocks; ++i) {
        auto valuesBlock = viewFromStack(valuesBlocksStartOffset + i);
        tassert(8448714,
                "Expected argument to be of valueBlock type",
                valuesBlock.tag == value::TypeTags::valueBlock);

        values.emplace_back(value::getValueBlock(valuesBlock.value)->extract());

        tassert(8448715,
                "Expected block and bitset to be the same size",
                values.back().count() == bitset.count());
    }

    TopBottomArgsFromBlocks topBottomArgs{
        Sense, ss, keyIsDecomposed, ValueIsDecomposedArray, std::move(keys), std::move(values)};

    for (size_t blockIndex = 0; blockIndex < bitset.count(); ++blockIndex) {
        if (value::bitcastTo<bool>(bitset[blockIndex].value)) {
            topBottomArgs.initForBlockIndex(blockIndex);

            if constexpr (Sense == TopBottomSense::kTop) {
                memUsage =
                    aggTopNAdd(stateArray, array, maxSize, memUsage, memLimit, topBottomArgs);
            } else {
                memUsage =
                    aggBottomNAdd(stateArray, array, maxSize, memUsage, memLimit, topBottomArgs);
            }
        }
    }

    return state;
}

value::TagValueOwned ByteCode::builtinValueBlockAggTopN(ArityType arity) {
    return builtinValueBlockAggTopBottomNImpl<TopBottomSense::kTop, false>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockAggBottomN(ArityType arity) {
    return builtinValueBlockAggTopBottomNImpl<TopBottomSense::kBottom, false>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockAggTopNArray(ArityType arity) {
    return builtinValueBlockAggTopBottomNImpl<TopBottomSense::kTop, true>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockAggBottomNArray(ArityType arity) {
    return builtinValueBlockAggTopBottomNImpl<TopBottomSense::kBottom, true>(arity);
}

enum class ArithmeticOp { Addition, Subtraction, Multiplication, Division };

template <int op>
value::TagValueOwned ByteCode::builtinBlockBlockArithmeticOperation(
    const value::TypeTags* bitsetTags,
    const value::Value* bitsetVals,
    value::ValueBlock* leftInputBlock,
    value::ValueBlock* rightInputBlock,
    size_t valsNum) {
    auto leftBlock = leftInputBlock->extract();
    auto rightBlock = rightInputBlock->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if (bitsetTags[i] != value::TypeTags::Boolean || !value::bitcastTo<bool>(bitsetVals[i])) {
            continue;
        }
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [resTag, resVal] =
                genericAdd(
                    leftBlock[i].tag, leftBlock[i].value, rightBlock[i].tag, rightBlock[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [resTag, resVal] =
                genericSub(
                    leftBlock[i].tag, leftBlock[i].value, rightBlock[i].tag, rightBlock[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [resTag, resVal] =
                genericMul(
                    leftBlock[i].tag, leftBlock[i].value, rightBlock[i].tag, rightBlock[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [resTag, resVal] = genericDiv(leftBlock[i], rightBlock[i]).releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(resBlock.release()));
}

template <int op>
value::TagValueOwned ByteCode::builtinBlockBlockArithmeticOperation(
    value::ValueBlock* leftInputBlock, value::ValueBlock* rightInputBlock, size_t valsNum) {
    auto leftBlock = leftInputBlock->extract();
    auto rightBlock = rightInputBlock->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [resTag, resVal] =
                genericAdd(
                    leftBlock[i].tag, leftBlock[i].value, rightBlock[i].tag, rightBlock[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [resTag, resVal] =
                genericSub(
                    leftBlock[i].tag, leftBlock[i].value, rightBlock[i].tag, rightBlock[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [resTag, resVal] =
                genericMul(
                    leftBlock[i].tag, leftBlock[i].value, rightBlock[i].tag, rightBlock[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [resTag, resVal] = genericDiv(leftBlock[i], rightBlock[i]).releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(resBlock.release()));
}

template <int op>
value::TagValueOwned ByteCode::builtinScalarBlockArithmeticOperation(
    const value::TypeTags* bitsetTags,
    const value::Value* bitsetVals,
    value::TagValueView scalar,
    value::ValueBlock* block,
    size_t valsNum) {
    auto extractedValues = block->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if (bitsetTags[i] != value::TypeTags::Boolean || !value::bitcastTo<bool>(bitsetVals[i])) {
            continue;
        }
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [resTag, resVal] =
                genericAdd(
                    scalar.tag, scalar.value, extractedValues[i].tag, extractedValues[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [resTag, resVal] =
                genericSub(
                    scalar.tag, scalar.value, extractedValues[i].tag, extractedValues[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [resTag, resVal] =
                genericMul(
                    scalar.tag, scalar.value, extractedValues[i].tag, extractedValues[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [resTag, resVal] = genericDiv(scalar, extractedValues[i]).releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(resBlock.release()));
}

template <int op>
value::TagValueOwned ByteCode::builtinScalarBlockArithmeticOperation(value::TagValueView scalar,
                                                                     value::ValueBlock* block,
                                                                     size_t valsNum) {
    auto extractedValues = block->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [resTag, resVal] =
                genericAdd(
                    scalar.tag, scalar.value, extractedValues[i].tag, extractedValues[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [resTag, resVal] =
                genericSub(
                    scalar.tag, scalar.value, extractedValues[i].tag, extractedValues[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [resTag, resVal] =
                genericMul(
                    scalar.tag, scalar.value, extractedValues[i].tag, extractedValues[i].value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [resTag, resVal] = genericDiv(scalar, extractedValues[i]).releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(resBlock.release()));
}

template <int op>
value::TagValueOwned ByteCode::builtinBlockScalarArithmeticOperation(
    const value::TypeTags* bitsetTags,
    const value::Value* bitsetVals,
    value::ValueBlock* block,
    value::TagValueView scalar,
    size_t valsNum) {
    auto extractedValues = block->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if (bitsetTags[i] != value::TypeTags::Boolean || !value::bitcastTo<bool>(bitsetVals[i])) {
            continue;
        }
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [resTag, resVal] =
                genericAdd(
                    extractedValues[i].tag, extractedValues[i].value, scalar.tag, scalar.value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [resTag, resVal] =
                genericSub(
                    extractedValues[i].tag, extractedValues[i].value, scalar.tag, scalar.value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [resTag, resVal] =
                genericMul(
                    extractedValues[i].tag, extractedValues[i].value, scalar.tag, scalar.value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [resTag, resVal] = genericDiv(extractedValues[i], scalar).releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(resBlock.release()));
}

template <int op>
value::TagValueOwned ByteCode::builtinBlockScalarArithmeticOperation(value::ValueBlock* block,
                                                                     value::TagValueView scalar,
                                                                     size_t valsNum) {
    auto extractedValues = block->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [resTag, resVal] =
                genericAdd(
                    extractedValues[i].tag, extractedValues[i].value, scalar.tag, scalar.value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [resTag, resVal] =
                genericSub(
                    extractedValues[i].tag, extractedValues[i].value, scalar.tag, scalar.value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [resTag, resVal] =
                genericMul(
                    extractedValues[i].tag, extractedValues[i].value, scalar.tag, scalar.value)
                    .releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [resTag, resVal] = genericDiv(extractedValues[i], scalar).releaseToOwnedRaw();
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(resBlock.release()));
}

template <int op>
value::TagValueOwned ByteCode::builtinScalarScalarArithmeticOperation(
    value::TagValueView leftInputScalar, value::TagValueView rightInputScalar, size_t valsNum) {
    std::unique_ptr<value::MonoBlock> resBlock;
    if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
        auto [resultTag, resultValue] = genericAdd(leftInputScalar.tag,
                                                   leftInputScalar.value,
                                                   rightInputScalar.tag,
                                                   rightInputScalar.value)
                                            .releaseToOwnedRaw();
        resBlock = std::make_unique<value::MonoBlock>(valsNum, resultTag, resultValue);
    } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
        auto [resultTag, resultValue] = genericSub(leftInputScalar.tag,
                                                   leftInputScalar.value,
                                                   rightInputScalar.tag,
                                                   rightInputScalar.value)
                                            .releaseToOwnedRaw();
        resBlock = std::make_unique<value::MonoBlock>(valsNum, resultTag, resultValue);
    } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
        auto [resultTag, resultValue] = genericMul(leftInputScalar.tag,
                                                   leftInputScalar.value,
                                                   rightInputScalar.tag,
                                                   rightInputScalar.value)
                                            .releaseToOwnedRaw();
        resBlock = std::make_unique<value::MonoBlock>(valsNum, resultTag, resultValue);
    } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
        auto [resultTag, resultValue] =
            genericDiv(leftInputScalar, rightInputScalar).releaseToOwnedRaw();
        resBlock = std::make_unique<value::MonoBlock>(valsNum, resultTag, resultValue);
    } else {
        resBlock = std::make_unique<value::MonoBlock>(valsNum, value::TypeTags::Nothing, 0);
    }

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(resBlock.release()));
}

template <int op>
value::TagValueOwned ByteCode::builtinValueBlockArithmeticOperation(ArityType arity) {

    static_assert(op >= 0 && op <= 3, "op should be between 0 and 3 inclusive");

    tassert(11079913, "Unexpected arity value", arity == 3);

    auto bitsetView = viewFromStack(0);
    auto l = viewFromStack(1);
    auto r = viewFromStack(2);

    tassert(8332300,
            "First argument of block arithmetic operation must be block of values representing a "
            "bitmask or Nothing",
            bitsetView.tag == value::TypeTags::valueBlock ||
                bitsetView.tag == value::TypeTags::Nothing);
    const value::Value* bitsetVals = nullptr;
    const value::TypeTags* bitsetTags = nullptr;
    size_t valsNum = 0;
    if (bitsetView.tag == value::TypeTags::valueBlock) {
        auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetView.value);
        auto bitset = bitsetBlock->extract();
        bitsetVals = bitset.vals();
        bitsetTags = bitset.tags();
        valsNum = bitset.count();
    }

    tassert(8332302,
            "At least one of the second and third arguments of block arithmetic operation must be "
            "block of values",
            l.tag == value::TypeTags::valueBlock || r.tag == value::TypeTags::valueBlock);

    if (l.tag == value::TypeTags::valueBlock && r.tag == value::TypeTags::valueBlock) {
        // Block - Block

        auto* leftInputBlock = value::bitcastTo<value::ValueBlock*>(l.value);
        auto* rightInputBlock = value::bitcastTo<value::ValueBlock*>(r.value);

        auto leftMonoBlock = leftInputBlock->as<value::MonoBlock>();
        auto rightMonoBlock = rightInputBlock->as<value::MonoBlock>();

        auto leftValsNum = leftInputBlock->count();
        auto rightValsNum = rightInputBlock->count();
        tassert(8332303,
                str::stream() << "Expected blocks to be the same size but they are leftBlock =  "
                              << leftValsNum << " rightBlock = " << rightValsNum,
                leftValsNum == rightValsNum);

        if (bitsetVals) {
            tassert(8332304,
                    str::stream() << "Expected value blocks and bitset block to be the same size "
                                     "but they are value block =  "
                                  << leftValsNum << " bitset block = " << valsNum,
                    leftValsNum == valsNum);
        } else {
            valsNum = leftValsNum;
        }

        if (!leftMonoBlock && !rightMonoBlock) {
            if (bitsetVals) {
                return builtinBlockBlockArithmeticOperation<op>(
                    bitsetTags, bitsetVals, leftInputBlock, rightInputBlock, valsNum);
            }
            return builtinBlockBlockArithmeticOperation<op>(
                leftInputBlock, rightInputBlock, valsNum);
        } else if (leftMonoBlock && !rightMonoBlock) {
            if (bitsetVals) {
                return builtinScalarBlockArithmeticOperation<op>(
                    bitsetTags,
                    bitsetVals,
                    {leftMonoBlock->getTag(), leftMonoBlock->getValue()},
                    rightInputBlock,
                    valsNum);
            }
            return builtinScalarBlockArithmeticOperation<op>(
                {leftMonoBlock->getTag(), leftMonoBlock->getValue()}, rightInputBlock, valsNum);
        } else if (!leftMonoBlock && rightMonoBlock) {
            if (bitsetVals) {
                return builtinBlockScalarArithmeticOperation<op>(
                    bitsetTags,
                    bitsetVals,
                    leftInputBlock,
                    {rightMonoBlock->getTag(), rightMonoBlock->getValue()},
                    valsNum);
            }
            return builtinBlockScalarArithmeticOperation<op>(
                leftInputBlock, {rightMonoBlock->getTag(), rightMonoBlock->getValue()}, valsNum);
        } else {
            if (bitsetVals) {
                return builtinBlockBlockArithmeticOperation<op>(
                    bitsetTags, bitsetVals, leftInputBlock, rightInputBlock, valsNum);
            }

            return builtinScalarScalarArithmeticOperation<op>(
                {leftMonoBlock->getTag(), leftMonoBlock->getValue()},
                {rightMonoBlock->getTag(), rightMonoBlock->getValue()},
                valsNum);
        }
    } else if (l.tag != value::TypeTags::valueBlock && r.tag == value::TypeTags::valueBlock) {
        // scalar - block
        auto* rightInputBlock = value::bitcastTo<value::ValueBlock*>(r.value);
        auto rightMonoBlock = rightInputBlock->as<value::MonoBlock>();
        auto rightValsNum = rightInputBlock->count();
        if (valsNum == 0) {
            valsNum = rightValsNum;
        } else {
            tassert(8332305,
                    str::stream() << "Expected value blocks and bitset block to be the same size "
                                     "but they are value block =  "
                                  << rightValsNum << " bitset block = " << valsNum,
                    rightValsNum == valsNum);
        }

        if (bitsetVals) {
            return builtinScalarBlockArithmeticOperation<op>(
                bitsetTags, bitsetVals, l, rightInputBlock, valsNum);
        } else if (rightMonoBlock) {
            return builtinScalarScalarArithmeticOperation<op>(
                l, {rightMonoBlock->getTag(), rightMonoBlock->getValue()}, valsNum);

        } else {
            return builtinScalarBlockArithmeticOperation<op>(l, rightInputBlock, valsNum);
        }

    } else if (l.tag == value::TypeTags::valueBlock && r.tag != value::TypeTags::valueBlock) {
        // block - scalar
        auto* leftInputBlock = value::bitcastTo<value::ValueBlock*>(l.value);
        auto leftMonoBlock = leftInputBlock->as<value::MonoBlock>();
        auto leftValsNum = leftInputBlock->count();
        if (valsNum == 0) {
            valsNum = leftValsNum;
        } else {
            tassert(8332306,
                    str::stream() << "Expected value blocks and bitset block to be the same size "
                                     "but they are value block =  "
                                  << leftValsNum << " bitset block = " << valsNum,
                    leftValsNum == valsNum);
        }

        if (bitsetVals) {
            return builtinBlockScalarArithmeticOperation<op>(
                bitsetTags, bitsetVals, leftInputBlock, r, valsNum);
        } else if (leftMonoBlock) {
            return builtinScalarScalarArithmeticOperation<op>(
                {leftMonoBlock->getTag(), leftMonoBlock->getValue()}, r, valsNum);
        } else {
            return builtinBlockScalarArithmeticOperation<op>(leftInputBlock, r, valsNum);
        }

    } else {
        MONGO_UNREACHABLE_TASSERT(11122937);
    }
}

value::TagValueOwned ByteCode::builtinValueBlockAdd(ArityType arity) {
    return builtinValueBlockArithmeticOperation<static_cast<int>(ArithmeticOp::Addition)>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockSub(ArityType arity) {
    return builtinValueBlockArithmeticOperation<static_cast<int>(ArithmeticOp::Subtraction)>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockMult(ArityType arity) {
    return builtinValueBlockArithmeticOperation<static_cast<int>(ArithmeticOp::Multiplication)>(
        arity);
}

value::TagValueOwned ByteCode::builtinValueBlockDiv(ArityType arity) {
    return builtinValueBlockArithmeticOperation<static_cast<int>(ArithmeticOp::Division)>(arity);
}

value::TagValueMaybeOwned ByteCode::blockRoundTrunc(std::string funcName,
                                                    Decimal128::RoundingMode roundingMode,
                                                    ArityType arity) {
    tassert(11079912, "Unexpected arity value", arity == 1 || arity == 2);
    auto input = viewFromStack(0);
    tassert(8333100,
            "First argument of " + funcName + " must be block of values.",
            input.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(input.value);

    int32_t place = 0;
    if (arity == 2) {
        const auto placeView = viewFromStack(1);
        if (!value::isNumber(placeView.tag)) {
            return value::TagValueMaybeOwned::nothing();
        }
        place = convertNumericToInt32(placeView);
    }

    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> value::TagValueOwned {
            return genericRoundTrunc(funcName, roundingMode, place, tag, val).moveToOwned();
        });

    auto res = valueBlockIn->map(cmpOp);

    return value::TagValueMaybeOwned(
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release()));
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockTrunc(ArityType arity) {
    return blockRoundTrunc("$trunc", Decimal128::kRoundTowardZero, arity);
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockRound(ArityType arity) {
    return blockRoundTrunc("$round", Decimal128::kRoundTiesToEven, arity);
}

namespace {
template <class Cmp, typename T>
void compareNativeCppType(size_t count,
                          const value::Value* inVals,
                          T rhsVal,
                          value::TypeTags* outTags,
                          value::Value* outVals,
                          Cmp op = {}) {
    std::fill_n(outTags, count, value::TypeTags::Boolean);
    for (size_t index = 0; index < count; index++) {
        outVals[index] = value::bitcastFrom<bool>(op(value::bitcastTo<T>(inVals[index]), rhsVal));
    }
}

template <class Cmp, ColumnOpType::Flags AddFlags = ColumnOpType::kNoFlags>
value::TagValueOwned blockCompareGeneric(value::ValueBlock* blockView,
                                         value::TypeTags rhsTag,
                                         value::Value rhsVal) {
    const auto cmpOp = value::makeColumnOp<AddFlags>(
        [&](value::TypeTags tag, value::Value val) {
            return value::genericCompare<Cmp>(tag, val, rhsTag, rhsVal);
        },
        [&](value::TypeTags inTag,
            const value::Value* inVals,
            value::TypeTags* outTags,
            value::Value* outVals,
            size_t count) {
            // We can natively process the comparison of numeric values, if both values are of the
            // same type or if we can make them match by improving the precision of the constant
            // value.
            switch (inTag) {
                case value::TypeTags::NumberDouble: {
                    if (isNumber(rhsTag) &&
                        getWidestNumericalType(inTag, rhsTag) == value::TypeTags::NumberDouble &&
                        rhsTag != value::TypeTags::NumberInt64) {
                        compareNativeCppType<Cmp, double>(
                            count, inVals, numericCast<double>(rhsTag, rhsVal), outTags, outVals);
                        return;
                    }
                } break;
                case value::TypeTags::NumberInt32: {
                    if (isNumber(rhsTag) &&
                        getWidestNumericalType(inTag, rhsTag) == value::TypeTags::NumberInt32) {
                        compareNativeCppType<Cmp, int32_t>(
                            count, inVals, numericCast<int32_t>(rhsTag, rhsVal), outTags, outVals);
                        return;
                    }
                } break;
                case value::TypeTags::NumberInt64: {
                    if (isNumber(rhsTag) &&
                        getWidestNumericalType(inTag, rhsTag) == value::TypeTags::NumberInt64) {
                        compareNativeCppType<Cmp, int64_t>(
                            count, inVals, numericCast<int64_t>(rhsTag, rhsVal), outTags, outVals);
                        return;
                    }
                } break;
                case value::TypeTags::NumberDecimal: {
                    if (isNumber(rhsTag) &&
                        getWidestNumericalType(inTag, rhsTag) == value::TypeTags::NumberDecimal &&
                        rhsTag != value::TypeTags::NumberDouble) {
                        compareNativeCppType<Cmp, Decimal128>(
                            count,
                            inVals,
                            numericCast<Decimal128>(rhsTag, rhsVal),
                            outTags,
                            outVals);
                        return;
                    }
                } break;
                case value::TypeTags::Date: {
                    if (rhsTag == value::TypeTags::Date) {
                        compareNativeCppType<Cmp, int64_t>(
                            count, inVals, value::bitcastTo<int64_t>(rhsVal), outTags, outVals);
                        return;
                    }
                } break;
                default:
                    break;
            }
            for (size_t index = 0; index < count; index++) {
                std::tie(outTags[index], outVals[index]) =
                    value::genericCompare<Cmp>(inTag, inVals[index], rhsTag, rhsVal);
            }
        });

    auto res = blockView->map(cmpOp);

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(res.release()));
}
}  // namespace

template <class Cmp, ColumnOpType::Flags AddFlags>
value::TagValueOwned ByteCode::builtinValueBlockCmpScalar(ArityType arity) {
    tassert(11079911, "Unexpected arity value", arity == 2);
    auto block = viewFromStack(0);
    tassert(8625709,
            "Expected argument to be of valueBlock type",
            block.tag == value::TypeTags::valueBlock);
    auto scalar = viewFromStack(1);

    auto blockView = value::getValueBlock(block.value);

    return blockCompareGeneric<Cmp, AddFlags>(blockView, scalar.tag, scalar.value);
}

/*
 * Comparison against scalar functions.
 */
value::TagValueOwned ByteCode::builtinValueBlockGtScalar(ArityType arity) {
    return builtinValueBlockCmpScalar<std::greater<>, ColumnOpType::kMonotonic>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockGteScalar(ArityType arity) {
    return builtinValueBlockCmpScalar<std::greater_equal<>, ColumnOpType::kMonotonic>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockEqScalar(ArityType arity) {
    // This is not monotonic, because the min and max not being equal to the target value does not
    // imply that no values in the block will be equal to the target value.
    return builtinValueBlockCmpScalar<std::equal_to<>>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockNeqScalar(ArityType arity) {
    auto equalResult = builtinValueBlockCmpScalar<std::equal_to<>>(arity);

    // For neq we apply equal_to and then use genericNot() to negate it, just like the scalar
    // variation in the VM.
    const auto notOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) { return genericNot(tag, val); });

    tassert(8625710,
            "Expected argument to be of valueBlock type",
            equalResult.tag() == value::TypeTags::valueBlock);

    auto res = value::getValueBlock(equalResult.value())->map(notOp);
    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(res.release()));
}

value::TagValueOwned ByteCode::builtinValueBlockLtScalar(ArityType arity) {
    return builtinValueBlockCmpScalar<std::less<>, ColumnOpType::kMonotonic>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockLteScalar(ArityType arity) {
    return builtinValueBlockCmpScalar<std::less_equal<>, ColumnOpType::kMonotonic>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockCmp3wScalar(ArityType arity) {
    tassert(11079910, "Unexpected arity value", arity == 2);
    auto block = viewFromStack(0);
    tassert(8625711,
            "Expected argument to be of valueBlock type",
            block.tag == value::TypeTags::valueBlock);
    auto value = viewFromStack(1);

    auto blockView = value::getValueBlock(block.value);

    const auto cmpOp =
        value::makeColumnOp<ColumnOpType::kMonotonic>([&](value::TypeTags tag, value::Value val) {
            return value::compare3way(tag, val, value.tag, value.value);
        });

    auto res = blockView->map(cmpOp);

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(res.release()));
}

/*
 * Given two blocks and a mask of equal size, return a new block having the values from the first
 * argument when the matching entry in the mask is True, and the values from the second argument
 * when the matching entry in the mask is False.
 */
value::TagValueMaybeOwned ByteCode::builtinValueBlockCombine(ArityType arity) {
    tassert(11079909, "Unexpected arity value", arity == 3);

    auto bitmapView = viewFromStack(2);
    tassert(8141609,
            "valueBlockCombine expects a block of boolean values as mask",
            bitmapView.tag == value::TypeTags::valueBlock);
    auto* bitmap = value::getValueBlock(bitmapView.value);
    auto bitmapExtracted = bitmap->extract();

    if (allBools(bitmapExtracted.tags(), bitmapExtracted.count())) {
        size_t numTrue = 0;
        for (size_t i = 0; i < bitmapExtracted.count(); i++) {
            numTrue += value::bitcastTo<bool>(bitmapExtracted.vals()[i]);
        }
        auto promoteArgAsResult = [&](size_t stackPos) -> value::TagValueMaybeOwned {
            auto [owned, tag, val] = moveFromStack(stackPos);
            tassert(8141611,
                    "valueBlockCombine expects a block as argument",
                    tag == value::TypeTags::valueBlock);
            auto* rhsBlock = value::getValueBlock(val);
            tassert(8141612,
                    "valueBlockCombine expects the arguments to have the same size",
                    rhsBlock->count() == bitmapExtracted.count());
            return value::TagValueMaybeOwned(owned, tag, val);
        };
        if (numTrue == 0) {
            return promoteArgAsResult(1);
        } else if (numTrue == bitmapExtracted.count()) {
            return promoteArgAsResult(0);
        }
    }

    auto lhs = viewFromStack(0);
    tassert(8141615,
            "valueBlockCombine expects a block as first argument",
            lhs.tag == value::TypeTags::valueBlock);
    auto rhs = viewFromStack(1);
    tassert(8141616,
            "valueBlockCombine expects a block as second argument",
            rhs.tag == value::TypeTags::valueBlock);
    auto* lhsBlock = value::getValueBlock(lhs.value);
    auto* rhsBlock = value::getValueBlock(rhs.value);

    auto lhsExtracted = lhsBlock->extract();
    auto rhsExtracted = rhsBlock->extract();
    tassert(8141617,
            "valueBlockCombine expects the arguments to have the same size",
            lhsExtracted.count() == rhsExtracted.count() &&
                lhsExtracted.count() == bitmapExtracted.count());

    std::vector<value::Value> valueOut(bitmapExtracted.count());
    std::vector<value::TypeTags> tagOut(bitmapExtracted.count(), value::TypeTags::Nothing);
    for (size_t i = 0; i < bitmapExtracted.count(); i++) {
        if (bitmapExtracted.tags()[i] == value::TypeTags::Boolean) {
            if (value::bitcastTo<bool>(bitmapExtracted.vals()[i])) {
                std::tie(tagOut[i], valueOut[i]) =
                    value::copyValue(lhsExtracted.tags()[i], lhsExtracted.vals()[i]);
            } else {
                std::tie(tagOut[i], valueOut[i]) =
                    value::copyValue(rhsExtracted.tags()[i], rhsExtracted.vals()[i]);
            }
        }
    }

    auto blockOut = buildBlockFromStorage(std::move(tagOut), std::move(valueOut));

    return value::TagValueMaybeOwned(true,
                                     value::TypeTags::valueBlock,
                                     value::bitcastFrom<value::ValueBlock*>(blockOut.release()));
}

static const auto invokeLambdaOp =
    value::makeColumnOpWithParams<ColumnOpType::kNoFlags, ByteCode::InvokeLambdaFunctor>();

/**
 * Implementation of the valueBlockApplyLambda instruction. This instruction takes a mask, a block
 * and an SBE lambda f(), and produces a new block with the result of f() applied to each element of
 * the input for which the mask has, in the same position, a 'true' value. A mask value of Nothing
 * is equivalent to a mask full of 'true' values.
 */
void ByteCode::valueBlockApplyLambda(const CodeFragment* code) {
    auto [lamOwn, lamTag, lamVal] = moveFromStack(0);
    popAndReleaseStack();
    value::TagValueMaybeOwned lam{lamOwn, lamTag, lamVal};

    auto [blockOwn, blockTag, blockVal] = moveFromStack(0);
    popAndReleaseStack();
    value::TagValueMaybeOwned ownedBlock{blockOwn, blockTag, blockVal};

    auto [maskOwn, maskTag, maskVal] = moveFromStack(0);
    popAndReleaseStack();
    value::TagValueMaybeOwned ownedMask{maskOwn, maskTag, maskVal};

    if (lamTag != value::TypeTags::LocalOneArgLambda) {
        pushStack(false, value::TypeTags::Nothing, 0);
        return;
    }

    if (blockTag != value::TypeTags::valueBlock) {
        pushStack(false, value::TypeTags::Nothing, 0);
        return;
    }

    const auto lamPos = value::bitcastTo<int64_t>(lamVal);
    auto* block = value::getValueBlock(blockVal);

    std::unique_ptr<value::ValueBlock> outBlock;
    if (maskTag == value::TypeTags::valueBlock) {
        // We have a valid mask, loop only over the enabled indexes.
        auto* mask = value::getValueBlock(maskVal);
        auto extractedMask = mask->extract();
        auto extracted = block->extract();
        tassert(8123000,
                "Mask and block have a different number of items",
                extracted.count() == extractedMask.count());
        dassert(allBools(extractedMask.tags(), extractedMask.count()),
                "Expected mask to be all bools");

        // Pre-fill with Nothing, and overwrite only the allowed indexes.
        std::vector<value::Value> valueOut(extracted.count());
        std::vector<value::TypeTags> tagOut(extracted.count(), value::TypeTags::Nothing);

        ByteCode::InvokeLambdaFunctor invoker(*this, code, lamPos);
        for (size_t i = 0; i < extracted.count(); ++i) {
            if (value::bitcastTo<bool>(extractedMask.vals()[i])) {
                std::tie(tagOut[i], valueOut[i]) =
                    invoker(extracted.tags()[i], extracted.vals()[i]);
            }
        }
        outBlock = buildBlockFromStorage(std::move(tagOut), std::move(valueOut));
    } else {
        outBlock = block->map(invokeLambdaOp.bindParams(*this, code, lamPos));
    }

    pushStack(true,
              value::TypeTags::valueBlock,
              value::bitcastFrom<value::ValueBlock*>(outBlock.release()));
}

enum class LogicalOp { AND, OR };

template <int op>
value::TagValueMaybeOwned ByteCode::builtinValueBlockLogicalOperation(ArityType arity) {
    static_assert(op >= 0 && op <= 1, "op should be either 0 or 1");

    tassert(11079908, "Unexpected arity value", arity == 2);
    auto leftInput = viewFromStack(0);
    tassert(8625714,
            "Expected 'left' argument to be of valueBlock type",
            leftInput.tag == value::TypeTags::valueBlock);
    auto* leftValueBlock = value::bitcastTo<value::ValueBlock*>(leftInput.value);

    auto rightInput = viewFromStack(1);
    tassert(8625715,
            "Expected 'right' argument to be of valueBlock type",
            rightInput.tag == value::TypeTags::valueBlock);
    auto* rightValueBlock = value::bitcastTo<value::ValueBlock*>(rightInput.value);

    auto leftBlockSize = leftValueBlock->count();
    auto rightBlockSize = rightValueBlock->count();

    tassert(8649600, "Mismatch on size", leftBlockSize == rightBlockSize);
    tassert(8625712, "Expected bitmap vectors to be non-empty", leftBlockSize > 0);

    auto outputLeft = [](value::TypeTags lTag, value::Value lValue) -> bool {
        if constexpr (static_cast<int>(LogicalOp::AND) == op) {
            return (lTag == value::TypeTags::Boolean && !value::bitcastTo<bool>(lValue));
        } else if constexpr (static_cast<int>(LogicalOp::OR) == op) {
            return (lTag == value::TypeTags::Boolean && value::bitcastTo<bool>(lValue));
        }
    };

    if (auto leftMonoBlock = leftValueBlock->as<value::MonoBlock>(); leftMonoBlock) {
        // If the first item is a Nothing, the answer is always Nothing.
        if (leftMonoBlock->getTag() == value::TypeTags::Nothing) {
            auto nothingBlock =
                std::make_unique<value::MonoBlock>(leftBlockSize, value::TypeTags::Nothing, 0);
            return value::TagValueMaybeOwned(
                true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(nothingBlock.release()));
        }

        if (outputLeft(leftMonoBlock->getTag(), leftMonoBlock->getValue())) {
            return moveMaybeOwnedFromStack(0);
        } else {
            return moveMaybeOwnedFromStack(1);
        }
    }

    auto left = leftValueBlock->extract();
    auto right = rightValueBlock->extract();

    std::vector<value::TypeTags> tagsOut(leftBlockSize, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(leftBlockSize, 0);

    for (size_t i = 0; i < leftBlockSize; ++i) {
        if (left.tags()[i] == value::TypeTags::Nothing) {
            continue;
        }
        auto [resTag, resVal] = outputLeft(left.tags()[i], left.vals()[i])
            ? std::pair(left.tags()[i], left.vals()[i])
            : value::copyValue(right.tags()[i], right.vals()[i]);
        tagsOut[i] = resTag;
        valuesOut[i] = resVal;
    }

    auto blockOut = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return value::TagValueMaybeOwned(true,
                                     value::TypeTags::valueBlock,
                                     value::bitcastFrom<value::ValueBlock*>(blockOut.release()));
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockLogicalAnd(ArityType arity) {
    return builtinValueBlockLogicalOperation<static_cast<int>(LogicalOp::AND)>(arity);
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockLogicalOr(ArityType arity) {
    return builtinValueBlockLogicalOperation<static_cast<int>(LogicalOp::OR)>(arity);
}

value::TagValueOwned ByteCode::builtinValueBlockNewFill(ArityType arity) {
    tassert(11079907, "Unexpected arity value", arity == 2);

    auto right = viewFromStack(1);
    auto count = value::genericNumConvert(right.tag, right.value, value::TypeTags::NumberInt32);
    tassert(8141602,
            "valueBlockNewFill expects an integer in the size argument",
            count.tag() == value::TypeTags::NumberInt32);

    // Take ownership of the value, we are transferring it to the block.
    auto [leftTag, leftVal] = moveRawOwnedFromStack(0);
    auto blockOut = std::make_unique<value::MonoBlock>(
        value::bitcastTo<int32_t>(count.value()), leftTag, leftVal);
    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(blockOut.release()));
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockSize(ArityType arity) {
    tassert(11079906, "Unexpected arity value", arity == 1);

    auto blockView = viewFromStack(0);
    tassert(8141603,
            "valueBlockSize expects a block as argument",
            blockView.tag == value::TypeTags::valueBlock);
    auto* block = value::getValueBlock(blockView.value);
    auto count = block->count();
    tassert(8141604, "block exceeds maximum length", std::in_range<int32_t>(count));

    return value::TagValueMaybeOwned(
        false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(count));
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockNone(ArityType arity) {
    tassert(11079905, "Unexpected arity value", arity == 2);

    auto blockView = viewFromStack(0);
    tassert(8141605,
            "valueBlockNone expects a block as first argument",
            blockView.tag == value::TypeTags::valueBlock);
    auto search = viewFromStack(1);

    auto* block = value::getValueBlock(blockView.value);
    auto extracted = block->extract();

    for (size_t i = 0; i < extracted.count(); i++) {
        auto [cmpTag, cmpVal] = sbe::value::compareValue(
            extracted.tags()[i], extracted.vals()[i], search.tag, search.value);
        if (cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0) {
            return value::TagValueMaybeOwned(
                false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        }
    }
    return value::TagValueMaybeOwned(
        false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
}

value::TagValueOwned ByteCode::builtinValueBlockLogicalNot(ArityType arity) {
    tassert(11079904, "Unexpected arity value", arity == 1);

    auto bitmap = viewFromStack(0);
    tassert(8141607,
            "valueBlockLogicalNot expects a block of boolean values as argument",
            bitmap.tag == value::TypeTags::valueBlock);

    auto bitmapView = value::getValueBlock(bitmap.value);

    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) { return genericNot(tag, val); });

    auto res = bitmapView->map(cmpOp);

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(res.release()));
}

value::TagValueMaybeOwned ByteCode::builtinCellFoldValues_F(ArityType arity) {
    auto valBlock = viewFromStack(0);
    tassert(8625718,
            "Expected argument to be of valueBlock type",
            valBlock.tag == value::TypeTags::valueBlock);
    auto* valueBlock = value::bitcastTo<value::ValueBlock*>(valBlock.value);

    auto cell = viewFromStack(1);
    tassert(8625719,
            "Expected argument to be of cellBlock type",
            cell.tag == value::TypeTags::cellBlock);
    auto* cellBlock = value::bitcastTo<value::CellBlock*>(cell.value);

    const auto& positionInfo = cellBlock->filterPositionInfo();
    const bool isEmptyPositionInfo = emptyPositionInfo(positionInfo);

    if (isEmptyPositionInfo && valueBlock->allTrue().has_value()) {
        // The block is made of all boolean values, return the input unchanged.
        return moveMaybeOwnedFromStack(0);
    }

    auto valsExtracted = valueBlock->extract();
    std::vector<value::Value> folded;

    if (isEmptyPositionInfo) {
        folded.resize(valsExtracted.count(), value::Value(0));
        for (size_t i = 0; i < valsExtracted.count(); ++i) {
            auto [t, v] = valsExtracted[i];
            folded[i] = (t == value::TypeTags::Boolean && value::bitcastTo<bool>(v));
        }
    } else {
        // Note that the representation for positionInfo was chosen for simplicity and not for
        // performance.  If this code ends up being a bottleneck, there are plenty of tricks we can
        // do to speed it up. At time of writing, this code was not at all "hot," so we kept it
        // simple.

        folded.resize(positionInfo.size(), value::Value(0));

        // We keep two parallel iterators, one over the position info and another over the vector of
        // values. There is exactly one position info element per row, so our output is guaranteed
        // to be of size positionInfo.size().

        // pos info: [1, 1, 2, 1, 1, 0, 1, 1]
        //                  ^rowIdx
        //
        // vals:     [true, false, true, true, Nothing, ...]
        //                         ^valIdx
        //
        // We then logically OR the values associated with the same row. So if a row has any 'trues'
        // associated with it, its result is true. Otherwise its result is false. In the special
        // case where a row has 0 values associated with it (corresponding to an empty array), it's
        // result is false.

        size_t valIdx = 0;
        for (size_t rowIdx = 0; rowIdx < positionInfo.size(); ++rowIdx) {
            const auto nElementsForRow = positionInfo[rowIdx];
            tassert(11093704, "nElementsForRow cannot be negative", nElementsForRow >= 0);

            bool foldedResultForRow = false;
            for (int elementForDoc = 0; elementForDoc < nElementsForRow; ++elementForDoc) {
                tassert(8604101, "Corrupt position info", valIdx < valsExtracted.count());

                const bool valForElement =
                    valsExtracted[valIdx].tag == sbe::value::TypeTags::Boolean &&
                    value::bitcastTo<bool>(valsExtracted[valIdx].value);
                ++valIdx;

                foldedResultForRow = foldedResultForRow || valForElement;
            }
            folded[rowIdx] = foldedResultForRow;
        }
    }

    auto blockOut = std::make_unique<value::BoolBlock>(std::move(folded));

    return value::TagValueMaybeOwned(true,
                                     value::TypeTags::valueBlock,
                                     value::bitcastFrom<value::ValueBlock*>(blockOut.release()));
}

value::TagValueMaybeOwned ByteCode::builtinCellFoldValues_P(ArityType arity) {
    auto valBlock = viewFromStack(0);
    tassert(8625720,
            "Expected argument to be of valueBlock type",
            valBlock.tag == value::TypeTags::valueBlock);

    auto cell = viewFromStack(1);
    tassert(8625721,
            "Expected argument to be of cellBlock type",
            cell.tag == value::TypeTags::cellBlock);
    auto* cellBlock = value::bitcastTo<value::CellBlock*>(cell.value);

    const auto& positionInfo = cellBlock->filterPositionInfo();
    uassert(7953901, "Only top-level cell values are supported", emptyPositionInfo(positionInfo));
    // Return the input unchanged.
    return moveMaybeOwnedFromStack(0);
}

value::TagValueMaybeOwned ByteCode::builtinCellBlockGetFlatValuesBlock(ArityType arity) {
    tassert(11079903, "Unexpected arity value", arity == 1);
    auto [cellOwn, cellTag, cellVal] = getFromStack(0);

    if (cellTag != value::TypeTags::cellBlock) {
        return value::TagValueMaybeOwned::nothing();
    }
    tassert(7946600, "Cannot process temporary cell values", !cellOwn);

    auto* cell = value::getCellBlock(cellVal);

    return value::TagValueMaybeOwned(
        false,
        value::TypeTags::valueBlock,
        value::bitcastFrom<value::ValueBlock*>(&cell->getValueBlock()));
}

value::TagValueOwned ByteCode::builtinValueBlockIsMember(ArityType arity) {
    auto valBlock = viewFromStack(0);
    auto arr = viewFromStack(1);

    tassert(8625722,
            "Expected argument to be of valueBlock type",
            valBlock.tag == value::TypeTags::valueBlock);
    auto valueBlockView = value::getValueBlock(valBlock.value);

    if (!value::isArray(arr.tag) && arr.tag != value::TypeTags::inList) {
        auto blockOut = std::make_unique<value::MonoBlock>(
            valueBlockView->count(), value::TypeTags::Nothing, 0);
        return value::TagValueOwned(value::TypeTags::valueBlock,
                                    value::bitcastFrom<value::ValueBlock*>(blockOut.release()));
    }

    auto res = [&]() {
        if (arr.tag == value::TypeTags::inList) {
            auto inList = value::getInListView(arr.value);

            return valueBlockView->map(value::makeColumnOp<ColumnOpType::kNoFlags>(
                [&](value::TypeTags tag, value::Value val) {
                    return std::pair{value::TypeTags::Boolean,
                                     value::bitcastFrom<bool>(tag != value::TypeTags::Nothing &&
                                                              inList->contains(tag, val))};
                }));
        } else if (arr.tag == value::TypeTags::ArraySet) {
            auto arrSet = value::getArraySetView(arr.value);
            auto& values = arrSet->values();

            return valueBlockView->map(value::makeColumnOp<ColumnOpType::kNoFlags>(
                [&](value::TypeTags tag, value::Value val) {
                    return std::pair{
                        value::TypeTags::Boolean,
                        value::bitcastFrom<bool>(values.find({tag, val}) != values.end())};
                }));
        } else {
            value::ValueSetType values(0, value::ValueHash(nullptr), value::ValueEq(nullptr));
            value::arrayForEach(
                arr.tag, arr.value, [&](value::TypeTags elemTag, value::Value elemVal) {
                    values.insert({elemTag, elemVal});
                });

            return valueBlockView->map(value::makeColumnOp<ColumnOpType::kNoFlags>(
                [&](value::TypeTags tag, value::Value val) {
                    return std::pair{
                        value::TypeTags::Boolean,
                        value::bitcastFrom<bool>(values.find({tag, val}) != values.end())};
                }));
        }
    }();

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(res.release()));
}

value::TagValueOwned ByteCode::builtinValueBlockCoerceToBool(ArityType arity) {
    auto valBlock = viewFromStack(0);

    tassert(8625723,
            "Expected argument to be of valueBlock type",
            valBlock.tag == value::TypeTags::valueBlock);
    auto valueBlockView = value::getValueBlock(valBlock.value);

    auto res = valueBlockView->map(value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) { return value::coerceToBool(tag, val); }));

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(res.release()));
}

value::TagValueOwned ByteCode::builtinValueBlockMod(ArityType arity) {
    tassert(11079902, "Unexpected arity value", arity == 2);
    auto input = viewFromStack(0);

    tassert(8332900,
            "First argument of $mod must be block of values.",
            input.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(input.value);

    auto mod = viewFromStack(1);
    if (!value::isNumber(mod.tag)) {
        auto nothingBlock =
            std::make_unique<value::MonoBlock>(valueBlockIn->count(), value::TypeTags::Nothing, 0);
        return value::TagValueOwned(value::TypeTags::valueBlock,
                                    value::bitcastFrom<value::ValueBlock*>(nothingBlock.release()));
    }

    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> std::pair<value::TypeTags, value::Value> {
            auto [resTag, resVal] =
                genericMod(value::TagValueView{tag, val}, mod).releaseToOwnedRaw();
            return {resTag, resVal};
        });

    auto res = valueBlockIn->map(cmpOp);

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(res.release()));
}

value::TagValueOwned ByteCode::builtinValueBlockConvert(ArityType arity) {
    tassert(11079901, "Unexpected arity value", arity == 2);
    auto input = viewFromStack(0);

    tassert(8332901,
            "First argument of convert must be block of values.",
            input.tag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(input.value);

    auto target = viewFromStack(1);
    tassert(8907000, "Expected targetTag to be int32", target.tag == value::TypeTags::NumberInt32);
    auto convertTag = static_cast<sbe::value::TypeTags>(value::bitcastTo<int32_t>(target.value));

    // Numeric convert expects always a numeric type as target. However, it does not check for it
    // and throws if the value is not numeric. We let genericNumConvert do this check and we do not
    // make any checks here.
    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> std::pair<value::TypeTags, value::Value> {
            return value::genericNumConvert(tag, val, convertTag).releaseToOwnedRaw();
        });

    auto res = valueBlockIn->map(cmpOp);

    return value::TagValueOwned(value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(res.release()));
}

template <bool IsAscending>
value::TagValueMaybeOwned ByteCode::builtinValueBlockGetSortKey(ArityType arity) {
    tassert(11079900, "Unexpected arity value", arity == 1 || arity == 2);

    CollatorInterface* collator = nullptr;
    if (arity == 2) {
        auto coll = viewFromStack(1);
        if (coll.tag == value::TypeTags::collator) {
            collator = value::getCollatorView(coll.value);
        }
    }

    auto blockView = viewFromStack(0);
    tassert(8448716,
            "Expected argument to be valueBlock",
            blockView.tag == value::TypeTags::valueBlock);

    auto block = value::getValueBlock(blockView.value);

    if (!block->tryHasArray().get_value_or(true)) {
        // Fast path for non-array case. We just fill any empty values with null.
        std::unique_ptr<value::ValueBlock> filledBlock = block->fillEmpty(value::TypeTags::Null, 0);
        if (!filledBlock) {
            // The block was already dense.
            return moveMaybeOwnedFromStack(0);
        }
        return value::TagValueMaybeOwned(
            true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(filledBlock.release()));
    }

    auto outBlock = IsAscending ? block->map(getSortKeyAscOp.bindParams(collator))
                                : block->map(getSortKeyDescOp.bindParams(collator));

    return value::TagValueMaybeOwned(true,
                                     value::TypeTags::valueBlock,
                                     value::bitcastFrom<value::ValueBlock*>(outBlock.release()));
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockGetSortKeyAsc(ArityType arity) {
    return builtinValueBlockGetSortKey<true /*IsAscending*/>(arity);
}

value::TagValueMaybeOwned ByteCode::builtinValueBlockGetSortKeyDesc(ArityType arity) {
    return builtinValueBlockGetSortKey<false /*IsAscending*/>(arity);
}
}  // namespace mongo::sbe::vm
