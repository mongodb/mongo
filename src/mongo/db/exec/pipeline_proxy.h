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

#include <boost/intrusive_ptr.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/record_id.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Stage for pulling results out from an aggregation pipeline.
 */
class PipelineProxyStage : public PlanStage {
public:
    PipelineProxyStage(OperationContext* opCtx,
                       std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                       WorkingSet* ws);

    virtual ~PipelineProxyStage() = default;

    PlanStage::StageState doWork(WorkingSetID* out) final;

    bool isEOF() final;

    //
    // Manage our OperationContext.
    //
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    // Returns empty PlanStageStats object
    std::unique_ptr<PlanStageStats> getStats() override;

    // Not used.
    SpecificStats* getSpecificStats() const final {
        MONGO_UNREACHABLE;
    }

    std::string getPlanSummaryStr() const;
    void getPlanSummaryStats(PlanSummaryStats* statsOut) const;

    StageType stageType() const override {
        return STAGE_PIPELINE_PROXY;
    }

    /**
     * Writes the pipelineProxyStage's operators to a std::vector<Value>, providing the level of
     * detail specified by 'verbosity'.
     */
    std::vector<Value> writeExplainOps(ExplainOptions::Verbosity verbosity) const;

    static const char* kStageType;

protected:
    PipelineProxyStage(OperationContext* opCtx,
                       std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                       WorkingSet* ws,
                       const char* stageTypeName);

    virtual boost::optional<BSONObj> getNextBson();
    void doDispose() final;

    // Items in the _stash should be returned before pulling items from _pipeline.
    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
    const bool _includeMetaData;

private:
    std::vector<BSONObj> _stash;
    WorkingSet* _ws;
};

}  // namespace mongo
