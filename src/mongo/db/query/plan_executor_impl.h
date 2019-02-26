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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

class PlanExecutorImpl : public PlanExecutor {
    MONGO_DISALLOW_COPYING(PlanExecutorImpl);

public:
    /**
     * Public factory methods delegate to this impl factory to do their work.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        OperationContext* opCtx,
        std::unique_ptr<WorkingSet> ws,
        std::unique_ptr<PlanStage> rt,
        std::unique_ptr<QuerySolution> qs,
        std::unique_ptr<CanonicalQuery> cq,
        const Collection* collection,
        NamespaceString nss,
        YieldPolicy yieldPolicy);

    virtual ~PlanExecutorImpl();
    WorkingSet* getWorkingSet() const final;
    PlanStage* getRootStage() const final;
    CanonicalQuery* getCanonicalQuery() const final;
    const NamespaceString& nss() const final;
    OperationContext* getOpCtx() const final;
    void saveState() final;
    void restoreState() final;
    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;
    void restoreStateWithoutRetrying() final;
    ExecState getNextSnapshotted(Snapshotted<BSONObj>* objOut, RecordId* dlOut) final;
    ExecState getNext(BSONObj* objOut, RecordId* dlOut) final;
    bool isEOF() final;
    Status executePlan() final;
    void markAsKilled(Status killStatus) final;
    void dispose(OperationContext* opCtx) final;
    void enqueue(const BSONObj& obj) final;
    bool isMarkedAsKilled() const final;
    Status getKillStatus() final;
    bool isDisposed() const final;
    bool isDetached() const final;
    Timestamp getLatestOplogTimestamp() const final;
    BSONObj getPostBatchResumeToken() const final;
    Status getMemberObjectStatus(const BSONObj& memberObj) const final;

private:
    /**
     * New PlanExecutor instances are created with the static make() method above.
     */
    PlanExecutorImpl(OperationContext* opCtx,
                     std::unique_ptr<WorkingSet> ws,
                     std::unique_ptr<PlanStage> rt,
                     std::unique_ptr<QuerySolution> qs,
                     std::unique_ptr<CanonicalQuery> cq,
                     const Collection* collection,
                     NamespaceString nss,
                     YieldPolicy yieldPolicy);

    /**
     * Clients of PlanExecutor expect that on receiving a new instance from one of the make()
     * factory methods, plan selection has already been completed. In order to enforce this
     * property, this function is called to do plan selection prior to returning the new
     * PlanExecutor.
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
     * Yields locks and waits for inserts to the collection. Returns ADVANCED if there has been an
     * insertion and there may be new results. Returns FAILURE if the PlanExecutor was killed during
     * a yield. This method is only to be used for tailable and awaitData cursors, so rather than
     * returning FAILURE if the operation has exceeded its time limit, we return IS_EOF to preserve
     * this PlanExecutor for future use.
     *
     * If an error is encountered and 'errorObj' is provided, it is populated with an object
     * describing the error.
     */
    ExecState _waitForInserts(CappedInsertNotifierData* notifierData,
                              Snapshotted<BSONObj>* errorObj);

    /**
     * Common implementation for getNext() and getNextSnapshotted().
     */
    ExecState _getNextImpl(Snapshotted<BSONObj>* objOut, RecordId* dlOut);

    // The OperationContext that we're executing within. This can be updated if necessary by using
    // detachFromOperationContext() and reattachToOperationContext().
    OperationContext* _opCtx;

    std::unique_ptr<CanonicalQuery> _cq;
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
    std::queue<BSONObj> _stash;

    enum { kUsable, kSaved, kDetached, kDisposed } _currentState = kUsable;

    bool _everDetachedFromOperationContext = false;
};

}  // namespace mongo
