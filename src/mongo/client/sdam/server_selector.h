/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#pragma once
#include <functional>
#include <vector>

#include "mongo/client/read_preference.h"
#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/platform/random.h"

namespace mongo::sdam {
/**
 * This is the interface that allows one to select a server to satisfy a DB operation given a
 * TopologyDescription and a ReadPreferenceSetting.
 */
class ServerSelector {
public:
    /**
     * Finds a list of candidate servers according to the ReadPreferenceSetting.
     */
    virtual boost::optional<std::vector<ServerDescriptionPtr>> selectServers(
        TopologyDescriptionPtr topologyDescription,
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts = std::vector<HostAndPort>()) = 0;

    /**
     * Select a single server according to the ReadPreference and latency of the
     * ServerDescription(s). The server is selected randomly from those that match the criteria.
     */
    virtual boost::optional<ServerDescriptionPtr> selectServer(
        TopologyDescriptionPtr topologyDescription,
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts = std::vector<HostAndPort>()) = 0;

    virtual ~ServerSelector();
};
using ServerSelectorPtr = std::unique_ptr<ServerSelector>;

class SdamServerSelector : public ServerSelector {
public:
    explicit SdamServerSelector(const SdamConfiguration& config);

    boost::optional<std::vector<ServerDescriptionPtr>> selectServers(
        TopologyDescriptionPtr topologyDescription,
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts = std::vector<HostAndPort>()) override;

    boost::optional<ServerDescriptionPtr> selectServer(
        TopologyDescriptionPtr topologyDescription,
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts = std::vector<HostAndPort>()) override;

    // remove servers that do not match the TagSet
    void filterTags(std::vector<ServerDescriptionPtr>* servers, const TagSet& tagSet);

private:
    void _getCandidateServers(std::vector<ServerDescriptionPtr>* result,
                              TopologyDescriptionPtr topologyDescription,
                              const ReadPreferenceSetting& criteria,
                              const std::vector<HostAndPort>& excludedHosts);

    bool _containsAllTags(ServerDescriptionPtr server, const BSONObj& tags);

    ServerDescriptionPtr _randomSelect(const std::vector<ServerDescriptionPtr>& servers) const;

    // staleness for a ServerDescription is defined here:
    // https://github.com/mongodb/specifications/blob/master/source/server-selection/server-selection.rst#maxstalenessseconds
    Milliseconds _calculateStaleness(const TopologyDescriptionPtr& topologyDescription,
                                     const ServerDescriptionPtr& serverDescription) {
        if (serverDescription->getType() != ServerType::kRSSecondary)
            return Milliseconds(0);

        const Date_t& lastWriteDate = serverDescription->getLastWriteDate()
            ? *serverDescription->getLastWriteDate()
            : Date_t::min();

        if (topologyDescription->getType() == TopologyType::kReplicaSetWithPrimary) {
            // (S.lastUpdateTime - S.lastWriteDate) - (P.lastUpdateTime - P.lastWriteDate) +
            // heartbeatFrequencyMS

            // topologyType == kReplicaSetWithPrimary implies the validity of the primary server
            // description.
            const auto primary = topologyDescription->getPrimary();
            invariant(primary);
            const auto& primaryDescription = *primary;

            const auto& primaryLastWriteDate = primaryDescription->getLastWriteDate()
                ? *primaryDescription->getLastWriteDate()
                : Date_t::min();

            auto result = durationCount<Milliseconds>(
                              (serverDescription->getLastUpdateTime() - lastWriteDate)) -
                durationCount<Milliseconds>(
                              (primaryDescription->getLastUpdateTime() - primaryLastWriteDate)) +
                durationCount<Milliseconds>(_config.getHeartBeatFrequency());
            return Milliseconds{result};
        } else if (topologyDescription->getType() == TopologyType::kReplicaSetNoPrimary) {
            //  SMax.lastWriteDate - S.lastWriteDate + heartbeatFrequencyMS
            Date_t maxLastWriteDate = Date_t::min();

            // identify secondary with max last write date.
            for (const auto& s : topologyDescription->getServers()) {
                if (s->getType() != ServerType::kRSSecondary)
                    continue;

                const auto& sLastWriteDate =
                    s->getLastWriteDate() ? *s->getLastWriteDate() : Date_t::min();

                if (sLastWriteDate > maxLastWriteDate) {
                    maxLastWriteDate = sLastWriteDate;
                }
            }

            auto result = durationCount<Milliseconds>(maxLastWriteDate - lastWriteDate) +
                durationCount<Milliseconds>(_config.getHeartBeatFrequency());
            return Milliseconds{result};
        } else {
            // Not a replica set
            return Milliseconds(0);
        }
    }

    void _verifyMaxstalenessLowerBound(TopologyDescriptionPtr topologyDescription,
                                       Seconds maxStalenessSeconds);
    void _verifyMaxstalenessWireVersions(TopologyDescriptionPtr topologyDescription,
                                         Seconds maxStalenessSeconds);

    bool recencyFilter(const ReadPreferenceSetting& readPref, const ServerDescriptionPtr& s);

    static bool excludedHostsFilter(const std::vector<HostAndPort>& excludedHosts,
                                    const ServerDescriptionPtr& s) {
        return std::find(excludedHosts.begin(), excludedHosts.end(), s->getAddress()) ==
            excludedHosts.end();
    }

    // A SelectionFilter is a higher order function used to filter out servers from the current
    // Topology. It's return value is used as input to the TopologyDescription::findServers
    // function, and is a function that takes a ServerDescriptionPtr and returns a bool indicating
    // whether to keep this server or not based on the ReadPreference, server type, and recency
    // metrics of the server.
    using SelectionFilter = unique_function<std::function<bool(const ServerDescriptionPtr&)>(
        const ReadPreferenceSetting&, const std::vector<HostAndPort>&)>;

    const SelectionFilter secondaryFilter = [this](const ReadPreferenceSetting& readPref,
                                                   const std::vector<HostAndPort>& excludedHosts) {
        return [&](const ServerDescriptionPtr& s) {
            return (s->getType() == ServerType::kRSSecondary) && recencyFilter(readPref, s) &&
                excludedHostsFilter(excludedHosts, s);
        };
    };

    const SelectionFilter primaryFilter = [this](const ReadPreferenceSetting& readPref,
                                                 const std::vector<HostAndPort>& excludedHosts) {
        return [&](const ServerDescriptionPtr& s) {
            return (s->getType() == ServerType::kRSPrimary) && recencyFilter(readPref, s) &&
                excludedHostsFilter(excludedHosts, s);
        };
    };

    const SelectionFilter nearestFilter = [this](const ReadPreferenceSetting& readPref,
                                                 const std::vector<HostAndPort>& excludedHosts) {
        return [&](const ServerDescriptionPtr& s) {
            return (s->getType() == ServerType::kRSPrimary ||
                    s->getType() == ServerType::kRSSecondary) &&
                recencyFilter(readPref, s) && excludedHostsFilter(excludedHosts, s);
        };
    };

    const SelectionFilter shardedFilter = [this](const ReadPreferenceSetting& readPref,
                                                 const std::vector<HostAndPort>& excludedHosts) {
        return [&](const ServerDescriptionPtr& s) { return s->getType() == ServerType::kMongos; };
    };

    SdamConfiguration _config;
    static thread_local PseudoRandom _random;
};

// This is used to filter out servers based on their current latency measurements.
struct LatencyWindow {
    const HelloRTT lower;
    HelloRTT upper;

    explicit LatencyWindow(const HelloRTT lowerBound, const HelloRTT windowWidth)
        : lower(lowerBound) {
        upper = (lowerBound == HelloRTT::max()) ? lowerBound : lowerBound + windowWidth;
    }

    bool isWithinWindow(HelloRTT latency);

    // remove servers not in the latency window in-place.
    void filterServers(std::vector<ServerDescriptionPtr>* servers);

    static bool rttCompareFn(const ServerDescriptionPtr& a, const ServerDescriptionPtr& b) {
        return a->getRtt() < b->getRtt();
    }
};
}  // namespace mongo::sdam
