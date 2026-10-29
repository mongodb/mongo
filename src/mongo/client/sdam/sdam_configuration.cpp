// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/sdam/sdam_configuration.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <iterator>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sdam {
SdamConfiguration::SdamConfiguration(boost::optional<std::vector<HostAndPort>> seedList,
                                     TopologyType initialType,
                                     Milliseconds heartBeatFrequencyMs,
                                     Milliseconds connectTimeoutMs,
                                     Milliseconds localThreshholdMs,
                                     boost::optional<std::string> setName)
    : _seedList(seedList),
      _initialType(initialType),
      _heartbeatFrequency(heartBeatFrequencyMs),
      _connectionTimeout(connectTimeoutMs),
      _localThreshold(localThreshholdMs),
      _setName(setName),
      _bsonDoc(_toBson()) {
    uassert(ErrorCodes::InvalidSeedList,
            "seed list size must be >= 1",
            !seedList || (*seedList).size() >= 1);

    uassert(ErrorCodes::InvalidSeedList,
            "TopologyType Single must have exactly one entry in the seed list.",
            _initialType != TopologyType::kSingle || (*seedList).size() == 1);

    uassert(
        ErrorCodes::InvalidTopologyType,
        "Only ToplogyTypes ReplicaSetNoPrimary and Single are allowed when a setName is provided.",
        !_setName ||
            (_initialType == TopologyType::kReplicaSetNoPrimary ||
             _initialType == TopologyType::kSingle));

    uassert(ErrorCodes::TopologySetNameRequired,
            "setName is required for ReplicaSetNoPrimary",
            _initialType != TopologyType::kReplicaSetNoPrimary || _setName);

    uassert(ErrorCodes::InvalidHeartBeatFrequency,
            "topology heartbeat must be >= 500ms",
            _heartbeatFrequency >= kMinHeartbeatFrequency);
}

const boost::optional<std::vector<HostAndPort>>& SdamConfiguration::getSeedList() const {
    return _seedList;
}

TopologyType SdamConfiguration::getInitialType() const {
    return _initialType;
}

Milliseconds SdamConfiguration::getHeartBeatFrequency() const {
    return _heartbeatFrequency;
}

const boost::optional<std::string>& SdamConfiguration::getSetName() const {
    return _setName;
}

Milliseconds SdamConfiguration::getConnectionTimeout() const {
    return _connectionTimeout;
}

Milliseconds SdamConfiguration::getLocalThreshold() const {
    return _localThreshold;
}

BSONObj SdamConfiguration::_toBson() const {
    BSONObjBuilder builder;

    if (_setName) {
        builder.append("replicaSet", *_setName);
    }

    builder.append("topologyType", toString(getInitialType()));

    if (_seedList) {
        const auto& hostAndPorts = *_seedList;
        std::vector<std::string> seedList;
        std::transform(hostAndPorts.begin(),
                       hostAndPorts.end(),
                       back_inserter(seedList),
                       [](const HostAndPort& h) { return h.toString(); });
        builder.append("seedList", seedList);
    }

    builder.append("heartbeatFrequency", _heartbeatFrequency.toBSON());
    builder.append("connectionTimeout", _connectionTimeout.toBSON());
    builder.append("localThreshhold", _localThreshold.toBSON());

    return builder.obj();
}
};  // namespace mongo::sdam
