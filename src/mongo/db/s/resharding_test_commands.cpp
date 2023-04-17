/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding_test_commands_gen.h"
#include "mongo/db/vector_clock_metadata_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ReshardingCloneCollectionTestCommand final
    : public TypedCommand<ReshardingCloneCollectionTestCommand> {
public:
    using Request = TestReshardCloneCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // The ReshardingCollectionCloner expects there to already be a Client associated with
            // the thread from the thread pool. We set up the ThreadPoolTaskExecutor identically to
            // how the recipient's primary-only service is set up.
            ThreadPool::Options threadPoolOptions;
            threadPoolOptions.maxThreads = 1;
            threadPoolOptions.threadNamePrefix = "TestReshardCloneCollection-";
            threadPoolOptions.poolName = "TestReshardCloneCollectionThreadPool";
            threadPoolOptions.onCreateThread = [](const std::string& threadName) {
                Client::initThread(threadName.c_str());
                auto* client = Client::getCurrent();
                AuthorizationSession::get(*client)->grantInternalAuthorization(client);
            };

            auto metrics = ReshardingMetrics::makeInstance(
                request().getUuid(),
                request().getShardKey(),
                ns(),
                ReshardingMetrics::Role::kRecipient,
                opCtx->getServiceContext()->getFastClockSource()->now(),
                opCtx->getServiceContext());

            auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
            hookList->addHook(
                std::make_unique<rpc::VectorClockMetadataHook>(opCtx->getServiceContext()));

            auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
                std::make_unique<ThreadPool>(std::move(threadPoolOptions)),
                executor::makeNetworkInterface(
                    "TestReshardCloneCollectionNetwork", nullptr, std::move(hookList)));
            executor->startup();

            ReshardingCollectionCloner cloner(metrics.get(),
                                              ShardKeyPattern(request().getShardKey()),
                                              ns(),
                                              request().getUuid(),
                                              request().getShardId(),
                                              request().getAtClusterTime(),
                                              request().getOutputNs());

            std::shared_ptr<ThreadPool> cancelableOperationContextPool = [] {
                ThreadPool::Options options;
                options.poolName = "TestReshardingCollectionClonerCancelableOpCtxPool";
                options.minThreads = 1;
                options.maxThreads = 1;

                auto threadPool = std::make_shared<ThreadPool>(std::move(options));
                threadPool->startup();
                return threadPool;
            }();

            cloner
                .run(executor,
                     executor,
                     opCtx->getCancellationToken(),
                     CancelableOperationContextFactory(opCtx->getCancellationToken(),
                                                       cancelableOperationContextPool))
                .get(opCtx);
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::internal));
        }
    };


    std::string help() const override {
        return "Internal command for testing resharding collection cloning";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_TEST_COMMAND(ReshardingCloneCollectionTestCommand);

}  // namespace
}  // namespace mongo
