// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"

#include "mongo/db/database_name.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <cstddef>
#include <memory>
#include <string_view>

namespace mongo {

class DDLLockManagerTest : public ShardServerTestFixture {
    /**
     * Implementation of the interface to inject to be able to wait for recovery
     * The empty implementation means the recovering happened immediately.
     */
    class Recoverable : public DDLLockManager::Recoverable {
    public:
        void waitForRecovery(OperationContext*) const override {}
    };

public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        operationContext()->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        DDLLockManager::get(getServiceContext())->setRecoverable(_recoverable.get());
    }

protected:
    const DatabaseName _dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const DatabaseVersion _dbVersion{UUID::gen(), Timestamp(1, 0)};
    std::unique_ptr<Recoverable> _recoverable = std::make_unique<Recoverable>();
};

TEST_F(DDLLockManagerTest, LockNormalCollection) {
    const auto nss = NamespaceString::createNamespaceString_forTest(_dbName, "foo");
    ScopedSetShardRole shardRole(operationContext(), nss, boost::none, _dbVersion);

    {
        const std::string_view reason;
        DDLLockManager::ScopedCollectionDDLLock ddlLock(operationContext(), nss, reason, MODE_X);

        ASSERT_TRUE(
            shard_role_details::getLocker(operationContext())
                ->isLockHeldForMode(ResourceId{RESOURCE_DDL_DATABASE, nss.dbName()}, MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isLockHeldForMode(ResourceId{RESOURCE_DDL_COLLECTION, nss}, MODE_X));
    }

    ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                     ->isLockHeldForMode(ResourceId{RESOURCE_DDL_DATABASE, nss.dbName()}, MODE_IX));
    ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                     ->isLockHeldForMode(ResourceId{RESOURCE_DDL_COLLECTION, nss}, MODE_X));
}

TEST_F(DDLLockManagerTest, LockTimeseriesBucketsCollection) {
    const auto viewNss = NamespaceString::createNamespaceString_forTest(_dbName, "foo");
    const auto bucketsNss = viewNss.makeTimeseriesBucketsNamespace();
    ScopedSetShardRole shardRole(operationContext(), bucketsNss, boost::none, _dbVersion);

    {
        std::string_view reason;
        DDLLockManager::ScopedCollectionDDLLock ddlLock(
            operationContext(), bucketsNss, reason, MODE_X);

        ASSERT_TRUE(
            shard_role_details::getLocker(operationContext())
                ->isLockHeldForMode(ResourceId{RESOURCE_DDL_DATABASE, viewNss.dbName()}, MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(operationContext())
                        ->isLockHeldForMode(ResourceId{RESOURCE_DDL_COLLECTION, viewNss}, MODE_X));
    }

    ASSERT_FALSE(
        shard_role_details::getLocker(operationContext())
            ->isLockHeldForMode(ResourceId{RESOURCE_DDL_DATABASE, viewNss.dbName()}, MODE_IX));
    ASSERT_FALSE(shard_role_details::getLocker(operationContext())
                     ->isLockHeldForMode(ResourceId{RESOURCE_DDL_COLLECTION, viewNss}, MODE_X));
}

TEST_F(DDLLockManagerTest, TryLockDatabase) {
    FailPointEnableBlock fp("overrideDDLLockTimeout", BSON("timeoutMillisecs" << 10));

    const auto nss = NamespaceString::createNamespaceString_forTest(_dbName, "foo");
    ScopedSetShardRole shardRole(operationContext(), nss, boost::none, _dbVersion);
    DDLLockManager::ScopedDatabaseDDLLock ddlLock(operationContext(), _dbName, "ddlLock", MODE_S);

    const auto tryLock = [&](LockMode mode, bool shouldSuccess) {
        auto newClient =
            operationContext()->getServiceContext()->getService()->makeClient("newClient");
        const AlternativeClientRegion acr(newClient);
        const auto newCtx = cc().makeOperationContext();
        newCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        ScopedSetShardRole shardRole(newCtx.get(), nss, boost::none, _dbVersion);
        auto strategy = DDLLockManager::SingleTryBackoffStrategy();

        const auto lock = [&]() {
            DDLLockManager::ScopedDatabaseDDLLock(
                newCtx.get(), _dbName, "compatibleLock", mode, strategy);
        };

        if (shouldSuccess) {
            ASSERT_DOES_NOT_THROW(lock());
        } else {
            ASSERT_THROWS(lock(), DBException);
        }
    };

    tryLock(MODE_S, true);
    tryLock(MODE_IX, false);
}

TEST_F(DDLLockManagerTest, TryLockCollection) {
    FailPointEnableBlock fp("overrideDDLLockTimeout", BSON("timeoutMillisecs" << 10));

    const auto nss = NamespaceString::createNamespaceString_forTest(_dbName, "foo");
    ScopedSetShardRole shardRole(operationContext(), nss, boost::none, _dbVersion);
    DDLLockManager::ScopedCollectionDDLLock ddlLock(
        operationContext(), nss, std::string_view{}, MODE_S);

    const auto tryLock = [&](LockMode mode, bool shouldSuccess) {
        auto newClient =
            operationContext()->getServiceContext()->getService()->makeClient("newClient");
        const AlternativeClientRegion acr(newClient);
        const auto newCtx = cc().makeOperationContext();
        newCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        ScopedSetShardRole shardRole(newCtx.get(), nss, boost::none, _dbVersion);
        auto strategy = DDLLockManager::SingleTryBackoffStrategy();

        const auto lock = [&]() {
            DDLLockManager::ScopedCollectionDDLLock(
                newCtx.get(), nss, "compatibleLock", mode, strategy);
        };

        if (shouldSuccess) {
            ASSERT_DOES_NOT_THROW(lock());
        } else {
            ASSERT_THROWS(lock(), DBException);
        }
    };

    tryLock(MODE_S, true);
    tryLock(MODE_IX, false);
}

TEST(DDLLockManagerBackoffStrategyTest, SingleTryBackoffStrategy) {
    // successful call, no need to retry.
    {
        DDLLockManager::SingleTryBackoffStrategy strategy;
        size_t callCount{0};
        ASSERT_TRUE(strategy.execute(
            [&]() {
                callCount++;
                return true;
            },
            [](Milliseconds) { FAIL("unexpected call on sleep"); }));
        ASSERT_EQ(callCount, 1);
    }

    // unsuccessful call, still no retry.
    {
        DDLLockManager::SingleTryBackoffStrategy strategy;
        size_t callCount{0};
        ASSERT_FALSE(strategy.execute(
            [&]() {
                callCount++;
                return false;
            },
            [](Milliseconds) { FAIL("unexpected call on sleep"); }));
        ASSERT_EQ(callCount, 1);
    }

    // unsuccessful call, unexpected error, propagate it.
    {
        DDLLockManager::SingleTryBackoffStrategy strategy;
        size_t callCount{0};
        ASSERT_THROWS(strategy.execute(
                          [&]() -> bool {
                              callCount++;
                              uasserted(ErrorCodes::InternalError, "");
                          },
                          [](Milliseconds) { FAIL("unexpected call on sleep"); }),
                      ExceptionFor<ErrorCodes::InternalError>);
        ASSERT_EQ(callCount, 1);
    }
}

TEST(DDLLockManagerBackoffStrategyTest, ConstantBackoffStrategy) {
    // successful call, no need to retry.
    {
        DDLLockManager::ConstantBackoffStrategy<3, 100> strategy;
        size_t callCount{0};
        Milliseconds totalSleep{0};
        ASSERT_TRUE(strategy.execute(
            [&]() {
                callCount++;
                return true;
            },
            [&](Milliseconds millis) { totalSleep += millis; }));
        ASSERT_EQ(totalSleep.count(), 0);
        ASSERT_EQ(callCount, 1);
    }

    // unsuccessful call, max out retry.
    {
        DDLLockManager::ConstantBackoffStrategy<3, 100> strategy;
        size_t callCount{0};
        Milliseconds totalSleep{0};
        ASSERT_FALSE(strategy.execute(
            [&]() {
                callCount++;
                return false;
            },
            [&](Milliseconds millis) { totalSleep += millis; }));
        ASSERT_GREATER_THAN_OR_EQUALS(totalSleep.count(), 2 * 50);
        ASSERT_LESS_THAN_OR_EQUALS(totalSleep.count(), 2 * 100);
        ASSERT_EQ(callCount, 3);
    }

    // eventually successful call
    {
        DDLLockManager::ConstantBackoffStrategy<3, 100> strategy;
        size_t callCount{0};
        Milliseconds totalSleep{0};
        ASSERT_TRUE(strategy.execute(
            [&]() {
                callCount++;
                if (callCount == 1) {
                    return false;
                }
                return true;
            },
            [&](Milliseconds millis) { totalSleep += millis; }));
        ASSERT_GREATER_THAN_OR_EQUALS(totalSleep.count(), 50);
        ASSERT_LESS_THAN_OR_EQUALS(totalSleep.count(), 100);
        ASSERT_EQ(callCount, 2);
    }
}

TEST(DDLLockManagerBackoffStrategyTest, TruncatedExponentialBackoffStrategy) {
    // unsuccessful call, max out retry, no cap.
    {
        DDLLockManager::
            TruncatedExponentialBackoffStrategy<3, 100, std::numeric_limits<unsigned int>::max()>
                strategy;
        size_t callCount{0};
        Milliseconds totalSleep{0};
        ASSERT_FALSE(strategy.execute(
            [&]() {
                callCount++;
                return false;
            },
            [&](Milliseconds millis) { totalSleep += millis; }));
        ASSERT_GREATER_THAN_OR_EQUALS(totalSleep.count(), 50 + 100);
        ASSERT_LESS_THAN_OR_EQUALS(totalSleep.count(), 100 + 200);
        ASSERT_EQ(callCount, 3);
    }

    // unsuccessful call, max out retry, with cap.
    {
        DDLLockManager::TruncatedExponentialBackoffStrategy<5, 100, 200> strategy;
        size_t callCount{0};
        Milliseconds totalSleep{0};
        ASSERT_FALSE(strategy.execute(
            [&]() {
                callCount++;
                return false;
            },
            [&](Milliseconds millis) { totalSleep += millis; }));
        ASSERT_GREATER_THAN_OR_EQUALS(totalSleep.count(), 50 + 100 + 100 + 100);
        ASSERT_LESS_THAN_OR_EQUALS(totalSleep.count(), 100 + 200 + 200 + 200);
        ASSERT_EQ(callCount, 5);
    }
}

}  // namespace mongo
