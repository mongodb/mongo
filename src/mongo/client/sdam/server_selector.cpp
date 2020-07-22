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

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork
#include "mongo/client/sdam/topology_description.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/util/fail_point.h"

namespace mongo::sdam {
MONGO_FAIL_POINT_DEFINE(sdamServerSelectorIgnoreLatencyWindow);

ServerSelector::~ServerSelector() {}

SdamServerSelector::SdamServerSelector(const SdamConfiguration& config)
    : _config(config), _random(PseudoRandom(SecureRandom().nextInt64())) {}

void SdamServerSelector::_getCandidateServers(std::vector<ServerDescriptionPtr>* result,
                                              const TopologyDescriptionPtr topologyDescription,
                                              const ReadPreferenceSetting& criteria) {
    // when querying the primary we don't need to consider tags
    bool shouldTagFilter = true;

    // TODO SERVER-46499: check to see if we want to enforce minOpTime at all since
    // it was effectively optional in the original implementation.
    if (!criteria.minOpTime.isNull()) {
        auto eligibleServers = topologyDescription->findServers([](const ServerDescriptionPtr& s) {
            return (s->getType() == ServerType::kRSPrimary ||
                    s->getType() == ServerType::kRSSecondary);
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
            if (maxOpTime && maxOpTime < criteria.minOpTime) {
                // ignore minOpTime
                const_cast<ReadPreferenceSetting&>(criteria) = ReadPreferenceSetting(criteria.pref);
            }
        }
    }

    switch (criteria.pref) {
        case ReadPreference::Nearest:
            *result = topologyDescription->findServers(nearestFilter(criteria));
            break;

        case ReadPreference::SecondaryOnly:
            *result = topologyDescription->findServers(secondaryFilter(criteria));
            break;

        case ReadPreference::PrimaryOnly: {
            const auto primaryCriteria = ReadPreferenceSetting(criteria.pref);
            *result = topologyDescription->findServers(primaryFilter(primaryCriteria));
            shouldTagFilter = false;
            break;
        }

        case ReadPreference::PrimaryPreferred: {
            // ignore tags and max staleness for primary query
            auto primaryCriteria = ReadPreferenceSetting(ReadPreference::PrimaryOnly);
            _getCandidateServers(result, topologyDescription, primaryCriteria);
            if (result->size()) {
                shouldTagFilter = false;
                break;
            }

            // keep tags and maxStaleness for secondary query
            auto secondaryCriteria = criteria;
            secondaryCriteria.pref = ReadPreference::SecondaryOnly;
            _getCandidateServers(result, topologyDescription, secondaryCriteria);
            break;
        }

        case ReadPreference::SecondaryPreferred: {
            // keep tags and maxStaleness for secondary query
            auto secondaryCriteria = criteria;
            secondaryCriteria.pref = ReadPreference::SecondaryOnly;
            _getCandidateServers(result, topologyDescription, secondaryCriteria);
            if (result->size()) {
                break;
            }

            // ignore tags and maxStaleness for primary query
            shouldTagFilter = false;
            auto primaryCriteria = ReadPreferenceSetting(ReadPreference::PrimaryOnly);
            _getCandidateServers(result, topologyDescription, primaryCriteria);
            break;
        }

        default:
            MONGO_UNREACHABLE
    }

    if (shouldTagFilter) {
        filterTags(result, criteria.tags);
    }
}

boost::optional<std::vector<ServerDescriptionPtr>> SdamServerSelector::selectServers(
    const TopologyDescriptionPtr topologyDescription, const ReadPreferenceSetting& criteria) {

    // If the topology wire version is invalid, raise an error
    if (!topologyDescription->isWireVersionCompatible()) {
        uasserted(ErrorCodes::IncompatibleServerVersion,
                  *topologyDescription->getWireVersionCompatibleError());
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
    _getCandidateServers(&results, topologyDescription, criteria);

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
    const TopologyDescriptionPtr topologyDescription, const ReadPreferenceSetting& criteria) {
    auto servers = selectServers(topologyDescription, criteria);
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
    const auto& checkTags = tagSet.getTagBSON();

    if (checkTags.nFields() == 0)
        return;

    const auto predicate = [&](const ServerDescriptionPtr& s) {
        auto it = checkTags.begin();
        while (it != checkTags.end()) {
            if (it->isABSONObj()) {
                const BSONObj& tags = it->Obj();
                if (_containsAllTags(s, tags)) {
                    // found a match -- don't remove the server
                    return false;
                }
            } else {
                LOGV2_WARNING(
                    4671202,
                    "Invalid tags specified for server selection; tags should be specified as "
                    "bson Objects",
                    "tag"_attr = *it);
            }
            ++it;
        }

        // remove the server
        return true;
    };

    servers->erase(std::remove_if(servers->begin(), servers->end(), predicate), servers->end());
}

bool SdamServerSelector::recencyFilter(const ReadPreferenceSetting& readPref,
                                       const ServerDescriptionPtr& s) {
    bool result = true;

    // TODO SERVER-46499: check to see if we want to enforce minOpTime at all since
    // it was effectively optional in the original implementation.
    if (!readPref.minOpTime.isNull()) {
        result = result && (s->getOpTime() >= readPref.minOpTime);
    }

    if (readPref.maxStalenessSeconds.count()) {
        auto topologyDescription = s->getTopologyDescription();
        invariant(topologyDescription);
        auto staleness = _calculateStaleness(*topologyDescription, s);
        result = result && (staleness <= readPref.maxStalenessSeconds);
    }

    return result;
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

bool LatencyWindow::isWithinWindow(IsMasterRTT latency) {
    return lower <= latency && latency <= upper;
}
}  // namespace mongo::sdam
