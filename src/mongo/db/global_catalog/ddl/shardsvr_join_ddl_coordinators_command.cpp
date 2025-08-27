/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_ddl_coordinators_request_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
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
        return "Internal command invoked by the config server to join any ShardingDDLCoordinator "
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
                        "Not primary while trying to join ongoing DDL coordinators",
                        repl::ReplicationCoordinator::get(opCtx)->getMemberState().primary());
            }

            const auto& types = request().getTypes();
            IDLParserContext parserContext(Request::kTypesFieldName);

            // Exclude add and remove shard from this since it is used to drain operations for add
            // and remove shard.
            ShardingDDLCoordinatorService::getService(opCtx)->waitForOngoingCoordinatorsToFinish(
                opCtx, [&](const ShardingDDLCoordinator& coordinatorInstance) -> bool {
                    const auto opType = coordinatorInstance.operationType();
                    if (opType == DDLCoordinatorTypeEnum::kRemoveShardCommit ||
                        opType == DDLCoordinatorTypeEnum::kAddShard) {
                        return false;
                    }
                    if (types) {
                        return std::ranges::any_of(*types, [&](StringData type) {
                            return DDLCoordinatorType_parse(type, parserContext) == opType;
                        });
                    }
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
