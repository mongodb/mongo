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

#include <boost/shared_ptr.hpp>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/pipeline_proxy.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/service_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/storage/record_fetcher.h"

#include "mongo/util/stacktrace.h"

namespace mongo {

    using boost::shared_ptr;
    using std::string;
    using std::vector;

    namespace {

        /**
         * Retrieves the first stage of a given type from the plan tree, or NULL
         * if no such stage is found.
         */
        PlanStage* getStageByType(PlanStage* root, StageType type) {
            if (root->stageType() == type) {
                return root;
            }

            vector<PlanStage*> children = root->getChildren();
            for (size_t i = 0; i < children.size(); i++) {
                PlanStage* result = getStageByType(children[i], type);
                if (result) {
                    return result;
                }
            }

            return NULL;
        }

    }

    // static
    Status PlanExecutor::make(OperationContext* opCtx,
                              WorkingSet* ws,
                              PlanStage* rt,
                              const Collection* collection,
                              YieldPolicy yieldPolicy,
                              PlanExecutor** out) {
        return PlanExecutor::make(opCtx, ws, rt, NULL, NULL, collection, "", yieldPolicy, out);
    }

    // static
    Status PlanExecutor::make(OperationContext* opCtx,
                              WorkingSet* ws,
                              PlanStage* rt,
                              const std::string& ns,
                              YieldPolicy yieldPolicy,
                              PlanExecutor** out) {
        return PlanExecutor::make(opCtx, ws, rt, NULL, NULL, NULL, ns, yieldPolicy, out);
    }

    // static
    Status PlanExecutor::make(OperationContext* opCtx,
                              WorkingSet* ws,
                              PlanStage* rt,
                              CanonicalQuery* cq,
                              const Collection* collection,
                              YieldPolicy yieldPolicy,
                              PlanExecutor** out) {
        return PlanExecutor::make(opCtx, ws, rt, NULL, cq, collection, "", yieldPolicy, out);
    }

    // static
    Status PlanExecutor::make(OperationContext* opCtx,
                              WorkingSet* ws,
                              PlanStage* rt,
                              QuerySolution* qs,
                              CanonicalQuery* cq,
                              const Collection* collection,
                              YieldPolicy yieldPolicy,
                              PlanExecutor** out) {
        return PlanExecutor::make(opCtx, ws, rt, qs, cq, collection, "", yieldPolicy, out);
    }

    // static
    Status PlanExecutor::make(OperationContext* opCtx,
                              WorkingSet* ws,
                              PlanStage* rt,
                              QuerySolution* qs,
                              CanonicalQuery* cq,
                              const Collection* collection,
                              const std::string& ns,
                              YieldPolicy yieldPolicy,
                              PlanExecutor** out) {
        std::auto_ptr<PlanExecutor> exec(new PlanExecutor(opCtx, ws, rt, qs, cq, collection, ns));

        // Perform plan selection, if necessary.
        Status status = exec->pickBestPlan(yieldPolicy);
        if (!status.isOK()) {
            return status;
        }

        *out = exec.release();
        return Status::OK();
    }

    PlanExecutor::PlanExecutor(OperationContext* opCtx,
                               WorkingSet* ws,
                               PlanStage* rt,
                               QuerySolution* qs,
                               CanonicalQuery* cq,
                               const Collection* collection,
                               const std::string& ns)
        : _opCtx(opCtx),
          _collection(collection),
          _cq(cq),
          _workingSet(ws),
          _qs(qs),
          _root(rt),
          _ns(ns),
          _yieldPolicy(new PlanYieldPolicy(this, YIELD_MANUAL)) {
        // We may still need to initialize _ns from either _collection or _cq.
        if (!_ns.empty()) {
            // We already have an _ns set, so there's nothing more to do.
            return;
        }

        if (NULL != _collection) {
            _ns = _collection->ns().ns();
        }
        else {
            invariant(NULL != _cq.get());
            _ns = _cq->getParsed().ns();
        }
    }

    Status PlanExecutor::pickBestPlan(YieldPolicy policy) {
        // For YIELD_AUTO, this will both set an auto yield policy on the PlanExecutor and
        // register it to receive notifications.
        this->setYieldPolicy(policy);

        // First check if we need to do subplanning.
        PlanStage* foundStage = getStageByType(_root.get(), STAGE_SUBPLAN);
        if (foundStage) {
            SubplanStage* subplan = static_cast<SubplanStage*>(foundStage);
            return subplan->pickBestPlan(_yieldPolicy.get());
        }

        // If we didn't have to do subplanning, we might still have to do regular
        // multi plan selection...
        foundStage = getStageByType(_root.get(), STAGE_MULTI_PLAN);
        if (foundStage) {
            MultiPlanStage* mps = static_cast<MultiPlanStage*>(foundStage);
            return mps->pickBestPlan(_yieldPolicy.get());
        }

        // ...or, we might have to run a plan from the cache for a trial period, falling back on
        // regular planning if the cached plan performs poorly.
        foundStage = getStageByType(_root.get(), STAGE_CACHED_PLAN);
        if (foundStage) {
            CachedPlanStage* cachedPlan = static_cast<CachedPlanStage*>(foundStage);
            return cachedPlan->pickBestPlan(_yieldPolicy.get());
        }

        // Either we chose a plan, or no plan selection was required. In both cases,
        // our work has been successfully completed.
        return Status::OK();
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
            verify(PlanExecutor::FAILURE == s);
            return "FAILURE";
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
        if (!killed()) {
            _root->saveState();
        }

        // Doc-locking storage engines drop their transactional context after saving state.
        // The query stages inside this stage tree might buffer record ids (e.g. text, geoNear,
        // mergeSort, sort) which are no longer protected by the storage engine's transactional
        // boundaries. Force-fetch the documents for any such record ids so that we have our
        // own copy in the working set.
        if (supportsDocLocking()) {
            WorkingSetCommon::prepareForSnapshotChange(_workingSet.get());
        }

        _opCtx = NULL;
    }

    bool PlanExecutor::restoreState(OperationContext* opCtx) {
        try {
            return restoreStateWithoutRetrying(opCtx);
        }
        catch (const WriteConflictException& wce) {
            if (!_yieldPolicy->allowedToYield())
                throw;

            // Handles retries by calling restoreStateWithoutRetrying() in a loop.
            return _yieldPolicy->yield(NULL);
        }
    }

    bool PlanExecutor::restoreStateWithoutRetrying(OperationContext* opCtx) {
        invariant(NULL == _opCtx);
        invariant(opCtx);

        _opCtx = opCtx;

        // We're restoring after a yield or getMore now. If we're a yielding plan executor, reset
        // the yield timer in order to prevent from yielding again right away.
        _yieldPolicy->resetTimer();

        if (!killed()) {
            _root->restoreState(opCtx);
        }

        return !killed();
    }

    void PlanExecutor::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        if (!killed()) { _root->invalidate(txn, dl, type); }
    }

    PlanExecutor::ExecState PlanExecutor::getNext(BSONObj* objOut, RecordId* dlOut) {
        Snapshotted<BSONObj> snapshotted;
        ExecState state = getNextSnapshotted(objOut ? &snapshotted : NULL, dlOut);

        if (objOut) {
            *objOut = snapshotted.value();
        }

        return state;
    }

    PlanExecutor::ExecState PlanExecutor::getNextSnapshotted(Snapshotted<BSONObj>* objOut,
                                                             RecordId* dlOut) {
        if (killed()) {
            if (NULL != objOut) {
                Status status(ErrorCodes::OperationFailed,
                              str::stream() << "Operation aborted because: " << *_killReason);
                *objOut = Snapshotted<BSONObj>(SnapshotId(),
                                               WorkingSetCommon::buildMemberStatusObject(status));
            }
            return PlanExecutor::DEAD;
        }

        if (!_stash.empty()) {
            invariant(objOut && !dlOut);
            *objOut = {SnapshotId(), _stash.front()};
            _stash.pop();
            return PlanExecutor::ADVANCED;
        }

        // When a stage requests a yield for document fetch, it gives us back a RecordFetcher*
        // to use to pull the record into memory. We take ownership of the RecordFetcher here,
        // deleting it after we've had a chance to do the fetch. For timing-based yields, we
        // just pass a NULL fetcher.
        boost::scoped_ptr<RecordFetcher> fetcher;

        // Incremented on every writeConflict, reset to 0 on any successful call to _root->work.
        size_t writeConflictsInARow = 0;

        for (;;) {
            // These are the conditions which can cause us to yield:
            //   1) The yield policy's timer elapsed, or
            //   2) some stage requested a yield due to a document fetch, or
            //   3) we need to yield and retry due to a WriteConflictException.
            // In all cases, the actual yielding happens here.
            if (_yieldPolicy->shouldYield()) {
                _yieldPolicy->yield(fetcher.get());

                if (killed()) {
                    if (NULL != objOut) {
                        Status status(ErrorCodes::OperationFailed,
                                      str::stream() << "Operation aborted because: "
                                                    << *_killReason);
                        *objOut = Snapshotted<BSONObj>(
                            SnapshotId(),
                            WorkingSetCommon::buildMemberStatusObject(status));
                    }
                    return PlanExecutor::DEAD;
                }
            }

            // We're done using the fetcher, so it should be freed. We don't want to
            // use the same RecordFetcher twice.
            fetcher.reset();

            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState code = _root->work(&id);

            if (code != PlanStage::NEED_YIELD)
                writeConflictsInARow = 0;

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
                            // TODO: currently snapshot ids are only associated with documents, and
                            // not with index keys.
                            *objOut = Snapshotted<BSONObj>(SnapshotId(),
                                                           member->keyData[0].keyData);
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
            else if (PlanStage::NEED_YIELD == code) {
                if (id == WorkingSet::INVALID_ID) {
                    if (!_yieldPolicy->allowedToYield()) throw WriteConflictException();
                    CurOp::get(_opCtx)->debug().writeConflicts++;
                    writeConflictsInARow++;
                    WriteConflictException::logAndBackoff(writeConflictsInARow,
                                                          "plan execution",
                                                          _collection->ns().ns());

                }
                else {
                    WorkingSetMember* member = _workingSet->get(id);
                    invariant(member->hasFetcher());
                    // Transfer ownership of the fetcher. Next time around the loop a yield will
                    // happen.
                    fetcher.reset(member->releaseFetcher());
                }

                // If we're allowed to, we will yield next time through the loop.
                if (_yieldPolicy->allowedToYield()) _yieldPolicy->forceYield();
            }
            else if (PlanStage::NEED_TIME == code) {
                // Fall through to yield check at end of large conditional.
            }
            else if (PlanStage::IS_EOF == code) {
                return PlanExecutor::IS_EOF;
            }
            else if (PlanStage::DEAD == code) {
                if (NULL != objOut) {
                    BSONObj statusObj;
                    WorkingSetCommon::getStatusMemberObject(*_workingSet, id, &statusObj);
                    *objOut = Snapshotted<BSONObj>(SnapshotId(), statusObj);
                }
                return PlanExecutor::DEAD;
            }
            else {
                verify(PlanStage::FAILURE == code);
                if (NULL != objOut) {
                    BSONObj statusObj;
                    WorkingSetCommon::getStatusMemberObject(*_workingSet, id, &statusObj);
                    *objOut = Snapshotted<BSONObj>(SnapshotId(), statusObj);
                }
                return PlanExecutor::FAILURE;
            }
        }
    }

    bool PlanExecutor::isEOF() {
        return killed() || (_stash.empty() && _root->isEOF());
    }

    void PlanExecutor::registerExec() {
        _safety.reset(new ScopedExecutorRegistration(this));
    }

    void PlanExecutor::deregisterExec() {
        _safety.reset();
    }

    void PlanExecutor::kill(std::string reason) {
        _killReason = std::move(reason);
        _collection = NULL;

        // XXX: PlanExecutor is designed to wrap a single execution tree. In the case of
        // aggregation queries, PlanExecutor wraps a proxy stage responsible for pulling results
        // from an aggregation pipeline. The aggregation pipeline pulls results from yet another
        // PlanExecutor. Such nested PlanExecutors require us to manually propagate kill() to
        // the "inner" executor. This is bad, and hopefully can be fixed down the line with the
        // unification of agg and query.
        //
        // The CachedPlanStage is another special case. It needs to update the plan cache from
        // its destructor. It needs to know whether it has been killed so that it can avoid
        // touching a potentially invalid plan cache in this case.
        //
        // TODO: get rid of this code block.
        {
            PlanStage* foundStage = getStageByType(_root.get(), STAGE_PIPELINE_PROXY);
            if (foundStage) {
                PipelineProxyStage* proxyStage = static_cast<PipelineProxyStage*>(foundStage);
                shared_ptr<PlanExecutor> childExec = proxyStage->getChildExecutor();
                if (childExec) {
                    childExec->kill(*_killReason);
                }
            }
        }
    }

    Status PlanExecutor::executePlan() {
        BSONObj obj;
        PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
        while (PlanExecutor::ADVANCED == state) {
            state = this->getNext(&obj, NULL);
        }

        if (PlanExecutor::DEAD == state) {
            return Status(ErrorCodes::OperationFailed, "Exec error: PlanExecutor killed");
        }
        else if (PlanExecutor::FAILURE == state) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Exec error: "
                                        << WorkingSetCommon::toStatusString(obj));
        }

        invariant(PlanExecutor::IS_EOF == state);
        return Status::OK();
    }

    const string& PlanExecutor::ns() {
        return _ns;
    }

    void PlanExecutor::setYieldPolicy(YieldPolicy policy, bool registerExecutor) {
        _yieldPolicy->setPolicy(policy);
        if (PlanExecutor::YIELD_AUTO == policy) {
            // Runners that yield automatically generally need to be registered so that
            // after yielding, they receive notifications of events like deletions and
            // index drops. The only exception is that a few PlanExecutors get registered
            // by ClientCursor instead of being registered here. This is unneeded if we only do
            // partial "yields" for WriteConflict retrying.
            if (registerExecutor) {
                this->registerExec();
            }
        }
    }

    void PlanExecutor::enqueue(const BSONObj& obj) {
        _stash.push(obj.getOwned());
    }

    //
    // ScopedExecutorRegistration
    //

    PlanExecutor::ScopedExecutorRegistration::ScopedExecutorRegistration(PlanExecutor* exec)
        : _exec(exec) {
        // Collection can be null for an EOFStage plan, or other places where registration
        // is not needed.
        if (_exec->collection()) {
            _exec->collection()->getCursorManager()->registerExecutor(exec);
        }
    }

    PlanExecutor::ScopedExecutorRegistration::~ScopedExecutorRegistration() {
        if (_exec->collection()) {
            _exec->collection()->getCursorManager()->deregisterExecutor(_exec);
        }
    }

} // namespace mongo
