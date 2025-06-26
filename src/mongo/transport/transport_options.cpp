/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/transport/transport_options.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/cidr_range_list_parameter.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_options_gen.h"

namespace mongo::transport {

// TODO: SERVER-106468 Define CIDRRangeListParameter and remove this glue code
void MaxIncomingConnectionsOverrideServerParameter::append(OperationContext*,
                                                           BSONObjBuilder* bob,
                                                           StringData name,
                                                           const boost::optional<TenantId>&) {
    appendCIDRRangeListParameter(serverGlobalParams.maxIncomingConnsOverride, bob, name);
}

Status MaxIncomingConnectionsOverrideServerParameter::set(const BSONElement& value,
                                                          const boost::optional<TenantId>&) {
    return setCIDRRangeListParameter(serverGlobalParams.maxIncomingConnsOverride, value.Obj());
}

Status MaxIncomingConnectionsOverrideServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
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
            newValue, newValue * gIngressConnectionEstablishmentBurstCapacitySecs.load());
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
