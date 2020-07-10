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
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/count_scan.h"
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/near.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/text.h"
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
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
MONGO_FAIL_POINT_DEFINE(planExecutorHangWhileYieldedInWaitForInserts);

/**
 * Constructs a PlanYieldPolicy based on 'policy'.
 */
std::unique_ptr<PlanYieldPolicy> makeYieldPolicy(PlanExecutorImpl* exec,
                                                 PlanYieldPolicy::YieldPolicy policy) {
    switch (policy) {
        case PlanYieldPolicy::YieldPolicy::YIELD_AUTO:
        case PlanYieldPolicy::YieldPolicy::YIELD_MANUAL:
        case PlanYieldPolicy::YieldPolicy::NO_YIELD:
        case PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY:
        case PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY: {
            return std::make_unique<PlanYieldPolicyImpl>(exec, policy);
        }
        case PlanYieldPolicy::YieldPolicy::ALWAYS_TIME_OUT: {
            return std::make_unique<AlwaysTimeOutYieldPolicy>(exec);
        }
        case PlanYieldPolicy::YieldPolicy::ALWAYS_MARK_KILLED: {
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

PlanExecutorImpl::PlanExecutorImpl(OperationContext* opCtx,
                                   unique_ptr<WorkingSet> ws,
                                   unique_ptr<PlanStage> rt,
                                   unique_ptr<QuerySolution> qs,
                                   unique_ptr<CanonicalQuery> cq,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   const Collection* collection,
                                   NamespaceString nss,
                                   PlanYieldPolicy::YieldPolicy yieldPolicy)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _expCtx(_cq ? _cq->getExpCtx() : expCtx),
      _workingSet(std::move(ws)),
      _qs(std::move(qs)),
      _root(std::move(rt)),
      _nss(std::move(nss)),
      // There's no point in yielding if the collection doesn't exist.
      _yieldPolicy(makeYieldPolicy(
          this, collection ? yieldPolicy : PlanYieldPolicy::YieldPolicy::NO_YIELD)) {
    invariant(!_expCtx || _expCtx->opCtx == _opCtx);
    invariant(!_cq || !_expCtx || _cq->getExpCtx() == _expCtx);

    // If this PlanExecutor is executing a COLLSCAN, keep a pointer directly to the COLLSCAN stage.
    // This is used for change streams in order to keep the the latest oplog timestamp and post
    // batch resume token up to date as the oplog scan progresses.
    if (auto collectionScan = getStageByType(_root.get(), STAGE_COLLSCAN)) {
        _collScanStage = static_cast<CollectionScan*>(collectionScan);
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

    uassertStatusOK(_pickBestPlan());
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

std::string PlanExecutor::statestr(ExecState execState) {
    switch (execState) {
        case PlanExecutor::ADVANCED:
            return "ADVANCED";
        case PlanExecutor::IS_EOF:
            return "IS_EOF";
    }
    MONGO_UNREACHABLE;
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
        uassertStatusOK(_yieldPolicy->yieldOrInterrupt(getOpCtx()));
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
    const auto state = getNextDocument(&_docOutput, dlOut);
    if (objOut) {
        const bool includeMetadata = _expCtx && _expCtx->needsMerge;
        *objOut = includeMetadata ? _docOutput.toBsonWithMetaData() : _docOutput.toBson();
    }
    return state;
}

PlanExecutor::ExecState PlanExecutorImpl::getNextDocument(Document* objOut, RecordId* dlOut) {
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

void PlanExecutorImpl::_waitForInserts(CappedInsertNotifierData* notifierData) {
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
    auto yieldResult = _yieldPolicy->yieldOrInterrupt(opCtx, [opCtx, notifierData] {
        const auto deadline = awaitDataState(opCtx).waitForInsertsDeadline;
        notifierData->notifier->waitUntil(notifierData->lastEOFVersion, deadline);
        if (MONGO_unlikely(planExecutorHangWhileYieldedInWaitForInserts.shouldFail())) {
            LOGV2(4452903,
                  "PlanExecutor - planExecutorHangWhileYieldedInWaitForInserts fail point enabled. "
                  "Blocking until fail point is disabled");
            planExecutorHangWhileYieldedInWaitForInserts.pauseWhileSet();
        }
    });
    notifierData->lastEOFVersion = currentNotifierVersion;

    uassertStatusOK(yieldResult);
}

PlanExecutor::ExecState PlanExecutorImpl::_getNextImpl(Snapshotted<Document>* objOut,
                                                       RecordId* dlOut) {
    if (MONGO_unlikely(planExecutorAlwaysFails.shouldFail())) {
        uasserted(ErrorCodes::Error(4382101),
                  "PlanExecutor hit planExecutorAlwaysFails fail point");
    }

    invariant(_currentState == kUsable);
    if (isMarkedAsKilled()) {
        uassertStatusOK(_killStatus);
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
        if (_yieldPolicy->shouldYieldOrInterrupt(_opCtx)) {
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(_opCtx));
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
        } else {
            invariant(PlanStage::IS_EOF == code);
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
                      "enabled. Blocking until fail point is disabled");
                planExecutorHangBeforeShouldWaitForInserts.pauseWhileSet();
            }
            if (!_shouldWaitForInserts()) {
                return PlanExecutor::IS_EOF;
            }
            _waitForInserts(&cappedInsertNotifierData);
            // There may be more results, keep going.
            continue;
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

void PlanExecutorImpl::executePlan() {
    invariant(_currentState == kUsable);
    Document obj;
    PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
    while (PlanExecutor::ADVANCED == state) {
        state = this->getNextDocument(&obj, nullptr);
    }

    if (isMarkedAsKilled()) {
        uassertStatusOK(_killStatus);
    }

    invariant(!isMarkedAsKilled());
    invariant(PlanExecutor::IS_EOF == state);
}

void PlanExecutorImpl::enqueue(const BSONObj& obj) {
    _stash.push(Document{obj.getOwned()});
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

Timestamp PlanExecutorImpl::getLatestOplogTimestamp() const {
    return _collScanStage ? _collScanStage->getLatestOplogTimestamp() : Timestamp{};
}

BSONObj PlanExecutorImpl::getPostBatchResumeToken() const {
    static const BSONObj kEmptyPBRT;
    return _collScanStage ? _collScanStage->getPostBatchResumeToken() : kEmptyPBRT;
}

PlanExecutor::LockPolicy PlanExecutorImpl::lockPolicy() const {
    // If this PlanExecutor is simply unspooling queued data, then there is no need to acquire
    // locks.
    if (_root->stageType() == StageType::STAGE_QUEUED_DATA) {
        return LockPolicy::kLocksInternally;
    }

    return LockPolicy::kLockExternally;
}

std::string PlanExecutorImpl::getPlanSummary() const {
    return Explain::getPlanSummary(_root.get());
}

void PlanExecutorImpl::getSummaryStats(PlanSummaryStats* statsOut) const {
    invariant(statsOut);

    // We can get some of the fields we need from the common stats stored in the
    // root stage of the plan tree.
    const CommonStats* common = _root->getCommonStats();
    statsOut->nReturned = common->advanced;

    // The other fields are aggregations over the stages in the plan tree. We flatten
    // the tree into a list and then compute these aggregations.
    std::vector<const PlanStage*> stages;
    Explain::flattenExecTree(_root.get(), &stages);

    statsOut->totalKeysExamined = 0;
    statsOut->totalDocsExamined = 0;

    for (size_t i = 0; i < stages.size(); i++) {
        statsOut->totalKeysExamined +=
            Explain::getKeysExamined(stages[i]->stageType(), stages[i]->getSpecificStats());
        statsOut->totalDocsExamined +=
            Explain::getDocsExamined(stages[i]->stageType(), stages[i]->getSpecificStats());

        if (isSortStageType(stages[i]->stageType())) {
            statsOut->hasSortStage = true;

            auto sortStage = static_cast<const SortStage*>(stages[i]);
            auto sortStats = static_cast<const SortStats*>(sortStage->getSpecificStats());
            statsOut->usedDisk = sortStats->wasDiskUsed;
        }

        if (STAGE_IXSCAN == stages[i]->stageType()) {
            const IndexScan* ixscan = static_cast<const IndexScan*>(stages[i]);
            const IndexScanStats* ixscanStats =
                static_cast<const IndexScanStats*>(ixscan->getSpecificStats());
            statsOut->indexesUsed.insert(ixscanStats->indexName);
        } else if (STAGE_COUNT_SCAN == stages[i]->stageType()) {
            const CountScan* countScan = static_cast<const CountScan*>(stages[i]);
            const CountScanStats* countScanStats =
                static_cast<const CountScanStats*>(countScan->getSpecificStats());
            statsOut->indexesUsed.insert(countScanStats->indexName);
        } else if (STAGE_IDHACK == stages[i]->stageType()) {
            const IDHackStage* idHackStage = static_cast<const IDHackStage*>(stages[i]);
            const IDHackStats* idHackStats =
                static_cast<const IDHackStats*>(idHackStage->getSpecificStats());
            statsOut->indexesUsed.insert(idHackStats->indexName);
        } else if (STAGE_DISTINCT_SCAN == stages[i]->stageType()) {
            const DistinctScan* distinctScan = static_cast<const DistinctScan*>(stages[i]);
            const DistinctScanStats* distinctScanStats =
                static_cast<const DistinctScanStats*>(distinctScan->getSpecificStats());
            statsOut->indexesUsed.insert(distinctScanStats->indexName);
        } else if (STAGE_TEXT == stages[i]->stageType()) {
            const TextStage* textStage = static_cast<const TextStage*>(stages[i]);
            const TextStats* textStats =
                static_cast<const TextStats*>(textStage->getSpecificStats());
            statsOut->indexesUsed.insert(textStats->indexName);
        } else if (STAGE_GEO_NEAR_2D == stages[i]->stageType() ||
                   STAGE_GEO_NEAR_2DSPHERE == stages[i]->stageType()) {
            const NearStage* nearStage = static_cast<const NearStage*>(stages[i]);
            const NearStats* nearStats =
                static_cast<const NearStats*>(nearStage->getSpecificStats());
            statsOut->indexesUsed.insert(nearStats->indexName);
        } else if (STAGE_CACHED_PLAN == stages[i]->stageType()) {
            const CachedPlanStage* cachedPlan = static_cast<const CachedPlanStage*>(stages[i]);
            const CachedPlanStats* cachedStats =
                static_cast<const CachedPlanStats*>(cachedPlan->getSpecificStats());
            statsOut->replanReason = cachedStats->replanReason;
        } else if (STAGE_MULTI_PLAN == stages[i]->stageType()) {
            statsOut->fromMultiPlanner = true;
        } else if (STAGE_COLLSCAN == stages[i]->stageType()) {
            statsOut->collectionScans++;
            const auto collScan = static_cast<const CollectionScan*>(stages[i]);
            const auto collScanStats =
                static_cast<const CollectionScanStats*>(collScan->getSpecificStats());
            if (!collScanStats->tailable)
                statsOut->collectionScansNonTailable++;
        }
    }
}

BSONObj PlanExecutorImpl::getStats() const {
    // Serialize all stats from the winning plan.
    auto mps = getMultiPlanStage();
    auto winningPlanStats =
        mps ? std::move(mps->getStats()->children[mps->bestPlanIdx()]) : _root->getStats();
    return Explain::statsToBSON(*winningPlanStats);
}

MultiPlanStage* PlanExecutorImpl::getMultiPlanStage() const {
    PlanStage* ps = getStageByType(_root.get(), StageType::STAGE_MULTI_PLAN);
    invariant(ps == nullptr || ps->stageType() == StageType::STAGE_MULTI_PLAN);
    return static_cast<MultiPlanStage*>(ps);
}
}  // namespace mongo
