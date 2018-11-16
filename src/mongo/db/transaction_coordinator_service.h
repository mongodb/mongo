
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

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/transaction_coordinator.h"
#include "mongo/db/transaction_coordinator_catalog.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo {

class ShardId;
class OperationContext;
class ServiceContext;

class TransactionCoordinatorService final {
    MONGO_DISALLOW_COPYING(TransactionCoordinatorService);

public:
    TransactionCoordinatorService();
    ~TransactionCoordinatorService() = default;

    /**
     * Shuts down the thread pool used for executing commits.
     */
    void shutdown();

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
                           TxnNumber txnNumber,
                           Date_t commitDeadline);

    /**
     * If a coordinator for the (lsid, txnNumber) exists, delivers the participant list to the
     * coordinator, which will cause the coordinator to start coordinating the commit if the
     * coordinator had not yet received a list, and returns a Future that will contain the decision
     * when the transaction finishes committing or aborting.
     *
     * If no coordinator for the (lsid, txnNumber) exists, returns boost::none.
     */
    boost::optional<Future<TransactionCoordinator::CommitDecision>> coordinateCommit(
        OperationContext* opCtx,
        LogicalSessionId lsid,
        TxnNumber txnNumber,
        const std::set<ShardId>& participantList);

    /**
     * If a coordinator for the (lsid, txnNumber) exists, returns a Future that will contain the
     * decision when the transaction finishes committing or aborting.
     *
     * If no coordinator for the (lsid, txnNumber) exists, returns boost::none.
     */
    boost::optional<Future<TransactionCoordinator::CommitDecision>> recoverCommit(
        OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber);

private:
    std::shared_ptr<TransactionCoordinatorCatalog> _coordinatorCatalog;

    ThreadPool _threadPool;
};

}  // namespace mongo
