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
#include "mongo/db/commands/query_settings_cmds_gen.h"
#include "mongo/db/commands/set_cluster_parameter_command_impl.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_settings/query_settings_cluster_parameter_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/platform/basic.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using namespace query_settings;

MONGO_FAIL_POINT_DEFINE(querySettingsPlanCacheInvalidation);

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
 * Merges the query settings 'lhs' with query settings 'rhs', by replacing all attributes in 'lhs'
 * with the existing attributes in 'rhs'.
 */
QuerySettings mergeQuerySettings(const QuerySettings& lhs, const QuerySettings& rhs) {
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

    return querySettings;
}

void simplifyQuerySettings(QuerySettings& settings) {
    // If reject is present, but is false, set to an empty optional.
    // This is not strictly necessary, as isEmpty treats OptionalBool(false)
    // as empty.
    if (!settings.getReject().has_value()) {
        settings.setReject({});
    }
}

/**
 * Reads (from the in-memory 'storage' = cache), modifies, and updates the 'querySettings' cluster
 * parameter.
 */
void readModifyWrite(OperationContext* opCtx,
                     const mongo::DatabaseName& dbName,
                     std::function<void(std::vector<QueryShapeConfiguration>&)> modify) {
    auto& querySettingsManager = QuerySettingsManager::get(opCtx);

    // Read the query settings array from the cache. The cache might not have the latest cluster
    // parameter values on mongos, therefore we trigger the cache update before reading from it.
    querySettingsManager.refreshQueryShapeConfigurations(opCtx);
    auto settingsArray =
        querySettingsManager.getAllQueryShapeConfigurations(opCtx, dbName.tenantId());

    // Modify the query settings array (append, replace, or remove).
    modify(settingsArray);

    // Run SetClusterParameter command with the new value of the 'querySettings' cluster
    // parameter.
    setClusterParameter(opCtx,
                        makeSetClusterParameterRequest(settingsArray, dbName),
                        boost::none,
                        querySettingsManager.getClusterParameterTime(opCtx, dbName.tenantId()));
    querySettingsManager.refreshQueryShapeConfigurations(opCtx);

    /**
     * Clears the SBE plan cache if 'querySettingsPlanCacheInvalidation' failpoint is set.
     * Used in tests when setting index filters via query settings interface.
     */
    if (MONGO_unlikely(querySettingsPlanCacheInvalidation.shouldFail())) {
        sbe::getPlanCache(opCtx).clear();
    }
}

/**
 * Finds a query shape configuration by the given query shape hash in the given vector. Returns an
 * iterator (non-const) in cases of success or throws otherwise.
 */
std::vector<QueryShapeConfiguration>::iterator findByHash(
    std::vector<QueryShapeConfiguration>& settingsArray,
    const query_shape::QueryShapeHash& queryShapeHash) {
    auto matchingQueryShapeConfigurationIt =
        std::find_if(settingsArray.begin(),
                     settingsArray.end(),
                     [&](const QueryShapeConfiguration& configuration) {
                         return configuration.getQueryShapeHash() == queryShapeHash;
                     });
    // Ensure the 'queryShapeHash' is present in the 'settingsArray'.
    uassert(7746701,
            "A matching query settings entry does not exist",
            matchingQueryShapeConfigurationIt != settingsArray.end());
    return matchingQueryShapeConfigurationIt;
}

void assertNoStandalone(OperationContext* opCtx, const std::string& cmdName) {
    auto* repl = repl::ReplicationCoordinator::get(opCtx);
    bool isStandalone = repl && !repl->getSettings().isReplSet() &&
        serverGlobalParams.clusterRole.has(ClusterRole::None);
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << cmdName << " can only run on replica sets or sharded clusters",
            !isStandalone);
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

        SetQuerySettingsCommandReply insertQuerySettings(
            OperationContext* opCtx,
            QueryShapeConfiguration newQueryShapeConfiguration,
            const RepresentativeQueryInfo& representativeQueryInfo) {
            // Assert querySettings command is valid.
            utils::validateQuerySettings(newQueryShapeConfiguration,
                                         representativeQueryInfo,
                                         request().getDbName().tenantId());
            simplifyQuerySettings(newQueryShapeConfiguration.getSettings());
            uassert(8587401,
                    "QuerySettings would be empty (all default settings), nothing to store, no "
                    "action needed",
                    !utils::isEmpty(newQueryShapeConfiguration.getSettings()));

            // Append 'newQueryShapeConfiguration' to the list of all 'QueryShapeConfigurations' for
            // the given database / tenant.
            readModifyWrite(opCtx, request().getDbName(), [&](auto& settingsArray) {
                settingsArray.push_back(newQueryShapeConfiguration);
            });

            SetQuerySettingsCommandReply reply;
            reply.setQueryShapeConfiguration(std::move(newQueryShapeConfiguration));
            return reply;
        }

        SetQuerySettingsCommandReply updateQuerySettings(
            OperationContext* opCtx,
            const QuerySettings& newQuerySettings,
            const QueryShapeConfiguration& currentQueryShapeConfiguration) {
            // Compute the merged query settings.
            auto mergedQuerySettings =
                mergeQuerySettings(currentQueryShapeConfiguration.getSettings(), newQuerySettings);
            simplifyQuerySettings(mergedQuerySettings);
            uassert(8587402,
                    "Resulting QuerySettings would be empty (all default settings), use "
                    "removeQuerySettings instead",
                    !utils::isEmpty(mergedQuerySettings));

            // Build the new 'settingsArray' by updating the existing QueryShapeConfiguration with
            // the 'mergedQuerySettings'.
            readModifyWrite(opCtx, request().getDbName(), [&](auto& settingsArray) {
                findByHash(settingsArray, currentQueryShapeConfiguration.getQueryShapeHash())
                    ->setSettings(mergedQuerySettings);
            });

            SetQuerySettingsCommandReply reply;
            reply.setQueryShapeConfiguration(
                QueryShapeConfiguration(currentQueryShapeConfiguration.getQueryShapeHash(),
                                        mergedQuerySettings,
                                        currentQueryShapeConfiguration.getRepresentativeQuery()));
            return reply;
        }

        SetQuerySettingsCommandReply setQuerySettingsByQueryShapeHash(
            OperationContext* opCtx, const query_shape::QueryShapeHash& queryShapeHash) {
            auto& querySettingsManager = QuerySettingsManager::get(opCtx);
            auto tenantId = request().getDbName().tenantId();

            auto querySettings = querySettingsManager.getQuerySettingsForQueryShapeHash(
                opCtx, queryShapeHash, tenantId);
            uassert(7746401,
                    "New query settings can only be created with a query instance, but a query "
                    "hash was given.",
                    querySettings.has_value());

            return updateQuerySettings(opCtx,
                                       request().getSettings(),
                                       QueryShapeConfiguration(queryShapeHash,
                                                               std::move(querySettings->first),
                                                               std::move(querySettings->second)));
        }

        SetQuerySettingsCommandReply setQuerySettingsByQueryInstance(
            OperationContext* opCtx, const QueryInstance& queryInstance) {
            auto& querySettingsManager = QuerySettingsManager::get(opCtx);
            auto tenantId = request().getDbName().tenantId();
            auto representativeQueryInfo = createRepresentativeInfo(queryInstance, opCtx, tenantId);
            auto& queryShapeHash = representativeQueryInfo.queryShapeHash;

            // If there is already an entry for a given QueryShapeHash, then perform
            // an update, otherwise insert.
            if (auto lookupResult = querySettingsManager.getQuerySettingsForQueryShapeHash(
                    opCtx,
                    [&]() { return queryShapeHash; },
                    representativeQueryInfo.namespaceString)) {
                return updateQuerySettings(
                    opCtx,
                    request().getSettings(),
                    QueryShapeConfiguration(std::move(queryShapeHash),
                                            std::move(lookupResult->first),
                                            std::move(lookupResult->second)));
            } else {
                return insertQuerySettings(
                    opCtx,
                    QueryShapeConfiguration(std::move(queryShapeHash),
                                            std::move(request().getSettings()),
                                            queryInstance),
                    representativeQueryInfo);
            }
        }

        SetQuerySettingsCommandReply typedRun(OperationContext* opCtx) {
            uassert(7746400,
                    "setQuerySettings command is unknown",
                    feature_flags::gFeatureFlagQuerySettings.isEnabled(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
            assertNoStandalone(opCtx, definition()->getName());
            auto response =
                visit(OverloadedVisitor{
                          [&](const query_shape::QueryShapeHash& queryShapeHash) {
                              return setQuerySettingsByQueryShapeHash(opCtx, queryShapeHash);
                          },
                          [&](const QueryInstance& queryInstance) {
                              return setQuerySettingsByQueryInstance(opCtx, queryInstance);
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
            auto queryShapeHash =
                visit(OverloadedVisitor{
                          [&](const query_shape::QueryShapeHash& queryShapeHash) {
                              return queryShapeHash;
                          },
                          [&](const QueryInstance& queryInstance) {
                              // Converts 'queryInstance' into QueryShapeHash, for convenient
                              // comparison during search for the matching
                              // QueryShapeConfiguration.
                              auto representativeQueryInfo =
                                  createRepresentativeInfo(queryInstance, opCtx, tenantId);

                              return representativeQueryInfo.queryShapeHash;
                          },
                      },
                      request().getCommandParameter());

            // Build the new 'settingsArray' by removing the QueryShapeConfiguration with a matching
            // QueryShapeHash.
            readModifyWrite(opCtx, request().getDbName(), [&](auto& settingsArray) {
                settingsArray.erase(findByHash(settingsArray, queryShapeHash));
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
