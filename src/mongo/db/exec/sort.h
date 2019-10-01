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

#include <set>
#include <vector>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/sort_executor.h"
#include "mongo/db/exec/sort_key_comparator.h"
#include "mongo/db/exec/sort_key_generator.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/record_id.h"

namespace mongo {

/**
 * Sorts the input received from the child according to the sort pattern provided.
 *
 * Preconditions:
 *   -- For each field in 'pattern', all inputs in the child must handle a getFieldDotted for that
 *   field.
 *   -- All WSMs produced by the child stage must have the sort key available as WSM computed data.
 */
class SortStage final : public PlanStage {
public:
    static constexpr StringData kStageType = "SORT"_sd;

    SortStage(boost::intrusive_ptr<ExpressionContext> expCtx,
              WorkingSet* ws,
              SortPattern sortPattern,
              uint64_t limit,
              uint64_t maxMemoryUsageBytes,
              std::unique_ptr<PlanStage> child);

    bool isEOF() final {
        return _sortExecutor.isEOF();
    }

    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SORT;
    }

    std::unique_ptr<PlanStageStats> getStats();

    /**
     * Returns nullptr. Stats related to sort execution must be extracted with 'getStats()', since
     * they are retrieved on demand from the underlying sort execution machinery.
     */
    const SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

private:
    // Not owned by us.
    WorkingSet* _ws;

    SortExecutor _sortExecutor;

    SortStats _specificStats;

    // Whether or not we have finished loading data into '_sortExecutor'.
    bool _populated = false;
};

}  // namespace mongo
