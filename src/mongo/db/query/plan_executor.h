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
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/restore_context.h"

namespace mongo {

class CollectionPtr;
class BSONObj;
class PlanStage;
class RecordId;
class ScopedCollectionAcquisition;

// TODO: SERVER-76397 Remove this once we use ScopedCollectionAcquisition everywhere.
class VariantCollectionPtrOrAcquisition {
public:
    VariantCollectionPtrOrAcquisition(const CollectionPtr* collectionPtr)
        : _collectionPtrOrAcquisition(collectionPtr) {}
    VariantCollectionPtrOrAcquisition(const ScopedCollectionAcquisition* collection)
        : _collectionPtrOrAcquisition(collection) {}

    const stdx::variant<const CollectionPtr*, const ScopedCollectionAcquisition*>& get() {
        return _collectionPtrOrAcquisition;
    };

    const CollectionPtr& getCollectionPtr() const;

private:
    stdx::variant<const CollectionPtr*, const ScopedCollectionAcquisition*>
        _collectionPtrOrAcquisition;
};

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
 * If a plan yielded because it encountered a sharding critical section,
 * 'planExecutorShardingCriticalSectionFuture' will be set to a future that becomes ready when the
 * critical section ends. This future can be waited on to hold off resuming the plan execution while
 * the critical section is still active.
 */
extern const OperationContext::Decoration<boost::optional<SharedSemiFuture<void>>>
    planExecutorShardingCriticalSectionFuture;

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

    /**
     * Helper method to aid in displaying an ExecState for debug or other recreational purposes.
     */
    static std::string stateToStr(ExecState s);

    /**
     * Throws a user exception if "planExecutorAlwaysFails" is enabled.
     */
    static void checkFailPointPlanExecAlwaysFails();

    /**
     * A PlanExecutor must be disposed before destruction. In most cases, this will happen
     * automatically through a PlanExecutor::Deleter or a ClientCursor.
     */
    PlanExecutor() = default;

    virtual ~PlanExecutor() = default;

    /**
     * Get the query that this executor is executing, without transferring ownership.
     */
    virtual CanonicalQuery* getCanonicalQuery() const = 0;

    /**
     * Return the namespace that the query is running over.
     *
     * WARNING: In general, a query execution plan can involve multiple collections, and therefore
     * there is not a single namespace associated with a PlanExecutor. This method is here for
     * legacy reasons, and new call sites should not be added.
     */
    virtual const NamespaceString& nss() const = 0;

    /**
     * Returns a vector of secondary namespaces that are relevant to this executor.
     */
    virtual const std::vector<NamespaceStringOrUUID>& getSecondaryNamespaces() const = 0;

    /**
     * Return the OperationContext that the plan is currently executing within.
     */
    virtual OperationContext* getOpCtx() const = 0;

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
     * RestoreContext is a context containing external state needed by plan stages to be able to
     * restore into a valid state. The RequiresCollectionStage requires a valid CollectionPtr for
     * example.
     *
     * Throws a UserException if the state cannot be successfully restored (e.g. a collection was
     * dropped or the position of a capped cursor was lost during a yield). If restore fails, it is
     * only safe to call dispose(), detachFromOperationContext(), or the destructor.
     *
     * If allowed by the executor's yield policy, will yield and retry internally if a
     * WriteConflictException is encountered. If the time limit is exceeded during this retry
     * process, throws ErrorCodes::MaxTimeMSExpired.
     */
    virtual void restoreState(const RestoreContext& context) = 0;

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
    virtual ExecState getNext(BSONObj* out, RecordId* dlOut) = 0;

    /**
     * Similar to 'getNext()', but returns a Document rather than a BSONObj.
     *
     * Callers should generally prefer the BSONObj variant, since not all implementations of
     * PlanExecutor use Document/Value as their runtime value format. These implementations will
     * typically just convert the BSON to Document on behalf of the caller.
     */
    virtual ExecState getNextDocument(Document* objOut, RecordId* dlOut) = 0;

    /**
     * Returns 'true' if the plan is done producing results (or writing), 'false' otherwise.
     *
     * Tailable cursors are a possible exception to this: they may have further results even if
     * isEOF() returns true.
     */
    virtual bool isEOF() = 0;

    /**
     * If this plan executor was constructed to execute a count implementation, e.g. it was obtained
     * by calling 'getExecutorCount()', then executes the count operation and returns the result.
     * Illegal to call on other plan executors.
     */
    virtual long long executeCount() = 0;

    /**
     * If this plan executor was constructed to execute an update, e.g. it was obtained by calling
     * 'getExecutorUpdate()', then executes the update operation and returns an 'UpdateResult'
     * describing the outcome. Illegal to call on other plan executors.
     */
    virtual UpdateResult executeUpdate() = 0;

    /**
     * If this plan executor has already executed an update operation, returns the an 'UpdateResult'
     * describing the outcome of the update. Illegal to call if either 1) the PlanExecutor is not
     * an update PlanExecutor, or 2) the PlanExecutor has not yet been executed either with
     * 'executeUpdate()' or by calling 'getNext()' until end-of-stream.
     */
    virtual UpdateResult getUpdateResult() const = 0;

    /**
     * If this plan executor was constructed to execute a delete, e.g. it was obtained by calling
     * 'getExecutorDelete()', then executes the delete operation and returns the number of documents
     * that were deleted. Illegal to call on other plan executors.
     */
    virtual long long executeDelete() = 0;

    /**
     * If this plan executor has already executed a batched delete operation, returns the
     * 'BatchedDeleteStats' describing the outcome of the batched delete. Illegal to call if either
     * 1) the PlanExecutor is not a delete PlanExecutor that executed a batched delete, or 2) the
     * PlanExecutor has not yet been executed either with 'executeDelete()' or by calling
     * 'getNext()' until end-of-stream.
     */
    virtual BatchedDeleteStats getBatchedDeleteStats() = 0;

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
     * Stash the BSONObj so that it gets returned from the PlanExecutor a subsequent call to
     * getNext(). Implementations should NOT support returning stashed BSON objects using
     * 'getNextDocument()'. Only 'getNext()' should return the stashed BSON objects.
     *
     * Enqueued documents are returned in LIFO order. The stashed results are exhausted before
     * generating further results from the underlying query plan.
     *
     * Subsequent calls to getNext() must request the BSONObj and *not* the RecordId.
     */
    virtual void stashResult(const BSONObj& obj) = 0;

    virtual bool isMarkedAsKilled() const = 0;
    virtual Status getKillStatus() = 0;

    virtual bool isDisposed() const = 0;

    /**
     * If the last oplog timestamp is being tracked for this PlanExecutor, return it.
     * Otherwise return a null timestamp.
     */
    virtual Timestamp getLatestOplogTimestamp() const = 0;

    /**
     * If this PlanExecutor is tracking change stream or other resume tokens, returns the most
     * recent token for the batch that is currently being built. Otherwise, returns an empty object.
     */
    virtual BSONObj getPostBatchResumeToken() const = 0;

    virtual LockPolicy lockPolicy() const = 0;

    /**
     * Returns a PlanExplainer instance to generate plan details and execution stats tracked by this
     * executor.
     *
     * Implementations must be able to successfully generate and return stats even if the
     * PlanExecutor has issued a query-fatal exception and the executor cannot be used for further
     * query execution.
     */
    virtual const PlanExplainer& getPlanExplainer() const = 0;

    /*
     * Virtual methods to enable using save/restore logic that stashes the RecoveryUnit on the
     * ClientCursor for future getMore commands in order to retain valid and positioned cursors.
     */
    virtual void enableSaveRecoveryUnitAcrossCommandsIfSupported() = 0;
    virtual bool isSaveRecoveryUnitAcrossCommandsEnabled() const = 0;

    /**
     * For queries that have multiple executors, this can be used to differentiate between them.
     */
    virtual boost::optional<StringData> getExecutorType() const {
        return boost::none;
    }

    /**
     * Describes the query framework which an executor used.
     */
    enum class QueryFramework {
        // Null value.
        kUnknown,
        // The entirety of this plan was executed in the classic execution engine.
        kClassicOnly,
        // This plan was executed using classic document source and any find pushdown was executed
        // in the classic execution engine.
        kClassicHybrid,
        // The entirety of this plan was exectued in SBE via stage builders.
        kSBEOnly,
        // A portion of this plan was executed in SBE via stage builders.
        kSBEHybrid,
        // The entirely of this plan was executed using CQF. Hybrid CQF plans are not possible.
        kCQF
    };

    /**
     * Returns the query framework that this executor used.
     */
    virtual QueryFramework getQueryFramework() const = 0;

    /**
     * Sets whether the executor needs to return owned data.
     */
    virtual void setReturnOwnedData(bool returnOwnedData){};
};

}  // namespace mongo
