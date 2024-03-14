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

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/decorable.h"

namespace mongo::query_settings {

namespace {
const auto getQuerySettingsManager =
    ServiceContext::declareDecoration<boost::optional<QuerySettingsManager>>();

class QuerySettingsServerStatusSection final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        // Only include if Query Settings are enabled.
        // We need to use isEnabledUseLatestFCVWhenUninitialized instead of isEnabled because
        // this could run during startup while the FCV is still uninitialized.
        return feature_flags::gFeatureFlagQuerySettings.isEnabledUseLatestFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        stdx::lock_guard<Latch> lk(_mutex);
        return BSON("count" << _count << "size" << _size << "rejectCount"
                            << _numSettingsWithReject);
    }

    void record(int count, int size, int numSettingsWithReject) {
        stdx::lock_guard<Latch> lk(_mutex);
        _count = count;
        _size = size;
        _numSettingsWithReject = numSettingsWithReject;
    }

private:
    int _count = 0;
    int _size = 0;
    int _numSettingsWithReject = 0;
    mutable Mutex _mutex = MONGO_MAKE_LATCH("QuerySettingsServerStatusSection::_mutex");
};

auto& querySettingsServerStatusSection =
    *ServerStatusSectionBuilder<QuerySettingsServerStatusSection>("querySettings");

auto computeTenantConfiguration(std::vector<QueryShapeConfiguration>&& settingsArray,
                                const boost::optional<TenantId>& tenantId) {
    QueryShapeConfigurationsMap queryShapeConfigurationMap;
    for (auto&& queryShapeConfiguration : settingsArray) {
        queryShapeConfigurationMap.insert({queryShapeConfiguration.getQueryShapeHash(),
                                           {queryShapeConfiguration.getSettings(),
                                            queryShapeConfiguration.getRepresentativeQuery()}});
    }
    return stdx::unordered_map<NamespaceString, QueryShapeConfigurationsMap>(
        {{NamespaceString(), queryShapeConfigurationMap}});
}
}  // namespace

QuerySettingsManager& QuerySettingsManager::get(ServiceContext* service) {
    return *getQuerySettingsManager(service);
}

QuerySettingsManager& QuerySettingsManager::get(OperationContext* opCtx) {
    return *getQuerySettingsManager(opCtx->getServiceContext());
}

void QuerySettingsManager::create(
    ServiceContext* service, std::function<void(OperationContext*)> clusterParameterRefreshFn) {
    getQuerySettingsManager(service).emplace(service, clusterParameterRefreshFn);
}

boost::optional<std::pair<QuerySettings, boost::optional<QueryInstance>>>
QuerySettingsManager::getQuerySettingsForQueryShapeHash(
    OperationContext* opCtx,
    const query_shape::QueryShapeHash& queryShapeHash,
    const boost::optional<TenantId>& tenantId) const {
    Lock::SharedLock readLock(opCtx, _mutex);
    // Perform the lookup for namespace string to query settings map maintained for the given
    // tenant.
    auto queryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (queryShapeConfigurationsIt == _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return boost::none;
    }

    // Iterate through all the possible QueryShapeConfiguration maps with the help of the previously
    // resolved namespaces, and try to find the first one containing the query shape hash.
    auto const nssToQueryShapeConfigurationsMap =
        queryShapeConfigurationsIt->second.nssToQueryShapeConfigurationsMap;
    for (auto& nssToQueryShapeConfigurationsMapIt : nssToQueryShapeConfigurationsMap) {
        // Finally, early exit with the resolved query shape configuration if one is found, or
        // move on to the next ones otherwise.
        auto queryShapeConfigurationsMap = nssToQueryShapeConfigurationsMapIt.second;
        auto queryShapeConfigurationsIt = queryShapeConfigurationsMap.find(queryShapeHash);
        if (queryShapeConfigurationsIt != queryShapeConfigurationsMap.end()) {
            return queryShapeConfigurationsIt->second;
        }
    }

    // The given query shape hash didn't point to any known query shape configuration.
    return boost::none;
}

void QuerySettingsManager::setQueryShapeConfigurations(
    OperationContext* opCtx,
    std::vector<QueryShapeConfiguration>&& settingsArray,
    LogicalTime parameterClusterTime,
    const boost::optional<TenantId>& tenantId) {
    auto nssToQueryShapeConfigurationsMap =
        computeTenantConfiguration(std::move(settingsArray), tenantId);

    Lock::ExclusiveLock writeLock(opCtx, _mutex);
    _tenantIdToVersionedQueryShapeConfigurationsMap.insert_or_assign(
        tenantId,
        VersionedQueryShapeConfigurations{std::move(nssToQueryShapeConfigurationsMap),
                                          parameterClusterTime});
}

std::vector<QueryShapeConfiguration> QuerySettingsManager::getAllQueryShapeConfigurations(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const {
    Lock::SharedLock readLock(opCtx, _mutex);
    return getAllQueryShapeConfigurations_inlock(opCtx, tenantId);
}

std::vector<QueryShapeConfiguration> QuerySettingsManager::getAllQueryShapeConfigurations_inlock(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const {
    auto versionedQueryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (versionedQueryShapeConfigurationsIt ==
        _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return {};
    }

    std::vector<QueryShapeConfiguration> configurations;
    for (const auto& it :
         versionedQueryShapeConfigurationsIt->second.nssToQueryShapeConfigurationsMap) {
        for (const auto& [queryShapeHash, value] : it.second) {
            auto& newConfiguration = configurations.emplace_back(queryShapeHash, value.first);
            newConfiguration.setRepresentativeQuery(value.second);
        }
    }
    return configurations;
}

void QuerySettingsManager::refreshQueryShapeConfigurations(OperationContext* opCtx) {
    if (_clusterParameterRefreshFn)
        _clusterParameterRefreshFn(opCtx);
}

void QuerySettingsManager::removeAllQueryShapeConfigurations(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) {
    Lock::ExclusiveLock writeLock(opCtx, _mutex);
    _tenantIdToVersionedQueryShapeConfigurationsMap.erase(tenantId);
}

LogicalTime QuerySettingsManager::getClusterParameterTime(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const {
    Lock::SharedLock readLock(opCtx, _mutex);
    return getClusterParameterTime_inlock(opCtx, tenantId);
}

LogicalTime QuerySettingsManager::getClusterParameterTime_inlock(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId) const {
    auto versionedQueryShapeConfigurationsIt =
        _tenantIdToVersionedQueryShapeConfigurationsMap.find(tenantId);
    if (versionedQueryShapeConfigurationsIt ==
        _tenantIdToVersionedQueryShapeConfigurationsMap.end()) {
        return LogicalTime::kUninitialized;
    }
    return versionedQueryShapeConfigurationsIt->second.clusterParameterTime;
}

void QuerySettingsManager::appendQuerySettingsClusterParameterValue(
    OperationContext* opCtx, BSONObjBuilder* bob, const boost::optional<TenantId>& tenantId) {
    Lock::SharedLock readLock(opCtx, _mutex);
    bob->append("_id"_sd, QuerySettingsManager::kQuerySettingsClusterParameterName);
    BSONArrayBuilder arrayBuilder(
        bob->subarrayStart(QuerySettingsClusterParameterValue::kSettingsArrayFieldName));
    for (auto&& item : getAllQueryShapeConfigurations_inlock(opCtx, tenantId)) {
        arrayBuilder.append(item.toBSON());
    }
    arrayBuilder.done();
    bob->append(QuerySettingsClusterParameterValue::kClusterParameterTimeFieldName,
                getClusterParameterTime_inlock(opCtx, tenantId).asTimestamp());
}

void QuerySettingsClusterParameter::append(OperationContext* opCtx,
                                           BSONObjBuilder* bob,
                                           StringData name,
                                           const boost::optional<TenantId>& tenantId) {
    auto& querySettingsManager = QuerySettingsManager::get(getGlobalServiceContext());
    querySettingsManager.appendQuerySettingsClusterParameterValue(opCtx, bob, tenantId);
}

Status QuerySettingsClusterParameter::set(const BSONElement& newValueElement,
                                          const boost::optional<TenantId>& tenantId) {
    auto& querySettingsManager = QuerySettingsManager::get(getGlobalServiceContext());
    auto newSettings = QuerySettingsClusterParameterValue::parse(
        IDLParserContext("querySettingsParameterValue",
                         false /* apiStrict */,
                         boost::none /* vts */,
                         tenantId,
                         SerializationContext::stateDefault()),
        newValueElement.Obj());
    size_t rejectCount = 0;
    for (const auto& config : newSettings.getSettingsArray()) {
        if (config.getSettings().getReject()) {
            ++rejectCount;
        }
    }
    querySettingsServerStatusSection.record(
        /* count */ static_cast<int>(newSettings.getSettingsArray().size()),
        /* size */ static_cast<int>(newValueElement.valuesize()),
        /* numSettingsWithReject */ static_cast<int>(rejectCount));
    querySettingsManager.setQueryShapeConfigurations(Client::getCurrent()->getOperationContext(),
                                                     std::move(newSettings.getSettingsArray()),
                                                     newSettings.getClusterParameterTime(),
                                                     tenantId);
    return Status::OK();
}

Status QuerySettingsClusterParameter::reset(const boost::optional<TenantId>& tenantId) {
    auto& querySettingsManager = QuerySettingsManager::get(getGlobalServiceContext());
    querySettingsManager.removeAllQueryShapeConfigurations(
        Client::getCurrent()->getOperationContext(), tenantId);
    return Status::OK();
}

LogicalTime QuerySettingsClusterParameter::getClusterParameterTime(
    const boost::optional<TenantId>& tenantId) const {
    auto& querySettingsManager = QuerySettingsManager::get(getGlobalServiceContext());
    return querySettingsManager.getClusterParameterTime(Client::getCurrent()->getOperationContext(),
                                                        tenantId);
}

};  // namespace mongo::query_settings
