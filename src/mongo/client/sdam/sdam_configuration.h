// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/sdam/sdam_configuration_parameters_gen.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/db/server_options.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sdam {
class SdamConfiguration {
public:
    SdamConfiguration() : SdamConfiguration(boost::none) {};

    /**
     * Initialize the TopologyDescription. This constructor may uassert if the provided
     * configuration options are not valid according to the Server Discovery & Monitoring Spec.
     *
     * Initial Servers
     * initial servers may be set to a seed list of one or more server addresses.
     *
     * Initial TopologyType
     * The initial TopologyType may be set to Single, Unknown, or ReplicaSetNoPrimary.
     *
     * Initial setName
     * The client's initial replica set name is required in order to initially configure the
     * topology type as ReplicaSetNoPrimary.
     *
     * Allowed configuration combinations
     * TopologyType Single cannot be used with multiple seeds.
     * If setName is not null, only TopologyType ReplicaSetNoPrimary and Single, are
     * allowed.
     */
    explicit SdamConfiguration(
        boost::optional<std::vector<HostAndPort>> seedList,
        TopologyType initialType = TopologyType::kUnknown,
        Milliseconds heartBeatFrequencyMs = Milliseconds(sdamHeartBeatFrequencyMs),
        Milliseconds connectTimeoutMs = Milliseconds(sdamConnectTimeoutMs),
        Milliseconds localThreshholdMs =
            Milliseconds(serverGlobalParams.defaultLocalThresholdMillis),
        boost::optional<std::string> setName = boost::none);

    SdamConfiguration(boost::optional<std::vector<HostAndPort>> seedList,
                      TopologyType initialType,
                      boost::optional<std::string> setName)
        : SdamConfiguration(seedList,
                            initialType,
                            Milliseconds(sdamHeartBeatFrequencyMs),
                            Milliseconds(sdamConnectTimeoutMs),
                            Milliseconds(serverGlobalParams.defaultLocalThresholdMillis),
                            setName) {}

    /**
     * The initial set of servers to monitor in the replica set.
     */
    const boost::optional<std::vector<HostAndPort>>& getSeedList() const;

    /**
     * The initial type of the replica set.
     */
    TopologyType getInitialType() const;

    /**
     * The replica set name.
     */
    const boost::optional<std::string>& getSetName() const;

    /**
     * The frequency at which we measure RTT and "hello" responses.
     */
    Milliseconds getHeartBeatFrequency() const;

    /**
     * How long to wait to obtain a connection.
     */
    Milliseconds getConnectionTimeout() const;

    /**
     * The width of the latency window used in server selection.
     */
    Milliseconds getLocalThreshold() const;

    const BSONObj& toBson() const {
        return _bsonDoc;
    }

    static constexpr Milliseconds kMinHeartbeatFrequency = Milliseconds(500);

private:
    BSONObj _toBson() const;

    boost::optional<std::vector<HostAndPort>> _seedList;
    TopologyType _initialType;

    Milliseconds _heartbeatFrequency;
    Milliseconds _connectionTimeout;
    Milliseconds _localThreshold;

    boost::optional<std::string> _setName;
    BSONObj _bsonDoc;
};
}  // namespace mongo::sdam
