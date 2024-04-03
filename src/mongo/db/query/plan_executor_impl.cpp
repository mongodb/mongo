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


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/timeseries_modify.h"
#include "mongo/db/exec/trial_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_insert_listener.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
using namespace fmt::literals;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

const OperationContext::Decoration<boost::optional<repl::OpTime>> clientsLastKnownCommittedOpTime =
    OperationContext::declareDecoration<boost::optional<repl::OpTime>>();

// This failpoint is also accessed by the SBE executor so we define it outside of an anonymous
// namespace.
MONGO_FAIL_POINT_DEFINE(planExecutorHangBeforeShouldWaitForInserts);

PlanExecutorImpl::PlanExecutorImpl(OperationContext* opCtx,
                                   unique_ptr<WorkingSet> ws,
                                   unique_ptr<PlanStage> rt,
                                   unique_ptr<QuerySolution> qs,
                                   unique_ptr<CanonicalQuery> cq,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   VariantCollectionPtrOrAcquisition collection,
                                   bool returnOwnedBson,
                                   NamespaceString nss,
                                   PlanYieldPolicy::YieldPolicy yieldPolicy,
                                   boost::optional<size_t> cachedPlanHash)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _expCtx(_cq ? _cq->getExpCtx() : expCtx),
      _workingSet(std::move(ws)),
      _qs(std::move(qs)),
      _root(std::move(rt)),
      _planExplainer(plan_explainer_factory::make(_root.get(), cachedPlanHash)),
      _mustReturnOwnedBson(returnOwnedBson),
      _nss(std::move(nss)) {
    invariant(!_expCtx || _expCtx->opCtx == _opCtx);
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
    // and post batch resume token up to date as the oplog scan progresses.
    if (auto collectionScan = getStageByType(_root.get(), STAGE_COLLSCAN)) {
        _collScanStage = static_cast<CollectionScan*>(collectionScan);
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

    if (!_yieldPolicy->usesCollectionAcquisitions()) {
        _yieldPolicy->setYieldable(nullptr);
    }
    _currentState = kSaved;
}

void PlanExecutorImpl::restoreState(const RestoreContext& context) {
    try {
        restoreStateWithoutRetrying(context, context.collection());
    } catch (const StorageUnavailableException&) {
        if (!_yieldPolicy->canAutoYield())
            throw;

        // Handles retries by calling restoreStateWithoutRetrying() in a loop.
        uassertStatusOK(_yieldPolicy->yieldOrInterrupt(getOpCtx()));
    }
}

void PlanExecutorImpl::restoreStateWithoutRetrying(const RestoreContext& context,
                                                   const Yieldable* yieldable) {
    invariant(_currentState == kSaved);

    if (!_yieldPolicy->usesCollectionAcquisitions()) {
        _yieldPolicy->setYieldable(yieldable);
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
    const auto& shardingCriticalSection = planExecutorShardingCriticalSectionFuture(opCtx);
    if (shardingCriticalSection) {
        OperationShardingState::waitForCriticalSectionToComplete(opCtx, *shardingCriticalSection)
            .ignore();
        planExecutorShardingCriticalSectionFuture(opCtx).reset();
    }
}
}  // namespace

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

    // The below are incremented on every WriteConflict or TemporarilyUnavailable error accordingly,
    // and reset to 0 on any successful call to _root->work.
    size_t writeConflictsInARow = 0;
    size_t tempUnavailErrorsInARow = 0;

    // Capped insert data; declared outside the loop so we hold a shared pointer to the capped
    // insert notifier the entire time we are in the loop.  Holding a shared pointer to the
    // capped insert notifier is necessary for the notifierVersion to advance.
    std::unique_ptr<insert_listener::Notifier> notifier;
    if (insert_listener::shouldListenForInserts(_opCtx, _cq.get())) {
        // We always construct the insert_listener::Notifier for awaitData cursors.
        notifier = insert_listener::getCappedInsertNotifier(_opCtx, _nss, _yieldPolicy.get());
    }
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
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(_opCtx, whileYieldingFn));
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
                *dlOut = std::move(member->recordId);
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
            invariant(shard_role_details::getRecoveryUnit(_opCtx));

            if (_expCtx->getTemporarilyUnavailableException()) {
                _expCtx->setTemporarilyUnavailableException(false);

                if (!_yieldPolicy->canAutoYield()) {
                    throwTemporarilyUnavailableException(
                        "got TemporarilyUnavailable exception on a plan that cannot auto-yield");
                }

                tempUnavailErrorsInARow++;
                handleTemporarilyUnavailableException(
                    _opCtx,
                    tempUnavailErrorsInARow,
                    "plan executor",
                    NamespaceStringOrUUID(_nss),
                    ExceptionFor<ErrorCodes::TemporarilyUnavailable>(
                        Status(ErrorCodes::TemporarilyUnavailable, "temporarily unavailable")),
                    writeConflictsInARow);
            } else {
                // We're yielding because of a WriteConflictException.
                if (!_yieldPolicy->canAutoYield() ||
                    MONGO_unlikely(skipWriteConflictRetries.shouldFail())) {
                    throwWriteConflictException(
                        "Write conflict during plan execution and yielding is disabled.");
                }

                CurOp::get(_opCtx)->debug().additiveMetrics.incrementWriteConflicts(1);
                writeConflictsInARow++;
                logWriteConflictAndBackoff(
                    writeConflictsInARow, "plan execution", ""_sd, NamespaceStringOrUUID(_nss));
            }

            // Yield next time through the loop.
            invariant(_yieldPolicy->canAutoYield());
            _yieldPolicy->forceYield();
        } else if (PlanStage::NEED_TIME == code) {
            // Fall through to yield check at end of large conditional.
        } else {
            invariant(PlanStage::IS_EOF == code);
            hangBeforeShouldWaitForInsertsIfFailpointEnabled(this);

            // The !notifier check is necessary because shouldWaitForInserts can return 'true' when
            // shouldListenForInserts returned 'false' (above) in the case of a deadline becoming
            // "unexpired" due to the system clock going backwards.
            if (!notifier ||
                !insert_listener::shouldWaitForInserts(_opCtx, _cq.get(), _yieldPolicy.get())) {
                return PlanExecutor::IS_EOF;
            }

            insert_listener::waitForInserts(_opCtx, _yieldPolicy.get(), notifier);

            // There may be more results, keep going.
            continue;
        }
    }
}

class PlanExecutorImpl::GetNextWorker {
public:
    GetNextWorker(PlanExecutorImpl* exec)
        : _exec(exec), _notifier(makeNotifier(exec)), _whileYieldingFn(makeYieldingFn(exec)) {
        tassert(873400, "Illegal for GetNextWorker to be initialized with a null executor.", exec);
    }

    GetNextWorker() = delete;
    GetNextWorker(GetNextWorker&) = delete;
    ~GetNextWorker() = default;

    /**
     * Implements 'executeExhaustive()' functionality to exhaust the executor without doing anything
     * with any output documents.
     */
    void exhaustDoWork() {
        checkFailPointPlanExecAlwaysFails();

        WorkingSetID id = WorkingSet::INVALID_ID;

        for (;;) {
            checkIfKilledOrMustYield();

            // We don't need the BSON in this loop, so we don't go to the trouble of materializing
            // it or doing anything with it.
            _code = _exec->_root->work(&id);

            if (MONGO_likely(_code == PlanStage::ADVANCED)) {
                // Free WSM.
                _exec->_workingSet->free(id);

                // Reset counters on successfull calls to doWork().
                _writeConflictsInARow = 0;
                _tempUnavailErrorsInARow = 0;
                continue;
            }

            if (MONGO_unlikely(shouldFinish())) {
                break;
            }
        }
    }

    /**
     * Implements 'getNextBatch()' functionality. It is illegal to call this if the GetNextWorker
     * was initialized with a null builder/ resource counter.
     */
    void batchedDoWork(const size_t batchSize, const bool includeMetadata, AppendBSONObjFn append) {
        if (batchSize == 0) {
            return;
        }

        checkFailPointPlanExecAlwaysFails();

        WorkingSetID id = WorkingSet::INVALID_ID;
        WorkingSetMember* member;

        // Handle case where previous execution stashed a result.
        if (!_exec->_stash.empty()) {
            _objOut = includeMetadata ? _exec->_stash.front().toBson()
                                      : _exec->_stash.front().toBsonWithMetaData();
            _exec->_stash.pop_front();
            append(_objOut, _exec->getPostBatchResumeToken(), _numResults);
            _numResults++;
        }

        for (;;) {
            if (MONGO_unlikely(_numResults >= batchSize)) {
                return;
            }

            checkIfKilledOrMustYield();

            if (MONGO_likely((_code = _exec->_root->work(&id)) == PlanStage::ADVANCED)) {
                // Process working set member.
                member = _exec->_workingSet->get(id);
                if (MONGO_likely(member->hasObj())) {
                    if (includeMetadata) {
                        makeBsonWithMetadata(member->doc.value(), member);
                    } else {
                        _objOut = member->doc.value().toBson();
                    }

                } else if (member->keyData.size() >= 1) {
                    if (includeMetadata) {
                        _exec->_docOutput = Document{member->keyData[0].keyData};
                        makeBsonWithMetadata(_exec->_docOutput, member);
                    } else {
                        _objOut = member->keyData[0].keyData;
                    }

                } else {
                    _exec->_workingSet->free(id);
                    continue;  // Try to call work() again- we didn't get what we needed.
                }

                _exec->_workingSet->free(id);

                if (MONGO_unlikely(
                        !append(_objOut, _exec->getPostBatchResumeToken(), _numResults))) {
                    _exec->stashResult(_objOut);
                    return;
                }
                _numResults++;

            } else if (MONGO_unlikely(shouldFinish())) {
                return;
            }
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE size_t count() const {
        return _numResults;
    }

private:
    static std::unique_ptr<insert_listener::Notifier> makeNotifier(PlanExecutorImpl* exec) {
        if (insert_listener::shouldListenForInserts(exec->_opCtx, exec->_cq.get())) {
            // We always construct the insert_listener::Notifier for awaitData cursors.
            return insert_listener::getCappedInsertNotifier(
                exec->_opCtx, exec->_nss, exec->_yieldPolicy.get());
        }
        return nullptr;
    }

    static std::function<void()> makeYieldingFn(PlanExecutorImpl* exec) {
        return [opCtx = exec->_opCtx]() {
            return doYield(opCtx);
        };
    }

    MONGO_COMPILER_ALWAYS_INLINE void checkIfKilledOrMustYield() {
        if (MONGO_unlikely(_exec->isMarkedAsKilled())) {
            uassertStatusOK(_exec->_killStatus);
        }

        // These are the conditions which can cause us to yield:
        //   1) The yield policy's timer elapsed, or
        //   2) some stage requested a yield, or
        //   3) we need to yield and retry due to a WriteConflictException.
        // In all cases, the actual yielding happens here.
        if (MONGO_unlikely(_exec->_yieldPolicy->shouldYieldOrInterrupt(_exec->_opCtx))) {
            uassertStatusOK(_exec->_yieldPolicy->yieldOrInterrupt(_exec->_opCtx, _whileYieldingFn));
        }
    }

    MONGO_COMPILER_NOINLINE void handleNeedYield() {
        invariant(shard_role_details::getRecoveryUnit(_exec->_opCtx));

        if (_exec->_expCtx->getTemporarilyUnavailableException()) {
            _exec->_expCtx->setTemporarilyUnavailableException(false);

            if (!_exec->_yieldPolicy->canAutoYield()) {
                throwTemporarilyUnavailableException(
                    "got TemporarilyUnavailable exception on a plan that "
                    "cannot "
                    "auto-yield");
            }

            _tempUnavailErrorsInARow++;
            handleTemporarilyUnavailableException(
                _exec->_opCtx,
                _tempUnavailErrorsInARow,
                "plan executor",
                NamespaceStringOrUUID(_exec->_nss),
                ExceptionFor<ErrorCodes::TemporarilyUnavailable>(
                    Status(ErrorCodes::TemporarilyUnavailable, "temporarily unavailable")),
                _writeConflictsInARow);

        } else {
            // We're yielding because of a WriteConflictException.
            if (!_exec->_yieldPolicy->canAutoYield() ||
                MONGO_unlikely(skipWriteConflictRetries.shouldFail())) {
                throwWriteConflictException(
                    "Write conflict during plan execution and yielding is "
                    "disabled.");
            }

            CurOp::get(_exec->_opCtx)->debug().additiveMetrics.incrementWriteConflicts(1);
            _writeConflictsInARow++;
            logWriteConflictAndBackoff(
                _writeConflictsInARow, "plan execution", ""_sd, NamespaceStringOrUUID(_exec->_nss));
        }

        // Yield next time through the loop.
        invariant(_exec->_yieldPolicy->canAutoYield());
        _exec->_yieldPolicy->forceYield();
    }

    MONGO_COMPILER_NOINLINE void resetCounters() {
        _writeConflictsInARow = 0;
        _tempUnavailErrorsInARow = 0;
    }

    MONGO_COMPILER_NOINLINE bool shouldFinish() {
        // Resetting these counters is expensive to do on the hot path, so we would rather do an
        // extra check here to see if we've made progress since the last time we entered this block,
        // and if so, reset the counters.
        if (_prevNumAdvances < _numResults) {
            resetCounters();
            _prevNumAdvances = _numResults;
        }

        // The slow path begins here. We always retry work() unless we've hit EOF *and* we
        // should not wait for inserts.
        switch (_code) {
            case PlanStage::ADVANCED: {
                // We should never hit this clause, since we only call this logic for other enum
                // values.
                MONGO_UNREACHABLE_TASSERT(8723400);
            }

            case PlanStage::NEED_YIELD: {
                handleNeedYield();
                // Don't reset counters.
                break;
            }

            case PlanStage::IS_EOF: {
                hangBeforeShouldWaitForInsertsIfFailpointEnabled(_exec);

                // The !notifier check is necessary because shouldWaitForInserts can
                // return 'true' when shouldListenForInserts returned 'false'
                // (above) in the case of a deadline becoming "unexpired" due to the
                // system clock going backwards.
                if (!_notifier ||
                    !insert_listener::shouldWaitForInserts(
                        _exec->_opCtx, _exec->_cq.get(), _exec->_yieldPolicy.get())) {
                    // Time to exit.
                    return true;
                }

                insert_listener::waitForInserts(
                    _exec->_opCtx, _exec->_yieldPolicy.get(), _notifier);
                resetCounters();
                break;
            }

            case PlanStage::NEED_TIME: {
                // Do nothing.
                resetCounters();
                break;
            }

            default: {
                MONGO_UNREACHABLE_TASSERT(8723401);
            }
        }

        // Retry work().
        return false;
    }

    MONGO_COMPILER_ALWAYS_INLINE void makeBsonWithMetadata(Document& doc,
                                                           WorkingSetMember* member) {
        if (member->metadata()) {
            MutableDocument md(std::move(doc));
            md.setMetadata(member->releaseMetadata());
            _objOut = md.freeze().toBsonWithMetaData();
        } else {
            _objOut = doc.toBsonWithMetaData();
        }
    }

    // State throughout batched work. We don't own any pointers here, except for the notifier.
    PlanExecutorImpl* _exec;
    std::unique_ptr<insert_listener::Notifier> _notifier;
    const std::function<void()> _whileYieldingFn;

    // State for doWork() loop.
    BSONObj _objOut;
    size_t _numResults = 0;
    PlanStage::StageState _code = PlanStage::StageState::ADVANCED;

    // The below are incremented on every WriteConflict or TemporarilyUnavailable error
    // accordingly, and reset to 0 on any successful call to _root->work.
    size_t _writeConflictsInARow = 0;
    size_t _tempUnavailErrorsInARow = 0;
    // Used to track if we've made any progress since the last call to shouldFinish().
    size_t _prevNumAdvances = 0;
};

size_t PlanExecutorImpl::getNextBatch(size_t batchSize, AppendBSONObjFn append) {
    const bool includeMetadata = _expCtx && _expCtx->needsMerge;
    GetNextWorker worker(this);
    worker.batchedDoWork(batchSize, includeMetadata, append);
    return worker.count();
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

void PlanExecutorImpl::executeExhaustive() {
    // We don't check batch size in exhaustDoWork().
    GetNextWorker(this).exhaustDoWork();
}

long long PlanExecutorImpl::executeCount() {
    invariant(_root->stageType() == StageType::STAGE_COUNT ||
              _root->stageType() == StageType::STAGE_RECORD_STORE_FAST_COUNT);

    executeExhaustive();
    auto countStats = static_cast<const CountStats*>(_root->getSpecificStats());
    return countStats->nCounted;
}

UpdateResult PlanExecutorImpl::executeUpdate() {
    executeExhaustive();
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
    // stage, or (for findAndModify) a projection stage wrapping an update / TS_MODIFY stage.
    const auto updateStage = [&] {
        switch (_root->stageType()) {
            case StageType::STAGE_PROJECTION_DEFAULT:
            case StageType::STAGE_PROJECTION_COVERED:
            case StageType::STAGE_PROJECTION_SIMPLE: {
                tassert(7314604,
                        "Unexpected number of children: {}"_format(_root->getChildren().size()),
                        _root->getChildren().size() == 1U);
                auto childStage = _root->child().get();
                tassert(7314605,
                        "Unexpected child stage type: {}"_format(childStage->stageType()),
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

long long PlanExecutorImpl::executeDelete() {
    executeExhaustive();
    return getDeleteResult();
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
                tassert(7308302,
                        "Unexpected number of children: {}"_format(_root->getChildren().size()),
                        _root->getChildren().size() == 1U);
                auto childStage = _root->child().get();
                tassert(7308303,
                        "Unexpected child stage type: {}"_format(childStage->stageType()),
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

bool PlanExecutorImpl::usesCollectionAcquisitions() const {
    return _yieldPolicy->usesCollectionAcquisitions();
}
}  // namespace mongo
