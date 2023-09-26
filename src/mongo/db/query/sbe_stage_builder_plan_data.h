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

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/interval_evaluation_tree.h"
#include "mongo/db/query/plan_cache_debug_info.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo::stage_builder {

/**
 * The ParameterizedIndexScanSlots struct is used by SlotBasedStageBuilder while building the index
 * scan stage to return the slots that are registered in the runtime environment and will be
 * populated based on the index bounds.
 */
struct ParameterizedIndexScanSlots {
    // Holds the low and high key for the single interval index scan algorithm.
    struct SingleIntervalPlan {
        sbe::value::SlotId lowKey;
        sbe::value::SlotId highKey;
    };

    // Holds the slots for the generic index scan algorithm.
    struct GenericPlan {
        // Holds the value whether the generic or optimized index scan should be used.
        sbe::value::SlotId isGenericScan;

        // Holds the value of the IndexBounds used for the generic index scan algorithm.
        sbe::value::SlotId indexBounds;

        // Holds the value of an array of low and high keys for each interval.
        sbe::value::SlotId lowHighKeyIntervals;
    };

    // In the case that the parameterized plan will always consist of a single interval index scan,
    // this holds the SingleInterval struct. Otherwise, holds the necessary slots for a fully
    // generic parameterized index scan plan.
    stdx::variant<SingleIntervalPlan, GenericPlan> slots;
};

// Holds the slots for the clustered collection scan bounds.
struct ParameterizedClusteredScanSlots {
    // Holds the min and max record for the bounds of a clustered collection scan.
    boost::optional<sbe::value::SlotId> minRecord;
    boost::optional<sbe::value::SlotId> maxRecord;
};

using VariableIdToSlotMap = stdx::unordered_map<Variables::Id, sbe::value::SlotId>;

/**
 * IndexBoundsEvaluationInfo struct contains Interval Evaluation Trees (IETs) and additional data
 * structures required to restore index bounds from IETs and bind them to generic index scan
 * algorithm.
 */
struct IndexBoundsEvaluationInfo {
    IndexEntry index;
    key_string::Version keyStringVersion;
    Ordering ordering;
    int direction;
    std::vector<interval_evaluation_tree::IET> iets;
    ParameterizedIndexScanSlots slots;
};

/**
 * This class holds the RuntimeEnvironment and CompileCtx for an SBE plan. The RuntimeEnvironment
 * owns various SlotAccessors which are accessed when the SBE plan is executed. The CompileCtx is
 * used when the SBE plan needs to be "prepared" (via the prepare() method).
 */
struct Environment {
    Environment(std::unique_ptr<sbe::RuntimeEnvironment> runtimeEnv)
        : runtimeEnv(runtimeEnv.get()), ctx(std::move(runtimeEnv)) {}

    Environment makeCopy() const {
        return Environment(runtimeEnv->makeDeepCopy());
    }

    sbe::RuntimeEnvironment* operator->() noexcept {
        return runtimeEnv;
    }

    const sbe::RuntimeEnvironment* operator->() const noexcept {
        return runtimeEnv;
    }

    sbe::RuntimeEnvironment& operator*() noexcept {
        return *runtimeEnv;
    }

    const sbe::RuntimeEnvironment& operator*() const noexcept {
        return *runtimeEnv;
    }

    sbe::RuntimeEnvironment* runtimeEnv{nullptr};
    sbe::CompileCtx ctx;
};

struct PlanStageMetadataSlots {
    boost::optional<sbe::value::SlotId> searchScoreSlot;
    boost::optional<sbe::value::SlotId> searchHighlightsSlot;
    boost::optional<sbe::value::SlotId> searchDetailsSlot;
    boost::optional<sbe::value::SlotId> searchSortValuesSlot;
};

/**
 * This struct used to hold all of a PlanStageData's immutable data.
 */
struct PlanStageStaticData {
    // This holds the output slots produced by SBE plan (resultSlot, recordIdSlot, etc).
    boost::optional<sbe::value::SlotId> resultSlot;
    boost::optional<sbe::value::SlotId> recordIdSlot;
    PlanStageMetadataSlots metadataSlots;

    // Various flags copied from the CollectionScanNode. If the plan generated by the query planner
    // did not have a CollectionScanNode, then each of these flags is initialized to its respective
    // default value.
    bool shouldTrackLatestOplogTimestamp{false};
    bool shouldTrackResumeToken{false};
    bool shouldUseTailableScan{false};

    // Scan direction if this plan has a collection scan: 1 means forward; -1 means reverse.
    int direction{1};

    // True iff this plan does an SBE clustered collection scan.
    bool doSbeClusteredCollectionScan{false};

    // Iff 'doSbeClusteredCollectionScan', this holds the cluster key field name.
    std::string clusterKeyFieldName;

    // Iff 'doSbeClusteredCollectionScan', this holds the clustered collection's native collator,
    // needed to compute scan bounds.
    std::shared_ptr<CollatorInterface> ccCollator;

    // If the query has been auto-parameterized, then the mapping from input parameter id to the
    // id of a slot in the runtime environment is maintained here. This mapping is established
    // during stage building and stored in the cache. When a cached plan is used for a
    // subsequent query, this mapping is used to set the new constant value associated with each
    // input parameter id in the runtime environment.
    //
    // For example, imagine an auto-parameterized query {a: <p1>, b: <p2>} is present in the SBE
    // plan cache. Also present in the cache is this mapping:
    //    p1 -> s3
    //    p2 -> s4
    //
    // A new query {a: 5, b: 6} runs. Using this mapping, we set a value of 5 in s3 and 6 in s4.
    sbe::InputParamToSlotMap inputParamToSlotMap;

    // This Variable-to-SlotId map stores all Variables that were translated into corresponding
    // SBE Slots. The slots are registered in the 'RuntimeEnvironment'.
    VariableIdToSlotMap variableIdToSlotMap;

    // Stores auxiliary data to restore index bounds for a cached auto-parameterized SBE plan
    // for every index used by the plan.
    std::vector<IndexBoundsEvaluationInfo> indexBoundsEvaluationInfos;

    // Stores data to restore collection scan bounds for a cached auto-parameterized SBE plan for
    // every clustered collection scan used by the plan.
    std::vector<ParameterizedClusteredScanSlots> clusteredCollBoundsInfos;

    // Stores all namespaces involved in the build side of a hash join plan. Needed to check if
    // the plan should be evicted as the size of the foreign namespace changes.
    absl::flat_hash_set<NamespaceString> foreignHashJoinCollections;

    // Stores CollatorInterface to be used for this plan. Raw pointer may be stored inside data
    // structures, so it must be kept stable.
    std::shared_ptr<CollatorInterface> queryCollator;

    // Shared pointers to InListDatas used by this SBE plan.
    std::vector<std::shared_ptr<InListData>> inLists;

    // Shared pointers (and a map) to the additonal collators used by this SBE plan.
    std::vector<std::unique_ptr<CollatorInterface>> collators;
};

/**
 * Some auxiliary data returned by a 'SlotBasedStageBuilder' along with a PlanStage tree root, which
 * is needed to execute the PlanStage tree.
 */
struct PlanStageData {
    using DebugInfoSBE = plan_cache_debug_info::DebugInfoSBE;

    explicit PlanStageData(Environment env, std::shared_ptr<const PlanStageStaticData> staticData)
        : env(std::move(env)), staticData(std::move(staticData)) {}

    PlanStageData(PlanStageData&&) = default;

    PlanStageData(const PlanStageData& other)
        : env(other.env.makeCopy()),
          staticData(other.staticData),
          replanReason(other.replanReason),
          savedStatsOnEarlyExit(std::unique_ptr<sbe::PlanStageStats>(
              other.savedStatsOnEarlyExit ? other.savedStatsOnEarlyExit->clone() : nullptr)),
          debugInfo(other.debugInfo),
          inLists(other.inLists),
          inListsSet(other.inListsSet) {}

    PlanStageData& operator=(PlanStageData&&) = default;

    PlanStageData& operator=(const PlanStageData& other) {
        if (this != &other) {
            env = other.env.makeCopy();
            staticData = other.staticData;
            replanReason = other.replanReason;
            savedStatsOnEarlyExit = std::unique_ptr<sbe::PlanStageStats>{
                other.savedStatsOnEarlyExit ? other.savedStatsOnEarlyExit->clone() : nullptr};
            debugInfo = other.debugInfo;
            inLists = other.inLists;
            inListsSet = other.inListsSet;
        }
        return *this;
    }

    std::string debugString() const;

    // This field holds the RuntimeEnvironment and the CompileCtx.
    Environment env;

    // This field holds all of the immutable data that needs to accompany an SBE PlanStage tree.
    std::shared_ptr<const PlanStageStaticData> staticData;

    // If this execution tree was built as a result of replanning of the cached plan, this string
    // will include the reason for replanning.
    boost::optional<std::string> replanReason;

    // If this candidate plan has completed the trial run early by achieving one of the trial run
    // metrics, the stats are cached in here.
    std::unique_ptr<sbe::PlanStageStats> savedStatsOnEarlyExit{nullptr};

    // Stores plan cache entry information used as debug information or for "explain" purpose. Note
    // that 'debugInfo' is present only if this PlanStageData is recovered from the plan cache.
    std::shared_ptr<const DebugInfoSBE> debugInfo;

    // Shared pointers to InListDatas used by this SBE plan.
    std::vector<std::shared_ptr<InListData>> inLists;

    // Hash set of the InListData pointers in 'inLists'.
    absl::flat_hash_set<InListData*> inListsSet;
};

}  // namespace mongo::stage_builder
