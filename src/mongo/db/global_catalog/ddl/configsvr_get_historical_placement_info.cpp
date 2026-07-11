// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/placement_history_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

HistoricalPlacement initializePlacementHistoryAndRetry(
    OperationContext* opCtx,
    const boost::optional<NamespaceString>& targetedNs,
    const ConfigsvrGetHistoricalPlacement& request) {
    LOGV2(11190100, "(Re)initializing config.placementHistory upon target request failure");
    auto catalogManager = ShardingCatalogManager::get(opCtx);
    auto configShard = catalogManager->localConfigShard();

    // 1. Create the supporting indexes if needed.
    uassertStatusOK(catalogManager->createIndexesForConfigPlacementHistory(opCtx));

    // 2. Re-generate its content.
    ConfigsvrResetPlacementHistory configsvrRequest;
    configsvrRequest.setDbName(DatabaseName::kAdmin);
    configsvrRequest.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
    const auto commandResponse = uassertStatusOK(configShard->runCommandWithIndefiniteRetries(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kAdmin,
        configsvrRequest.toBSON(),
        Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(commandResponse));

    // 3. Re-send the original targeting request.
    try {
        return ShardingCatalogManager::get(opCtx)->getHistoricalPlacement(
            opCtx,
            targetedNs,
            request.getAt(),
            request.getCheckIfPointInTimeIsInFuture(),
            request.getIgnoreRemovedShards());
    } catch (const ExceptionFor<ErrorCodes::PlacementHistoryInitializationMissing>&) {
        // The scenario of observing missing metadata after having successfully completed an
        // initialization request is only expected to happen if the content of
        // config.placementHistory has been tampered between these two steps. React accordingly.
        tasserted(1190101,
                  "config.placementHistory should include initialization metadata documents");
    }
}

class ConfigsvrGetHistoricalPlacementCommand final
    : public TypedCommand<ConfigsvrGetHistoricalPlacementCommand> {
public:
    using Request = ConfigsvrGetHistoricalPlacement;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        ConfigsvrGetHistoricalPlacementResponse typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrGetHistoricalPlacement can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            // Set the operation context read concern level to majority for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);

            boost::optional<NamespaceString> targetedNs = request().getTargetWholeCluster()
                ? (boost::optional<NamespaceString>)boost::none
                : nss;

            try {
                return ShardingCatalogManager::get(opCtx)->getHistoricalPlacement(
                    opCtx,
                    targetedNs,
                    request().getAt(),
                    request().getCheckIfPointInTimeIsInFuture(),
                    request().getIgnoreRemovedShards());
            } catch (const ExceptionFor<ErrorCodes::PlacementHistoryInitializationMissing>&) {
                // Initialize the content of config.placementHistory, then retry.
                return initializePlacementHistoryAndRetry(opCtx, targetedNs, request());
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Allows to run queries concerning historical placement of a namespace in "
               "a controlled way.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrGetHistoricalPlacementCommand).forShard();

}  // namespace
}  // namespace mongo
