/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

#include <boost/scoped_ptr.hpp>
#include <string>
#include <queue>

#include "mongo/base/status.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    class OperationContext;

    /**
     * The SubplanStage is used for rooted $or queries. It plans each clause of the $or
     * individually, and then creates an overall query plan based on the winning plan from
     * each clause.
     *
     * Uses the MultiPlanStage in order to rank plans for the individual clauses.
     */
    class SubplanStage : public PlanStage {
    public:
        /**
         * Used to create SubplanStage instances. The caller owns the instance
         * returned through 'out'.
         *
         * 'out' is valid only if an OK status is returned.
         */
        static Status make(OperationContext* txn,
                           Collection* collection,
                           WorkingSet* ws,
                           const QueryPlannerParams& params,
                           CanonicalQuery* cq,
                           SubplanStage** out);

        virtual ~SubplanStage();

        static bool canUseSubplanning(const CanonicalQuery& query);

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield(OperationContext* opCtx);
        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual std::vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const { return STAGE_SUBPLAN; }

        PlanStageStats* getStats();

        virtual const CommonStats* getCommonStats();

        virtual const SpecificStats* getSpecificStats();

        static const char* kStageType;

    private:
        SubplanStage(OperationContext* txn,
                     Collection* collection,
                     WorkingSet* ws,
                     const QueryPlannerParams& params,
                     CanonicalQuery* cq);

        /**
         * Plan each branch of the $or independently, and store the resulting
         * lists of query solutions in '_solutions'.
         *
         * Called from SubplanStage::make so that construction of the subplan stage
         * fails immediately, rather than returning a plan executor and subsequently
         * through getNext(...).
         */
        Status planSubqueries();

        /**
         * Uses the query planning results from planSubqueries() and the multi plan stage
         * to select the best plan for each branch.
         */
        Status pickBestPlan();

        // transactional context for read locks. Not owned by us
        OperationContext* _txn;

        Collection* _collection;

        // Not owned here.
        WorkingSet* _ws;

        QueryPlannerParams _plannerParams;

        // Not owned here.
        CanonicalQuery* _query;

        bool _killed;

        boost::scoped_ptr<PlanStage> _child;

        // We do the subquery planning up front, and keep the resulting
        // query solutions here. Lists of query solutions are dequeued
        // and ownership is transferred to the underlying runners one
        // at a time.
        std::queue< std::vector<QuerySolution*> > _solutions;

        // Holds the canonicalized subqueries. Ownership is transferred
        // to the underlying runners one at a time.
        std::queue<CanonicalQuery*> _cqs;

        // We need this to extract cache-friendly index data from the index assignments.
        map<BSONObj, size_t> _indexMap;

        CommonStats _commonStats;
    };

}  // namespace mongo
