// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/database_holder_mock.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_mock.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class CatalogRAIITestFixture : public ServiceContextTest {
public:
    using ClientAndCtx =
        std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>;

    ClientAndCtx makeClientWithLocker(const std::string& clientName) {
        auto client = getServiceContext()->getService()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const NamespaceString kSecondaryNss1 =
        NamespaceString::createNamespaceString_forTest("test", "secondaryColl1");
    const NamespaceString kSecondaryNss2 =
        NamespaceString::createNamespaceString_forTest("test", "secondaryColl2");
    const NamespaceString kSecondaryNss3 =
        NamespaceString::createNamespaceString_forTest("test", "secondaryColl3");
    const NamespaceString kSecondaryNssOtherDb1 =
        NamespaceString::createNamespaceString_forTest("test2", "secondaryColl1");
    const NamespaceString kSecondaryNssOtherDb2 =
        NamespaceString::createNamespaceString_forTest("test2", "secondaryColl2");
    const Milliseconds timeoutMs = Seconds(1);
    const ClientAndCtx client1 = makeClientWithLocker("client1");
    const ClientAndCtx client2 = makeClientWithLocker("client2");

private:
    void setUp() override;
    FailPointEnableBlock _skipDirectConnectionChecks{"skipDirectConnectionChecks"};
};

void CatalogRAIITestFixture::setUp() {
    DatabaseShardingStateFactory::set(getServiceContext(),
                                      std::make_unique<DatabaseShardingStateFactoryMock>());
    DatabaseHolder::set(getServiceContext(), std::make_unique<DatabaseHolderMock>());
}

void failsWithLockTimeout(std::function<void()> func, Milliseconds timeoutMillis) {
    Date_t t1 = Date_t::now();
    try {
        func();
        FAIL("Should have gotten an exception due to timeout");
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>& ex) {
        LOGV2(20396, "{ex}", "ex"_attr = ex);
        Date_t t2 = Date_t::now();
        ASSERT_GTE(t2 - t1, timeoutMillis);
    }
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_X);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isDbLockedForMode(nss.dbName(), MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetDb db(client2.second.get(), nss.dbName(), MODE_X, Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetDBGlobalLockDeadline) {
    Lock::GlobalLock gLock1(client1.second.get(), MODE_X);
    ASSERT(gLock1.isLocked());
    failsWithLockTimeout(
        [&] {
            AutoGetDb db(client2.second.get(), nss.dbName(), MODE_X, Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDeadlineNow) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isDbLockedForMode(nss.dbName(), MODE_IX));
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.dbName(), MODE_X, Date_t::now()); },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDeadlineMin) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isDbLockedForMode(nss.dbName(), MODE_IX));
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.dbName(), MODE_X, Date_t{}); },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionCollLockDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isDbLockedForMode(nss.dbName(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isCollectionLockedForMode(nss, MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(
                client2.second.get(),
                nss,
                MODE_X,
                auto_get_collection::Options{}.deadline(Date_t::now() + timeoutMs));
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDBLockDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_X);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isDbLockedForMode(nss.dbName(), MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(
                client2.second.get(),
                nss,
                MODE_X,
                auto_get_collection::Options{}.deadline(Date_t::now() + timeoutMs));
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionGlobalLockDeadline) {
    Lock::GlobalLock gLock1(client1.second.get(), MODE_X);
    ASSERT(shard_role_details::getLocker(client1.second.get())->isLocked());
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(
                client2.second.get(),
                nss,
                MODE_X,
                auto_get_collection::Options{}.deadline(Date_t::now() + timeoutMs));
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDeadlineNow) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isDbLockedForMode(nss.dbName(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isCollectionLockedForMode(nss, MODE_X));

    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   auto_get_collection::Options{}.deadline(Date_t::now()));
        },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDeadlineMin) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isDbLockedForMode(nss.dbName(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isCollectionLockedForMode(nss, MODE_X));

    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   auto_get_collection::Options{}.deadline(Date_t()));
        },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionNotCompatibleWithRSTLExclusiveLock) {
    Lock::GlobalLock gLock1(client1.second.get(), MODE_X);
    ASSERT(shard_role_details::getLocker(client1.second.get())->isLocked());

    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(
                client2.second.get(),
                nss,
                MODE_IX,
                auto_get_collection::Options{}.deadline(Date_t::now() + timeoutMs));
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDBLockCompatibleX) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(shard_role_details::getLocker(client1.second.get())
               ->isDbLockedForMode(nss.dbName(), MODE_IX));

    AutoGetCollection coll(client2.second.get(), nss, MODE_X);
}

TEST_F(CatalogRAIITestFixture, AutoGetDbSecondaryNamespacesSingleDb) {
    auto opCtx1 = client1.second.get();

    boost::optional<AutoGetDb> autoGetDb;
    autoGetDb.emplace(opCtx1, nss.dbName(), MODE_IS, Date_t::max());

    if (!gFeatureFlagIntentRegistration.isEnabled()) {
        ASSERT(shard_role_details::getLocker(opCtx1)->isRSTLLocked());
    }
    ASSERT(shard_role_details::getLocker(opCtx1)->isReadLocked());  // Global lock check
    ASSERT(shard_role_details::getLocker(opCtx1)->isDbLockedForMode(nss.dbName(), MODE_IS));
    ASSERT(
        shard_role_details::getLocker(opCtx1)->isDbLockedForMode(kSecondaryNss1.dbName(), MODE_IS));
    ASSERT(
        shard_role_details::getLocker(opCtx1)->isDbLockedForMode(kSecondaryNss2.dbName(), MODE_IS));

    ASSERT(!shard_role_details::getLocker(opCtx1)->isDbLockedForMode(kSecondaryNssOtherDb1.dbName(),
                                                                     MODE_IS));
    ASSERT(!shard_role_details::getLocker(opCtx1)->isDbLockedForMode(kSecondaryNssOtherDb2.dbName(),
                                                                     MODE_IS));
    ASSERT(!shard_role_details::getLocker(opCtx1)->isRSTLExclusive());
    ASSERT(!shard_role_details::getLocker(opCtx1)->isGlobalLockedRecursively());
    ASSERT(!shard_role_details::getLocker(opCtx1)->isWriteLocked());

    // All the locks should release.
    autoGetDb.reset();
    ASSERT(!shard_role_details::getLocker(opCtx1)->isLocked());  // Global lock check.
}

using ReadSource = RecoveryUnit::ReadSource;

class RecoveryUnitMock : public RecoveryUnitNoop {
public:
    void setTimestampReadSource(ReadSource source,
                                boost::optional<Timestamp> provided = boost::none) override {
        _source = source;
        _timestamp = provided;
    }
    ReadSource getTimestampReadSource() const override {
        return _source;
    };
    boost::optional<Timestamp> getPointInTimeReadTimestamp() override {
        return _timestamp;
    }

private:
    ReadSource _source = ReadSource::kNoTimestamp;
    boost::optional<Timestamp> _timestamp;
};

class ReadSourceScopeTest : public ServiceContextTest {
public:
    OperationContext* opCtx() {
        return _opCtx.get();
    }

protected:
    void setUp() override;

    ServiceContext::UniqueOperationContext _opCtx;
};

void ReadSourceScopeTest::setUp() {
    _opCtx = getClient()->makeOperationContext();
    shard_role_details::setRecoveryUnit(_opCtx.get(),
                                        std::make_unique<RecoveryUnitMock>(),
                                        WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
}

TEST_F(ReadSourceScopeTest, RestoreReadSource) {
    shard_role_details::getRecoveryUnit(opCtx())->setTimestampReadSource(ReadSource::kProvided,
                                                                         Timestamp(1, 2));
    ASSERT_EQ(shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource(),
              ReadSource::kProvided);
    ASSERT_EQ(shard_role_details::getRecoveryUnit(opCtx())->getPointInTimeReadTimestamp(),
              Timestamp(1, 2));
    {
        ReadSourceScope scope(opCtx(), ReadSource::kNoTimestamp);
        ASSERT_EQ(shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource(),
                  ReadSource::kNoTimestamp);

        shard_role_details::getRecoveryUnit(opCtx())->setTimestampReadSource(
            ReadSource::kNoOverlap);
        ASSERT_EQ(shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource(),
                  ReadSource::kNoOverlap);
        ASSERT_EQ(shard_role_details::getRecoveryUnit(opCtx())->getPointInTimeReadTimestamp(),
                  boost::none);
    }
    ASSERT_EQ(shard_role_details::getRecoveryUnit(opCtx())->getTimestampReadSource(),
              ReadSource::kProvided);
    ASSERT_EQ(shard_role_details::getRecoveryUnit(opCtx())->getPointInTimeReadTimestamp(),
              Timestamp(1, 2));
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDifferentTenantsConflictingNamespaces) {
    auto db = "db1";
    auto tenant1 = TenantId(OID::gen());
    auto tenant2 = TenantId(OID::gen());

    DatabaseName dbName1 = DatabaseName::createDatabaseName_forTest(tenant1, db);
    DatabaseName dbName2 = DatabaseName::createDatabaseName_forTest(tenant2, db);

    AutoGetDb db1(client1.second.get(), dbName1, MODE_X);
    AutoGetDb db2(client2.second.get(), dbName2, MODE_X);

    ASSERT(shard_role_details::getLocker(client1.second.get())->isDbLockedForMode(dbName1, MODE_X));
    ASSERT(shard_role_details::getLocker(client2.second.get())->isDbLockedForMode(dbName2, MODE_X));
}

TEST_F(CatalogRAIITestFixture, AutoGetDBWithTenantHitsDeadline) {
    auto db = "db1";
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(TenantId(OID::gen()), db);

    Lock::DBLock dbLock1(client1.second.get(), dbName, MODE_X);
    ASSERT(shard_role_details::getLocker(client1.second.get())->isDbLockedForMode(dbName, MODE_X));
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), dbName, MODE_X, Date_t::now() + timeoutMs); },
        timeoutMs);
}

}  // namespace
}  // namespace mongo
