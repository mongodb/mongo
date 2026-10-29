// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/transport_options.h"

#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/cidr_range_list_parameter.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_options_gen.h"

#include <string_view>

namespace mongo::transport {

// TODO: SERVER-106468 Define CIDRRangeListParameter and remove this glue code
void MaxIncomingConnectionsOverrideServerParameter::append(OperationContext*,
                                                           BSONObjBuilder* bob,
                                                           std::string_view name,
                                                           const boost::optional<TenantId>&) {
    appendCIDRRangeListParameter(serverGlobalParams.maxIncomingConnsOverride, bob, name);
}

Status MaxIncomingConnectionsOverrideServerParameter::set(const BSONElement& value,
                                                          const boost::optional<TenantId>&) {
    return setCIDRRangeListParameter(serverGlobalParams.maxIncomingConnsOverride, value.Obj());
}

Status MaxIncomingConnectionsOverrideServerParameter::setFromString(
    std::string_view str, const boost::optional<TenantId>&) {
    return setCIDRRangeListParameter(serverGlobalParams.maxIncomingConnsOverride, fromjson(str));
}

template <typename Callback>
Status forEachSessionManager(Callback&& updateFunc) try {
    // If the global service context hasn't yet been initialized, then the parameters will be
    // set on SessionManager construction rather than through the hooks here.
    if (MONGO_likely(hasGlobalServiceContext())) {
        getGlobalServiceContext()->getTransportLayerManager()->forEach([&](auto tl) {
            if (tl->getSessionManager()) {
                updateFunc(tl->getSessionManager());
            }
        });
    }
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status onUpdateEstablishmentRefreshRate(int32_t newValue) {
    return forEachSessionManager([newValue](SessionManager* sm) {
        sm->getSessionEstablishmentRateLimiter().updateRateParameters(
            newValue, gIngressConnectionEstablishmentBurstCapacitySecs.load());
    });
}

Status onUpdateEstablishmentBurstCapacitySecs(double newValue) {
    auto refreshRate = gIngressConnectionEstablishmentRatePerSec.load();
    return forEachSessionManager([refreshRate, burstCapacitySecs = newValue](SessionManager* sm) {
        sm->getSessionEstablishmentRateLimiter().updateRateParameters(refreshRate,
                                                                      burstCapacitySecs);
    });
}

Status onUpdateEstablishmentMaxQueueDepth(int32_t newValue) {
    return forEachSessionManager([newValue](SessionManager* sm) {
        sm->getSessionEstablishmentRateLimiter().setMaxQueueDepth(newValue);
    });
}
}  // namespace mongo::transport
