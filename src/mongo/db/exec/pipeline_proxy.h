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

#include <boost/optional/optional.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/record_id.h"

namespace mongo {

    /**
     * Stage for pulling results out from an aggregation pipeline.
     */
    class PipelineProxyStage : public PlanStage {
    public:
        PipelineProxyStage(boost::intrusive_ptr<Pipeline> pipeline,
                           const boost::shared_ptr<PlanExecutor>& child,
                           WorkingSet* ws);

        virtual PlanStage::StageState work(WorkingSetID* out);

        virtual bool isEOF();

        virtual void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type);

        //
        // Manage our OperationContext. We intentionally don't propagate to the child
        // Runner as that is handled by DocumentSourceCursor as it needs to.
        //
        virtual void saveState();
        virtual void restoreState(OperationContext* opCtx);

        /**
         * Make obj the next object returned by getNext().
         */
        void pushBack(const BSONObj& obj);

        /**
         * Return a shared pointer to the PlanExecutor that feeds the pipeline. The returned
         * pointer may be NULL.
         */
        boost::shared_ptr<PlanExecutor> getChildExecutor();

        //
        // These should not be used.
        //

        virtual PlanStageStats* getStats() { return NULL; }
        virtual CommonStats* getCommonStats() { return NULL; }
        virtual SpecificStats* getSpecificStats() { return NULL; }

        // Not used.
        virtual std::vector<PlanStage*> getChildren() const;

        // Not used.
        virtual StageType stageType() const { return STAGE_PIPELINE_PROXY; }

    private:
        boost::optional<BSONObj> getNextBson();

        // Things in the _stash sould be returned before pulling items from _pipeline.
        const boost::intrusive_ptr<Pipeline> _pipeline;
        std::vector<BSONObj> _stash;
        const bool _includeMetaData;
        boost::weak_ptr<PlanExecutor> _childExec;

        // Not owned by us.
        WorkingSet* _ws;
    };

} // namespace mongo
