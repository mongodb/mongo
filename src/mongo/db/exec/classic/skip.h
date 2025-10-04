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

#include <memory>

namespace mongo {

/**
 * This stage implements skip functionality.  It drops the first 'toSkip' results from its child
 * then returns the rest verbatim.
 *
 * Preconditions: None.
 */
class SkipStage final : public PlanStage {
public:
    SkipStage(ExpressionContext* expCtx,
              long long toSkip,
              WorkingSet* ws,
              std::unique_ptr<PlanStage> child);
    ~SkipStage() override;

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SKIP;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    WorkingSet* _ws;

    // The number of results left to skip. This number is decremented during query execution as we
    // successfully skip a document.
    long long _leftToSkip;

    // Represents the number of results to skip. Unlike '_leftToSkip', this remains constant and
    // is used when gathering statistics in explain.
    const long long _skipAmount;

    // Stats
    SkipStats _specificStats;
};

}  // namespace mongo
