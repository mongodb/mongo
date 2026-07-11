// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/connections_server_status_section.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/transport_layer_manager.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace mongo::transport {

bool Connections::includeByDefault() const {
    return true;
}

BSONObj Connections::generateSection(OperationContext* opCtx, const BSONElement&) const {
    auto* tlm = opCtx->getServiceContext()->getTransportLayerManager();
    if (!tlm)
        return {};

    // Collect all session managers that opt in to this section.
    std::vector<SessionManagerCommon*> managers;
    tlm->forEach([&](TransportLayer* tl) {
        if (auto* sm = dynamic_cast<SessionManagerCommon*>(tl->getSessionManager())) {
            if (sm->shouldIncludeInConnectionsServerStatus()) {
                managers.push_back(sm);
            }
        }
    });

    if (managers.empty())
        return {};

    SessionManagerCommon::SessionStats totals;
    HelloMetrics totalHelloMetrics;
    int64_t totalLimitExempt = 0;
    int64_t totalQueued = 0;
    int64_t totalRateLimiterRejected = 0;
    int64_t totalRateLimiterExempted = 0;
    int64_t totalRateLimiterInterrupted = 0;
    for (auto* sm : managers) {
        auto s = sm->getSessionStats();
        totals.numOpenSessions += s.numOpenSessions;
        // Currently, all session managers are configured by the same maxConns config option,
        // so maxOpenSessions cannot differ at runtime. Take the max to be safe.
        totals.maxOpenSessions = std::max(totals.maxOpenSessions, s.maxOpenSessions);
        totals.numCreatedSessions += s.numCreatedSessions;
        totals.numRejectedSessions += s.numRejectedSessions;
        totals.numActiveOperations += s.numActiveOperations;
        totals.numLoadBalancedSessions += s.numLoadBalancedSessions;
        totals.numPrioritySessions += s.numPrioritySessions;
        totalHelloMetrics += sm->helloMetrics;
        totalLimitExempt += sm->serviceExecutorStats.limitExempt.load();
        auto& rateLimiter = sm->getSessionEstablishmentRateLimiter();
        totalQueued += rateLimiter.queued();
        totalRateLimiterRejected += rateLimiter.rejected();
        totalRateLimiterExempted += rateLimiter.exempted();
        totalRateLimiterInterrupted += rateLimiter.interruptedDueToClientDisconnect();
    }

    BSONObjBuilder bb;
    const auto appendInt = [&](std::string_view n, int64_t v) {
        bb.append(n, static_cast<int>(v));
    };

    appendInt("current", totals.numOpenSessions - totalQueued);
    appendInt("available", totals.maxOpenSessions - totals.numOpenSessions);
    appendInt("totalCreated", totals.numCreatedSessions);
    appendInt("rejected", totals.numRejectedSessions);
    appendInt("active", totals.numActiveOperations - totalQueued);
    appendInt("queuedForEstablishment", totalQueued);

    {
        BSONObjBuilder sub(bb.subobjStart("establishmentRateLimit"));
        sub.append("rejected", static_cast<int>(totalRateLimiterRejected));
        sub.append("exempted", static_cast<int>(totalRateLimiterExempted));
        sub.append("interruptedDueToClientDisconnect",
                   static_cast<int>(totalRateLimiterInterrupted));
    }

    appendInt("threaded", totals.numOpenSessions - totalQueued);

    auto maxIncomingConnsOverride = serverGlobalParams.maxIncomingConnsOverride.makeSnapshot();
    const bool priorityPortEnabled = serverGlobalParams.priorityPort.has_value();
    if (priorityPortEnabled || (maxIncomingConnsOverride && !maxIncomingConnsOverride->empty())) {
        appendInt("limitExempt", totalLimitExempt);
    }

    totalHelloMetrics.serialize(&bb);
    if (auto adminExec = ServiceExecutorReserved::get(opCtx->getServiceContext())) {
        BSONObjBuilder section(bb.subobjStart("adminConnections"));
        adminExec->appendStats(&section);
    }

    bb.append("loadBalanced", static_cast<int>(totals.numLoadBalancedSessions));
    if (gFeatureFlagDedicatedPortForPriorityOperations.isEnabled()) {
        bb.append("priority", static_cast<int>(totals.numPrioritySessions));
    }

    return bb.obj();
}

auto& connections = *ServerStatusSectionBuilder<Connections>("connections");

}  // namespace mongo::transport
