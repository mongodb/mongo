// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/move_primary_coordinator.h"
#include "mongo/db/global_catalog/ddl/move_primary_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/move_primary_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#include <string>
#include <utility>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrMovePrimaryCommand final : public TypedCommand<ShardsvrMovePrimaryCommand> {
public:
    using Request = ShardsvrMovePrimary;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto& dbNss = ns();
            const auto& toShardId = request().getTo();

            uassert(
                ErrorCodes::InvalidNamespace,
                fmt::format("invalid database {}", dbNss.toStringForErrorMsg()),
                DatabaseName::isValid(dbNss.dbName(), DatabaseName::DollarInDbNameBehavior::Allow));

            uassert(ErrorCodes::InvalidOptions,
                    fmt::format("cannot move primary of internal database {}",
                                dbNss.toStringForErrorMsg()),
                    !dbNss.isOnInternalDb());

            sharding_ddl_util::assertDataMovementAllowed();

            ScopeGuard onBlockExit(
                [&] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbNss.dbName()); });

            const auto coordinatorFuture = [&] {
                FixedFCVRegion fcvRegion(opCtx);

                // The Operation FCV is currently propagated only for DDL operations,
                // which cannot be nested. Therefore, the VersionContext shouldn't have an OFCV yet.
                invariant(!VersionContext::getDecoration(opCtx).hasOperationFCV());

                auto shardRegistry = Grid::get(opCtx)->shardRegistry();
                // Ensure that the shard information is up-to-date as possible to catch the case
                // where a shard with the same name, but with a different host, has been
                // removed/re-added.
                shardRegistry->reload(opCtx);
                const auto resolvedToShardId = uassertStatusOKWithContext(
                    shardRegistry->resolveShardId(
                        opCtx, toShardId, true /* allowNonShardIdIdentifiers */),
                    fmt::format("requested primary shard {} does not exist", toShardId.toString()));

                auto coordinatorDoc = [&] {
                    MovePrimaryCoordinatorDocument doc;
                    doc.setShardingCoordinatorMetadata(
                        {{dbNss, CoordinatorTypeEnum::kMovePrimary}});
                    doc.setToShardId(resolvedToShardId);
                    return doc.toBSON();
                }();

                const auto coordinator = [&] {
                    auto service = ShardingCoordinatorService::getService(opCtx);
                    return checked_pointer_cast<MovePrimaryCoordinator>(
                        service->getOrCreateInstance(opCtx, std::move(coordinatorDoc), fcvRegion));
                }();

                return coordinator->getCompletionFuture();
            }();

            coordinatorFuture.get(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getCommandParameter());
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

private:
    bool adminOnly() const override {
        return true;
    }

    bool skipApiVersionCheck() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }
};
MONGO_REGISTER_COMMAND(ShardsvrMovePrimaryCommand).forShard();

}  // namespace
}  // namespace mongo
