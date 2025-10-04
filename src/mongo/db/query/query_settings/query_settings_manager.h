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


#pragma once

#include "mongo/base/data_view.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/rwmutex.h"

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo::query_settings {

using QueryInstance = BSONObj;

/**
 * The in-memory representation for the data stored in QueryShapeConfiguration.
 */
struct QueryShapeConfigCachedEntry {
    QuerySettings querySettings;

    // TODO SERVER-105064 Remove this property once 9.0 is last-lts.
    boost::optional<QueryInstance> representativeQuery_deprecated;
    bool hasRepresentativeQuery;
};

using QueryShapeConfigurationsMap = absl::
    flat_hash_map<query_shape::QueryShapeHash, QueryShapeConfigCachedEntry, QueryShapeHashHasher>;

/**
 * Stores all query shape configurations for a tenant, containing the same information as the
 * QuerySettingsClusterParameterValue. The data present in the 'settingsArray' is stored in the
 * QueryShapeConfigurationsMap for faster access.
 */
struct VersionedQueryShapeConfigurations {
    /**
     * 'QueryShapeHash' -> 'QueryShapeConfiguration' mapping.
     */
    QueryShapeConfigurationsMap queryShapeHashToQueryShapeConfigurationsMap;

    /**
     * Cluster time of the current version of the QuerySettingsClusterParameter.
     */
    LogicalTime clusterParameterTime;
};

/**
 * Result structure for 'QuerySettingsManager::getQuerySettingsForQueryShapeHash()'.
 */
struct QuerySettingsLookupResult {
    /**
     * The query settings associated with the given query shape hash.
     */
    QuerySettings querySettings;

    /**
     * Whether the given query shape hash has an associated representative query.
     */
    bool hasRepresentativeQuery;

    /**
     * Cluster time representing the current version of the QuerySettingsClusterParameter.
     */
    LogicalTime clusterParameterTime;
};

/**
 * Class responsible for managing in-memory storage and fetching of query settings. Query settings
 * in-memory storage is maintained separately for each tenant. In dedicated environments the
 * 'tenantId' argument passed to the methods must be boost::none.
 */
class QuerySettingsManager {
public:
    QuerySettingsManager() = default;
    ~QuerySettingsManager() = default;

    QuerySettingsManager(QuerySettingsManager&&) = delete;
    QuerySettingsManager(const QuerySettingsManager&) = delete;
    QuerySettingsManager& operator=(QuerySettingsManager&&) = delete;
    QuerySettingsManager& operator=(const QuerySettingsManager&) = delete;

    /**
     * Returns QuerySettings associated with a query which query shape hash is 'queryShapeHash' for
     * the given tenant.
     */
    boost::optional<QuerySettingsLookupResult> getQuerySettingsForQueryShapeHash(
        const query_shape::QueryShapeHash& queryShapeHash,
        const boost::optional<TenantId>& tenantId) const;

    /**
     * Returns all query shape configurations and an associated timestamp for the given tenant
     * 'tenantId'.
     */
    QueryShapeConfigurationsWithTimestamp getAllQueryShapeConfigurations(
        const boost::optional<TenantId>& tenantId) const;

    /**
     * Sets the QueryShapeConfigurations by replacing an existing VersionedQueryShapeConfigurations
     * with the newly built one.
     */
    void setAllQueryShapeConfigurations(QueryShapeConfigurationsWithTimestamp&& config,
                                        const boost::optional<TenantId>& tenantId);

    /**
     * Removes all query settings documents for the given tenant.
     */
    void removeAllQueryShapeConfigurations(const boost::optional<TenantId>& tenantId);

    /**
     * Marks the query shape configurations associated with the given 'backfilledHashes' as having a
     * representative query. Fails with 'ConflictingOperationInProgress' if the provided
     * 'clusterParameterTime' argument diverged from the current manager one.
     */
    void markBackfilledRepresentativeQueries(
        const std::vector<query_shape::QueryShapeHash>& backfilledHashes,
        const LogicalTime& clusterParameterTime,
        const boost::optional<TenantId>& tenantId);

    /**
     * Returns the cluster parameter time of the current QuerySettingsClusterParameter value for the
     * given tenant.
     */
    LogicalTime getClusterParameterTime(const boost::optional<TenantId>& tenantId) const;

private:
    VersionedQueryShapeConfigurations getVersionedQueryShapeConfigurations(
        const boost::optional<TenantId>& tenantId) const;

    /**
     * Installs the new versioned query shape configurations.
     *
     * Additionally checks for 'clusterParameterTime' divergences if
     * 'enforceClusterParameterTimeMatch' is true. Throws 'ConflictingOperationInProgress' if the
     * new 'clusterParameterTime' contained in 'newQueryShapeConfigurations' is not equal to the
     * current one.
     */
    template <bool enforceClusterParameterTimeMatch>
    void setVersionedQueryShapeConfigurations(
        VersionedQueryShapeConfigurations&& newQueryShapeConfigurations,
        const boost::optional<TenantId>& tenantId);

    LogicalTime getClusterParameterTime_inlock(const boost::optional<TenantId>& tenantId) const;

    mutable WriteRarelyRWMutex _mutex;
    absl::flat_hash_map<boost::optional<TenantId>, VersionedQueryShapeConfigurations>
        _tenantIdToVersionedQueryShapeConfigurationsMap;
};
}  // namespace mongo::query_settings
