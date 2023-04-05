/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/query_analysis_coordinator.h"

#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(queryAnalysisCoordinatorDistributeSampleRateEqually);

const auto getQueryAnalysisCoordinator =
    ServiceContext::declareDecoration<QueryAnalysisCoordinator>();

const ReplicaSetAwareServiceRegistry::Registerer<QueryAnalysisCoordinator>
    queryAnalysisCoordinatorServiceServiceRegisterer("QueryAnalysisCoordinator");

}  // namespace


QueryAnalysisCoordinator* QueryAnalysisCoordinator::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisCoordinator* QueryAnalysisCoordinator::get(ServiceContext* serviceContext) {
    return &getQueryAnalysisCoordinator(serviceContext);
}

bool QueryAnalysisCoordinator::shouldRegisterReplicaSetAwareService() const {
    // This is invoked when the Register above is constructed which is before FCV is set so we need
    // to ignore FCV when checking if the feature flag is enabled.
    return supportsCoordinatingQueryAnalysis(true /* isReplEnabled */, true /* ignoreFCV */);
}

void QueryAnalysisCoordinator::onConfigurationInsert(const QueryAnalyzerDocument& doc) {
    stdx::lock_guard<Latch> lk(_mutex);

    LOGV2(7372308, "Detected new query analyzer configuration", "configuration"_attr = doc);

    if (doc.getMode() == QueryAnalyzerModeEnum::kOff) {
        // Do not create an entry for it if the mode is "off".
        return;
    }

    auto configuration = CollectionQueryAnalyzerConfiguration{
        doc.getNs(), doc.getCollectionUuid(), *doc.getSampleRate(), doc.getStartTime()};

    _configurations.emplace(doc.getCollectionUuid(), std::move(configuration));
}

void QueryAnalysisCoordinator::onConfigurationUpdate(const QueryAnalyzerDocument& doc) {
    stdx::lock_guard<Latch> lk(_mutex);

    LOGV2(7372309, "Detected a query analyzer configuration update", "configuration"_attr = doc);

    if (doc.getMode() == QueryAnalyzerModeEnum::kOff) {
        // Remove the entry for it if the mode has been set to "off".
        _configurations.erase(doc.getCollectionUuid());
    } else {
        auto it = _configurations.find(doc.getCollectionUuid());
        if (it == _configurations.end()) {
            auto configuration = CollectionQueryAnalyzerConfiguration{
                doc.getNs(), doc.getCollectionUuid(), *doc.getSampleRate(), doc.getStartTime()};
            _configurations.emplace(doc.getCollectionUuid(), std::move(configuration));
        } else {
            it->second.setSampleRate(*doc.getSampleRate());
        }
    }
}

void QueryAnalysisCoordinator::onConfigurationDelete(const QueryAnalyzerDocument& doc) {
    stdx::lock_guard<Latch> lk(_mutex);

    LOGV2(7372310, "Detected a query analyzer configuration delete", "configuration"_attr = doc);

    _configurations.erase(doc.getCollectionUuid());
}

Date_t QueryAnalysisCoordinator::_getMinLastPingTime() {
    auto serviceContext = getQueryAnalysisCoordinator.owner(this);
    return serviceContext->getFastClockSource()->now() -
        Seconds(gQueryAnalysisSamplerInActiveThresholdSecs.load());
}

void QueryAnalysisCoordinator::Sampler::setLastPingTime(Date_t pingTime) {
    _lastPingTime = pingTime;
}

void QueryAnalysisCoordinator::Sampler::setLastNumQueriesExecutedPerSecond(double numQueries) {
    _lastNumQueriesExecutedPerSecond = numQueries;
}

void QueryAnalysisCoordinator::Sampler::resetLastNumQueriesExecutedPerSecond() {
    _lastNumQueriesExecutedPerSecond = boost::none;
}

void QueryAnalysisCoordinator::onSamplerInsert(const MongosType& doc) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
    stdx::lock_guard<Latch> lk(_mutex);

    if (doc.getPing() < _getMinLastPingTime()) {
        return;
    }
    auto sampler = Sampler{doc.getName(), doc.getPing()};
    _samplers.emplace(doc.getName(), std::move(sampler));
}

void QueryAnalysisCoordinator::onSamplerUpdate(const MongosType& doc) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
    stdx::lock_guard<Latch> lk(_mutex);

    auto it = _samplers.find(doc.getName());
    if (it == _samplers.end()) {
        auto sampler = Sampler{doc.getName(), doc.getPing()};
        _samplers.emplace(doc.getName(), std::move(sampler));
    } else {
        it->second.setLastPingTime(doc.getPing());
    }
}

void QueryAnalysisCoordinator::onSamplerDelete(const MongosType& doc) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
    stdx::lock_guard<Latch> lk(_mutex);

    auto erased = _samplers.erase(doc.getName());
    invariant(erased);
}

void QueryAnalysisCoordinator::onStartup(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_mutex);

    DBDirectClient client(opCtx);

    {
        invariant(_configurations.empty());
        FindCommandRequest findRequest{NamespaceString::kConfigQueryAnalyzersNamespace};
        findRequest.setFilter(BSON(QueryAnalyzerDocument::kModeFieldName << BSON("$ne"
                                                                                 << "off")));
        auto cursor = client.find(std::move(findRequest));
        while (cursor->more()) {
            auto doc = QueryAnalyzerDocument::parse(IDLParserContext("QueryAnalysisCoordinator"),
                                                    cursor->next());
            invariant(doc.getMode() != QueryAnalyzerModeEnum::kOff);
            auto configuration = CollectionQueryAnalyzerConfiguration{
                doc.getNs(), doc.getCollectionUuid(), *doc.getSampleRate(), doc.getStartTime()};
            auto [_, inserted] =
                _configurations.emplace(doc.getCollectionUuid(), std::move(configuration));
            invariant(inserted);
        }
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        invariant(_samplers.empty());

        auto minPingTime = _getMinLastPingTime();
        FindCommandRequest findRequest{MongosType::ConfigNS};
        findRequest.setFilter(BSON(MongosType::ping << BSON("$gte" << minPingTime)));
        auto cursor = client.find(std::move(findRequest));
        while (cursor->more()) {
            auto doc = uassertStatusOK(MongosType::fromBSON(cursor->next()));
            invariant(doc.getPing() >= minPingTime);
            auto sampler = Sampler{doc.getName(), doc.getPing()};
            _samplers.emplace(doc.getName(), std::move(sampler));
        }
    }
}

void QueryAnalysisCoordinator::onSetCurrentConfig(OperationContext* opCtx) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        stdx::lock_guard<Latch> lk(_mutex);

        StringMap<Sampler> samplers;

        auto replMembers = repl::ReplicationCoordinator::get(opCtx)->getConfig().members();
        for (const auto& member : replMembers) {
            if (member.isArbiter()) {
                continue;
            }

            auto samplerName = member.getHostAndPort().toString();
            auto now = opCtx->getServiceContext()->getFastClockSource()->now();
            auto it = _samplers.find(samplerName);
            if (it == _samplers.end()) {
                // Initialize a sampler for every new data-bearing replica set member.
                samplers.emplace(samplerName, Sampler{samplerName, now});
            } else {
                auto sampler = it->second;
                sampler.setLastPingTime(now);
                samplers.emplace(samplerName, std::move(sampler));
            }
        }

        _samplers = std::move(samplers);
    }
}

void QueryAnalysisCoordinator::onStepUpBegin(OperationContext* opCtx, long long term) {
    stdx::lock_guard<Latch> lk(_mutex);
    for (auto& [_, sampler] : _samplers) {
        sampler.resetLastNumQueriesExecutedPerSecond();
    }
}

std::vector<CollectionQueryAnalyzerConfiguration>
QueryAnalysisCoordinator::getNewConfigurationsForSampler(OperationContext* opCtx,
                                                         StringData samplerName,
                                                         double numQueriesExecutedPerSecond) {
    stdx::lock_guard<Latch> lk(_mutex);

    // Update the last ping time and last number of queries executed per second of this sampler.
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto it = _samplers.find(samplerName);
    if (it == _samplers.end()) {
        auto sampler = Sampler{samplerName.toString(), now};
        it = _samplers.emplace(samplerName, std::move(sampler)).first;
    } else {
        it->second.setLastPingTime(now);
    }
    it->second.setLastNumQueriesExecutedPerSecond(numQueriesExecutedPerSecond);

    // Calculate the sample rate ratio for this sampler.
    int numActiveSamplers = 0;
    int numWeights = 0;
    double weight = numQueriesExecutedPerSecond;
    double totalWeight = 0;

    auto minPingTime = _getMinLastPingTime();
    for (const auto& [name, sampler] : _samplers) {
        if (sampler.getLastPingTime() > minPingTime) {
            numActiveSamplers++;
            if (auto weight = sampler.getLastNumQueriesExecutedPerSecond()) {
                totalWeight += *weight;
                numWeights++;
            }
        }
    }
    invariant(numActiveSamplers > 0);
    // If the coordinator doesn't yet have a full view of the query distribution or no samplers
    // have executed any queries, each sampler gets an equal ratio of the sample rates. Otherwise,
    // the ratio is weighted based on the query distribution across samplers.
    double sampleRateRatio =
        (numWeights < numActiveSamplers || totalWeight == 0 ||
         MONGO_unlikely(queryAnalysisCoordinatorDistributeSampleRateEqually.shouldFail()))
        ? (1.0 / numActiveSamplers)
        : (weight / totalWeight);

    // Populate the query analyzer configurations for all collections.
    std::vector<CollectionQueryAnalyzerConfiguration> configurations;
    for (const auto& [_, configuration] : _configurations) {
        configurations.emplace_back(configuration.getNs(),
                                    configuration.getCollectionUuid(),
                                    sampleRateRatio * configuration.getSampleRate(),
                                    configuration.getStartTime());
    }
    return configurations;
}

}  // namespace analyze_shard_key
}  // namespace mongo
