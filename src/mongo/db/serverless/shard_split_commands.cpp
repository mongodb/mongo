/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/db/serverless/shard_split_commands_gen.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {
using namespace fmt::literals;

class CommitShardSplitCmd : public TypedCommand<CommitShardSplitCmd> {
public:
    using Request = CommitShardSplit;
    using Response = CommitShardSplitResponse;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "Shard split is not available on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::None) ||
                        serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
            uassert(ErrorCodes::CommandNotSupported,
                    "Shard split is only supported in serverless mode",
                    getGlobalReplSettings().isServerless());

            const auto& cmd = request();
            auto stateDoc = ShardSplitDonorDocument(cmd.getMigrationId());
            stateDoc.setTenantIds(cmd.getTenantIds());
            stateDoc.setRecipientTagName(cmd.getRecipientTagName());
            stateDoc.setRecipientSetName(cmd.getRecipientSetName());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            auto donorService = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                                    ->lookupServiceByName(ShardSplitDonorService::kServiceName);

            auto donorPtr = ShardSplitDonorService::DonorStateMachine::getOrCreate(
                opCtx, donorService, stateDoc.toBSON());
            invariant(donorPtr);

            auto state = donorPtr->decisionFuture().get(opCtx);

            uassert(ErrorCodes::TenantMigrationAborted,
                    "The shard split operation was aborted. " +
                        (state.abortReason ? state.abortReason->toString() : ""),
                    state.state != ShardSplitDonorStateEnum::kAborted);

            Response response;
            invariant(state.blockOpTime.has_value());
            response.setBlockOpTime(*state.blockOpTime);

            return response;
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    std::string help() const override {
        return "Start an operation to split a shard into its own slice.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(CommitShardSplitCmd).forShard();

class AbortShardSplitCmd : public TypedCommand<AbortShardSplitCmd> {
public:
    using Request = AbortShardSplit;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "Shard split is only supported in serverless mode",
                    getGlobalReplSettings().isServerless());

            const RequestType& cmd = request();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            auto splitService = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                                    ->lookupServiceByName(ShardSplitDonorService::kServiceName);
            auto instance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
                opCtx,
                splitService,
                BSON("_id" << cmd.getMigrationId() << ShardSplitDonorDocument::kStateFieldName
                           << ShardSplitDonorState_serializer(ShardSplitDonorStateEnum::kAborted)),
                false);

            invariant(instance);

            instance->tryAbort();

            auto state = instance->decisionFuture().get(opCtx);

            uassert(ErrorCodes::CommandFailed,
                    "Failed to abort shard split",
                    state.abortReason &&
                        state.abortReason.value() == ErrorCodes::TenantMigrationAborted);

            uassert(ErrorCodes::TenantMigrationCommitted,
                    "Failed to abort : shard split already committed",
                    state.state == ShardSplitDonorStateEnum::kAborted);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    std::string help() const override {
        return "Abort a shard split operation.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(AbortShardSplitCmd).forShard();

class ForgetShardSplitCmd : public TypedCommand<ForgetShardSplitCmd> {
public:
    using Request = ForgetShardSplit;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "Shard split is only supported in serverless mode",
                    getGlobalReplSettings().isServerless());

            const RequestType& cmd = request();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            auto splitService = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                                    ->lookupServiceByName(ShardSplitDonorService::kServiceName);
            auto [optionalDonor, _] = ShardSplitDonorService::DonorStateMachine::lookup(
                opCtx, splitService, BSON("_id" << cmd.getMigrationId()));

            uassert(ErrorCodes::NoSuchTenantMigration,
                    str::stream() << "Could not find shard split with id " << cmd.getMigrationId(),
                    optionalDonor);

            auto donorPtr = optionalDonor.value();

            auto decision = donorPtr->decisionFuture().get(opCtx);

            uassert(
                ErrorCodes::TenantMigrationInProgress,
                "Could not forget migration with id {} since no decision has been made yet"_format(
                    cmd.getMigrationId().toString()),
                decision.state == ShardSplitDonorStateEnum::kCommitted ||
                    decision.state == ShardSplitDonorStateEnum::kAborted);

            donorPtr->tryForget();
            donorPtr->garbageCollectableFuture().get(opCtx);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    std::string help() const override {
        return "Forget the shard split operation.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ForgetShardSplitCmd).forShard();


}  // namespace
}  // namespace mongo
