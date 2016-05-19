/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/record_id.h"

namespace mongo {

/**
 * Stage for pulling results out from an aggregation pipeline.
 */
class PipelineProxyStage final : public PlanStage {
public:
    PipelineProxyStage(OperationContext* opCtx,
                       boost::intrusive_ptr<Pipeline> pipeline,
                       const std::shared_ptr<PlanExecutor>& child,
                       WorkingSet* ws);

    PlanStage::StageState doWork(WorkingSetID* out) final;

    bool isEOF() final;

    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) final;

    //
    // Manage our OperationContext.
    //
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    /**
     * Return a shared pointer to the PlanExecutor that feeds the pipeline. The returned
     * pointer may be NULL.
     */
    std::shared_ptr<PlanExecutor> getChildExecutor();

    // Returns empty PlanStageStats object
    std::unique_ptr<PlanStageStats> getStats() final;

    // Not used.
    SpecificStats* getSpecificStats() const final {
        return NULL;
    }

    std::string getPlanSummaryStr() const;
    void getPlanSummaryStats(PlanSummaryStats* statsOut) const;

    // Not used.
    StageType stageType() const final {
        return STAGE_PIPELINE_PROXY;
    }

    static const char* kStageType;

private:
    boost::optional<BSONObj> getNextBson();

    // Things in the _stash should be returned before pulling items from _pipeline.
    const boost::intrusive_ptr<Pipeline> _pipeline;
    std::vector<BSONObj> _stash;
    const bool _includeMetaData;
    std::weak_ptr<PlanExecutor> _childExec;

    // Not owned by us.
    WorkingSet* _ws;
};

}  // namespace mongo
