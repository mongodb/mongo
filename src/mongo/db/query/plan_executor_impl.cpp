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


#include "mongo/db/query/plan_executor_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/cached_plan.h"
#include "mongo/db/exec/classic/collection_scan.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/classic/timeseries_modify.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_util.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_insert_listener.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/namespace_string_util.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {
const BSONObj kEmptyPBRT;
const std::vector<NamespaceStringOrUUID> kEmptyNssVector;
}  // namespace

const OperationContext::Decoration<boost::optional<repl::OpTime>> clientsLastKnownCommittedOpTime =
    OperationContext::declareDecoration<boost::optional<repl::OpTime>>();

// This failpoint is also accessed by the SBE executor so we define it outside of an anonymous
// namespace.
MONGO_FAIL_POINT_DEFINE(planExecutorHangBeforeShouldWaitForInserts);

PlanExecutorImpl::PlanExecutorImpl(OperationContext* opCtx,
                                   std::unique_ptr<WorkingSet> ws,
                                   std::unique_ptr<PlanStage> rt,
                                   std::unique_ptr<QuerySolution> qs,
                                   std::unique_ptr<CanonicalQuery> cq,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   VariantCollectionPtrOrAcquisition collection,
                                   bool returnOwnedBson,
                                   NamespaceString nss,
                                   PlanYieldPolicy::YieldPolicy yieldPolicy,
                                   boost::optional<size_t> cachedPlanHash,
                                   QueryPlanner::CostBasedRankerResult cbrResult,
                                   stage_builder::PlanStageToQsnMap planStageQsnMap,
                                   std::vector<std::unique_ptr<PlanStage>> cbrRejectedPlanStages)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _expCtx(_cq ? _cq->getExpCtx() : expCtx),
      _workingSet(std::move(ws)),
      _qs(std::move(qs)),
      _root(std::move(rt)),
      _planExplainer(plan_explainer_factory::make(_root.get(),
                                                  cachedPlanHash,
                                                  std::move(cbrResult),
                                                  std::move(planStageQsnMap),
                                                  std::move(cbrRejectedPlanStages))),
      _mustReturnOwnedBson(returnOwnedBson),
      _nss(std::move(nss)) {
    invariant(!_expCtx || _expCtx->getOperationContext() == _opCtx);
    invariant(!_cq || !_expCtx || _cq->getExpCtx() == _expCtx);

    const CollectionPtr* collectionPtr = &collection.getCollectionPtr();
    invariant(collectionPtr);
    const bool collectionExists = static_cast<bool>(*collectionPtr);

    // If we don't yet have a namespace string, then initialize it from either 'collection' or
    // '_cq'.
    if (_nss.isEmpty()) {
        if (collectionExists) {
            _nss = (*collectionPtr)->ns();
        } else {
            invariant(_cq);
            if (_cq->getFindCommandRequest().getNamespaceOrUUID().isNamespaceString()) {
                _nss = _cq->getFindCommandRequest().getNamespaceOrUUID().nss();
            }
        }
    }

    _yieldPolicy = makeClassicYieldPolicy(
        _opCtx,
        _nss,
        this,
        collectionExists ? yieldPolicy : PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
        collection);

    if (_qs) {
        _planExplainer->setQuerySolution(_qs.get());
        _planExplainer->updateEnumeratorExplainInfo(_qs->_enumeratorExplainInfo);
    } else if (const MultiPlanStage* mps = getMultiPlanStage()) {
        const QuerySolution* soln = mps->bestSolution();
        _planExplainer->setQuerySolution(soln);
        _planExplainer->updateEnumeratorExplainInfo(soln->_enumeratorExplainInfo);
    } else if (auto subplan = getStageByType(_root.get(), STAGE_SUBPLAN)) {
        auto subplanStage = static_cast<SubplanStage*>(subplan);
        _planExplainer->updateEnumeratorExplainInfo(
            subplanStage->compositeSolution()->_enumeratorExplainInfo);
    }

    // If this PlanExecutor is executing a COLLSCAN, keep a pointer directly to the COLLSCAN
    // stage. This is used for change streams in order to keep the the latest oplog timestamp
    // and post batch resume token up to date as the oplog scan progresses. Similarly, this is
    // used for oplog scans to coordinate waiting for oplog visiblity.
    if (auto collectionScan = getStageByType(_root.get(), STAGE_COLLSCAN)) {
        _collScanStage = static_cast<CollectionScan*>(collectionScan);

        if (_nss.isOplog()) {
            _oplogWaitConfig = _collScanStage->getOplogWaitConfig();
            tassert(9478713,
                    "Should have '_oplogWaitConfig' if we are scanning the oplog",
                    _oplogWaitConfig);

            // Allow waiting for oplog visiblity if our yield policy supports auto yielding.
            if (_yieldPolicy->canAutoYield() &&
                _collScanStage->params().shouldWaitForOplogVisibility) {
                _oplogWaitConfig->enableWaitingForOplogVisibility();
                _afterSnapshotAbandonFn = [&]() {
                    _waitForAllEarlierOplogWritesToBeVisible();
                };
            }
        }
    }
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
    return kEmptyNssVector;
}

OperationContext* PlanExecutorImpl::getOpCtx() const {
    return _opCtx;
}

void PlanExecutorImpl::saveState() {
    invariant(_currentState == kUsable || _currentState == kSaved);

    if (!isMarkedAsKilled()) {
        // The following call can throw, leaving '_currentState' unchanged.
        _root->saveState();
    }

    if (!_yieldPolicy->usesCollectionAcquisitions()) {
        _yieldPolicy->setYieldable(nullptr);
    }
    _currentState = kSaved;
}

void PlanExecutorImpl::restoreState(const RestoreContext& context) {
    try {
        restoreStateWithoutRetrying(context);
    } catch (const StorageUnavailableException&) {
        if (!_yieldPolicy->canAutoYield()) {
            throw;
        }

        // Handles retries by calling restoreStateWithoutRetrying() in a loop.
        uassertStatusOK(_yieldPolicy->yieldOrInterrupt(
            getOpCtx(), nullptr /* whileYieldingFn */, context.type()));
    }
}

void PlanExecutorImpl::restoreStateWithoutRetrying(const RestoreContext& context) {
    invariant(_currentState == kSaved);

    if (!_yieldPolicy->usesCollectionAcquisitions()) {
        _yieldPolicy->setYieldable(context.collection());
    }
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
        _expCtx->setOperationContext(nullptr);
    }
    _currentState = kDetached;
}

void PlanExecutorImpl::reattachToOperationContext(OperationContext* opCtx) {
    invariant(_currentState == kDetached);
    invariant(_opCtx == nullptr);

    // We're reattaching for a getMore now.  Reset the yield timer in order to prevent from
    // yielding again right away.
    _yieldPolicy->resetTimer();

    _opCtx = opCtx;
    _root->reattachToOperationContext(opCtx);
    if (_expCtx) {
        _expCtx->setOperationContext(opCtx);
    }
    _currentState = kSaved;
}

namespace {
/**
 * Helper function used to determine if we need to hang before inserts.
 */
void hangBeforeShouldWaitForInsertsIfFailpointEnabled(PlanExecutorImpl* exec) {
    if (MONGO_unlikely(
            planExecutorHangBeforeShouldWaitForInserts.shouldFail([exec](const BSONObj& data) {
                auto fpNss = NamespaceStringUtil::parseFailPointData(data, "namespace"_sd);
                return fpNss.isEmpty() || fpNss == exec->nss();
            }))) {
        LOGV2(20946,
              "PlanExecutor - planExecutorHangBeforeShouldWaitForInserts fail point "
              "enabled. Blocking until fail point is disabled");
        planExecutorHangBeforeShouldWaitForInserts.pauseWhileSet();
    }
}

/**
 * Helper function used to construct lambda passed into yielding logic.
 */
void doYield(OperationContext* opCtx) {
    // If we yielded because we encountered a sharding critical section, wait for the critical
    // section to end before continuing. By waiting for the critical section to be exited we avoid
    // busy spinning immediately and encountering the same critical section again. It is important
    // that this wait happens after having released the lock hierarchy -- otherwise deadlocks could
    // happen, or the very least, locks would be unnecessarily held while waiting.
    const auto& shardingCriticalSection = planExecutorShardingState(opCtx).criticalSectionFuture;
    if (shardingCriticalSection) {
        refresh_util::waitForCriticalSectionToComplete(opCtx, *shardingCriticalSection).ignore();
        planExecutorShardingState(opCtx).criticalSectionFuture.reset();
    }
}
}  // namespace

/**
 * This function waits for all oplog entries before the read to become visible. This must be done
 * before initializing a cursor to perform an oplog scan as that is when we establish the endpoint
 * for the cursor. Note that this function can only be called for forward, non-tailable scans.
 */
void PlanExecutorImpl::_waitForAllEarlierOplogWritesToBeVisible() {
    tassert(9478702, "This function should not be called outside of oplog scans", nss().isOplog());
    tassert(9478703, "This function should not be called outside of oplog scans", _collScanStage);
    const auto& params = _collScanStage->params();
    if (!(params.direction == CollectionScanParams::FORWARD &&
          params.shouldWaitForOplogVisibility)) {
        return;
    }

    if (_collScanStage->initializedCursor()) {
        return;
    }

    tassert(9478704, "This function should not be called on tailable cursors", !params.tailable);

    // If we do not have an oplog, we do not wait.
    LocalOplogInfo* oplogInfo = LocalOplogInfo::get(_opCtx);
    if (!oplogInfo) {
        return;
    }

    RecordStore* oplogRecordStore = oplogInfo->getRecordStore();
    if (!oplogRecordStore) {
        return;
    }

    auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    storageEngine->waitForAllEarlierOplogWritesToBeVisible(_opCtx, oplogRecordStore);
}

PlanExecutor::ExecState PlanExecutorImpl::getNext(BSONObj* objOut, RecordId* dlOut) {
    ExecState state = _getNextImpl(&_docOutput, dlOut);

    if (objOut && state == ExecState::ADVANCED) {
        const bool includeMetadata = _expCtx && _expCtx->getNeedsMerge();
        *objOut = includeMetadata ? _docOutput.toBsonWithMetaData() : _docOutput.toBson();
    }
    return state;
}

PlanExecutor::ExecState PlanExecutorImpl::getNextDocument(Document& objOut) {
    return _getNextImpl(&objOut, nullptr);
}

PlanExecutor::ExecState PlanExecutorImpl::_getNextImpl(Document* objOut, RecordId* dlOut) {
    checkFailPointPlanExecAlwaysFails();

    invariant(_currentState == kUsable);
    if (isMarkedAsKilled()) {
        uassertStatusOK(_killStatus);
    }

    if (!_stash.empty()) {
        invariant(objOut && !dlOut);
        *objOut = std::move(_stash.front());
        _stash.pop_front();
        return PlanExecutor::ADVANCED;
    }

    // The below are incremented on every WriteConflict or TemporarilyUnavailable error accordingly,
    // and reset to 0 on any successful call to _root->work.
    size_t writeConflictsInARow = 0;
    size_t tempUnavailErrorsInARow = 0;

    // Capped insert data; declared outside the loop so we hold a shared pointer to the capped
    // insert notifier the entire time we are in the loop.  Holding a shared pointer to the
    // capped insert notifier is necessary for the notifierVersion to advance.
    auto notifier = makeNotifier();

    for (;;) {
        // These are the conditions which can cause us to yield:
        //   1) The yield policy's timer elapsed, or
        //   2) some stage requested a yield, or
        //   3) we need to yield and retry due to a WriteConflictException.
        // In all cases, the actual yielding happens here.

        const auto whileYieldingFn = [&]() {
            doYield(_opCtx);
        };

        if (_yieldPolicy->shouldYieldOrInterrupt(_opCtx)) {
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(_opCtx,
                                                           whileYieldingFn,
                                                           RestoreContext::RestoreType::kYield,
                                                           _afterSnapshotAbandonFn));
        }

        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState code = _root->work(&id);

        if (code != PlanStage::NEED_YIELD) {
            writeConflictsInARow = 0;
            tempUnavailErrorsInARow = 0;
        }

        if (PlanStage::ADVANCED == code) {
            WorkingSetMember* member = _workingSet->get(id);
            bool hasRequestedData = true;

            if (nullptr != objOut) {
                if (WorkingSetMember::RID_AND_IDX == member->getState()) {
                    if (1 != member->keyData.size()) {
                        _workingSet->free(id);
                        hasRequestedData = false;
                    } else {
                        *objOut = Document{member->keyData[0].keyData};
                    }
                } else if (member->hasObj()) {
                    std::swap(*objOut, member->doc.value());
                } else {
                    _workingSet->free(id);
                    hasRequestedData = false;
                }
            }

            if (nullptr != dlOut) {
                tassert(6297500, "Working set member has no record ID", member->hasRecordId());
                *dlOut = std::move(member->recordId);
            }

            if (hasRequestedData) {
                // transfer the metadata from the WSM to Document.
                if (objOut) {
                    if (_mustReturnOwnedBson) {
                        *objOut = objOut->getOwned();
                    }

                    if (member->metadata()) {
                        MutableDocument md(std::move(*objOut));
                        md.setMetadata(member->releaseMetadata());
                        *objOut = md.freeze();
                    }
                }
                _workingSet->free(id);
                return PlanExecutor::ADVANCED;
            }
            // This result didn't have the data the caller wanted, try again.

        } else if (PlanStage::NEED_YIELD == code) {
            _handleNeedYield(writeConflictsInARow, tempUnavailErrorsInARow);

        } else if (PlanStage::NEED_TIME == code) {
            // Fall through to yield check at end of large conditional.

        } else if (_handleEOFAndExit(code, notifier)) {
            return PlanExecutor::IS_EOF;
        }
    }
}

namespace {
BSONObj makeBsonWithMetadata(Document& doc, WorkingSetMember* member) {
    if (member->metadata()) {
        MutableDocument md(std::move(doc));
        md.setMetadata(member->releaseMetadata());
        return md.freeze().toBsonWithMetaData();
    }

    return doc.toBsonWithMetaData();
}
}  // namespace

std::unique_ptr<insert_listener::Notifier> PlanExecutorImpl::makeNotifier() {
    if (insert_listener::shouldListenForInserts(_opCtx, _cq.get())) {
        // We always construct the insert_listener::Notifier for awaitData cursors.
        return insert_listener::getCappedInsertNotifier(_opCtx, _nss, _yieldPolicy.get());
    }
    return nullptr;
}

void PlanExecutorImpl::_handleNeedYield(size_t& writeConflictsInARow,
                                        size_t& tempUnavailErrorsInARow) {
    invariant(shard_role_details::getRecoveryUnit(_opCtx));

    if (_expCtx->getTemporarilyUnavailableException()) {
        _expCtx->setTemporarilyUnavailableException(false);

        if (!_yieldPolicy->canAutoYield()) {
            throwTemporarilyUnavailableException(
                "got TemporarilyUnavailable exception on a plan that "
                "cannot "
                "auto-yield");
        }

        tempUnavailErrorsInARow++;
        handleTemporarilyUnavailableException(
            _opCtx,
            tempUnavailErrorsInARow,
            "plan executor",
            NamespaceStringOrUUID(_nss),
            Status(ErrorCodes::TemporarilyUnavailable, "temporarily unavailable"),
            writeConflictsInARow);
    } else if (!_oplogWaitConfig || !_oplogWaitConfig->waitedForOplogVisiblity()) {
        // If we didn't wait for oplog visiblity, then we must be yielding because of a
        // WriteConflictException.
        if (!_yieldPolicy->canAutoYield() ||
            MONGO_unlikely(skipWriteConflictRetries.shouldFail())) {
            throwWriteConflictException(
                "Write conflict during plan execution and yielding is "
                "disabled.");
        }

        writeConflictsInARow++;
        logAndRecordWriteConflictAndBackoff(
            _opCtx, writeConflictsInARow, "plan execution", ""_sd, NamespaceStringOrUUID(_nss));
    }

    // Yield next time through the loop.
    invariant(_yieldPolicy->canAutoYield());
    _yieldPolicy->forceYield();
}

bool PlanExecutorImpl::_handleEOFAndExit(PlanStage::StageState code,
                                         std::unique_ptr<insert_listener::Notifier>& notifier) {
    invariant(PlanStage::IS_EOF == code);
    hangBeforeShouldWaitForInsertsIfFailpointEnabled(this);

    // The !notifier check is necessary because shouldWaitForInserts can return 'true' when
    // shouldListenForInserts returned 'false' (above) in the case of a deadline becoming
    // "unexpired" due to the system clock going backwards.
    if (!notifier ||
        !insert_listener::shouldWaitForInserts(_opCtx, _cq.get(), _yieldPolicy.get())) {
        // Time to exit.
        return true;
    }

    insert_listener::waitForInserts(_opCtx, _yieldPolicy.get(), notifier);
    return false;
}

size_t PlanExecutorImpl::getNextBatch(size_t batchSize, AppendBSONObjFn append) {
    if (batchSize == 0) {
        return 0;
    }

    checkFailPointPlanExecAlwaysFails();
    _checkIfKilled();

    const bool includeMetadata = _expCtx && _expCtx->getNeedsMerge();
    const bool hasAppendFn = static_cast<bool>(append);

    const auto whileYieldingFn = [opCtx = _opCtx]() {
        return doYield(opCtx);
    };
    auto notifier = makeNotifier();

    WorkingSetID id = WorkingSet::INVALID_ID;

    // The below are incremented on every WriteConflict or TemporarilyUnavailable error
    // accordingly, and reset to 0 on any successful call to _root->work.
    size_t writeConflictsInARow = 0;
    size_t tempUnavailErrorsInARow = 0;

    size_t numResults = 0;
    BSONObj objOut;

    // Handle case where previous execution stashed a result.
    if (!_stash.empty()) {
        objOut = includeMetadata ? _stash.front().toBson() : _stash.front().toBsonWithMetaData();
        _stash.pop_front();
        if (hasAppendFn) {
            append(objOut, getPostBatchResumeToken(), numResults);
        }
        numResults++;
    }

    for (;;) {
        _checkIfMustYield(whileYieldingFn);

        PlanStage::StageState code = _root->work(&id);

        if (code != PlanStage::NEED_YIELD) {
            writeConflictsInARow = 0;
            tempUnavailErrorsInARow = 0;
        }

        if (code == PlanStage::ADVANCED) {
            // Process working set member.
            WorkingSetMember* member = _workingSet->get(id);

            if (MONGO_likely(member->hasObj())) {
                if (includeMetadata) {
                    objOut = makeBsonWithMetadata(member->doc.value(), member);
                } else {
                    objOut = member->doc.value().toBson();
                }

            } else if (member->keyData.size() >= 1) {
                if (includeMetadata) {
                    _docOutput = Document{member->keyData[0].keyData};
                    objOut = makeBsonWithMetadata(_docOutput, member);
                } else {
                    objOut = member->keyData[0].keyData;
                }

            } else {
                _workingSet->free(id);
                continue;  // Try to call work() again- we didn't get what we needed.
            }

            _workingSet->free(id);

            if (MONGO_unlikely(hasAppendFn &&
                               !append(objOut, getPostBatchResumeToken(), numResults))) {
                stashResult(objOut);
                break;
            }
            numResults++;

            // Only check if the query has been killed or if we've filled up the batch once a result
            // has been produced. Doing these checks every loop can impact the performance of
            // queries that repeatedly return NEED_TIME.
            if (MONGO_unlikely(numResults >= batchSize)) {
                break;
            }

            _checkIfKilled();

        } else if (code == PlanStage::NEED_YIELD) {
            _handleNeedYield(writeConflictsInARow, tempUnavailErrorsInARow);

        } else if (code == PlanStage::NEED_TIME) {
            // Do nothing except reset counters; need more time.

        } else if (_handleEOFAndExit(code, notifier)) {
            break;
        }
    }
    return numResults;
}

bool PlanExecutorImpl::isEOF() const {
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

long long PlanExecutorImpl::executeCount() {
    invariant(_root->stageType() == StageType::STAGE_COUNT ||
              _root->stageType() == StageType::STAGE_RECORD_STORE_FAST_COUNT);

    // Iterate until EOF, returning no data.
    int numResults = getNextBatch(std::numeric_limits<int64_t>::max(), nullptr);
    tassert(
        9212603, "PlanExecutorImpl expects count plans to return no documents", numResults == 0);

    auto countStats = static_cast<const CountStats*>(_root->getSpecificStats());
    return countStats->nCounted;
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
    // stage, or (for findAndModify) a projection stage wrapping an update / TS_MODIFY stage.
    const auto updateStage = [&] {
        switch (_root->stageType()) {
            case StageType::STAGE_PROJECTION_DEFAULT:
            case StageType::STAGE_PROJECTION_COVERED:
            case StageType::STAGE_PROJECTION_SIMPLE: {
                tassert(
                    7314604,
                    fmt::format("Unexpected number of children: {}", _root->getChildren().size()),
                    _root->getChildren().size() == 1U);
                auto childStage = _root->child().get();
                tassert(7314605,
                        fmt::format("Unexpected child stage type: {}",
                                    fmt::underlying(childStage->stageType())),
                        StageType::STAGE_UPDATE == childStage->stageType() ||
                            StageType::STAGE_TIMESERIES_MODIFY == childStage->stageType());
                return childStage;
            }
            default:
                return _root.get();
        }
    }();
    switch (updateStage->stageType()) {
        case StageType::STAGE_TIMESERIES_MODIFY: {
            const auto& stats =
                static_cast<const TimeseriesModifyStats&>(*updateStage->getSpecificStats());
            return UpdateResult(
                stats.nMeasurementsModified > 0 /* Did we update at least one obj? */,
                stats.isModUpdate /* Is this a $mod update? */,
                stats.nMeasurementsModified /* number of modified docs, no no-ops */,
                stats.nMeasurementsMatched /* # of docs matched/updated, even no-ops */,
                stats.objInserted /* objInserted */,
                static_cast<TimeseriesModifyStage*>(updateStage)->containsDotsAndDollarsField());
        }
        case StageType::STAGE_UPDATE: {
            const auto& stats = static_cast<const UpdateStats&>(*updateStage->getSpecificStats());
            return updateStatsToResult(
                stats, static_cast<UpdateStage*>(updateStage)->containsDotsAndDollarsField());
        }
        default:
            MONGO_UNREACHABLE_TASSERT(7314606);
    }
}

long long PlanExecutorImpl::getDeleteResult() const {
    // If we're deleting from a non-existent collection, then the delete plan may have an EOF as
    // the root stage.
    if (_root->stageType() == STAGE_EOF) {
        return 0LL;
    }

    // If the collection exists, the delete plan may either have a delete stage at the root, or
    // (for findAndModify) a projection stage wrapping a delete / TS_MODIFY stage.
    const auto deleteStage = [&] {
        switch (_root->stageType()) {
            case StageType::STAGE_PROJECTION_DEFAULT:
            case StageType::STAGE_PROJECTION_COVERED:
            case StageType::STAGE_PROJECTION_SIMPLE: {
                tassert(
                    7308302,
                    fmt::format("Unexpected number of children: {}", _root->getChildren().size()),
                    _root->getChildren().size() == 1U);
                auto childStage = _root->child().get();
                tassert(7308303,
                        fmt::format("Unexpected child stage type: {}",
                                    fmt::underlying(childStage->stageType())),
                        StageType::STAGE_DELETE == childStage->stageType() ||
                            StageType::STAGE_TIMESERIES_MODIFY == childStage->stageType());
                return childStage;
            }
            default:
                return _root.get();
        }
    }();
    switch (deleteStage->stageType()) {
        case StageType::STAGE_TIMESERIES_MODIFY: {
            const auto& tsModifyStats =
                static_cast<const TimeseriesModifyStats&>(*deleteStage->getSpecificStats());
            return tsModifyStats.nMeasurementsModified;
        }
        case StageType::STAGE_DELETE:
        case StageType::STAGE_BATCHED_DELETE: {
            const auto& deleteStats =
                static_cast<const DeleteStats&>(*deleteStage->getSpecificStats());
            return deleteStats.docsDeleted;
        }
        default:
            MONGO_UNREACHABLE_TASSERT(7308306);
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

Status PlanExecutorImpl::getKillStatus() const {
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

bool PlanExecutorImpl::usesCollectionAcquisitions() const {
    return _yieldPolicy->usesCollectionAcquisitions();
}
}  // namespace mongo
