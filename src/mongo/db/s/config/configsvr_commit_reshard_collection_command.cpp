// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {


class ConfigsvrCommitReshardCollectionCommand final
    : public TypedCommand<ConfigsvrCommitReshardCollectionCommand> {
public:
    using Request = ConfigsvrCommitReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            uassert(ErrorCodes::IllegalOperation,
                    fmt::format("{} can only be run on config servers", definition()->getName()),
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto sourceNss = resharding::resolveReshardingSourceNss(opCtx, ns());
            UUID reshardingUUID = resharding::retrieveReshardingUUID(opCtx, sourceNss);

            auto machine = resharding::tryGetReshardingStateMachineAndThrowIfShuttingDown<
                ReshardingCoordinatorService,
                ReshardingCoordinator,
                ReshardingCoordinatorDocument>(opCtx, reshardingUUID);

            uassert(ErrorCodes::NoSuchReshardCollection,
                    "Could not find in-progress resharding operation to commit",
                    machine);
            (*machine)->onOkayToEnterCritical();
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
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

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Allows a reshard operation to enter critical section ASAP.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrCommitReshardCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
