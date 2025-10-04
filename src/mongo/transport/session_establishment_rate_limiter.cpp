/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/transport/session_establishment_rate_limiter.h"

#include "mongo/bson/json.h"
#include "mongo/transport/cidr_range_list_parameter.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_options_gen.h"

namespace mongo {
namespace transport {
namespace {

VersionedValue<CIDRList> maxEstablishingConnsOverride;

}  // namespace

// TODO: SERVER-106468 Define CIDRRangeListParameter and remove this glue code
void MaxEstablishingConnectionsOverrideServerParameter::append(OperationContext*,
                                                               BSONObjBuilder* bob,
                                                               StringData name,
                                                               const boost::optional<TenantId>&) {
    appendCIDRRangeListParameter(maxEstablishingConnsOverride, bob, name);
}

Status MaxEstablishingConnectionsOverrideServerParameter::set(const BSONElement& value,
                                                              const boost::optional<TenantId>&) {
    return setCIDRRangeListParameter(maxEstablishingConnsOverride, value.Obj());
}

Status MaxEstablishingConnectionsOverrideServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
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
    // Check if the session is exempt from rate limiting based on its IP.
    maxEstablishingConnsOverride.refreshSnapshot(_maxEstablishingConnsOverride);
    if (_maxEstablishingConnsOverride &&
        client->session()->isExemptedByCIDRList(*_maxEstablishingConnsOverride)) {
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
    subBuilder.append("exempted", _rateLimiter.stats().exemptedAdmissions.get());
    subBuilder.append("interruptedDueToClientDisconnect", _interruptedDueToClientDisconnect.get());
    subBuilder.done();
}

}  // namespace transport
}  // namespace mongo
