/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_executor_impl.h"

#include <memory>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/change_stream_proxy.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

const OperationContext::Decoration<repl::OpTime> clientsLastKnownCommittedOpTime =
    OperationContext::declareDecoration<repl::OpTime>();

struct CappedInsertNotifierData {
    shared_ptr<CappedInsertNotifier> notifier;
    uint64_t lastEOFVersion = ~0;
};

namespace {

MONGO_FAIL_POINT_DEFINE(planExecutorAlwaysFails);
MONGO_FAIL_POINT_DEFINE(planExecutorHangBeforeShouldWaitForInserts);

/**
 * Constructs a PlanYieldPolicy based on 'policy'.
 */
std::unique_ptr<PlanYieldPolicy> makeYieldPolicy(PlanExecutor* exec,
                                                 PlanExecutor::YieldPolicy policy) {
    switch (policy) {
        case PlanExecutor::YieldPolicy::YIELD_AUTO:
        case PlanExecutor::YieldPolicy::YIELD_MANUAL:
        case PlanExecutor::YieldPolicy::NO_YIELD:
        case PlanExecutor::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY:
        case PlanExecutor::YieldPolicy::INTERRUPT_ONLY: {
            return std::make_unique<PlanYieldPolicy>(exec, policy);
        }
        case PlanExecutor::YieldPolicy::ALWAYS_TIME_OUT: {
            return std::make_unique<AlwaysTimeOutYieldPolicy>(exec);
        }
        case PlanExecutor::YieldPolicy::ALWAYS_MARK_KILLED: {
            return std::make_unique<AlwaysPlanKilledYieldPolicy>(exec);
        }
        default:
            MONGO_UNREACHABLE;
    }
}

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

    return nullptr;
}
}  // namespace

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rt,
    const Collection* collection,
    YieldPolicy yieldPolicy,
    NamespaceString nss,
    std::unique_ptr<QuerySolution> qs) {
    auto expCtx = cq->getExpCtx();
    return PlanExecutorImpl::make(expCtx->opCtx,
                                  std::move(ws),
                                  std::move(rt),
                                  std::move(qs),
                                  std::move(cq),
                                  expCtx,
                                  collection,
                                  nss,
                                  yieldPolicy);
}

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutor::make(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rt,
    const Collection* collection,
    YieldPolicy yieldPolicy,
    NamespaceString nss,
    std::unique_ptr<QuerySolution> qs) {
    return PlanExecutorImpl::make(expCtx->opCtx,
                                  std::move(ws),
                                  std::move(rt),
                                  std::move(qs),
                                  nullptr,
                                  expCtx,
                                  collection,
                                  nss,
                                  yieldPolicy);
}

StatusWith<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> PlanExecutorImpl::make(
    OperationContext* opCtx,
    unique_ptr<WorkingSet> ws,
    unique_ptr<PlanStage> rt,
    unique_ptr<QuerySolution> qs,
    unique_ptr<CanonicalQuery> cq,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const Collection* collection,
    NamespaceString nss,
    YieldPolicy yieldPolicy) {

    auto execImpl = new PlanExecutorImpl(opCtx,
                                         std::move(ws),
                                         std::move(rt),
                                         std::move(qs),
                                         std::move(cq),
                                         expCtx,
                                         collection,
                                         std::move(nss),
                                         yieldPolicy);
    PlanExecutor::Deleter planDeleter(opCtx);
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec(execImpl, std::move(planDeleter));

    // Perform plan selection, if necessary.
    Status status = execImpl->_pickBestPlan();
    if (!status.isOK()) {
        return status;
    }

    return std::move(exec);
}

PlanExecutorImpl::PlanExecutorImpl(OperationContext* opCtx,
                                   unique_ptr<WorkingSet> ws,
                                   unique_ptr<PlanStage> rt,
                                   unique_ptr<QuerySolution> qs,
                                   unique_ptr<CanonicalQuery> cq,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   const Collection* collection,
                                   NamespaceString nss,
                                   YieldPolicy yieldPolicy)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _expCtx(_cq ? _cq->getExpCtx() : expCtx),
      _workingSet(std::move(ws)),
      _qs(std::move(qs)),
      _root(std::move(rt)),
      _nss(std::move(nss)),
      // There's no point in yielding if the collection doesn't exist.
      _yieldPolicy(makeYieldPolicy(this, collection ? yieldPolicy : NO_YIELD)) {
    invariant(!_expCtx || _expCtx->opCtx == _opCtx);
    invariant(!_cq || !_expCtx || _cq->getExpCtx() == _expCtx);

    // Both ChangeStreamProxy and CollectionScan stages can provide oplog tracking info, such as
    // post batch resume token, or latest oplog timestamp. If either of these two stages is present
    // in the execution tree, then cache it for fast retrieval of the oplog info, avoiding the need
    // traverse the tree in runtime.
    if (auto changeStreamProxy = getStageByType(_root.get(), STAGE_CHANGE_STREAM_PROXY)) {
        _oplogTrackingStage = static_cast<ChangeStreamProxyStage*>(changeStreamProxy);
    } else if (auto collectionScan = getStageByType(_root.get(), STAGE_COLLSCAN)) {
        _oplogTrackingStage = static_cast<CollectionScan*>(collectionScan);
    }

    // We may still need to initialize _nss from either collection or _cq.
    if (!_nss.isEmpty()) {
        return;  // We already have an _nss set, so there's nothing more to do.
    }

    if (collection) {
        _nss = collection->ns();
    } else {
        invariant(_cq);
        _nss = _cq->getQueryRequest().nss();
    }
}

Status PlanExecutorImpl::_pickBestPlan() {
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

    // Finally, we might have an explicit TrialPhase. This specifies exactly two candidate plans,
    // one of which is to be evaluated. If it fails the trial, then the backup plan is adopted.
    foundStage = getStageByType(_root.get(), STAGE_TRIAL);
    if (foundStage) {
        TrialStage* trialStage = static_cast<TrialStage*>(foundStage);
        return trialStage->pickBestPlan(_yieldPolicy.get());
    }

    // Either we chose a plan, or no plan selection was required. In both cases,
    // our work has been successfully completed.
    return Status::OK();
}

PlanExecutorImpl::~PlanExecutorImpl() {
    invariant(_currentState == kDisposed);
}

string PlanExecutor::statestr(ExecState s) {
    if (PlanExecutor::ADVANCED == s) {
        return "ADVANCED";
    } else if (PlanExecutor::IS_EOF == s) {
        return "IS_EOF";
    } else {
        verify(PlanExecutor::FAILURE == s);
        return "FAILURE";
    }
}

WorkingSet* PlanExecutorImpl::getWorkingSet() const {
    return _workingSet.get();
}

PlanStage* PlanExecutorImpl::getRootStage() const {
    return _root.get();
}

CanonicalQuery* PlanExecutorImpl::getCanonicalQuery() const {
    return _cq.get();
}

const NamespaceString& PlanExecutorImpl::nss() const {
    return _nss;
}

OperationContext* PlanExecutorImpl::getOpCtx() const {
    return _opCtx;
}

const boost::intrusive_ptr<ExpressionContext>& PlanExecutorImpl::getExpCtx() const {
    return _expCtx;
}

void PlanExecutorImpl::saveState() {
    invariant(_currentState == kUsable || _currentState == kSaved);

    if (!isMarkedAsKilled()) {
        _root->saveState();
    }
    _currentState = kSaved;
}

void PlanExecutorImpl::restoreState() {
    try {
        restoreStateWithoutRetrying();
    } catch (const WriteConflictException&) {
        if (!_yieldPolicy->canAutoYield())
            throw;

        // Handles retries by calling restoreStateWithoutRetrying() in a loop.
        uassertStatusOK(_yieldPolicy->yieldOrInterrupt());
    }
}

void PlanExecutorImpl::restoreStateWithoutRetrying() {
    invariant(_currentState == kSaved);

    if (!isMarkedAsKilled()) {
        _root->restoreState();
    }

    _currentState = kUsable;
    uassertStatusOK(_killStatus);
}

void PlanExecutorImpl::detachFromOperationContext() {
    invariant(_currentState == kSaved);
    _opCtx = nullptr;
    _root->detachFromOperationContext();
    if (_expCtx) {
        _expCtx->opCtx = nullptr;
    }
    _currentState = kDetached;
    _everDetachedFromOperationContext = true;
}

void PlanExecutorImpl::reattachToOperationContext(OperationContext* opCtx) {
    invariant(_currentState == kDetached);

    // We're reattaching for a getMore now.  Reset the yield timer in order to prevent from
    // yielding again right away.
    _yieldPolicy->resetTimer();

    _opCtx = opCtx;
    _root->reattachToOperationContext(opCtx);
    if (_expCtx) {
        _expCtx->opCtx = opCtx;
    }
    _currentState = kSaved;
}

PlanExecutor::ExecState PlanExecutorImpl::getNext(BSONObj* objOut, RecordId* dlOut) {
    const auto state = getNext(&_docOutput, dlOut);
    if (objOut) {
        *objOut = _docOutput.toBson();
    }
    return state;
}

PlanExecutor::ExecState PlanExecutorImpl::getNext(Document* objOut, RecordId* dlOut) {
    Snapshotted<Document> snapshotted;
    if (objOut) {
        snapshotted.value() = std::move(*objOut);
    }
    ExecState state = _getNextImpl(objOut ? &snapshotted : nullptr, dlOut);

    if (objOut) {
        *objOut = std::move(snapshotted.value());
    }

    return state;
}

PlanExecutor::ExecState PlanExecutorImpl::getNextSnapshotted(Snapshotted<Document>* objOut,
                                                             RecordId* dlOut) {
    // Detaching from the OperationContext means that the returned snapshot ids could be invalid.
    invariant(!_everDetachedFromOperationContext);
    return _getNextImpl(objOut, dlOut);
}

PlanExecutor::ExecState PlanExecutorImpl::getNextSnapshotted(Snapshotted<BSONObj>* objOut,
                                                             RecordId* dlOut) {
    // Detaching from the OperationContext means that the returned snapshot ids could be invalid.
    invariant(!_everDetachedFromOperationContext);
    Snapshotted<Document> docOut;
    docOut.value() = std::move(_docOutput);
    const auto status = _getNextImpl(&docOut, dlOut);
    if (objOut) {
        *objOut = {docOut.snapshotId(), docOut.value().toBson()};
    }
    _docOutput = std::move(docOut.value());
    return status;
}

bool PlanExecutorImpl::_shouldListenForInserts() {
    return _cq && _cq->getQueryRequest().isTailableAndAwaitData() &&
        awaitDataState(_opCtx).shouldWaitForInserts && _opCtx->checkForInterruptNoAssert().isOK() &&
        awaitDataState(_opCtx).waitForInsertsDeadline >
        _opCtx->getServiceContext()->getPreciseClockSource()->now();
}

bool PlanExecutorImpl::_shouldWaitForInserts() {
    // If this is an awaitData-respecting operation and we have time left and we're not interrupted,
    // we should wait for inserts.
    if (_shouldListenForInserts()) {
        // We expect awaitData cursors to be yielding.
        invariant(_yieldPolicy->canReleaseLocksDuringExecution());

        // For operations with a last committed opTime, we should not wait if the replication
        // coordinator's lastCommittedOpTime has progressed past the client's lastCommittedOpTime.
        // In that case, we will return early so that we can inform the client of the new
        // lastCommittedOpTime immediately.
        if (!clientsLastKnownCommittedOpTime(_opCtx).isNull()) {
            auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
            return clientsLastKnownCommittedOpTime(_opCtx) >= replCoord->getLastCommittedOpTime();
        }
        return true;
    }
    return false;
}

std::shared_ptr<CappedInsertNotifier> PlanExecutorImpl::_getCappedInsertNotifier() {
    // We don't expect to need a capped insert notifier for non-yielding plans.
    invariant(_yieldPolicy->canReleaseLocksDuringExecution());

    // We can only wait if we have a collection; otherwise we should retry immediately when
    // we hit EOF.
    dassert(_opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_IS));
    auto databaseHolder = DatabaseHolder::get(_opCtx);
    auto db = databaseHolder->getDb(_opCtx, _nss.db());
    invariant(db);
    auto collection = CollectionCatalog::get(_opCtx).lookupCollectionByNamespace(_opCtx, _nss);
    invariant(collection);

    return collection->getCappedInsertNotifier();
}

PlanExecutor::ExecState PlanExecutorImpl::_waitForInserts(CappedInsertNotifierData* notifierData,
                                                          Snapshotted<Document>* errorObj) {
    invariant(notifierData->notifier);

    // The notifier wait() method will not wait unless the version passed to it matches the
    // current version of the notifier.  Since the version passed to it is the current version
    // of the notifier at the time of the previous EOF, we require two EOFs in a row with no
    // notifier version change in order to wait.  This is sufficient to ensure we never wait
    // when data is available.
    auto curOp = CurOp::get(_opCtx);
    curOp->pauseTimer();
    ON_BLOCK_EXIT([curOp] { curOp->resumeTimer(); });
    auto opCtx = _opCtx;
    uint64_t currentNotifierVersion = notifierData->notifier->getVersion();
    auto yieldResult = _yieldPolicy->yieldOrInterrupt([opCtx, notifierData] {
        const auto deadline = awaitDataState(opCtx).waitForInsertsDeadline;
        notifierData->notifier->waitUntil(notifierData->lastEOFVersion, deadline);
    });
    notifierData->lastEOFVersion = currentNotifierVersion;

    if (yieldResult.isOK()) {
        // There may be more results, try to get more data.
        return ADVANCED;
    }

    if (errorObj) {
        *errorObj = Snapshotted<Document>(SnapshotId(),
                                          WorkingSetCommon::buildMemberStatusObject(yieldResult));
    }
    return FAILURE;
}

PlanExecutor::ExecState PlanExecutorImpl::_getNextImpl(Snapshotted<Document>* objOut,
                                                       RecordId* dlOut) {
    if (MONGO_unlikely(planExecutorAlwaysFails.shouldFail())) {
        Status status(ErrorCodes::InternalError,
                      str::stream() << "PlanExecutor hit planExecutorAlwaysFails fail point");
        *objOut =
            Snapshotted<Document>(SnapshotId(), WorkingSetCommon::buildMemberStatusObject(status));

        return PlanExecutor::FAILURE;
    }

    invariant(_currentState == kUsable);
    if (isMarkedAsKilled()) {
        if (nullptr != objOut) {
            *objOut = Snapshotted<Document>(SnapshotId(),
                                            WorkingSetCommon::buildMemberStatusObject(_killStatus));
        }
        return PlanExecutor::FAILURE;
    }

    if (!_stash.empty()) {
        invariant(objOut && !dlOut);
        *objOut = {SnapshotId(), _stash.front()};
        _stash.pop();
        return PlanExecutor::ADVANCED;
    }

    // Incremented on every writeConflict, reset to 0 on any successful call to _root->work.
    size_t writeConflictsInARow = 0;

    // Capped insert data; declared outside the loop so we hold a shared pointer to the capped
    // insert notifier the entire time we are in the loop.  Holding a shared pointer to the capped
    // insert notifier is necessary for the notifierVersion to advance.
    CappedInsertNotifierData cappedInsertNotifierData;
    if (_shouldListenForInserts()) {
        // We always construct the CappedInsertNotifier for awaitData cursors.
        cappedInsertNotifierData.notifier = _getCappedInsertNotifier();
    }
    for (;;) {
        // These are the conditions which can cause us to yield:
        //   1) The yield policy's timer elapsed, or
        //   2) some stage requested a yield, or
        //   3) we need to yield and retry due to a WriteConflictException.
        // In all cases, the actual yielding happens here.
        if (_yieldPolicy->shouldYieldOrInterrupt()) {
            auto yieldStatus = _yieldPolicy->yieldOrInterrupt();
            if (!yieldStatus.isOK()) {
                if (objOut) {
                    *objOut = Snapshotted<Document>(
                        SnapshotId(), WorkingSetCommon::buildMemberStatusObject(yieldStatus));
                }
                return PlanExecutor::FAILURE;
            }
        }

        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState code = _root->work(&id);

        if (code != PlanStage::NEED_YIELD)
            writeConflictsInARow = 0;

        if (PlanStage::ADVANCED == code) {
            WorkingSetMember* member = _workingSet->get(id);
            bool hasRequestedData = true;

            if (nullptr != objOut) {
                if (WorkingSetMember::RID_AND_IDX == member->getState()) {
                    if (1 != member->keyData.size()) {
                        _workingSet->free(id);
                        hasRequestedData = false;
                    } else {
                        // TODO: currently snapshot ids are only associated with documents, and
                        // not with index keys.
                        *objOut = Snapshotted<Document>(SnapshotId(),
                                                        Document{member->keyData[0].keyData});
                    }
                } else if (member->hasObj()) {
                    std::swap(*objOut, member->doc);
                } else {
                    _workingSet->free(id);
                    hasRequestedData = false;
                }
            }

            if (nullptr != dlOut) {
                if (member->hasRecordId()) {
                    *dlOut = member->recordId;
                } else {
                    _workingSet->free(id);
                    hasRequestedData = false;
                }
            }

            if (hasRequestedData) {
                // transfer the metadata from the WSM to Document.
                if (objOut && member->metadata()) {
                    MutableDocument md(std::move(objOut->value()));
                    md.setMetadata(member->releaseMetadata());
                    objOut->setValue(md.freeze());
                }
                _workingSet->free(id);
                return PlanExecutor::ADVANCED;
            }
            // This result didn't have the data the caller wanted, try again.
        } else if (PlanStage::NEED_YIELD == code) {
            invariant(id == WorkingSet::INVALID_ID);
            if (!_yieldPolicy->canAutoYield() ||
                MONGO_unlikely(skipWriteConflictRetries.shouldFail())) {
                throw WriteConflictException();
            }

            CurOp::get(_opCtx)->debug().additiveMetrics.incrementWriteConflicts(1);
            writeConflictsInARow++;
            WriteConflictException::logAndBackoff(
                writeConflictsInARow, "plan execution", _nss.ns());

            // If we're allowed to, we will yield next time through the loop.
            if (_yieldPolicy->canAutoYield()) {
                _yieldPolicy->forceYield();
            }
        } else if (PlanStage::NEED_TIME == code) {
            // Fall through to yield check at end of large conditional.
        } else if (PlanStage::IS_EOF == code) {
            if (MONGO_unlikely(planExecutorHangBeforeShouldWaitForInserts.shouldFail(
                    [this](const BSONObj& data) {
                        if (data.hasField("namespace") &&
                            _nss != NamespaceString(data.getStringField("namespace"))) {
                            return false;
                        }
                        return true;
                    }))) {
                LOGV2(20946,
                      "PlanExecutor - planExecutorHangBeforeShouldWaitForInserts fail point "
                      "enabled. Blocking until fail point is disabled.");
                planExecutorHangBeforeShouldWaitForInserts.pauseWhileSet();
            }
            if (!_shouldWaitForInserts()) {
                return PlanExecutor::IS_EOF;
            }
            const ExecState waitResult = _waitForInserts(&cappedInsertNotifierData, objOut);
            if (waitResult == PlanExecutor::ADVANCED) {
                // There may be more results, keep going.
                continue;
            }
            return waitResult;
        } else {
            invariant(PlanStage::FAILURE == code);

            if (nullptr != objOut) {
                invariant(WorkingSet::INVALID_ID != id);
                auto statusObj = WorkingSetCommon::getStatusMemberDocument(*_workingSet, id);
                *objOut = Snapshotted<Document>(SnapshotId(), *statusObj);
            }

            return PlanExecutor::FAILURE;
        }
    }
}

bool PlanExecutorImpl::isEOF() {
    invariant(_currentState == kUsable);
    return isMarkedAsKilled() || (_stash.empty() && _root->isEOF());
}

void PlanExecutorImpl::markAsKilled(Status killStatus) {
    invariant(!killStatus.isOK());
    // If killed multiple times, only retain the first status.
    if (_killStatus.isOK()) {
        _killStatus = killStatus;
    }
}

void PlanExecutorImpl::dispose(OperationContext* opCtx) {
    if (_currentState == kDisposed) {
        return;
    }

    _root->dispose(opCtx);
    _currentState = kDisposed;
}

Status PlanExecutorImpl::executePlan() {
    invariant(_currentState == kUsable);
    Document obj;
    PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
    while (PlanExecutor::ADVANCED == state) {
        state = this->getNext(&obj, nullptr);
    }

    if (PlanExecutor::FAILURE == state) {
        if (isMarkedAsKilled()) {
            return _killStatus;
        }

        auto errorStatus = getMemberObjectStatus(obj);
        invariant(!errorStatus.isOK());
        return errorStatus.withContext(str::stream() << "Exec error resulting in state "
                                                     << PlanExecutor::statestr(state));
    }

    invariant(!isMarkedAsKilled());
    invariant(PlanExecutor::IS_EOF == state);
    return Status::OK();
}


void PlanExecutorImpl::enqueue(const Document& obj) {
    _stash.push(obj.getOwned());
}

void PlanExecutorImpl::enqueue(const BSONObj& obj) {
    enqueue(Document{obj});
}

bool PlanExecutorImpl::isMarkedAsKilled() const {
    return !_killStatus.isOK();
}

Status PlanExecutorImpl::getKillStatus() {
    invariant(isMarkedAsKilled());
    return _killStatus;
}

bool PlanExecutorImpl::isDisposed() const {
    return _currentState == kDisposed;
}

bool PlanExecutorImpl::isDetached() const {
    return _currentState == kDetached;
}

Timestamp PlanExecutorImpl::getLatestOplogTimestamp() const {
    if (!_oplogTrackingStage) {
        return {};
    }

    const auto stageType = _oplogTrackingStage->stageType();
    if (stageType == STAGE_COLLSCAN) {
        return static_cast<const CollectionScan*>(_oplogTrackingStage)->getLatestOplogTimestamp();
    } else {
        invariant(stageType == STAGE_CHANGE_STREAM_PROXY);
        return static_cast<const ChangeStreamProxyStage*>(_oplogTrackingStage)
            ->getLatestOplogTimestamp();
    }
}

BSONObj PlanExecutorImpl::getPostBatchResumeToken() const {
    static const BSONObj kEmptyPBRT;
    if (!_oplogTrackingStage) {
        return kEmptyPBRT;
    }

    const auto stageType = _oplogTrackingStage->stageType();
    if (stageType == STAGE_COLLSCAN) {
        return static_cast<const CollectionScan*>(_oplogTrackingStage)->getPostBatchResumeToken();
    } else {
        invariant(stageType == STAGE_CHANGE_STREAM_PROXY);
        return static_cast<const ChangeStreamProxyStage*>(_oplogTrackingStage)
            ->getPostBatchResumeToken();
    }
}

Status PlanExecutorImpl::getMemberObjectStatus(const Document& memberObj) const {
    return WorkingSetCommon::getMemberObjectStatus(memberObj);
}

Status PlanExecutorImpl::getMemberObjectStatus(const BSONObj& memberObj) const {
    return WorkingSetCommon::getMemberObjectStatus(memberObj);
}
}  // namespace mongo
