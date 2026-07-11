// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sdam/sdam_datatypes.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

#include <ostream>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sdam {
std::string toString(const ServerType serverType) {
    switch (serverType) {
        case ServerType::kStandalone:
            return "Standalone";
        case ServerType::kMongos:
            return "Mongos";
        case ServerType::kRSPrimary:
            return "RSPrimary";
        case ServerType::kRSSecondary:
            return "RSSecondary";
        case ServerType::kRSArbiter:
            return "RSArbiter";
        case ServerType::kRSOther:
            return "RSOther";
        case ServerType::kRSGhost:
            return "RSGhost";
        case ServerType::kUnknown:
            return "Unknown";
        default:
            MONGO_UNREACHABLE;
    }
}

StatusWith<ServerType> parseServerType(std::string_view strServerType) {
    if (strServerType == "Standalone") {
        return ServerType::kStandalone;
    } else if (strServerType == "Mongos") {
        return ServerType::kMongos;
    } else if (strServerType == "RSPrimary") {
        return ServerType::kRSPrimary;
    } else if (strServerType == "RSSecondary") {
        return ServerType::kRSSecondary;
    } else if (strServerType == "RSArbiter") {
        return ServerType::kRSArbiter;
    } else if (strServerType == "RSOther") {
        return ServerType::kRSOther;
    } else if (strServerType == "RSGhost") {
        return ServerType::kRSGhost;
    } else if (strServerType == "PossiblePrimary" || strServerType == "Unknown") {
        return ServerType::kUnknown;
    } else {
        std::stringstream errorMessage;
        errorMessage << strServerType << " is an invalid ServerType.";
        return StatusWith<ServerType>(ErrorCodes::InvalidServerType, errorMessage.str());
    }
}


std::ostream& operator<<(std::ostream& os, const ServerType serverType) {
    os << toString(serverType);
    return os;
}

std::vector<ServerType> allServerTypes() {
    static auto const result = std::vector<ServerType>{ServerType::kStandalone,
                                                       ServerType::kMongos,
                                                       ServerType::kRSPrimary,
                                                       ServerType::kRSSecondary,
                                                       ServerType::kRSArbiter,
                                                       ServerType::kRSOther,
                                                       ServerType::kRSGhost,
                                                       ServerType::kUnknown};
    return result;
}


std::string toString(const TopologyType topologyType) {
    switch (topologyType) {
        case TopologyType::kReplicaSetNoPrimary:
            return "ReplicaSetNoPrimary";
        case TopologyType::kReplicaSetWithPrimary:
            return "ReplicaSetWithPrimary";
        case TopologyType::kSharded:
            return "Sharded";
        case TopologyType::kUnknown:
            return "Unknown";
        case TopologyType::kSingle:
            return "Single";
        default:
            MONGO_UNREACHABLE
    }
}

StatusWith<TopologyType> parseTopologyType(std::string_view strTopologyType) {
    if (strTopologyType == "ReplicaSetNoPrimary") {
        return TopologyType::kReplicaSetNoPrimary;
    } else if (strTopologyType == "ReplicaSetWithPrimary") {
        return TopologyType::kReplicaSetWithPrimary;
    } else if (strTopologyType == "Sharded") {
        return TopologyType::kSharded;
    } else if (strTopologyType == "Unknown") {
        return TopologyType::kUnknown;
    } else if (strTopologyType == "Single") {
        return TopologyType::kSingle;
    } else {
        std::stringstream errorMessage;
        errorMessage << strTopologyType << " is an invalid TopologyType.";
        return StatusWith<TopologyType>(ErrorCodes::InvalidTopologyType, errorMessage.str());
    }
}

std::ostream& operator<<(std::ostream& os, const TopologyType topologyType) {
    os << toString(topologyType);
    return os;
}

std::vector<TopologyType> allTopologyTypes() {
    static auto const result = std::vector<TopologyType>{TopologyType::kSingle,
                                                         TopologyType::kReplicaSetNoPrimary,
                                                         TopologyType::kReplicaSetWithPrimary,
                                                         TopologyType::kSharded,
                                                         TopologyType::kUnknown};
    return result;
}

const HostAndPort& HelloOutcome::getServer() const {
    return _server;
}
bool HelloOutcome::isSuccess() const {
    return _success;
}
const boost::optional<BSONObj>& HelloOutcome::getResponse() const {
    return _response;
}
const boost::optional<HelloRTT>& HelloOutcome::getRtt() const {
    return _rtt;
}
const boost::optional<TopologyVersion>& HelloOutcome::getTopologyVersion() const {
    return _topologyVersion;
}
const std::string& HelloOutcome::getErrorMsg() const {
    return _errorMsg;
}

BSONObj HelloOutcome::toBSON() const {
    BSONObjBuilder builder;
    builder.append("host", _server.toString());
    builder.append("success", _success);

    if (_errorMsg != "")
        builder.append("errorMessage", _errorMsg);

    if (_topologyVersion)
        builder.append("topologyVersion", _topologyVersion->toBSON());

    if (_rtt)
        builder.append("duration", _rtt->toBSON());

    if (_response)
        builder.append("response", *_response);

    return builder.obj();
}
};  // namespace mongo::sdam
