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

#include "mongo/db/s/transaction_coordinator_catalog.h"

namespace mongo {

class TransactionCoordinatorService {
    TransactionCoordinatorService(const TransactionCoordinatorService&) = delete;
    TransactionCoordinatorService& operator=(const TransactionCoordinatorService&) = delete;

public:
    TransactionCoordinatorService();
    ~TransactionCoordinatorService();

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
     * Marks the coordinator catalog as stepping up, which blocks all incoming requests for
     * coordinators, and launches an async task to:
     * 1. Wait for the coordinators in the catalog to complete (successfully or with an error) and
     *    be removed from the catalog.
     * 2. Read all pending commit tasks from the config.transactionCoordinators collection.
     * 3. Create TransactionCoordinator objects in memory for each pending commit and launch an
     *    async task to continue coordinating its commit.
     *
     * The 'recoveryDelay' argument is only used for testing in order to simulate recovery taking
     * long time.
     */
    void onStepUp(OperationContext* opCtx, Milliseconds recoveryDelayForTesting = Milliseconds(0));
    void onStepDown();

    /**
     * Shuts down this service. This will no longer be usable once shutdown is called.
     */
    void shutdown();

    /**
     * Called when an already established replica set is added as a shard to a cluster. Ensures that
     * the TransactionCoordinator service is started up if the replica set is currently primary.
     */
    void onShardingInitialization(OperationContext* opCtx, bool isPrimary);

    /**
     * Cancel commit on the coordinator for the given transaction only if it has not started yet.
     */
    void cancelIfCommitNotYetStarted(OperationContext* opCtx,
                                     LogicalSessionId lsid,
                                     TxnNumberAndRetryCounter txnNumberAndRetryCounter);

    /**
     * Blocking call which waits for the previous stepUp/stepDown round to join and ensures all
     * tasks scheduled by that round have completed.
     */
    void joinPreviousRound();

private:
    struct CatalogAndScheduler {
        CatalogAndScheduler(ServiceContext* service) : scheduler(service) {}

        void onStepDown();
        void join();

        txn::AsyncWorkScheduler scheduler;
        TransactionCoordinatorCatalog catalog;

        boost::optional<Future<void>> recoveryTaskCompleted;
    };

    /**
     * Returns the current catalog + scheduler if stepUp has started, otherwise throws a
     * NotWritablePrimary exception.
     */
    std::shared_ptr<CatalogAndScheduler> _getCatalogAndScheduler(OperationContext* opCtx);

    // Contains the catalog + scheduler, which was active at the last step-down attempt (if any).
    // Set at onStepDown and destroyed at onStepUp, which are always invoked sequentially by the
    // replication machinery, so there is no need to explicitly synchronize it
    std::shared_ptr<CatalogAndScheduler> _catalogAndSchedulerToCleanup;

    // Protects the state below
    mutable Mutex _mutex = MONGO_MAKE_LATCH("TransactionCoordinatorService::_mutex");

    // The catalog + scheduler instantiated at the last step-up attempt. When nullptr, it means
    // onStepUp has not been called yet after the last stepDown (or construction).
    std::shared_ptr<CatalogAndScheduler> _catalogAndScheduler;

    // Sets to false once shutdown was called at least once.
    bool _isShuttingDown{false};
};

}  // namespace mongo
