/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"

#include "mongo/base/string_data.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <chrono>
#include <cstddef>
#include <memory>

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
        const StringData reason;
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
        StringData reason;
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
        auto newClient = operationContext()
                             ->getServiceContext()
                             ->getService(ClusterRole::ShardServer)
                             ->makeClient("newClient");
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
    DDLLockManager::ScopedCollectionDDLLock ddlLock(operationContext(), nss, StringData{}, MODE_S);

    const auto tryLock = [&](LockMode mode, bool shouldSuccess) {
        auto newClient = operationContext()
                             ->getServiceContext()
                             ->getService(ClusterRole::ShardServer)
                             ->makeClient("newClient");
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
