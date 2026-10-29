// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/client_transport_observer_mongos.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/transaction_router.h"

#include <boost/optional.hpp>

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
