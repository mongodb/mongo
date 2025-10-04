/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/clone_authoritative_metadata_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrCloneAuthoritativeMetadataCommand final
    : public TypedCommand<ShardsvrCloneAuthoritativeMetadataCommand> {
public:
    using Request = ShardsvrCloneAuthoritativeMetadata;

    std::string help() const override {
        return "Internal command, do not invoke directly.";
    }

    bool adminOnly() const override {
        return true;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Ensure that the vector clock has been persisted on disk; the DDL coordinator will
            // later issue potentially secondary reads to the config server requesting the last
            // known config time as the "Majority Read Concern" value; such queries could return
            // data that are not causally consistent with the invocation of this command from the
            // config server when 1) the coordinator has not an up-to-date configTime value (for
            // example, as a consequence of recently becoming a primary) and 2) the queries are
            // received by a lagging secondary config server node.
            VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

            auto coordinatorDoc = CloneAuthoritativeMetadataCoordinatorDocument();
            coordinatorDoc.setShardingDDLCoordinatorMetadata(
                {{NamespaceString::kConfigShardCatalogDatabasesNamespace,
                  DDLCoordinatorTypeEnum::kCloneAuthoritativeMetadata}});

            auto service = ShardingDDLCoordinatorService::getService(opCtx);
            auto coordinator = checked_pointer_cast<CloneAuthoritativeMetadataCoordinator>(
                service->getOrCreateInstance(
                    opCtx, coordinatorDoc.toBSON(), FixedFCVRegion{opCtx}));
            coordinator->getCompletionFuture().get(opCtx);
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
MONGO_REGISTER_COMMAND(ShardsvrCloneAuthoritativeMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
