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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/pipeline_proxy.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"

#include "mongo/util/stacktrace.h"

namespace mongo {

    PlanExecutor::PlanExecutor(OperationContext* opCtx,
                               WorkingSet* ws,
                               PlanStage* rt,
                               const Collection* collection)
        : _opCtx(opCtx),
          _collection(collection),
          _cq(NULL),
          _workingSet(ws),
          _qs(NULL),
          _root(rt),
          _killed(false) {
        initNs();
    }

    PlanExecutor::PlanExecutor(OperationContext* opCtx,
                               WorkingSet* ws,
                               PlanStage* rt,
                               std::string ns)
        : _opCtx(opCtx),
          _collection(NULL),
          _cq(NULL),
          _workingSet(ws),
          _qs(NULL),
          _root(rt),
          _ns(ns),
          _killed(false) { }

    PlanExecutor::PlanExecutor(OperationContext* opCtx,
                               WorkingSet* ws,
                               PlanStage* rt,
                               CanonicalQuery* cq,
                               const Collection* collection)
        : _opCtx(opCtx),
          _collection(collection),
          _cq(cq),
          _workingSet(ws),
          _qs(NULL),
          _root(rt),
          _killed(false) {
        initNs();
    }

    PlanExecutor::PlanExecutor(OperationContext* opCtx,
                               WorkingSet* ws,
                               PlanStage* rt,
                               QuerySolution* qs,
                               CanonicalQuery* cq,
                               const Collection* collection)
        : _opCtx(opCtx),
          _collection(collection),
          _cq(cq),
          _workingSet(ws),
          _qs(qs),
          _root(rt),
          _killed(false) {
        initNs();
    }

    void PlanExecutor::initNs() {
        if (NULL != _collection) {
            _ns = _collection->ns().ns();
        }
        else {
            invariant(NULL != _cq.get());
            _ns = _cq->getParsed().ns();
        }
    }

    PlanExecutor::~PlanExecutor() { }

    // static
    std::string PlanExecutor::statestr(ExecState s) {
        if (PlanExecutor::ADVANCED == s) {
            return "ADVANCED";
        }
        else if (PlanExecutor::IS_EOF == s) {
            return "IS_EOF";
        }
        else if (PlanExecutor::DEAD == s) {
            return "DEAD";
        }
        else {
            verify(PlanExecutor::EXEC_ERROR == s);
            return "EXEC_ERROR";
        }
    }

    WorkingSet* PlanExecutor::getWorkingSet() const {
        return _workingSet.get();
    }

    PlanStage* PlanExecutor::getRootStage() const {
        return _root.get();
    }

    CanonicalQuery* PlanExecutor::getCanonicalQuery() const {
        return _cq.get();
    }

    PlanStageStats* PlanExecutor::getStats() const {
        return _root->getStats();
    }

    const Collection* PlanExecutor::collection() const {
        return _collection;
    }

    OperationContext* PlanExecutor::getOpCtx() const {
        return _opCtx;
    }

    void PlanExecutor::saveState() {
        if (!_killed) {
            _root->saveState();
        }

        _opCtx = NULL;
    }

    bool PlanExecutor::restoreState(OperationContext* opCtx) {
        // SERVER-15526
        // invariant(NULL == _opCtx);
        // invariant(opCtx);

        _opCtx = opCtx;

        if (!_killed) {
            _root->restoreState(opCtx);
        }

        return !_killed;
    }

    void PlanExecutor::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (!_killed) { _root->invalidate(dl, type); }
    }

    PlanExecutor::ExecState PlanExecutor::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        if (_killed) { return PlanExecutor::DEAD; }

        for (;;) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState code = _root->work(&id);

            if (PlanStage::ADVANCED == code) {
                // Fast count.
                if (WorkingSet::INVALID_ID == id) {
                    invariant(NULL == objOut);
                    invariant(NULL == dlOut);
                    return PlanExecutor::ADVANCED;
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
                    return PlanExecutor::ADVANCED;
                }
                // This result didn't have the data the caller wanted, try again.
            }
            else if (PlanStage::NEED_TIME == code) {
                // Fall through to yield check at end of large conditional.
            }
            else if (PlanStage::IS_EOF == code) {
                return PlanExecutor::IS_EOF;
            }
            else if (PlanStage::DEAD == code) {
                return PlanExecutor::DEAD;
            }
            else {
                verify(PlanStage::FAILURE == code);
                if (NULL != objOut) {
                    WorkingSetCommon::getStatusMemberObject(*_workingSet, id, objOut);
                }
                return PlanExecutor::EXEC_ERROR;
            }
        }
    }

    bool PlanExecutor::isEOF() {
        return _killed || _root->isEOF();
    }

    void PlanExecutor::registerExecInternalPlan() {
        _safety.reset(new ScopedExecutorRegistration(this));
    }

    void PlanExecutor::kill() {
        _killed = true;
        _collection = NULL;

        // XXX: PlanExecutor is designed to wrap a single execution tree. In the case of
        // aggregation queries, PlanExecutor wraps a proxy stage responsible for pulling results
        // from an aggregation pipeline. The aggregation pipeline pulls results from yet another
        // PlanExecutor. Such nested PlanExecutors require us to manually propagate kill() to
        // the "inner" executor. This is bad, and hopefully can be fixed down the line with the
        // unification of agg and query.
        //
        // TODO: get rid of this code block.
        if (STAGE_PIPELINE_PROXY == _root->stageType()) {
            PipelineProxyStage* proxyStage = static_cast<PipelineProxyStage*>(_root.get());
            shared_ptr<PlanExecutor> childExec = proxyStage->getChildExecutor();
            if (childExec) {
                childExec->kill();
            }
        }
    }

    Status PlanExecutor::executePlan() {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState code = PlanStage::NEED_TIME;
        while (PlanStage::NEED_TIME == code || PlanStage::ADVANCED == code) {
            code = _root->work(&id);
        }

        if (PlanStage::FAILURE == code) {
            BSONObj obj;
            WorkingSetCommon::getStatusMemberObject(*_workingSet, id, &obj);
            return Status(ErrorCodes::BadValue,
                          "Exec error: " + WorkingSetCommon::toStatusString(obj));
        }

        return Status::OK();
    }

    const string& PlanExecutor::ns() {
        return _ns;
    }

    void PlanExecutor::setYieldPolicy(YieldPolicy policy, bool registerExecutor) {
        // TODO: implement.
    }

    //
    // ScopedExecutorRegistration
    //

    ScopedExecutorRegistration::ScopedExecutorRegistration(PlanExecutor* exec)
        : _exec(exec) {
        // Collection can be null for an EOFStage plan, or other places where registration
        // is not needed.
        if ( _exec->collection() )
            _exec->collection()->cursorCache()->registerExecutor( exec );
    }

    ScopedExecutorRegistration::~ScopedExecutorRegistration() {
        if ( _exec->collection() )
            _exec->collection()->cursorCache()->deregisterExecutor( _exec );
    }

} // namespace mongo
