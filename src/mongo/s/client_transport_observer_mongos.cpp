/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/s/client_transport_observer_mongos.h"

#include <boost/optional.hpp>

#include "mongo/db/cursor_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/s/grid.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/transaction_router.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {

void killOpenCursors(OperationContext& opCtx) {
    Client* client = opCtx.getClient();
    auto ccm = Grid::get(client->getServiceContext())->getCursorManager();
    ccm->killCursorsSatisfying(&opCtx,
                               [&](CursorId, const ClusterCursorManager::CursorEntry& entry) {
                                   return entry.originatingClientUuid() == client->getUUID();
                               });
}

void killInProgressTransactions(OperationContext& opCtx) {
    Client* client = opCtx.getClient();

    // Kill any in-progress transactions over this Client connection.
    auto lsid = load_balancer_support::getMruSession(client);

    auto killToken = [&]() -> boost::optional<SessionCatalog::KillToken> {
        try {
            return SessionCatalog::get(&opCtx)->killSession(lsid);
        } catch (const ExceptionFor<ErrorCodes::NoSuchSession>&) {
            return boost::none;
        }
    }();
    if (!killToken) {
        // There was no entry in the SessionCatalog for the session most recently used by the
        // disconnecting client, so we have no transaction state to clean up.
        return;
    }
    OperationContextSession sessionCtx(&opCtx, std::move(*killToken));
    invariant(lsid == OperationContextSession::get(&opCtx)->getSessionId());

    auto txnRouter = TransactionRouter::get(&opCtx);
    if (txnRouter && txnRouter.isInitialized() && !txnRouter.isTrackingOver()) {
        txnRouter.implicitlyAbortTransaction(
            &opCtx,
            {ErrorCodes::Interrupted,
             "aborting in-progress transaction because client disconnected"});
    }
}

void ClientTransportObserverMongos::onClientDisconnect(Client* client) try {
    auto session = client->session();
    if (!session || !session->bindsToOperationState())
        return;

    auto killerOperationContext = client->makeOperationContext();

    killOpenCursors(*killerOperationContext);
    killInProgressTransactions(*killerOperationContext);
} catch (const DBException& ex) {
    LOGV2_DEBUG(8969800,
                2,
                "Encountered error while performing client connection cleanup",
                "error"_attr = ex.toStatus());
}

}  // namespace mongo
