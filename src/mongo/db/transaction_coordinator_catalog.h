
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

#include <boost/optional/optional.hpp>
#include <map>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class TransactionCoordinator;

/**
 * A container for TransactionCoordinator objects, indexed by logical session id and transaction
 * number. It allows holding several coordinator objects per session. It also knows how to recreate
 * itself from the config.txnCommitDecisions collection, which will be done on transition to
 * primary (whether from startup or ordinary step up).
 */
class TransactionCoordinatorCatalog
    : public std::enable_shared_from_this<TransactionCoordinatorCatalog> {
    MONGO_DISALLOW_COPYING(TransactionCoordinatorCatalog);

public:
    TransactionCoordinatorCatalog();
    virtual ~TransactionCoordinatorCatalog();

    /**
     * Creates a coordinator, inserts it into the catalog, and returns the newly created
     * coordinator.
     *
     * Note: Creating a duplicate coordinator for the given session id and transaction number
     * is not allowed and will lead to an invariant failure. Users of the catalog must ensure this
     * does not take place.
     */
    std::shared_ptr<TransactionCoordinator> create(LogicalSessionId lsid, TxnNumber txnNumber);

    /**
     * Returns the coordinator with the given session id and transaction number, if it exists. If it
     * does not exist, return nullptr.
     */
    std::shared_ptr<TransactionCoordinator> get(LogicalSessionId lsid, TxnNumber txnNumber);

    /**
     * Returns the coordinator with the highest transaction number with the given session id, if it
     * exists. If it does not exist, return boost::none.
     */
    boost::optional<std::pair<TxnNumber, std::shared_ptr<TransactionCoordinator>>>
    getLatestOnSession(LogicalSessionId lsid);

    /**
     * Removes the coordinator with the given session id and transaction number from the catalog, if
     * one exists.
     *
     * Note: The coordinator must be in a state suitable for removal (i.e. committed or aborted).
     */
    void remove(LogicalSessionId lsid, TxnNumber txnNumber);

private:
    /**
     * Protects the _coordinatorsBySession map.
     */
    stdx::mutex _mutex;

    /**
     * Contains TransactionCoordinator objects by session id and transaction number. May contain
     * more than one coordinator per session. All coordinators for a session that do not correspond
     * to the latest transaction should either be in the process of committing or aborting.
     */
    LogicalSessionIdMap<std::map<TxnNumber, std::shared_ptr<TransactionCoordinator>>>
        _coordinatorsBySession;
};

}  // namespace mongo
