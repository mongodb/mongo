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
 */

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/runner.h"
#include "mongo/db/pdfile.h"

#pragma once

namespace mongo {

    /**
     * Executes a plan.  Used by a runner.  Calls work() on a plan until a result is produced.
     * Stops when the plan is EOF or if the plan errors.
     *
     * TODO: Yielding policy.  This will handle yielding internally, eventually.
     * TODO: Graceful error handling.
     * TODO: Stats, diagnostics, instrumentation, etc.
     */
    class PlanExecutor {
    public:
        PlanExecutor() : _workingSet(new WorkingSet()) { }
        PlanExecutor(WorkingSet* ws, PlanStage* rt) : _workingSet(ws), _root(rt) { }

        WorkingSet* getWorkingSet() { return _workingSet.get(); }

        /**
         * Takes ownership of root.
         */
        void setRoot(PlanStage* root) {
            verify(root);
            _root.reset(root);
        }

        void saveState() { _root->prepareToYield(); }
        void restoreState() { _root->recoverFromYield(); }
        void invalidate(const DiskLoc& dl) { _root->invalidate(dl); }

        PlanStageStats* getStats() { return _root->getStats(); }

        Runner::RunnerState getNext(BSONObj* objOut) {
            for (;;) {
                WorkingSetID id;
                PlanStage::StageState code = _root->work(&id);

                if (PlanStage::ADVANCED == code) {
                    WorkingSetMember* member = _workingSet->get(id);
                    uassert(16912, "Couldn't fetch obj from query plan",
                            WorkingSetCommon::fetch(member));
                    *objOut = member->obj;
                    _workingSet->free(id);
                    return Runner::RUNNER_ADVANCED;
                }
                else if (PlanStage::NEED_TIME == code) {
                    // TODO: Runners can't yield themselves until we rework ClientCursor to not
                    // delete itself.
                }
                else if (PlanStage::NEED_FETCH == code) {
                    // id has a loc and refers to an obj we need to fetch.
                    WorkingSetMember* member = _workingSet->get(id);

                    // This must be true for somebody to request a fetch and can only change when an
                    // invalidation happens, which is when we give up a lock.  Don't give up the
                    // lock between receiving the NEED_FETCH and actually fetching(?).
                    verify(member->hasLoc());

                    // Actually bring record into memory.
                    Record* record = member->loc.rec();
                    // TODO: We would yield ourselves here and call ClientCursor::staticYield once
                    // we rework ClientCursor to not delete itself.
                    record->touch();

                    // Record should be in memory now.  Log if it's not.
                    if (!Record::likelyInPhysicalMemory(record->dataNoThrowing())) {
                        OCCASIONALLY {
                            warning() << "Record wasn't in memory immediately after fetch: "
                                      << member->loc.toString() << endl;
                        }
                    }

                    // Note that we're not freeing id.  Fetch semantics say that we shouldn't.
                }
                else if (PlanStage::IS_EOF == code) {
                    return Runner::RUNNER_EOF;
                }
                else {
                    verify(PlanStage::FAILURE == code);
                    return Runner::RUNNER_DEAD;
                }
            }
        }

    private:
        scoped_ptr<WorkingSet> _workingSet;
        scoped_ptr<PlanStage> _root;
    };

}  // namespace mongo
