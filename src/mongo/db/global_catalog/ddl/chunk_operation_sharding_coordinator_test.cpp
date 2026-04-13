/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/chunk_operation_sharding_coordinator.h"

#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state_for_test.h"
#include "mongo/db/global_catalog/ddl/test_chunk_operation_sharding_coordinator_document_gen.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"

#include <memory>

namespace mongo {

class ClientObserver : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client*) override {}

    void onDestroyClient(Client*) override {}

    void onCreateOperationContext(OperationContext* opCtx) override {
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    }

    void onDestroyOperationContext(OperationContext*) override {}
};

class ChunkOperationShardingCoordinatorTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    ChunkOperationShardingCoordinatorTest()
        : repl::PrimaryOnlyServiceMongoDTest(
              Options{}.addClientObserver(std::make_unique<ClientObserver>())),
          _externalState(std::make_shared<ShardingCoordinatorExternalStateForTest>()) {}

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        auto externalStateFactory =
            std::make_unique<ShardingCoordinatorExternalStateFactoryForTest>(_externalState);
        return std::make_unique<ShardingCoordinatorService>(serviceContext,
                                                            std::move(externalStateFactory));
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();
        _opCtx = cc().getOperationContext();
        if (!_opCtx) {
            _opCtxHolder = cc().makeOperationContext();
            _opCtx = _opCtxHolder.get();
        }

        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _network = network.get();
        executor::ThreadPoolMock::Options thread_pool_options;
        thread_pool_options.onCreateThread = [] {
            Client::initThread("ChunkOperationShardingCoordinatorTest",
                               getGlobalServiceContext()->getService());
        };

        _executor = makeThreadPoolTestExecutor(std::move(network), thread_pool_options);
        _executor->startup();

        _scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(_executor);
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
        _executor.reset();

        repl::PrimaryOnlyServiceMongoDTest::tearDown();
    }

protected:
    executor::NetworkInterfaceMock* _network;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;
    std::shared_ptr<ShardingCoordinatorExternalStateForTest> _externalState;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;

    class TestChunkOperationShardingCoordinator
        : public ChunkOperationShardingCoordinator<TestChunkOperationShardingCoordinatorDocument> {
    public:
        TestChunkOperationShardingCoordinator(
            ShardingCoordinatorService* service,
            TestChunkOperationShardingCoordinatorDocument coordinatorMetadata)
            : ChunkOperationShardingCoordinator<TestChunkOperationShardingCoordinatorDocument>(
                  service, "TestChunkOperationShardingCoordinator", coordinatorMetadata.toBSON()) {}

        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override {
            return boost::none;
        }

        void checkIfOptionsConflict(const BSONObj& doc) const final {}

        ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                      const CancellationToken& token) noexcept override {
            return ExecutorFuture<void>(**executor);
        }

        bool isInCriticalSection(Phase phase) const override {
            return false;
        }
    };
};

TEST_F(ChunkOperationShardingCoordinatorTest, SmokeTest) {
    CancellationSource cancellationSource;

    TestChunkOperationShardingCoordinatorDocument doc;
    ShardingCoordinatorMetadata coorMetadata{
        {NamespaceString::createNamespaceString_forTest("test"),
         CoordinatorTypeEnum::kTestCoordinator}};

    ForwardableOperationMetadata forwardableOpMetadata(_opCtx);
    coorMetadata.setForwardableOpMetadata(forwardableOpMetadata);

    doc.setShardingCoordinatorMetadata(std::move(coorMetadata));

    auto coordinator = std::make_shared<TestChunkOperationShardingCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), std::move(doc));
    auto future = (static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get()))
                      ->run(_scopedExecutor, cancellationSource.token());
    future.get();
}

}  // namespace mongo
