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
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/util/deferred.h"

namespace mongo {
class SetClusterParameter;

namespace query_settings {
using QueryInstance = BSONObj;

using SetClusterParameterImplFn = std::function<void(OperationContext*,
                                                     const SetClusterParameter&,
                                                     boost::optional<Timestamp>,
                                                     boost::optional<LogicalTime>)>;

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
     * Returns a set of system and administrative aggregation pipeline stages that, if used as the
     * initial stage, prevent the query from being rejected via query settings.
     *
     * Query settings module is responsible for maintaining the information about what aggregation
     * stages can be rejected.
     */
    static const stdx::unordered_set<StringData, StringMapHasher>& getRejectionIncompatibleStages();

    virtual ~QuerySettingsService() = default;

    /**
     * Returns the appropriate QuerySettings:
     *
     * - On router and shard in replica set deployment performs QuerySettings lookup for a specified
     * 'queryShape'. If no settings are found or if the 'queryShape' is ineligible (e.g., IDHACK
     * queries), returns empty QuerySettings. If the QuerySettings include 'reject: true' and is not
     * run in explain, a uassert is thrown with the QueryRejectedBySettings error code, rejecting
     * the query. Additionally, records the QueryShapeHash within the CurOp
     *
     * - On shard in sharded cluster returns 'querySettingsFromOriginalCommand'. This corresponds to
     * the QuerySettings looked up on the router and passed to shards as part of the command.
     * Rejection check is not performed here, as queries with 'reject: true' in their QuerySettings
     * would already have been rejected by the router.
     */
    virtual QuerySettings lookupQuerySettingsWithRejectionCheck(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const query_shape::DeferredQueryShape& queryShape,
        const NamespaceString& nss,
        const boost::optional<QuerySettings>& querySettingsFromOriginalCommand =
            boost::none) const = 0;

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
     * Refreshes the local copy of all QueryShapeConfiguration by fetching the latest version from
     * the configsvr. Is a no-op when called on shard.
     */
    virtual void refreshQueryShapeConfigurations(OperationContext* opCtx) = 0;

    /**
     * Creates the corresponding 'querySettings' cluster parameter value out of the 'config' and
     * issues the setClusterParameter command.
     */
    virtual void setQuerySettingsClusterParameter(
        OperationContext* opCtx, const QueryShapeConfigurationsWithTimestamp& config) = 0;

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

void initializeForRouter(ServiceContext* serviceContext, SetClusterParameterImplFn fn);
void initializeForShard(ServiceContext* serviceContext, SetClusterParameterImplFn fn);
void initializeForTest(ServiceContext* serviceContext);

/**
 * Retrieves the QuerySettings for a specified 'queryShape' over namespace 'nss'. If no settings are
 * found or if the 'queryShape' is ineligible (e.g., IDHACK queries), returns empty QuerySettings.
 * If the QuerySettings include 'reject: true' and is not run in explain, a uassert is thrown with
 * the QueryRejectedBySettings error code, rejecting the query. Additionally, records the
 * QueryShapeHash within the CurOp.
 */
QuerySettings lookupQuerySettingsWithRejectionCheckOnRouter(
    const boost::intrusive_ptr<mongo::ExpressionContext>& expCtx,
    const query_shape::DeferredQueryShape& queryShape,
    const NamespaceString& nss);

/**
 * Returns the appropriate QuerySettings based on deployment:
 *
 * - Sharded cluster: returns 'querySettingsFromOriginalCommand'. This corresponds to the
 * QuerySettings looked up on the router and passed to shards as part of the command. Rejection
 * check is not performed here, as queries with 'reject: true' in their QuerySettings would already
 * have been rejected by the router.
 *
 * - Replica set: retrieves the QuerySettings for a specified 'queryShape' over namespace 'nss'. If
 * no settings are found or if the 'queryShape' is ineligible (e.g., IDHACK queries), returns empty
 * QuerySettings. If the QuerySettings include 'reject: true' and is not run in explain, a uassert
 * is thrown with the QueryRejectedBySettings error code, rejecting the query. Additionally, records
 * the QueryShapeHash within the CurOp.
 */
QuerySettings lookupQuerySettingsWithRejectionCheckOnShard(
    const boost::intrusive_ptr<mongo::ExpressionContext>& expCtx,
    const query_shape::DeferredQueryShape& queryShape,
    const NamespaceString& nss,
    const boost::optional<QuerySettings>& querySettingsFromOriginalCommand);

/**
 * Returns all the query shape configurations and the timestamp of the last modification.
 */
QueryShapeConfigurationsWithTimestamp getAllQueryShapeConfigurations(
    OperationContext* opCtx, const boost::optional<TenantId>& tenantId);

/**
 * Refreshes the local copy of all QueryShapeConfiguration by fetching the latest version from the
 * configsvr. Is a no-op when called on shard.
 */
void refreshQueryShapeConfigurations(OperationContext* opCtx);

/**
 * Returns the name of the cluster parameter that stores QuerySettings for all QueryShapes.
 */
std::string getQuerySettingsClusterParameterName();

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
