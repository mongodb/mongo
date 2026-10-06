// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"

#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state_for_test.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_test_grid.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/scopeguard.h"

#include <memory>

namespace mongo {

class CoordinatorStateDocTest {
public:
    explicit CoordinatorStateDocTest(ShardingCoordinatorMetadata metadata)
        : _metadata(std::move(metadata)) {}

    const ShardingCoordinatorMetadata& getShardingCoordinatorMetadata() const {
        return _metadata;
    }

    void setShardingCoordinatorMetadata(ShardingCoordinatorMetadata newMetadata) {
        _metadata = std::move(newMetadata);
    }

    BSONObj toBSON() const {
        return _metadata.toBSON();
    }

    static inline CoordinatorStateDocTest parseOwned(BSONObj&& bsonObject,
                                                     const IDLParserContext& ctxt) {
        return CoordinatorStateDocTest{
            ShardingCoordinatorMetadata::parseOwned(std::move(bsonObject))};
    }

private:
    ShardingCoordinatorMetadata _metadata;
};

class ShardingDDLCoordinatorTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    static inline const ShardId kTestShardId{"test-shard"};

    ShardingDDLCoordinatorTest() : repl::PrimaryOnlyServiceMongoDTest(makeOptions()) {}

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ShardingCoordinatorService>(
            serviceContext, std::make_unique<ShardingCoordinatorExternalStateFactoryForTest>());
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        ShardingState::get(getServiceContext())
            ->setRecoveryCompleted({OID::gen(),
                                    ClusterRole::ShardServer,
                                    ConnectionString(HostAndPort("localhost", 27017)),
                                    kTestShardId});

        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _network = network.get();
        executor::ThreadPoolMock::Options thread_pool_options;
        thread_pool_options.onCreateThread = [] {
            Client::initThread("ShardingDDLCoordinatorTest",
                               getGlobalServiceContext()->getService());
        };

        _executor = makeThreadPoolTestExecutor(std::move(network), thread_pool_options);
        _executor->startup();

        _scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(_executor);

        DDLLockManager::get(getServiceContext())
            ->setRecoverable(static_cast<ShardingCoordinatorService*>(_service));
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

    class TestShardingDDLCoordinator
        : public NonRecoverableShardingDDLCoordinator<CoordinatorStateDocTest> {
    public:
        TestShardingDDLCoordinator(ShardingCoordinatorService* service,
                                   ShardingCoordinatorMetadata coordinatorMetadata,
                                   std::set<NamespaceString> additionalNss)
            : NonRecoverableShardingDDLCoordinator<CoordinatorStateDocTest>(
                  service, "TestShardingDDLCoordinator", coordinatorMetadata.toBSON()),
              _additionalNss(additionalNss) {}

        ShardingCoordinatorMetadata const& metadata() const override {
            return _doc.getShardingCoordinatorMetadata();
        }

        void setMetadata(ShardingCoordinatorMetadata&& metadata) override {}

        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override {
            return boost::none;
        }

        void checkIfOptionsConflict(const BSONObj& doc) const final {}

        std::set<NamespaceString> _getAdditionalLocksToAcquire(OperationContext* opCtx) override {
            return _additionalNss;
        };

        ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                      const CancellationToken& token) noexcept override {
            return ExecutorFuture<void>(**executor);
        }

        void fulfillPromises() {
            _constructionCompletionPromise.emplaceValue();
            _completionPromise.emplaceValue();
        }

        bool isInCriticalSection(Phase) const override {
            return false;
        }

        using ShardingCoordinator::_acquireLocksAsync;
        using NonRecoverableShardingDDLCoordinator<CoordinatorStateDocTest>::_locker;

    protected:
        std::set<NamespaceString> _additionalNss;
    };

    class ClientObserver : public ServiceContext::ClientObserver {
    public:
        void onCreateClient(Client*) override {}

        void onDestroyClient(Client*) override {}

        void onCreateOperationContext(OperationContext* opCtx) override {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        }

        void onDestroyOperationContext(OperationContext*) override {}
    };  // namespace mongo

    static Options makeOptions() {
        return Options{}.addClientObserver(std::make_unique<ClientObserver>());
    }
};

TEST_F(ShardingDDLCoordinatorTest, AcquiresDDLLocks) {
    auto testDDLLocksAcquired = [&](NamespaceString mainNss,
                                    std::set<NamespaceString> additionalNss,
                                    std::set<DatabaseName> expectedDbLocks,
                                    std::set<NamespaceString> expectedCollLocks) {
        // Create a dummy ShardingCoordinator.
        ShardingCoordinatorMetadata coordinatorMetadata(
            ShardingCoordinatorId(mainNss, CoordinatorTypeEnum::kDropCollection));
        coordinatorMetadata.setForwardableOpMetadata(ForwardableOperationMetadata{});

        auto coordinator = std::make_shared<TestShardingDDLCoordinator>(
            static_cast<ShardingCoordinatorService*>(_service),
            coordinatorMetadata,
            std::set<NamespaceString>({additionalNss}));
        coordinator->fulfillPromises();
        CancellationSource cancellationSource;

        coordinator->_locker = std::make_unique<Locker>(getServiceContext());

        // Just run the '_acquireAllLocksAsync()' bit of ShardingCoordinator::run().
        ExecutorFuture<void>(**_scopedExecutor)
            .then([scopedExecutor = _scopedExecutor,
                   coordinator = coordinator,
                   cancellationToken = cancellationSource.token()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                return coordinator->_acquireLocksAsync(opCtx, scopedExecutor, cancellationToken);
            })
            .get();

        // Always dump the locker for debuggability.
        coordinator->_locker->dump();

        // Check expected locks acquired
        for (const auto& expectedDbLock : expectedDbLocks) {
            ASSERT_TRUE(coordinator->_locker->isLockHeldForMode(
                ResourceId{RESOURCE_DDL_DATABASE, expectedDbLock}, MODE_IX));
        }

        for (const auto& expectedCollLock : expectedCollLocks) {
            ASSERT_TRUE(coordinator->_locker->isLockHeldForMode(
                ResourceId{RESOURCE_DDL_COLLECTION, expectedCollLock}, MODE_X));
        }
    };

    DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(boost::none, "test1");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(dbName1, "foo1");
    NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(dbName1, "foo2");

    DatabaseName dbName2 = DatabaseName::createDatabaseName_forTest(boost::none, "test3");
    NamespaceString nss3 = NamespaceString::createNamespaceString_forTest(dbName2, "foo3");

    // Test with "normal" collections.
    testDDLLocksAcquired(nss1,
                         std::set<NamespaceString>({nss2, nss3}),
                         std::set<DatabaseName>({dbName1, dbName2}),
                         std::set<NamespaceString>({nss1, nss2, nss3}));

    // Test with timeseries buckets collections. Expect locks to be acquired on the corresponding
    // view nss instead.
    testDDLLocksAcquired(nss1.makeTimeseriesBucketsNamespace(),
                         std::set<NamespaceString>({nss2.makeTimeseriesBucketsNamespace(),
                                                    nss3.makeTimeseriesBucketsNamespace()}),
                         std::set<DatabaseName>({dbName1, dbName2}),
                         std::set<NamespaceString>({nss1, nss2, nss3}));
}

TEST_F(ShardingDDLCoordinatorTest, NonAuthoritativeDDLRetriesOnFCVTransition) {
    auto nss = NamespaceString::createNamespaceString_forTest("test", "foo");

    // Create a non-authoritative DDL coordinator
    unittest::ServerParameterGuard disableAuthDDL{"featureFlagAuthoritativeShardsDDL", false};
    unittest::ServerParameterGuard disableAuthCRUD{"featureFlagAuthoritativeShardsCRUD", false};

    ShardingCoordinatorMetadata coordinatorMetadata(
        ShardingCoordinatorId(nss, CoordinatorTypeEnum::kDropCollection));
    coordinatorMetadata.setForwardableOpMetadata(ForwardableOperationMetadata{});
    coordinatorMetadata.setAuthoritativeMetadataAccessLevel(
        AuthoritativeMetadataAccessLevelEnum::kNone);
    coordinatorMetadata.setDatabaseVersion(DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto coordinator = std::make_shared<TestShardingDDLCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service),
        coordinatorMetadata,
        std::set<NamespaceString>{});

    // Run the coordinator with authoritative shards enabled and test it asks for a retry.
    unittest::ServerParameterGuard enableAuthDDL{"featureFlagAuthoritativeShardsDDL", true};
    auto future = static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
                      ->run(_scopedExecutor, CancellationToken::uncancelable());
    ASSERT_THROWS_CODE(
        future.get(), DBException, ErrorCodes::DDLCoordinatorMustRetryDueToFCVTransition);
}

std::shared_ptr<executor::TaskExecutor> makeMultiThreadTestExecutor() {
    auto executor = executor::ThreadPoolTaskExecutor::create(
        ThreadPool::make({
            .poolName = "ShardingDDLCoordinatorIndexBuildTest",
            .threadNamePrefix = "ShardingDDLCoordinatorIndexBuildTest-",
            .minThreads = 2,
            .maxThreads = 4,
            .onCreateThread =
                [](const std::string& threadName) {
                    Client::initThread(threadName, getGlobalServiceContext()->getService());
                },
        }),
        executor::makeNetworkInterface("ShardingDDLCoordinatorIndexBuildTest"));
    executor->startup();
    return executor;
}

ShardingCoordinatorMetadata makeAuthoritativeMetadata(const NamespaceString& nss,
                                                      CoordinatorTypeEnum type) {
    ShardingCoordinatorMetadata coordinatorMetadata(ShardingCoordinatorId(nss, type));
    coordinatorMetadata.setForwardableOpMetadata(ForwardableOperationMetadata{});
    coordinatorMetadata.setAuthoritativeMetadataAccessLevel(
        AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed);
    coordinatorMetadata.setDatabaseVersion(DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    return coordinatorMetadata;
}

ShardingCoordinatorMetadata makeAuthoritativeDropCollectionMetadata(const NamespaceString& nss) {
    return makeAuthoritativeMetadata(nss, CoordinatorTypeEnum::kDropCollection);
}

std::set<std::string> listIndexesTargets(const IndexBuildCommandLog& log) {
    std::set<std::string> targets;
    for (const auto& cmd : log.commands()) {
        if (cmd.name == "listIndexes") {
            targets.insert(cmd.host + " " + cmd.db + "." + cmd.coll);
        }
    }
    return targets;
}

TEST_F(ShardingDDLCoordinatorTest, FailsEarlyWhenIndexBuildInProgress) {
    auto opCtx = makeOperationContext();
    ShardingCoordinatorTestGrid grid(opCtx.get(), IndexBuildProgress::kInProgress);

    auto nss = NamespaceString::createNamespaceString_forTest("test", "foo");
    auto coordinator = std::make_shared<ShardingDDLCoordinatorTest::TestShardingDDLCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service),
        makeAuthoritativeDropCollectionMetadata(nss),
        std::set<NamespaceString>{});

    auto executor = makeMultiThreadTestExecutor();
    auto scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(executor);
    ScopeGuard shutdownExecutor([&] {
        scopedExecutor.reset();
        executor->shutdown();
        executor->join();
    });
    auto future = static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
                      ->run(scopedExecutor, CancellationToken::uncancelable());
    ASSERT_THROWS_CODE(
        future.get(), DBException, ErrorCodes::BackgroundOperationInProgressForNamespace);
}

TEST_F(ShardingDDLCoordinatorTest, ContinuesWhenNoIndexBuildInProgress) {
    auto opCtx = makeOperationContext();
    ShardingCoordinatorTestGrid grid(opCtx.get(), IndexBuildProgress::kNone);

    auto nss = NamespaceString::createNamespaceString_forTest("test", "foo");
    auto coordinator = std::make_shared<ShardingDDLCoordinatorTest::TestShardingDDLCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service),
        makeAuthoritativeDropCollectionMetadata(nss),
        std::set<NamespaceString>{});

    auto executor = makeMultiThreadTestExecutor();
    auto scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(executor);
    ScopeGuard shutdownExecutor([&] {
        scopedExecutor.reset();
        executor->shutdown();
        executor->join();
    });
    auto future = static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
                      ->run(scopedExecutor, CancellationToken::uncancelable());
    ASSERT_OK(future.getNoThrow());
}

TEST_F(ShardingDDLCoordinatorTest, DatabaseCheckQueriesOnlyShardsThatReportedTheCollection) {
    auto opCtx = makeOperationContext();
    auto commandLog = std::make_shared<IndexBuildCommandLog>();
    IndexBuildCheckOptions options;
    options.shardNames = {"shard0", "shard1"};
    options.collectionsByHost = {{"shard0", {"collA"}}, {"shard1", {"collB"}}};
    options.viewsByHost = {{"shard0", {"collView"}}};
    options.commandLog = commandLog;
    ShardingCoordinatorTestGrid grid(opCtx.get(), options);

    auto nss = NamespaceString::createNamespaceString_forTest(
        DatabaseName::createDatabaseName_forTest(boost::none, "test"));
    ASSERT_TRUE(nss.isDbOnly());
    auto coordinator = std::make_shared<ShardingDDLCoordinatorTest::TestShardingDDLCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service),
        makeAuthoritativeMetadata(nss, CoordinatorTypeEnum::kDropDatabase),
        std::set<NamespaceString>{});

    auto executor = makeMultiThreadTestExecutor();
    auto scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(executor);
    ScopeGuard shutdownExecutor([&] {
        scopedExecutor.reset();
        executor->shutdown();
        executor->join();
    });
    auto future = static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
                      ->run(scopedExecutor, CancellationToken::uncancelable());
    ASSERT_OK(future.getNoThrow());

    ASSERT_EQ(listIndexesTargets(*commandLog),
              (std::set<std::string>{"shard0 test.collA", "shard1 test.collB"}));
}

TEST_F(ShardingDDLCoordinatorTest, DatabaseCheckStillFailsWhenOwningShardHasIndexBuild) {
    auto opCtx = makeOperationContext();
    auto commandLog = std::make_shared<IndexBuildCommandLog>();
    IndexBuildCheckOptions options;
    options.shardNames = {"shard0", "shard1"};
    options.collectionsByHost = {{"shard0", {"collA"}}, {"shard1", {"collB"}}};
    options.namespacesWithIndexBuild = {"test.collA"};
    options.commandLog = commandLog;
    ShardingCoordinatorTestGrid grid(opCtx.get(), options);

    auto nss = NamespaceString::createNamespaceString_forTest(
        DatabaseName::createDatabaseName_forTest(boost::none, "test"));
    auto coordinator = std::make_shared<ShardingDDLCoordinatorTest::TestShardingDDLCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service),
        makeAuthoritativeMetadata(nss, CoordinatorTypeEnum::kDropDatabase),
        std::set<NamespaceString>{});

    auto executor = makeMultiThreadTestExecutor();
    auto scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(executor);
    ScopeGuard shutdownExecutor([&] {
        scopedExecutor.reset();
        executor->shutdown();
        executor->join();
    });
    auto future = static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
                      ->run(scopedExecutor, CancellationToken::uncancelable());
    ASSERT_THROWS_CODE(
        future.get(), DBException, ErrorCodes::BackgroundOperationInProgressForNamespace);

    const auto targets = listIndexesTargets(*commandLog);
    ASSERT_TRUE(targets.contains("shard0 test.collA"));
    ASSERT_FALSE(targets.contains("shard1 test.collA"));
}

TEST_F(ShardingDDLCoordinatorTest, ConfigNamespaceIsCheckedOnTheConfigServer) {
    auto opCtx = makeOperationContext();
    auto commandLog = std::make_shared<IndexBuildCommandLog>();
    IndexBuildCheckOptions options;
    options.shardNames = {"shard0", "shard1"};
    options.indexBuilds = IndexBuildProgress::kInProgress;
    options.commandLog = commandLog;
    ShardingCoordinatorTestGrid grid(opCtx.get(), options);

    auto nss = NamespaceString::kConfigsvrPlacementHistoryNamespace;
    auto coordinator = std::make_shared<ShardingDDLCoordinatorTest::TestShardingDDLCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service),
        makeAuthoritativeMetadata(nss, CoordinatorTypeEnum::kDropCollection),
        std::set<NamespaceString>{});

    auto executor = makeMultiThreadTestExecutor();
    auto scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(executor);
    ScopeGuard shutdownExecutor([&] {
        scopedExecutor.reset();
        executor->shutdown();
        executor->join();
    });
    auto future = static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
                      ->run(scopedExecutor, CancellationToken::uncancelable());
    auto status = future.getNoThrow();
    ASSERT_EQ(listIndexesTargets(*commandLog),
              (std::set<std::string>{"config config.placementHistory"}));
    ASSERT_EQ(status.code(), ErrorCodes::BackgroundOperationInProgressForNamespace);
}

TEST_F(ShardingDDLCoordinatorTest, CollectionCheckStillContactsEveryShard) {
    auto opCtx = makeOperationContext();
    auto commandLog = std::make_shared<IndexBuildCommandLog>();
    IndexBuildCheckOptions options;
    options.shardNames = {"shard0", "shard1"};
    options.commandLog = commandLog;
    ShardingCoordinatorTestGrid grid(opCtx.get(), options);

    auto nss = NamespaceString::createNamespaceString_forTest("test", "foo");
    auto coordinator = std::make_shared<ShardingDDLCoordinatorTest::TestShardingDDLCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service),
        makeAuthoritativeDropCollectionMetadata(nss),
        std::set<NamespaceString>{});

    auto executor = makeMultiThreadTestExecutor();
    auto scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(executor);
    ScopeGuard shutdownExecutor([&] {
        scopedExecutor.reset();
        executor->shutdown();
        executor->join();
    });
    auto future = static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
                      ->run(scopedExecutor, CancellationToken::uncancelable());
    ASSERT_OK(future.getNoThrow());

    ASSERT_EQ(listIndexesTargets(*commandLog),
              (std::set<std::string>{"shard0 test.foo", "shard1 test.foo"}));
    for (const auto& cmd : commandLog->commands()) {
        ASSERT_NE(cmd.name, "listCollections");
    }
}

}  // namespace mongo
