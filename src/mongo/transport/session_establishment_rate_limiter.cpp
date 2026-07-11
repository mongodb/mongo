// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/session_establishment_rate_limiter.h"

#include "mongo/bson/json.h"
#include "mongo/transport/cidr_range_list_parameter.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_options_gen.h"

#include <string_view>

namespace mongo {
namespace transport {
namespace {

VersionedValue<CIDRList> maxEstablishingConnsOverride;
thread_local VersionedValue<CIDRList>::Snapshot maxEstablishingConnsOverrideSnapshot;

}  // namespace

// TODO: SERVER-106468 Define CIDRRangeListParameter and remove this glue code
void MaxEstablishingConnectionsOverrideServerParameter::append(OperationContext*,
                                                               BSONObjBuilder* bob,
                                                               std::string_view name,
                                                               const boost::optional<TenantId>&) {
    appendCIDRRangeListParameter(maxEstablishingConnsOverride, bob, name);
}

Status MaxEstablishingConnectionsOverrideServerParameter::set(const BSONElement& value,
                                                              const boost::optional<TenantId>&) {
    return setCIDRRangeListParameter(maxEstablishingConnsOverride, value.Obj());
}

Status MaxEstablishingConnectionsOverrideServerParameter::setFromString(
    std::string_view str, const boost::optional<TenantId>&) {
    return setCIDRRangeListParameter(maxEstablishingConnsOverride, fromjson(str));
}


SessionEstablishmentRateLimiter::SessionEstablishmentRateLimiter()
    : _rateLimiter(gIngressConnectionEstablishmentRatePerSec.load(),
                   gIngressConnectionEstablishmentBurstCapacitySecs.load(),
                   gIngressConnectionEstablishmentMaxQueueDepth.load(),
                   "SessionEstablishmentRateLimiter") {}

SessionEstablishmentRateLimiter* SessionEstablishmentRateLimiter::get(ServiceContext& svcCtx,
                                                                      TransportProtocol protocol) {
    auto* const transportLayerManager = svcCtx.getTransportLayerManager();
    invariant(transportLayerManager);

    auto* tl = transportLayerManager->getTransportLayer(protocol);
    if (!tl || !tl->isIngress()) {
        return nullptr;
    }
    auto* const sessionManager = tl->getSessionManager();
    invariant(sessionManager);
    return &sessionManager->getSessionEstablishmentRateLimiter();
}

Status SessionEstablishmentRateLimiter::throttleIfNeeded(Client* client) {
    // Exempt priority port sessions from rate limiters
    if (client->isPriorityPortClient()) {
        _rateLimiter.recordExemption();
        return Status::OK();
    }

    // Exempt session from rate limiters when its IP is whitelisted
    maxEstablishingConnsOverride.refreshSnapshot(maxEstablishingConnsOverrideSnapshot);
    if (maxEstablishingConnsOverrideSnapshot &&
        client->session()->isExemptedByCIDRList(*maxEstablishingConnsOverrideSnapshot)) {
        _rateLimiter.recordExemption();
        return Status::OK();
    }

    // Create an opCtx for interruptibility and to make queued waiters return tokens
    // when the client has disconnected.
    auto establishmentOpCtx = client->makeOperationContext();
    establishmentOpCtx->markKillOnClientDisconnect();

    // Acquire a token or block until one becomes available.
    Status s = _rateLimiter.acquireToken(establishmentOpCtx.get());
    if (MONGO_unlikely(!s.isOK())) {
        if (s.code() == ErrorCodes::ClientDisconnect) {
            _interruptedDueToClientDisconnect.incrementRelaxed();
        }
    }

    return s;
}

void SessionEstablishmentRateLimiter::appendStatsConnections(BSONObjBuilder* bob) const {
    bob->append("queuedForEstablishment", queued());

    BSONObjBuilder subBuilder = bob->subobjStart("establishmentRateLimit");
    subBuilder.append("rejected", rejected());
    subBuilder.append("exempted", _rateLimiter.stats().exemptedAdmissions());
    subBuilder.append("interruptedDueToClientDisconnect", _interruptedDueToClientDisconnect.get());
    subBuilder.done();
}

}  // namespace transport
}  // namespace mongo
