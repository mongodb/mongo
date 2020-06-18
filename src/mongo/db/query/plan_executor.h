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

#include "mongo/base/status.h"
#include "mongo/db/catalog/util/partitioned.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

class BSONObj;
class CappedInsertNotifier;
struct CappedInsertNotifierData;
class Collection;
class PlanExecutor;
class PlanStage;
class RecordId;
class WorkingSet;

namespace sbe {
class PlanStage;
}  // namespace sbe

/**
 * If a getMore command specified a lastKnownCommittedOpTime (as secondaries do), we want to stop
 * waiting for new data as soon as the committed op time changes.
 *
 * 'clientsLastKnownCommittedOpTime' represents the time passed to the getMore command.
 * If the replication coordinator ever reports a higher committed op time, we should stop waiting
 * for inserts and return immediately to speed up the propagation of commit level changes.
 */
extern const OperationContext::Decoration<repl::OpTime> clientsLastKnownCommittedOpTime;

/**
 * A PlanExecutor is the abstraction that knows how to crank a tree of stages into execution.
 * The executor is usually part of a larger abstraction that is interacting with the cache
 * and/or the query optimizer.
 *
 * Executes a plan. Calls work() on a plan until a result is produced. Stops when the plan is
 * EOF or if the plan errors.
 */
class PlanExecutor {
public:
    enum ExecState {
        // Successfully returned the next document and/or record id.
        ADVANCED,

        // Execution is complete. There is no next document to return.
        IS_EOF,
    };

    // Describes whether callers should acquire locks when using a PlanExecutor. Not all cursors
    // have the same locking behavior. In particular, find executors using the legacy PlanStage
    // engine require the caller to lock the collection in MODE_IS. Aggregate executors and SBE
    // executors, on the other hand, may access multiple collections and acquire their own locks on
    // any involved collections while producing query results. Therefore, the caller need not
    // explicitly acquire any locks for such PlanExecutors.
    //
    // The policy is consulted on getMore in order to determine locking behavior, since during
    // getMore we otherwise could not easily know what flavor of cursor we're using.
    enum class LockPolicy {
        // The caller is responsible for locking the collection over which this PlanExecutor
        // executes.
        kLockExternally,

        // The caller need not hold no locks; this PlanExecutor acquires any necessary locks itself.
        kLocksInternally,
    };

    /**
     * This class will ensure a PlanExecutor is disposed before it is deleted.
     */
    class Deleter {
    public:
        /**
         * Constructs an empty deleter. Useful for creating a
         * unique_ptr<PlanExecutor, PlanExecutor::Deleter> without populating it.
         */
        Deleter() = default;

        inline Deleter(OperationContext* opCtx) : _opCtx(opCtx) {}

        /**
         * If an owner of a std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> wants to assume
         * responsibility for calling PlanExecutor::dispose(), they can call dismissDisposal(). If
         * dismissed, a Deleter will not call dispose() when deleting the PlanExecutor.
         */
        void dismissDisposal() {
            _dismissed = true;
        }

        /**
         * If 'execPtr' hasn't already been disposed, will call dispose(). If 'execPtr' is a
         * yielding PlanExecutor, callers must hold a lock on the collection in at least MODE_IS.
         */
        inline void operator()(PlanExecutor* execPtr) {
            try {
                // It is illegal to invoke operator() on a default constructed Deleter.
                invariant(_opCtx);
                if (!_dismissed) {
                    execPtr->dispose(_opCtx);
                }
                delete execPtr;
            } catch (...) {
                std::terminate();
            }
        }


    private:
        OperationContext* _opCtx = nullptr;

        bool _dismissed = false;
    };

    //
    // Factory methods.
    //
    // On success, return a new PlanExecutor, owned by the caller.
    //
    // Passing YIELD_AUTO to any of these factories will construct a yielding executor which may
    // yield in the following circumstances:
    //   - During plan selection inside the call to make().
    //   - On any call to getNext().
    //   - On any call to restoreState().
    //   - While executing the plan inside executePlan().
    //
    // If auto-yielding is enabled, a yield during make() may result in the PlanExecutor being
    // killed, in which case this method will return a non-OK status.
    //
    // All callers of these factory methods should provide either a non-null value for 'collection'
    // or a non-empty 'nss' NamespaceString but not both.
    //

    /**
     * Note that the PlanExecutor will use the ExpressionContext associated with 'cq' and the
     * OperationContext associated with that ExpressionContext.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        std::unique_ptr<CanonicalQuery> cq,
        std::unique_ptr<WorkingSet> ws,
        std::unique_ptr<PlanStage> rt,
        const Collection* collection,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        NamespaceString nss = NamespaceString(),
        std::unique_ptr<QuerySolution> qs = nullptr);

    /**
     * This overload is provided for executors that do not need a CanonicalQuery. For example, the
     * outer plan executor for an aggregate command does not have a CanonicalQuery.
     *
     * Note that the PlanExecutor will use the OperationContext associated with the 'expCtx'
     * ExpressionContext.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<WorkingSet> ws,
        std::unique_ptr<PlanStage> rt,
        const Collection* collection,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        NamespaceString nss = NamespaceString(),
        std::unique_ptr<QuerySolution> qs = nullptr);

    /**
     * These overloads are for SBE.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        OperationContext* opCtx,
        std::unique_ptr<CanonicalQuery> cq,
        std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
        NamespaceString nss,
        std::unique_ptr<PlanYieldPolicySBE> yieldPolicy);
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        OperationContext* opCtx,
        std::unique_ptr<CanonicalQuery> cq,
        std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
        NamespaceString nss,
        std::queue<std::pair<BSONObj, boost::optional<RecordId>>> stash,
        std::unique_ptr<PlanYieldPolicySBE> yieldPolicy);

    /**
     * A PlanExecutor must be disposed before destruction. In most cases, this will happen
     * automatically through a PlanExecutor::Deleter or a ClientCursor.
     */
    PlanExecutor() = default;
    virtual ~PlanExecutor() = default;

    //
    // Accessors
    //

    /**
     * Get the working set used by this executor, without transferring ownership.
     */
    virtual WorkingSet* getWorkingSet() const = 0;

    /**
     * Get the stage tree wrapped by this executor, without transferring ownership.
     */
    virtual PlanStage* getRootStage() const = 0;

    /**
     * Get the query that this executor is executing, without transferring ownership.
     */
    virtual CanonicalQuery* getCanonicalQuery() const = 0;

    /**
     * Return the NS that the query is running over.
     */
    virtual const NamespaceString& nss() const = 0;

    /**
     * Return the OperationContext that the plan is currently executing within.
     */
    virtual OperationContext* getOpCtx() const = 0;

    /**
     * Return the ExpressionContext that the plan is currently executing with.
     */
    virtual const boost::intrusive_ptr<ExpressionContext>& getExpCtx() const = 0;

    //
    // Methods that just pass down to the PlanStage tree.
    //

    /**
     * Save any state required to recover from changes to the underlying collection's data.
     *
     * While in the "saved" state, it is only legal to call restoreState,
     * detachFromOperationContext, or the destructor.
     */
    virtual void saveState() = 0;

    /**
     * Restores the state saved by a saveState() call. When this method returns successfully, the
     * execution tree can once again be executed via work().
     *
     * Throws a UserException if the state cannot be successfully restored (e.g. a collection was
     * dropped or the position of a capped cursor was lost during a yield). If restore fails, it is
     * only safe to call dispose(), detachFromOperationContext(), or the destructor.
     *
     * If allowed by the executor's yield policy, will yield and retry internally if a
     * WriteConflictException is encountered. If the time limit is exceeded during this retry
     * process, throws ErrorCodes::MaxTimeMSExpired.
     */
    virtual void restoreState() = 0;

    /**
     * Detaches from the OperationContext and releases any storage-engine state.
     *
     * It is only legal to call this when in a "saved" state. While in the "detached" state, it is
     * only legal to call reattachToOperationContext or the destructor. It is not legal to call
     * detachFromOperationContext() while already in the detached state.
     */
    virtual void detachFromOperationContext() = 0;

    /**
     * Reattaches to the OperationContext and reacquires any storage-engine state.
     *
     * It is only legal to call this in the "detached" state. On return, the cursor is left in a
     * "saved" state, so callers must still call restoreState to use this object.
     */
    virtual void reattachToOperationContext(OperationContext* opCtx) = 0;

    /**
     * Same as restoreState but without the logic to retry if a WriteConflictException is
     * thrown.
     *
     * This is only public for PlanYieldPolicy. DO NOT CALL ANYWHERE ELSE.
     */
    virtual void restoreStateWithoutRetrying() = 0;

    //
    // Running Support
    //

    /**
     * Return the next result from the underlying execution tree.
     *
     * For read operations, objOut or dlOut are populated with another query result.
     *
     * For write operations, the return depends on the particulars of the write stage.
     *
     * If a YIELD_AUTO policy is set, then this method may yield.
     *
     * The Documents returned by this method may not be owned. If the caller wants to ensure a
     * returned Document is preserved across a yield, getOwned() should be called.
     */
    virtual ExecState getNextSnapshotted(Snapshotted<Document>* objOut, RecordId* dlOut) = 0;
    virtual ExecState getNextSnapshotted(Snapshotted<BSONObj>* objOut, RecordId* dlOut) = 0;

    /**
     * Produces the next document from the query execution plan. The caller can request that the
     * executor returns documents by passing a non-null pointer for the 'objOut' output parameter,
     * and similarly can request the RecordId by passing a non-null pointer for 'dlOut'.
     *
     * If a query-fatal error occurs, this method will throw an exception. If an exception is
     * thrown, then the PlanExecutor is no longer capable of executing. The caller may extract stats
     * from the underlying plan stages, but should not attempt to do anything else with the executor
     * other than dispose() and destroy it.
     *
     * If the plan's YieldPolicy allows yielding, then any call to this method can result in a
     * yield. This relinquishes any locks that were previously acquired, regardless of the use of
     * any RAII locking helpers such as 'AutoGetCollection'. Furthermore, if an error is encountered
     * during yield recovery, an exception can be thrown while locks are not held. Callers cannot
     * expect locks to be held when this method throws an exception.
     */
    virtual ExecState getNext(Document* objOut, RecordId* dlOut) = 0;

    /**
     * Will perform the Document -> BSON conversion for the caller.
     */
    virtual ExecState getNext(BSONObj* out, RecordId* dlOut) = 0;

    /**
     * Returns 'true' if the plan is done producing results (or writing), 'false' otherwise.
     *
     * Tailable cursors are a possible exception to this: they may have further results even if
     * isEOF() returns true.
     */
    virtual bool isEOF() = 0;

    /**
     * Execute the plan to completion, throwing out the results. Used when you want to work the
     * underlying tree without getting results back.
     *
     * If a YIELD_AUTO policy is set on this executor, then this will automatically yield.
     *
     * Throws an exception if this plan results in a runtime error or is killed.
     */
    virtual void executePlan() = 0;

    //
    // Concurrency-related methods.
    //

    /**
     * Notifies a PlanExecutor that it should die. Callers must specify the reason for why this
     * executor is being killed. Subsequent calls to getNext() will throw a query-fatal exception
     * with an error reflecting 'killStatus'. If this method is called multiple times, only the
     * first 'killStatus' will be retained. It is illegal to call this method with Status::OK.
     */
    virtual void markAsKilled(Status killStatus) = 0;

    /**
     * Cleans up any state associated with this PlanExecutor. Must be called before deleting this
     * PlanExecutor. It is illegal to use a PlanExecutor after calling dispose().
     *
     * There are multiple cleanup scenarios:
     *  - This PlanExecutor will only ever use one OperationContext. In this case the
     *    PlanExecutor::Deleter will automatically call dispose() before deleting the PlanExecutor,
     *    and the owner need not call dispose().
     *  - This PlanExecutor may use multiple OperationContexts over its lifetime. In this case it
     *    is the owner's responsibility to call dispose() with a valid OperationContext before
     *    deleting the PlanExecutor.
     */
    virtual void dispose(OperationContext* opCtx) = 0;

    /**
     * Helper method to aid in displaying an ExecState for debug or other recreational purposes.
     */
    static std::string statestr(ExecState s);

    /**
     * Stash the BSONObj so that it gets returned from the PlanExecutor on a later call to
     * getNext().
     *
     * Enqueued documents are returned in FIFO order. The queued results are exhausted before
     * generating further results from the underlying query plan.
     *
     * Subsequent calls to getNext() must request the BSONObj and *not* the RecordId.
     *
     * If used in combination with getNextSnapshotted(), then the SnapshotId associated with
     * 'obj' will be null when 'obj' is dequeued.
     */
    virtual void enqueue(const Document& obj) = 0;
    virtual void enqueue(const BSONObj& obj) = 0;

    virtual bool isMarkedAsKilled() const = 0;
    virtual Status getKillStatus() = 0;

    virtual bool isDisposed() const = 0;
    virtual bool isDetached() const = 0;

    /**
     * If the last oplog timestamp is being tracked for this PlanExecutor, return it.
     * Otherwise return a null timestamp.
     */
    virtual Timestamp getLatestOplogTimestamp() const = 0;

    /**
     * If this PlanExecutor is tracking change stream resume tokens, return the most recent token
     * for the batch that is currently being built. Otherwise, return an empty object.
     */
    virtual BSONObj getPostBatchResumeToken() const = 0;

    virtual LockPolicy lockPolicy() const = 0;

    /**
     * Returns true if this PlanExecutor proxies to a Pipeline of DocumentSources.
     *
     * TODO SERVER-48478 : Create a new PlanExecutor implementation specifically for executing the
     * Pipeline, and delete PipelineProxyStage.
     */
    virtual bool isPipelineExecutor() const = 0;
};

}  // namespace mongo
