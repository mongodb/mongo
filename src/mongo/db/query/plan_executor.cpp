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

#include "mongo/db/query/plan_executor.h"

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/storage/record.h"

namespace mongo {

    PlanExecutor::PlanExecutor(WorkingSet* ws, PlanStage* rt, const Collection* collection)
        : _collection(collection), _workingSet(ws), _root(rt), _killed(false) {}

    PlanExecutor::~PlanExecutor() { }

    WorkingSet* PlanExecutor::getWorkingSet() {
        return _workingSet.get();
    }

    PlanStageStats* PlanExecutor::getStats() const {
        return _root->getStats();
    }

    void PlanExecutor::saveState() {
        if (!_killed) { _root->prepareToYield(); }
    }

    bool PlanExecutor::restoreState() {
        if (!_killed) {
            _root->recoverFromYield();
        }
        return !_killed;
    }

    void PlanExecutor::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (!_killed) { _root->invalidate(dl, type); }
    }

    void PlanExecutor::setYieldPolicy(Runner::YieldPolicy policy) {
        if (Runner::YIELD_MANUAL == policy) {
            _yieldPolicy.reset();
        }
        else {
            _yieldPolicy.reset(new RunnerYieldPolicy());
        }
    }

    Runner::RunnerState PlanExecutor::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        if (_killed) { return Runner::RUNNER_DEAD; }

        for (;;) {
            // Yield, if we can yield ourselves.
            if (NULL != _yieldPolicy.get() && _yieldPolicy->shouldYield()) {
                saveState();
                _yieldPolicy->yield();
                if (_killed) { return Runner::RUNNER_DEAD; }
                restoreState();
            }

            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState code = _root->work(&id);

            if (PlanStage::ADVANCED == code) {
                // Fast count.
                if (WorkingSet::INVALID_ID == id) {
                    invariant(NULL == objOut);
                    invariant(NULL == dlOut);
                    return Runner::RUNNER_ADVANCED;
                }

                WorkingSetMember* member = _workingSet->get(id);
                bool hasRequestedData = true;

                if (NULL != objOut) {
                    if (WorkingSetMember::LOC_AND_IDX == member->state) {
                        if (1 != member->keyData.size()) {
                            _workingSet->free(id);
                            hasRequestedData = false;
                        }
                        else {
                            *objOut = member->keyData[0].keyData;
                        }
                    }
                    else if (member->hasObj()) {
                        *objOut = member->obj;
                    }
                    else {
                        _workingSet->free(id);
                        hasRequestedData = false;
                    }
                }

                if (NULL != dlOut) {
                    if (member->hasLoc()) {
                        *dlOut = member->loc;
                    }
                    else {
                        _workingSet->free(id);
                        hasRequestedData = false;
                    }
                }

                if (hasRequestedData) {
                    _workingSet->free(id);
                    return Runner::RUNNER_ADVANCED;
                }
                // This result didn't have the data the caller wanted, try again.
            }
            else if (PlanStage::NEED_TIME == code) {
                // Fall through to yield check at end of large conditional.
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

                // If we're allowed to, go to disk outside of the lock.
                if (NULL != _yieldPolicy.get()) {
                    saveState();
                    _yieldPolicy->yield(record);
                    if (_killed) { return Runner::RUNNER_DEAD; }
                    restoreState();
                }
                else {
                    // We're set to manually yield.  We go to disk in the lock.
                    record->touch();
                }

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
            else if (PlanStage::DEAD == code) {
                return Runner::RUNNER_DEAD;
            }
            else {
                verify(PlanStage::FAILURE == code);
                if (NULL != objOut) {
                    WorkingSetCommon::getStatusMemberObject(*_workingSet, id, objOut);
                }
                return Runner::RUNNER_ERROR;
            }
        }
    }

    bool PlanExecutor::isEOF() {
        return _killed || _root->isEOF();
    }

    void PlanExecutor::kill() {
        _killed = true;
    }

} // namespace mongo
