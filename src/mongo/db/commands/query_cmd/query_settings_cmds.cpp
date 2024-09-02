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

#include "mongo/base/shim.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/commands/query_cmd/query_settings_cmds_gen.h"
#include "mongo/db/commands/set_cluster_parameter_command_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

using namespace query_settings;

MONGO_FAIL_POINT_DEFINE(querySettingsPlanCacheInvalidation);
MONGO_FAIL_POINT_DEFINE(pauseAfterReadingQuerySettingsConfigurationParameter);

static constexpr auto kQuerySettingsClusterParameterName = "querySettings"_sd;

SetClusterParameter makeSetClusterParameterRequest(
    const std::vector<QueryShapeConfiguration>& settingsArray,
    const mongo::DatabaseName& dbName) try {
    BSONObjBuilder bob;
    BSONArrayBuilder arrayBuilder(
        bob.subarrayStart(QuerySettingsClusterParameterValue::kSettingsArrayFieldName));
    for (const auto& item : settingsArray) {
        arrayBuilder.append(item.toBSON());
    }
    arrayBuilder.done();
    SetClusterParameter setClusterParameterRequest(
        BSON(QuerySettingsManager::kQuerySettingsClusterParameterName << bob.done()));
    setClusterParameterRequest.setDbName(dbName);
    return setClusterParameterRequest;
} catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
    uasserted(ErrorCodes::BSONObjectTooLarge,
              str::stream() << "cannot modify query settings: the total size exceeds "
                            << BSONObjMaxInternalSize << " bytes");
}

/**
 * Invokes the setClusterParameter() weak function, which is an abstraction over the corresponding
 * command implementation in the router-role vs. the shard-role/the replica-set or standalone impl.
 */
void setClusterParameter(OperationContext* opCtx,
                         const SetClusterParameter& request,
                         boost::optional<Timestamp> clusterParameterTime,
                         boost::optional<LogicalTime> previousTime) {
    auto w = getSetClusterParameterImpl(opCtx);
    w(opCtx, request, clusterParameterTime, previousTime);
}

/**
 * Returns an iterator pointing to QueryShapeConfiguration in 'queryShapeConfigurations' that has
 * query shape hash value 'queryShapeHash'. Returns 'queryShapeConfigurations.end()' if there is no
 * match.
 */
auto findQueryShapeConfigurationByQueryShapeHash(
    std::vector<QueryShapeConfiguration>& queryShapeConfigurations,
    const query_shape::QueryShapeHash& queryShapeHash) {
    return std::find_if(queryShapeConfigurations.begin(),
                        queryShapeConfigurations.end(),
                        [&](const QueryShapeConfiguration& configuration) {
                            return configuration.getQueryShapeHash() == queryShapeHash;
                        });
}

/**
 * Merges the query settings 'lhs' with query settings 'rhs', by replacing all attributes in 'lhs'
 * with the existing attributes in 'rhs'.
 */
QuerySettings mergeQuerySettings(const QuerySettings& lhs, const QuerySettings& rhs) {
    static_assert(
        QuerySettings::fieldNames.size() == 5,
        "A new field has been added to the QuerySettings structure, mergeQuerySettings() should be "
        "updated appropriately.");

    QuerySettings querySettings = lhs;

    if (rhs.getQueryFramework()) {
        querySettings.setQueryFramework(rhs.getQueryFramework());
    }

    if (rhs.getIndexHints()) {
        querySettings.setIndexHints(rhs.getIndexHints());
    }

    // Note: update if reject has a value in the rhs, not just if that value is true.
    if (rhs.getReject().has_value()) {
        querySettings.setReject(rhs.getReject());
    }

    if (auto comment = rhs.getComment()) {
        querySettings.setComment(comment);
    }

    return querySettings;
}

/**
 * Reads, modifies, and updates the 'querySettings' cluster-wide configuration option. Follows the
 * Optimistic Offline Lock pattern when updating the option value. 'representativeQuery' indicates
 * the representative query for which an operation is performed and is used only for fail-point
 * programming.
 */
void readModifyWriteQuerySettingsConfigOption(
    OperationContext* opCtx,
    const mongo::DatabaseName& dbName,
    const boost::optional<QueryInstance>& representativeQuery,
    std::function<void(std::vector<QueryShapeConfiguration>&)> modify) {
    auto& querySettingsManager = QuerySettingsManager::get(opCtx);

    // The local copy of the query settings cluster-wide configuration option might not have the
    // latest value on mongos, therefore we trigger the update of the local copy before reading from
    // it in order to reduce the probability of update conflicts.
    querySettingsManager.refreshQueryShapeConfigurations(opCtx);

    // Read the query shape configurations for the tenant from the local copy of the query settings
    // cluster-wide configuration option.
    auto queryShapeConfigurations =
        querySettingsManager.getAllQueryShapeConfigurations(opCtx, dbName.tenantId());

    // Block if the operation is on the 'representativeQuery' that matches the
    // "representativeQueryToBlock" field of the fail-point configuration.
    if (MONGO_unlikely(pauseAfterReadingQuerySettingsConfigurationParameter.shouldFail(
            [&](const BSONObj& failPointConfiguration) {
                if (failPointConfiguration.isEmpty() || !representativeQuery.has_value()) {
                    return false;
                }
                BSONElement representativeQueryToBlock =
                    failPointConfiguration.getField("representativeQueryToBlock");
                return representativeQueryToBlock.isABSONObj() &&
                    representativeQueryToBlock.Obj().woCompare(*representativeQuery) == 0;
            }))) {
        tassert(8911800, "unexpected empty 'representativeQuery'", representativeQuery);
        LOGV2(8911801,
              "Hit pauseAfterReadingQuerySettingsConfigurationParameter fail-point",
              "representativeQuery"_attr = representativeQuery->toString());
        pauseAfterReadingQuerySettingsConfigurationParameter.pauseWhileSet(opCtx);
    }

    // Modify the query settings array (append, replace, or remove).
    modify(queryShapeConfigurations.queryShapeConfigurations);

    // Run "setClusterParameter" command with the new value of the 'querySettings' cluster-wide
    // parameter.
    setClusterParameter(
        opCtx,
        makeSetClusterParameterRequest(queryShapeConfigurations.queryShapeConfigurations, dbName),
        boost::none,
        queryShapeConfigurations.clusterParameterTime);

    // Refresh the local copy of the query settings cluster-wide configuration option so the results
    // of the update step above are visible.
    querySettingsManager.refreshQueryShapeConfigurations(opCtx);

    // Clears the SBE plan cache if 'querySettingsPlanCacheInvalidation' fail-point is set. Used in
    // tests when setting index filters via query settings interface.
    if (MONGO_unlikely(querySettingsPlanCacheInvalidation.shouldFail())) {
        sbe::getPlanCache(opCtx).clear();
    }
}

void assertNoStandalone(OperationContext* opCtx, const std::string& cmdName) {
    auto* repl = repl::ReplicationCoordinator::get(opCtx);
    bool isStandalone = repl && !repl->getSettings().isReplSet() &&
        serverGlobalParams.clusterRole.has(ClusterRole::None);
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << cmdName << " can only run on replica sets or sharded clusters",
            !isStandalone);
}

/**
 * Validates and simplifies query settings 'querySettings' for a representative query described by
 * 'representativeQueryInfo'. An empty 'representativeQueryInfo' indicates that the representative
 * query was not provided. 'previousRepresentativeQuery' is the previous version of representative
 * query of the query settings entry being updated, if available.
 */
void validateAndSimplifyQuerySettings(
    OperationContext* opCtx,
    const boost::optional<TenantId>& tenantId,
    const boost::optional<const RepresentativeQueryInfo&>& representativeQueryInfo,
    const boost::optional<QueryInstance>& previousRepresentativeQuery,
    QuerySettings& querySettings) {
    // In case the representative query was not provided but the previous representative query is
    // available, assert that query settings will be set on a valid query.
    if (!representativeQueryInfo && previousRepresentativeQuery) {
        utils::validateRepresentativeQuery(
            createRepresentativeInfo(opCtx, *previousRepresentativeQuery, tenantId));
    }
    if (representativeQueryInfo) {
        utils::verifyQueryCompatibleWithSettings(*representativeQueryInfo, querySettings);
    }
    utils::simplifyQuerySettings(querySettings);
    utils::validateQuerySettings(querySettings);
}

class SetQuerySettingsCommand final : public TypedCommand<SetQuerySettingsCommand> {
public:
    using Request = SetQuerySettingsCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Sets the query settings for the query shape of a given query.";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        SetQuerySettingsCommandReply typedRun(OperationContext* opCtx) {
            uassert(7746400,
                    "setQuerySettings command is unknown",
                    feature_flags::gFeatureFlagQuerySettings.isEnabled(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
            assertNoStandalone(opCtx, definition()->getName());
            uassert(8727502,
                    "settings field in setQuerySettings command cannot be empty",
                    !request().getSettings().toBSON().isEmpty());
            auto response =
                visit(OverloadedVisitor{
                          [&](const query_shape::QueryShapeHash& queryShapeHash) {
                              return setQuerySettings(opCtx,
                                                      boost::none /*representativeQuery*/,
                                                      boost::none /*representativeQueryInfo*/,
                                                      queryShapeHash);
                          },
                          [&](const QueryInstance& representativeQuery) {
                              const auto representativeQueryInfo = createRepresentativeInfo(
                                  opCtx, representativeQuery, request().getDbName().tenantId());
                              return setQuerySettings(opCtx,
                                                      representativeQuery,
                                                      representativeQueryInfo,
                                                      representativeQueryInfo.queryShapeHash);
                          },
                      },
                      request().getCommandParameter());
            return response;
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::querySettings}));
        }

        // Inserts or updates a query settings entry for a query identified by query shape hash
        // 'queryShapeHash'. If 'representativeQuery' is set, then it is the representative query to
        // use and 'representativeQueryInfo' for the query must be set too.
        SetQuerySettingsCommandReply setQuerySettings(
            OperationContext* opCtx,
            const boost::optional<QueryInstance> representativeQuery,
            const boost::optional<const RepresentativeQueryInfo&> representativeQueryInfo,
            const query_shape::QueryShapeHash& queryShapeHash) {
            // Validate that both 'representativeQuery' and 'representativeQueryInfo' are either
            // empty or not empty.
            dassert(!(representativeQuery.has_value() ^ representativeQueryInfo.has_value()));

            // Assert that query settings will be set on a valid query.
            if (representativeQuery) {
                utils::validateRepresentativeQuery(*representativeQueryInfo);
            }

            SetQuerySettingsCommandReply reply;
            auto&& tenantId = request().getDbName().tenantId();

            readModifyWriteQuerySettingsConfigOption(
                opCtx,
                request().getDbName(),
                representativeQuery,
                [&](auto& queryShapeConfigurations) {
                    // Lookup a query shape configuration by query shape hash.
                    auto matchingQueryShapeConfigurationIt =
                        findQueryShapeConfigurationByQueryShapeHash(queryShapeConfigurations,
                                                                    queryShapeHash);
                    if (matchingQueryShapeConfigurationIt == queryShapeConfigurations.end()) {
                        // Make a query shape configuration to insert.
                        QueryShapeConfiguration newQueryShapeConfiguration(queryShapeHash,
                                                                           request().getSettings());
                        newQueryShapeConfiguration.setRepresentativeQuery(representativeQuery);
                        // Add a new query settings entry.
                        validateAndSimplifyQuerySettings(
                            opCtx,
                            tenantId,
                            representativeQueryInfo,
                            boost::none /*previousRepresentativeQuery*/,
                            newQueryShapeConfiguration.getSettings());
                        LOGV2_DEBUG(8911805,
                                    1,
                                    "Inserting query settings entry",
                                    "representativeQuery"_attr = representativeQuery,
                                    "settings"_attr =
                                        newQueryShapeConfiguration.getSettings().toBSON());
                        queryShapeConfigurations.push_back(newQueryShapeConfiguration);

                        // Update the reply with the new query shape configuration.
                        reply.setQueryShapeConfiguration(std::move(newQueryShapeConfiguration));
                    } else {
                        // Update an existing query settings entry by updating the existing
                        // QueryShapeConfiguration with the new query settings.
                        auto&& queryShapeConfigurationToUpdate = *matchingQueryShapeConfigurationIt;
                        auto mergedQuerySettings = mergeQuerySettings(
                            queryShapeConfigurationToUpdate.getSettings(), request().getSettings());
                        validateAndSimplifyQuerySettings(
                            opCtx,
                            tenantId,
                            representativeQueryInfo,
                            queryShapeConfigurationToUpdate.getRepresentativeQuery(),
                            mergedQuerySettings);
                        LOGV2_DEBUG(8911806,
                                    1,
                                    "Updating query settings entry",
                                    "representativeQuery"_attr = representativeQuery,
                                    "settings"_attr = mergedQuerySettings.toBSON());
                        queryShapeConfigurationToUpdate.setSettings(mergedQuerySettings);

                        // Update the representative query if provided.
                        if (representativeQuery) {
                            queryShapeConfigurationToUpdate.setRepresentativeQuery(
                                representativeQuery);
                        }

                        // Update the reply with the updated query shape configuration.
                        reply.setQueryShapeConfiguration(queryShapeConfigurationToUpdate);
                    }
                });
            return reply;
        }
    };
};
MONGO_REGISTER_COMMAND(SetQuerySettingsCommand).forRouter().forShard();

class RemoveQuerySettingsCommand final : public TypedCommand<RemoveQuerySettingsCommand> {
public:
    using Request = RemoveQuerySettingsCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Removes the query settings for the query shape of a given query.";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(7746700,
                    "removeQuerySettings command is unknown",
                    feature_flags::gFeatureFlagQuerySettings.isEnabled(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
            assertNoStandalone(opCtx, definition()->getName());
            auto tenantId = request().getDbName().tenantId();
            auto queryShapeHashAndRepresentativeQuery =
                visit(OverloadedVisitor{
                          [&](const query_shape::QueryShapeHash& queryShapeHash) {
                              return std::pair{queryShapeHash, boost::optional<QueryInstance>{}};
                          },
                          [&](const QueryInstance& representativeQuery) {
                              // Converts 'representativeQuery' into QueryShapeHash, for convenient
                              // comparison during search for the matching QueryShapeConfiguration.
                              auto representativeQueryInfo =
                                  createRepresentativeInfo(opCtx, representativeQuery, tenantId);

                              return std::pair{representativeQueryInfo.queryShapeHash,
                                               boost::optional<QueryInstance>{representativeQuery}};
                          },
                      },
                      request().getCommandParameter());

            const auto& queryShapeHash = queryShapeHashAndRepresentativeQuery.first;
            readModifyWriteQuerySettingsConfigOption(
                opCtx,
                request().getDbName(),
                queryShapeHashAndRepresentativeQuery.second,
                [&](auto& queryShapeConfigurations) {
                    // Build the new 'queryShapeConfigurations' by removing the first
                    // QueryShapeConfiguration matching the 'queryShapeHash'. There can be only one
                    // match, since 'queryShapeConfigurations' is constructed from a map where
                    // QueryShapeHash is the key.
                    auto matchingQueryShapeConfigurationIt =
                        findQueryShapeConfigurationByQueryShapeHash(queryShapeConfigurations,
                                                                    queryShapeHash);
                    if (matchingQueryShapeConfigurationIt != queryShapeConfigurations.end()) {
                        LOGV2_DEBUG(8911807,
                                    1,
                                    "Removing query settings entry",
                                    "representativeQuery"_attr =
                                        queryShapeHashAndRepresentativeQuery.second);
                        queryShapeConfigurations.erase(matchingQueryShapeConfigurationIt);
                    }
                });
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::querySettings}));
        }
    };
};
MONGO_REGISTER_COMMAND(RemoveQuerySettingsCommand).forRouter().forShard();
}  // namespace
}  // namespace mongo
