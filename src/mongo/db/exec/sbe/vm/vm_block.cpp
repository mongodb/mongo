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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/in_list.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/generic_compare.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/represent_as.h"

#include <algorithm>

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
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockExists(ArityType arity) {
    invariant(arity == 1);
    auto [inputOwned, inputTag, inputVal] = getFromStack(0);

    tassert(8625700,
            "Expected argument to be of valueBlock type",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto out = valueBlockIn->exists();

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

/* This instruction takes as input a ValueBlock and a type mask and returns a ValueBlock indicating
 * whether each value in the ValueBlock is of the type defined by the type mask. If the value is
 * Nothing then Nothing is returned. If no type mask is provided, it returns a MonoBlock with
 * Nothing.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockTypeMatch(
    ArityType arity) {
    invariant(arity == 2);

    auto [inputOwned, inputTag, inputVal] = getFromStack(0);
    tassert(8300800,
            "First argument of valueBlockTypeMatch must be block of values",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [typeMaskOwned, typeMaskTag, typeMaskVal] = getFromStack(1);
    if (typeMaskTag != value::TypeTags::NumberInt32) {
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(
                    value::MonoBlock::makeNothingBlock(valueBlockIn->count()).release())};
    }

    auto typeMask = static_cast<uint32_t>(value::bitcastTo<int32_t>(typeMaskVal));

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

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(valueBlockOut.release())};
}

/* This instruction takes as input a timezoneDB and a ValueBlock and returns a ValueBlock indicating
 * whether each value in the ValueBlock is a valid timezone. If no timezoneDB is provided, it
 * returns a MonoBlock with Nothing.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockIsTimezone(
    ArityType arity) {
    invariant(arity == 2);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    tassert(8300801,
            "Second argument of valueBlockIsTimezone must be block of values",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [timezoneDBOwned, timezoneDBTag, timezoneDBVal] = getFromStack(0);
    if (timezoneDBTag != value::TypeTags::timeZoneDB) {
        auto nothingBlock =
            std::make_unique<value::MonoBlock>(valueBlockIn->count(), value::TypeTags::Nothing, 0);
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(nothingBlock.release())};
    }
    auto timezoneDB = value::getTimeZoneDBView(timezoneDBVal);

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

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(valueBlockOut.release())};
}

/**
 * Implementation of the valueBlockFillEmpty builtin. This instruction takes a block and an
 * SBE value, and produces a new block where all missing values in the block have been replaced
 * with the SBE value.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockFillEmpty(
    ArityType arity) {
    invariant(arity == 2);
    auto [fillOwned, fillTag, fillVal] = getFromStack(1);
    if (fillTag == value::TypeTags::Nothing) {
        return moveFromStack(0);
    }

    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    tassert(8625701,
            "Expected argument to be of valueBlock type",
            blockTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(blockVal);

    auto out = valueBlockIn->fillEmpty(fillTag, fillVal);
    if (!out) {
        // Input block was dense so we can just return it unmodified.
        return moveFromStack(0);
    }

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

/**
 * Implementation of the valueBlockFillEmptyBlock builtin. This instruction takes two blocks of the
 * same size, and produces a new block where all missing values in the first block have been
 * replaced with the correpsonding value in the second block.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockFillEmptyBlock(
    ArityType arity) {
    invariant(arity == 2);
    auto [fillOwned, fillTag, fillVal] = getFromStack(1);
    if (fillTag == value::TypeTags::Nothing) {
        return moveFromStack(0);
    }
    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    tassert(8141618,
            "Arguments of valueBlockFillEmptyBlock must be block of values",
            fillTag == value::TypeTags::valueBlock && blockTag == value::TypeTags::valueBlock);

    auto* fillBlockIn = value::bitcastTo<value::ValueBlock*>(fillVal);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(blockVal);

    if (valueBlockIn->tryDense().get_value_or(false)) {
        return moveFromStack(0);
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

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

/**
 * Implementation of the valueBlockFillType builtin. This instruction takes a block a BSON type
 * mask, and an SBE value, and produces a new block where all values that match the type mask are
 * replaced with the SBE value. Any Nothings in the input will be preserved as Nothings in the
 * output regardless of what the type mask is.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockFillType(
    ArityType arity) {
    invariant(arity == 3);
    auto [fillOwned, fillTag, fillVal] = getFromStack(2);

    auto [typeMaskOwned, typeMaskTag, typeMaskVal] = getFromStack(1);

    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    tassert(8872900,
            "Expected argument to be of valueBlock type",
            blockTag == value::TypeTags::valueBlock);
    auto* block = value::bitcastTo<value::ValueBlock*>(blockVal);

    if (typeMaskTag != value::TypeTags::NumberInt32) {
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(
                    value::MonoBlock::makeNothingBlock(block->count()).release())};
    }
    uint32_t typeMask = static_cast<uint32_t>(value::bitcastTo<int32_t>(typeMaskVal));

    auto out = block->fillType(typeMask, fillTag, fillVal);
    if (!out) {
        // Input block didn't have any non-Nothing values that matched the type mask so we can
        // return it unmodified.
        return moveFromStack(0);
    }

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

template <bool less>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::valueBlockMinMaxImpl(
    value::ValueBlock* inputBlock, value::ValueBlock* bitsetBlock) {
    if (bitsetBlock->allTrue().get_value_or(false)) {
        if constexpr (less) {
            auto [minTag, minVal] = inputBlock->tryMin();
            if (minTag != value::TypeTags::Nothing) {
                auto [minTagCpy, minValCpy] = value::copyValue(minTag, minVal);
                return {true, minTagCpy, minValCpy};
            }
        } else {
            auto [maxTag, maxVal] = inputBlock->tryMax();
            if (maxTag != value::TypeTags::Nothing) {
                auto [maxTagCpy, maxValCpy] = value::copyValue(maxTag, maxVal);
                return {true, maxTagCpy, maxValCpy};
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
        if (!value::bitcastTo<bool>(bitset[i].second) ||
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
    return {true, retTag, retVal};
}

template <bool less>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::valueBlockAggMinMaxImpl(
    value::TypeTags accTag,
    value::Value accVal,
    value::TypeTags inputTag,
    value::Value inputVal,
    value::TypeTags bitsetTag,
    value::Value bitsetVal) {
    // The caller transferred ownership of 'acc' to us, so set up a guard.
    value::ValueGuard guard{accTag, accVal};

    tassert(8625702,
            "Expected input argument to be of valueBlock type",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    tassert(8625703,
            "Expected bitset argument to be of valueBlock type",
            bitsetTag == value::TypeTags::valueBlock);
    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);

    // If there is a valid accumulated value and the min(/max) value of the entire block is
    // greater(/less) than the accumulated value then we can directly return the accumulated value
    if (accTag != value::TypeTags::Nothing) {
        auto [minOrMaxTag, minOrMaxVal] = less ? valueBlockIn->tryMin() : valueBlockIn->tryMax();
        if (minOrMaxTag != value::TypeTags::Nothing) {
            auto [cmpTag, cmpVal] = value::compare3way(minOrMaxTag, minOrMaxVal, accTag, accVal);
            if (cmpTag == value::TypeTags::NumberInt32) {
                int32_t cmp = value::bitcastTo<int32_t>(cmpVal);
                if (less ? cmp >= 0 : cmp <= 0) {
                    guard.reset();
                    return {true, accTag, accVal};
                }
            }
        }
    }

    // Evaluate the min/max value in the block taking into account the bitmap
    auto [resultOwned, resultTag, resultVal] =
        valueBlockMinMaxImpl<less>(valueBlockIn, bitsetBlock);

    value::ValueGuard resultGuard{resultOwned, resultTag, resultVal};

    // If 'result' is not Nothing, check if we should return 'result'.
    if (resultTag != value::TypeTags::Nothing) {
        bool returnResult = false;

        if (accTag == value::TypeTags::Nothing) {
            // If 'acc' is Nothing, then we should return 'result'.
            returnResult = true;
        } else {
            // If 'acc' is not Nothing, compare 'result' with 'acc'.
            auto [tag, val] = value::compare3way(resultTag, resultVal, accTag, accVal);

            if (tag == value::TypeTags::NumberInt32) {
                // If 'result' is less than or equal to 'acc' (is 'less' is true) or if 'result'
                // is greater than or equal to 'acc' (is 'less' is false), then we should return
                // 'result'.
                int32_t cmp = value::bitcastTo<int32_t>(val);
                returnResult = less ? cmp <= 0 : cmp >= 0;
            }
        }

        if (returnResult) {
            // If the result is not owned, make it owned.
            if (!resultOwned) {
                std::tie(resultTag, resultVal) = value::copyValue(resultTag, resultVal);
            }
            // Return result as the updated accumulator state.
            resultGuard.reset();
            return {true, resultTag, resultVal};
        }
    }

    // Return the current accumulator state unmodified.
    guard.reset();
    return {true, accTag, accVal};
}

/*
 * Given a ValueBlock and bitset as input, returns a tag, value pair that contains the minimum value
 * in the block based on compareValue. Values whose corresponding bit is set to false get ignored.
 * This function will return a non-Nothing value if the block contains any non-Nothing values.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggMin(ArityType arity) {
    invariant(arity == 3);

    auto [_, inputTag, inputVal] = getFromStack(2);
    auto [__, bitsetTag, bitsetVal] = getFromStack(1);

    // Move the incoming accumulator state from the stack. We now own and have exclusive access
    // to the accumulator state and can make in-place modifications if desired.
    auto [accTag, accVal] = moveOwnedFromStack(0);

    return valueBlockAggMinMaxImpl<true /* less */>(
        accTag, accVal, inputTag, inputVal, bitsetTag, bitsetVal);
}

/*
 * Given a ValueBlock and bitset as input, returns a tag, value pair that contains the maximum value
 * in the block based on compareValue. Values whose corresponding bit is set to false get ignored.
 * This function will return a non-Nothing value if the block contains any non-Nothing values.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggMax(ArityType arity) {
    invariant(arity == 3);

    auto [_, inputTag, inputVal] = getFromStack(2);
    auto [__, bitsetTag, bitsetVal] = getFromStack(1);

    // Move the incoming accumulator state from the stack. We now own and have exclusive access
    // to the accumulator state and can make in-place modifications if desired.
    auto [accTag, accVal] = moveOwnedFromStack(0);

    return valueBlockAggMinMaxImpl<false /* less */>(
        accTag, accVal, inputTag, inputVal, bitsetTag, bitsetVal);
}

/*
 * Given a ValueBlock bitset, count how many "true" elements there are.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggCount(
    ArityType arity) {
    // TODO SERVER-83450 add monoblock fast path.
    invariant(arity == 2);

    // Move the incoming accumulator state from the stack. We now own and have exclusive access
    // to the accumulator state and can make in-place modifications if desired.
    auto [accTag, accVal] = moveOwnedFromStack(0);
    value::ValueGuard guard{accTag, accVal};

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(1);
    tassert(8625706,
            "Expected bitset argument to be of valueBlock type",
            bitsetTag == value::TypeTags::valueBlock);
    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);

    value::DeblockedTagVals bitset = bitsetBlock->extract();

    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");

    int64_t n = accTag == value::TypeTags::NumberInt64 ? value::bitcastTo<int64_t>(accVal) : 0;

    int64_t count = 0;
    for (size_t i = 0; i < bitset.count(); ++i) {
        if (value::bitcastTo<bool>(bitset[i].second)) {
            ++count;
        }
    }

    return {true, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(n + count)};
}

/*
 * Given a ValueBlock and bitset, returns the sum of the elements of the ValueBlock where the bitset
 * indicates true. If all elements of the bitset are false, return Nothing. If there are non-Nothing
 * elements where the bitset indicates true, we return a value. If there are only Nothing elements,
 * we return Nothing.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggSum(ArityType arity) {
    // TODO SERVER-83450 add monoblock fast path.
    invariant(arity == 3);

    // Move the incoming accumulator state from the stack. We now own and have exclusive access
    // to the accumulator state and can make in-place modifications if desired.
    auto [accTag, accVal] = moveOwnedFromStack(0);
    value::ValueGuard guard{accTag, accVal};

    auto [inputOwned, inputTag, inputVal] = getFromStack(2);
    tassert(8625707,
            "Expected input argument to be of valueBlock type",
            inputTag == value::TypeTags::valueBlock);
    value::ValueBlock* inputBlock = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(1);
    tassert(8625708,
            "Expected bitset argument to be of valueBlock type",
            bitsetTag == value::TypeTags::valueBlock);
    value::ValueBlock* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);

    const value::DeblockedTagVals block = inputBlock->extract();
    const value::DeblockedTagVals bitset = bitsetBlock->extract();

    tassert(
        8151801, "Expected block and bitset to be the same size", block.count() == bitset.count());
    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");

    value::TypeTags blockResTag = value::TypeTags::Nothing;
    value::Value blockResVal = 0;
    for (size_t i = 0; i < bitset.count(); ++i) {
        if (value::bitcastTo<bool>(bitset[i].second) && value::isNumber(block.tags()[i])) {
            // If 'blockRes' is Nothing, set 'blockRes' equal to 'block[i]'. Otherwise, compute the
            // sum of 'blockRes' and 'block[i]' and store the result back into 'blockRes'.
            if (blockResTag == value::TypeTags::Nothing) {
                std::tie(blockResTag, blockResVal) =
                    value::copyValue(block.tags()[i], block.vals()[i]);
            } else {
                value::ValueGuard curBlockResGuard{blockResTag, blockResVal};

                auto [sumOwned, sumTag, sumVal] =
                    genericAdd(blockResTag, blockResVal, block.tags()[i], block.vals()[i]);

                if (!sumOwned) {
                    std::tie(sumTag, sumVal) = value::copyValue(sumTag, sumVal);
                }

                std::tie(blockResTag, blockResVal) = std::pair(sumTag, sumVal);
            }
        }
    }

    if (blockResTag == value::TypeTags::Nothing) {
        // Return the accumulator state unmodified.
        guard.reset();
        return {true, accTag, accVal};
    }

    value::ValueGuard blockResGuard{blockResTag, blockResVal};

    if (accTag == value::TypeTags::Nothing) {
        // Return 'blockRes' as the updated accumulator state.
        blockResGuard.reset();
        return {true, blockResTag, blockResVal};
    }

    // Compute the result of adding the accumulator state with 'blockRes'.
    auto [resultOwned, resultTag, resultVal] = genericAdd(accTag, accVal, blockResTag, blockResVal);

    // If 'result' is not owned, make it owned.
    if (!resultOwned) {
        std::tie(resultTag, resultVal) = value::copyValue(resultTag, resultVal);
    }

    // Return 'result' as the updated accumulator state.
    return {true, resultTag, resultVal};
}  // builtinValueBlockAggSum

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggDoubleDoubleSum(
    ArityType arity) {
    invariant(arity == 3);

    // Input: next block to accumulate.
    auto [blockOwned, blockTag, blockVal] = getFromStack(2);
    tassert(8695107,
            "Expected input argument to be of valueBlock type",
            blockTag == value::TypeTags::valueBlock);
    value::ValueBlock* inputBlock = value::bitcastTo<value::ValueBlock*>(blockVal);

    // Input: bitset matching 'inputBlock'.
    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(1);
    tassert(8695108,
            "Expected bitset argument to be of valueBlock type",
            bitsetTag == value::TypeTags::valueBlock);
    value::ValueBlock* inputBitset = value::bitcastTo<value::ValueBlock*>(bitsetVal);

    // Input-output: running accumulator result. moveOwnedFromStack() takes ownership by our local
    // copy so we can do in-place updates to it.
    auto [accTag, accValue] = moveOwnedFromStack(0);

    // Initialize the accumulator if this is the first use of it.
    if (accTag == value::TypeTags::Nothing) {
        std::tie(accTag, accValue) = genericInitializeDoubleDoubleSumState();
    }

    value::ValueGuard guard{accTag, accValue};
    tassert(8695109, "The result slot must be Array-typed", accTag == value::TypeTags::Array);
    value::Array* accumulator = value::getArrayView(accValue);

    const value::DeblockedTagVals block = inputBlock->extract();
    const value::DeblockedTagVals bitset = inputBitset->extract();
    tassert(
        8695110, "Expected block and bitset to be the same size", block.count() == bitset.count());

    for (size_t i = 0; i < block.count(); ++i) {
        if (value::bitcastTo<bool>(bitset[i].second)) {
            aggDoubleDoubleSumImpl(accumulator, block.tags()[i], block.vals()[i]);
        }
    }

    guard.reset();
    return {true, accTag, accValue};
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

int memAdded(std::pair<value::TypeTags, value::Value> key,
             std::pair<value::TypeTags, value::Value> output) {
    return value::getApproximateSize(key.first, key.second) +
        value::getApproximateSize(output.first, output.second);
}

template <typename Comp>
int addNewPair(value::Array* mergeArr,
               value::TypeTags keyTag,
               value::Value keyVal,
               value::TypeTags outTag,
               value::Value outVal,
               const PairKeyComp<Comp>& keyLess) {
    int memDelta = memAdded(std::pair{keyTag, keyVal}, std::pair{outTag, outVal});

    auto [pairArrTag, pairArrVal] = value::makeNewArray();
    value::ValueGuard pairArrGuard{pairArrTag, pairArrVal};
    auto* pairArr = value::getArrayView(pairArrVal);
    pairArr->reserve(2);

    // Update the sortKey with a copy.
    pairArr->push_back(value::copyValue(keyTag, keyVal));

    // Add a coppy of the output value to the SBE pair array.
    pairArr->push_back(value::copyValue(outTag, outVal));

    // The caller of this function will take ownership of the pair array.
    pairArrGuard.reset();

    mergeArr->push_back(pairArrTag, pairArrVal);

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
        memAdded(std::pair{newKeyTag, newKeyVal}, std::pair{newOutTag, newOutVal});

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

std::pair<value::Array*, std::pair<value::TypeTags, value::Value>> getWorst(
    value::Array* mergeArr) {
    auto [worstTag, worstVal] = mergeArr->getAt(0);
    auto worstArr = value::getArrayView(worstVal);
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
        value::TypeTags bestTag{value::TypeTags::Nothing};
        value::Value bestVal{0u};
        // topN with descending sort and bottomN with ascending sort return the same values for
        // scalar keys. This is not true for array keys, but we also check that bestTag is not an
        // array or object before using this fast path. The "best" possible key in the block cannot
        // be better than the upper bound.
        if ((sense == TopBottomSense::kTop && !isAscending) ||
            (sense == TopBottomSense::kBottom && isAscending)) {
            std::tie(bestTag, bestVal) = sortKeyBlock->tryUpperBound();
        }
        // topN with ascending sort and bottomN with descending sort return the same values subject
        // to the same constraints described above. The "best" possible key in the block cannot be
        // better than the lower bound.
        else {
            std::tie(bestTag, bestVal) = sortKeyBlock->tryLowerBound();
        }
        if (bestTag != value::TypeTags::Nothing && !isArray(bestTag) && !isObject(bestTag)) {
            auto [_, worstKey] = getWorst(mergeArr);

            if (!less(std::pair{bestTag, bestVal}, worstKey)) {
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

                    if (less(std::pair{bestTag, bestVal}, worstKey)) {
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

            if (less(std::pair{newSortKeyTag, newSortKeyVal}, worstKey)) {
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
                MONGO_UNREACHABLE;
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
FastTuple<bool, value::TypeTags, value::Value> ByteCode::blockNativeAggTopBottomNImpl(
    value::TypeTags stateTag,
    value::Value stateVal,
    value::ValueBlock* bitsetBlock,
    SortSpec* sortSpec,
    size_t numKeysBlocks,
    size_t numValuesBlocks) {
    using Less =
        std::conditional_t<Sense == TopBottomSense::kTop, SortPatternLess, SortPatternGreater>;

    invariant(!ValueIsDecomposedArray);
    invariant(numKeysBlocks == 1);
    invariant(numValuesBlocks == 1);

    value::ValueGuard stateGuard{stateTag, stateVal};

    // We already read numKeysBlocks (stack position 3) in builtinValueBlockAggTopBottomNImpl before
    // calling into this function.

    constexpr size_t keysBlocksStartOffset = 4;
    const size_t valuesBlocksStartOffset = keysBlocksStartOffset + numKeysBlocks;

    auto [sortKeyBlockOwned, sortKeyBlockTag, sortKeyBlockVal] =
        getFromStack(keysBlocksStartOffset);
    tassert(8794906,
            "Expected key argument to be of valueBlock type",
            sortKeyBlockTag == value::TypeTags::valueBlock);
    auto* sortKeyBlock = value::getValueBlock(sortKeyBlockVal);
    size_t sortKeyCount = sortKeyBlock->count();

    auto [valBlockOwned, valBlockTag, valBlockVal] = getFromStack(valuesBlocksStartOffset);
    tassert(8794905,
            "Expected key argument to be of valueBlock type",
            valBlockTag == value::TypeTags::valueBlock);
    auto* valBlock = value::getValueBlock(valBlockVal);

    MultiAccState stateTuple = getMultiAccState(stateTag, stateVal);
    auto [state, mergeArr, startIdx, maxSize, memUsage, memLimit, isGroupAccum] = stateTuple;
    invariant(maxSize > 0);

    invariant(sortKeyCount == valBlock->count() && sortKeyCount == bitsetBlock->count());

    const auto sortPattern = sortSpec->getSortPattern();
    bool isAscending = sortPattern.front().isAscending;

    auto less = Less(sortSpec);

    // See if we can skip processing the entire block based on the currently accumulated heap and
    // block metadata of the current block. We don't need to check the bitset because if the best
    // possible value wouldn't make it into the accumulated heap, it doesn't matter if this value
    // passed any filters.
    if (tryFullMergeArrFastPath(Sense, isAscending, mergeArr, maxSize, sortKeyBlock, less)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    // Try to use the argMin/Max API if possible instead of calling extract on the sortKeyBlock.
    if (tryArgMinMaxFastPath(
            Sense, isAscending, stateTuple, bitsetBlock, sortKeyBlock, valBlock, less)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
    }

    auto bitset = bitsetBlock->extract();
    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");
    auto bitsetVals = bitset.valsSpan();

    auto sortKeys = sortKeyBlock->extract();

    // Fast path for $top/$bottom with homogeneous sort field input and no missing values.
    if (tryHomogeneousFastPath(Sense,
                               isAscending,
                               stateTag,
                               stateVal,
                               stateTuple,
                               bitsetVals,
                               sortKeys,
                               valBlock,
                               less)) {
        stateGuard.reset();
        return {true, stateTag, stateVal};
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
    stateGuard.reset();
    return {true, stateTag, stateVal};
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

    bool keySortsBeforeImpl(std::pair<value::TypeTags, value::Value> item) final {
        tassert(8448705, "Expected item to be an Array", item.first == value::TypeTags::Array);

        const SortPattern& sortPattern = _sortSpec->getSortPattern();
        tassert(8448706,
                "Expected numKeys to be equal to number of sort pattern parts",
                sortPattern.size() == _keys.size());

        auto itemArray = value::getArrayView(item.second);
        tassert(8448707,
                "Expected size of item array to be equal to number of sort pattern parts",
                sortPattern.size() == itemArray->size());

        if (_sense == TopBottomSense::kTop) {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                auto [keyTag, keyVal] = _keys[i][_blockIndex];
                auto [itemTag, itemVal] = itemArray->getAt(i);
                int32_t cmp = compare<TopBottomSense::kTop>(keyTag, keyVal, itemTag, itemVal);

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        } else {
            for (size_t i = 0; i < sortPattern.size(); i++) {
                auto [keyTag, keyVal] = _keys[i][_blockIndex];
                auto [itemTag, itemVal] = itemArray->getAt(i);
                int32_t cmp = compare<TopBottomSense::kBottom>(keyTag, keyVal, itemTag, itemVal);

                if (cmp != 0) {
                    return sortPattern[i].isAscending ? cmp < 0 : cmp > 0;
                }
            }
        }

        return false;
    }

    std::pair<value::TypeTags, value::Value> getOwnedKeyImpl() final {
        auto [keysArrTag, keysArrVal] = value::makeNewArray();
        value::ValueGuard keysArrGuard{keysArrTag, keysArrVal};
        auto keysArr = value::getArrayView(keysArrVal);

        for (size_t i = 0; i < _keys.size(); ++i) {
            auto [keyTag, keyVal] = _keys[i][_blockIndex];
            std::tie(keyTag, keyVal) = value::copyValue(keyTag, keyVal);
            keysArr->push_back(keyTag, keyVal);
        }

        keysArrGuard.reset();
        return std::pair{keysArrTag, keysArrVal};
    }

    std::pair<value::TypeTags, value::Value> getOwnedValueImpl() final {
        auto [valuesArrTag, valuesArrVal] = value::makeNewArray();
        value::ValueGuard valuesArrGuard{valuesArrTag, valuesArrVal};
        auto valuesArr = value::getArrayView(valuesArrVal);

        for (size_t i = 0; i < _values.size(); ++i) {
            auto [valueTag, valueVal] = _values[i][_blockIndex];
            std::tie(valueTag, valueVal) = value::copyValue(valueTag, valueVal);
            valuesArr->push_back(valueTag, valueVal);
        }

        valuesArrGuard.reset();
        return std::pair{valuesArrTag, valuesArrVal};
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
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggTopBottomNImpl(
    ArityType arity) {

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(1);
    tassert(8448708,
            "Expected bitset argument to be of valueBlock type",
            bitsetTag == value::TypeTags::valueBlock);

    auto [sortSpecOwned, sortSpecTag, sortSpecVal] = getFromStack(2);
    tassert(8448709, "Argument must be of sortSpec type", sortSpecTag == value::TypeTags::sortSpec);

    size_t numKeysBlocks = 1;
    bool keyIsDecomposed = false;
    auto [_, numKeysBlocksTag, numKeysBlocksVal] = getFromStack(3);
    if (numKeysBlocksTag == value::TypeTags::NumberInt32) {
        numKeysBlocks = static_cast<size_t>(value::bitcastTo<int32_t>(numKeysBlocksVal));
        keyIsDecomposed = true;
    } else {
        tassert(8448710,
                "Expected numKeys to be Null or Int32",
                numKeysBlocksTag == value::TypeTags::Null);
    }

    constexpr size_t keysBlocksStartOffset = 4;
    const size_t valuesBlocksStartOffset = keysBlocksStartOffset + numKeysBlocks;
    const size_t numValuesBlocks = ValueIsDecomposedArray ? arity - valuesBlocksStartOffset : 1;

    auto [stateTag, stateVal] = moveOwnedFromStack(0);
    value::ValueGuard stateGuard{stateTag, stateVal};

    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);
    auto ss = value::getSortSpecView(sortSpecVal);

    if (!keyIsDecomposed && !ValueIsDecomposedArray) {
        stateGuard.reset();
        return blockNativeAggTopBottomNImpl<Sense, ValueIsDecomposedArray>(
            stateTag, stateVal, bitsetBlock, ss, numKeysBlocks, numValuesBlocks);
    }

    auto [state, array, startIdx, maxSize, memUsage, memLimit, isGroupAccum] =
        getMultiAccState(stateTag, stateVal);
    invariant(maxSize > 0);

    value::DeblockedTagVals bitset = bitsetBlock->extract();

    dassert(allBools(bitset.tags(), bitset.count()), "Expected bitset to be all bools");

    std::vector<value::DeblockedTagVals> keys;
    std::vector<value::DeblockedTagVals> values;
    keys.reserve(numKeysBlocks);
    values.reserve(numValuesBlocks);

    for (size_t i = 0; i < numKeysBlocks; ++i) {
        auto [_, keysBlockTag, keysBlockVal] = getFromStack(keysBlocksStartOffset + i);
        tassert(8448712,
                "Expected argument to be of valueBlock type",
                keysBlockTag == value::TypeTags::valueBlock);

        keys.emplace_back(value::getValueBlock(keysBlockVal)->extract());

        tassert(8448713,
                "Expected block and bitset to be the same size",
                keys.back().count() == bitset.count());
    }

    for (size_t i = 0; i < numValuesBlocks; ++i) {
        auto [_, valuesBlockTag, valuesBlockVal] = getFromStack(valuesBlocksStartOffset + i);
        tassert(8448714,
                "Expected argument to be of valueBlock type",
                valuesBlockTag == value::TypeTags::valueBlock);

        values.emplace_back(value::getValueBlock(valuesBlockVal)->extract());

        tassert(8448715,
                "Expected block and bitset to be the same size",
                values.back().count() == bitset.count());
    }

    TopBottomArgsFromBlocks topBottomArgs{
        Sense, ss, keyIsDecomposed, ValueIsDecomposedArray, std::move(keys), std::move(values)};

    for (size_t blockIndex = 0; blockIndex < bitset.count(); ++blockIndex) {
        if (value::bitcastTo<bool>(bitset[blockIndex].second)) {
            topBottomArgs.initForBlockIndex(blockIndex);

            if constexpr (Sense == TopBottomSense::kTop) {
                memUsage = aggTopNAdd(state, array, maxSize, memUsage, memLimit, topBottomArgs);
            } else {
                memUsage = aggBottomNAdd(state, array, maxSize, memUsage, memLimit, topBottomArgs);
            }
        }
    }

    stateGuard.reset();
    return {true, stateTag, stateVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggTopN(ArityType arity) {
    return builtinValueBlockAggTopBottomNImpl<TopBottomSense::kTop, false>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggBottomN(
    ArityType arity) {
    return builtinValueBlockAggTopBottomNImpl<TopBottomSense::kBottom, false>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggTopNArray(
    ArityType arity) {
    return builtinValueBlockAggTopBottomNImpl<TopBottomSense::kTop, true>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAggBottomNArray(
    ArityType arity) {
    return builtinValueBlockAggTopBottomNImpl<TopBottomSense::kBottom, true>(arity);
}

enum class ArithmeticOp { Addition, Subtraction, Multiplication, Division };

template <int op>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBlockBlockArithmeticOperation(
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
            auto [_, resTag, resVal] = genericAdd(
                leftBlock[i].first, leftBlock[i].second, rightBlock[i].first, rightBlock[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [_, resTag, resVal] = genericSub(
                leftBlock[i].first, leftBlock[i].second, rightBlock[i].first, rightBlock[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [_, resTag, resVal] = genericMul(
                leftBlock[i].first, leftBlock[i].second, rightBlock[i].first, rightBlock[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [_, resTag, resVal] = genericDiv(
                leftBlock[i].first, leftBlock[i].second, rightBlock[i].first, rightBlock[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(resBlock.release())};
}

template <int op>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBlockBlockArithmeticOperation(
    value::ValueBlock* leftInputBlock, value::ValueBlock* rightInputBlock, size_t valsNum) {
    auto leftBlock = leftInputBlock->extract();
    auto rightBlock = rightInputBlock->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [_, resTag, resVal] = genericAdd(
                leftBlock[i].first, leftBlock[i].second, rightBlock[i].first, rightBlock[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [_, resTag, resVal] = genericSub(
                leftBlock[i].first, leftBlock[i].second, rightBlock[i].first, rightBlock[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [_, resTag, resVal] = genericMul(
                leftBlock[i].first, leftBlock[i].second, rightBlock[i].first, rightBlock[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [_, resTag, resVal] = genericDiv(
                leftBlock[i].first, leftBlock[i].second, rightBlock[i].first, rightBlock[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(resBlock.release())};
}

template <int op>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinScalarBlockArithmeticOperation(
    const value::TypeTags* bitsetTags,
    const value::Value* bitsetVals,
    std::pair<value::TypeTags, value::Value> scalar,
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
            auto [_, resTag, resVal] = genericAdd(
                scalar.first, scalar.second, extractedValues[i].first, extractedValues[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [_, resTag, resVal] = genericSub(
                scalar.first, scalar.second, extractedValues[i].first, extractedValues[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [_, resTag, resVal] = genericMul(
                scalar.first, scalar.second, extractedValues[i].first, extractedValues[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [_, resTag, resVal] = genericDiv(
                scalar.first, scalar.second, extractedValues[i].first, extractedValues[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(resBlock.release())};
}

template <int op>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinScalarBlockArithmeticOperation(
    std::pair<value::TypeTags, value::Value> scalar, value::ValueBlock* block, size_t valsNum) {
    auto extractedValues = block->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [_, resTag, resVal] = genericAdd(
                scalar.first, scalar.second, extractedValues[i].first, extractedValues[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [_, resTag, resVal] = genericSub(
                scalar.first, scalar.second, extractedValues[i].first, extractedValues[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [_, resTag, resVal] = genericMul(
                scalar.first, scalar.second, extractedValues[i].first, extractedValues[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [_, resTag, resVal] = genericDiv(
                scalar.first, scalar.second, extractedValues[i].first, extractedValues[i].second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(resBlock.release())};
}

template <int op>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBlockScalarArithmeticOperation(
    const value::TypeTags* bitsetTags,
    const value::Value* bitsetVals,
    value::ValueBlock* block,
    std::pair<value::TypeTags, value::Value> scalar,
    size_t valsNum) {
    auto extractedValues = block->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if (bitsetTags[i] != value::TypeTags::Boolean || !value::bitcastTo<bool>(bitsetVals[i])) {
            continue;
        }
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [_, resTag, resVal] = genericAdd(
                extractedValues[i].first, extractedValues[i].second, scalar.first, scalar.second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [_, resTag, resVal] = genericSub(
                extractedValues[i].first, extractedValues[i].second, scalar.first, scalar.second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [_, resTag, resVal] = genericMul(
                extractedValues[i].first, extractedValues[i].second, scalar.first, scalar.second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [_, resTag, resVal] = genericDiv(
                extractedValues[i].first, extractedValues[i].second, scalar.first, scalar.second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(resBlock.release())};
}

template <int op>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinBlockScalarArithmeticOperation(
    value::ValueBlock* block, std::pair<value::TypeTags, value::Value> scalar, size_t valsNum) {
    auto extractedValues = block->extract();

    std::vector<value::TypeTags> tagsOut(valsNum, value::TypeTags::Nothing);
    std::vector<value::Value> valuesOut(valsNum, 0);

    for (size_t i = 0; i < valsNum; ++i) {
        if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
            auto [_, resTag, resVal] = genericAdd(
                extractedValues[i].first, extractedValues[i].second, scalar.first, scalar.second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
            auto [_, resTag, resVal] = genericSub(
                extractedValues[i].first, extractedValues[i].second, scalar.first, scalar.second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
            auto [_, resTag, resVal] = genericMul(
                extractedValues[i].first, extractedValues[i].second, scalar.first, scalar.second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
            auto [_, resTag, resVal] = genericDiv(
                extractedValues[i].first, extractedValues[i].second, scalar.first, scalar.second);
            tagsOut[i] = resTag;
            valuesOut[i] = resVal;
        }
    }

    auto resBlock = buildBlockFromStorage(std::move(tagsOut), std::move(valuesOut));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(resBlock.release())};
}

template <int op>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinScalarScalarArithmeticOperation(
    std::pair<value::TypeTags, value::Value> leftInputScalar,
    std::pair<value::TypeTags, value::Value> rightInputScalar,
    size_t valsNum) {
    std::unique_ptr<value::MonoBlock> resBlock;
    if constexpr (static_cast<int>(ArithmeticOp::Addition) == op) {
        auto [_, resultTag, resultValue] = genericAdd(leftInputScalar.first,
                                                      leftInputScalar.second,
                                                      rightInputScalar.first,
                                                      rightInputScalar.second);
        resBlock = std::make_unique<value::MonoBlock>(valsNum, resultTag, resultValue);
    } else if constexpr (static_cast<int>(ArithmeticOp::Subtraction) == op) {
        auto [_, resultTag, resultValue] = genericSub(leftInputScalar.first,
                                                      leftInputScalar.second,
                                                      rightInputScalar.first,
                                                      rightInputScalar.second);
        resBlock = std::make_unique<value::MonoBlock>(valsNum, resultTag, resultValue);
    } else if constexpr (static_cast<int>(ArithmeticOp::Multiplication) == op) {
        auto [_, resultTag, resultValue] = genericMul(leftInputScalar.first,
                                                      leftInputScalar.second,
                                                      rightInputScalar.first,
                                                      rightInputScalar.second);
        resBlock = std::make_unique<value::MonoBlock>(valsNum, resultTag, resultValue);
    } else if constexpr (static_cast<int>(ArithmeticOp::Division) == op) {
        auto [_, resultTag, resultValue] = genericDiv(leftInputScalar.first,
                                                      leftInputScalar.second,
                                                      rightInputScalar.first,
                                                      rightInputScalar.second);
        resBlock = std::make_unique<value::MonoBlock>(valsNum, resultTag, resultValue);
    } else {
        resBlock = std::make_unique<value::MonoBlock>(valsNum, value::TypeTags::Nothing, 0);
    }

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(resBlock.release())};
}

template <int op>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockArithmeticOperation(
    ArityType arity) {

    static_assert(op >= 0 && op <= 3, "op should be between 0 and 3 inclusive");

    invariant(arity == 3);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    auto [lOwned, lTag, lVal] = getFromStack(1);
    auto [rOwned, rTag, rVal] = getFromStack(2);

    tassert(8332300,
            "First argument of block arithmetic operation must be block of values representing a "
            "bitmask or Nothing",
            bitsetTag == value::TypeTags::valueBlock || bitsetTag == value::TypeTags::Nothing);
    const value::Value* bitsetVals = nullptr;
    const value::TypeTags* bitsetTags = nullptr;
    size_t valsNum = 0;
    if (bitsetTag == value::TypeTags::valueBlock) {
        auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);
        auto bitset = bitsetBlock->extract();
        bitsetVals = bitset.vals();
        bitsetTags = bitset.tags();
        valsNum = bitset.count();
    }

    tassert(8332302,
            "At least one of the second and third arguments of block arithmetic operation must be "
            "block of values",
            lTag == value::TypeTags::valueBlock || rTag == value::TypeTags::valueBlock);

    if (lTag == value::TypeTags::valueBlock && rTag == value::TypeTags::valueBlock) {
        // Block - Block

        auto* leftInputBlock = value::bitcastTo<value::ValueBlock*>(lVal);
        auto* rightInputBlock = value::bitcastTo<value::ValueBlock*>(rVal);

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
                    std::pair<value::TypeTags, value::Value>{leftMonoBlock->getTag(),
                                                             leftMonoBlock->getValue()},
                    rightInputBlock,
                    valsNum);
            }
            return builtinScalarBlockArithmeticOperation<op>(
                std::pair<value::TypeTags, value::Value>{leftMonoBlock->getTag(),
                                                         leftMonoBlock->getValue()},
                rightInputBlock,
                valsNum);
        } else if (!leftMonoBlock && rightMonoBlock) {
            if (bitsetVals) {
                return builtinBlockScalarArithmeticOperation<op>(
                    bitsetTags,
                    bitsetVals,
                    leftInputBlock,
                    std::pair<value::TypeTags, value::Value>{rightMonoBlock->getTag(),
                                                             rightMonoBlock->getValue()},
                    valsNum);
            }
            return builtinBlockScalarArithmeticOperation<op>(
                leftInputBlock,
                std::pair<value::TypeTags, value::Value>{rightMonoBlock->getTag(),
                                                         rightMonoBlock->getValue()},
                valsNum);
        } else {
            if (bitsetVals) {
                return builtinBlockBlockArithmeticOperation<op>(
                    bitsetTags, bitsetVals, leftInputBlock, rightInputBlock, valsNum);
            }

            return builtinScalarScalarArithmeticOperation<op>(
                std::pair<value::TypeTags, value::Value>{leftMonoBlock->getTag(),
                                                         leftMonoBlock->getValue()},
                std::pair<value::TypeTags, value::Value>{rightMonoBlock->getTag(),
                                                         rightMonoBlock->getValue()},
                valsNum);
        }
    } else if (lTag != value::TypeTags::valueBlock && rTag == value::TypeTags::valueBlock) {
        // scalar - block
        auto* rightInputBlock = value::bitcastTo<value::ValueBlock*>(rVal);
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
                bitsetTags,
                bitsetVals,
                std::pair<value::TypeTags, value::Value>{lTag, lVal},
                rightInputBlock,
                valsNum);
        } else if (rightMonoBlock) {
            return builtinScalarScalarArithmeticOperation<op>(
                std::pair<value::TypeTags, value::Value>{lTag, lVal},
                std::pair<value::TypeTags, value::Value>{rightMonoBlock->getTag(),
                                                         rightMonoBlock->getValue()},
                valsNum);

        } else {
            return builtinScalarBlockArithmeticOperation<op>(
                std::pair<value::TypeTags, value::Value>{lTag, lVal}, rightInputBlock, valsNum);
        }

    } else if (lTag == value::TypeTags::valueBlock && rTag != value::TypeTags::valueBlock) {
        // block - scalar
        auto* leftInputBlock = value::bitcastTo<value::ValueBlock*>(lVal);
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
                bitsetTags,
                bitsetVals,
                leftInputBlock,
                std::pair<value::TypeTags, value::Value>{rTag, rVal},
                valsNum);
        } else if (leftMonoBlock) {
            return builtinScalarScalarArithmeticOperation<op>(
                std::pair<value::TypeTags, value::Value>{leftMonoBlock->getTag(),
                                                         leftMonoBlock->getValue()},
                std::pair<value::TypeTags, value::Value>{rTag, rVal},
                valsNum);
        } else {
            return builtinBlockScalarArithmeticOperation<op>(
                leftInputBlock, std::pair<value::TypeTags, value::Value>{rTag, rVal}, valsNum);
        }

    } else {
        MONGO_UNREACHABLE
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockAdd(ArityType arity) {
    return builtinValueBlockArithmeticOperation<static_cast<int>(ArithmeticOp::Addition)>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockSub(ArityType arity) {
    return builtinValueBlockArithmeticOperation<static_cast<int>(ArithmeticOp::Subtraction)>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockMult(ArityType arity) {
    return builtinValueBlockArithmeticOperation<static_cast<int>(ArithmeticOp::Multiplication)>(
        arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockDiv(ArityType arity) {
    return builtinValueBlockArithmeticOperation<static_cast<int>(ArithmeticOp::Division)>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::blockRoundTrunc(
    std::string funcName, Decimal128::RoundingMode roundingMode, ArityType arity) {
    invariant(arity == 1 || arity == 2);
    auto [inputOwned, inputTag, inputVal] = getFromStack(0);
    tassert(8333100,
            "First argument of " + funcName + " must be block of values.",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    int32_t place = 0;
    if (arity == 2) {
        const auto [placeOwn, placeTag, placeVal] = getFromStack(1);
        if (!value::isNumber(placeTag)) {
            return {false, value::TypeTags::Nothing, 0};
        }
        place = convertNumericToInt32(placeTag, placeVal);
    }

    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> std::pair<value::TypeTags, value::Value> {
            auto [_, resTag, resVal] = genericRoundTrunc(funcName, roundingMode, place, tag, val);
            return {resTag, resVal};
        });

    auto res = valueBlockIn->map(cmpOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockTrunc(ArityType arity) {
    return blockRoundTrunc("$trunc", Decimal128::kRoundTowardZero, arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockRound(ArityType arity) {
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
FastTuple<bool, value::TypeTags, value::Value> blockCompareGeneric(value::ValueBlock* blockView,
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

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}
}  // namespace

template <class Cmp, ColumnOpType::Flags AddFlags>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCmpScalar(
    ArityType arity) {
    invariant(arity == 2);
    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    tassert(8625709,
            "Expected argument to be of valueBlock type",
            blockTag == value::TypeTags::valueBlock);
    auto [valueOwned, valueTag, valueVal] = getFromStack(1);

    auto blockView = value::getValueBlock(blockVal);

    return blockCompareGeneric<Cmp, AddFlags>(blockView, valueTag, valueVal);
}

/*
 * Comparison against scalar functions.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGtScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::greater<>, ColumnOpType::kMonotonic>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGteScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::greater_equal<>, ColumnOpType::kMonotonic>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockEqScalar(
    ArityType arity) {
    // This is not monotonic, because the min and max not being equal to the target value does not
    // imply that no values in the block will be equal to the target value.
    return builtinValueBlockCmpScalar<std::equal_to<>>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockNeqScalar(
    ArityType arity) {
    auto [blockOwned, blockTag, blockVal] = builtinValueBlockCmpScalar<std::equal_to<>>(arity);
    value::ValueGuard guard(blockTag, blockVal);

    // For neq we apply equal_to and then use genericNot() to negate it, just like the scalar
    // variation in the VM.
    const auto notOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) { return genericNot(tag, val); });

    tassert(8625710,
            "Expected argument to be of valueBlock type",
            blockTag == value::TypeTags::valueBlock);

    auto res = value::getValueBlock(blockVal)->map(notOp);
    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLtScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::less<>, ColumnOpType::kMonotonic>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLteScalar(
    ArityType arity) {
    return builtinValueBlockCmpScalar<std::less_equal<>, ColumnOpType::kMonotonic>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCmp3wScalar(
    ArityType arity) {
    invariant(arity == 2);
    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    tassert(8625711,
            "Expected argument to be of valueBlock type",
            blockTag == value::TypeTags::valueBlock);
    auto value = getFromStack(1);

    auto blockView = value::getValueBlock(blockVal);

    const auto cmpOp =
        value::makeColumnOp<ColumnOpType::kMonotonic>([&](value::TypeTags tag, value::Value val) {
            return value::compare3way(tag, val, value.b, value.c);
        });

    auto res = blockView->map(cmpOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

/*
 * Given two blocks and a mask of equal size, return a new block having the values from the first
 * argument when the matching entry in the mask is True, and the values from the second argument
 * when the matching entry in the mask is False.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCombine(ArityType arity) {
    invariant(arity == 3);

    auto [bitmapOwned, bitmapTag, bitmapVal] = getFromStack(2);
    tassert(8141609,
            "valueBlockCombine expects a block of boolean values as mask",
            bitmapTag == value::TypeTags::valueBlock);
    auto* bitmap = value::getValueBlock(bitmapVal);
    auto bitmapExtracted = bitmap->extract();

    if (allBools(bitmapExtracted.tags(), bitmapExtracted.count())) {
        size_t numTrue = 0;
        for (size_t i = 0; i < bitmapExtracted.count(); i++) {
            numTrue += value::bitcastTo<bool>(bitmapExtracted.vals()[i]);
        }
        auto promoteArgAsResult =
            [&](size_t stackPos) -> FastTuple<bool, value::TypeTags, value::Value> {
            auto [owned, tag, val] = moveFromStack(stackPos);
            tassert(8141611,
                    "valueBlockCombine expects a block as argument",
                    tag == value::TypeTags::valueBlock);
            auto* rhsBlock = value::getValueBlock(val);
            tassert(8141612,
                    "valueBlockCombine expects the arguments to have the same size",
                    rhsBlock->count() == bitmapExtracted.count());
            return {owned, tag, val};
        };
        if (numTrue == 0) {
            return promoteArgAsResult(1);
        } else if (numTrue == bitmapExtracted.count()) {
            return promoteArgAsResult(0);
        }
    }

    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
    tassert(8141615,
            "valueBlockCombine expects a block as first argument",
            lhsTag == value::TypeTags::valueBlock);
    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(1);
    tassert(8141616,
            "valueBlockCombine expects a block as second argument",
            rhsTag == value::TypeTags::valueBlock);
    auto* lhsBlock = value::getValueBlock(lhsVal);
    auto* rhsBlock = value::getValueBlock(rhsVal);

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

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
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
    value::ValueGuard lamGuard(lamOwn, lamTag, lamVal);

    auto [blockOwn, blockTag, blockVal] = moveFromStack(0);
    popAndReleaseStack();
    value::ValueGuard blockGuard(blockOwn, blockTag, blockVal);

    auto [maskOwn, maskTag, maskVal] = moveFromStack(0);
    popAndReleaseStack();
    value::ValueGuard maskGuard(maskOwn, maskTag, maskVal);

    if (lamTag != value::TypeTags::LocalLambda) {
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
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalOperation(
    ArityType arity) {
    static_assert(op >= 0 && op <= 1, "op should be either 0 or 1");

    invariant(arity == 2);
    auto [leftOwned, leftInputTag, leftInputVal] = getFromStack(0);
    tassert(8625714,
            "Expected 'left' argument to be of valueBlock type",
            leftInputTag == value::TypeTags::valueBlock);
    auto* leftValueBlock = value::bitcastTo<value::ValueBlock*>(leftInputVal);

    auto [rightOwned, rightInputTag, rightInputVal] = getFromStack(1);
    tassert(8625715,
            "Expected 'right' argument to be of valueBlock type",
            rightInputTag == value::TypeTags::valueBlock);
    auto* rightValueBlock = value::bitcastTo<value::ValueBlock*>(rightInputVal);

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
            return {true,
                    value::TypeTags::valueBlock,
                    value::bitcastFrom<value::ValueBlock*>(nothingBlock.release())};
        }

        if (outputLeft(leftMonoBlock->getTag(), leftMonoBlock->getValue())) {
            return moveFromStack(0);
        } else {
            return moveFromStack(1);
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

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalAnd(
    ArityType arity) {
    return builtinValueBlockLogicalOperation<static_cast<int>(LogicalOp::AND)>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalOr(
    ArityType arity) {
    return builtinValueBlockLogicalOperation<static_cast<int>(LogicalOp::OR)>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockNewFill(ArityType arity) {
    invariant(arity == 2);

    auto [rightOwned, rightTag, rightVal] = getFromStack(1);
    auto [countOwned, countTag, countVal] =
        value::genericNumConvert(rightTag, rightVal, value::TypeTags::NumberInt32);
    tassert(8141602,
            "valueBlockNewFill expects an integer in the size argument",
            countTag == value::TypeTags::NumberInt32);

    // Take ownership of the value, we are transferring it to the block.
    auto [leftTag, leftVal] = moveOwnedFromStack(0);
    auto blockOut =
        std::make_unique<value::MonoBlock>(value::bitcastTo<int32_t>(countVal), leftTag, leftVal);
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockSize(ArityType arity) {
    invariant(arity == 1);

    auto [_, blockTag, blockVal] = getFromStack(0);
    tassert(8141603,
            "valueBlockSize expects a block as argument",
            blockTag == value::TypeTags::valueBlock);
    auto* block = value::getValueBlock(blockVal);
    auto count = block->count();
    tassert(8141604, "block exceeds maximum length", mongo::detail::inRange<int32_t>(count));

    return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(count)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockNone(ArityType arity) {
    invariant(arity == 2);

    auto [blockOwned, blockTag, blockVal] = getFromStack(0);
    tassert(8141605,
            "valueBlockNone expects a block as first argument",
            blockTag == value::TypeTags::valueBlock);
    auto [searchOwned, searchTag, searchVal] = getFromStack(1);

    auto* block = value::getValueBlock(blockVal);
    auto extracted = block->extract();

    for (size_t i = 0; i < extracted.count(); i++) {
        auto [cmpTag, cmpVal] = sbe::value::compareValue(
            extracted.tags()[i], extracted.vals()[i], searchTag, searchVal);
        if (cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0) {
            return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false)};
        }
    }
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalNot(
    ArityType arity) {
    invariant(arity == 1);

    auto [bitmapOwned, bitmapTag, bitmapVal] = getFromStack(0);
    tassert(8141607,
            "valueBlockLogicalNot expects a block of boolean values as argument",
            bitmapTag == value::TypeTags::valueBlock);

    auto bitmapView = value::getValueBlock(bitmapVal);

    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) { return genericNot(tag, val); });

    auto res = bitmapView->map(cmpOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCellFoldValues_F(ArityType arity) {
    auto [valBlockOwned, valBlockTag, valBlockVal] = getFromStack(0);
    tassert(8625718,
            "Expected argument to be of valueBlock type",
            valBlockTag == value::TypeTags::valueBlock);
    auto* valueBlock = value::bitcastTo<value::ValueBlock*>(valBlockVal);

    auto [cellOwned, cellTag, cellVal] = getFromStack(1);
    tassert(8625719,
            "Expected argument to be of cellBlock type",
            cellTag == value::TypeTags::cellBlock);
    auto* cellBlock = value::bitcastTo<value::CellBlock*>(cellVal);

    const auto& positionInfo = cellBlock->filterPositionInfo();
    const bool isEmptyPositionInfo = emptyPositionInfo(positionInfo);

    if (isEmptyPositionInfo && valueBlock->allTrue().has_value()) {
        // The block is made of all boolean values, return the input unchanged.
        return moveFromStack(0);
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
            invariant(nElementsForRow >= 0);

            bool foldedResultForRow = false;
            for (int elementForDoc = 0; elementForDoc < nElementsForRow; ++elementForDoc) {
                tassert(8604101, "Corrupt position info", valIdx < valsExtracted.count());

                const bool valForElement =
                    valsExtracted[valIdx].first == sbe::value::TypeTags::Boolean &&
                    value::bitcastTo<bool>(valsExtracted[valIdx].second);
                ++valIdx;

                foldedResultForRow = foldedResultForRow || valForElement;
            }
            folded[rowIdx] = foldedResultForRow;
        }
    }

    auto blockOut = std::make_unique<value::BoolBlock>(std::move(folded));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCellFoldValues_P(ArityType arity) {
    auto [valBlockOwned, valBlockTag, valBlockVal] = getFromStack(0);
    tassert(8625720,
            "Expected argument to be of valueBlock type",
            valBlockTag == value::TypeTags::valueBlock);

    auto [cellOwned, cellTag, cellVal] = getFromStack(1);
    tassert(8625721,
            "Expected argument to be of cellBlock type",
            cellTag == value::TypeTags::cellBlock);
    auto* cellBlock = value::bitcastTo<value::CellBlock*>(cellVal);

    const auto& positionInfo = cellBlock->filterPositionInfo();
    uassert(7953901, "Only top-level cell values are supported", emptyPositionInfo(positionInfo));
    // Return the input unchanged.
    return moveFromStack(0);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCellBlockGetFlatValuesBlock(
    ArityType arity) {
    invariant(arity == 1);
    auto [cellOwn, cellTag, cellVal] = getFromStack(0);

    if (cellTag != value::TypeTags::cellBlock) {
        return {false, value::TypeTags::Nothing, 0};
    }
    tassert(7946600, "Cannot process temporary cell values", !cellOwn);

    auto* cell = value::getCellBlock(cellVal);

    return {false,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(&cell->getValueBlock())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockIsMember(
    ArityType arity) {
    auto [valBlockOwned, valBlockTag, valBlockVal] = getFromStack(0);
    auto [arrOwned, arrTag_, arrVal_] = getFromStack(1);

    tassert(8625722,
            "Expected argument to be of valueBlock type",
            valBlockTag == value::TypeTags::valueBlock);
    auto valueBlockView = value::getValueBlock(valBlockVal);

    if (!value::isArray(arrTag_) && arrTag_ != value::TypeTags::inList) {
        auto blockOut = std::make_unique<value::MonoBlock>(
            valueBlockView->count(), value::TypeTags::Nothing, 0);
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
    }

    auto arrTag = arrTag_;
    auto arrVal = arrVal_;

    auto res = [&]() {
        if (arrTag == value::TypeTags::inList) {
            auto inList = value::getInListView(arrVal);

            return valueBlockView->map(value::makeColumnOp<ColumnOpType::kNoFlags>(
                [&](value::TypeTags tag, value::Value val) {
                    return std::pair{value::TypeTags::Boolean,
                                     value::bitcastFrom<bool>(tag != value::TypeTags::Nothing &&
                                                              inList->contains(tag, val))};
                }));
        } else if (arrTag == value::TypeTags::ArraySet) {
            auto arrSet = value::getArraySetView(arrVal);
            auto& values = arrSet->values();

            return valueBlockView->map(value::makeColumnOp<ColumnOpType::kNoFlags>(
                [&](value::TypeTags tag, value::Value val) {
                    return std::pair{
                        value::TypeTags::Boolean,
                        value::bitcastFrom<bool>(values.find({tag, val}) != values.end())};
                }));
        } else {
            value::ValueSetType values(0, value::ValueHash(nullptr), value::ValueEq(nullptr));
            value::arrayForEach(arrTag, arrVal, [&](value::TypeTags elemTag, value::Value elemVal) {
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

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCoerceToBool(
    ArityType arity) {
    auto [valBlockOwned, valBlockTag, valBlockVal] = getFromStack(0);

    tassert(8625723,
            "Expected argument to be of valueBlock type",
            valBlockTag == value::TypeTags::valueBlock);
    auto valueBlockView = value::getValueBlock(valBlockVal);

    auto res = valueBlockView->map(value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) { return value::coerceToBool(tag, val); }));

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockMod(ArityType arity) {
    invariant(arity == 2);
    auto [inputOwned, inputTag, inputVal] = getFromStack(0);

    tassert(8332900,
            "First argument of $mod must be block of values.",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto mod = getFromStack(1);
    if (!value::isNumber(mod.b)) {
        auto nothingBlock =
            std::make_unique<value::MonoBlock>(valueBlockIn->count(), value::TypeTags::Nothing, 0);
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(nothingBlock.release())};
    }

    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> std::pair<value::TypeTags, value::Value> {
            auto [_, resTag, resVal] = genericMod(tag, val, mod.b, mod.c);
            return {resTag, resVal};
        });

    auto res = valueBlockIn->map(cmpOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockConvert(ArityType arity) {
    invariant(arity == 2);
    auto [inputOwned, inputTag, inputVal] = getFromStack(0);

    tassert(8332901,
            "First argument of convert must be block of values.",
            inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [targetOwned, targetTag, targetVal] = getFromStack(1);
    tassert(8907000, "Expected targetTag to be int32", targetTag == value::TypeTags::NumberInt32);
    auto convertTag = static_cast<sbe::value::TypeTags>(value::bitcastTo<int32_t>(targetVal));

    // Numeric convert expects always a numeric type as target. However, it does not check for it
    // and throws if the value is not numeric. We let genericNumConvert do this check and we do not
    // make any checks here.
    const auto cmpOp = value::makeColumnOp<ColumnOpType::kNoFlags>(
        [&](value::TypeTags tag, value::Value val) -> std::pair<value::TypeTags, value::Value> {
            auto [_, resTag, resVal] = value::genericNumConvert(tag, val, convertTag);
            return {resTag, resVal};
        });

    auto res = valueBlockIn->map(cmpOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(res.release())};
}

template <bool IsAscending>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGetSortKey(
    ArityType arity) {
    invariant(arity == 1 || arity == 2);

    CollatorInterface* collator = nullptr;
    if (arity == 2) {
        auto [_, collTag, collVal] = getFromStack(1);
        if (collTag == value::TypeTags::collator) {
            collator = value::getCollatorView(collVal);
        }
    }

    auto [_, blockTag, blockVal] = getFromStack(0);
    tassert(8448716, "Expected argument to be valueBlock", blockTag == value::TypeTags::valueBlock);

    auto block = value::getValueBlock(blockVal);

    if (!block->tryHasArray().get_value_or(true)) {
        // Fast path for non-array case. We just fill any empty values with null.
        std::unique_ptr<value::ValueBlock> filledBlock = block->fillEmpty(value::TypeTags::Null, 0);
        if (!filledBlock) {
            // The block was already dense.
            return moveFromStack(0);
        }
        return {true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(filledBlock.release())};
    }

    auto outBlock = IsAscending ? block->map(getSortKeyAscOp.bindParams(collator))
                                : block->map(getSortKeyDescOp.bindParams(collator));

    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(outBlock.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGetSortKeyAsc(
    ArityType arity) {
    return builtinValueBlockGetSortKey<true /*IsAscending*/>(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGetSortKeyDesc(
    ArityType arity) {
    return builtinValueBlockGetSortKey<false /*IsAscending*/>(arity);
}
}  // namespace mongo::sbe::vm
