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
