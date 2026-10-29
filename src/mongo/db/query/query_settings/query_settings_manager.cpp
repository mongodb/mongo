// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_manager.h"

#include "mongo/db/logical_time.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_settings/query_settings_usage_tracker.h"

#include <algorithm>

#include <boost/optional/optional.hpp>

namespace mongo::query_settings {

namespace {
auto computeTenantConfiguration(std::vector<QueryShapeConfiguration>&& settingsArray) {
    QueryShapeConfigurationsMap queryShapeConfigurationMap;
    queryShapeConfigurationMap.reserve(settingsArray.size());
    for (auto&& queryShapeConfiguration : settingsArray) {
        queryShapeConfigurationMap.insert(
            {queryShapeConfiguration.getQueryShapeHash(),
             QueryShapeConfigCachedEntry{
                 .querySettings = queryShapeConfiguration.getSettings(),
                 // Initially assume that no representative query is present. If one is present in
                 // "config.queryShapeRepresentativeQueries", the next backfill attempt will update
                 // this flag to reflect the correct state.
                 // TODO SERVER-105065 Populate this flag with the correct state.
                 .hasRepresentativeQuery = false,
             }});
    }
    return queryShapeConfigurationMap;
}

int countMissingRepresentativeQueries(const QueryShapeConfigurationsMap& config) {
    return std::count_if(config.begin(), config.end(), [](auto&& elem) {
        return !elem.second.hasRepresentativeQuery;
    });
}
}  // namespace

boost::optional<QuerySettingsLookupResult> QuerySettingsManager::getQuerySettingsForQueryShapeHash(
    const query_shape::QueryShapeHash& queryShapeHash) const {
    auto readLock = _mutex.readLock();

    const auto& [queryShapeHashToQueryShapeConfigurationsMap, clusterParameterTime] =
        _versionedQueryShapeConfigurations;

    // Lookup the query shape configuration by the query shape hash.
    const auto queryShapeConfigurationsIt =
        queryShapeHashToQueryShapeConfigurationsMap.find(queryShapeHash);
    if (queryShapeHashToQueryShapeConfigurationsMap.end() == queryShapeConfigurationsIt) {
        return boost::none;
    }

    const auto& queryShapeConfiguration = queryShapeConfigurationsIt->second;
    return QuerySettingsLookupResult{.querySettings = queryShapeConfiguration.querySettings,
                                     .hasRepresentativeQuery =
                                         queryShapeConfiguration.hasRepresentativeQuery,
                                     .clusterParameterTime = clusterParameterTime};
}

void QuerySettingsManager::setAllQueryShapeConfigurations(
    QueryShapeConfigurationsWithTimestamp&& config) {
    // Set the new versioned query shape configurations. Do not enforce the strict time match
    // as the new 'clusterParameterTime' might've been incremented in the meantime.
    setVersionedQueryShapeConfigurations</* enforceClusterParameterTimeMatch  */ false>(
        VersionedQueryShapeConfigurations{
            computeTenantConfiguration(std::move(config.queryShapeConfigurations)),
            config.clusterParameterTime});
}

template <bool enforceClusterParameterTimeMatch>
void QuerySettingsManager::setVersionedQueryShapeConfigurations(
    VersionedQueryShapeConfigurations&& newQueryShapeConfigurations) {
    auto&& tracker = QuerySettingsUsageTracker::get(getGlobalServiceContext());
    const auto missingRepresentativeQueries = countMissingRepresentativeQueries(
        newQueryShapeConfigurations.queryShapeHashToQueryShapeConfigurationsMap);
    auto writeLock = _mutex.writeLock();

    // TODO SERVER-106885 Ensure that 'clusterParameterTime' is monotonous.
    auto&& currQueryShapeConfigurations = _versionedQueryShapeConfigurations;
    if constexpr (enforceClusterParameterTimeMatch) {
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "detected concurent operation in progress while marking backfilled "
                "representative queries",
                currQueryShapeConfigurations.clusterParameterTime ==
                    newQueryShapeConfigurations.clusterParameterTime);
    }

    // Swap the configurations to minimize the time the lock is held in exclusive mode by
    // deferring the destruction of the previous version of the query shape configurations
    // to the time when the lock is not held.
    std::swap(currQueryShapeConfigurations, newQueryShapeConfigurations);
    tracker.setMissingRepresentativeQueries(missingRepresentativeQueries);
}

void QuerySettingsManager::removeAllQueryShapeConfigurations() {
    // Previous query shape configurations for destruction outside the critical section.
    VersionedQueryShapeConfigurations previousQueryShapeConfigurations;
    {
        auto writeLock = _mutex.writeLock();
        // Swap the configurations to minimize the time the lock is held in exclusive mode by
        // deferring the destruction of the previous version of the query shape configurations
        // to the time when the lock is not held.
        std::swap(_versionedQueryShapeConfigurations, previousQueryShapeConfigurations);
    }
}

void QuerySettingsManager::markBackfilledRepresentativeQueries(
    const std::vector<query_shape::QueryShapeHash>& backfilledHashes,
    const LogicalTime& clusterParameterTime) {
    if (backfilledHashes.empty()) {
        // Nothing to do, just return early to avoid acquiring the locks.
        return;
    }
    auto versionedQueryShapeConfigurations = getVersionedQueryShapeConfigurations();
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "detected concurent operation in progress while marking backfilled "
            "representative queries",
            clusterParameterTime == versionedQueryShapeConfigurations.clusterParameterTime);
    for (auto&& hash : backfilledHashes) {
        auto&& it =
            versionedQueryShapeConfigurations.queryShapeHashToQueryShapeConfigurationsMap.find(
                hash);
        tassert(10566101,
                str::stream() << "missing query shape configuration for " << hash.toHexString(),
                it !=
                    versionedQueryShapeConfigurations.queryShapeHashToQueryShapeConfigurationsMap
                        .end());
        it->second.hasRepresentativeQuery = true;
    }
    // Set the new query shape configuration. Ensure that the new 'clusterParameterTime' matches
    // its previous value to protect against concurent operations in progress.
    setVersionedQueryShapeConfigurations</* enforceClusterParameterTimeMatch */ true>(
        std::move(versionedQueryShapeConfigurations));
}

QueryShapeConfigurationsWithTimestamp QuerySettingsManager::getAllQueryShapeConfigurations() const {
    auto [queryShapeHashToQueryShapeConfigurationsMap, clusterParameterTime] =
        getVersionedQueryShapeConfigurations();

    std::vector<QueryShapeConfiguration> configurations;
    configurations.reserve(queryShapeHashToQueryShapeConfigurationsMap.size());
    for (const auto& [queryShapeHash, queryShapeConfiguration] :
         queryShapeHashToQueryShapeConfigurationsMap) {
        configurations.emplace_back(queryShapeHash, queryShapeConfiguration.querySettings);
    }
    return QueryShapeConfigurationsWithTimestamp{std::move(configurations), clusterParameterTime};
}

VersionedQueryShapeConfigurations QuerySettingsManager::getVersionedQueryShapeConfigurations()
    const {
    auto readLock = _mutex.readLock();

    const auto& queryShapeHashToQueryShapeConfigurationsMap =
        _versionedQueryShapeConfigurations.queryShapeHashToQueryShapeConfigurationsMap;
    return VersionedQueryShapeConfigurations{
        .queryShapeHashToQueryShapeConfigurationsMap = queryShapeHashToQueryShapeConfigurationsMap,
        .clusterParameterTime = getClusterParameterTime(readLock)};
}

LogicalTime QuerySettingsManager::getClusterParameterTime() const {
    auto readLock = _mutex.readLock();
    return getClusterParameterTime(readLock);
}

LogicalTime QuerySettingsManager::getClusterParameterTime(WithLock) const {
    return _versionedQueryShapeConfigurations.clusterParameterTime;
}
};  // namespace mongo::query_settings
