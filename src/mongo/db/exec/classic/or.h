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
#include "mongo/db/exec/classic/recordid_deduplicator.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <cstddef>
#include <memory>

namespace mongo {

/**
 * This stage outputs the union of its children. It optionally deduplicates on RecordId.
 *
 * Preconditions: Valid RecordId.
 */
class OrStage final : public PlanStage {
public:
    OrStage(ExpressionContext* expCtx, WorkingSet* ws, bool dedup, const MatchExpression* filter);

    void addChild(std::unique_ptr<PlanStage> child);

    void addChildren(Children childrenToAdd);

    bool isEOF() const final;

    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_OR;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    const SimpleMemoryUsageTracker& getMemoryTracker_forTest() {
        return _memoryTracker;
    }

    static const char* kStageType;

private:
    // Not owned by us.
    WorkingSet* _ws;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    // Which of _children are we calling work(...) on now?
    size_t _currentChild;

    // True if we dedup on RecordId, false otherwise.
    const bool _dedup;

    // Which RecordIds have we returned?
    RecordIdDeduplicator _recordIdDeduplicator;

    // Stats
    OrStats _specificStats;

    // Track memory used by this stage for deduplicating.
    SimpleMemoryUsageTracker _memoryTracker;
};

}  // namespace mongo
