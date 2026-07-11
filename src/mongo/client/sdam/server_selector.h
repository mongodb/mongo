// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/targeting_metadata.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sdam {

class ServerSelector {
public:
    explicit ServerSelector(const SdamConfiguration& config);

    boost::optional<std::vector<ServerDescriptionPtr>> selectServers(
        TopologyDescriptionPtr topologyDescription,
        const ReadPreferenceSetting& criteria,
        const TargetingMetadata& targetingMetadata);

    boost::optional<ServerDescriptionPtr> selectServer(TopologyDescriptionPtr topologyDescription,
                                                       const ReadPreferenceSetting& criteria,
                                                       const TargetingMetadata& targetingMetadata);

    // remove servers that do not match the TagSet
    void filterTags(std::vector<ServerDescriptionPtr>* servers, const TagSet& tagSet);

private:
    static PseudoRandom& _random();

    void _getCandidateServers(std::vector<ServerDescriptionPtr>* result,
                              TopologyDescriptionPtr topologyDescription,
                              ReadPreferenceSetting effectiveCriteria,
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

    // Returns true when the server is allowed to be selected. Rejects:
    //   - servers in the caller-provided excludedHosts list (e.g. retry blocklists), and
    //   - servers tagged as injectors (_internalProcessType: INJECTOR), which belong to a standby
    //     cluster's replica set and are kept in the topology for replication/heartbeat
    //     tracking but must never receive client commands.
    static bool passesExclusionFilters(const std::vector<HostAndPort>& excludedHosts,
                                       const ServerDescriptionPtr& s) {
        if (s->isInjector()) {
            return false;
        }
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

    // Note that each replica-set filter below delegates to passesExclusionFilters(), so it will
    // always skip injector-tagged servers. See ServerDescription::isInjector for additional
    // details.
    const SelectionFilter secondaryFilter = [this](const ReadPreferenceSetting& readPref,
                                                   const std::vector<HostAndPort>& excludedHosts) {
        return [&](const ServerDescriptionPtr& s) {
            return (s->getType() == ServerType::kRSSecondary) && recencyFilter(readPref, s) &&
                passesExclusionFilters(excludedHosts, s);
        };
    };

    const SelectionFilter primaryFilter = [this](const ReadPreferenceSetting& readPref,
                                                 const std::vector<HostAndPort>& excludedHosts) {
        return [&](const ServerDescriptionPtr& s) {
            return (s->getType() == ServerType::kRSPrimary) && recencyFilter(readPref, s) &&
                passesExclusionFilters(excludedHosts, s);
        };
    };

    const SelectionFilter nearestFilter = [this](const ReadPreferenceSetting& readPref,
                                                 const std::vector<HostAndPort>& excludedHosts) {
        return [&](const ServerDescriptionPtr& s) {
            return (s->getType() == ServerType::kRSPrimary ||
                    s->getType() == ServerType::kRSSecondary) &&
                recencyFilter(readPref, s) && passesExclusionFilters(excludedHosts, s);
        };
    };

    const SelectionFilter shardedFilter = [this](const ReadPreferenceSetting& readPref,
                                                 const std::vector<HostAndPort>& excludedHosts) {
        return [&](const ServerDescriptionPtr& s) {
            return s->getType() == ServerType::kMongos;
        };
    };

    SdamConfiguration _config;
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
