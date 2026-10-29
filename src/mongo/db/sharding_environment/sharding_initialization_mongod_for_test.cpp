// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/replica_set_aware_service.h"

namespace mongo {
namespace {

/**
 * This is a fake ReplicaSetAwareService meant to be used exclusively for tests. This class exists
 * as a way to insert a fake node into the initialization graph that has the same identifier as the
 * real ShardingInitializationMongoD. All of it's methods are (and should be left to be) noops.
 */
class NoopShardingInitializerForTests
    : public ReplicaSetAwareService<NoopShardingInitializerForTests> {
public:
    static NoopShardingInitializerForTests* get(ServiceContext*) {
        static std::unique_ptr<NoopShardingInitializerForTests> instance;
        if (!instance) {
            instance = std::make_unique<NoopShardingInitializerForTests>();
        }
        return instance.get();
    }

private:
    // Virtual methods coming from the ReplicaSetAwareService
    void onStartup(OperationContext* opCtx) final {}
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}
    void onShutdown() final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final {}
    void onStepUpComplete(OperationContext* opCtx, long long term) final {}
    void onStepDown() final {}
    void onRollbackBegin() final {}
    void onBecomeArbiter() final {}
    inline std::string getServiceName() const final {
        return "ShardingInitializationMongoD";
    }
};

const ReplicaSetAwareServiceRegistry::Registerer<NoopShardingInitializerForTests>
    _registryRegisterer("ShardingInitializationMongoDRegistry");
}  // namespace
}  // namespace mongo
