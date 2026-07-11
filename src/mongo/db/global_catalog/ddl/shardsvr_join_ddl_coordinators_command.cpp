// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_ddl_coordinators_request_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <string_view>
#include <type_traits>

namespace mongo {
namespace {
class ShardsvrJoinDDLCoordinatorsCommand final
    : public TypedCommand<ShardsvrJoinDDLCoordinatorsCommand> {
public:
    using Request = ShardsvrJoinDDLCoordinators;

    bool skipApiVersionCheck() const override {
        // Internal command (config -> shard).
        return true;
    }

    std::string help() const override {
        return "Internal command invoked by the config server to join any ShardingCoordinator "
               "activity other than add and remove shard executed by the shard";
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
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            {
                Lock::GlobalLock lk(opCtx, MODE_IX);
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "Not primary while trying to join ongoing coordinators",
                        repl::ReplicationCoordinator::get(opCtx)->getMemberState().primary());
            }

            const auto& types = request().getTypes();
            IDLParserContext parserContext(Request::kTypesFieldName);

            ShardingCoordinatorService::getService(opCtx)->waitForOngoingCoordinatorsToFinish(
                opCtx, [&](const ShardingCoordinator& coordinatorInstance) -> bool {
                    const auto opType = coordinatorInstance.operationType();
                    // Disregard DDL types that use this command as part of their workflow.
                    if (opType == CoordinatorTypeEnum::kRemoveShardCommit ||
                        opType == CoordinatorTypeEnum::kAddShard ||
                        opType == CoordinatorTypeEnum::kInitializePlacementHistory) {
                        return false;
                    }
                    // If the submitter specified a subset of types, only join those.
                    if (types) {
                        return std::ranges::any_of(*types, [&](std::string_view type) {
                            return idl::deserialize<CoordinatorTypeEnum>(type, parserContext) ==
                                opType;
                        });
                    }
                    // Join all other types.
                    return true;
                });

            // Before leaving, we have to ensure that this node is not operating in a split-brain
            // scenario (where another primary node could be serving DDL operations that cannot be
            // drained within this context); a majority dummy write is performed here to persist the
            // session ID and TXN number received by the caller (and allowing the execution of the
            // replay protection check).
            DBDirectClient dbClient(opCtx);
            dbClient.update(NamespaceString::kServerConfigurationNamespace,
                            BSON("_id" << Request::kCommandName),
                            BSON("$inc" << BSON("count" << 1)),
                            true /* upsert */,
                            false /* multi */);

            WriteConcernResult ignoreResult;
            auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            uassertStatusOK(waitForWriteConcern(
                opCtx, latestOpTime, defaultMajorityWriteConcern(), &ignoreResult));
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
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
};
MONGO_REGISTER_COMMAND(ShardsvrJoinDDLCoordinatorsCommand).forShard();

}  // namespace
}  // namespace mongo
