// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/replica_set_monitor_server_parameters.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/replica_set_monitor_server_parameters_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

ReplicaSetMonitorProtocol gReplicaSetMonitorProtocol{ReplicaSetMonitorProtocol::kStreamable};

std::string toString(ReplicaSetMonitorProtocol protocol) {
    if (protocol == ReplicaSetMonitorProtocol::kStreamable) {
        return "streamable";
    } else {
        return "sdam";
    }
}

void RSMProtocolServerParameter::append(OperationContext*,
                                        BSONObjBuilder* builder,
                                        std::string_view name,
                                        const boost::optional<TenantId>&) {
    builder->append(name, toString(gReplicaSetMonitorProtocol));
}

Status RSMProtocolServerParameter::setFromString(std::string_view protocolStr,
                                                 const boost::optional<TenantId>&) {
    if (protocolStr == toString(ReplicaSetMonitorProtocol::kStreamable)) {
        gReplicaSetMonitorProtocol = ReplicaSetMonitorProtocol::kStreamable;
    } else if (protocolStr == toString(ReplicaSetMonitorProtocol::kSdam)) {
        gReplicaSetMonitorProtocol = ReplicaSetMonitorProtocol::kSdam;
    } else {
        return Status{ErrorCodes::BadValue,
                      str::stream()
                          << "Unrecognized replicaSetMonitorProtocol '" << protocolStr << "'"};
    }
    return Status::OK();
}

}  // namespace mongo
