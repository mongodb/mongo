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
#include "mongo/db/invalidation_type.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/storage/snapshot.h"

namespace mongo {

class BSONObj;
class Collection;
class RecordId;
class PlanStage;
class PlanExecutor;
struct PlanStageStats;
class PlanYieldPolicy;
class WorkingSet;

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
     * The yielding policy of the plan executor.  By default, an executor does not yield itself
     * (YIELD_MANUAL).
     */
    enum YieldPolicy {
        // Any call to getNext() may yield. In particular, the executor may be killed during any
        // call to getNext().  If this occurs, getNext() will return DEAD. Additionally, this
        // will handle all WriteConflictExceptions that occur while processing the query.
        YIELD_AUTO,

        // This will handle WriteConflictExceptions that occur while processing the query, but
        // will not yield locks. abandonSnapshot() will be called if a WriteConflictException
        // occurs so callers must be prepared to get a new snapshot.
        WRITE_CONFLICT_RETRY_ONLY,

        // Owner must yield manually if yields are requested.  How to yield yourself:
        //
        // 0. Let's say you have PlanExecutor* exec.
        //
        // 1. Register your PlanExecutor with ClientCursor. Registered executors are informed
        // about RecordId deletions and namespace invalidation, as well as other important
        // events. Do this by calling registerExec() on the executor. Alternatively, this can
        // be done per-yield (as described below).
        //
        // 2. Construct a PlanYieldPolicy 'policy', passing 'exec' to the constructor.
        //
        // 3. Call PlanYieldPolicy::yield() on 'policy'. If your PlanExecutor is not yet
        // registered (because you want to register on a per-yield basis), then pass
        // 'true' to yield().
        //
        // 4. The call to yield() returns a boolean indicating whether or not 'exec' is
        // still alove. If it is false, then 'exec' was killed during the yield and is
        // no longer valid.
        //
        // It is not possible to handle WriteConflictExceptions in this mode without restarting
        // the query.
        YIELD_MANUAL,
    };

    //
    // Factory methods.
    //
    // On success, return a new PlanExecutor, owned by the caller, through 'out'.
    //
    // Passing YIELD_AUTO to any of these factories will construct a yielding executor which
    // may yield in the following circumstances:
    //   1) During plan selection inside the call to make().
    //   2) On any call to getNext().
    //   3) While executing the plan inside executePlan().
    //
    // The executor will also be automatically registered to receive notifications in the
    // case of YIELD_AUTO, so no further calls to registerExec() or setYieldPolicy() are
    // necessary.
    //

    /**
     * Used when there is no canonical query and no query solution.
     *
     * Right now this is only for idhack updates which neither canonicalize
     * nor go through normal planning.
     */
    static StatusWith<std::unique_ptr<PlanExecutor>> make(OperationContext* opCtx,
                                                          std::unique_ptr<WorkingSet> ws,
                                                          std::unique_ptr<PlanStage> rt,
                                                          const Collection* collection,
                                                          YieldPolicy yieldPolicy);

    /**
     * Used when we have a NULL collection and no canonical query. In this case,
     * we need to explicitly pass a namespace to the plan executor.
     */
    static StatusWith<std::unique_ptr<PlanExecutor>> make(OperationContext* opCtx,
                                                          std::unique_ptr<WorkingSet> ws,
                                                          std::unique_ptr<PlanStage> rt,
                                                          const std::string& ns,
                                                          YieldPolicy yieldPolicy);

    /**
     * Used when there is a canonical query but no query solution (e.g. idhack
     * queries, queries against a NULL collection, queries using the subplan stage).
     */
    static StatusWith<std::unique_ptr<PlanExecutor>> make(OperationContext* opCtx,
                                                          std::unique_ptr<WorkingSet> ws,
                                                          std::unique_ptr<PlanStage> rt,
                                                          std::unique_ptr<CanonicalQuery> cq,
                                                          const Collection* collection,
                                                          YieldPolicy yieldPolicy);

    /**
     * The constructor for the normal case, when you have a collection, a canonical query,
     * and a query solution.
     */
    static StatusWith<std::unique_ptr<PlanExecutor>> make(OperationContext* opCtx,
                                                          std::unique_ptr<WorkingSet> ws,
                                                          std::unique_ptr<PlanStage> rt,
                                                          std::unique_ptr<QuerySolution> qs,
                                                          std::unique_ptr<CanonicalQuery> cq,
                                                          const Collection* collection,
                                                          YieldPolicy yieldPolicy);

    ~PlanExecutor();

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
    const std::string& ns();

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
     * Returns true if the state was successfully restored and the execution tree can be
     * work()'d.
     *
     * Returns false if the PlanExecutor was killed while saved. A killed execution tree cannot be
     * worked and should be deleted.
     *
     * If allowed, will yield and retry if a WriteConflictException is encountered.
     */
    bool restoreState();

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
    bool restoreStateWithoutRetrying();

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
     */
    Status executePlan();

    //
    // Concurrency-related methods.
    //

    /**
     * Register this plan executor with the collection cursor manager so that it
     * receives notifications for events that happen while yielding any locks.
     *
     * Deregistration happens automatically when this plan executor is destroyed.
     */
    void registerExec(const Collection* collection);

    /**
     * Unregister this PlanExecutor. Normally you want the PlanExecutor to be registered
     * for its lifetime, and you shouldn't have to call this explicitly.
     */
    void deregisterExec();

    /**
     * If we're yielding locks, the database we're operating over or any collection we're
     * relying on may be dropped.  When this happens all cursors and plan executors on that
     * database and collection are killed or deleted in some fashion.  Callers must specify
     * the 'reason' for why this executor is being killed.
     */
    void kill(std::string reason);

    /**
     * If we're yielding locks, writes may occur to documents that we rely on to keep valid
     * state.  As such, if the plan yields, it must be notified of relevant writes so that
     * we can ensure that it doesn't crash if we try to access invalid state.
     */
    void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type);

    /**
     * Helper method to aid in displaying an ExecState for debug or other recreational purposes.
     */
    static std::string statestr(ExecState s);

    /**
     * Change the yield policy of the PlanExecutor to 'policy'. If 'registerExecutor' is true,
     * and the yield policy is YIELD_AUTO, then the plan executor gets registered to receive
     * notifications of events from other threads.
     *
     * Everybody who sets the policy to YIELD_AUTO really wants to call registerExec()
     * immediately after EXCEPT commands that create cursors...so we expose the ability to
     * register (or not) here, rather than require all users to have yet another RAII object.
     * Only cursor-creating things like find.cpp set registerExecutor to false.
     */
    void setYieldPolicy(YieldPolicy policy,
                        const Collection* collection,
                        bool registerExecutor = true);

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

private:
    ExecState getNextImpl(Snapshotted<BSONObj>* objOut, RecordId* dlOut);

    /**
     * RAII approach to ensuring that plan executors are deregistered.
     *
     * While retrieving the first batch of results, runQuery manually registers the executor
     * with ClientCursor.  Certain query execution paths, namely $where, can throw an exception.
     * If we fail to deregister the executor, we will call invalidate/kill on the
     * still-registered-yet-deleted executor.
     *
     * For any subsequent calls to getMore, the executor is already registered with ClientCursor
     * by virtue of being cached, so this exception-proofing is not required.
     */
    struct ScopedExecutorRegistration {
        ScopedExecutorRegistration(PlanExecutor* exec, const Collection* collection);
        ~ScopedExecutorRegistration();

        PlanExecutor* const _exec;
        const Collection* const _collection;
    };

    /**
     * New PlanExecutor instances are created with the static make() methods above.
     */
    PlanExecutor(OperationContext* opCtx,
                 std::unique_ptr<WorkingSet> ws,
                 std::unique_ptr<PlanStage> rt,
                 std::unique_ptr<QuerySolution> qs,
                 std::unique_ptr<CanonicalQuery> cq,
                 const Collection* collection,
                 const std::string& ns);

    /**
     * Public factory methods delegate to this private factory to do their work.
     */
    static StatusWith<std::unique_ptr<PlanExecutor>> make(OperationContext* txn,
                                                          std::unique_ptr<WorkingSet> ws,
                                                          std::unique_ptr<PlanStage> rt,
                                                          std::unique_ptr<QuerySolution> qs,
                                                          std::unique_ptr<CanonicalQuery> cq,
                                                          const Collection* collection,
                                                          const std::string& ns,
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
    Status pickBestPlan(YieldPolicy policy, const Collection* collection);

    bool killed() {
        return static_cast<bool>(_killReason);
    };

    // The OperationContext that we're executing within.  We need this in order to release
    // locks.
    OperationContext* _opCtx;

    std::unique_ptr<CanonicalQuery> _cq;
    std::unique_ptr<WorkingSet> _workingSet;
    std::unique_ptr<QuerySolution> _qs;
    std::unique_ptr<PlanStage> _root;

    // If _killReason has a value, then we have been killed and the value represents the reason
    // for the kill.
    // The ScopedExecutorRegistration skips dereigstering the plan executor when the plan executor
    // has been killed, so _killReason must outlive _safety.
    boost::optional<std::string> _killReason;

    // Deregisters this executor when it is destroyed.
    std::unique_ptr<ScopedExecutorRegistration> _safety;

    // What namespace are we operating over?
    std::string _ns;

    // This is used to handle automatic yielding when allowed by the YieldPolicy. Never NULL.
    // TODO make this a non-pointer member. This requires some header shuffling so that this
    // file includes plan_yield_policy.h rather than the other way around.
    const std::unique_ptr<PlanYieldPolicy> _yieldPolicy;

    // A stash of results generated by this plan that the user of the PlanExecutor didn't want
    // to consume yet. We empty the queue before retrieving further results from the plan
    // stages.
    std::queue<BSONObj> _stash;

    enum { kUsable, kSaved, kDetached } _currentState = kUsable;

    bool _everDetachedFromOperationContext = false;
};

}  // namespace mongo
