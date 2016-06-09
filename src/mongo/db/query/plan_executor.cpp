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
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

namespace {

namespace {
MONGO_FP_DECLARE(planExecutorAlwaysDead);
}  // namespace

/**
 * Retrieves the first stage of a given type from the plan tree, or NULL
 * if no such stage is found.
 */
PlanStage* getStageByType(PlanStage* root, StageType type) {
    if (root->stageType() == type) {
        return root;
    }

    const auto& children = root->getChildren();
    for (size_t i = 0; i < children.size(); i++) {
        PlanStage* result = getStageByType(children[i].get(), type);
        if (result) {
            return result;
        }
    }

    return NULL;
}
}

// static
StatusWith<unique_ptr<PlanExecutor>> PlanExecutor::make(OperationContext* opCtx,
                                                        unique_ptr<WorkingSet> ws,
                                                        unique_ptr<PlanStage> rt,
                                                        const Collection* collection,
                                                        YieldPolicy yieldPolicy) {
    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(rt), nullptr, nullptr, collection, "", yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor>> PlanExecutor::make(OperationContext* opCtx,
                                                        unique_ptr<WorkingSet> ws,
                                                        unique_ptr<PlanStage> rt,
                                                        const string& ns,
                                                        YieldPolicy yieldPolicy) {
    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(rt), nullptr, nullptr, nullptr, ns, yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor>> PlanExecutor::make(OperationContext* opCtx,
                                                        unique_ptr<WorkingSet> ws,
                                                        unique_ptr<PlanStage> rt,
                                                        unique_ptr<CanonicalQuery> cq,
                                                        const Collection* collection,
                                                        YieldPolicy yieldPolicy) {
    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(rt), nullptr, std::move(cq), collection, "", yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor>> PlanExecutor::make(OperationContext* opCtx,
                                                        unique_ptr<WorkingSet> ws,
                                                        unique_ptr<PlanStage> rt,
                                                        unique_ptr<QuerySolution> qs,
                                                        unique_ptr<CanonicalQuery> cq,
                                                        const Collection* collection,
                                                        YieldPolicy yieldPolicy) {
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(rt),
                              std::move(qs),
                              std::move(cq),
                              collection,
                              "",
                              yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor>> PlanExecutor::make(OperationContext* txn,
                                                        unique_ptr<WorkingSet> ws,
                                                        unique_ptr<PlanStage> rt,
                                                        unique_ptr<QuerySolution> qs,
                                                        unique_ptr<CanonicalQuery> cq,
                                                        const Collection* collection,
                                                        const string& ns,
                                                        YieldPolicy yieldPolicy) {
    unique_ptr<PlanExecutor> exec(new PlanExecutor(
        txn, std::move(ws), std::move(rt), std::move(qs), std::move(cq), collection, ns));

    // Perform plan selection, if necessary.
    Status status = exec->pickBestPlan(yieldPolicy, collection);
    if (!status.isOK()) {
        return status;
    }

    return std::move(exec);
}

PlanExecutor::PlanExecutor(OperationContext* opCtx,
                           unique_ptr<WorkingSet> ws,
                           unique_ptr<PlanStage> rt,
                           unique_ptr<QuerySolution> qs,
                           unique_ptr<CanonicalQuery> cq,
                           const Collection* collection,
                           const string& ns)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _workingSet(std::move(ws)),
      _qs(std::move(qs)),
      _root(std::move(rt)),
      _ns(ns),
      _yieldPolicy(new PlanYieldPolicy(this, YIELD_MANUAL)) {
    // We may still need to initialize _ns from either collection or _cq.
    if (!_ns.empty()) {
        // We already have an _ns set, so there's nothing more to do.
        return;
    }

    if (collection) {
        _ns = collection->ns().ns();
    } else {
        invariant(_cq);
        _ns = _cq->getQueryRequest().ns();
    }
}

Status PlanExecutor::pickBestPlan(YieldPolicy policy, const Collection* collection) {
    invariant(_currentState == kUsable);
    // For YIELD_AUTO, this will both set an auto yield policy on the PlanExecutor and
    // register it to receive notifications.
    this->setYieldPolicy(policy, collection);

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

PlanExecutor::~PlanExecutor() {}

// static
string PlanExecutor::statestr(ExecState s) {
    if (PlanExecutor::ADVANCED == s) {
        return "ADVANCED";
    } else if (PlanExecutor::IS_EOF == s) {
        return "IS_EOF";
    } else if (PlanExecutor::DEAD == s) {
        return "DEAD";
    } else {
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

unique_ptr<PlanStageStats> PlanExecutor::getStats() const {
    return _root->getStats();
}

BSONObjSet PlanExecutor::getOutputSorts() const {
    if (_qs && _qs->root) {
        _qs->root->computeProperties();
        return _qs->root->getSort();
    }

    if (_root->stageType() == STAGE_MULTI_PLAN) {
        // If we needed a MultiPlanStage, the PlanExecutor does not own the QuerySolution. We
        // must go through the MultiPlanStage to access the output sort.
        auto multiPlanStage = static_cast<MultiPlanStage*>(_root.get());
        if (multiPlanStage->bestSolution()) {
            multiPlanStage->bestSolution()->root->computeProperties();
            return multiPlanStage->bestSolution()->root->getSort();
        }
    } else if (_root->stageType() == STAGE_SUBPLAN) {
        auto subplanStage = static_cast<SubplanStage*>(_root.get());
        if (subplanStage->compositeSolution()) {
            subplanStage->compositeSolution()->root->computeProperties();
            return subplanStage->compositeSolution()->root->getSort();
        }
    }

    return BSONObjSet();
}

OperationContext* PlanExecutor::getOpCtx() const {
    return _opCtx;
}

void PlanExecutor::saveState() {
    invariant(_currentState == kUsable || _currentState == kSaved);

    // The query stages inside this stage tree might buffer record ids (e.g. text, geoNear,
    // mergeSort, sort) which are no longer protected by the storage engine's transactional
    // boundaries.
    WorkingSetCommon::prepareForSnapshotChange(_workingSet.get());

    if (!killed()) {
        _root->saveState();
    }
    _currentState = kSaved;
}

bool PlanExecutor::restoreState() {
    try {
        return restoreStateWithoutRetrying();
    } catch (const WriteConflictException& wce) {
        if (!_yieldPolicy->allowedToYield())
            throw;

        // Handles retries by calling restoreStateWithoutRetrying() in a loop.
        return _yieldPolicy->yield();
    }
}

bool PlanExecutor::restoreStateWithoutRetrying() {
    invariant(_currentState == kSaved);

    if (!killed()) {
        _root->restoreState();
    }

    _currentState = kUsable;
    return !killed();
}

void PlanExecutor::detachFromOperationContext() {
    invariant(_currentState == kSaved);
    _opCtx = nullptr;
    _root->detachFromOperationContext();
    _currentState = kDetached;
    _everDetachedFromOperationContext = true;
}

void PlanExecutor::reattachToOperationContext(OperationContext* txn) {
    invariant(_currentState == kDetached);

    // We're reattaching for a getMore now.  Reset the yield timer in order to prevent from
    // yielding again right away.
    _yieldPolicy->resetTimer();

    _opCtx = txn;
    _root->reattachToOperationContext(txn);
    _currentState = kSaved;
}

void PlanExecutor::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    if (!killed()) {
        _root->invalidate(txn, dl, type);
    }
}

PlanExecutor::ExecState PlanExecutor::getNext(BSONObj* objOut, RecordId* dlOut) {
    Snapshotted<BSONObj> snapshotted;
    ExecState state = getNextImpl(objOut ? &snapshotted : NULL, dlOut);

    if (objOut) {
        *objOut = snapshotted.value();
    }

    return state;
}

PlanExecutor::ExecState PlanExecutor::getNextSnapshotted(Snapshotted<BSONObj>* objOut,
                                                         RecordId* dlOut) {
    // Detaching from the OperationContext means that the returned snapshot ids could be invalid.
    invariant(!_everDetachedFromOperationContext);
    return getNextImpl(objOut, dlOut);
}

PlanExecutor::ExecState PlanExecutor::getNextImpl(Snapshotted<BSONObj>* objOut, RecordId* dlOut) {
    MONGO_FAIL_POINT_BLOCK(planExecutorAlwaysDead, customKill) {
        const BSONObj& data = customKill.getData();
        BSONElement customKillNS = data["namespace"];
        if (!customKillNS || _ns == customKillNS.str()) {
            deregisterExec();
            kill("hit planExecutorAlwaysDead fail point");
        }
    }

    invariant(_currentState == kUsable);
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
    unique_ptr<RecordFetcher> fetcher;

    // Incremented on every writeConflict, reset to 0 on any successful call to _root->work.
    size_t writeConflictsInARow = 0;

    for (;;) {
        // These are the conditions which can cause us to yield:
        //   1) The yield policy's timer elapsed, or
        //   2) some stage requested a yield due to a document fetch, or
        //   3) we need to yield and retry due to a WriteConflictException.
        // In all cases, the actual yielding happens here.
        if (_yieldPolicy->shouldYield()) {
            if (!_yieldPolicy->yield(fetcher.get())) {
                // A return of false from a yield should only happen if we've been killed during the
                // yield.
                invariant(killed());

                if (NULL != objOut) {
                    Status status(ErrorCodes::OperationFailed,
                                  str::stream() << "Operation aborted because: " << *_killReason);
                    *objOut = Snapshotted<BSONObj>(
                        SnapshotId(), WorkingSetCommon::buildMemberStatusObject(status));
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
            WorkingSetMember* member = _workingSet->get(id);
            bool hasRequestedData = true;

            if (NULL != objOut) {
                if (WorkingSetMember::RID_AND_IDX == member->getState()) {
                    if (1 != member->keyData.size()) {
                        _workingSet->free(id);
                        hasRequestedData = false;
                    } else {
                        // TODO: currently snapshot ids are only associated with documents, and
                        // not with index keys.
                        *objOut = Snapshotted<BSONObj>(SnapshotId(), member->keyData[0].keyData);
                    }
                } else if (member->hasObj()) {
                    *objOut = member->obj;
                } else {
                    _workingSet->free(id);
                    hasRequestedData = false;
                }
            }

            if (NULL != dlOut) {
                if (member->hasRecordId()) {
                    *dlOut = member->recordId;
                } else {
                    _workingSet->free(id);
                    hasRequestedData = false;
                }
            }

            if (hasRequestedData) {
                _workingSet->free(id);
                return PlanExecutor::ADVANCED;
            }
            // This result didn't have the data the caller wanted, try again.
        } else if (PlanStage::NEED_YIELD == code) {
            if (id == WorkingSet::INVALID_ID) {
                if (!_yieldPolicy->allowedToYield())
                    throw WriteConflictException();
                CurOp::get(_opCtx)->debug().writeConflicts++;
                writeConflictsInARow++;
                WriteConflictException::logAndBackoff(writeConflictsInARow, "plan execution", _ns);

            } else {
                WorkingSetMember* member = _workingSet->get(id);
                invariant(member->hasFetcher());
                // Transfer ownership of the fetcher. Next time around the loop a yield will
                // happen.
                fetcher.reset(member->releaseFetcher());
            }

            // If we're allowed to, we will yield next time through the loop.
            if (_yieldPolicy->allowedToYield())
                _yieldPolicy->forceYield();
        } else if (PlanStage::NEED_TIME == code) {
            // Fall through to yield check at end of large conditional.
        } else if (PlanStage::IS_EOF == code) {
            return PlanExecutor::IS_EOF;
        } else {
            invariant(PlanStage::DEAD == code || PlanStage::FAILURE == code);

            if (NULL != objOut) {
                BSONObj statusObj;
                WorkingSetCommon::getStatusMemberObject(*_workingSet, id, &statusObj);
                *objOut = Snapshotted<BSONObj>(SnapshotId(), statusObj);
            }

            return (PlanStage::DEAD == code) ? PlanExecutor::DEAD : PlanExecutor::FAILURE;
        }
    }
}

bool PlanExecutor::isEOF() {
    invariant(_currentState == kUsable);
    return killed() || (_stash.empty() && _root->isEOF());
}

void PlanExecutor::registerExec(const Collection* collection) {
    // There's no need to register a PlanExecutor for which the underlying collection
    // doesn't exist.
    if (collection) {
        _safety.reset(new ScopedExecutorRegistration(this, collection));
    }
}

void PlanExecutor::deregisterExec() {
    _safety.reset();
}

void PlanExecutor::kill(string reason) {
    _killReason = std::move(reason);

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
    invariant(_currentState == kUsable);
    BSONObj obj;
    PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
    while (PlanExecutor::ADVANCED == state) {
        state = this->getNext(&obj, NULL);
    }

    if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
        return Status(ErrorCodes::OperationFailed,
                      str::stream() << "Exec error: " << WorkingSetCommon::toStatusString(obj)
                                    << ", state: "
                                    << PlanExecutor::statestr(state));
    }

    invariant(PlanExecutor::IS_EOF == state);
    return Status::OK();
}

const string& PlanExecutor::ns() {
    return _ns;
}

void PlanExecutor::setYieldPolicy(YieldPolicy policy,
                                  const Collection* collection,
                                  bool registerExecutor) {
    if (!collection) {
        // If the collection doesn't exist, then there's no need to yield at all.
        invariant(!_yieldPolicy->allowedToYield());
        return;
    }

    _yieldPolicy->setPolicy(policy);
    if (PlanExecutor::YIELD_AUTO == policy) {
        // Runners that yield automatically generally need to be registered so that
        // after yielding, they receive notifications of events like deletions and
        // index drops. The only exception is that a few PlanExecutors get registered
        // by ClientCursor instead of being registered here. This is unneeded if we only do
        // partial "yields" for WriteConflict retrying.
        if (registerExecutor) {
            this->registerExec(collection);
        }
    }
}

void PlanExecutor::enqueue(const BSONObj& obj) {
    _stash.push(obj.getOwned());
}

//
// ScopedExecutorRegistration
//

// PlanExecutor::ScopedExecutorRegistration
PlanExecutor::ScopedExecutorRegistration::ScopedExecutorRegistration(PlanExecutor* exec,
                                                                     const Collection* collection)
    : _exec(exec), _collection(collection) {
    invariant(_collection);
    _collection->getCursorManager()->registerExecutor(_exec);
}

PlanExecutor::ScopedExecutorRegistration::~ScopedExecutorRegistration() {
    if (_exec->killed()) {
        // If the plan executor has been killed, then it's possible that the collection
        // no longer exists.
        return;
    }
    _collection->getCursorManager()->deregisterExecutor(_exec);
}

}  // namespace mongo
