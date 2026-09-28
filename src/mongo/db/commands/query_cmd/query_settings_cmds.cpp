// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/query_cmd/query_settings_cmds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

using namespace query_settings;

using QueryShapeHashQueryInstanceOptPair =
    std::pair<query_shape::QueryShapeHash, boost::optional<QueryInstance>>;

MONGO_FAIL_POINT_DEFINE(querySettingsPlanCacheInvalidation);
MONGO_FAIL_POINT_DEFINE(pauseAfterReadingQuerySettingsConfigurationParameter);
MONGO_FAIL_POINT_DEFINE(pauseAfterCallingSetClusterParameterInQuerySettingsCommands);

enum class QuerySettingsCmdType { kSet, kRemove };

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

using query_settings::mergeQuerySettings;

/**
 * Reads, modifies, and updates the 'querySettings' cluster-wide configuration option. Follows the
 * Optimistic Offline Lock pattern when updating the option value. 'representativeQuery' indicates
 * the representative query for which an operation is performed; it is recorded in the
 * 'config.queryShapeRepresentativeQueries' collection and used for fail-point programming.
 */
template <QuerySettingsCmdType CmdType>
void readModifyWriteQuerySettingsConfigOption(
    OperationContext* opCtx,
    const QueryShapeHashQueryInstanceOptPair& queryShapeHashQueryInstanceOptPair,
    std::function<void(std::vector<QueryShapeConfiguration>&)> modify) {
    auto& querySettingsService = QuerySettingsService::get(opCtx);

    // Read the query shape configurations from the local copy of the query settings
    // cluster-wide configuration option.
    auto queryShapeConfigurations = querySettingsService.getAllQueryShapeConfigurations();

    // Generate a new cluster parameter time that will be used when settings new version of
    // 'querySettings' cluster parameter. This cluster time will be assigned to the corresponding
    // representative query as well.
    LogicalTime newQuerySettingsParameterClusterTime = [&]() {
        auto vt = VectorClock::get(opCtx)->getTime();
        auto clusterTime = vt.clusterTime();
        dassert(clusterTime > queryShapeConfigurations.clusterParameterTime);
        return clusterTime;
    }();

    // Modify the query settings array (append, replace, or remove).
    modify(queryShapeConfigurations.queryShapeConfigurations);

    // Ensure 'queryShapeConfigurations' is in valid after modification.
    querySettingsService.validateQueryShapeConfigurations(queryShapeConfigurations);

    // Upsert QueryShapeRepresentativeQuery into the corresponding collection if provided.
    if constexpr (CmdType == QuerySettingsCmdType::kSet) {
        if (queryShapeHashQueryInstanceOptPair.second.has_value()) {
            querySettingsService.upsertRepresentativeQueries(
                opCtx,
                {QueryShapeRepresentativeQuery(
                    /* queryShapeHash */ queryShapeHashQueryInstanceOptPair.first,
                    /* representativeQuery */ *queryShapeHashQueryInstanceOptPair.second,
                    newQuerySettingsParameterClusterTime)});
        }
    }

    // Block if the operation is on the 'representativeQuery' that matches the
    // "representativeQueryToBlock" field of the fail-point configuration.
    // This failpoint should rather be called "hangBeforeCallingSetClusterParameter", but it is not
    // renamed due to multiversion compatibility in tests.
    if (MONGO_unlikely(pauseAfterReadingQuerySettingsConfigurationParameter.shouldFail(
            [&](const BSONObj& failPointConfiguration) {
                if (failPointConfiguration.isEmpty() ||
                    !queryShapeHashQueryInstanceOptPair.second.has_value()) {
                    return false;
                }
                BSONElement representativeQueryToBlock =
                    failPointConfiguration.getField("representativeQueryToBlock");
                return representativeQueryToBlock.isABSONObj() &&
                    representativeQueryToBlock.Obj().woCompare(
                        *queryShapeHashQueryInstanceOptPair.second) == 0;
            }))) {
        tassert(8911800,
                "unexpected empty 'representativeQuery'",
                queryShapeHashQueryInstanceOptPair.second);
        LOGV2(8911801,
              "Hit pauseAfterReadingQuerySettingsConfigurationParameter fail-point",
              "representativeQuery"_attr = queryShapeHashQueryInstanceOptPair.second->toString());
        pauseAfterReadingQuerySettingsConfigurationParameter.pauseWhileSet(opCtx);
    }

    // Run "setClusterParameter" command with the new value of the 'querySettings' cluster-wide
    // parameter and 'newQuerySettingsParameterClusterTime' cluster time.
    querySettingsService.setQuerySettingsClusterParameter(
        opCtx, queryShapeConfigurations, newQuerySettingsParameterClusterTime);

    // Hang in between setClusterParameter and representative query removal.
    if (MONGO_unlikely(pauseAfterCallingSetClusterParameterInQuerySettingsCommands.shouldFail(
            [&](const BSONObj& failPointConfiguration) {
                auto cmdTypeString = failPointConfiguration.firstElement().String();
                auto cmdTypeToHangOn = cmdTypeString == "set" ? QuerySettingsCmdType::kSet
                                                              : QuerySettingsCmdType::kRemove;
                return CmdType == cmdTypeToHangOn;
            }))) {
        pauseAfterCallingSetClusterParameterInQuerySettingsCommands.pauseWhileSet(opCtx);
    }

    // Remove QueryShapeRepresentativeQuery from the corresponding collection if present.
    if constexpr (CmdType == QuerySettingsCmdType::kRemove) {
        // deleteQueryShapeRepresentativeQuery() must only delete representative queries at the
        // cluster parameter time of 'querySettings' cluster parameter that it observes.
        querySettingsService.deleteQueryShapeRepresentativeQuery(
            opCtx,
            queryShapeHashQueryInstanceOptPair.first,
            queryShapeConfigurations.clusterParameterTime);
    }

    // Clears the SBE plan cache if 'querySettingsPlanCacheInvalidation' fail-point is set. Used in
    // tests when setting index filters via query settings interface.
    if (MONGO_unlikely(querySettingsPlanCacheInvalidation.shouldFail())) {
        sbe::getPlanCache(opCtx).clear();
    }
}

void assertNoStandalone(OperationContext* opCtx, const std::string& cmdName) {
    // TODO SERVER-104560: replace the code checking for standalone mode with a call to the utility.
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
 * query was not provided.
 */
void validateAndSimplifyQuerySettings(
    OperationContext* opCtx,
    const boost::optional<const RepresentativeQueryInfo&>& representativeQueryInfo,
    QuerySettings& querySettings) {
    auto& service = QuerySettingsService::get(opCtx);

    if (representativeQueryInfo) {
        service.validateQueryCompatibleWithQuerySettings(*representativeQueryInfo, querySettings);
    }
    service.simplifyQuerySettings(querySettings);
    service.validateQuerySettings(querySettings);
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
            assertNoStandalone(opCtx, definition()->getName());

            // Ensure FCV is not changing throughout the command execution.
            FixedFCVRegion fixedFcvRegion(opCtx);
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
                              const auto representativeQueryInfo =
                                  createRepresentativeInfo(opCtx, representativeQuery);
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
            auto& querySettingsService = QuerySettingsService::get(opCtx);
            querySettingsService.validateQueryKnobs(request().getSettings());

            // Validate that both 'representativeQuery' and 'representativeQueryInfo' are either
            // empty or not empty.
            dassert(!(representativeQuery.has_value() ^ representativeQueryInfo.has_value()));

            // Assert that query settings will be set on a valid query.
            if (representativeQuery) {
                querySettingsService.validateQueryCompatibleWithAnyQuerySettings(
                    *representativeQueryInfo);
            }

            SetQuerySettingsCommandReply reply;
            QueryShapeHashQueryInstanceOptPair queryShapeHashQueryInstanceOptPair = {
                queryShapeHash, representativeQuery};

            readModifyWriteQuerySettingsConfigOption<QuerySettingsCmdType::kSet>(
                opCtx, queryShapeHashQueryInstanceOptPair, [&](auto& queryShapeConfigurations) {
                    // Lookup a query shape configuration by query shape hash.
                    auto matchingQueryShapeConfigurationIt =
                        findQueryShapeConfigurationByQueryShapeHash(queryShapeConfigurations,
                                                                    queryShapeHash);
                    if (matchingQueryShapeConfigurationIt == queryShapeConfigurations.end()) {
                        // Make a query shape configuration to insert.
                        QueryShapeConfiguration newQueryShapeConfiguration(queryShapeHash,
                                                                           request().getSettings());

                        // Add a new query settings entry.
                        validateAndSimplifyQuerySettings(opCtx,
                                                         representativeQueryInfo,
                                                         newQueryShapeConfiguration.getSettings());

                        LOGV2_DEBUG(
                            8911805,
                            1,
                            "Inserting query settings entry",
                            "representativeQuery"_attr =
                                representativeQuery.map([](const BSONObj& b) { return redact(b); }),
                            "settings"_attr = newQueryShapeConfiguration.getSettings().toBSON());
                        queryShapeConfigurations.push_back(newQueryShapeConfiguration);

                        // Update the reply with the new query shape configuration.
                        reply.setRepresentativeQuery(representativeQuery);
                        reply.setQueryShapeConfiguration(std::move(newQueryShapeConfiguration));
                    } else {
                        // Update an existing query settings entry by updating the existing
                        // QueryShapeConfiguration with the new query settings.
                        auto&& queryShapeConfigurationToUpdate = *matchingQueryShapeConfigurationIt;
                        auto mergedQuerySettings = mergeQuerySettings(
                            queryShapeConfigurationToUpdate.getSettings(), request().getSettings());
                        validateAndSimplifyQuerySettings(
                            opCtx, representativeQueryInfo, mergedQuerySettings);
                        LOGV2_DEBUG(8911806,
                                    1,
                                    "Updating query settings entry",
                                    "representativeQuery"_attr = representativeQuery.map(
                                        [](const BSONObj& b) { return redact(b); }),
                                    "settings"_attr = mergedQuerySettings.toBSON());
                        queryShapeConfigurationToUpdate.setSettings(mergedQuerySettings);

                        // Update the reply with the updated query shape configuration.
                        reply.setQueryShapeConfiguration(queryShapeConfigurationToUpdate);
                    }
                });
            return reply;
        }
    };
};
MONGO_REGISTER_COMMAND(SetQuerySettingsCommand).forShard();

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
            assertNoStandalone(opCtx, definition()->getName());

            // Ensure FCV is not changing throughout the command execution.
            FixedFCVRegion fixedFcvRegion(opCtx);
            QueryShapeHashQueryInstanceOptPair queryShapeHashAndRepresentativeQuery =
                visit(OverloadedVisitor{
                          [&](const query_shape::QueryShapeHash& queryShapeHash) {
                              return std::pair{queryShapeHash, boost::optional<QueryInstance>{}};
                          },
                          [&](const QueryInstance& representativeQuery) {
                              // Converts 'representativeQuery' into QueryShapeHash, for convenient
                              // comparison during search for the matching QueryShapeConfiguration.
                              auto representativeQueryInfo =
                                  createRepresentativeInfo(opCtx, representativeQuery);

                              return std::pair{representativeQueryInfo.queryShapeHash,
                                               boost::optional<QueryInstance>{representativeQuery}};
                          },
                      },
                      request().getCommandParameter());

            const auto& queryShapeHash = queryShapeHashAndRepresentativeQuery.first;
            readModifyWriteQuerySettingsConfigOption<QuerySettingsCmdType::kRemove>(
                opCtx, queryShapeHashAndRepresentativeQuery, [&](auto& queryShapeConfigurations) {
                    // Build the new 'queryShapeConfigurations' by removing the first
                    // QueryShapeConfiguration matching the 'queryShapeHash'. There can be only one
                    // match, since 'queryShapeConfigurations' is constructed from a map where
                    // QueryShapeHash is the key.
                    auto matchingQueryShapeConfigurationIt =
                        findQueryShapeConfigurationByQueryShapeHash(queryShapeConfigurations,
                                                                    queryShapeHash);
                    if (matchingQueryShapeConfigurationIt != queryShapeConfigurations.end()) {
                        const auto& rep = queryShapeHashAndRepresentativeQuery.second;
                        LOGV2_DEBUG(8911807,
                                    1,
                                    "Removing query settings entry",
                                    "representativeQuery"_attr =
                                        rep.map([](const BSONObj& b) { return redact(b); }));
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
MONGO_REGISTER_COMMAND(RemoveQuerySettingsCommand).forShard();
}  // namespace
}  // namespace mongo
