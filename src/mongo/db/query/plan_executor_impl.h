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

#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

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
                     const Collection* collection,
                     NamespaceString nss,
                     PlanYieldPolicy::YieldPolicy yieldPolicy);

    virtual ~PlanExecutorImpl();
    PlanStage* getRootStage() const final;
    CanonicalQuery* getCanonicalQuery() const final;
    const NamespaceString& nss() const final;
    OperationContext* getOpCtx() const final;
    void saveState() final;
    void restoreState() final;
    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;
    ExecState getNextDocument(Document* objOut, RecordId* dlOut) final;
    ExecState getNext(BSONObj* out, RecordId* dlOut) final;
    bool isEOF() final;
    void executePlan() final;
    void markAsKilled(Status killStatus) final;
    void dispose(OperationContext* opCtx) final;
    void enqueue(const BSONObj& obj) final;
    bool isMarkedAsKilled() const final;
    Status getKillStatus() final;
    bool isDisposed() const final;
    Timestamp getLatestOplogTimestamp() const final;
    BSONObj getPostBatchResumeToken() const final;
    LockPolicy lockPolicy() const final;
    std::string getPlanSummary() const final;
    void getSummaryStats(PlanSummaryStats* statsOut) const final;
    BSONObj getStats() const final;

    /**
     * Same as restoreState() but without the logic to retry if a WriteConflictException is thrown.
     *
     * This is only public for PlanYieldPolicy. DO NOT CALL ANYWHERE ELSE.
     */
    void restoreStateWithoutRetrying();

    /**
     * Return a pointer to this executor's MultiPlanStage, or nullptr if it does not have one.
     */
    MultiPlanStage* getMultiPlanStage() const;

private:
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

    /**
     * Returns true if the PlanExecutor should listen for inserts, which is when a getMore is called
     * on a tailable and awaitData cursor that still has time left and hasn't been interrupted.
     */
    bool _shouldListenForInserts();

    /**
     * Returns true if the PlanExecutor should wait for data to be inserted, which is when a getMore
     * is called on a tailable and awaitData cursor on a capped collection.  Returns false if an EOF
     * should be returned immediately.
     */
    bool _shouldWaitForInserts();

    /**
     * Gets the CappedInsertNotifier for a capped collection.  Returns nullptr if this plan executor
     * is not capable of yielding based on a notifier.
     */
    std::shared_ptr<CappedInsertNotifier> _getCappedInsertNotifier();

    /**
     * Called for tailable and awaitData cursors in order to yield locks and waits for inserts to
     * the collection being tailed. Returns control to the caller once there has been an insertion
     * and there may be new results. If the PlanExecutor was killed during a yield, throws an
     * exception.
     */
    void _waitForInserts(CappedInsertNotifierData* notifierData);

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

    // If _killStatus has a non-OK value, then we have been killed and the value represents the
    // reason for the kill.
    Status _killStatus = Status::OK();

    // What namespace are we operating over?
    NamespaceString _nss;

    // This is used to handle automatic yielding when allowed by the YieldPolicy. Never NULL.
    // TODO make this a non-pointer member. This requires some header shuffling so that this
    // file includes plan_yield_policy.h rather than the other way around.
    const std::unique_ptr<PlanYieldPolicy> _yieldPolicy;

    // A stash of results generated by this plan that the user of the PlanExecutor didn't want
    // to consume yet. We empty the queue before retrieving further results from the plan
    // stages.
    std::queue<Document> _stash;

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
