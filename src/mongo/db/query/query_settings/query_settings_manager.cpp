/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_settings/query_settings_manager.h"

#include "mongo/db/logical_time.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"

#include <algorithm>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
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
                 .representativeQuery_deprecated = queryShapeConfiguration.getRepresentativeQuery(),
                 // Initially assume that no representative query is present. If one is present in
                 // "config.queryShapeRepresentativeQueries", the next backfill attempt will update
                 // this flag to reflect the correct state.
                 // TODO SERVER-105065 Populate this flag with the correct state.
                 .hasRepresentativeQuery = false,
             }});
    }
    return queryShapeConfigurationMap;
}
}  // namespace

boost::optional<QuerySettingsLookupResult> QuerySettingsManager::getQuerySettingsForQueryShapeHash(
    const query_shape::QueryShapeHash& queryShapeHash,
    const boost::optional<TenantId>& tenantId) const {
    auto readLock = _mutex.readLock();

    // Perform the lookup of query shape configurations for the given tenant.
    const auto versionedQueryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (versionedQueryShapeConfigurationsIt ==
        _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return boost::none;
    }
    const auto& [queryShapeHashToQueryShapeConfigurationsMap, clusterParameterTime] =
        versionedQueryShapeConfigurationsIt->second;

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
    QueryShapeConfigurationsWithTimestamp&& config, const boost::optional<TenantId>& tenantId) {
    // Build new query shape configurations.
    VersionedQueryShapeConfigurations newQueryShapeConfigurations{
        computeTenantConfiguration(std::move(config.queryShapeConfigurations)),
        config.clusterParameterTime};

    // Install the query shape configurations.
    {
        auto writeLock = _mutex.writeLock();
        const auto versionedQueryShapeConfigurationsIt =
            _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
        if (_tenantIdToVersionedQueryShapeConfigurationsMap.end() !=
            versionedQueryShapeConfigurationsIt) {
            // Swap the configurations to minimize the time the lock is held in exclusive mode by
            // deferring the destruction of the previous version of the query shape configurations
            // to the time when the lock is not held.
            std::swap(versionedQueryShapeConfigurationsIt->second, newQueryShapeConfigurations);
        } else {
            _tenantIdToVersionedQueryShapeConfigurationsMap.emplace(
                tenantId, std::move(newQueryShapeConfigurations));
        }
    }
}

void QuerySettingsManager::removeAllQueryShapeConfigurations(
    const boost::optional<TenantId>& tenantId) {
    // Previous query shape configurations for destruction outside the critical section.
    VersionedQueryShapeConfigurations previousQueryShapeConfigurations;
    {
        auto writeLock = _mutex.writeLock();
        const auto versionedQueryShapeConfigurationsIt =
            _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
        if (_tenantIdToVersionedQueryShapeConfigurationsMap.end() !=
            versionedQueryShapeConfigurationsIt) {
            // Swap the configurations to minimize the time the lock is held in exclusive mode by
            // deferring the destruction of the previous version of the query shape configurations
            // to the time when the lock is not held.
            std::swap(versionedQueryShapeConfigurationsIt->second,
                      previousQueryShapeConfigurations);
            _tenantIdToVersionedQueryShapeConfigurationsMap.erase(
                versionedQueryShapeConfigurationsIt);
        }
    }
}

QueryShapeConfigurationsWithTimestamp QuerySettingsManager::getAllQueryShapeConfigurations(
    const boost::optional<TenantId>& tenantId) const {
    auto readLock = _mutex.readLock();
    return {getAllQueryShapeConfigurations_inlock(tenantId),
            getClusterParameterTime_inlock(tenantId)};
}

std::vector<QueryShapeConfiguration> QuerySettingsManager::getAllQueryShapeConfigurations_inlock(
    const boost::optional<TenantId>& tenantId) const {
    auto versionedQueryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (versionedQueryShapeConfigurationsIt ==
        _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return {};
    }

    const auto& queryShapeHashToQueryShapeConfigurationsMap =
        versionedQueryShapeConfigurationsIt->second.queryShapeHashToQueryShapeConfigurationsMap;
    std::vector<QueryShapeConfiguration> configurations;
    configurations.reserve(queryShapeHashToQueryShapeConfigurationsMap.size());
    for (const auto& [queryShapeHash, queryShapeConfiguration] :
         queryShapeHashToQueryShapeConfigurationsMap) {
        auto& newConfiguration =
            configurations.emplace_back(queryShapeHash, queryShapeConfiguration.querySettings);
        newConfiguration.setRepresentativeQuery(
            queryShapeConfiguration.representativeQuery_deprecated);
    }
    return configurations;
}

LogicalTime QuerySettingsManager::getClusterParameterTime(
    const boost::optional<TenantId>& tenantId) const {
    auto readLock = _mutex.readLock();
    return getClusterParameterTime_inlock(tenantId);
}

LogicalTime QuerySettingsManager::getClusterParameterTime_inlock(
    const boost::optional<TenantId>& tenantId) const {
    auto versionedQueryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (versionedQueryShapeConfigurationsIt ==
        _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return LogicalTime::kUninitialized;
    }
    return versionedQueryShapeConfigurationsIt->second.clusterParameterTime;
}
};  // namespace mongo::query_settings
