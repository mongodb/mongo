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
#include "server_selector.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/sdam/sdam_configuration_parameters_gen.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <ostream>
#include <ratio>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::sdam {
MONGO_FAIL_POINT_DEFINE(sdamServerSelectorIgnoreLatencyWindow);

ServerSelector::~ServerSelector() {}

thread_local PseudoRandom SdamServerSelector::_random = PseudoRandom(SecureRandom().nextInt64());

SdamServerSelector::SdamServerSelector(const SdamConfiguration& config) : _config(config) {}

void SdamServerSelector::_getCandidateServers(std::vector<ServerDescriptionPtr>* result,
                                              const TopologyDescriptionPtr topologyDescription,
                                              ReadPreferenceSetting effectiveCriteria,
                                              const std::vector<HostAndPort>& excludedHosts) {
    // when querying the primary we don't need to consider tags
    bool shouldTagFilter = true;

    if (!effectiveCriteria.minClusterTime.isNull()) {
        auto eligibleServers =
            topologyDescription->findServers([excludedHosts](const ServerDescriptionPtr& s) {
                auto isPrimaryOrSecondary = (s->getType() == ServerType::kRSPrimary ||
                                             s->getType() == ServerType::kRSSecondary);
                auto isNotExcluded =
                    (std::find(excludedHosts.begin(), excludedHosts.end(), s->getAddress()) ==
                     excludedHosts.end());
                return (isPrimaryOrSecondary && isNotExcluded);
            });

        auto beginIt = eligibleServers.begin();
        auto endIt = eligibleServers.end();
        auto maxIt = std::max_element(beginIt,
                                      endIt,
                                      [topologyDescription](const ServerDescriptionPtr& left,
                                                            const ServerDescriptionPtr& right) {
                                          return left->getOpTime() < right->getOpTime();
                                      });

        if (maxIt != endIt) {
            auto maxOpTime = (*maxIt)->getOpTime();
            if (maxOpTime->getTimestamp() < effectiveCriteria.minClusterTime) {
                // ignore minClusterTime
                effectiveCriteria.minClusterTime = Timestamp{};
            }
        }
    }

    switch (effectiveCriteria.pref) {
        case ReadPreference::Nearest: {
            auto filter = (topologyDescription->getType() != TopologyType::kSharded)
                ? nearestFilter(effectiveCriteria, excludedHosts)
                : shardedFilter(effectiveCriteria, excludedHosts);
            *result = topologyDescription->findServers(filter);
            break;
        }

        case ReadPreference::SecondaryOnly:
            *result =
                topologyDescription->findServers(secondaryFilter(effectiveCriteria, excludedHosts));
            break;

        case ReadPreference::PrimaryOnly: {
            const auto primaryCriteria = ReadPreferenceSetting(effectiveCriteria.pref);
            *result =
                topologyDescription->findServers(primaryFilter(primaryCriteria, excludedHosts));
            shouldTagFilter = false;
            break;
        }

        case ReadPreference::PrimaryPreferred: {
            // ignore tags and max staleness for primary query
            auto primaryCriteria = ReadPreferenceSetting(ReadPreference::PrimaryOnly);
            _getCandidateServers(result, topologyDescription, primaryCriteria, excludedHosts);
            if (result->size()) {
                shouldTagFilter = false;
                break;
            }

            // keep tags and maxStaleness for secondary query
            auto secondaryCriteria = effectiveCriteria;
            secondaryCriteria.pref = ReadPreference::SecondaryOnly;
            _getCandidateServers(result, topologyDescription, secondaryCriteria, excludedHosts);
            break;
        }

        case ReadPreference::SecondaryPreferred: {
            // keep tags and maxStaleness for secondary query
            auto secondaryCriteria = effectiveCriteria;
            secondaryCriteria.pref = ReadPreference::SecondaryOnly;
            _getCandidateServers(result, topologyDescription, secondaryCriteria, excludedHosts);
            if (result->size()) {
                break;
            }

            // ignore tags and maxStaleness for primary query
            shouldTagFilter = false;
            auto primaryCriteria = ReadPreferenceSetting(ReadPreference::PrimaryOnly);
            _getCandidateServers(result, topologyDescription, primaryCriteria, excludedHosts);
            break;
        }

        default:
            MONGO_UNREACHABLE
    }

    if (shouldTagFilter) {
        filterTags(result, effectiveCriteria.tags);
    }
}

boost::optional<std::vector<ServerDescriptionPtr>> SdamServerSelector::selectServers(
    const TopologyDescriptionPtr topologyDescription,
    const ReadPreferenceSetting& criteria,
    const std::vector<HostAndPort>& excludedHosts) {
    ReadPreferenceSetting effectiveCriteria = [&criteria](TopologyType topologyType) {
        if (topologyType != TopologyType::kSharded) {
            return criteria;
        } else {
            // Topology type Sharded should ignore read preference fields
            return ReadPreferenceSetting(ReadPreference::Nearest);
        };
    }(topologyDescription->getType());


    // If the topology wire version is invalid, raise an error
    if (!topologyDescription->isWireVersionCompatible()) {
        uasserted(ErrorCodes::IncompatibleServerVersion,
                  *topologyDescription->getWireVersionCompatibleError());
    }

    if (criteria.maxStalenessSeconds.count()) {
        _verifyMaxstalenessLowerBound(topologyDescription, effectiveCriteria.maxStalenessSeconds);
        _verifyMaxstalenessWireVersions(topologyDescription, effectiveCriteria.maxStalenessSeconds);
    }

    if (topologyDescription->getType() == TopologyType::kUnknown) {
        return boost::none;
    }

    if (topologyDescription->getType() == TopologyType::kSingle) {
        auto servers = topologyDescription->getServers();
        return (servers.size() && servers[0]->getType() != ServerType::kUnknown)
            ? boost::optional<std::vector<ServerDescriptionPtr>>{{servers[0]}}
            : boost::none;
    }

    std::vector<ServerDescriptionPtr> results;
    _getCandidateServers(&results, topologyDescription, effectiveCriteria, excludedHosts);

    if (results.size()) {
        if (MONGO_unlikely(sdamServerSelectorIgnoreLatencyWindow.shouldFail())) {
            return results;
        }

        ServerDescriptionPtr minServer =
            *std::min_element(results.begin(), results.end(), LatencyWindow::rttCompareFn);

        invariant(minServer->getRtt());
        auto latencyWindow = LatencyWindow(*minServer->getRtt(), _config.getLocalThreshold());
        latencyWindow.filterServers(&results);

        // latency window should always leave at least one result
        invariant(results.size());
        std::shuffle(std::begin(results), std::end(results), _random.urbg());
        return results;
    }

    return boost::none;
}

ServerDescriptionPtr SdamServerSelector::_randomSelect(
    const std::vector<ServerDescriptionPtr>& servers) const {
    return servers[_random.nextInt64(servers.size())];
}

boost::optional<ServerDescriptionPtr> SdamServerSelector::selectServer(
    const TopologyDescriptionPtr topologyDescription,
    const ReadPreferenceSetting& criteria,
    const std::vector<HostAndPort>& excludedHosts) {
    auto servers = selectServers(topologyDescription, criteria, excludedHosts);
    return servers ? boost::optional<ServerDescriptionPtr>(_randomSelect(*servers)) : boost::none;
}

bool SdamServerSelector::_containsAllTags(ServerDescriptionPtr server, const BSONObj& tags) {
    auto serverTags = server->getTags();
    for (auto& checkTag : tags) {
        auto checkKey = checkTag.fieldName();
        auto checkValue = checkTag.String();
        auto pos = serverTags.find(checkKey);
        if (pos == serverTags.end() || pos->second != checkValue) {
            return false;
        }
    }
    return true;
}

void SdamServerSelector::filterTags(std::vector<ServerDescriptionPtr>* servers,
                                    const TagSet& tagSet) {
    const auto& tagSetList = tagSet.getTagBSON();

    if (tagSetList.isEmpty()) {
        return;
    }

    for (const auto& tagSetElem : tagSetList) {
        if (tagSetElem.type() != BSONType::object) {
            LOGV2_WARNING(4671202,
                          "Invalid tag set specified for server selection; tag sets should be"
                          " specified as a BSON object",
                          "tag"_attr = tagSetElem);
            continue;
        }

        const auto predicate = [&](const ServerDescriptionPtr& s) {
            const bool shouldRemove = !_containsAllTags(s, tagSetElem.embeddedObject());
            return shouldRemove;
        };

        auto it = std::remove_if(servers->begin(), servers->end(), predicate);
        // If none of the server descriptions match the tag set, then continue on to check the next
        // tag set in the list. Otherwise, if at least one of the server descriptions match the tag
        // set criteria, then we've found our preferred host(s) to read from.
        if (it != servers->begin()) {
            servers->erase(it, servers->end());
            return;
        }
    }

    // Getting here means a non-empty tag set list was specified but none of the server descriptions
    // matched any of the tag sets in the list. We've therefore failed to find any server
    // description matching the read preference tag criteria.
    servers->clear();
}

bool SdamServerSelector::recencyFilter(const ReadPreferenceSetting& readPref,
                                       const ServerDescriptionPtr& s) {
    bool result = true;

    if (!readPref.minClusterTime.isNull()) {
        result = (s->getOpTime() && s->getOpTime()->getTimestamp() >= readPref.minClusterTime);
    }

    if (readPref.maxStalenessSeconds.count()) {
        auto topologyDescription = s->getTopologyDescription();
        invariant(topologyDescription);
        auto staleness = _calculateStaleness(*topologyDescription, s);
        result = result && (staleness <= readPref.maxStalenessSeconds);
    }

    return result;
}

void SdamServerSelector::_verifyMaxstalenessLowerBound(TopologyDescriptionPtr topologyDescription,
                                                       Seconds maxStalenessSeconds) {
    static const auto kIdleWritePeriodMs = Milliseconds{10000};
    auto topologyType = topologyDescription->getType();
    if (topologyType == TopologyType::kReplicaSetWithPrimary ||
        topologyType == TopologyType::kReplicaSetNoPrimary) {
        const auto lowerBoundMs =
            sdamHeartBeatFrequencyMs + durationCount<Milliseconds>(kIdleWritePeriodMs);

        if (durationCount<Milliseconds>(maxStalenessSeconds) < lowerBoundMs) {
            // using if to avoid creating the string if there's no error
            std::stringstream ss;
            ss << "Parameter maxStalenessSeconds cannot be less than "
               << durationCount<Seconds>(Milliseconds{lowerBoundMs});
            uassert(ErrorCodes::MaxStalenessOutOfRange, ss.str(), false);
        }
    }
}

void SdamServerSelector::_verifyMaxstalenessWireVersions(TopologyDescriptionPtr topologyDescription,
                                                         Seconds maxStalenessSeconds) {
    for (auto& server : topologyDescription->getServers()) {
        uassert(ErrorCodes::IncompatibleServerVersion,
                "Incompatible wire version",
                server->getType() == ServerType::kUnknown ||
                    server->getMaxWireVersion() >= WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    }
}

void LatencyWindow::filterServers(std::vector<ServerDescriptionPtr>* servers) {
    servers->erase(std::remove_if(servers->begin(),
                                  servers->end(),
                                  [&](const ServerDescriptionPtr& s) {
                                      // Servers that have made it to this stage are not ServerType
                                      // == kUnknown, so they must have an associated latency.
                                      invariant(s->getType() != ServerType::kUnknown);
                                      invariant(s->getRtt());
                                      return !this->isWithinWindow(*s->getRtt());
                                  }),
                   servers->end());
}

bool LatencyWindow::isWithinWindow(HelloRTT latency) {
    return lower <= latency && latency <= upper;
}
}  // namespace mongo::sdam
