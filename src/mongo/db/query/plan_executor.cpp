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

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_executor.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
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
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

const OperationContext::Decoration<bool> shouldWaitForInserts =
    OperationContext::declareDecoration<bool>();
const OperationContext::Decoration<repl::OpTime> clientsLastKnownCommittedOpTime =
    OperationContext::declareDecoration<repl::OpTime>();

namespace {

namespace {
MONGO_FP_DECLARE(planExecutorAlwaysFails);
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
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    const Collection* collection,
    YieldPolicy yieldPolicy) {
    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(rt), nullptr, nullptr, collection, {}, yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    NamespaceString nss,
    YieldPolicy yieldPolicy) {
    return PlanExecutor::make(opCtx,
                              std::move(ws),
                              std::move(rt),
                              nullptr,
                              nullptr,
                              nullptr,
                              std::move(nss),
                              yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    unique_ptr<CanonicalQuery> cq,
    const Collection* collection,
    YieldPolicy yieldPolicy) {
    return PlanExecutor::make(
        opCtx, std::move(ws), std::move(rt), nullptr, std::move(cq), collection, {}, yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
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
                              {},
                              yieldPolicy);
}

// static
StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    unique_ptr<QuerySolution> qs,
    unique_ptr<CanonicalQuery> cq,
    const Collection* collection,
    NamespaceString nss,
    YieldPolicy yieldPolicy) {

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec(
        new PlanExecutor(opCtx,
                         std::move(ws),
                         std::move(rt),
                         std::move(qs),
                         std::move(cq),
                         collection,
                         std::move(nss),
                         yieldPolicy),
        PlanExecutor::Deleter(opCtx, collection));

    // Perform plan selection, if necessary.
    Status status = exec->pickBestPlan(collection);
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
                           NamespaceString nss,
                           YieldPolicy yieldPolicy)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _workingSet(std::move(ws)),
      _qs(std::move(qs)),
      _root(std::move(rt)),
      _nss(std::move(nss)),
      // There's no point in yielding if the collection doesn't exist.
      _yieldPolicy(new PlanYieldPolicy(this, collection ? yieldPolicy : NO_YIELD)) {
    // We may still need to initialize _nss from either collection or _cq.
    if (!_nss.isEmpty()) {
        return;  // We already have an _nss set, so there's nothing more to do.
    }

    if (collection) {
        _nss = collection->ns();
        if (_yieldPolicy->canReleaseLocksDuringExecution()) {
            _registrationToken = collection->getCursorManager()->registerExecutor(this);
        }
    } else {
        invariant(_cq);
        _nss = _cq->getQueryRequest().nss();
    }
}

Status PlanExecutor::pickBestPlan(const Collection* collection) {
    invariant(_currentState == kUsable);

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

PlanExecutor::~PlanExecutor() {
    invariant(_currentState == kDisposed);
}

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

    return SimpleBSONObjComparator::kInstance.makeBSONObjSet();
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

    if (!isMarkedAsKilled()) {
        _root->saveState();
    }
    _currentState = kSaved;
}

bool PlanExecutor::restoreState() {
    try {
        return restoreStateWithoutRetrying();
    } catch (const WriteConflictException&) {
        if (!_yieldPolicy->canAutoYield())
            throw;

        // Handles retries by calling restoreStateWithoutRetrying() in a loop.
        return _yieldPolicy->yield();
    }
}

bool PlanExecutor::restoreStateWithoutRetrying() {
    invariant(_currentState == kSaved);

    if (!isMarkedAsKilled()) {
        _root->restoreState();
    }

    _currentState = kUsable;
    return !isMarkedAsKilled();
}

void PlanExecutor::detachFromOperationContext() {
    invariant(_currentState == kSaved);
    _opCtx = nullptr;
    _root->detachFromOperationContext();
    _currentState = kDetached;
    _everDetachedFromOperationContext = true;
}

void PlanExecutor::reattachToOperationContext(OperationContext* opCtx) {
    invariant(_currentState == kDetached);

    // We're reattaching for a getMore now.  Reset the yield timer in order to prevent from
    // yielding again right away.
    _yieldPolicy->resetTimer();

    _opCtx = opCtx;
    _root->reattachToOperationContext(opCtx);
    _currentState = kSaved;
}

void PlanExecutor::invalidate(OperationContext* opCtx, const RecordId& dl, InvalidationType type) {
    if (!isMarkedAsKilled()) {
        _root->invalidate(opCtx, dl, type);
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


bool PlanExecutor::shouldWaitForInserts() {
    // If this is an awaitData-respecting operation and we have time left and we're not interrupted,
    // we should wait for inserts.
    if (mongo::shouldWaitForInserts(_opCtx) && _opCtx->checkForInterruptNoAssert().isOK() &&
        _opCtx->getRemainingMaxTimeMicros() > Microseconds::zero()) {
        // For operations with a last committed opTime, we should not wait if the replication
        // coordinator's lastCommittedOpTime has changed.
        if (!clientsLastKnownCommittedOpTime(_opCtx).isNull()) {
            auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
            return clientsLastKnownCommittedOpTime(_opCtx) == replCoord->getLastCommittedOpTime();
        }
        return true;
    }
    return false;
}

bool PlanExecutor::waitForInserts() {
    // If we cannot yield, we should retry immediately.
    if (!_yieldPolicy->canReleaseLocksDuringExecution())
        return true;

    // We can only wait if we have a collection; otherwise retry immediately.
    dassert(_opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IS));
    auto db = dbHolder().get(_opCtx, _nss.db());
    if (!db)
        return true;
    auto collection = db->getCollection(_opCtx, _nss);
    if (!collection)
        return true;

    auto notifier = collection->getCappedInsertNotifier();
    uint64_t notifierVersion = notifier->getVersion();
    auto curOp = CurOp::get(_opCtx);
    curOp->pauseTimer();
    ON_BLOCK_EXIT([curOp] { curOp->resumeTimer(); });
    auto opCtx = _opCtx;
    bool yieldResult = _yieldPolicy->yield(nullptr, [opCtx, notifier, notifierVersion] {
        const auto timeout = opCtx->getRemainingMaxTimeMicros();
        notifier->wait(notifierVersion, timeout);
    });
    return yieldResult;
}

PlanExecutor::ExecState PlanExecutor::getNextImpl(Snapshotted<BSONObj>* objOut, RecordId* dlOut) {
    if (MONGO_FAIL_POINT(planExecutorAlwaysFails)) {
        Status status(ErrorCodes::OperationFailed,
                      str::stream() << "PlanExecutor hit planExecutorAlwaysFails fail point");
        *objOut =
            Snapshotted<BSONObj>(SnapshotId(), WorkingSetCommon::buildMemberStatusObject(status));

        return PlanExecutor::FAILURE;
    }

    invariant(_currentState == kUsable);
    if (isMarkedAsKilled()) {
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
                invariant(isMarkedAsKilled());

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
                if (!_yieldPolicy->canAutoYield())
                    throw WriteConflictException();
                CurOp::get(_opCtx)->debug().writeConflicts++;
                writeConflictsInARow++;
                WriteConflictException::logAndBackoff(
                    writeConflictsInARow, "plan execution", _nss.ns());

            } else {
                WorkingSetMember* member = _workingSet->get(id);
                invariant(member->hasFetcher());
                // Transfer ownership of the fetcher. Next time around the loop a yield will
                // happen.
                fetcher.reset(member->releaseFetcher());
            }

            // If we're allowed to, we will yield next time through the loop.
            if (_yieldPolicy->canAutoYield())
                _yieldPolicy->forceYield();
        } else if (PlanStage::NEED_TIME == code) {
            // Fall through to yield check at end of large conditional.
        } else if (PlanStage::IS_EOF == code) {
            if (shouldWaitForInserts()) {
                const bool locksReacquiredAfterYield = waitForInserts();
                if (locksReacquiredAfterYield) {
                    // There may be more results, try to get more data.
                    continue;
                }
                invariant(isMarkedAsKilled());
                if (objOut) {
                    Status status(ErrorCodes::OperationFailed,
                                  str::stream() << "Operation aborted because: " << *_killReason);
                    *objOut = Snapshotted<BSONObj>(
                        SnapshotId(), WorkingSetCommon::buildMemberStatusObject(status));
                }
                return PlanExecutor::DEAD;
            } else {
                return PlanExecutor::IS_EOF;
            }
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
    return isMarkedAsKilled() || (_stash.empty() && _root->isEOF());
}

void PlanExecutor::markAsKilled(string reason) {
    _killReason = std::move(reason);
}

void PlanExecutor::dispose(OperationContext* opCtx, CursorManager* cursorManager) {
    if (_currentState == kDisposed) {
        return;
    }

    // If we are registered with the CursorManager we need to be sure to deregister ourselves.
    // However, if we have been killed we should not attempt to deregister ourselves, since the
    // caller of markAsKilled() will have done that already, and the CursorManager may no longer
    // exist. Note that the caller's collection lock prevents us from being marked as killed during
    // this method, since any interruption event requires a lock in at least MODE_IX.
    if (cursorManager && _registrationToken && !isMarkedAsKilled()) {
        dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IS));
        cursorManager->deregisterExecutor(this);
    }
    _root->dispose(opCtx);
    _currentState = kDisposed;
}

Status PlanExecutor::executePlan() {
    invariant(_currentState == kUsable);
    BSONObj obj;
    PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
    while (PlanExecutor::ADVANCED == state) {
        state = this->getNext(&obj, NULL);
    }

    if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
        if (isMarkedAsKilled()) {
            return Status(ErrorCodes::QueryPlanKilled,
                          str::stream() << "Operation aborted because: " << *_killReason);
        }

        return Status(ErrorCodes::OperationFailed,
                      str::stream() << "Exec error: " << WorkingSetCommon::toStatusString(obj)
                                    << ", state: "
                                    << PlanExecutor::statestr(state));
    }

    invariant(!isMarkedAsKilled());
    invariant(PlanExecutor::IS_EOF == state);
    return Status::OK();
}


void PlanExecutor::enqueue(const BSONObj& obj) {
    _stash.push(obj.getOwned());
}

//
// PlanExecutor::Deleter
//

PlanExecutor::Deleter::Deleter(OperationContext* opCtx, const Collection* collection)
    : _opCtx(opCtx), _cursorManager(collection ? collection->getCursorManager() : nullptr) {}

void PlanExecutor::Deleter::operator()(PlanExecutor* execPtr) {
    try {
        invariant(_opCtx);  // It is illegal to invoke operator() on a default constructed Deleter.
        if (!_dismissed) {
            execPtr->dispose(_opCtx, _cursorManager);
        }
        delete execPtr;
    } catch (...) {
        std::terminate();
    }
}

}  // namespace mongo
