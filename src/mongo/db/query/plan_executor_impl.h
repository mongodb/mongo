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

#include <boost/optional.hpp>
#include <queue>

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_solution.h"

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
auto handlePlanStageYield(
    ExpressionContext* expCtx, StringData opStr, StringData ns, F&& f, H&& yieldHandler) {
    auto opCtx = expCtx->opCtx;
    invariant(opCtx);
    invariant(opCtx->lockState());
    invariant(opCtx->recoveryUnit());
    invariant(!expCtx->getTemporarilyUnavailableException());

    try {
        return f();
    } catch (const WriteConflictException&) {
        yieldHandler();
        return PlanStage::NEED_YIELD;
    } catch (const TemporarilyUnavailableException& e) {
        if (opCtx->inMultiDocumentTransaction()) {
            handleTemporarilyUnavailableExceptionInTransaction(opCtx, opStr, ns, e);
        }
        expCtx->setTemporarilyUnavailableException(true);
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
                     const CollectionPtr& collection,
                     bool returnOwnedBson,
                     NamespaceString nss,
                     PlanYieldPolicy::YieldPolicy yieldPolicy);

    virtual ~PlanExecutorImpl();
    CanonicalQuery* getCanonicalQuery() const final;
    const NamespaceString& nss() const final;
    const std::vector<NamespaceStringOrUUID>& getSecondaryNamespaces() const final;
    OperationContext* getOpCtx() const final;
    void saveState() final;
    void restoreState(const RestoreContext& context) final;
    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;
    ExecState getNextDocument(Document* objOut, RecordId* dlOut) final;
    ExecState getNext(BSONObj* out, RecordId* dlOut) final;
    bool isEOF() final;
    long long executeCount() override;
    UpdateResult executeUpdate() override;
    UpdateResult getUpdateResult() const override;
    long long executeDelete() override;
    BatchedDeleteStats getBatchedDeleteStats() override;
    void markAsKilled(Status killStatus) final;
    void dispose(OperationContext* opCtx) final;
    void stashResult(const BSONObj& obj) final;
    bool isMarkedAsKilled() const final;
    Status getKillStatus() final;
    bool isDisposed() const final;
    Timestamp getLatestOplogTimestamp() const final;
    BSONObj getPostBatchResumeToken() const final;
    LockPolicy lockPolicy() const final;
    const PlanExplainer& getPlanExplainer() const final;

    /**
     * Same as restoreState() but without the logic to retry if a WriteConflictException is thrown.
     *
     * This is only public for PlanYieldPolicy. DO NOT CALL ANYWHERE ELSE.
     */
    void restoreStateWithoutRetrying(const RestoreContext& context, const Yieldable* yieldable);

    /**
     * Return a pointer to this executor's MultiPlanStage, or nullptr if it does not have one.
     */
    MultiPlanStage* getMultiPlanStage() const;

    PlanStage* getRootStage() const;

    void enableSaveRecoveryUnitAcrossCommandsIfSupported() override {}
    bool isSaveRecoveryUnitAcrossCommandsEnabled() const override {
        return false;
    }

private:
    /**
     *  Executes the underlying PlanStage tree until it indicates EOF. Throws an exception if the
     *  plan results in an error.
     *
     *  Useful for cases where the caller wishes to execute the plan and extract stats from it (e.g.
     *  the result of a count or update) rather than returning a set of resulting documents.
     */
    void _executePlan();

    /**
     * Called on construction in order to ensure that when callers receive a new instance of a
     * 'PlanExecutorImpl', plan selection has already been completed.
     *
     * If the tree contains plan selection stages, such as MultiPlanStage or SubplanStage,
     * this calls into their underlying plan selection facilities. Otherwise, does nothing.
     *
     * If a YIELD_AUTO policy is set then locks are yielded during plan selection.
     *
     * Returns a non-OK status if query planning fails. In particular, this function returns
     * ErrorCodes::QueryPlanKilled if plan execution cannot proceed due to a concurrent write or
     * catalog operation.
     */
    Status _pickBestPlan();

    ExecState _getNextImpl(Snapshotted<Document>* objOut, RecordId* dlOut);

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
    const bool _mustReturnOwnedBson;

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
    const CollectionScan* _collScanStage{nullptr};
};

}  // namespace mongo
