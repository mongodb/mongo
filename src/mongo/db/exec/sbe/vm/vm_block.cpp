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
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"

#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe::vm {
using ColumnOpType = value::ColumnOpType;

static constexpr auto existsOpType = ColumnOpType{ColumnOpType::kOutputNonNothingOnExpectedInput,
                                                  value::TypeTags::Nothing,
                                                  value::TypeTags::Boolean,
                                                  ColumnOpType::ReturnBoolOnMissing{}};

static const auto existsOp =
    value::makeColumnOp<existsOpType>([](value::TypeTags tag, value::Value val) {
        return std::pair(value::TypeTags::Boolean,
                         value::bitcastFrom<bool>(tag != value::TypeTags::Nothing));
    });

/*
 * Given a ValueBlock as input, returns a ValueBlock of true/false values indicating whether
 * each value in the input was non-Nothing (true) or Nothing (false).
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockExists(ArityType arity) {
    invariant(arity == 1);
    auto [inputOwned, inputTag, inputVal] = getFromStack(0);

    invariant(inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto out = valueBlockIn->map(existsOp);

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

namespace {

bool allBools(const value::TypeTags* tag, size_t sz) {
    for (size_t i = 0; i < sz; ++i) {
        if (tag[i] != value::TypeTags::Boolean) {
            return false;
        }
    }
    return true;
}

struct FillEmptyFunctor {
    FillEmptyFunctor(value::TypeTags fillTag, value::Value fillVal)
        : _fillTag(fillTag), _fillVal(fillVal) {}

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        if (tag == value::TypeTags::Nothing) {
            return value::copyValue(_fillTag, _fillVal);
        }
        return value::copyValue(tag, val);
    }

    value::TypeTags _fillTag;
    value::Value _fillVal;
};

// Currently have an invariant that prevents the fill value being Nothing, need to change this flag
// if that invariant gets removed.
static constexpr auto fillEmptyOpType = ColumnOpType{ColumnOpType::kOutputNonNothingOnMissingInput,
                                                     value::TypeTags::Nothing,
                                                     value::TypeTags::Nothing,
                                                     ColumnOpType::ReturnNonNothingOnMissing{}};

static const auto fillEmptyOp = value::makeColumnOpWithParams<fillEmptyOpType, FillEmptyFunctor>();
}  // namespace

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
    invariant(blockTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(blockVal);

    auto out = valueBlockIn->map(fillEmptyOp.bindParams(fillTag, fillVal));

    return {
        true, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(out.release())};
}

template <bool less>
FastTuple<bool, value::TypeTags, value::Value> ByteCode::valueBlockMinMaxImpl(
    value::ValueBlock* inputBlock, value::ValueBlock* bitsetBlock) {
    auto block = inputBlock->extract();
    auto bitset = bitsetBlock->extract();

    ValueCompare<less> comp{nullptr /* collator */};

    tassert(8137400, "Expected block and bitset to be the same size", block.count == bitset.count);
    tassert(8137401, "Expected bitset to be all bools", allBools(bitset.tags, bitset.count));

    value::TypeTags accTag = value::TypeTags::Nothing;
    value::Value accVal = 0;
    for (size_t i = 0; i < block.count; ++i) {
        if (value::bitcastTo<bool>(bitset[i].second) && accTag == value::TypeTags::Nothing &&
            block.tags[i] != value::TypeTags::Nothing) {
            accTag = block.tags[i];
            accVal = block.vals[i];
        } else if (value::bitcastTo<bool>(bitset[i].second) &&
                   block.tags[i] != value::TypeTags::Nothing) {
            if (comp({block.tags[i], block.vals[i]}, {accTag, accVal})) {
                accTag = block.tags[i], accVal = block.vals[i];
            }
        }
    }

    auto [retTag, retVal] = value::copyValue(accTag, accVal);
    return {true, retTag, retVal};
}

/*
 * Given a ValueBlock and bitset as input, returns a tag, value pair that contains the minimum value
 * in the block based on compareValue. Values whose corresponding bit is set to false get ignored.
 * This function will return a non-Nothing value if the block contains any non-Nothing values.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockMin(ArityType arity) {
    invariant(arity == 2);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    invariant(inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    invariant(bitsetTag == value::TypeTags::valueBlock);
    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);

    return valueBlockMinMaxImpl<true /* less */>(valueBlockIn, bitsetBlock);
}

/*
 * Given a ValueBlock and bitset as input, returns a tag, value pair that contains the maximum value
 * in the block based on compareValue. Values whose corresponding bit is set to false get ignored.
 * This function will return a non-Nothing value if the block contains any non-Nothing values.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockMax(ArityType arity) {
    invariant(arity == 2);

    auto [inputOwned, inputTag, inputVal] = getFromStack(1);
    invariant(inputTag == value::TypeTags::valueBlock);
    auto* valueBlockIn = value::bitcastTo<value::ValueBlock*>(inputVal);

    auto [bitsetOwned, bitsetTag, bitsetVal] = getFromStack(0);
    invariant(bitsetTag == value::TypeTags::valueBlock);
    auto* bitsetBlock = value::bitcastTo<value::ValueBlock*>(bitsetVal);

    return valueBlockMinMaxImpl<false /* less */>(valueBlockIn, bitsetBlock);
}

/*
 * TODO: Comment.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCount(ArityType arity) {
    MONGO_UNREACHABLE;
}

/*
 * TODO: Comment.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGtScalar(
    ArityType arity) {
    MONGO_UNREACHABLE;
}

/*
 * TODO: Comment.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockGteScalar(
    ArityType arity) {
    MONGO_UNREACHABLE;
}

/*
 * TODO: Comment.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockEqScalar(
    ArityType arity) {
    MONGO_UNREACHABLE;
}

/*
 * TODO: Comment.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLtScalar(
    ArityType arity) {
    MONGO_UNREACHABLE;
}

/*
 * TODO: Comment.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLteScalar(
    ArityType arity) {
    MONGO_UNREACHABLE;
}

/*
 * TODO: Comment.
 */
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockCombine(ArityType arity) {
    MONGO_UNREACHABLE;
}

static constexpr auto invokeLambdaOpType = ColumnOpType{ColumnOpType::kNoFlags,
                                                        value::TypeTags::Nothing,
                                                        value::TypeTags::Nothing,
                                                        ColumnOpType::OnMissingInput{}};

static const auto invokeLambdaOp =
    value::makeColumnOpWithParams<invokeLambdaOpType, ByteCode::InvokeLambdaFunctor>();

/**
 * Implementation of the valueBlockApplyLambda instruction. This instruction takes a mask, a block
 * and an SBE lambda f(), and produces a new block with the result of f() applied to each element of
 * the input for which the mask has, in the same position, a 'true' value.
 * A mask value of Nothing is equivalent to a mask full of 'true' values.
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
                extracted.count == extractedMask.count);
        tassert(8123001,
                "Expected mask to be all bool values",
                allBools(extractedMask.tags, extractedMask.count));

        // Pre-fill with Nothing, and overwrite only the allowed indexes.
        std::vector<value::Value> valueOut(extracted.count);
        std::vector<value::TypeTags> tagOut(extracted.count, value::TypeTags::Nothing);

        ByteCode::InvokeLambdaFunctor invoker(*this, code, lamPos);
        for (size_t i = 0; i < extracted.count; ++i) {
            if (value::bitcastTo<bool>(extractedMask.vals[i])) {
                std::tie(tagOut[i], valueOut[i]) = invoker(extracted.tags[i], extracted.vals[i]);
            }
        }
        outBlock =
            std::make_unique<value::HeterogeneousBlock>(std::move(tagOut), std::move(valueOut));
    } else {
        outBlock = block->map(invokeLambdaOp.bindParams(*this, code, lamPos));
    }

    pushStack(true,
              value::TypeTags::valueBlock,
              value::bitcastFrom<value::ValueBlock*>(outBlock.release()));
}

template <class Op>
std::unique_ptr<value::ValueBlock> applyBoolBinOp(value::ValueBlock* leftBlock,
                                                  value::ValueBlock* rightBlock,
                                                  Op op = {}) {
    auto left = leftBlock->extract();
    auto right = rightBlock->extract();

    tassert(7953531, "Mismatch on size", left.count == right.count);
    // Check that both contain all booleans.
    bool allBool = allBools(left.tags, left.count) && allBools(right.tags, right.count);
    tassert(7953532, "Expected all bool inputs", allBool);

    std::vector<value::Value> boolOut(left.count);
    std::vector<value::TypeTags> tagOut(left.count, value::TypeTags::Boolean);

    for (size_t i = 0; i < left.count; ++i) {
        const auto leftBool = value::bitcastTo<bool>(left.vals[i]);
        const auto rightBool = value::bitcastTo<bool>(right.vals[i]);
        boolOut[i] = value::bitcastFrom<bool>(op(leftBool, rightBool));
    }

    return std::make_unique<value::HeterogeneousBlock>(std::move(tagOut), std::move(boolOut));
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalAnd(
    ArityType arity) {
    invariant(arity == 2);

    auto [leftOwned, leftTag, leftVal] = getFromStack(1);
    invariant(leftTag == value::TypeTags::valueBlock);
    auto* leftValueBlock = value::bitcastTo<value::ValueBlock*>(leftVal);

    auto [rightOwned, rightTag, rightVal] = getFromStack(0);
    invariant(rightTag == value::TypeTags::valueBlock);
    auto* rightValueBlock = value::bitcastTo<value::ValueBlock*>(rightVal);

    auto blockOut = applyBoolBinOp<std::logical_and<>>(leftValueBlock, rightValueBlock);
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValueBlockLogicalOr(
    ArityType arity) {
    invariant(arity == 2);

    auto [leftOwned, leftTag, leftVal] = getFromStack(1);
    invariant(leftTag == value::TypeTags::valueBlock);
    auto* leftValueBlock = value::bitcastTo<value::ValueBlock*>(leftVal);

    auto [rightOwned, rightTag, rightVal] = getFromStack(0);
    invariant(rightTag == value::TypeTags::valueBlock);
    auto* rightValueBlock = value::bitcastTo<value::ValueBlock*>(rightVal);

    auto blockOut = applyBoolBinOp<std::logical_or<>>(leftValueBlock, rightValueBlock);
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCellFoldValues_F(ArityType arity) {
    auto [valBlockOwned, valBlockTag, valBlockVal] = getFromStack(0);
    invariant(valBlockTag == value::TypeTags::valueBlock);
    auto* valueBlock = value::bitcastTo<value::ValueBlock*>(valBlockVal);

    auto [cellOwned, cellTag, cellVal] = getFromStack(1);
    invariant(cellTag == value::TypeTags::cellBlock);
    auto* cellBlock = value::bitcastTo<value::CellBlock*>(cellVal);

    auto valsExtracted = valueBlock->extract();
    tassert(7953533, "Expected all bool inputs", allBools(valsExtracted.tags, valsExtracted.count));

    const auto& positionInfo = cellBlock->filterPositionInfo();
    if (positionInfo.empty()) {
        // Return the input unchanged.
        return moveFromStack(0);
    }

    tassert(7953534,
            "Expected position info count to be same as value size",
            valsExtracted.count == positionInfo.size());
    tassert(7953535, "Unsupported empty block", valsExtracted.count > 0);
    tassert(7953536, "First position info element should always be true", positionInfo[0]);

    // Note: if this code ends up being a bottleneck, we can make some changes. foldCounts()
    // can be initialized based on the number of 1 bits in filterPosInfo. We can also try to
    // make 'folded' and 'foldCounts' use one buffer, rather than two.

    // Represents number of true values in each run.
    std::vector<int> foldCounts(valsExtracted.count, 0);
    int runsSeen = -1;
    for (size_t i = 0; i < valsExtracted.count; ++i) {
        dassert(positionInfo[i] == 1 || positionInfo[i] == 0);
        runsSeen += positionInfo[i];
        foldCounts[runsSeen] += static_cast<int>(value::bitcastTo<bool>(valsExtracted[i].second));
    }

    // The last run is implicitly ended.
    ++runsSeen;

    std::vector<value::Value> folded(runsSeen);
    for (size_t i = 0; i < folded.size(); ++i) {
        folded[i] = value::bitcastFrom<bool>(static_cast<bool>(foldCounts[i]));
    }

    auto blockOut = std::make_unique<value::HeterogeneousBlock>(
        std::vector<value::TypeTags>(folded.size(), value::TypeTags::Boolean), std::move(folded));
    return {true,
            value::TypeTags::valueBlock,
            value::bitcastFrom<value::ValueBlock*>(blockOut.release())};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCellFoldValues_P(ArityType arity) {
    MONGO_UNREACHABLE;
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

}  // namespace mongo::sbe::vm
