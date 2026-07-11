// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"

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

    LogicalTime getClusterParameterTime(WithLock, const boost::optional<TenantId>& tenantId) const;

    mutable WriteRarelyRWMutex _mutex;
    absl::flat_hash_map<boost::optional<TenantId>, VersionedQueryShapeConfigurations>
        _tenantIdToVersionedQueryShapeConfigurationsMap;
};
}  // namespace mongo::query_settings
