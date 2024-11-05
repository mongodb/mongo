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

#include <memory>

#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/s/database_version.h"

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

        AutoGetDb autoDb(operationContext(), _dbName, MODE_X);
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(operationContext(), _dbName);
        scopedDss->setDbInfo(operationContext(), {_dbName, kMyShardName, _dbVersion});
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

}  // namespace mongo
