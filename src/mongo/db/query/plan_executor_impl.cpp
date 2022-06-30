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


#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_executor_impl.h"

#include <memory>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/exception_util.h"
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
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_insert_listener.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

const OperationContext::Decoration<repl::OpTime> clientsLastKnownCommittedOpTime =
    OperationContext::declareDecoration<repl::OpTime>();

// This failpoint is also accessed by the SBE executor so we define it outside of an anonymous
// namespace.
MONGO_FAIL_POINT_DEFINE(planExecutorHangBeforeShouldWaitForInserts);

namespace {

/**
 * Constructs a PlanYieldPolicy based on 'policy'.
 */
std::unique_ptr<PlanYieldPolicy> makeYieldPolicy(PlanExecutorImpl* exec,
                                                 PlanYieldPolicy::YieldPolicy policy,
                                                 const Yieldable* yieldable) {
    switch (policy) {
        case PlanYieldPolicy::YieldPolicy::YIELD_AUTO:
        case PlanYieldPolicy::YieldPolicy::YIELD_MANUAL:
        case PlanYieldPolicy::YieldPolicy::NO_YIELD:
        case PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY:
        case PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY: {
            return std::make_unique<PlanYieldPolicyImpl>(
                exec, policy, yieldable, std::make_unique<YieldPolicyCallbacksImpl>(exec->nss()));
        }
        case PlanYieldPolicy::YieldPolicy::ALWAYS_TIME_OUT: {
            return std::make_unique<AlwaysTimeOutYieldPolicy>(
                exec->getOpCtx()->getServiceContext()->getFastClockSource());
        }
        case PlanYieldPolicy::YieldPolicy::ALWAYS_MARK_KILLED: {
            return std::make_unique<AlwaysPlanKilledYieldPolicy>(
                exec->getOpCtx()->getServiceContext()->getFastClockSource());
        }
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

PlanExecutorImpl::PlanExecutorImpl(OperationContext* opCtx,
                                   unique_ptr<WorkingSet> ws,
                                   unique_ptr<PlanStage> rt,
                                   unique_ptr<QuerySolution> qs,
                                   unique_ptr<CanonicalQuery> cq,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   const CollectionPtr& collection,
                                   bool returnOwnedBson,
                                   NamespaceString nss,
                                   PlanYieldPolicy::YieldPolicy yieldPolicy)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _expCtx(_cq ? _cq->getExpCtx() : expCtx),
      _workingSet(std::move(ws)),
      _qs(std::move(qs)),
      _root(std::move(rt)),
      _planExplainer(plan_explainer_factory::make(_root.get())),
      _mustReturnOwnedBson(returnOwnedBson),
      _nss(std::move(nss)) {
    invariant(!_expCtx || _expCtx->opCtx == _opCtx);
    invariant(!_cq || !_expCtx || _cq->getExpCtx() == _expCtx);

    // If this PlanExecutor is executing a COLLSCAN, keep a pointer directly to the COLLSCAN
    // stage. This is used for change streams in order to keep the the latest oplog timestamp
    // and post batch resume token up to date as the oplog scan progresses.
    if (auto collectionScan = getStageByType(_root.get(), STAGE_COLLSCAN)) {
        _collScanStage = static_cast<CollectionScan*>(collectionScan);
    }

    // If we don't yet have a namespace string, then initialize it from either 'collection' or
    // '_cq'.
    if (_nss.isEmpty()) {
        if (collection) {
            _nss = collection->ns();
        } else {
            invariant(_cq);
            _nss =
                _cq->getFindCommandRequest().getNamespaceOrUUID().nss().value_or(NamespaceString());
        }
    }

    // There's no point in yielding if the collection doesn't exist.
    _yieldPolicy =
        makeYieldPolicy(this,
                        collection ? yieldPolicy : PlanYieldPolicy::YieldPolicy::NO_YIELD,
                        collection ? &collection : nullptr);

    uassertStatusOK(_pickBestPlan());

    if (_qs) {
        _planExplainer->updateEnumeratorExplainInfo(_qs->_enumeratorExplainInfo);
    } else if (const MultiPlanStage* mps = getMultiPlanStage()) {
        const QuerySolution* soln = mps->bestSolution();
        _planExplainer->updateEnumeratorExplainInfo(soln->_enumeratorExplainInfo);

    } else if (auto subplan = getStageByType(_root.get(), STAGE_SUBPLAN)) {
        auto subplanStage = static_cast<SubplanStage*>(subplan);
        _planExplainer->updateEnumeratorExplainInfo(
            subplanStage->compositeSolution()->_enumeratorExplainInfo);
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

    // Finally, we might have an explicit TrialPhase. This specifies exactly two candidate
    // plans, one of which is to be evaluated. If it fails the trial, then the backup plan is
    // adopted.
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

PlanStage* PlanExecutorImpl::getRootStage() const {
    return _root.get();
}

CanonicalQuery* PlanExecutorImpl::getCanonicalQuery() const {
    return _cq.get();
}

const NamespaceString& PlanExecutorImpl::nss() const {
    return _nss;
}

const std::vector<NamespaceStringOrUUID>& PlanExecutorImpl::getSecondaryNamespaces() const {
    // Return a reference to an empty static array. This array will never contain any elements
    // because a PlanExecutorImpl is only capable of executing against a single namespace. As
    // such, it will never lock more than one namespace.
    const static std::vector<NamespaceStringOrUUID> emptyNssVector;
    return emptyNssVector;
}

OperationContext* PlanExecutorImpl::getOpCtx() const {
    return _opCtx;
}

void PlanExecutorImpl::saveState() {
    invariant(_currentState == kUsable || _currentState == kSaved);

    if (!isMarkedAsKilled()) {
        _root->saveState();
    }
    _yieldPolicy->setYieldable(nullptr);
    _currentState = kSaved;
}

void PlanExecutorImpl::restoreState(const RestoreContext& context) {
    try {
        restoreStateWithoutRetrying(context, context.collection());
    } catch (const WriteConflictException&) {
        if (!_yieldPolicy->canAutoYield())
            throw;

        // Handles retries by calling restoreStateWithoutRetrying() in a loop.
        uassertStatusOK(_yieldPolicy->yieldOrInterrupt(getOpCtx()));
    }
}

void PlanExecutorImpl::restoreStateWithoutRetrying(const RestoreContext& context,
                                                   const Yieldable* yieldable) {
    invariant(_currentState == kSaved);

    _yieldPolicy->setYieldable(yieldable);
    if (!isMarkedAsKilled()) {
        _root->restoreState(context);
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
    if (objOut && state == ExecState::ADVANCED) {
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

PlanExecutor::ExecState PlanExecutorImpl::_getNextImpl(Snapshotted<Document>* objOut,
                                                       RecordId* dlOut) {
    checkFailPointPlanExecAlwaysFails();

    invariant(_currentState == kUsable);
    if (isMarkedAsKilled()) {
        uassertStatusOK(_killStatus);
    }

    if (!_stash.empty()) {
        invariant(objOut && !dlOut);
        *objOut = {SnapshotId(), _stash.front()};
        _stash.pop_front();
        return PlanExecutor::ADVANCED;
    }

    // Incremented on every writeConflict, reset to 0 on any successful call to _root->work.
    size_t writeConflictsInARow = 0;

    // Capped insert data; declared outside the loop so we hold a shared pointer to the capped
    // insert notifier the entire time we are in the loop.  Holding a shared pointer to the
    // capped insert notifier is necessary for the notifierVersion to advance.
    insert_listener::CappedInsertNotifierData cappedInsertNotifierData;
    if (insert_listener::shouldListenForInserts(_opCtx, _cq.get())) {
        // We always construct the CappedInsertNotifier for awaitData cursors.
        cappedInsertNotifierData.notifier =
            insert_listener::getCappedInsertNotifier(_opCtx, _nss, _yieldPolicy.get());
    }
    for (;;) {
        // These are the conditions which can cause us to yield:
        //   1) The yield policy's timer elapsed, or
        //   2) some stage requested a yield, or
        //   3) we need to yield and retry due to a WriteConflictException.
        // In all cases, the actual yielding happens here.

        const auto whileYieldingFn = [&]() {
            // If we yielded because we encountered a sharding critical section, wait for the
            // critical section to end before continuing. By waiting for the critical section to be
            // exited we avoid busy spinning immediately and encountering the same critical section
            // again. It is important that this wait happens after having released the lock
            // hierarchy -- otherwise deadlocks could happen, or the very least, locks would be
            // unnecessarily held while waiting.
            const auto& shardingCriticalSection = planExecutorShardingCriticalSectionFuture(_opCtx);
            if (shardingCriticalSection) {
                OperationShardingState::waitForCriticalSectionToComplete(_opCtx,
                                                                         *shardingCriticalSection)
                    .ignore();
                planExecutorShardingCriticalSectionFuture(_opCtx).reset();
            }
        };

        if (_yieldPolicy->shouldYieldOrInterrupt(_opCtx)) {
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(_opCtx, whileYieldingFn));
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
                tassert(6297500, "Working set member has no record ID", member->hasRecordId());
                *dlOut = member->recordId;
            }

            if (hasRequestedData) {
                // transfer the metadata from the WSM to Document.
                if (objOut) {
                    if (_mustReturnOwnedBson) {
                        objOut->value() = objOut->value().getOwned();
                    }

                    if (member->metadata()) {
                        MutableDocument md(std::move(objOut->value()));
                        md.setMetadata(member->releaseMetadata());
                        objOut->setValue(md.freeze());
                    }
                }
                _workingSet->free(id);
                return PlanExecutor::ADVANCED;
            }
            // This result didn't have the data the caller wanted, try again.
        } else if (PlanStage::NEED_YIELD == code) {
            invariant(id == WorkingSet::INVALID_ID);
            if (!_yieldPolicy->canAutoYield() ||
                MONGO_unlikely(skipWriteConflictRetries.shouldFail())) {
                throwWriteConflictException();
            }

            CurOp::get(_opCtx)->debug().additiveMetrics.incrementWriteConflicts(1);
            writeConflictsInARow++;
            logWriteConflictAndBackoff(writeConflictsInARow, "plan execution", _nss.ns());

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

            if (!insert_listener::shouldWaitForInserts(_opCtx, _cq.get(), _yieldPolicy.get())) {
                return PlanExecutor::IS_EOF;
            }

            insert_listener::waitForInserts(_opCtx, _yieldPolicy.get(), &cappedInsertNotifierData);

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
    _currentState = kDisposed;
}

void PlanExecutorImpl::_executePlan() {
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

long long PlanExecutorImpl::executeCount() {
    invariant(_root->stageType() == StageType::STAGE_COUNT ||
              _root->stageType() == StageType::STAGE_RECORD_STORE_FAST_COUNT);

    _executePlan();
    auto countStats = static_cast<const CountStats*>(_root->getSpecificStats());
    return countStats->nCounted;
}

UpdateResult PlanExecutorImpl::executeUpdate() {
    _executePlan();
    return getUpdateResult();
}

UpdateResult PlanExecutorImpl::getUpdateResult() const {
    auto updateStatsToResult = [](const UpdateStats& updateStats,
                                  bool containsDotsAndDollarsField) -> UpdateResult {
        return UpdateResult(updateStats.nMatched > 0 /* Did we update at least one obj? */,
                            updateStats.isModUpdate /* Is this a $mod update? */,
                            updateStats.nModified /* number of modified docs, no no-ops */,
                            updateStats.nMatched /* # of docs matched/updated, even no-ops */,
                            updateStats.objInserted,
                            containsDotsAndDollarsField);
    };

    // If we're updating a non-existent collection, then the delete plan may have an EOF as the
    // root stage.
    if (_root->stageType() == STAGE_EOF) {
        const auto stats = std::make_unique<UpdateStats>();
        return updateStatsToResult(static_cast<const UpdateStats&>(*stats), false);
    }

    // If the collection exists, then we expect the root of the plan tree to either be an update
    // stage, or (for findAndModify) a projection stage wrapping an update stage.
    switch (_root->stageType()) {
        case StageType::STAGE_PROJECTION_DEFAULT:
        case StageType::STAGE_PROJECTION_COVERED:
        case StageType::STAGE_PROJECTION_SIMPLE: {
            invariant(_root->getChildren().size() == 1U);
            invariant(StageType::STAGE_UPDATE == _root->child()->stageType());
            const SpecificStats* stats = _root->child()->getSpecificStats();
            return updateStatsToResult(
                static_cast<const UpdateStats&>(*stats),
                static_cast<UpdateStage*>(_root->child().get())->containsDotsAndDollarsField());
        }
        default:
            invariant(StageType::STAGE_UPDATE == _root->stageType());
            const auto stats = _root->getSpecificStats();
            return updateStatsToResult(
                static_cast<const UpdateStats&>(*stats),
                static_cast<UpdateStage*>(_root.get())->containsDotsAndDollarsField());
    }
}

long long PlanExecutorImpl::executeDelete() {
    _executePlan();

    // If we're deleting from a non-existent collection, then the delete plan may have an EOF as
    // the root stage.
    if (_root->stageType() == STAGE_EOF) {
        return 0LL;
    }

    // If the collection exists, the delete plan may either have a delete stage at the root, or
    // (for findAndModify) a projection stage wrapping a delete stage.
    switch (_root->stageType()) {
        case StageType::STAGE_PROJECTION_DEFAULT:
        case StageType::STAGE_PROJECTION_COVERED:
        case StageType::STAGE_PROJECTION_SIMPLE: {
            invariant(_root->getChildren().size() == 1U);
            invariant(StageType::STAGE_DELETE == _root->child()->stageType());
            const SpecificStats* stats = _root->child()->getSpecificStats();
            return static_cast<const DeleteStats*>(stats)->docsDeleted;
        }
        default: {
            invariant(StageType::STAGE_DELETE == _root->stageType() ||
                      StageType::STAGE_BATCHED_DELETE == _root->stageType());
            const auto* deleteStats = static_cast<const DeleteStats*>(_root->getSpecificStats());
            return deleteStats->docsDeleted;
        }
    }
}

BatchedDeleteStats PlanExecutorImpl::getBatchedDeleteStats() {
    // If we're deleting on a non-existent collection, then the delete plan may have an EOF as the
    // root stage.
    if (_root->stageType() == STAGE_EOF) {
        return BatchedDeleteStats();
    }

    invariant(_root->stageType() == StageType::STAGE_BATCHED_DELETE);

    // If the collection exists, we expect the root of the plan tree to be a batched delete stage.
    // Note: findAndModify is incompatible with the batched delete stage so no need to handle
    // projection stage wrapping.
    const auto stats = _root->getSpecificStats();
    auto batchedStats = static_cast<const BatchedDeleteStats*>(stats);
    return *batchedStats;
}

void PlanExecutorImpl::stashResult(const BSONObj& obj) {
    _stash.push_front(Document{obj.getOwned()});
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

const PlanExplainer& PlanExecutorImpl::getPlanExplainer() const {
    invariant(_planExplainer);
    return *_planExplainer;
}

MultiPlanStage* PlanExecutorImpl::getMultiPlanStage() const {
    PlanStage* ps = getStageByType(_root.get(), StageType::STAGE_MULTI_PLAN);
    invariant(ps == nullptr || ps->stageType() == StageType::STAGE_MULTI_PLAN);
    return static_cast<MultiPlanStage*>(ps);
}
}  // namespace mongo
