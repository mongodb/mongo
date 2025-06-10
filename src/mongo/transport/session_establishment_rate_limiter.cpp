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

#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session_establishment_rate_limiter_utils.h"
#include "mongo/transport/transport_options_gen.h"

namespace mongo {
namespace transport {

namespace {
const auto getGlobalSessionEstablishmentRateLimiter =
    ServiceContext::declareDecoration<SessionEstablishmentRateLimiter>();

}

SessionEstablishmentRateLimiter::SessionEstablishmentRateLimiter()
    : _rateLimiter(gIngressConnectionEstablishmentRatePerSec.load(),
                   gIngressConnectionEstablishmentRatePerSec.load() *
                       gIngressConnectionEstablishmentBurstCapacitySecs.load(),
                   gIngressConnectionEstablishmentMaxQueueDepth.load(),
                   "SessionEstablishmentRateLimiter") {}

SessionEstablishmentRateLimiter* SessionEstablishmentRateLimiter::get(ServiceContext& svcCtx) {
    return &getGlobalSessionEstablishmentRateLimiter(svcCtx);
}

Status SessionEstablishmentRateLimiter::throttleIfNeeded(Client* client) {
    // Check if the session is exempt from rate limiting based on its IP.
    serverGlobalParams.maxEstablishingConnsOverride.refreshSnapshot(_maxEstablishingConnsOverride);
    if (_maxEstablishingConnsOverride &&
        isExemptedByCIDRList(client->session(), *_maxEstablishingConnsOverride)) {
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
            _interruptedDueToClientDisconnect.increment();
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
