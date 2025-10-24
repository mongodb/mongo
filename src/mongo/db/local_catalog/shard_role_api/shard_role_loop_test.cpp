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

#include "mongo/db/local_catalog/shard_role_api/shard_role_loop.h"

#include "mongo/db/global_catalog/catalog_cache/catalog_cache_mock.h"
#include "mongo/db/global_catalog/catalog_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader_mock.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_api_d_params_gen.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"

#include <memory>
#include <queue>

#include "src/mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "src/mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "src/mongo/db/service_context_test_fixture.h"
#include "src/mongo/db/versioning_protocol/stale_exception.h"

namespace mongo {

using shard_role_loop::CanRetry;
using shard_role_loop::handleStaleError;
using shard_role_loop::withStaleShardRetry;

namespace {

const DatabaseName kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "dbname");
const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest(kDbName, "coll");
const ShardId kTestShardId = ShardId("shard0000");

class StaleShardExceptionHandlerMock : public StaleShardCollectionMetadataHandler,
                                       public StaleShardDatabaseMetadataHandler {
public:
    virtual ~StaleShardExceptionHandlerMock() = default;

    boost::optional<DatabaseVersion> handleStaleDatabaseVersionException(
        OperationContext* opCtx, const StaleDbRoutingVersion& staleDbException) const override {
        ++_numDbVersionCalls;
        return std::visit(
            OverloadedVisitor{
                [&](const ReturnReceivedVersion&) {
                    return boost::optional<DatabaseVersion>(staleDbException.getVersionReceived());
                },
                [&](const boost::optional<DatabaseVersion>& dbVersion) { return dbVersion; },
            },
            _handleStaleDatabaseVersionExceptionRet);
    }

    boost::optional<ChunkVersion> handleStaleShardVersionException(
        OperationContext* opCtx, const StaleConfigInfo& sci) const override {
        ++_numShardVersionCalls;
        return std::visit(
            OverloadedVisitor{
                [&](const ReturnReceivedVersion&) {
                    return boost::optional<ChunkVersion>(
                        sci.getVersionReceived().placementVersion());
                },
                [&](const boost::optional<ChunkVersion>& chunkVersion) { return chunkVersion; },
            },
            _handleStaleShardVersionExceptionRet);
    }

    void clear() {
        _numDbVersionCalls = 0;
        _numShardVersionCalls = 0;
    }

    mutable int _numDbVersionCalls{0};
    mutable int _numShardVersionCalls{0};

    class ReturnReceivedVersion {};
    mutable std::variant<ReturnReceivedVersion, boost::optional<DatabaseVersion>>
        _handleStaleDatabaseVersionExceptionRet = ReturnReceivedVersion{};
    mutable std::variant<ReturnReceivedVersion, boost::optional<ChunkVersion>>
        _handleStaleShardVersionExceptionRet = ReturnReceivedVersion{};
};

class CollectionShardingStateFactoryMock : public CollectionShardingStateFactory {
public:
    CollectionShardingStateFactoryMock(
        std::shared_ptr<StaleShardExceptionHandlerMock> staleExceptionHandler)
        : _staleExceptionHandler(staleExceptionHandler) {}

    std::unique_ptr<CollectionShardingState> make(const NamespaceString& nss) override {
        MONGO_UNREACHABLE;
    }

    const StaleShardCollectionMetadataHandler& getStaleShardExceptionHandler() const override {
        return *_staleExceptionHandler;
    };

    const std::shared_ptr<StaleShardExceptionHandlerMock> _staleExceptionHandler;
};

class DatabaseShardingStateFactoryMock : public DatabaseShardingStateFactory {
public:
    DatabaseShardingStateFactoryMock(
        std::shared_ptr<StaleShardExceptionHandlerMock> staleExceptionHandler)
        : _staleExceptionHandler(staleExceptionHandler) {}

    std::unique_ptr<DatabaseShardingState> make(const DatabaseName& dbName) override {
        MONGO_UNREACHABLE;
    }

    const StaleShardDatabaseMetadataHandler& getStaleShardExceptionHandler() const override {
        return *_staleExceptionHandler;
    };

    const std::shared_ptr<StaleShardExceptionHandlerMock> _staleExceptionHandler;
};

class CatalogCacheMockExtended : public CatalogCacheMock {
public:
    CatalogCacheMockExtended(ServiceContext* serviceContext,
                             std::shared_ptr<CatalogCacheLoader> loader)
        : CatalogCacheMock(serviceContext, loader) {}
    StatusWith<CollectionRoutingInfo> getCollectionRoutingInfo(OperationContext* opCtx,
                                                               const NamespaceString& nss,
                                                               bool allowLocks) override {
        numGetCollectionRoutingInfoCalls++;
        return CatalogCacheMock::getCollectionRoutingInfo(opCtx, nss, allowLocks);
    }

    int numGetCollectionRoutingInfoCalls{0};
};

class ShardRoleLoopTest : public ServiceContextTest {
protected:
    void setUp() override {
        // Initialize the StaleShardExceptionHandlerMock
        _staleShardExceptionHandlerMock = std::make_shared<StaleShardExceptionHandlerMock>();

        // Initialize CSS Factory mock.
        CollectionShardingStateFactory::set(
            getServiceContext(),
            std::make_unique<CollectionShardingStateFactoryMock>(_staleShardExceptionHandlerMock));

        // Initialize DSS Factory mock.
        DatabaseShardingStateFactory::set(
            getServiceContext(),
            std::make_unique<DatabaseShardingStateFactoryMock>(_staleShardExceptionHandlerMock));

        // Initialize catalog cache mock.
        auto catalogCacheMock = std::make_unique<CatalogCacheMockExtended>(
            getServiceContext(), std::make_shared<ShardServerCatalogCacheLoaderMock>());
        _catalogCacheMock = catalogCacheMock.get();
        auto grid = Grid::get(getServiceContext());
        grid->setCatalogCache_forTest(std::move(catalogCacheMock));
        grid->setInitialized_forTest();

        // Create an opCtx.
        _uniqueOpCtx = makeOperationContext();
        _opCtx = _uniqueOpCtx.get();
    }

    void tearDown() override {}

    ServiceContext::UniqueOperationContext _uniqueOpCtx;
    OperationContext* _opCtx;

    std::shared_ptr<StaleShardExceptionHandlerMock> _staleShardExceptionHandlerMock;
    CatalogCacheMockExtended* _catalogCacheMock{nullptr};
};

TEST_F(ShardRoleLoopTest, handleStaleDbException) {
    const Status staleDbStaleShardStatus{
        StaleDbRoutingVersion(kDbName,
                              DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                              DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                              boost::none),
        "stale is stale"};

    const Status staleDbUnknownShardMetadataStatus{
        StaleDbRoutingVersion(
            kDbName, DatabaseVersion(UUID::gen(), Timestamp(2, 0)), boost::none, boost::none),
        "shard does not know its version"};

    const Status staleDbCriticalSectionActiveStatus{
        StaleDbRoutingVersion(kDbName,
                              DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                              boost::none,
                              SemiFuture<void>::makeReady().share()),
        "critical section active"};

    const Status staleDbStaleRouterStatus{
        StaleDbRoutingVersion(kDbName,
                              DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                              DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                              boost::none),
        "router is stale"};

    // Shard is stale.
    shard_role_loop::RetryContext retryCtx;
    ASSERT_EQ(CanRetry::YES, handleStaleError(_opCtx, staleDbStaleShardStatus, retryCtx));
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();

    // Shard doesn't know its version.
    retryCtx = shard_role_loop::RetryContext{};
    ASSERT_EQ(CanRetry::YES, handleStaleError(_opCtx, staleDbUnknownShardMetadataStatus, retryCtx));
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();

    // Shard critical section is active.
    retryCtx = shard_role_loop::RetryContext{};
    ASSERT_EQ(CanRetry::YES,
              handleStaleError(_opCtx, staleDbCriticalSectionActiveStatus, retryCtx));
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();

    // Router is stale.
    retryCtx = shard_role_loop::RetryContext{};
    ASSERT_EQ(CanRetry::NO, handleStaleError(_opCtx, staleDbStaleRouterStatus, retryCtx));
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();

    // The router was stale, but the database does not exist anymore. The shard shall not retry.
    _staleShardExceptionHandlerMock->_handleStaleDatabaseVersionExceptionRet = boost::none;
    retryCtx = shard_role_loop::RetryContext{};
    ASSERT_EQ(CanRetry::NO, handleStaleError(_opCtx, staleDbUnknownShardMetadataStatus, retryCtx));
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();
}

TEST_F(ShardRoleLoopTest, handleStaleConfigException) {
    const auto collectionGeneration1 = CollectionGeneration(OID::gen(), Timestamp(1, 0));
    const auto collectionGeneration2 = CollectionGeneration(OID::gen(), Timestamp(2, 0));

    const Status staleConfigShardIsStaleStatus{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {2, 0})),
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        kTestShardId),
        "shard is stale"};

    const Status staleConfigShardUnknownMetadataStatus{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        boost::none,
                        kTestShardId),
        "shard does not know its version"};

    const Status staleConfigShardCriticalSectionActiveStatus{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        boost::none,
                        kTestShardId,
                        SemiFuture<void>::makeReady().share()),
        "shard critical section active"};

    const Status staleConfigRouterIsStaleStatus{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {2, 0})),
                        kTestShardId),
        "router is stale"};

    // Shard is stale.
    shard_role_loop::RetryContext retryCtx;
    ASSERT_EQ(CanRetry::YES, handleStaleError(_opCtx, staleConfigShardIsStaleStatus, retryCtx));
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();

    // Shard doesn't know its version.
    retryCtx = shard_role_loop::RetryContext{};
    ASSERT_EQ(CanRetry::YES,
              handleStaleError(_opCtx, staleConfigShardUnknownMetadataStatus, retryCtx));
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();

    // Shard critical section is active.
    retryCtx = shard_role_loop::RetryContext{};
    ASSERT_EQ(CanRetry::YES,
              handleStaleError(_opCtx, staleConfigShardCriticalSectionActiveStatus, retryCtx));
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();

    // Router is stale.
    retryCtx = shard_role_loop::RetryContext{};
    ASSERT_EQ(CanRetry::NO, handleStaleError(_opCtx, staleConfigRouterIsStaleStatus, retryCtx));
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    _staleShardExceptionHandlerMock->clear();
}

TEST_F(ShardRoleLoopTest, handleStaleConfigExceptionNonComparableVersions) {
    const auto testFn = [&](ShardVersion shardWantedVersion,
                            ShardVersion routerVersion,
                            ShardVersion shardWantedVersionAfterRecovery,
                            bool expectShardRetry) {
        const Status staleConfigUncomparableVersionsStatus{
            StaleConfigInfo(kTestNss, routerVersion, shardWantedVersion, kTestShardId),
            "version mismatch, but versions not totally ordered."};

        _staleShardExceptionHandlerMock->_handleStaleShardVersionExceptionRet =
            shardWantedVersionAfterRecovery.placementVersion();

        shard_role_loop::RetryContext retryCtx;
        ASSERT_EQ(expectShardRetry ? CanRetry::YES : CanRetry::NO,
                  handleStaleError(_opCtx, staleConfigUncomparableVersionsStatus, retryCtx));
        ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
        // Always expect the shard to refresh pessimistically.
        ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numShardVersionCalls);
        _staleShardExceptionHandlerMock->clear();
    };

    const auto collectionGeneration = CollectionGeneration(OID::gen(), Timestamp(1, 0));

    const auto shardVersionSharded =
        ShardVersionFactory::make(ChunkVersion(collectionGeneration, {1, 0}));

    const auto shardVersionShardedNoChunks =
        ShardVersionFactory::make(ChunkVersion(collectionGeneration, {0, 0}));

    // UNSHARDED vs ShardVersion(x, y). Router was right. Expect shard retry.
    testFn(ShardVersion::UNSHARDED(), shardVersionSharded, shardVersionSharded, true);

    // UNSHARDED vs ShardVersion(x, y). Shard was right. Expect no shard retry.
    testFn(ShardVersion::UNSHARDED(), shardVersionSharded, ShardVersion::UNSHARDED(), false);

    // ShardVersion(gen, 0, 0) vs ShardVersion(gen, x, y). Router was right. Expect shard retry.
    testFn(shardVersionShardedNoChunks, shardVersionSharded, shardVersionSharded, true);

    // UNSHARDED vs ShardVersion(x, y). Shard was right. Expect no shard retry.
    testFn(shardVersionShardedNoChunks, shardVersionSharded, shardVersionShardedNoChunks, false);
}

TEST_F(ShardRoleLoopTest, handleShardCannotRefreshDueToLocksHeldException) {
    const Status cannotRefreshStatus{ShardCannotRefreshDueToLocksHeldInfo(kTestNss),
                                     "cannot refresh due to locks held"};

    _catalogCacheMock->setCollectionReturnValue(
        kTestNss,
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(
            kTestNss, kTestShardId, DatabaseVersion(UUID::gen(), Timestamp(1, 0))));

    // Can be retried.
    shard_role_loop::RetryContext retryCtx;
    ASSERT_EQ(CanRetry::YES, handleStaleError(_opCtx, cannotRefreshStatus, retryCtx));
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
}

TEST_F(ShardRoleLoopTest, handleNonStaleException) {
    const Status nonStaleStatus{ErrorCodes::InternalError, "some error"};
    // Can not retry.
    shard_role_loop::RetryContext retryCtx;
    ASSERT_EQ(CanRetry::NO, handleStaleError(_opCtx, nonStaleStatus, retryCtx));
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
}

class MockFn {
public:
    MockFn(std::queue<Status> returnStatuses) : _returnStatuses(std::move(returnStatuses)) {}
    void operator()() {
        ASSERT_FALSE(_returnStatuses.empty());
        auto status = _returnStatuses.front();
        _returnStatuses.pop();
        uassertStatusOK(status);
    }

    int remainingCalls() {
        return _returnStatuses.size();
    }

private:
    std::queue<Status> _returnStatuses;
};

TEST_F(ShardRoleLoopTest, LoopFnOk) {
    std::queue<Status> statuses{{Status::OK()}};
    MockFn fn(statuses);
    ASSERT_DOES_NOT_THROW(withStaleShardRetry(_opCtx, fn));
    // No shard refresh expected
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsNonStaleMetadataError) {
    std::queue<Status> statuses{{Status{ErrorCodes::InternalError, "some error"}}};
    MockFn fn(statuses);
    ASSERT_THROWS_CODE(withStaleShardRetry(_opCtx, fn), DBException, ErrorCodes::InternalError);
    // No shard refresh expected
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsStaleDbVersionBecauseShardIsStaleStale) {
    std::queue<Status> statuses;
    // Shard is stale.
    statuses.push({Status{StaleDbRoutingVersion(kDbName,
                                                DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                                                DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                                                boost::none),
                          "stale db reason"}});
    // Shard won't be stale after refresh.
    statuses.push(Status::OK());

    MockFn fn(statuses);
    ASSERT_DOES_NOT_THROW(withStaleShardRetry(_opCtx, fn));
    // We expect the ShardRoleLoop to refresh the shard role metadata and retry.
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsStaleDbVersionBecauseShardHasUnknownVersion) {
    std::queue<Status> statuses;
    // Shard is stale.
    statuses.push({Status{StaleDbRoutingVersion(kDbName,
                                                DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                                                DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                                                boost::none),
                          "stale db reason"}});
    // Shard won't be stale after refresh.
    statuses.push(Status::OK());

    MockFn fn(statuses);
    ASSERT_DOES_NOT_THROW(withStaleShardRetry(_opCtx, fn));
    // We expect the ShardRoleLoop to refresh the shard role metadata and retry.
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsStaleDbVersionBecauseShardCriticalSectionIsActive) {
    std::queue<Status> statuses;
    // Shard critical section is active.
    statuses.push({Status{
        StaleDbRoutingVersion(
            kDbName, DatabaseVersion(UUID::gen(), Timestamp(2, 0)), boost::none, boost::none),
        "critical section active"}});
    // Critical section not active anymore after refresh.
    statuses.push(Status::OK());

    MockFn fn(statuses);
    ASSERT_DOES_NOT_THROW(withStaleShardRetry(_opCtx, fn));
    // We expect the ShardRoleLoop to refresh the shard role metadata and retry.
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsStaleDbVersionBecauseRouterIsStale) {
    std::queue<Status> statuses;
    // Router is stale.
    statuses.push({Status{StaleDbRoutingVersion(kDbName,
                                                DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                                                DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                                                boost::none),
                          "Router is stale"}});

    MockFn fn(statuses);
    ASSERT_THROWS_CODE(withStaleShardRetry(_opCtx, fn), DBException, ErrorCodes::StaleDbVersion);
    // No shard refresh expected
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsStaleConfigBecauseShardIsStale) {
    const auto testFn = [&](Status staleStatus) {
        std::queue<Status> statuses;
        // Shard is stale.
        statuses.push(staleStatus);
        // Shard won't be stale after refresh.
        statuses.push(Status::OK());

        MockFn fn(statuses);
        ASSERT_DOES_NOT_THROW(withStaleShardRetry(_opCtx, fn));
        // We expect the ShardRoleLoop to refresh the shard role metadata and retry.
        ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
        ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numShardVersionCalls);
        ASSERT_EQ(0, fn.remainingCalls());
        _staleShardExceptionHandlerMock->clear();
    };

    const auto collectionGeneration1 = CollectionGeneration(OID::gen(), Timestamp(1, 0));
    const auto collectionGeneration2 = CollectionGeneration(OID::gen(), Timestamp(2, 0));

    testFn(Status{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration2, {1, 0})),
                        boost::none,
                        kTestShardId),
        "shard has unknown collection metadata"});

    testFn(Status{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration2, {1, 0})),
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        kTestShardId),
        "shard's known coll generation is older than the one in the request"});

    testFn(Status{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {2, 0})),
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        kTestShardId),
        "shard's known placement version is older than the one in the request"});

    testFn(Status{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {2, 0})),
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        kTestShardId,
                        SemiFuture<void>::makeReady().share()),
        "critical section active"});
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsStaleConfigBecauseRouterIsStale) {
    const auto testFn = [&](Status staleStatus) {
        std::queue<Status> statuses;
        // Router is stale.
        statuses.push(staleStatus);

        MockFn fn(statuses);
        ASSERT_THROWS_CODE(withStaleShardRetry(_opCtx, fn), DBException, ErrorCodes::StaleConfig);
        // We expect the ShardRoleLoop to refresh the shard role metadata and retry.
        ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
        ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
        ASSERT_EQ(0, fn.remainingCalls());
    };

    const auto collectionGeneration1 = CollectionGeneration(OID::gen(), Timestamp(1, 0));
    const auto collectionGeneration2 = CollectionGeneration(OID::gen(), Timestamp(2, 0));

    testFn(Status{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration2, {1, 0})),
                        kTestShardId),
        "router's known coll generation is older than the one in the request"});

    testFn(Status{
        StaleConfigInfo(kTestNss,
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0})),
                        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {2, 0})),
                        kTestShardId),
        "shard's known placement version is older than the one in the request"});
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsStaleConfigNonComparable_RouterActuallyStale) {
    const CollectionGeneration collectionGeneration1 = CollectionGeneration(OID::gen(), {1, 0});
    const ShardVersion shardedSV =
        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0}));

    std::queue<Status> statuses;
    statuses.push(
        {Status{StaleConfigInfo(kTestNss, ShardVersion::UNSHARDED(), shardedSV, kTestShardId),
                "non comparable"}});
    _staleShardExceptionHandlerMock->_handleStaleShardVersionExceptionRet =
        shardedSV.placementVersion();
    MockFn fn(statuses);
    // The shard was right, so no retry. The router must refresh and retry.
    ASSERT_THROWS_CODE(withStaleShardRetry(_opCtx, fn), DBException, ErrorCodes::StaleConfig);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}

TEST_F(ShardRoleLoopTest, LoopFnThrowsStaleConfigNonComparable_ShardActuallyStale) {
    const CollectionGeneration collectionGeneration1 = CollectionGeneration(OID::gen(), {1, 0});
    const ShardVersion shardedSV =
        ShardVersionFactory::make(ChunkVersion(collectionGeneration1, {1, 0}));

    std::queue<Status> statuses;
    statuses.push(
        {Status{StaleConfigInfo(kTestNss, ShardVersion::UNSHARDED(), shardedSV, kTestShardId),
                "non comparable"}});
    statuses.push(
        Status::OK());  // After refresh, the shard is not stale anymore so the fn succeeds.
    _staleShardExceptionHandlerMock->_handleStaleShardVersionExceptionRet =
        ShardVersion::UNSHARDED().placementVersion();
    MockFn fn(statuses);
    // The router was right, so retry. The shard must refresh and retry.
    ASSERT_DOES_NOT_THROW(withStaleShardRetry(_opCtx, fn));
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(1, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}


TEST_F(ShardRoleLoopTest, LoopFnThrowsShardCannotRefreshDueToLocksHeld) {
    std::queue<Status> statuses;
    // Shard cannot refresh due to locks held.
    statuses.push({Status{ShardCannotRefreshDueToLocksHeldInfo(kTestNss),
                          "cannot refresh due to locks held"}});
    // Shard's catalog cache will be available after refresh.
    statuses.push(Status::OK());

    _catalogCacheMock->setCollectionReturnValue(
        kTestNss,
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(
            kTestNss, kTestShardId, DatabaseVersion(UUID::gen(), Timestamp(1, 0))));

    MockFn fn(statuses);
    ASSERT_DOES_NOT_THROW(withStaleShardRetry(_opCtx, fn));
    // CSR not expected to be refreshed. Only the local catalog cache will be refreshed.
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
    ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
    ASSERT_EQ(1, _catalogCacheMock->numGetCollectionRoutingInfoCalls);
    ASSERT_EQ(0, fn.remainingCalls());
}

TEST_F(ShardRoleLoopTest, LoopExhaustRetryAttempts) {
    const auto testFn = [&](Status s) {
        const int kMaxRetryAttempts = maxShardStaleMetadataRetryAttempts.load();
        std::queue<Status> statuses;
        for (int i = 0; i < 1 + kMaxRetryAttempts; i++) {
            // Shard is stale.
            statuses.push(s);
        }

        MockFn fn(statuses);
        // If we exhaust retries, we propagate the last error.
        ASSERT_THROWS_CODE(withStaleShardRetry(_opCtx, fn), DBException, s);
        ASSERT_EQ(0, fn.remainingCalls());
    };

    testFn(Status{StaleDbRoutingVersion(kDbName,
                                        DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                                        DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                                        boost::none),
                  "shard is stale"});

    testFn(Status{StaleConfigInfo(kTestNss, ShardVersion::UNSHARDED(), boost::none, kTestShardId),
                  "shard is stale"});

    _catalogCacheMock->setCollectionReturnValue(
        kTestNss,
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(
            kTestNss, kTestShardId, DatabaseVersion(UUID::gen(), Timestamp(1, 0))));
    testFn(
        Status{ShardCannotRefreshDueToLocksHeldInfo(kTestNss), "cannot refresh due to locks held"});
}

TEST_F(ShardRoleLoopTest, LoopFnDoesNotRetryWhenLocksHeldHigherUp) {
    auto testFn = [&](Status s) {
        std::queue<Status> statuses{{s}};

        Lock::GlobalLock gl(_opCtx, MODE_IS);
        MockFn fn(statuses);
        // Expected to throw the original error, even when otherwise retriable.
        ASSERT_THROWS_CODE(withStaleShardRetry(_opCtx, fn), DBException, s);
        // Forbidden to refresh when holding locks.
        ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
        ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
        ASSERT_EQ(0, fn.remainingCalls());
    };

    testFn(Status{StaleDbRoutingVersion(kDbName,
                                        DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                                        DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                                        boost::none),
                  "stale db reason"});
    testFn(Status{StaleConfigInfo(kTestNss, ShardVersion::UNSHARDED(), boost::none, kTestShardId),
                  "unknown collection metadata"});
    testFn(
        Status{ShardCannotRefreshDueToLocksHeldInfo(kTestNss), "cannot refresh due to locks held"});
}

TEST_F(ShardRoleLoopTest, LoopFnDoesNotRetryWhenInDbDirectClient) {
    auto testFn = [&](Status s) {
        std::queue<Status> statuses{{s}};

        _opCtx->getClient()->setInDirectClient(true);
        MockFn fn(statuses);
        // Expected to throw the original error, even when otherwise retriable.
        ASSERT_THROWS_CODE(withStaleShardRetry(_opCtx, fn), DBException, s);
        // Forbidden to refresh when holding locks.
        ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numDbVersionCalls);
        ASSERT_EQ(0, _staleShardExceptionHandlerMock->_numShardVersionCalls);
        ASSERT_EQ(0, fn.remainingCalls());
    };

    testFn(Status{StaleDbRoutingVersion(kDbName,
                                        DatabaseVersion(UUID::gen(), Timestamp(2, 0)),
                                        DatabaseVersion(UUID::gen(), Timestamp(1, 0)),
                                        boost::none),
                  "stale db reason"});
    testFn(Status{StaleConfigInfo(kTestNss, ShardVersion::UNSHARDED(), boost::none, kTestShardId),
                  "unknown collection metadata"});
    testFn(
        Status{ShardCannotRefreshDueToLocksHeldInfo(kTestNss), "cannot refresh due to locks held"});
}

}  // namespace
}  // namespace mongo
