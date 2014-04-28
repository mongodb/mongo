/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query/subplan_runner.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/multi_plan_runner.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/query/type_explain.h"

namespace mongo {

    // static
    bool SubplanRunner::canUseSubplanRunner(const CanonicalQuery& query) {
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
        if (query.getParsed().hasOption(QueryOption_CursorTailable)) {
            return false;
        }

        // Snapshot is really a hint.
        if (query.getParsed().isSnapshot()) {
            return false;
        }

        return true;
    }

    SubplanRunner::SubplanRunner(Collection* collection,
                                 const QueryPlannerParams& params,
                                 CanonicalQuery* cq)
        : _state(SubplanRunner::PLANNING),
          _collection(collection),
          _plannerParams(params),
          _query(cq),
          _killed(false),
          _policy(Runner::YIELD_MANUAL),
          _ns(cq->getParsed().ns()) { }

    SubplanRunner::~SubplanRunner() { }

    Runner::RunnerState SubplanRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        if (_killed) {
            return Runner::RUNNER_DEAD;
        }

        if (isEOF()) { return Runner::RUNNER_EOF; }

        if (SubplanRunner::PLANNING == _state) {
            // Try to run as sub-plans.
            if (runSubplans()) {
                // If runSubplans returns true we expect something here.
                invariant(_underlyingRunner.get());
            }
            else if (!_killed) {
                // Couldn't run as subplans so we'll just call normal getRunner.

                Runner* runner;
                Status status = getRunnerAlwaysPlan(
                    _collection, _query.release(), _plannerParams, &runner);

                if (!status.isOK()) {
                    // We utterly failed.
                    _killed = true;

                    // Propagate the error to the user wrapped in a BSONObj
                    if (NULL != objOut) {
                        BSONObjBuilder bob;
                        bob.append("ok", status.isOK() ? 1.0 : 0.0);
                        bob.append("code", status.code());
                        bob.append("errmsg", status.reason());
                        *objOut = bob.obj();
                    }
                    return Runner::RUNNER_ERROR;
                }
                else {
                    _underlyingRunner.reset(runner);
                    _underlyingRunner->setYieldPolicy(_policy);
                }
            }

            // We can change state when we're either killed or we have an underlying runner.
            invariant(_killed || NULL != _underlyingRunner.get());
            _state = SubplanRunner::RUNNING;
        }

        if (_killed) {
            return Runner::RUNNER_DEAD;
        }

        if (isEOF()) {
            return Runner::RUNNER_EOF;
        }

        // If we're here we should have planned already.
        invariant(SubplanRunner::RUNNING == _state);
        invariant(_underlyingRunner.get());
        return _underlyingRunner->getNext(objOut, dlOut);
    }

    bool SubplanRunner::runSubplans() {
        // This is what we annotate with the index selections and then turn into a solution.
        auto_ptr<OrMatchExpression> theOr(
            static_cast<OrMatchExpression*>(_query->root()->shallowClone()));

        // This is the skeleton of index selections that is inserted into the cache.
        auto_ptr<PlanCacheIndexTree> cacheData(new PlanCacheIndexTree());

        // We need this to extract cache-friendly index data from the index assignments.
        map<BSONObj, size_t> indexMap;
        for (size_t i = 0; i < _plannerParams.indices.size(); ++i) {
            const IndexEntry& ie = _plannerParams.indices[i];
            indexMap[ie.keyPattern] = i;
            QLOG() << "Subplanner: index " << i << " is " << ie.toString() << endl;
        }

        for (size_t i = 0; i < theOr->numChildren(); ++i) {
            // Turn the i-th child into its own query.
            MatchExpression* orChild = theOr->getChild(i);
            CanonicalQuery* orChildCQ;
            Status childCQStatus = CanonicalQuery::canonicalize(*_query,
                                                                orChild,
                                                                &orChildCQ);
            if (!childCQStatus.isOK()) {
                QLOG() << "Subplanner: Can't canonicalize subchild " << orChild->toString();
                return false;
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
                QLOG() << "Subplanner: Can't plan for subchild " << orChildCQ->toString();
                return false;
            }
            QLOG() << "Subplanner: got " << solutions.size() << " solutions";

            if (0 == solutions.size()) {
                // If one child doesn't have an indexed solution, bail out.
                QLOG() << "Subplanner: No solutions for subchild " << orChildCQ->toString();
                return false;
            }
            else if (1 == solutions.size()) {
                auto_ptr<QuerySolution> autoSoln(solutions[0]);

                // We want a well-formed *indexed* solution.
                if (NULL == autoSoln->cacheData.get()) {
                    // For example, we don't cache things for 2d indices.
                    QLOG() << "Subplanner: No cache data for subchild " << orChildCQ->toString();
                    return false;
                }

                if (SolutionCacheData::USE_INDEX_TAGS_SOLN != autoSoln->cacheData->solnType) {
                    QLOG() << "Subplanner: No indexed cache data for subchild "
                           << orChildCQ->toString();
                    return false;
                }

                // Add the index assignments to our original query.
                Status tagStatus = QueryPlanner::tagAccordingToCache(
                    orChild, autoSoln->cacheData->tree.get(), indexMap);

                if (!tagStatus.isOK()) {
                    QLOG() << "Subplanner: Failed to extract indices from subchild"
                           << orChildCQ->toString();
                    return false;
                }

                // Add the child's cache data to the cache data we're creating for the main query.
                cacheData->children.push_back(autoSoln->cacheData->tree->clone());
            }
            else {
                // N solutions, rank them.  Takes ownership of safeOrChildCQ.
                MultiPlanRunner* mpr = new MultiPlanRunner(_collection, safeOrChildCQ.release());

                // Dump all the solutions into the MPR.
                for (size_t i = 0; i < solutions.size(); ++i) {
                    WorkingSet* ws;
                    PlanStage* root;
                    verify(StageBuilder::build(_collection, *solutions[i], &root, &ws));
                    // Takes ownership of all arguments.
                    mpr->addPlan(solutions[i], root, ws);
                }

                // If we're allowed to yield, let the MPR know.
                mpr->setYieldPolicy(_policy);

                // Calling pickBestPlan can yield so we must propagate events down to the MPR.
                _underlyingRunner.reset(mpr);

                // Pull out the best plan.
                size_t bestPlan;
                BSONObj errorObj;
                if (!mpr->pickBestPlan(&bestPlan, &errorObj)) {
                    QLOG() << "Subplanner: Failed to pick best plan for subchild "
                           << orChildCQ->toString()
                           << " error obj is " << errorObj.toString();
                    return false;
                }

                // pickBestPlan can yield.  Make sure we're not dead any which way.
                if (_killed) {
                    QLOG() << "Subplanner: Killed while picking best plan for subchild "
                           << orChildCQ->toString();
                    return false;
                }

                QuerySolution* bestSoln = solutions[bestPlan];

                if (SolutionCacheData::USE_INDEX_TAGS_SOLN != bestSoln->cacheData->solnType) {
                    QLOG() << "Subplanner: No indexed cache data for subchild "
                           << orChildCQ->toString();
                    return false;
                }

                // Add the index assignments to our original query.
                Status tagStatus = QueryPlanner::tagAccordingToCache(
                    orChild, bestSoln->cacheData->tree.get(), indexMap);

                if (!tagStatus.isOK()) {
                    QLOG() << "Subplanner: Failed to extract indices from subchild"
                           << orChildCQ->toString();
                    return false;
                }

                cacheData->children.push_back(solutions[bestPlan]->cacheData->tree->clone());
            }
        }

        // Must do this before using the planner functionality.
        sortUsingTags(theOr.get());

        // Use the cached index assignments to build solnRoot.  Takes ownership of 'theOr'
        QuerySolutionNode* solnRoot = QueryPlannerAccess::buildIndexedDataAccess(
            *_query, theOr.release(), false, _plannerParams.indices);

        if (NULL == solnRoot) {
            QLOG() << "Subplanner: Failed to build indexed data path for subplanned query\n";
            return false;
        }

        QLOG() << "Subplanner: fully tagged tree is " << solnRoot->toString();

        // Takes ownership of 'solnRoot'
        QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(*_query,
                                                                      _plannerParams,
                                                                      solnRoot);

        if (NULL == soln) {
            QLOG() << "Subplanner: Failed to analyze subplanned query";
            return false;
        }

        // We want our franken-solution to be cached.
        SolutionCacheData* scd = new SolutionCacheData();
        scd->tree.reset(cacheData.release());
        soln->cacheData.reset(scd);

        QLOG() << "Subplanner: Composite solution is " << soln->toString() << endl;

        // We use one of these even if there is one plan.  We do this so that the entry is cached
        // with stats obtained in the same fashion as a competitive ranking would have obtained
        // them.
        MultiPlanRunner* mpr = new MultiPlanRunner(_collection, _query.release());
        WorkingSet* ws;
        PlanStage* root;
        verify(StageBuilder::build(_collection, *soln, &root, &ws));
        // Takes ownership of all arguments.
        mpr->addPlan(soln, root, ws);

        mpr->setYieldPolicy(_policy);
        _underlyingRunner.reset(mpr);

        return true;
    }

    bool SubplanRunner::isEOF() {
        if (_killed) {
            return true;
        }

        // If we're still planning we're not done yet.
        if (SubplanRunner::PLANNING == _state) {
            return false;
        }

        // If we're running we best have a runner.
        invariant(_underlyingRunner.get());
        return _underlyingRunner->isEOF();
    }

    void SubplanRunner::saveState() {
        if (_killed) {
            return;
        }

        // We're ranking a sub-plan via an MPR or we're streaming results from this Runner.  Either
        // way, pass on the request.
        if (NULL != _underlyingRunner.get()) {
            _underlyingRunner->saveState();
        }
    }

    bool SubplanRunner::restoreState() {
        if (_killed) {
            return false;
        }

        // We're ranking a sub-plan via an MPR or we're streaming results from this Runner.  Either
        // way, pass on the request.
        if (NULL != _underlyingRunner.get()) {
            return _underlyingRunner->restoreState();
        }

        return true;
    }

    void SubplanRunner::setYieldPolicy(Runner::YieldPolicy policy) {
        if (_killed) { return; }

        // If somebody sets this before calling work() we need to know how to set it in our subquery
        // runners.
        _policy = policy;

        if (NULL != _underlyingRunner.get()) {
            _underlyingRunner->setYieldPolicy(policy);
        }
    }

    void SubplanRunner::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (_killed) { return; }

        if (NULL != _underlyingRunner.get()) {
            _underlyingRunner->invalidate(dl, type);
        }
    }

    const std::string& SubplanRunner::ns() {
        return _ns;
    }

    void SubplanRunner::kill() {
        _killed = true;
        _collection = NULL;

        if (NULL != _underlyingRunner.get()) {
            _underlyingRunner->kill();
        }
    }

    Status SubplanRunner::getInfo(TypeExplain** explain, PlanInfo** planInfo) const {
        if (SubplanRunner::RUNNING == _state) {
            invariant(_underlyingRunner.get());
            return _underlyingRunner->getInfo(explain, planInfo);
        }
        else {
            return Status(ErrorCodes::BadValue, "no sub-plan to defer getInfo to");
        }
    }

} // namespace mongo
