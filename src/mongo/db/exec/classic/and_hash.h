// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Reads from N children, each of which must have a valid RecordId. Uses a hash table to intersect
 * the outputs of the N children based on their record ids, and outputs the intersection.
 *
 * Preconditions: Valid RecordId. More than one child.
 */
class AndHashStage final : public PlanStage {
public:
    AndHashStage(ExpressionContext* expCtx, WorkingSet* ws);

    /**
     * For testing only. Allows tests to set memory usage threshold.
     */
    AndHashStage(ExpressionContext* expCtx, WorkingSet* ws, size_t maxMemUsage);

    void addChild(std::unique_ptr<PlanStage> child);

    /**
     * Returns memory usage.
     * For testing only.
     */
    size_t getMemUsage() const;

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() const final;

    StageType stageType() const final {
        return STAGE_AND_HASH;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "AND_HASH"sv;

private:
    static const size_t kLookAheadWorks;

    StageState readFirstChild(WorkingSetID* out);
    StageState hashOtherChildren(WorkingSetID* out);
    StageState workChild(size_t childNo, WorkingSetID* out);

    // Not owned by us.
    WorkingSet* _ws;

    // We want to see if any of our children are EOF immediately.  This requires working them a
    // few times to see if they hit EOF or if they produce a result.  If they produce a result,
    // we place that result here.
    std::vector<WorkingSetID> _lookAheadResults;

    // _dataMap is filled out by the first child and probed by subsequent children.  This is the
    // hash table that we create by intersecting _children and probe with the last child.
    typedef stdx::unordered_map<RecordId, WorkingSetID, RecordId::Hasher> DataMap;
    DataMap _dataMap;

    // Keeps track of what elements from _dataMap subsequent children have seen.
    // Only used while _hashingChildren.
    typedef stdx::unordered_set<RecordId, RecordId::Hasher> SeenMap;
    SeenMap _seenMap;

    // True if we're still intersecting _children[0..._children.size()-1].
    bool _hashingChildren;

    // Which child are we currently working on?
    size_t _currentChild;

    // Stats
    AndHashStats _specificStats;

    // The usage in bytes of all buffered data that we're holding.
    // Memory usage is calculated from keys held in _dataMap only.
    // For simplicity, results in _lookAheadResults do not count towards the limit.
    size_t _memUsage;

    // Upper limit for buffered data memory usage.
    // Defaults to 32 MB (See kMaxBytes in and_hash.cpp).
    size_t _maxMemUsage;
};

}  // namespace mongo
