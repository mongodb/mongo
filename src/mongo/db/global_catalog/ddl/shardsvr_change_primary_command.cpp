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

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/move_primary_coordinator.h"
#include "mongo/db/global_catalog/ddl/move_primary_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/move_primary_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>
#include <utility>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrChangePrimaryCommand final : public TypedCommand<ShardsvrChangePrimaryCommand> {
public:
    using Request = ShardsvrChangePrimary;

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
                    fmt::format("cannot change primary of internal database {}",
                                dbNss.toStringForErrorMsg()),
                    !dbNss.isOnInternalDb());

            sharding_ddl_util::assertDataMovementAllowed();

            ScopeGuard onBlockExit(
                [&] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbNss.dbName()); });

            const auto coordinatorFuture = [&] {
                FixedFCVRegion fcvRegion(opCtx);

                // The Operation FCV is currently propagated only for DDL operations,
                // which cannot be nested. Therefore, the VersionContext shouldn't have
                // been initialized yet.
                invariant(!VersionContext::getDecoration(opCtx).isInitialized());
                const auto authoritativeMetadataAccessLevel =
                    sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                        VersionContext::getDecoration(opCtx), fcvRegion->acquireFCVSnapshot());

                // TODO (SERVER-76436): Remove once 8.0 becomes last LTS.
                uassert(
                    ErrorCodes::IllegalOperation,
                    "Cannot run changePrimary with featureFlagBalanceUnshardedCollections disabled",
                    feature_flags::gBalanceUnshardedCollections.isEnabled(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

                auto shardRegistry = Grid::get(opCtx)->shardRegistry();
                // Ensure that the shard information is up-to-date as possible to catch the case
                // where a shard with the same name, but with a different host, has been
                // removed/re-added.
                shardRegistry->reload(opCtx);
                const auto toShard = uassertStatusOKWithContext(
                    shardRegistry->getShard(opCtx, toShardId),
                    fmt::format("requested primary shard {} does not exist", toShardId.toString()));

                auto coordinatorDoc = [&] {
                    MovePrimaryCoordinatorDocument doc;
                    doc.setShardingDDLCoordinatorMetadata(
                        {{dbNss, DDLCoordinatorTypeEnum::kMovePrimary}});
                    doc.setToShardId(toShard->getId());
                    doc.setAuthoritativeMetadataAccessLevel(authoritativeMetadataAccessLevel);
                    return doc.toBSON();
                }();

                const auto coordinator = [&] {
                    auto service = ShardingDDLCoordinatorService::getService(opCtx);
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
MONGO_REGISTER_COMMAND(ShardsvrChangePrimaryCommand).forShard();

}  // namespace
}  // namespace mongo
