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


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_test_commands_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/vector_clock/vector_clock_metadata_hook.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ShardsvrReshardingStepDownCommand final
    : public TypedCommand<ShardsvrReshardingStepDownCommand> {
public:
    using Request = ShardsvrReshardingStepDown;

    std::string help() const override {
        return "Test-only command to step down and step up all resharding PrimaryOnlyServices "
               "on this shard. Steps down in order: coordinator, donor, recipient; then steps "
               "up in the same order.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrReshardingStepDown can only run on a shard server",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            auto* registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());

            auto* donorService = static_cast<ReshardingDonorService*>(
                registry->lookupServiceByName(ReshardingDonorService::kServiceName));
            uassert(12755401, "resharding donor service does not exist", donorService);

            auto* recipientService = static_cast<ReshardingRecipientService*>(
                registry->lookupServiceByName(ReshardingRecipientService::kServiceName));
            uassert(12755402, "resharding recipient service does not exist", recipientService);

            ReshardingCoordinatorService* coordinatorService = nullptr;
            if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                coordinatorService = static_cast<ReshardingCoordinatorService*>(
                    registry->lookupServiceByName(ReshardingCoordinatorService::kServiceName));
                uassert(
                    12755403, "resharding coordinator service does not exist", coordinatorService);
            }

            // Try to mimic order in registerPrimaryOnlyServices in mongod_main.cpp.
            if (coordinatorService) {
                coordinatorService->stepDown_forTest();
            }

            donorService->stepDown_forTest();
            recipientService->stepDown_forTest();

            if (coordinatorService) {
                coordinatorService->stepUp_forTest();
            }

            donorService->stepUp_forTest();
            recipientService->stepUp_forTest();
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
MONGO_REGISTER_COMMAND(ShardsvrReshardingStepDownCommand).testOnly().forShard();

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
            auto donorShards = Grid::get(opCtx)->shardRegistry()->getAllShardRefs(opCtx).size();

            auto metrics =
                ReshardingMetrics::makeInstance_forTest(request().getUuid(),
                                                        request().getShardKey(),
                                                        ns(),
                                                        ReshardingMetrics::Role::kRecipient,
                                                        opCtx->fastClockSource().now(),
                                                        opCtx->getServiceContext());

            auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
            hookList->addHook(
                std::make_unique<rpc::VectorClockMetadataHook>(opCtx->getServiceContext()));

            auto executor = executor::ThreadPoolTaskExecutor::create(
                ThreadPool::make({
                    .poolName = "TestReshardCloneCollectionThreadPool",
                    .threadNamePrefix = "TestReshardCloneCollection-",
                    .maxThreads = 1 + 2 * donorShards +
                        resharding::gReshardingCollectionClonerWriteThreadCount.load(),
                    .onCreateThread =
                        [opCtx](const std::string& threadName) {
                            Client::initThread(threadName, opCtx->getService());
                            auto* client = Client::getCurrent();
                            AuthorizationSession::get(*client)->grantInternalAuthorization();
                        },
                }),
                executor::makeNetworkInterface(
                    "TestReshardCloneCollectionNetwork", nullptr, std::move(hookList)));
            executor->startup();

            UUID reshardingUUID =
                request().getReshardingUUID() ? *request().getReshardingUUID() : UUID::gen();
            ReshardingCollectionCloner cloner(metrics.get(),
                                              reshardingUUID,
                                              ShardKeyPattern(request().getShardKey()),
                                              ns(),
                                              request().getUuid(),
                                              request().getShardId(),
                                              request().getAtClusterTime(),
                                              request().getOutputNs(),
                                              true /* storeProgress */,
                                              request().getRelaxed());

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
                     std::make_shared<HierarchicalCancelableOperationContextFactory>(
                         opCtx->getCancellationToken(), executor))
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
MONGO_REGISTER_COMMAND(ReshardingCloneCollectionTestCommand).testOnly().forShard();

}  // namespace
}  // namespace mongo
