/**
 *    Copyright 2013 MongoDB Inc.
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

#include "mongo/db/query/single_solution_runner.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    SingleSolutionRunner::SingleSolutionRunner(const Collection* collection,
                                               CanonicalQuery* canonicalQuery,
                                               QuerySolution* soln,
                                               PlanStage* root,
                                               WorkingSet* ws)
        : _collection( collection ),
          _canonicalQuery(canonicalQuery),
          _solution(soln),
          _exec(new PlanExecutor(ws, root, collection)) { }

    SingleSolutionRunner::~SingleSolutionRunner() { }

    Runner::RunnerState SingleSolutionRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        return _exec->getNext(objOut, dlOut);
        // TODO: I'm not convinced we want to cache this run.  What if it's a collscan solution
        // and the user adds an index later?  We don't want to reach for this.  But if solving
        // the query is v. hard, we do want to cache it.  Maybe we can remove single solution
        // cache entries when we build an index?
    }

    bool SingleSolutionRunner::isEOF() {
        return _exec->isEOF();
    }

    void SingleSolutionRunner::saveState() {
        _exec->saveState();
    }

    bool SingleSolutionRunner::restoreState(OperationContext* opCtx) {
        return _exec->restoreState();
    }

    void SingleSolutionRunner::invalidate(const DiskLoc& dl, InvalidationType type) {
        _exec->invalidate(dl, type);
    }

    const std::string& SingleSolutionRunner::ns() {
        return _canonicalQuery->getParsed().ns();
    }

    void SingleSolutionRunner::kill() {
        _exec->kill();
        _collection = NULL;
    }

    Status SingleSolutionRunner::getInfo(TypeExplain** explain,
                                         PlanInfo** planInfo) const {
        if (NULL != explain) {
            verify(_exec.get());

            scoped_ptr<PlanStageStats> stats(_exec->getStats());
            if (NULL == stats.get()) {
                return Status(ErrorCodes::InternalError, "no stats available to explain plan");
            }

            Status status = explainPlan(*stats, explain, true /* full details */);
            if (!status.isOK()) {
                return status;
            }

            // Fill in explain fields that are accounted by on the runner level.
            TypeExplain* chosenPlan = NULL;
            explainPlan(*stats, &chosenPlan, false /* no full details */);
            if (chosenPlan) {
                (*explain)->addToAllPlans(chosenPlan);
            }
            (*explain)->setNScannedObjectsAllPlans((*explain)->getNScannedObjects());
            (*explain)->setNScannedAllPlans((*explain)->getNScanned());

            // _solution could be NULL in certain cases such as when QueryOption_OplogReplay
            // is enabled in the query flags.
            if (_solution) {
                (*explain)->setIndexFilterApplied(_solution->indexFilterApplied);
            }
        }
        else if (NULL != planInfo) {
            if (NULL == _solution.get()) {
                return Status(ErrorCodes::InternalError,
                              "no solution available for plan info");
            }
            getPlanInfo(*_solution, planInfo);
        }

        return Status::OK();
    }

} // namespace mongo
