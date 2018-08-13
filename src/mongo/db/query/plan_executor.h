/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/invalidation_type.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

class BSONObj;
class CappedInsertNotifier;
struct CappedInsertNotifierData;
class Collection;
class CursorManager;
class PlanExecutor;
class PlanStage;
class PlanYieldPolicy;
class RecordId;
struct PlanStageStats;
class WorkingSet;

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
        // We successfully populated the out parameter.
        ADVANCED,

        // We're EOF.  We won't return any more results (edge case exception: capped+tailable).
        IS_EOF,

        // We were killed. This is a special failure case in which we cannot rely on the
        // collection or database to still be valid.
        // If the underlying PlanStage has any information on the error, it will be available in
        // the objOut parameter. Call WorkingSetCommon::toStatusString() to retrieve the error
        // details from the output BSON object.
        DEAD,

        // getNext was asked for data it cannot provide, or the underlying PlanStage had an
        // unrecoverable error.
        // If the underlying PlanStage has any information on the error, it will be available in
        // the objOut parameter. Call WorkingSetCommon::toStatusString() to retrieve the error
        // details from the output BSON object.
        FAILURE,
    };

    /**
     * The yielding policy of the plan executor. By default, an executor does not yield itself
     * (NO_YIELD).
     */
    enum YieldPolicy {
        // Any call to getNext() may yield. In particular, the executor may be killed during any
        // call to getNext().  If this occurs, getNext() will return DEAD. Additionally, this
        // will handle all WriteConflictExceptions that occur while processing the query.
        YIELD_AUTO,

        // This will handle WriteConflictExceptions that occur while processing the query, but
        // will not yield locks. abandonSnapshot() will be called if a WriteConflictException
        // occurs so callers must be prepared to get a new snapshot. A PlanExecutor constructed with
        // this yield policy will not be registered to receive invalidations, so the caller must
        // hold their locks continuously from construction to destruction.
        WRITE_CONFLICT_RETRY_ONLY,

        // Use this policy if you want to disable auto-yielding, but will release locks while using
        // the PlanExecutor. Any WriteConflictExceptions will be raised to the caller of getNext().
        YIELD_MANUAL,

        // Can be used in one of the following scenarios:
        //  - The caller will hold a lock continuously for the lifetime of this PlanExecutor.
        //  - This PlanExecutor doesn't logically belong to a Collection, and so does not need to be
        //    locked during execution. For example, a PlanExecutor containing a PipelineProxyStage
        //    which is being used to execute an aggregation pipeline.
        NO_YIELD,

        // Used for testing, this yield policy will cause the PlanExecutor to time out on the first
        // yield, returning DEAD with an error object encoding a ErrorCodes::ExceededTimeLimit
        // message.
        ALWAYS_TIME_OUT,

        // Used for testing, this yield policy will cause the PlanExecutor to be marked as killed on
        // the first yield, returning DEAD with an error object encoding a
        // ErrorCodes::QueryPlanKilled message.
        ALWAYS_MARK_KILLED,
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
        Deleter() {}

        Deleter(OperationContext* opCtx, const Collection* collection);

        /**
         * If an owner of a std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> wants to assume
         * responsibility for calling PlanExecutor::dispose(), they can call dismissDisposal(). If
         * dismissed, a Deleter will not call dispose() when deleting the PlanExecutor.
         */
        void dismissDisposal() {
            _dismissed = true;
        }

        /**
         * If 'execPtr' hasn't already been disposed, will call dispose(). Also, if 'execPtr' has
         * been registered with the CursorManager, will deregister it. If 'execPtr' is a yielding
         * PlanExecutor, callers must hold a lock on the collection in at least MODE_IS.
         */
        void operator()(PlanExecutor* execPtr);

    private:
        OperationContext* _opCtx = nullptr;
        CursorManager* _cursorManager = nullptr;

        bool _dismissed = false;
    };

    //
    // Factory methods.
    //
    // On success, return a new PlanExecutor, owned by the caller.
    //
    // Passing YIELD_AUTO to any of these factories will construct a yielding executor which
    // may yield in the following circumstances:
    //   - During plan selection inside the call to make().
    //   - On any call to getNext().
    //   - On any call to restoreState().
    //   - While executing the plan inside executePlan().
    //
    // The executor will also be automatically registered to receive notifications in the case of
    // YIELD_AUTO or YIELD_MANUAL.
    //

    /**
     * Used when there is no canonical query and no query solution.
     *
     * Right now this is only for idhack updates which neither canonicalize nor go through normal
     * planning.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        OperationContext* opCtx,
        std::unique_ptr<WorkingSet> ws,
        std::unique_ptr<PlanStage> rt,
        const Collection* collection,
        YieldPolicy yieldPolicy);

    /**
     * Used when we have a NULL collection and no canonical query. In this case, we need to
     * explicitly pass a namespace to the plan executor.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        OperationContext* opCtx,
        std::unique_ptr<WorkingSet> ws,
        std::unique_ptr<PlanStage> rt,
        NamespaceString nss,
        YieldPolicy yieldPolicy);

    /**
     * Used when there is a canonical query but no query solution (e.g. idhack queries, queries
     * against a NULL collection, queries using the subplan stage).
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        OperationContext* opCtx,
        std::unique_ptr<WorkingSet> ws,
        std::unique_ptr<PlanStage> rt,
        std::unique_ptr<CanonicalQuery> cq,
        const Collection* collection,
        YieldPolicy yieldPolicy);

    /**
     * The constructor for the normal case, when you have a collection, a canonical query, and a
     * query solution.
     */
    static StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
        OperationContext* opCtx,
        std::unique_ptr<WorkingSet> ws,
        std::unique_ptr<PlanStage> rt,
        std::unique_ptr<QuerySolution> qs,
        std::unique_ptr<CanonicalQuery> cq,
        const Collection* collection,
        YieldPolicy yieldPolicy);

    //
    // Accessors
    //

    /**
     * Get the working set used by this executor, without transferring ownership.
     */
    WorkingSet* getWorkingSet() const;

    /**
     * Get the stage tree wrapped by this executor, without transferring ownership.
     */
    PlanStage* getRootStage() const;

    /**
     * Get the query that this executor is executing, without transferring ownership.
     */
    CanonicalQuery* getCanonicalQuery() const;

    /**
     * Return the NS that the query is running over.
     */
    const NamespaceString& nss() const {
        return _nss;
    }

    /**
     * Return the OperationContext that the plan is currently executing within.
     */
    OperationContext* getOpCtx() const;

    /**
     * Generates a tree of stats objects with a separate lifetime from the execution
     * stage tree wrapped by this PlanExecutor.
     *
     * This is OK even if we were killed.
     */
    std::unique_ptr<PlanStageStats> getStats() const;

    //
    // Methods that just pass down to the PlanStage tree.
    //

    /**
     * Save any state required to recover from changes to the underlying collection's data.
     *
     * While in the "saved" state, it is only legal to call restoreState,
     * detachFromOperationContext, or the destructor.
     */
    void saveState();

    /**
     * Restores the state saved by a saveState() call.
     *
     * Returns Status::OK() if the state was successfully restored and the execution tree can be
     * work()'d.
     *
     * Returns ErrorCodes::QueryPlanKilled if the PlanExecutor was killed while saved.
     *
     * If allowed, will yield and retry if a WriteConflictException is encountered. If the time
     * limit is exceeded during this retry process, returns ErrorCodes::ExceededTimeLimit. If this
     * PlanExecutor is killed during this retry process, returns ErrorCodes::QueryPlanKilled. In
     * this scenario, locks will have been released, and will not be held when control returns to
     * the caller.
     */
    Status restoreState();

    /**
     * Detaches from the OperationContext and releases any storage-engine state.
     *
     * It is only legal to call this when in a "saved" state. While in the "detached" state, it is
     * only legal to call reattachToOperationContext or the destructor. It is not legal to call
     * detachFromOperationContext() while already in the detached state.
     */
    void detachFromOperationContext();

    /**
     * Reattaches to the OperationContext and reacquires any storage-engine state.
     *
     * It is only legal to call this in the "detached" state. On return, the cursor is left in a
     * "saved" state, so callers must still call restoreState to use this object.
     */
    void reattachToOperationContext(OperationContext* opCtx);

    /**
     * Same as restoreState but without the logic to retry if a WriteConflictException is
     * thrown.
     *
     * This is only public for PlanYieldPolicy. DO NOT CALL ANYWHERE ELSE.
     */
    Status restoreStateWithoutRetrying();

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
     */
    ExecState getNextSnapshotted(Snapshotted<BSONObj>* objOut, RecordId* dlOut);

    ExecState getNext(BSONObj* objOut, RecordId* dlOut);

    /**
     * Returns 'true' if the plan is done producing results (or writing), 'false' otherwise.
     *
     * Tailable cursors are a possible exception to this: they may have further results even if
     * isEOF() returns true.
     */
    bool isEOF();

    /**
     * Execute the plan to completion, throwing out the results.  Used when you want to work the
     * underlying tree without getting results back.
     *
     * If a YIELD_AUTO policy is set on this executor, then this will automatically yield.
     *
     * Returns ErrorCodes::QueryPlanKilled if the plan executor was killed during a yield. If this
     * error occurs, it is illegal to subsequently access the collection, since it may have been
     * dropped.
     */
    Status executePlan();

    //
    // Concurrency-related methods.
    //

    /**
     * If we're yielding locks, the database we're operating over or any collection we're relying on
     * may be dropped. Plan executors are notified of such events by calling markAsKilled().
     * Callers must specify the 'reason' for why this executor is being killed. Subsequent calls to
     * getNext() will return DEAD, and fill 'objOut' with an error detail including 'reason'.
     */
    void markAsKilled(std::string reason);

    /**
     * Cleans up any state associated with this PlanExecutor. Must be called before deleting this
     * PlanExecutor. It is illegal to use a PlanExecutor after calling dispose(). 'cursorManager'
     * may be null.
     *
     * There are multiple cleanup scenarios:
     *  - This PlanExecutor will only ever use one OperationContext. In this case the
     *    PlanExecutor::Deleter will automatically call dispose() before deleting the PlanExecutor,
     *    and the owner need not call dispose().
     *  - This PlanExecutor may use multiple OperationContexts over its lifetime. In this case it
     *    is the owner's responsibility to call dispose() with a valid OperationContext before
     *    deleting the PlanExecutor.
     */
    void dispose(OperationContext* opCtx, CursorManager* cursorManager);

    /**
     * If we're yielding locks, writes may occur to documents that we rely on to keep valid
     * state.  As such, if the plan yields, it must be notified of relevant writes so that
     * we can ensure that it doesn't crash if we try to access invalid state.
     */
    void invalidate(OperationContext* opCtx, const RecordId& dl, InvalidationType type);

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
    void enqueue(const BSONObj& obj);

    /**
     * Helper method which returns a set of BSONObj, where each represents a sort order of our
     * output.
     */
    BSONObjSet getOutputSorts() const;

    /**
     * Communicate to this PlanExecutor that it is no longer registered with the CursorManager as a
     * 'non-cached PlanExecutor'.
     */
    void unsetRegistered() {
        _registrationToken.reset();
    }

    boost::optional<Partitioned<unordered_set<PlanExecutor*>>::PartitionId> getRegistrationToken()
        const& {
        return _registrationToken;
    }
    void getRegistrationToken() && = delete;

    void setRegistrationToken(Partitioned<unordered_set<PlanExecutor*>>::PartitionId token) & {
        invariant(!_registrationToken);
        _registrationToken = token;
    }

    bool isMarkedAsKilled() const {
        return static_cast<bool>(_killReason);
    }

    const std::string& getKillReason() {
        invariant(isMarkedAsKilled());
        return *_killReason;
    }

    /**
     * If the last oplog timestamp is being tracked for this PlanExecutor, return it.
     * Otherwise return a null timestamp.
     */
    Timestamp getLatestOplogTimestamp();

private:
    /**
     * Returns true if the PlanExecutor should wait for data to be inserted, which is when a getMore
     * is called on a tailable and awaitData cursor on a capped collection.  Returns false if an EOF
     * should be returned immediately.
     */
    bool shouldWaitForInserts();

    /**
     * Gets the CappedInsertNotifier for a capped collection.  Returns nullptr if this plan executor
     * is not capable of yielding based on a notifier.
     */
    std::shared_ptr<CappedInsertNotifier> getCappedInsertNotifier();

    /**
     * Yields locks and waits for inserts to the collection. Returns ADVANCED if there has been an
     * insertion and there may be new results. Returns DEAD if the PlanExecutor was killed during a
     * yield. This method is only to be used for tailable and awaitData cursors, so rather than
     * returning DEAD if the operation has exceeded its time limit, we return IS_EOF to preserve
     * this PlanExecutor for future use.
     *
     * If an error is encountered and 'errorObj' is provided, it is populated with an object
     * describing the error.
     */
    ExecState waitForInserts(CappedInsertNotifierData* notifierData,
                             Snapshotted<BSONObj>* errorObj);

    ExecState getNextImpl(Snapshotted<BSONObj>* objOut, RecordId* dlOut);

    /**
     * New PlanExecutor instances are created with the static make() methods above.
     */
    PlanExecutor(OperationContext* opCtx,
                 std::unique_ptr<WorkingSet> ws,
                 std::unique_ptr<PlanStage> rt,
                 std::unique_ptr<QuerySolution> qs,
                 std::unique_ptr<CanonicalQuery> cq,
                 const Collection* collection,
                 NamespaceString nss,
                 YieldPolicy yieldPolicy);

    /**
     * A PlanExecutor must be disposed before destruction. In most cases, this will happen
     * automatically through a PlanExecutor::Deleter or a ClientCursor.
     */
    ~PlanExecutor();

    /**
     * Public factory methods delegate to this private factory to do their work.
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
    Status pickBestPlan(const Collection* collection);

    // The OperationContext that we're executing within. This can be updated if necessary by using
    // detachFromOperationContext() and reattachToOperationContext().
    OperationContext* _opCtx;

    std::unique_ptr<CanonicalQuery> _cq;
    std::unique_ptr<WorkingSet> _workingSet;
    std::unique_ptr<QuerySolution> _qs;
    std::unique_ptr<PlanStage> _root;

    // If _killReason has a value, then we have been killed and the value represents the reason for
    // the kill.
    boost::optional<std::string> _killReason;

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

    // Set if this PlanExecutor is registered with the CursorManager.
    boost::optional<Partitioned<unordered_set<PlanExecutor*>>::PartitionId> _registrationToken;

    bool _everDetachedFromOperationContext = false;
};

}  // namespace mongo
