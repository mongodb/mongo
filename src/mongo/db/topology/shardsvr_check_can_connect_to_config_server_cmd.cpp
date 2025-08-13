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
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/db/topology/add_shard_gen.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrCheckCanConnectToConfigServerCommand
    : public TypedCommand<ShardsvrCheckCanConnectToConfigServerCommand> {
public:
    using Request = ShardsvrCheckCanConnectToConfigServer;

    ShardsvrCheckCanConnectToConfigServerCommand() : TypedCommand(Request::kCommandName) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            HelloCommand helloCmd;
            helloCmd.setDbName(DatabaseName::kAdmin);

            ConnectionString cstr(request().getCommandParameter());

            auto taskExecutor = executor::ThreadPoolTaskExecutor::create(
                std::make_unique<ThreadPool>(ThreadPool::Options{}),
                executor::makeNetworkInterface("HelloMe-TaskExecutor"));
            taskExecutor->startup();

            auto options = std::make_shared<async_rpc::AsyncRPCOptions<HelloCommand>>(
                taskExecutor, opCtx->getCancellationToken(), helloCmd);

            try {
                async_rpc::sendCommand<HelloCommand>(options, opCtx, cstr).get();
            } catch (const ExceptionFor<ErrorCodes::RemoteCommandExecutionError>& ex) {
                uassertStatusOK(async_rpc::unpackRPCStatus(ex.toStatus()));
            }
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        // The command parameter happens to be string so it's historically been interpreted
        // by parseNs as a collection. Continuing to do so here for unexamined compatibility.
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
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
        return "Internal command, that tries to contact the host specified in the parameter.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ShardsvrCheckCanConnectToConfigServerCommand).forShard();

}  // namespace
}  // namespace mongo
