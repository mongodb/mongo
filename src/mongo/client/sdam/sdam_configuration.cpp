/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "sdam_configuration.h"

namespace mongo::sdam {
SdamConfiguration::SdamConfiguration(boost::optional<std::vector<HostAndPort>> seedList,
                                     TopologyType initialType,
                                     mongo::Milliseconds heartBeatFrequencyMs,
                                     boost::optional<std::string> setName)
    : _seedList(seedList),
      _initialType(initialType),
      _heartBeatFrequencyMs(heartBeatFrequencyMs),
      _setName(setName) {
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
            _heartBeatFrequencyMs >= kMinHeartbeatFrequencyMS);
}

const boost::optional<std::vector<HostAndPort>>& SdamConfiguration::getSeedList() const {
    return _seedList;
}

TopologyType SdamConfiguration::getInitialType() const {
    return _initialType;
}

Milliseconds SdamConfiguration::getHeartBeatFrequency() const {
    return _heartBeatFrequencyMs;
}

const boost::optional<std::string>& SdamConfiguration::getSetName() const {
    return _setName;
}


ServerSelectionConfiguration::ServerSelectionConfiguration(
    const Milliseconds localThresholdMs, const Milliseconds serverSelectionTimeoutMs)
    : _localThresholdMs(localThresholdMs), _serverSelectionTimeoutMs(serverSelectionTimeoutMs) {}

Milliseconds ServerSelectionConfiguration::getLocalThresholdMs() const {
    return _localThresholdMs;
}

Milliseconds ServerSelectionConfiguration::getServerSelectionTimeoutMs() const {
    return _serverSelectionTimeoutMs;
}
Milliseconds ServerSelectionConfiguration::getHeartBeatFrequencyMs() const {
    return _heartBeatFrequencyMs;
}
};  // namespace mongo::sdam
