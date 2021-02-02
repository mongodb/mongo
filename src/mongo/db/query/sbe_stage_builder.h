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

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/lock_acquisition_callback.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/query/stage_builder.h"

namespace mongo::stage_builder {
/**
 * Creates a new compilation environment and registers global values within the
 * new environment.
 */
std::unique_ptr<sbe::RuntimeEnvironment> makeRuntimeEnvironment(
    const CanonicalQuery& cq,
    OperationContext* opCtx,
    sbe::value::SlotIdGenerator* slotIdGenerator);

class PlanStageReqs;

/**
 * The PlanStageSlots class is used by SlotBasedStageBuilder to return the output slots produced
 * after building a stage.
 */
class PlanStageSlots {
public:
    static constexpr StringData kResult = "result"_sd;
    static constexpr StringData kRecordId = "recordId"_sd;
    static constexpr StringData kReturnKey = "returnKey"_sd;
    static constexpr StringData kOplogTs = "oplogTs"_sd;

    PlanStageSlots() = default;

    PlanStageSlots(const PlanStageReqs& reqs, sbe::value::SlotIdGenerator* slotIdGenerator);

    bool has(StringData str) const {
        return _slots.count(str);
    }

    sbe::value::SlotId get(StringData str) const {
        auto it = _slots.find(str);
        invariant(it != _slots.end());
        return it->second;
    }

    boost::optional<sbe::value::SlotId> getIfExists(StringData str) const {
        if (auto it = _slots.find(str); it != _slots.end()) {
            return it->second;
        }
        return boost::none;
    }

    void set(StringData str, sbe::value::SlotId slot) {
        _slots[str] = slot;
    }

    void clear(StringData str) {
        _slots.erase(str);
    }

    const boost::optional<sbe::value::SlotVector>& getIndexKeySlots() const {
        return _indexKeySlots;
    }

    boost::optional<sbe::value::SlotVector> extractIndexKeySlots() {
        ON_BLOCK_EXIT([this] { _indexKeySlots = boost::none; });
        return std::move(_indexKeySlots);
    }

    void setIndexKeySlots(sbe::value::SlotVector iks) {
        _indexKeySlots = std::move(iks);
    }

    void setIndexKeySlots(boost::optional<sbe::value::SlotVector> iks) {
        _indexKeySlots = std::move(iks);
    }

    /**
     * This method applies an action to some/all of the slots within this struct (excluding index
     * key slots). For each slot in this struct, the action is will be applied to the slot if (and
     * only if) the corresponding flag in 'reqs' is true.
     */
    inline void forEachSlot(const PlanStageReqs& reqs,
                            const std::function<void(sbe::value::SlotId)>& fn);

private:
    StringMap<sbe::value::SlotId> _slots;

    // When an index scan produces parts of an index key for a covered projection, this is where
    // the slots for the produced values are stored.
    boost::optional<sbe::value::SlotVector> _indexKeySlots;
};

/**
 * The PlanStageReqs class is used by SlotBasedStageBuilder to represent the incoming requirements
 * and context when building a stage.
 */
class PlanStageReqs {
public:
    PlanStageReqs copy() const {
        return *this;
    }

    bool has(StringData str) const {
        auto it = _slots.find(str);
        return it != _slots.end() && it->second;
    }

    PlanStageReqs& set(StringData str) {
        _slots[str] = true;
        return *this;
    }

    PlanStageReqs& setIf(StringData str, bool condition) {
        if (condition) {
            _slots[str] = true;
        }
        return *this;
    }

    PlanStageReqs& clear(StringData str) {
        _slots.erase(str);
        return *this;
    }

    boost::optional<sbe::IndexKeysInclusionSet>& getIndexKeyBitset() {
        return _indexKeyBitset;
    }

    const boost::optional<sbe::IndexKeysInclusionSet>& getIndexKeyBitset() const {
        return _indexKeyBitset;
    }

    bool getIsBuildingUnionForTailableCollScan() const {
        return _isBuildingUnionForTailableCollScan;
    }

    void setIsBuildingUnionForTailableCollScan(bool b) {
        _isBuildingUnionForTailableCollScan = b;
    }

    bool getIsTailableCollScanResumeBranch() const {
        return _isTailableCollScanResumeBranch;
    }

    void setIsTailableCollScanResumeBranch(bool b) {
        _isTailableCollScanResumeBranch = b;
    }

    friend PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                                          sbe::value::SlotIdGenerator* slotIdGenerator);

    friend void PlanStageSlots::forEachSlot(const PlanStageReqs& reqs,
                                            const std::function<void(sbe::value::SlotId)>& fn);

private:
    StringMap<bool> _slots;

    // A bitset here indicates that we have a covered projection that is expecting to parts of the
    // index key from an index scan.
    boost::optional<sbe::IndexKeysInclusionSet> _indexKeyBitset;

    // When we're in the middle of building a special union sub-tree implementing a tailable cursor
    // collection scan, this flag will be set to true. Otherwise this flag will be false.
    bool _isBuildingUnionForTailableCollScan{false};

    // When we're in the middle of building a special union sub-tree implementing a tailable cursor
    // collection scan, this flag indicates whether we're currently building an anchor or resume
    // branch. At all other times, this flag will be false.
    bool _isTailableCollScanResumeBranch{false};
};

void PlanStageSlots::forEachSlot(const PlanStageReqs& reqs,
                                 const std::function<void(sbe::value::SlotId)>& fn) {
    for (auto&& [slotName, isRequired] : reqs._slots) {
        if (isRequired) {
            auto it = _slots.find(slotName);
            invariant(it != _slots.end());
            fn(it->second);
        }
    }
}

/**
 * Some auxiliary data returned by a 'SlotBasedStageBuilder' along with a PlanStage tree root, which
 * is needed to execute the PlanStage tree.
 */
struct PlanStageData {
    PlanStageData() = default;

    explicit PlanStageData(std::unique_ptr<sbe::RuntimeEnvironment> env)
        : env(env.get()), ctx(std::move(env)) {}

    std::string debugString() const;

    // This holds the output slots produced by SBE plan (resultSlot, recordIdSlot, etc).
    PlanStageSlots outputs;

    // The CompileCtx object owns the RuntimeEnvironment. The RuntimeEnvironment owns various
    // SlotAccessors which are accessed when the SBE plan is executed.
    sbe::RuntimeEnvironment* env{nullptr};
    sbe::CompileCtx ctx;

    bool shouldTrackLatestOplogTimestamp{false};
    bool shouldTrackResumeToken{false};
    bool shouldUseTailableScan{false};
};

/**
 * A stage builder which builds an executable tree using slot-based PlanStages.
 */
class SlotBasedStageBuilder final : public StageBuilder<sbe::PlanStage> {
public:
    static constexpr StringData kResult = PlanStageSlots::kResult;
    static constexpr StringData kRecordId = PlanStageSlots::kRecordId;
    static constexpr StringData kReturnKey = PlanStageSlots::kReturnKey;
    static constexpr StringData kOplogTs = PlanStageSlots::kOplogTs;

    SlotBasedStageBuilder(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          const CanonicalQuery& cq,
                          const QuerySolution& solution,
                          PlanYieldPolicySBE* yieldPolicy,
                          ShardFiltererFactoryInterface* shardFilterer);

    std::unique_ptr<sbe::PlanStage> build(const QuerySolutionNode* root) final;

    PlanStageData getPlanStageData() {
        return std::move(_data);
    }

private:
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> build(const QuerySolutionNode* node,
                                                                     const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildCollScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildVirtualScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildIndexScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildFetch(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildLimit(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSkip(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSort(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSortKeyGeneraror(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSortMerge(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionSimple(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionDefault(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildOr(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildText(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildReturnKey(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildEof(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildAndHash(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::tuple<sbe::value::SlotId, sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
    makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                         sbe::value::SlotId recordIdSlot,
                         PlanNodeId planNodeId,
                         sbe::value::SlotVector slotsToForward = {});

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> makeUnionForTailableCollScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildShardFilter(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;

    PlanYieldPolicySBE* const _yieldPolicy{nullptr};

    // Apart from generating just an execution tree, this builder will also produce some auxiliary
    // data which is needed to execute the tree.
    PlanStageData _data;

    bool _buildHasStarted{false};
    bool _shouldProduceRecordIdSlot{true};

    // A factory to construct shard filters.
    ShardFiltererFactoryInterface* _shardFiltererFactory;

    // A callback that should be installed on "scan" and "ixscan" nodes. It will get invoked when
    // these data access stages acquire their AutoGet*.
    const sbe::LockAcquisitionCallback _lockAcquisitionCallback;
};
}  // namespace mongo::stage_builder
