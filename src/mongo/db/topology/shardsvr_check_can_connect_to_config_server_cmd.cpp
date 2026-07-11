// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
                ThreadPool::make({}), executor::makeNetworkInterface("HelloMe-TaskExecutor"));
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
