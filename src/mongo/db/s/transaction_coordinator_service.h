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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/transaction_coordinator_catalog.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/db/s/transaction_coordinator_structures.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class TransactionCoordinatorService {
    TransactionCoordinatorService(const TransactionCoordinatorService&) = delete;
    TransactionCoordinatorService& operator=(const TransactionCoordinatorService&) = delete;

public:
    TransactionCoordinatorService();
    virtual ~TransactionCoordinatorService();

    /**
     * Retrieves the TransactionCoordinatorService associated with the service or operation context.
     */
    static TransactionCoordinatorService* get(OperationContext* opCtx);
    static TransactionCoordinatorService* get(ServiceContext* serviceContext);

    /**
     * Creates a new TransactionCoordinator for the given session id and transaction number, with a
     * deadline for the commit decision. If the coordinator has not decided to commit by that
     * deadline, it will abort.
     */
    void createCoordinator(OperationContext* opCtx,
                           LogicalSessionId lsid,
                           TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                           Date_t commitDeadline);

    /**
     * Outputs a vector of BSON documents to the ops out-param containing information about active
     * and idle coordinators in the system.
     */
    void reportCoordinators(OperationContext* opCtx, bool includeIdle, std::vector<BSONObj>* ops);

    /**
     * If a coordinator for the (lsid, txnNumber, txnRetryCounter) exists, delivers the participant
     * list to the coordinator, which will cause the coordinator to start coordinating the commit if
     * the coordinator had not yet received a list, and returns a Future that will contain the
     * decision when the transaction finishes committing or aborting.
     *
     * If no coordinator for the (lsid, txnNumber, txnRetryCounter) exists, returns boost::none.
     */
    boost::optional<SharedSemiFuture<txn::CommitDecision>> coordinateCommit(
        OperationContext* opCtx,
        LogicalSessionId lsid,
        TxnNumberAndRetryCounter txnNumberAndRetryCounter,
        const std::set<ShardId>& participantList);

    /**
     * If a coordinator for the (lsid, txnNumber, txnRetryCounter) exists, returns a Future that
     * will contain the decision when the transaction finishes committing or aborting.
     *
     * If no coordinator for the (lsid, txnNumber, txnRetryCounter) exists, returns boost::none.
     */
    boost::optional<SharedSemiFuture<txn::CommitDecision>> recoverCommit(
        OperationContext* opCtx,
        LogicalSessionId lsid,
        TxnNumberAndRetryCounter txnNumberAndRetryCounter);

    /**
     * Cancel commit on the coordinator for the given transaction only if it has not started yet.
     */
    void cancelIfCommitNotYetStarted(OperationContext* opCtx,
                                     LogicalSessionId lsid,
                                     TxnNumberAndRetryCounter txnNumberAndRetryCounter);

    /**
     * Initializes the CatalogAndScheduler if needed and launches a recovery async
     * task. Must be called on the primary.
     *
     * The 'recoveryDelay' argument is only used for testing in order to simulate recovery taking
     * long time.
     */
    void initializeIfNeeded(OperationContext* opCtx,
                            long long term,
                            Milliseconds recoveryDelay = Milliseconds(0));

    /**
     * Interrupts the scheduler and marks the coordinator catalog as stepping down, which triggers
     * all the coordinators to stop.
     */
    void interrupt();

    /**
     * Shuts down this service. This will no longer be usable once shutdown is called.
     */
    void shutdown();

    /**
     * Notifies this service that the provided TransactionCoordinator is finished and no longer
     * needs to be interrupted when the service is interrupted.
     */
    void notifyCoordinatorFinished(std::shared_ptr<TransactionCoordinator>);

protected:
    struct CatalogAndScheduler {
        CatalogAndScheduler(ServiceContext* service) : scheduler(service) {}

        void interrupt();
        void join();

        txn::AsyncWorkScheduler scheduler;
        TransactionCoordinatorCatalog catalog;

        boost::optional<Future<void>> recoveryTaskCompleted;
    };

    /**
     * Returns the current catalog + scheduler if initialized, otherwise throws a
     * NotWritablePrimary exception.
     */
    std::shared_ptr<CatalogAndScheduler> getCatalogAndScheduler(OperationContext* opCtx);

    virtual long long getInitTerm() const {
        stdx::lock_guard lg(_mutex);
        return _initTerm;
    };

    virtual bool pendingCleanup() const {
        stdx::lock_guard lg(_mutex);
        return _catalogAndSchedulerToCleanup != NULL;
    };

private:
    /**
     * Blocking call which waits for the old instance of _catalogAndScheduler to join and ensures
     * all tasks scheduled by that instance have completed.
     */
    void _joinAndCleanup();

    /**
     * Marks the coordinator catalog as stepping up, which blocks all incoming requests for
     * coordinators, and launches an async task after initializing to:
     * 1. Wait for the coordinators in the catalog to complete (successfully or with an error) and
     *    be removed from the catalog.
     * 2. Read all pending commit tasks from the config.transactionCoordinators collection.
     * 3. Create TransactionCoordinator objects in memory for each pending commit and launch an
     *    async task to continue coordinating its commit.
     */
    void _scheduleRecoveryTask(OperationContext* opCtx, Milliseconds recoveryDelay);

    // Contains the catalog + scheduler, which was last active before service got interrupted (if
    // any). Set when interrupted and destroyed at intialization, which are always invoked
    // sequentially by the replication stepup/stepdown machinery, so there is no need to explicitly
    // synchronize it.
    std::shared_ptr<CatalogAndScheduler> _catalogAndSchedulerToCleanup;

    // Protects the state below
    mutable ObservableMutex<stdx::mutex> _mutex;

    // The catalog + scheduler instantiated at the last initialization attempt. When nullptr, it
    // means initializeIfNeeded() has not been called yet after the last interruption (or
    // construction).
    std::shared_ptr<CatalogAndScheduler> _catalogAndScheduler;

    // Sets to true once shutdown was called at least once.
    bool _isShuttingDown{false};

    // Idenifies the term for which the catalog + scheduler is initialized.
    long long _initTerm{0};

    // Set to true during initialization to avoid multiple thread attempting to initialize at once.
    bool _isInitializing{false};

    // tracks active transactionCoordinators to be interrupted on step-down. previously they were
    // tracked implicitly through futures associated with the above CancellationSource, but that
    // does not provide a way to deregister those futures when sub sources complete.
    // NOTE: this must be an ordered container because std::weak_ptr only exposes an ordering
    // operator, it does not expose an address or other control block information that would allow
    // storage in a hash-based container.
    std::set<std::weak_ptr<TransactionCoordinator>, std::owner_less<>>
        _activeTransactionCoordinators;
};

}  // namespace mongo
