// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
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

    bool hasRepresentativeQuery;
};

using QueryShapeConfigurationsMap = absl::
    flat_hash_map<query_shape::QueryShapeHash, QueryShapeConfigCachedEntry, QueryShapeHashHasher>;

/**
 * Stores all query shape configurations, containing the same information as the
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
 * Class responsible for managing in-memory storage and fetching of query settings.
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
     * Returns QuerySettings associated with a query which query shape hash is 'queryShapeHash'.
     */
    boost::optional<QuerySettingsLookupResult> getQuerySettingsForQueryShapeHash(
        const query_shape::QueryShapeHash& queryShape) const;

    /**
     * Returns all query shape configurations and an associated timestamp.
     */
    QueryShapeConfigurationsWithTimestamp getAllQueryShapeConfigurations() const;

    /**
     * Sets the QueryShapeConfigurations by replacing an existing VersionedQueryShapeConfigurations
     * with the newly built one.
     */
    void setAllQueryShapeConfigurations(QueryShapeConfigurationsWithTimestamp&& config);

    /**
     * Removes all query settings documents.
     */
    void removeAllQueryShapeConfigurations();

    /**
     * Marks the query shape configurations associated with the given 'backfilledHashes' as having a
     * representative query. Fails with 'ConflictingOperationInProgress' if the provided
     * 'clusterParameterTime' argument diverged from the current manager one.
     */
    void markBackfilledRepresentativeQueries(
        const std::vector<query_shape::QueryShapeHash>& backfilledHashes,
        const LogicalTime& clusterParameterTime);

    /**
     * Returns the cluster parameter time of the current QuerySettingsClusterParameter value.
     */
    LogicalTime getClusterParameterTime() const;

private:
    VersionedQueryShapeConfigurations getVersionedQueryShapeConfigurations() const;

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
        VersionedQueryShapeConfigurations&& newQueryShapeConfigurations);

    LogicalTime getClusterParameterTime(WithLock) const;

    mutable WriteRarelyRWMutex _mutex;
    VersionedQueryShapeConfigurations _versionedQueryShapeConfigurations;
};
}  // namespace mongo::query_settings
