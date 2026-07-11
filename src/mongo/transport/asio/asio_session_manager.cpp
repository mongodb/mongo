// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/asio/asio_session_manager.h"

#include "mongo/db/service_context.h"
#include "mongo/transport/message_filter_hooks.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_manager.h"

#include <algorithm>

namespace mongo::transport {

std::string AsioSessionManager::getClientThreadName(const Session& session) const {
    return fmt::format("conn{}", session.id());
}

void AsioSessionManager::configureServiceExecutorContext(Client& client,
                                                         bool isPrivilegedSession) const {
    auto seCtx = std::make_unique<ServiceExecutorContext>();
    seCtx->setThreadModel(ServiceExecutorContext::kSynchronous);
    seCtx->setCanUseReserved(isPrivilegedSession);
    std::lock_guard lk(client);
    ServiceExecutorContext::set(&client, std::move(seCtx));
}

void AsioSessionManager::startSession(std::shared_ptr<Session> session) {
    invariant(session);
    MessageHooks::onConnectionEstablished(*session);
    SessionManagerCommon::startSession(std::move(session));
}

ConnectionsStatsSnapshot AsioSessionManager::getConnectionsStatsSnapshot() const {
    auto sessionCount = static_cast<int64_t>(numOpenSessions());
    auto queued = _sessionEstablishmentRateLimiter.queued();

    // Each field is derived from independent atomic loads with no lock spanning the group,
    // so a concurrent change between reads can produce a transient negative value (e.g.
    // `queued` advancing past `sessionCount` between the two loads, or a completion landing
    // between the `total` and `completed` reads inside getActiveOperations()). Clamp to zero
    // so we never publish obviously invalid data to OTel consumers.
    auto clamp = [](int64_t v) {
        return std::max<int64_t>(0, v);
    };

    return {
        .current = clamp(sessionCount - queued),
        .available = clamp(static_cast<int64_t>(maxOpenSessions()) - sessionCount),
        .totalCreated = static_cast<int64_t>(numCreatedSessions()),
        .rejected = static_cast<int64_t>(numRejectedSessions()) +
            _sessionEstablishmentRateLimiter.rejected(),
        .active = clamp(static_cast<int64_t>(getActiveOperations()) - queued),
    };
}

ConnectionsStatsSnapshot collectConnectionsStatsSnapshot(ServiceContext* svcCtx) {
    auto* tlm = svcCtx->getTransportLayerManager();
    if (!tlm)
        return {};
    ConnectionsStatsSnapshot snap;
    int asioSeen = 0;
    tlm->forEach([&](TransportLayer* tl) {
        // AsioSesionManager is the only manager currently used in production.
        if (auto* sm = dynamic_cast<AsioSessionManager*>(tl->getSessionManager())) {
            dassert(++asioSeen == 1, "Multiple AsioSessionManagers found");
            snap = sm->getConnectionsStatsSnapshot();
        }
    });
    return snap;
}

}  // namespace mongo::transport
