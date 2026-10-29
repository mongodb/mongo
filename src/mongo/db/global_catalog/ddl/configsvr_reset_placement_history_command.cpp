// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/ddl/initialize_placement_history_coordinator.h"
#include "mongo/db/global_catalog/ddl/initialize_placement_history_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/placement_history_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class ConfigSvrResetPlacementHistoryCommand final
    : public TypedCommand<ConfigSvrResetPlacementHistoryCommand> {
public:
    using Request = ConfigsvrResetPlacementHistory;

    std::string help() const override {
        return "Internal command only invokable on the config server. Do not call directly. "
               "Reinitializes the content of config.placementHistory based on a recent snapshot of "
               "the Sharding catalog.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName
                                  << " can only be run on the config server",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            auto initializePlacementHistoryCoordinator = [&] {
                FixedFCVRegion fcvRegion(opCtx);

                // The Operation FCV is currently propagated only for DDL operations,
                // which cannot be nested. Therefore, the VersionContext shouldn't have an OFCV yet.
                // TODO Revisit this invariant once this workflow gets integrated into addShard.
                invariant(!VersionContext::getDecoration(opCtx).hasOperationFCV());

                if (const auto fcvSnapshot = fcvRegion->acquireFCVSnapshot();
                    !feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting.isEnabled(
                        VersionContext::getDecoration(opCtx), fcvSnapshot)) {
                    uasserted(
                        ErrorCodes::CommandNotSupported,
                        "Unable to initialize config.placementHistory under the currently active "
                        "FCV version");
                }

                InitializePlacementHistoryCoordinatorDocument coordinatorDoc;
                coordinatorDoc.setShardingCoordinatorMetadata(
                    {{NamespaceString::kConfigsvrPlacementHistoryNamespace,
                      CoordinatorTypeEnum::kInitializePlacementHistory}});

                auto service = ShardingCoordinatorService::getService(opCtx);
                return checked_pointer_cast<InitializePlacementHistoryCoordinator>(
                    service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON(), fcvRegion));
            }();

            initializePlacementHistoryCoordinator->getCompletionFuture().get(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return true;
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
};
MONGO_REGISTER_COMMAND(ConfigSvrResetPlacementHistoryCommand).forShard();

}  // namespace
}  // namespace mongo
