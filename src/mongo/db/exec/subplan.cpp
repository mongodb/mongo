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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/subplan.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder.h"

namespace mongo {

    // static
    const char* SubplanStage::kStageType = "SUBPLAN";

    SubplanStage::SubplanStage(OperationContext* txn,
                               Collection* collection,
                               WorkingSet* ws,
                               const QueryPlannerParams& params,
                               CanonicalQuery* cq)
        : _txn(txn),
          _collection(collection),
          _ws(ws),
          _plannerParams(params),
          _query(cq),
          _killed(false),
          _child(NULL),
          _commonStats(kStageType) { }

    SubplanStage::~SubplanStage() {
        while (!_solutions.empty()) {
            vector<QuerySolution*> solns = _solutions.front();
            for (size_t i = 0; i < solns.size(); i++) {
                delete solns[i];
            }
            _solutions.pop();
        }

        while (!_cqs.empty()) {
            delete _cqs.front();
            _cqs.pop();
        }
    }

    // static
    bool SubplanStage::canUseSubplanning(const CanonicalQuery& query) {
        const LiteParsedQuery& lpq = query.getParsed();
        const MatchExpression* expr = query.root();

        // Only rooted ORs work with the subplan scheme.
        if (MatchExpression::OR != expr->matchType()) {
            return false;
        }

        // Collection scan
        // No sort order requested
        if (lpq.getSort().isEmpty() &&
            expr->matchType() == MatchExpression::AND && expr->numChildren() == 0) {
            return false;
        }

        // Hint provided
        if (!lpq.getHint().isEmpty()) {
            return false;
        }

        // Min provided
        // Min queries are a special case of hinted queries.
        if (!lpq.getMin().isEmpty()) {
            return false;
        }

        // Max provided
        // Similar to min, max queries are a special case of hinted queries.
        if (!lpq.getMax().isEmpty()) {
            return false;
        }

        // Tailable cursors won't get cached, just turn into collscans.
        if (query.getParsed().getOptions().tailable) {
            return false;
        }

        // Snapshot is really a hint.
        if (query.getParsed().isSnapshot()) {
            return false;
        }

        return true;
    }

    Status SubplanStage::planSubqueries() {
        // Adds the amount of time taken by planSubqueries() to executionTimeMillis. There's lots of
        // work that happens here, so this is needed for the time accounting to make sense.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        MatchExpression* theOr = _query->root();

        for (size_t i = 0; i < _plannerParams.indices.size(); ++i) {
            const IndexEntry& ie = _plannerParams.indices[i];
            _indexMap[ie.keyPattern] = i;
            QLOG() << "Subplanner: index " << i << " is " << ie.toString() << endl;
        }

        const WhereCallbackReal whereCallback(_txn, _collection->ns().db());

        for (size_t i = 0; i < theOr->numChildren(); ++i) {
            // Turn the i-th child into its own query.
            MatchExpression* orChild = theOr->getChild(i);
            CanonicalQuery* orChildCQ;
            Status childCQStatus = CanonicalQuery::canonicalize(*_query,
                                                                orChild,
                                                                &orChildCQ,
                                                                whereCallback);
            if (!childCQStatus.isOK()) {
                mongoutils::str::stream ss;
                ss << "Can't canonicalize subchild " << orChild->toString()
                   << " " << childCQStatus.reason();
                return Status(ErrorCodes::BadValue, ss);
            }

            // Make sure it gets cleaned up.
            auto_ptr<CanonicalQuery> safeOrChildCQ(orChildCQ);

            // Plan the i-th child.
            vector<QuerySolution*> solutions;

            // We don't set NO_TABLE_SCAN because peeking at the cache data will keep us from 
            // considering any plan that's a collscan.
            QLOG() << "Subplanner: planning child " << i << " of " << theOr->numChildren();
            Status status = QueryPlanner::plan(*safeOrChildCQ, _plannerParams, &solutions);

            if (!status.isOK()) {
                mongoutils::str::stream ss;
                ss << "Can't plan for subchild " << orChildCQ->toString()
                   << " " << status.reason();
                return Status(ErrorCodes::BadValue, ss);
            }
            QLOG() << "Subplanner: got " << solutions.size() << " solutions";

            if (0 == solutions.size()) {
                // If one child doesn't have an indexed solution, bail out.
                mongoutils::str::stream ss;
                ss << "No solutions for subchild " << orChildCQ->toString();
                return Status(ErrorCodes::BadValue, ss);
            }

            // Hang onto the canonicalized subqueries and the corresponding query solutions
            // so that they can be used in subplan running later on.
            _cqs.push(safeOrChildCQ.release());
            _solutions.push(solutions);
        }

        return Status::OK();
    }

    Status SubplanStage::choosePlanForSubqueries(PlanYieldPolicy* yieldPolicy) {
        // This is what we annotate with the index selections and then turn into a solution.
        auto_ptr<OrMatchExpression> theOr(
            static_cast<OrMatchExpression*>(_query->root()->shallowClone()));

        // This is the skeleton of index selections that is inserted into the cache.
        auto_ptr<PlanCacheIndexTree> cacheData(new PlanCacheIndexTree());

        for (size_t i = 0; i < theOr->numChildren(); ++i) {
            MatchExpression* orChild = theOr->getChild(i);

            auto_ptr<CanonicalQuery> orChildCQ(_cqs.front());
            _cqs.pop();

            // 'solutions' is owned by the SubplanStage instance until
            // it is popped from the queue.
            vector<QuerySolution*> solutions = _solutions.front();
            _solutions.pop();

            // We already checked for zero solutions in planSubqueries(...).
            invariant(!solutions.empty());

            if (1 == solutions.size()) {
                // There is only one solution. Transfer ownership to an auto_ptr.
                auto_ptr<QuerySolution> autoSoln(solutions[0]);

                // We want a well-formed *indexed* solution.
                if (NULL == autoSoln->cacheData.get()) {
                    // For example, we don't cache things for 2d indices.
                    mongoutils::str::stream ss;
                    ss << "No cache data for subchild " << orChild->toString();
                    return Status(ErrorCodes::BadValue, ss);
                }

                if (SolutionCacheData::USE_INDEX_TAGS_SOLN != autoSoln->cacheData->solnType) {
                    mongoutils::str::stream ss;
                    ss << "No indexed cache data for subchild "
                       << orChild->toString();
                    return Status(ErrorCodes::BadValue, ss);
                }

                // Add the index assignments to our original query.
                Status tagStatus = QueryPlanner::tagAccordingToCache(
                    orChild, autoSoln->cacheData->tree.get(), _indexMap);

                if (!tagStatus.isOK()) {
                    mongoutils::str::stream ss;
                    ss << "Failed to extract indices from subchild "
                       << orChild->toString();
                    return Status(ErrorCodes::BadValue, ss);
                }

                // Add the child's cache data to the cache data we're creating for the main query.
                cacheData->children.push_back(autoSoln->cacheData->tree->clone());
            }
            else {
                // N solutions, rank them.  Takes ownership of orChildCQ.

                _ws->clear();

                _child.reset(new MultiPlanStage(_txn, _collection, orChildCQ.get()));
                MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(_child.get());

                // Dump all the solutions into the MPR.
                for (size_t ix = 0; ix < solutions.size(); ++ix) {
                    PlanStage* nextPlanRoot;
                    verify(StageBuilder::build(_txn,
                                               _collection,
                                               *solutions[ix],
                                               _ws,
                                               &nextPlanRoot));

                    // Owns first two arguments
                    multiPlanStage->addPlan(solutions[ix], nextPlanRoot, _ws);
                }

                Status planSelectStat = multiPlanStage->pickBestPlan(yieldPolicy);
                if (!planSelectStat.isOK()) {
                    return planSelectStat;
                }

                if (!multiPlanStage->bestPlanChosen()) {
                    mongoutils::str::stream ss;
                    ss << "Failed to pick best plan for subchild "
                       << orChildCQ->toString();
                    return Status(ErrorCodes::BadValue, ss);
                }

                QuerySolution* bestSoln = multiPlanStage->bestSolution();

                // Check that we have good cache data. For example, we don't cache things
                // for 2d indices.
                if (NULL == bestSoln->cacheData.get()) {
                    mongoutils::str::stream ss;
                    ss << "No cache data for subchild " << orChild->toString();
                    return Status(ErrorCodes::BadValue, ss);
                }

                if (SolutionCacheData::USE_INDEX_TAGS_SOLN != bestSoln->cacheData->solnType) {
                    mongoutils::str::stream ss;
                    ss << "No indexed cache data for subchild "
                       << orChild->toString();
                    return Status(ErrorCodes::BadValue, ss);
                }

                // Add the index assignments to our original query.
                Status tagStatus = QueryPlanner::tagAccordingToCache(
                    orChild, bestSoln->cacheData->tree.get(), _indexMap);

                if (!tagStatus.isOK()) {
                    mongoutils::str::stream ss;
                    ss << "Failed to extract indices from subchild "
                       << orChild->toString();
                    return Status(ErrorCodes::BadValue, ss);
                }

                cacheData->children.push_back(bestSoln->cacheData->tree->clone());
            }
        }

        // Must do this before using the planner functionality.
        sortUsingTags(theOr.get());

        // Use the cached index assignments to build solnRoot.  Takes ownership of 'theOr'
        QuerySolutionNode* solnRoot = QueryPlannerAccess::buildIndexedDataAccess(
            *_query, theOr.release(), false, _plannerParams.indices);

        if (NULL == solnRoot) {
            mongoutils::str::stream ss;
            ss << "Failed to build indexed data path for subplanned query\n";
            return Status(ErrorCodes::BadValue, ss);
        }

        QLOG() << "Subplanner: fully tagged tree is " << solnRoot->toString();

        // Takes ownership of 'solnRoot'
        QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(*_query,
                                                                      _plannerParams,
                                                                      solnRoot);

        if (NULL == soln) {
            mongoutils::str::stream ss;
            ss << "Failed to analyze subplanned query";
            return Status(ErrorCodes::BadValue, ss);
        }

        // We want our franken-solution to be cached.
        SolutionCacheData* scd = new SolutionCacheData();
        scd->tree.reset(cacheData.release());
        soln->cacheData.reset(scd);

        QLOG() << "Subplanner: Composite solution is " << soln->toString() << endl;

        // We use one of these even if there is one plan.  We do this so that the entry is cached
        // with stats obtained in the same fashion as a competitive ranking would have obtained
        // them.
        _ws->clear();
        _child.reset(new MultiPlanStage(_txn, _collection, _query));
        MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(_child.get());
        PlanStage* root;
        verify(StageBuilder::build(_txn, _collection, *soln, _ws, &root));
        multiPlanStage->addPlan(soln, root, _ws); // Takes ownership first two arguments.

        multiPlanStage->pickBestPlan(yieldPolicy);
        if (! multiPlanStage->bestPlanChosen()) {
            mongoutils::str::stream ss;
            ss << "Failed to pick best plan for subchild "
               << _query->toString();
            return Status(ErrorCodes::BadValue, ss);
        }

        return Status::OK();
    }

    Status SubplanStage::choosePlanWholeQuery(PlanYieldPolicy* yieldPolicy) {
        // Clear out the working set. We'll start with a fresh working set.
        _ws->clear();

        // Use the query planning module to plan the whole query.
        vector<QuerySolution*> rawSolutions;
        Status status = QueryPlanner::plan(*_query, _plannerParams, &rawSolutions);
        if (!status.isOK()) {
            return Status(ErrorCodes::BadValue,
                          "error processing query: " + _query->toString() +
                          " planner returned error: " + status.reason());
        }

        OwnedPointerVector<QuerySolution> solutions(rawSolutions);

        // We cannot figure out how to answer the query.  Perhaps it requires an index
        // we do not have?
        if (0 == solutions.size()) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                          << "error processing query: "
                          << _query->toString()
                          << " No query solutions");
        }

        if (1 == solutions.size()) {
            PlanStage* root;
            // Only one possible plan.  Run it.  Build the stages from the solution.
            verify(StageBuilder::build(_txn, _collection, *solutions[0], _ws, &root));
            _child.reset(root);

            // This SubplanStage takes ownership of the query solution.
            _solutions.push(solutions.release());

            return Status::OK();
        }
        else {
            // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
            // and so on. The working set will be shared by all candidate plans.
            _child.reset(new MultiPlanStage(_txn, _collection, _query));
            MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(_child.get());

            for (size_t ix = 0; ix < solutions.size(); ++ix) {
                if (solutions[ix]->cacheData.get()) {
                    solutions[ix]->cacheData->indexFilterApplied =
                        _plannerParams.indexFiltersApplied;
                }

                // version of StageBuild::build when WorkingSet is shared
                PlanStage* nextPlanRoot;
                verify(StageBuilder::build(_txn, _collection, *solutions[ix], _ws,
                                           &nextPlanRoot));

                // Takes ownership of 'solutions[ix]' and 'nextPlanRoot'.
                multiPlanStage->addPlan(solutions.releaseAt(ix), nextPlanRoot, _ws);
            }

            // Delegate the the MultiPlanStage's plan selection facility.
            Status planSelectStat = multiPlanStage->pickBestPlan(yieldPolicy);
            if (!planSelectStat.isOK()) {
                return planSelectStat;
            }

            return Status::OK();
        }
    }

    Status SubplanStage::pickBestPlan(PlanYieldPolicy* yieldPolicy) {
        // Adds the amount of time taken by pickBestPlan() to executionTimeMillis. There's lots of
        // work that happens here, so this is needed for the time accounting to make sense.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        // Plan each branch of the $or.
        Status subplanningStatus = planSubqueries();
        if (!subplanningStatus.isOK()) {
            return choosePlanWholeQuery(yieldPolicy);
        }

        // Use the multi plan stage to select a winning plan for each branch, and then construct
        // the overall winning plan from the resulting index tags.
        Status subplanSelectStat = choosePlanForSubqueries(yieldPolicy);
        if (!subplanSelectStat.isOK()) {
            return choosePlanWholeQuery(yieldPolicy);
        }

        return Status::OK();
    }

    bool SubplanStage::isEOF() {
        if (_killed) {
            return true;
        }

        // If we're running we best have a runner.
        invariant(_child.get());
        return _child->isEOF();
    }

    PlanStage::StageState SubplanStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (_killed) {
            return PlanStage::DEAD;
        }

        if (isEOF()) { return PlanStage::IS_EOF; }

        invariant(_child.get());
        StageState state = _child->work(out);

        if (PlanStage::NEED_TIME == state) {
            ++_commonStats.needTime;
        }
        else if (PlanStage::NEED_FETCH == state) {
            ++_commonStats.needFetch;
        }
        else if (PlanStage::ADVANCED == state) {
            ++_commonStats.advanced;
        }

        return state;
    }

    void SubplanStage::saveState() {
        ++_commonStats.yields;
        if (_killed) {
            return;
        }

        // We're ranking a sub-plan via an MPR or we're streaming results from this stage.  Either
        // way, pass on the request.
        if (NULL != _child.get()) {
            _child->saveState();
        }
    }

    void SubplanStage::restoreState(OperationContext* opCtx) {
        _txn = opCtx;
        ++_commonStats.unyields;
        if (_killed) {
            return;
        }

        // We're ranking a sub-plan via an MPR or we're streaming results from this stage.  Either
        // way, pass on the request.
        if (NULL != _child.get()) {
            _child->restoreState(opCtx);
        }
    }

    void SubplanStage::invalidate(OperationContext* txn, const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        if (_killed) {
            return;
        }

        if (NULL != _child.get()) {
            _child->invalidate(txn, dl, type);
        }
    }

    vector<PlanStage*> SubplanStage::getChildren() const {
        vector<PlanStage*> children;
        if (NULL != _child.get()) {
            children.push_back(_child.get());
        }
        return children;
    }

    PlanStageStats* SubplanStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_SUBPLAN));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    const CommonStats* SubplanStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* SubplanStage::getSpecificStats() {
        return NULL;
    }

}  // namespace mongo
