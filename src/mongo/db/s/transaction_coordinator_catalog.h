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
#include <map>

#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

/**
 * This class is a registry for all the active TransactionCoordinator objects, indexed by lsid and
 * txnNumber. It supports holding several coordinator objects per session.
 */
class TransactionCoordinatorCatalog {
    TransactionCoordinatorCatalog(const TransactionCoordinatorCatalog&) = delete;
    TransactionCoordinatorCatalog& operator=(const TransactionCoordinatorCatalog&) = delete;

public:
    TransactionCoordinatorCatalog();
    ~TransactionCoordinatorCatalog();

    /**
     * Marks that recovery of the catalog has completed and that operations can be run on it.
     */
    void exitStepUp(Status status);

    /**
     * Cancels any outstanding idle transaction coordinators so that they will get unregistered.
     */
    void onStepDown();

    /**
     * Inserts a coordinator to be tracked by the catalog.
     *
     * Duplicate lsid + txnNumber are not legal and will lead to invariant. The consumer of this
     * class (TransactionCoordinatorService) guarantees this will not happen through the following
     * means:
     *  - At step-up recovery time - the catalog starts empty and the coordinators inserted are read
     *    from the `config.coordinators` collection, which only contains unique entries
     *  - At regular run time - the session check-out mechanism guarantees that calls to get,
     *    followed by insert are atomic for the same lsid + txnNumber
     */
    void insert(OperationContext* opCtx,
                const LogicalSessionId& lsid,
                TxnNumber txnNumber,
                std::shared_ptr<TransactionCoordinator> coordinator,
                bool forStepUp = false);

    /**
     * Returns the coordinator with the given session id and transaction number, if it exists. If it
     * does not exist, return nullptr.
     */
    std::shared_ptr<TransactionCoordinator> get(OperationContext* opCtx,
                                                const LogicalSessionId& lsid,
                                                TxnNumber txnNumber);

    /**
     * Returns the coordinator with the highest transaction number with the given session id, if it
     * exists. If it does not exist, return boost::none.
     */
    boost::optional<std::pair<TxnNumber, std::shared_ptr<TransactionCoordinator>>>
    getLatestOnSession(OperationContext* opCtx, const LogicalSessionId& lsid);

    /**
     * Blocking method, which waits for all coordinators registered on the catalog to complete
     * (after this returns, it is guaranteed that all onCompletion futures have been set)
     */
    void join();

    /**
     * Returns a string representation of the map from LogicalSessionId to the list of TxnNumbers
     * with TransactionCoordinators currently in the catalog.
     */
    std::string toString() const;

private:
    // Map of transaction coordinators, ordered in decreasing transaction number with the most
    // recent transaction at the front
    using TransactionCoordinatorMap =
        std::map<TxnNumber, std::shared_ptr<TransactionCoordinator>, std::greater<TxnNumber>>;

    /**
     * Blocks in an interruptible wait until the catalog is not marked as having a stepup in
     * progress.
     */
    void _waitForStepUpToComplete(stdx::unique_lock<stdx::mutex>& lk, OperationContext* opCtx);

    /**
     * Removes the coordinator with the given session id and transaction number from the catalog, if
     * one exists, and if this the last coordinator in the catalog, signals that there are no active
     * coordinators.
     *
     * Note: The coordinator must be in a state suitable for removal (i.e. committed or aborted).
     */
    void _remove(const LogicalSessionId& lsid, TxnNumber txnNumber);

    /**
     * Goes through the '_coordinatorsToCleanup' list and deletes entries from it
     */
    void _cleanupCompletedCoordinators();

    /**
     * Constructs a string representation of all the coordinators registered on the catalog.
     */
    std::string _toString(WithLock wl) const;

    // Protects the state below.
    mutable stdx::mutex _mutex;

    // Contains TransactionCoordinator objects by session id and transaction number. May contain
    // more than one coordinator per session. All coordinators for a session that do not correspond
    // to the latest transaction should either be in the process of committing or aborting.
    LogicalSessionIdMap<TransactionCoordinatorMap> _coordinatorsBySession;

    // Used only for testing. Contains TransactionCoordinator objects which have completed their
    // commit coordination and would normally be expunged from memory.
    LogicalSessionIdMap<TransactionCoordinatorMap> _coordinatorsBySessionDefunct;

    // Set of coordinators which have completed, but have not yet been destroyed.
    std::list<std::shared_ptr<TransactionCoordinator>> _coordinatorsToCleanup;

    // Stores the result of the coordinator catalog's recovery attempt (the status passed to
    // exitStepUp). This is what the values mean:
    //
    // stepUpCompletionStatus = none - brand new created object (exitStepUp has not been called
    //   yet). All calls will block.
    // stepUpCompletionStatus = OK - recovery completed successfully, transactions can be
    //   coordinated
    // stepUpCompletionStatus = error - recovery completed with an error, transactions cannot be
    //   coordinated (all methods will fail with this error)
    boost::optional<Status> _stepUpCompletionStatus;

    // Signaled when recovery of the catalog completes (when _stepUpCompletionStatus transitions
    // from none to either OK or error)
    stdx::condition_variable _stepUpCompleteCV;

    // Notified when the last coordinator is removed from the catalog.
    stdx::condition_variable _noActiveCoordinatorsCV;
};

}  // namespace mongo
