// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/local_executor.h"

#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

namespace {

const auto localExecutor =
    ServiceContext::declareDecoration<std::shared_ptr<executor::TaskExecutor>>();

}

std::shared_ptr<executor::TaskExecutor> getLocalExecutor(ServiceContext* service) {
    auto executor = localExecutor(service);
    invariant(executor);
    return executor;
}

std::shared_ptr<executor::TaskExecutor> getLocalExecutor(OperationContext* opCtx) {
    return getLocalExecutor(opCtx->getServiceContext());
}

void setLocalExecutor(ServiceContext* service, std::shared_ptr<executor::TaskExecutor> executor) {
    localExecutor(service) = std::move(executor);
}

std::shared_ptr<executor::TaskExecutor> createLocalExecutor(ServiceContext* serviceContext,
                                                            const std::string& name) {
    class BlockerHook : public rpc::EgressMetadataHook {
    public:
        Status writeRequestMetadata(OperationContext*, BSONObjBuilder*) override {
            tasserted(ErrorCodes::IllegalOperation,
                      "Remote task is prohibited to schedule on local task executor");
        }
        Status readReplyMetadata(OperationContext*, const BSONObj&) override {
            tasserted(ErrorCodes::IllegalOperation,
                      "Remote task is prohibited to schedule on local task executor");
        }
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<BlockerHook>());

    return executor::ThreadPoolTaskExecutor::create(
        ThreadPool::make({
            .poolName = fmt::format("{}ThreadPool", name),
            .threadNamePrefix = fmt::format("{}-", name),
            .maxThreads = ThreadPool::Options::kUnlimited,
            .onCreateThread =
                [serviceContext](const std::string& threadName) {
                    Client::initThread(threadName,
                                       serviceContext->getService(),
                                       Client::noSession(),
                                       ClientOperationKillableByStepdown{false});
                },
        }),
        executor::makeNetworkInterface(name + "Network", nullptr, std::move(hookList)));
}

}  // namespace mongo
