/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/block_hashagg.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
namespace {
// Verify that the block is made of booleans, and that it's not completely false.
bool allFalse(const value::DeblockedTagVals& booleanBlock) {
    for (size_t i = 0; i < booleanBlock.count(); i++) {
        tassert(8573802,
                "Bitmap used in aggregator must be of all boolean values",
                booleanBlock.tags()[i] == value::TypeTags::Boolean);
    }
    for (size_t i = 0; i < booleanBlock.count(); i++) {
        if (value::bitcastTo<bool>(booleanBlock.vals()[i])) {
            return false;
        }
    }
    return true;
}

struct SpanHasher {
    SpanHasher() {}

    size_t operator()(const std::span<size_t>& idxs) const {
        return boost::hash_range(idxs.begin(), idxs.end());
    }
};

struct SpanEq {
    SpanEq() {}

    bool operator()(const std::span<size_t>& lhs, const std::span<size_t>& rhs) const {
        invariant(lhs.size() == rhs.size());

        for (size_t i = 0; i < lhs.size(); ++i) {
            if (lhs[i] != rhs[i]) {
                return false;
            }
        }

        return true;
    }
};

// Map from <row of tokens> ---> finalToken
using TokenizeTableType = stdx::unordered_map<std::span<size_t>, size_t, SpanHasher, SpanEq>;

}  // namespace

BlockHashAggStage::BlockHashAggStage(std::unique_ptr<PlanStage> input,
                                     value::SlotVector groupSlotIds,
                                     value::SlotId blockBitsetInSlotId,
                                     value::SlotVector blockDataInSlotIds,
                                     value::SlotVector accumulatorDataSlotIds,
                                     value::SlotId accumulatorBitsetSlotId,
                                     BlockAggExprTupleVector aggs,
                                     bool allowDiskUse,
                                     SlotExprPairVector mergingExprs,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId planNodeId,
                                     bool participateInTrialRunTracking,
                                     bool forceIncreasedSpilling)
    : HashAggBaseStage("block_group"_sd,
                       yieldPolicy,
                       planNodeId,
                       nullptr,
                       participateInTrialRunTracking,
                       allowDiskUse,
                       forceIncreasedSpilling),
      _groupSlots(groupSlotIds),
      _blockBitsetInSlotId(blockBitsetInSlotId),
      _blockDataInSlotIds(std::move(blockDataInSlotIds)),
      _accumulatorBitsetSlotId(accumulatorBitsetSlotId),
      _accumulatorDataSlotIds(std::move(accumulatorDataSlotIds)),
      _aggs(std::move(aggs)),
      _mergingExprs(std::move(mergingExprs)) {
    tassert(8780600,
            "Expected 'blockDataInSlotIds' and 'accumulatorDataSlotIds' to have the same size",
            _blockDataInSlotIds.size() == _accumulatorDataSlotIds.size());

    _children.emplace_back(std::move(input));

    _outIdBlocks.resize(_groupSlots.size());
    _outAggBlocks.resize(_aggs.size());
    _blockDataInAccessors.resize(_blockDataInSlotIds.size());
    _accumulatorDataAccessors.resize(_accumulatorDataSlotIds.size());
    _gbBlocks.resize(_groupSlots.size());
    _dataBlocks.resize(_blockDataInSlotIds.size());
    _deblockedTokens.resize(_groupSlots.size());

    _tokenInfos.reserve(_groupSlots.size());

    if (_allowDiskUse) {
        tassert(
            8780601,
            "Disk use enabled for BlockHashAggStage but incorrect number of merging expressions",
            _aggs.size() == _mergingExprs.size());
    }
}

std::unique_ptr<PlanStage> BlockHashAggStage::clone() const {
    BlockAggExprTupleVector blockRowAggs;
    for (const auto& [slot, aggTuple] : _aggs) {
        std::unique_ptr<sbe::EExpression> init;
        std::unique_ptr<sbe::EExpression> blockAgg;
        std::unique_ptr<sbe::EExpression> agg = aggTuple.agg->clone();

        if (aggTuple.init) {
            init = aggTuple.init->clone();
        }
        if (aggTuple.blockAgg) {
            blockAgg = aggTuple.blockAgg->clone();
        }

        auto clonedAggTuple =
            BlockAggExprTuple{std::move(init), std::move(blockAgg), std::move(agg)};

        blockRowAggs.emplace_back(slot, std::move(clonedAggTuple));
    }

    SlotExprPairVector mergingExprs;
    mergingExprs.reserve(_mergingExprs.size());

    for (auto&& [k, v] : _mergingExprs) {
        mergingExprs.push_back({k, v->clone()});
    }

    return std::make_unique<BlockHashAggStage>(_children[0]->clone(),
                                               _groupSlots,
                                               _blockBitsetInSlotId,
                                               _blockDataInSlotIds,
                                               _accumulatorDataSlotIds,
                                               _accumulatorBitsetSlotId,
                                               std::move(blockRowAggs),
                                               _allowDiskUse,
                                               std::move(mergingExprs),
                                               _yieldPolicy,
                                               _commonStats.nodeId,
                                               participateInTrialRunTracking(),
                                               _forceIncreasedSpilling);
}

void BlockHashAggStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    value::SlotSet dupCheck;
    auto throwIfDupSlot = [&dupCheck](boost::optional<value::SlotId> slot) {
        if (slot) {
            auto [_, inserted] = dupCheck.emplace(*slot);
            tassert(7953400, "duplicate slot id", inserted);
        }
    };

    _blockBitsetInAccessor = _children[0]->getAccessor(ctx, _blockBitsetInSlotId);
    invariant(_blockBitsetInAccessor);

    for (size_t i = 0; i < _blockDataInSlotIds.size(); i++) {
        _blockDataInAccessors[i] = _children[0]->getAccessor(ctx, _blockDataInSlotIds[i]);
        invariant(_blockDataInAccessors[i]);
    }
    throwIfDupSlot(_blockBitsetInSlotId);

    _outAccessorsMap.reserve(_groupSlots.size() + _aggs.size());
    _outIdBlockAccessors.resize(_groupSlots.size());
    _outAggBlockAccessors.resize(_aggs.size());

    for (size_t i = 0; i < _groupSlots.size(); ++i) {
        auto& slot = _groupSlots[i];
        throwIfDupSlot(slot);

        _idInAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));

        // Construct accessor for obtaining the key values from the hash table.
        _idHtAccessors.emplace_back(std::make_unique<HashKeyAccessor>(_htIt, i));

        _outAccessorsMap[slot] = &_outIdBlockAccessors[i];
    }

    // Change the agg slot accessors to point to the blocks.
    for (size_t i = 0; i < _outAggBlocks.size(); ++i) {
        auto& outBlock = _outAggBlocks[i];
        _outAggBlockAccessors[i].reset(
            false, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(&outBlock));
    }

    for (size_t i = 0; i < _aggs.size(); ++i) {
        auto& [slot, _] = _aggs[i];
        throwIfDupSlot(slot);

        _rowAggHtAccessors.emplace_back(std::make_unique<HashAggAccessor>(_htIt, i));
        _rowAggRSAccessors.emplace_back(
            std::make_unique<value::MaterializedSingleRowAccessor>(_outAggRowRecordStore, i));
        _rowAggAccessors.emplace_back(
            std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                _rowAggHtAccessors.back().get(), _rowAggRSAccessors.back().get()}));
        _outAccessorsMap[slot] = &_outAggBlockAccessors[i];

        if (_allowDiskUse) {
            value::SlotId spillSlot = _mergingExprs[i].first;
            throwIfDupSlot(spillSlot);

            _spilledAccessors.push_back(
                std::make_unique<value::MaterializedSingleRowAccessor>(_spilledAggRow, i));

            _spilledAccessorMap[spillSlot] = _spilledAccessors[i].get();
        }
    }

    // Initialize '_accumulatorDataAccessorMap'.
    for (size_t i = 0; i < _accumulatorDataSlotIds.size(); ++i) {
        auto slot = _accumulatorDataSlotIds[i];
        throwIfDupSlot(slot);

        _accumulatorDataAccessorMap[slot] = &_accumulatorDataAccessors[i];
    }

    // Compile 'blockAggs', 'rowAggs', and 'mergingExprs'.
    for (size_t i = 0; i < _aggs.size(); ++i) {
        auto& [_, aggTuple] = _aggs[i];

        ctx.root = this;

        std::unique_ptr<vm::CodeFragment> initCode;
        if (aggTuple.init) {
            initCode = aggTuple.init->compile(ctx);
        }
        _initCodes.emplace_back(std::move(initCode));

        ctx.aggExpression = true;
        ctx.accumulator = _rowAggAccessors[i].get();

        std::unique_ptr<vm::CodeFragment> blockAggCode;
        if (aggTuple.blockAgg) {
            blockAggCode = aggTuple.blockAgg->compile(ctx);
        }
        _blockAggCodes.emplace_back(std::move(blockAggCode));

        _aggCodes.emplace_back(aggTuple.agg->compile(ctx));

        if (_allowDiskUse) {
            std::unique_ptr<EExpression>& mergingExpr = _mergingExprs[i].second;
            _mergingExprCodes.emplace_back(mergingExpr->compile(ctx));
        }

        ctx.aggExpression = false;
    }

    _compiled = true;

    _memoryTracker = OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(
        _opCtx, loadMemoryLimit(StageMemoryLimit::QuerySBEAggApproxMemoryUseInBytesBeforeSpill));
}

value::SlotAccessor* BlockHashAggStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (slot == _blockBitsetInSlotId) {
        // Re-map the bitset slot to our output bitset accessor.
        return &_blockBitsetOutAccessor;
    }

    if (_compiled) {
        if (_outAccessorsMap.count(slot)) {
            return _outAccessorsMap[slot];
        }
    } else {
        if (_accumulatorBitsetSlotId == slot) {
            return &_accumulatorBitsetAccessor;
        }
        if (auto it = _accumulatorDataAccessorMap.find(slot);
            it != _accumulatorDataAccessorMap.end()) {
            return it->second;
        }
        if (auto it = _spilledAccessorMap.find(slot); it != _spilledAccessorMap.end()) {
            return it->second;
        }
    }

    return _children[0]->getAccessor(ctx, slot);
}

void BlockHashAggStage::executeBlockLevelAccumulatorCode(const value::MaterializedRow& key) {
    // Track how many times we invoke the block accumulators.
    _specificStats.blockAccumulatorTotalCalls++;

    _htIt = _ht->find(key);
    if (_htIt == _ht->end()) {
        // New key we haven't seen before.
        value::MaterializedRow ownedKey = key;
        ownedKey.makeOwned();

        auto [it, _] = _ht->emplace(std::move(ownedKey), value::MaterializedRow{0});
        // Initialize accumulators.
        it->second.resize(_rowAggAccessors.size());
        _htIt = it;

        // Run accumulator initializers if needed.
        for (size_t i = 0; i < _initCodes.size(); ++i) {
            if (_initCodes[i]) {
                auto [owned, tag, val] = _bytecode.run(_initCodes[i].get());
                _rowAggHtAccessors[i]->reset(owned, tag, val);
            }
        }
    }

    // Run the block level accumulators.
    for (size_t i = 0; i < _blockAggCodes.size(); ++i) {
        auto [owned, tag, val] = _bytecode.run(_blockAggCodes[i].get());
        _rowAggHtAccessors[i]->reset(owned, tag, val);
    }
}

void BlockHashAggStage::executeRowLevelAccumulatorCode(
    const value::DeblockedTagVals& extractedBitmap,
    const std::vector<value::DeblockedTagVals>& extractedGbs,
    const std::vector<value::DeblockedTagVals>& extractedData) {

    boost::optional<value::MaterializedRow> key;

    for (size_t blockIndex = 0; blockIndex < _currentBlockSize; ++blockIndex) {
        auto [bitTag, bitVal] = extractedBitmap[blockIndex];
        invariant(bitTag == value::TypeTags::Boolean);

        if (!value::bitcastTo<bool>(bitVal)) {
            continue;
        }

        if (!key) {
            key.emplace(extractedGbs.size());
        }

        for (size_t i = 0; i < extractedGbs.size(); ++i) {
            auto [idTag, idVal] = extractedGbs[i][blockIndex];
            key->reset(i, false, idTag, idVal);
        }

        // Set '_htIt' to point to the entry for 'key' in '_ht'.
        _htIt = _ht->find(*key);

        if (_htIt == _ht->end()) {
            // New key we haven't seen before.
            key->makeOwned();
            auto [it, _] = _ht->emplace(std::move(*key), value::MaterializedRow{0});
            key = boost::none;

            // Initialize accumulators.
            it->second.resize(_rowAggAccessors.size());
            _htIt = it;

            // Run accumulator initializers if needed.
            for (size_t i = 0; i < _initCodes.size(); ++i) {
                if (_initCodes[i]) {
                    auto [owned, tag, val] = _bytecode.run(_initCodes[i].get());
                    _rowAggHtAccessors[i]->reset(owned, tag, val);
                }
            }
        }

        // Set '_accumulatorDataAccessors' to the input values for the current 'blockIndex'.
        for (size_t i = 0; i < extractedData.size(); ++i) {
            auto [tag, val] = extractedData[i][blockIndex];
            _accumulatorDataAccessors[i].reset(tag, val);
        }

        // Run each row level accumulator.
        for (size_t i = 0; i < _aggCodes.size(); ++i) {
            auto [rowOwned, rowTag, rowVal] = _bytecode.run(_aggCodes[i].get());
            _rowAggHtAccessors[i]->reset(rowOwned, rowTag, rowVal);
        }
    }
}

void BlockHashAggStage::runAccumulatorsTokenized(const TokenizedKeys& tokenizedKeys,
                                                 const value::DeblockedTagVals& inBitmap) {
    // We're using the block-based accumulator, so increment the corresponding metric.
    _specificStats.blockAccumulations++;
    tassert(8573801,
            "Bitmap used in aggregator must be of the same size of the tokenized keys",
            inBitmap.count() == tokenizedKeys.idxs.size());
    auto [bitmapInTag, bitmapInVal] = _blockBitsetInAccessor->getViewOfValue();

    // Set '_accumulatorDataAccessors' to the input value blocks.
    for (size_t i = 0; i < _dataBlocks.size(); ++i) {
        _accumulatorDataAccessors[i].reset(value::TypeTags::valueBlock,
                                           value::bitcastFrom<value::ValueBlock*>(_dataBlocks[i]));
    }

    value::BoolBlock bitmask;
    // Process the accumulators for each partition rather than one element at a time.
    for (size_t partition = 0; partition < tokenizedKeys.keys.size(); ++partition) {
        // The accumulators use `_accumulatorBitsetAccessor` to determine which values to
        // accumulate. If we have multiple partitions, we need some additional logic to
        // indicate which partition we're processing.
        if (tokenizedKeys.keys.size() > 1) {
            // Given a vector of partition IDs, and partition ID, create a bitset indicating whether
            // each element in the vector matches the given partition ID, and apply on top of it the
            // mask provided in input.
            bitmask.clear();
            bitmask.reserve(tokenizedKeys.idxs.size());

            for (size_t i = 0; i < tokenizedKeys.idxs.size(); i++) {
                bitmask.push_back(tokenizedKeys.idxs[i] == partition &&
                                  value::bitcastTo<bool>(inBitmap.vals()[i]));
            }
            // If all bits are false, there's no work to do. We don't want to make an erroneous
            // entry in our hash map.
            if (bitmask.allFalse().get_value_or(false)) {
                continue;
            }

            _accumulatorBitsetAccessor.reset(false,
                                             value::TypeTags::valueBlock,
                                             value::bitcastFrom<value::ValueBlock*>(&bitmask));
        } else {
            // The partition bitmap would be all 1s if we computed it, so we can just use
            // the input bitmap in this case, that we already checked for not being made
            // by all False values.
            _accumulatorBitsetAccessor.reset(false, bitmapInTag, bitmapInVal);
        }

        executeBlockLevelAccumulatorCode(tokenizedKeys.keys[partition]);
    }
    // Avoid leaving a pointer to inaccessible memory in the accessor.
    _accumulatorBitsetAccessor.reset(false, value::TypeTags::Nothing, 0);
}

void BlockHashAggStage::runAccumulatorsElementWise(const value::DeblockedTagVals& extractedBitmap) {
    // We're using the element-wise accumulator, so increment the corresponding metric.
    _specificStats.elementWiseAccumulations++;

    // Extract the group bys.
    std::vector<value::DeblockedTagVals> extractedGbs;
    extractedGbs.resize(_gbBlocks.size());
    for (size_t i = 0; i < _gbBlocks.size(); ++i) {
        extractedGbs[i] = _gbBlocks[i]->extract();
    }

    // Extract each data block into this array for when we process them element-wise.
    std::vector<value::DeblockedTagVals> extractedData;
    extractedData.resize(_dataBlocks.size());
    for (size_t i = 0; i < _dataBlocks.size(); i++) {
        extractedData[i] = _dataBlocks[i]->extract();
    }

    // Call executeRowLevelAccumulatorCode() to run the row accumulators.
    executeRowLevelAccumulatorCode(extractedBitmap, extractedGbs, extractedData);

    _accumulatorBitsetAccessor.reset(false, value::TypeTags::Nothing, 0);
}

boost::optional<std::vector<size_t>> BlockHashAggStage::tokenizeTokenInfos(
    const std::vector<value::TokenizedBlock>& tokenInfos) {
    invariant(!tokenInfos.empty());

    // If any individual ID block is high enough partition, we know the combined output will also be
    // high partition. We can return early in this case.
    for (const auto& tokenInfo : tokenInfos) {
        if (tokenInfo.tokens->count() > kMaxNumPartitionsForTokenizedPath) {
            return boost::none;
        }
    }

    // compoundKeys is a blockSize x numBlocks (row x column) vector.
    const size_t blockSize = tokenInfos[0].idxs.size();
    const size_t numBlocks = tokenInfos.size();

    // If we have one input to tokenize, there's no additional work to do. We can save the work of
    // creating and filling a hash table.
    if (numBlocks == 1) {
        // We don't have to worry about having high partition IDs (and returning boost::none)
        // because the check above would have bailed out in that case.
        return tokenInfos[0].idxs;
    }

    _compoundKeys.resize(blockSize * numBlocks, 0);

    // All input blocks must be the same size, enforced by an invariant in open().
    size_t ckIdx = 0;
    for (size_t blockIdx = 0; blockIdx < blockSize; ++blockIdx) {
        for (size_t keyIdx = 0; keyIdx < numBlocks; ++keyIdx) {
            _compoundKeys[ckIdx++] = tokenInfos[keyIdx].idxs[blockIdx];
        }
    }

    auto keyMap = TokenizeTableType();
    size_t uniqueCount = 0;
    std::vector<size_t> idxs(blockSize, 0);
    for (size_t blockIdx = 0; blockIdx < blockSize; ++blockIdx) {
        std::span<size_t> htKey{&_compoundKeys[blockIdx * numBlocks], numBlocks};
        auto [it, inserted] = keyMap.emplace(htKey, uniqueCount);
        if (inserted) {
            uniqueCount++;

            if (uniqueCount > kMaxNumPartitionsForTokenizedPath) {
                // We've seen more "partitions" for this block than we are willing to process in
                // the Tokenized path, so we will exit early and run the accumulators element
                // wise.
                return boost::none;
            }
        }
        idxs[blockIdx] = it->second;
    }

    return idxs;
}

boost::optional<BlockHashAggStage::TokenizedKeys> BlockHashAggStage::tryTokenizeGbs() {
    // First separate which gbBlocks are mono blocks and which are not.
    std::vector<value::ValueBlock*> nonMonoGbBlocks;
    nonMonoGbBlocks.reserve(_gbBlocks.size());
    std::vector<value::MonoBlock*> monoGbBlocks;
    monoGbBlocks.reserve(_gbBlocks.size());

    std::vector<bool> isMonoBlock(_gbBlocks.size());
    {
        size_t i = 0;
        for (auto* gbBlock : _gbBlocks) {
            if (auto monoBlock = gbBlock->as<value::MonoBlock>()) {
                isMonoBlock[i] = true;
                monoGbBlocks.push_back(monoBlock);
            } else {
                isMonoBlock[i] = false;
                nonMonoGbBlocks.push_back(gbBlock);
            }
            ++i;
        }
    }

    TokenizedKeys out;

    // Special case: we have only mono blocks.
    if (nonMonoGbBlocks.empty()) {
        value::MaterializedRow key{monoGbBlocks.size()};

        // Go over each mono block and produce the output manually.
        size_t idx = 0;
        for (auto* mb : monoGbBlocks) {
            key.reset(idx++, false, mb->getTag(), mb->getValue());
        }

        out.keys.push_back(std::move(key));
        out.idxs.resize(_currentBlockSize, 0);
        return out;
    }

    // Populate '_tokenInfos' and '_deblockedTokens'.
    _tokenInfos.clear();
    _deblockedTokens.clear();
    for (size_t i = 0; i < nonMonoGbBlocks.size(); ++i) {
        _tokenInfos.push_back(nonMonoGbBlocks[i]->tokenize());

        tassert(8608600,
                "All input blocks must be the same size",
                _tokenInfos[i].idxs.size() == _currentBlockSize);
    }

    // Combine the TokenizedBlocks for each input key, combine them into compound keys, tokenize
    // these compound keys, and then return the result.
    boost::optional<std::vector<size_t>> optFinalTokens = tokenizeTokenInfos(_tokenInfos);
    if (!optFinalTokens) {
        // Too many tokens, use the row path.
        return boost::none;
    }

    // Now extract everything.
    for (size_t i = 0; i < nonMonoGbBlocks.size(); ++i) {
        _deblockedTokens.push_back(_tokenInfos[i].tokens->extract());
    }

    const std::vector<size_t>& finalTokens = *optFinalTokens;
    // Now we convert our list of final tokens [1,2,3, 1, 4, ...] into a list of actual keys that
    // can be put into the hash table.
    {
        size_t nextTokenIdToAdd = 0;
        for (size_t i = 0; i < finalTokens.size(); ++i) {
            const size_t tokenId = finalTokens[i];
            tassert(8573900,
                    "Expected next tokenId to be less than or equal to the current maximum tokenId "
                    "plus one",
                    tokenId <= nextTokenIdToAdd);

            // We've found a token that we haven't yet constructed a key for.
            if (nextTokenIdToAdd == tokenId) {
                // Construct the GB index for this key.
                value::MaterializedRow key{_gbBlocks.size()};
                size_t monoBlockIdx = 0;
                size_t nonMonoBlockIdx = 0;
                tassert(8848300,
                        "Expected deblocked tokens to be correct size",
                        _deblockedTokens.size() == nonMonoGbBlocks.size());

                for (size_t keyIdx = 0; keyIdx < _gbBlocks.size(); ++keyIdx) {
                    if (isMonoBlock[keyIdx]) {
                        auto* monoBlock = monoGbBlocks[monoBlockIdx];
                        key.reset(keyIdx, false, monoBlock->getTag(), monoBlock->getValue());
                        ++monoBlockIdx;
                    } else {
                        const size_t originalTokenId = _tokenInfos[nonMonoBlockIdx].idxs[i];
                        dassert(originalTokenId < _deblockedTokens[nonMonoBlockIdx].count());
                        auto [t, v] = _deblockedTokens[nonMonoBlockIdx][originalTokenId];
                        key.reset(keyIdx, false, t, v);
                        ++nonMonoBlockIdx;
                    }
                }
                out.keys.push_back(std::move(key));
                ++nextTokenIdToAdd;
            }
        }
    }

    // Do not access 'finalTokens' again.
    out.idxs = std::move(*optFinalTokens);
    return out;
}

void BlockHashAggStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    _children[0]->open(reOpen);
    _commonStats.opens++;

    _ht.emplace();

    for (auto& aggAccessor : _rowAggAccessors) {
        aggAccessor->setIndex(0);
    }
    if (_recordStore) {
        _recordStore->resetCursor(_opCtx, _rsCursor);
    }
    _recordStore.reset();
    _outKeyRowRecordStore = {0};
    _outAggRowRecordStore = {0};
    _spilledAggRow = {0};
    _stashedNextRow = boost::none;

    if (reOpen) {
        _done = false;
    }

    MemoryCheckData memoryCheckData;

    while (PlanState::ADVANCED == _children[0]->getNext()) {
        // Update '_bitmapBlock' and '_currentBlockSize'.
        auto [bitmapInTag, bitmapInVal] = _blockBitsetInAccessor->getViewOfValue();
        invariant(bitmapInTag == value::TypeTags::valueBlock);

        _bitmapBlock = value::bitcastTo<value::ValueBlock*>(bitmapInVal);
        _currentBlockSize = _bitmapBlock->count();

        // Update '_gbBlocks' and '_dataBlocks'.
        for (size_t i = 0; i < _idInAccessors.size(); ++i) {
            auto [tag, val] = _idInAccessors[i]->getViewOfValue();
            _gbBlocks[i] = tag == value::TypeTags::valueBlock ? value::getValueBlock(val)
                                                              : makeMonoBlock(tag, val);
        }

        for (size_t i = 0; i < _blockDataInAccessors.size(); i++) {
            auto [tag, val] = _blockDataInAccessors[i]->getViewOfValue();
            _dataBlocks[i] = tag == value::TypeTags::valueBlock ? value::getValueBlock(val)
                                                                : makeMonoBlock(tag, val);
        }

        // If the bitset has any ones in it, return the deblocked data so we can accumulate.
        auto maybeDeblockedBitmap = [&]() -> boost::optional<value::DeblockedTagVals> {
            // Check the fast path first.
            if (_bitmapBlock->allFalse().get_value_or(false)) {
                return boost::none;
            }
            // Use the slower extract() path. If there are any active bits, return the deblocked
            // data.
            value::DeblockedTagVals deblockedBitmap = _bitmapBlock->extract();
            if (allFalse(deblockedBitmap)) {
                return boost::none;
            }
            return deblockedBitmap;
        }();

        // If the mask is all false we can avoid tokenization and running the accumulators.
        if (maybeDeblockedBitmap) {
            // Try to generate tokenized group-by keys.
            boost::optional<BlockHashAggStage::TokenizedKeys> tokenizedKeys = tryTokenizeGbs();
            if (tokenizedKeys) {
                // If we generated tokenized group-by keys successfully, call
                // runAccumulatorsTokenized() to run the block-level accumulators.
                runAccumulatorsTokenized(*tokenizedKeys, *maybeDeblockedBitmap);
            } else {
                // If tryTokenizeGbs() returned boost::none, call runAccumulatorsElementWise() to
                // deblock everything and run the row-level accumulators.
                runAccumulatorsElementWise(*maybeDeblockedBitmap);
            }
        }

        if (!_ht->empty()) {
            if (_forceIncreasedSpilling) {
                // Spill for every row that appears in the hash table.
                _htIt = _ht->begin();
                spill(memoryCheckData);
            } else {
                // Estimates how much memory is being used. If we estimate that the hash table
                // exceeds the allotted memory budget, its contents are spilled to the
                // '_recordStore' and '_ht' is cleared.
                checkMemoryUsageAndSpillIfNecessary(memoryCheckData);
            }
        }

        // We no longer need any of the TokenizedBlocks or MonoBlocks that we created during this
        // iteration, so we can discard them now.
        _tokenInfos.clear();
        _monoBlocks.clear();
    }

    // If we spilled at any point while consuming the input, then do one final spill to write
    // any leftover contents of '_ht' to the record store. That way, when recovering the input
    // from the record store and merging partial aggregates we don't have to worry about the
    // possibility of some of the data being in the hash table and some being in the record
    // store.
    if (_recordStore) {
        if (!_ht->empty()) {
            _htIt = _ht->begin();
            spill(memoryCheckData);
        }

        switchToDisk();
    }

    _accumulatorBitsetAccessor.reset(false, value::TypeTags::Nothing, 0);
    _htIt = _ht->end();
}

bool BlockHashAggStage::getNextSpilledHelper() {
    auto recoverSpilledRecord = [&](const Record& record, BufBuilder& keyBuffer) {
        return deserializeSpilledRecord(record, _groupSlots.size(), keyBuffer);
    };

    // If we have a stashed row from last time, use that first. Otherwise ask the record store for
    // the next value and process all the data for that key.
    if (!_stashedNextRow) {
        auto nextRecord = _rsCursor->next();
        if (!nextRecord) {
            return false;
        }

        // We are just starting the process of merging the spilled file segments.
        auto firstRecoveredRow = recoverSpilledRecord(*nextRecord, _currentBuffer);

        _outKeyRowRecordStore = std::move(firstRecoveredRow.first);
        _outAggRowRecordStore = std::move(firstRecoveredRow.second);
    } else {
        _currentBuffer = std::move(_stashedBuffer);
        _outKeyRowRecordStore = std::move(_stashedNextRow->first);
        _outAggRowRecordStore = std::move(_stashedNextRow->second);
        _stashedNextRow = boost::none;
    }

    // Find additional partial aggregates for the same key and merge them in order to compute the
    // final output.
    for (auto nextRecord = _rsCursor->next(); nextRecord; nextRecord = _rsCursor->next()) {
        auto recoveredRow = recoverSpilledRecord(*nextRecord, _stashedBuffer);
        // If we found a different key, then we're done accumulating the current key. Since there's
        // no peek API, we have to stash `recoveredRow` for next time.
        if (!value::MaterializedRowEq()(recoveredRow.first, _outKeyRowRecordStore)) {
            _stashedNextRow = std::move(recoveredRow);
            return true;
        }

        // Merge in the new partial aggregate values.
        _spilledAggRow = std::move(recoveredRow.second);
        for (size_t idx = 0; idx < _mergingExprCodes.size(); ++idx) {
            auto [rowOwned, rowTag, rowVal] = _bytecode.run(_mergingExprCodes[idx].get());
            _rowAggRSAccessors[idx]->reset(rowOwned, rowTag, rowVal);
        }
    }

    return true;
}

PlanState BlockHashAggStage::getNextSpilled() {
    size_t resultIdx = 0;
    for (; resultIdx < kBlockOutSize; resultIdx++) {
        bool hasNextKey = getNextSpilledHelper();
        // If we have a key, add the value to our result. If not, break because we won't get anymore
        // values from the record store.
        if (hasNextKey) {
            invariant(_outKeyRowRecordStore.size() == _outIdBlocks.size());
            for (size_t i = 0; i < _outKeyRowRecordStore.size(); i++) {
                auto [keyComponentTag, keyComponentVal] = _outKeyRowRecordStore.getViewOfValue(i);
                _outIdBlocks[i].push_back(value::copyValue(keyComponentTag, keyComponentVal));
            }
            for (size_t i = 0; i < _outAggBlocks.size(); ++i) {
                auto [accTag, accVal] = _rowAggRSAccessors[i]->getViewOfValue();
                _outAggBlocks[i].push_back(value::copyValue(accTag, accVal));
            }
        } else {
            break;
        }
    }

    // If we didn't put any new values in the blocks, we must have no more spilled values.
    if (resultIdx == 0) {
        return trackPlanState(PlanState::IS_EOF);
    }
    populateBitmapSlot(resultIdx);
    return trackPlanState(PlanState::ADVANCED);
}

PlanState BlockHashAggStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    size_t idx = 0;
    for (auto& b : _outIdBlocks) {
        b.clear();
        b.reserve(kBlockOutSize);
        _outIdBlockAccessors[idx++].reset(
            false, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(&b));
    }

    for (auto& b : _outAggBlocks) {
        b.clear();
        b.reserve(kBlockOutSize);
    }

    // If we've spilled, then we need to produce the output by merging the spilled segments from the
    // spill file.
    if (_recordStore) {
        return getNextSpilled();
    }

    // When we return, populate our bitmap slot with an block of all 1s, with size equal to the
    // number of rows in the block we produce.
    size_t numRows = 0;
    ON_BLOCK_EXIT([&]() { populateBitmapSlot(numRows); });

    while (numRows < kBlockOutSize) {
        setIteratorToNextRecord();

        if (_done) {
            return trackPlanState(PlanState::IS_EOF);
        }

        if (_htIt == _ht->end()) {
            // All records have been processed.
            _ht->clear();
            _htIt = _ht->end();
            _done = true;
            if (numRows == 0) {
                return trackPlanState(PlanState::IS_EOF);
            } else {
                return trackPlanState(PlanState::ADVANCED);
            }
        }

        invariant(_outAggBlocks.size() == _outAggBlockAccessors.size());
        invariant(_outAggBlocks.size() == _rowAggAccessors.size());

        // Copy the key from the current element in the HT into the out blocks.
        idx = 0;
        for (auto& idHtAccessor : _idHtAccessors) {
            auto [t, v] = idHtAccessor->copyOrMoveValue();
            _outIdBlocks[idx++].push_back(t, v);
        }

        // Copy the values from the current element in the HT into the out block.
        idx = 0;
        for (auto& rowAggHtAccessor : _rowAggHtAccessors) {
            auto [t, v] = rowAggHtAccessor->copyOrMoveValue();
            _outAggBlocks[idx++].push_back(t, v);
        }

        ++numRows;
    }

    return trackPlanState(PlanState::ADVANCED);
}

std::unique_ptr<PlanStageStats> BlockHashAggStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<HashAggStats>(_specificStats);

    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob;

        bob.append("groupBySlots", _groupSlots.begin(), _groupSlots.end());

        {
            BSONObjBuilder blockExprBob(bob.subobjStart("blockExpressions"));
            for (auto&& [slot, aggTuple] : _aggs) {
                blockExprBob.append(str::stream() << slot,
                                    printer.print(aggTuple.blockAgg->debugPrint()));
            }
        }

        {
            BSONObjBuilder rowExprBob(bob.subobjStart("rowExpressions"));
            for (auto&& [slot, aggTuple] : _aggs) {
                rowExprBob.append(str::stream() << slot, printer.print(aggTuple.agg->debugPrint()));
            }
        }

        {
            BSONObjBuilder initExprBuilder(bob.subobjStart("initExprs"));
            for (auto&& [slot, aggTuple] : _aggs) {
                if (aggTuple.init) {
                    initExprBuilder.append(str::stream() << slot,
                                           printer.print(aggTuple.init->debugPrint()));
                } else {
                    initExprBuilder.appendNull(str::stream() << slot);
                }
            }
        }

        bob.append("blockDataInSlots", _blockDataInSlotIds.begin(), _blockDataInSlotIds.end());

        bob.append(
            "accumulatorDataSlots", _accumulatorDataSlotIds.begin(), _accumulatorDataSlotIds.end());

        if (!_mergingExprs.empty()) {
            BSONObjBuilder nestedBuilder{bob.subobjStart("mergingExprs")};
            for (auto&& [slot, expr] : _mergingExprs) {
                nestedBuilder.append(str::stream() << slot, printer.print(expr->debugPrint()));
            }
        }

        // Spilling stats.
        bob.appendBool("usedDisk", _specificStats.usedDisk);
        bob.appendNumber("spills",
                         static_cast<long long>(_specificStats.spillingStats.getSpills()));
        bob.appendNumber("spilledBytes",
                         static_cast<long long>(_specificStats.spillingStats.getSpilledBytes()));
        bob.appendNumber("spilledRecords",
                         static_cast<long long>(_specificStats.spillingStats.getSpilledRecords()));
        bob.appendNumber(
            "spilledDataStorageSize",
            static_cast<long long>(_specificStats.spillingStats.getSpilledDataStorageSize()));

        // Block-specific stats.
        bob.appendNumber("blockAccumulations", _specificStats.blockAccumulations);
        bob.appendNumber("blockAccumulatorTotalCalls", _specificStats.blockAccumulatorTotalCalls);
        bob.appendNumber("elementWiseAccumulations", _specificStats.elementWiseAccumulations);

        if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
            bob.appendNumber("peakTrackedMemBytes",
                             static_cast<long long>(_specificStats.peakTrackedMemBytes));
        }

        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* BlockHashAggStage::getSpecificStats() const {
    return &_specificStats;
}

BlockHashAggStats* BlockHashAggStage::getHashAggStats() {
    return &_specificStats;
}

void BlockHashAggStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();

    _ht = boost::none;
    if (_recordStore && _opCtx) {
        _recordStore->resetCursor(_opCtx, _rsCursor);
    }
    _rsCursor.reset();
    _recordStore.reset();
    _outKeyRowRecordStore = {0};
    _outAggRowRecordStore = {0};
    _spilledAggRow = {0};
    _stashedNextRow = boost::none;

    _currentBlockSize = 0;
    _bitmapBlock = nullptr;
    _tokenInfos.clear();
    _monoBlocks.clear();

    _children[0]->close();

    _specificStats.peakTrackedMemBytes = _memoryTracker.value().peakTrackedMemoryBytes();
    _memoryTracker.value().set(0);
}

std::vector<DebugPrinter::Block> BlockHashAggStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("bitset ="));
    DebugPrinter::addIdentifier(ret, _blockBitsetInSlotId);

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _groupSlots.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _groupSlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    // Print the row-level and block-level accessors.
    for (bool blockExprPrint : {true, false}) {
        ret.emplace_back(DebugPrinter::Block("[`"));
        bool first = true;
        for (auto&& [slot, aggTuple] : _aggs) {
            if (!first) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }

            DebugPrinter::addIdentifier(ret, slot);
            ret.emplace_back("=");
            const auto& aggExpr = blockExprPrint ? aggTuple.blockAgg : aggTuple.agg;
            DebugPrinter::addBlocks(ret, aggExpr->debugPrint());

            if (!blockExprPrint && aggTuple.init) {
                ret.emplace_back(DebugPrinter::Block("init{`"));
                DebugPrinter::addBlocks(ret, aggTuple.init->debugPrint());
                ret.emplace_back(DebugPrinter::Block("`}"));
            }

            first = false;
        }
        ret.emplace_back("`]");
    }

    {
        bool first = true;
        ret.emplace_back(DebugPrinter::Block("[`"));
        for (auto slot : _blockDataInSlotIds) {
            if (!first) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }

            DebugPrinter::addIdentifier(ret, slot);
            first = false;
        }
        ret.emplace_back(DebugPrinter::Block("`]"));
    }

    {
        bool first = true;
        ret.emplace_back(DebugPrinter::Block("[`"));
        for (auto slot : _accumulatorDataSlotIds) {
            if (!first) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }

            DebugPrinter::addIdentifier(ret, slot);
            first = false;
        }
        ret.emplace_back(DebugPrinter::Block("`]"));
    }

    if (!_mergingExprs.empty()) {
        ret.emplace_back("spillSlots[`");
        for (size_t idx = 0; idx < _mergingExprs.size(); ++idx) {
            if (idx) {
                ret.emplace_back("`,");
            }

            DebugPrinter::addIdentifier(ret, _mergingExprs[idx].first);
        }
        ret.emplace_back("`]");

        ret.emplace_back("mergingExprs[`");
        for (size_t idx = 0; idx < _mergingExprs.size(); ++idx) {
            if (idx) {
                ret.emplace_back("`,");
            }

            DebugPrinter::addBlocks(ret, _mergingExprs[idx].second->debugPrint());
        }
        ret.emplace_back("`]");
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

size_t BlockHashAggStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_aggs);
    size += size_estimator::estimate(_outAccessorsMap);
    size += size_estimator::estimate(_mergingExprs);
    return size;
}

void BlockHashAggStage::populateBitmapSlot(size_t n) {
    _blockBitsetOutAccessor.reset(
        true,
        value::TypeTags::valueBlock,
        value::bitcastFrom<value::ValueBlock*>(
            std::make_unique<value::MonoBlock>(
                n, value::TypeTags::Boolean, value::bitcastFrom<bool>(true))
                .release()));
}

value::ValueBlock* BlockHashAggStage::makeMonoBlock(value::TypeTags tag, value::Value val) {
    // Add another element to the end of the '_monoBlocks' deque and get a reference to it
    // ('monoBlockOpt'), emplace a MonoBlock with the specified value into 'monoBlockOpt',
    // and then return a pointer to the MonoBlock.
    _monoBlocks.emplace_back();
    boost::optional<value::MonoBlock>& monoBlockOpt = _monoBlocks.back();

    // MonoBlock wants ownership of the value.
    auto [cpTag, cpVal] = value::copyValue(tag, val);
    monoBlockOpt.emplace(_currentBlockSize, cpTag, cpVal);

    return &*monoBlockOpt;
}
}  // namespace sbe
}  // namespace mongo
