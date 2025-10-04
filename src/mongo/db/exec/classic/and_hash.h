/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace mongo {

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

    static const char* kStageType;

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
