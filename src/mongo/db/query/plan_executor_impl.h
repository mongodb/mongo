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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/global_catalog/catalog_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/oplog_wait_config.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_insert_listener.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/restore_context.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/query/write_ops/update_result.h"
#include "mongo/db/record_id.h"
#include "mongo/db/yieldable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <deque>
#include <memory>
#include <vector>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Query execution helper. Runs the argument function 'f'. If 'f' throws an exception other than
 * 'WriteConflictException' or 'TemporarilyUnavailableException', then these exceptions escape
 * this function. In contrast 'WriteConflictException' or 'TemporarilyUnavailableException' are
 * caught, the given 'yieldHandler' is run, and the helper returns PlanStage::NEED_YIELD.
 *
 * In a multi-document transaction, it rethrows a TemporarilyUnavailableException as a
 * WriteConflictException.
 */
template <typename F, typename H>
[[nodiscard]] PlanStage::StageState handlePlanStageYield(ExpressionContext* expCtx,
                                                         StringData opStr,
                                                         F&& f,
                                                         H&& yieldHandler) {
    auto opCtx = expCtx->getOperationContext();
    invariant(opCtx);
    invariant(shard_role_details::getLocker(opCtx));
    invariant(shard_role_details::getRecoveryUnit(opCtx));
    invariant(!expCtx->getTemporarilyUnavailableException());

    try {
        return f();
    } catch (const ExceptionFor<ErrorCodes::WriteConflict>&) {
        recordWriteConflict(opCtx);
        yieldHandler();
        return PlanStage::NEED_YIELD;
    } catch (const ExceptionFor<ErrorCodes::TemporarilyUnavailable>& e) {
        if (opCtx->inMultiDocumentTransaction()) {
            convertToWCEAndRethrow(opCtx, opStr, e);
        }
        expCtx->setTemporarilyUnavailableException(true);
        yieldHandler();
        return PlanStage::NEED_YIELD;
    } catch (const ExceptionFor<ErrorCodes::TransactionTooLargeForCache>&) {
        if (opCtx->writesAreReplicated()) {
            // Surface error on primaries.
            throw;
        }

        // If an operation succeeds on primary, it should always be retried on secondaries.
        // Secondaries always retry TemporarilyUnavailableExceptions and WriteConflictExceptions
        // indefinitely, the only difference being the rate of retry. We prefer retrying faster, by
        // converting to WriteConflictException, to avoid stalling replication longer than
        // necessary.
        yieldHandler();
        return PlanStage::NEED_YIELD;
    } catch (const ExceptionFor<ErrorCodes::ShardCannotRefreshDueToLocksHeld>& ex) {
        // An operation may need to obtain the cached routing table (CatalogCache) for some
        // namespace other than the main nss of the plan. When that cache is not immediately
        // available, a refresh of the CatalogCache is needed. However, this refresh cannot be done
        // while locks are being held. We handle this by requesting a yield and then refreshing the
        // CatalogCache after having released the locks.
        const auto& extraInfo = ex.extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();
        planExecutorShardingState(expCtx->getOperationContext()).catalogCacheRefreshRequired =
            extraInfo->getNss();
        yieldHandler();
        return PlanStage::NEED_YIELD;
    }
}

class CappedInsertNotifier;
class CollectionScan;
struct CappedInsertNotifierData;

class PlanExecutorImpl : public PlanExecutor {
    PlanExecutorImpl(const PlanExecutorImpl&) = delete;
    PlanExecutorImpl& operator=(const PlanExecutorImpl&) = delete;

public:
    /**
     * Callers should obtain PlanExecutorImpl instances uses the 'plan_executor_factory' methods, in
     * order to avoid depending directly on this concrete implementation of the PlanExecutor
     * interface.
     */
    PlanExecutorImpl(OperationContext* opCtx,
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
                     std::vector<std::unique_ptr<PlanStage>> cbrRejectedPlanStages);

    ~PlanExecutorImpl() override;
    CanonicalQuery* getCanonicalQuery() const final;
    const NamespaceString& nss() const final;
    const std::vector<NamespaceStringOrUUID>& getSecondaryNamespaces() const final;
    OperationContext* getOpCtx() const final;
    void saveState() final;
    void restoreState(const RestoreContext& context) final;
    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;

    ExecState getNextDocument(Document& objOut) final;
    ExecState getNext(BSONObj* out, RecordId* dlOut) final;
    size_t getNextBatch(size_t batchSize, AppendBSONObjFn append) final;

    bool isEOF() const final;
    long long executeCount() override;
    UpdateResult getUpdateResult() const override;
    long long getDeleteResult() const override;
    BatchedDeleteStats getBatchedDeleteStats() override;
    void markAsKilled(Status killStatus) final;
    void dispose(OperationContext* opCtx) final;
    void forceSpill(PlanYieldPolicy* yieldPolicy) final {
        _root->forceSpill(yieldPolicy);
    }
    void stashResult(const BSONObj& obj) final;

    MONGO_COMPILER_ALWAYS_INLINE bool isMarkedAsKilled() const final {
        return !_killStatus.isOK();
    }

    Status getKillStatus() const final;
    bool isDisposed() const final;
    Timestamp getLatestOplogTimestamp() const final;
    BSONObj getPostBatchResumeToken() const final;
    LockPolicy lockPolicy() const final;
    const PlanExplainer& getPlanExplainer() const final;

    PlanExecutor::QueryFramework getQueryFramework() const final {
        return PlanExecutor::QueryFramework::kClassicOnly;
    }

    /**
     * Same as restoreState() but without the logic to retry if a WriteConflictException is thrown.
     *
     * This is only public for PlanYieldPolicy. DO NOT CALL ANYWHERE ELSE.
     */
    void restoreStateWithoutRetrying(const RestoreContext& context);

    /**
     * Return a pointer to this executor's MultiPlanStage, or nullptr if it does not have one.
     */
    MultiPlanStage* getMultiPlanStage() const;

    PlanStage* getRootStage() const;

    void setReturnOwnedData(bool returnOwnedData) final {
        _mustReturnOwnedBson = returnOwnedData;
    }

    bool usesCollectionAcquisitions() const final;

    /**
     * It is used to detect if the plan executor obtained after multiplanning is using a distinct
     * scan stage. That's because in this scenario modifications to the pipeline in the context of
     * aggregation need to be made.
     */
    bool isUsingDistinctScan() const final {
        auto soln = getQuerySolution();
        return soln && soln->root()->hasNode(STAGE_DISTINCT_SCAN);
    }

private:
    const QuerySolution* getQuerySolution() const {
        if (_qs) {
            return _qs.get();
        }
        if (const MultiPlanStage* mps = getMultiPlanStage()) {
            return mps->bestSolution();
        }
        return nullptr;
    }

    ExecState _getNextImpl(Document* objOut, RecordId* dlOut);

    // Helper for handling the NEED_YIELD stage state.
    void _handleNeedYield(size_t& writeConflictsInARow, size_t& tempUnavailErrorsInARow);

    // Helper for handling the EOF stage state. Returns whether or not to stop doing work().
    bool _handleEOFAndExit(PlanStage::StageState code,
                           std::unique_ptr<insert_listener::Notifier>& notifier);

    // Function which waits for oplog visiblity. It assumes that it is invoked following snapshot
    // abandonment, but before yielding any resources.
    void _waitForAllEarlierOplogWritesToBeVisible();

    MONGO_COMPILER_ALWAYS_INLINE void _checkIfMustYield(
        const std::function<void()>& whileYieldingFn) {
        // These are the conditions which can cause us to yield:
        //   1) The yield policy's timer elapsed, or
        //   2) some stage requested a yield, or
        //   3) we need to yield and retry due to a WriteConflictException.
        // In all cases, the actual yielding happens here.
        if (_yieldPolicy->shouldYieldOrInterrupt(_opCtx)) {
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(_opCtx,
                                                           whileYieldingFn,
                                                           RestoreContext::RestoreType::kYield,
                                                           _afterSnapshotAbandonFn));
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE void _checkIfKilled() const {
        if (MONGO_unlikely(isMarkedAsKilled())) {
            uassertStatusOK(_killStatus);
        }
    }

    std::unique_ptr<insert_listener::Notifier> makeNotifier();

    // The OperationContext that we're executing within. This can be updated if necessary by using
    // detachFromOperationContext() and reattachToOperationContext().
    OperationContext* _opCtx;

    // Note, this can be null. Some queries don't need a CanonicalQuery for planning. For example,
    // aggregation queries create a PlanExecutor with no CanonicalQuery.
    std::unique_ptr<CanonicalQuery> _cq;

    // When '_cq' is not null, this will point to the same ExpressionContext that is in the '_cq'
    // object. Note that this pointer can also be null when '_cq' is null. For example a "list
    // collections" query does not need a CanonicalQuery or ExpressionContext.
    boost::intrusive_ptr<ExpressionContext> _expCtx;

    std::unique_ptr<WorkingSet> _workingSet;
    std::unique_ptr<QuerySolution> _qs;
    std::unique_ptr<PlanStage> _root;
    std::unique_ptr<PlanExplainer> _planExplainer;

    // If _killStatus has a non-OK value, then we have been killed and the value represents the
    // reason for the kill.
    Status _killStatus = Status::OK();

    // Whether the executor must return owned BSON.
    bool _mustReturnOwnedBson;

    // What namespace are we operating over?
    NamespaceString _nss;

    // This is used to handle automatic yielding when allowed by the YieldPolicy. Never nullptr.
    std::unique_ptr<PlanYieldPolicy> _yieldPolicy;

    // A stash of results generated by this plan that the user of the PlanExecutor didn't want
    // to consume yet. We empty the queue before retrieving further results from the plan
    // stages.
    std::deque<Document> _stash;

    // The output document that is used by getNext BSON API. This allows us to avoid constantly
    // allocating and freeing DocumentStorage.
    Document _docOutput;

    enum { kUsable, kSaved, kDetached, kDisposed } _currentState = kUsable;

    // A pointer either to a CollectionScan stage, if present in the execution tree, or nullptr
    // otherwise. We cache it to avoid the need to traverse the execution tree in runtime when the
    // executor is requested to return the oplog tracking info. Since this info is provided by
    // either of these stages, the executor will simply delegate the request to the cached stage.
    CollectionScan* _collScanStage{nullptr};

    // Used to coordinate waiting for oplog visiblity. Note that this is owned by the collection
    // scan (if one exists). Initialized only if this executor is doing a collection scan over the
    // oplog, nullptr otherwise.
    OplogWaitConfig* _oplogWaitConfig{nullptr};

    // Function used to wait for oplog visibility in between snapshot abandonment and
    std::function<void()> _afterSnapshotAbandonFn{nullptr};
};

}  // namespace mongo
