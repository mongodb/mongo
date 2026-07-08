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
#include "mongo/util/modules.h"
#include "mongo/util/version/releases.h"

#include <string_view>

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

    // Value of the rawData parameter in the query, or false if the parameter was not included.
    // Some collection types can store data in a different format than users expect.
    // If this parameter is true, then the query operates on documents directly in the format in
    // which they are stored.
    const bool isRawDataQuery;
};

/**
 * Creates a RepresentativeQueryInfo for the given query.
 */
RepresentativeQueryInfo createRepresentativeInfo(OperationContext* opCtx,
                                                 const QueryInstance& queryInstance,
                                                 const boost::optional<TenantId>& tenantId);

class MONGO_MOD_PUBLIC QuerySettingsService {
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
     * Applies 'settings.maxTimeMS' (if set) to the current operation's deadline. A no-op during
     * explain, and if 'settings' has no 'maxTimeMS'. Exposed so that every code path resolving
     * query settings (including the QueryShapeHash-less fallback) can (re-)apply 'maxTimeMS' to
     * this node's own operation, since each node enforces its own deadline independently.
     */
    static void applyMaxTimeMSFromSettings(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const QuerySettings& settings);

    /**
     * Returns a set of system and administrative aggregation pipeline stages that, if used as the
     * initial stage, prevent the query from being rejected via query settings.
     *
     * Query settings module is responsible for maintaining the information about what aggregation
     * stages can be rejected.
     */
    static const stdx::unordered_set<std::string_view, StringMapHasher>&
    getRejectionIncompatibleStages();

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
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand) const = 0;

    /**
     * Convenience overload that skips settings lookup if 'queryShapeHash' is boost::none, returning
     * default settings. This method simplifies call sites by avoiding manual checks for
     * QueryShapeHash presence.
     */
    QuerySettings lookupQuerySettingsWithRejectionCheck(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::optional<query_shape::QueryShapeHash>& queryShapeHash,
        const NamespaceString& nss,
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand) const {
        // Ineligible queries (IDHACK/Express, FLE, internal/system namespaces) never receive query
        // settings, whether from persisted settings or supplied directly by the user.
        if (!isEligbleForQuerySettings(expCtx, nss)) {
            return QuerySettings();
        }

        if (!queryShapeHash) {
            // No shape hash means no cluster-configured settings can be looked up (e.g. internal
            // clients such as a shard receiving a command forwarded by mongos deliberately skip
            // recomputing it, since mongos already did). 'reject' does not need to be re-checked
            // here, since mongos would already have rejected the command before ever dispatching
            // it; 'maxTimeMS', however, governs this node's own operation deadline and must still
            // be (re-)applied here, or a query settings override that loosens the deadline would
            // be silently lost on this node.
            auto settings = querySettingsFromOriginalCommand.value_or(QuerySettings());
            applyMaxTimeMSFromSettings(expCtx, settings);
            return settings;
        }

        return lookupQuerySettingsWithRejectionCheck(
            expCtx, *queryShapeHash, nss, querySettingsFromOriginalCommand);
    }

    /**
     * Resolves the query settings for the current query and makes them the active settings on the
     * operation: looks them up by 'queryShapeHash' (running the rejection check) and stores the
     * result so that 'query_settings::forOp(opCtx)' returns it. When 'queryShapeHash' is
     * boost::none the query is not eligible for settings and default settings are resolved.
     *
     * Resolution only runs while the operation is still 'Pending' (eligibility is decided lazily by
     * 'query_settings_details::getQuerySettingsStateForOp'). An ineligible operation or a
     * re-entrant resolution (view re-dispatch or a nested query against the same 'opCtx') is a
     * no-op. Not yet wired into any command.
     */
    void initializeSettingsForQuery(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::optional<query_shape::QueryShapeHash>& queryShapeHash,
        const NamespaceString& nss,
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand) const;

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
     * Performs any query settings data migrations needed when upgrading to 'targetFCV'. The
     * required actions are derived from which feature flags are enabled on 'targetFCV'.
     */
    virtual void upgradeQuerySettings(
        OperationContext* opCtx, multiversion::FeatureCompatibilityVersion targetFCV) const = 0;

    /**
     * Performs any query settings data migrations needed when downgrading to 'targetFCV'. The
     * required actions are derived from which feature flags are enabled on 'targetFCV'.
     */
    virtual void downgradeQuerySettings(
        OperationContext* opCtx, multiversion::FeatureCompatibilityVersion targetFCV) const = 0;

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
     * - duplicate query knob overrides
     * - index hints specified without namespace information
     * - index hints specified for the same namespace more than once
     *
     * Throws a uassert if compatibility checks fail, indicating that 'querySettings' cannot be set.
     * Tasserts that simplifyQuerySettings() was called first (no DeleteQueryKnobOverride
     * sentinels).
     */
    void validateQuerySettings(const QuerySettings& querySettings) const;

    /**
     * Validates user-provided query knob overrides in 'querySettings': rejects 'queryKnobs'
     * unless featureFlagPqsQueryKnobs is enabled, and rejects individual knobs whose minimum FCV
     * is above the current FCV. Must be called at every entry point accepting external query
     * settings; parsing (QuerySettingsKnobOverrides::fromBSON()) deliberately performs no FCV
     * validation, as it also handles internal traffic and stored settings.
     * TODO SERVER-122103: Remove the feature flag guard once featureFlagPqsQueryKnobs is removed
     * (SPM-4364).
     */
    void validateQueryKnobs(OperationContext* opCtx, const QuerySettings& querySettings) const;

    /**
     * Validates that 'maxTimeMS' in 'querySettings' is only used when featureFlagPqsMaxTimeMS is
     * enabled. Must be called at every entry point accepting external query settings, mirroring
     * validateQueryKnobs().
     */
    void validateMaxTimeMS(OperationContext* opCtx, const QuerySettings& querySettings) const;

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
     * - deleting remove sentinels and resetting to boost::none if it is empty.
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

/**
 * Merges the query settings 'lhs' with query settings 'rhs', by replacing all attributes in 'lhs'
 * with the existing attributes in 'rhs'.
 */
QuerySettings mergeQuerySettings(const QuerySettings& lhs, const QuerySettings& rhs);
}  // namespace query_settings
}  // namespace mongo
