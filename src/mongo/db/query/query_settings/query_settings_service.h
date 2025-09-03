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

#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service_dependencies.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/stdx/trusted_hasher.h"

namespace mongo {
/**
 * Truncates the 256 bit QueryShapeHash by taking only the first sizeof(size_t) bytes.
 */
class QueryShapeHashHasher {
public:
    size_t operator()(const query_shape::QueryShapeHash& hash) const {
        return ConstDataView(reinterpret_cast<const char*>(hash.data())).read<size_t>();
    }
};

template <>
struct IsTrustedHasher<QueryShapeHashHasher, query_shape::QueryShapeHash> : std::true_type {};

namespace query_settings {
using QueryInstance = BSONObj;

/**
 * All query shape configurations and an associated timestamp.
 */
struct QueryShapeConfigurationsWithTimestamp {
    std::vector<QueryShapeConfiguration> queryShapeConfigurations;

    /**
     * Cluster time of the current version of the QuerySettingsClusterParameter.
     */
    LogicalTime clusterParameterTime;
};

struct RepresentativeQueryInfo {
    const BSONObj serializedQueryShape;
    const query_shape::QueryShapeHash queryShapeHash;
    const NamespaceString namespaceString;
    const stdx::unordered_set<NamespaceString> involvedNamespaces;
    const boost::optional<EncryptionInformation> encryptionInformation;
    const bool isIdHackQuery;

    // Name of the leading stage if it is "system"/"administrative" and is not eligible for
    // rejection by query settings.
    const boost::optional<std::string> systemStage;
};

/**
 * Creates a RepresentativeQueryInfo for the given query.
 */
RepresentativeQueryInfo createRepresentativeInfo(OperationContext* opCtx,
                                                 const QueryInstance& queryInstance,
                                                 const boost::optional<TenantId>& tenantId);

class QuerySettingsService {
public:
    /**
     * Gets the instance of the class using the service context.
     */
    static QuerySettingsService& get(ServiceContext* serviceContext);

    /**
     * Gets the instance of the class using the operation context.
     */
    static QuerySettingsService& get(OperationContext* opCtx);

    /**
     * Returns the name of the cluster parameter that stores QuerySettings for all QueryShapes.
     */
    static std::string getQuerySettingsClusterParameterName();

    /**
     * Checks the query settings eligibility of the current command referred by 'expCtx' for
     * namespace 'nss'. Query settings are not eligible for IDHACK/Express queries, encrypted
     * queries and queries run on internal or system collections.
     */
    static bool isEligbleForQuerySettings(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          const NamespaceString& nss);

    /**
     * Returns a set of system and administrative aggregation pipeline stages that, if used as the
     * initial stage, prevent the query from being rejected via query settings.
     *
     * Query settings module is responsible for maintaining the information about what aggregation
     * stages can be rejected.
     */
    static const stdx::unordered_set<StringData, StringMapHasher>& getRejectionIncompatibleStages();

    /**
     * Creates the QuerySettingsService that is attached to the 'serviceContext' with the logic
     * specific to the router/mongos.
     */
    static void initializeForRouter(ServiceContext* serviceContext);

    /**
     * Creates the QuerySettingsService that is attached to the 'serviceContext' with the logic
     * specific to the shard/mongod.
     */
    static void initializeForShard(ServiceContext* serviceContext,
                                   SetClusterParameterFn setClusterParameterFn);

    /**
     * Creates the QuerySettingsService that is attached to the 'serviceContext' with the logic
     * specific to the shard/mongod, without providing the necessary dependencies.
     */
    static void initializeForTest(ServiceContext* serviceContext);

    virtual ~QuerySettingsService() = default;

    /**
     * Returns the appropriate QuerySettings:
     *
     * - On router and shard in replica set deployment performs QuerySettings lookup for
     * QueryShapeHash. If the QuerySettings include 'reject: true' and is not run in explain, a
     * uassert is thrown with the QueryRejectedBySettings error code, rejecting the query.
     *
     * - On shard in sharded cluster returns 'querySettingsFromOriginalCommand'. This corresponds to
     * the QuerySettings looked up on the router and passed to shards as part of the command.
     * Rejection check is not performed here, as queries with 'reject: true' in their QuerySettings
     * would already have been rejected by the router.
     */
    virtual QuerySettings lookupQuerySettingsWithRejectionCheck(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const query_shape::QueryShapeHash& queryShapeHash,
        const NamespaceString& nss,
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand =
            boost::none) const = 0;

    /**
     * Convenience overload that skips settings lookup if 'queryShapeHash' is boost::none, returning
     * default settings. This method simplifies call sites by avoiding manual checks for
     * QueryShapeHash presence.
     */
    QuerySettings lookupQuerySettingsWithRejectionCheck(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::optional<query_shape::QueryShapeHash>& queryShapeHash,
        const NamespaceString& nss,
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand =
            boost::none) const {
        if (!queryShapeHash) {
            return querySettingsFromOriginalCommand.value_or(QuerySettings());
        }

        return lookupQuerySettingsWithRejectionCheck(
            expCtx, *queryShapeHash, nss, querySettingsFromOriginalCommand);
    }

    /**
     * Returns all the query shape configurations and the timestamp of the last modification.
     */
    virtual QueryShapeConfigurationsWithTimestamp getAllQueryShapeConfigurations(
        const boost::optional<TenantId>& tenantId) const = 0;

    /**
     * Sets all the query shape configurations with the given timestamp.
     */
    virtual void setAllQueryShapeConfigurations(
        QueryShapeConfigurationsWithTimestamp&& queryShapeConfigurations,
        const boost::optional<TenantId>& tenantId) = 0;

    /**
     * Removes all query shape configurations.
     */
    virtual void removeAllQueryShapeConfigurations(const boost::optional<TenantId>& tenantId) = 0;

    /**
     * Returns the LogicalTime of the 'querySettings' cluster parameter.
     */
    virtual LogicalTime getClusterParameterTime(
        const boost::optional<TenantId>& tenantId) const = 0;

    /**
     * Creates the corresponding 'querySettings' cluster parameter value out of the 'config' and
     * issues the setClusterParameter command.
     * 'querySettings' cluster parameter will set its cluster time to 'newClusterParameterTime'. If
     * not specified setClusterParameter module will come up with the new cluster time.
     */
    virtual void setQuerySettingsClusterParameter(
        OperationContext* opCtx,
        const QueryShapeConfigurationsWithTimestamp& config,
        boost::optional<LogicalTime> newClusterParameterTime = boost::none) const = 0;

    /**
     * Creates 'queryShapeRepresentativeQueries' collection locally. Throws an exception in case of
     * collection creation failure, unless the collection already exists.
     */
    virtual void createQueryShapeRepresentativeQueriesCollection(OperationContext* opCtx) const = 0;

    /**
     * Drops 'queryShapeRepresentativeQueries' collection locally. Throws an exception in case of
     * collection drop failure, unless the collection doesn't exist.
     */
    virtual void dropQueryShapeRepresentativeQueriesCollection(OperationContext* opCtx) const = 0;

    /**
     * Clears the 'representativeQuery' field from each QueryShapeConfiguration in 'querySettings'
     * cluster parameter and upserts these entries into 'queryShapeRepresentativeQueries'
     * collection.
     */
    virtual void migrateRepresentativeQueriesFromQuerySettingsClusterParameterToDedicatedCollection(
        OperationContext* opCtx) const = 0;

    /**
     * Populates the 'representativeQuery' field for each QueryShapeConfiguration from the
     * 'queryShapeRepresentativeQueries' collection. In case of BSONObjectTooLarge exception,
     * catches it, without performing any further migration.
     */
    virtual void migrateRepresentativeQueriesFromDedicatedCollectionToQuerySettingsClusterParameter(
        OperationContext* opCtx) const = 0;

    /**
     * Upserts the 'representativeQueries' into the 'queryShapeRepresentativeQueries' collection
     * locally via DBDirectClient. Catches all DBExceptions on failed upsert.
     */
    virtual void upsertRepresentativeQueries(
        OperationContext* opCtx,
        const std::vector<QueryShapeRepresentativeQuery>& representativeQueries) const = 0;

    /**
     * Deletes the 'representativeQuery' from the 'queryShapeRepresentativeQueries' collection by
     * 'queryShapeHash' locally via DBDirectClient. Catches all DBExceptions on failed delete.
     * Deletes only those representative queries that have 'lastModifiedTime' field less than or
     * equal to 'latestClusterParameterTime'.
     */
    virtual void deleteQueryShapeRepresentativeQuery(
        OperationContext* opCtx,
        const query_shape::QueryShapeHash& queryShapeHash,
        LogicalTime latestClusterParameterTime) const = 0;

    /**
     * Validates that 'querySettings' do not have:
     * - empty settings or settings with default values
     * - index hints specified without namespace information
     * - index hints specified for the same namespace more than once
     *
     * Throws a uassert if compatibility checks fail, indicating that 'querySettings' cannot be set.
     */
    void validateQuerySettings(const QuerySettings& querySettings) const;

    /**
     * Validates that QuerySettings can be applied to the query represented by 'queryInfo'.
     * Throws a uassert if compatibility checks fail, indicating that 'querySettings' cannot be set.
     */
    void validateQueryCompatibleWithAnyQuerySettings(
        const RepresentativeQueryInfo& queryInfo) const;

    /**
     * Validates that 'querySettings' can be applied to the query represented by 'queryInfo'.
     * Throws a uassert if compatibility checks fail, indicating that 'querySettings' cannot be set.
     */
    void validateQueryCompatibleWithQuerySettings(const RepresentativeQueryInfo& queryInfo,
                                                  const QuerySettings& querySettings) const;

    /**
     * Validates that 'config' is valid and does not exceed the allowed storage limits, throws a
     * DBException otherwise.
     */
    void validateQueryShapeConfigurations(
        const QueryShapeConfigurationsWithTimestamp& config) const;

    /**
     * Simplifies 'querySettings' in-place by:
     * - resetting the 'reject' field to boost::none if it contains a false value
     * - removing index hints that specify empty 'allowedIndexes', potentially resetting
     * 'indexHints' to boost::none if all 'allowedIndexes' are empty.
     */
    void simplifyQuerySettings(QuerySettings& querySettings) const;

    /**
     * Sanitizes the 'queryShapeConfigurations' removing those hints that contain invalid index key
     * pattern. In case the underlying query settings object contains only the default settings, the
     * corresponding QueryShapeConfiguration is removed.
     */
    void sanitizeQuerySettingsHints(
        std::vector<QueryShapeConfiguration>& queryShapeConfigurations) const;
};

/**
 * Returns true if the aggregation pipeline 'pipeline' does not start with rejection incompatible
 * stage, and therefore can be rejected.
 */
bool canPipelineBeRejected(const std::vector<BSONObj>& pipeline);

/**
 * Determines if 'querySettings' field is allowed to be present as part of the command request for
 * the given 'client'.
 */
bool allowQuerySettingsFromClient(Client* client);

/**
 * Returns true if given QuerySettings instance contains only default values.
 */
bool isDefault(const QuerySettings& querySettings);
}  // namespace query_settings
}  // namespace mongo
