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

#include "mongo/db/query/internal_runner.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/type_explain.h"

namespace mongo {

    /** Takes ownership of all arguments. */
    InternalRunner::InternalRunner(const std::string& ns, PlanStage* root, WorkingSet* ws)
        : _ns(ns), _exec(new PlanExecutor(ws, root)), _policy(Runner::YIELD_MANUAL) {
    }

    InternalRunner::~InternalRunner() {
        if (Runner::YIELD_AUTO == _policy) {
            ClientCursor::deregisterRunner(this);
        }
    }

    Runner::RunnerState InternalRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        return _exec->getNext(objOut, dlOut);
    }

    bool InternalRunner::isEOF() {
        return _exec->isEOF();
    }

    void InternalRunner::saveState() {
        _exec->saveState();
    }

    bool InternalRunner::restoreState() {
        return _exec->restoreState();
    }

    const std::string& InternalRunner::ns() {
        return _ns;
    }

    void InternalRunner::invalidate(const DiskLoc& dl) {
        _exec->invalidate(dl);
    }

    void InternalRunner::setYieldPolicy(Runner::YieldPolicy policy) {
        // No-op.
        if (_policy == policy) { return; }

        if (Runner::YIELD_AUTO == policy) {
            // Going from manual to auto.
            ClientCursor::registerRunner(this);
        }
        else {
            // Going from auto to manual.
            ClientCursor::deregisterRunner(this);
        }

        _policy = policy;
        _exec->setYieldPolicy(policy);
    }

    void InternalRunner::kill() {
        _exec->kill();
    }

   Status InternalRunner::getExplainPlan(TypeExplain** explain) const {
        dassert(_exec.get());

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

        return Status::OK();
   }

} // namespace mongo
