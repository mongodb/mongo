/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional/optional_io.hpp>

#include "mongo/db/catalog/database_holder_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class CatalogRAIITestFixture : public ServiceContextTest {
public:
    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

    ClientAndCtx makeClientWithLocker(const std::string& clientName) {
        auto client = getServiceContext()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        client->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    const NamespaceString nss = NamespaceString("test", "coll");
    const NamespaceString kSecondaryNss1 = NamespaceString("test", "secondaryColl1");
    const NamespaceString kSecondaryNss2 = NamespaceString("test", "secondaryColl2");
    const NamespaceString kSecondaryNss3 = NamespaceString("test", "secondaryColl3");
    const NamespaceString kSecondaryNssOtherDb1 = NamespaceString("test2", "secondaryColl1");
    const NamespaceString kSecondaryNssOtherDb2 = NamespaceString("test2", "secondaryColl2");
    const Milliseconds timeoutMs = Seconds(1);
    const ClientAndCtx client1 = makeClientWithLocker("client1");
    const ClientAndCtx client2 = makeClientWithLocker("client2");

private:
    void setUp() override;
};

void CatalogRAIITestFixture::setUp() {
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
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_X));
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
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
    AutoGetDb db(client2.second.get(), nss.dbName(), MODE_IX);
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.dbName(), MODE_X, Date_t::now()); },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDeadlineMin) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
    AutoGetDb db(client2.second.get(), nss.dbName(), MODE_IX);
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.dbName(), MODE_X, Date_t{}); },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionCollLockDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDBLockDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionGlobalLockDeadline) {
    Lock::GlobalLock gLock1(client1.second.get(), MODE_X);
    ASSERT(client1.second->lockState()->isLocked());
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDeadlineNow) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));

    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   Date_t::now());
        },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDeadlineMin) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));

    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   Date_t());
        },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionNotCompatibleWithRSTLExclusiveLock) {
    Lock::GlobalLock gLock1(client1.second.get(), MODE_X);
    ASSERT(client1.second->lockState()->isLocked());

    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_IX,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDBLockCompatibleX) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));

    AutoGetCollection coll(client2.second.get(), nss, MODE_X);
}

// Test multiple collections being locked on the same database.
TEST_F(CatalogRAIITestFixture, AutoGetCollectionSecondaryNamespacesSingleDb) {
    auto opCtx1 = client1.second.get();

    std::vector<NamespaceStringOrUUID> secondaryNamespaces{NamespaceStringOrUUID(kSecondaryNss1),
                                                           NamespaceStringOrUUID(kSecondaryNss2)};

    boost::optional<AutoGetCollection> autoGetColl;
    autoGetColl.emplace(opCtx1,
                        nss,
                        MODE_IS,
                        AutoGetCollectionViewMode::kViewsForbidden,
                        Date_t::max(),
                        secondaryNamespaces);

    ASSERT(opCtx1->lockState()->isRSTLLocked());
    ASSERT(opCtx1->lockState()->isReadLocked());  // Global lock check
    ASSERT(opCtx1->lockState()->isDbLockedForMode(nss.dbName(), MODE_IS));
    ASSERT(opCtx1->lockState()->isDbLockedForMode(kSecondaryNss1.db(), MODE_IS));
    ASSERT(opCtx1->lockState()->isDbLockedForMode(kSecondaryNss2.db(), MODE_IS));
    ASSERT(opCtx1->lockState()->isCollectionLockedForMode(nss, MODE_IS));
    ASSERT(opCtx1->lockState()->isCollectionLockedForMode(kSecondaryNss1, MODE_IS));
    ASSERT(opCtx1->lockState()->isCollectionLockedForMode(kSecondaryNss2, MODE_IS));

    ASSERT(!opCtx1->lockState()->isRSTLExclusive());
    ASSERT(!opCtx1->lockState()->isGlobalLockedRecursively());
    ASSERT(!opCtx1->lockState()->isWriteLocked());
    ASSERT(!opCtx1->lockState()->isDbLockedForMode(kSecondaryNssOtherDb1.db(), MODE_IS));
    ASSERT(!opCtx1->lockState()->isDbLockedForMode(kSecondaryNssOtherDb2.db(), MODE_IS));
    ASSERT(!opCtx1->lockState()->isCollectionLockedForMode(kSecondaryNssOtherDb1, MODE_IS));
    ASSERT(!opCtx1->lockState()->isCollectionLockedForMode(kSecondaryNssOtherDb2, MODE_IS));

    // All the locks should release.
    autoGetColl.reset();
    ASSERT(!opCtx1->lockState()->isLocked());  // Global lock check
}

// Test multiple collections being locked with MODE_IX. Multi-document transaction reads use MODE_IX
// instead of MODE_IS.
TEST_F(CatalogRAIITestFixture, AutoGetCollectionMultiNamespacesMODEIX) {
    auto opCtx1 = client1.second.get();

    std::vector<NamespaceStringOrUUID> secondaryNamespaces{NamespaceStringOrUUID(kSecondaryNss1),
                                                           NamespaceStringOrUUID(kSecondaryNss2)};

    boost::optional<AutoGetCollection> autoGetColl;
    autoGetColl.emplace(opCtx1,
                        nss,
                        MODE_IX,
                        AutoGetCollectionViewMode::kViewsForbidden,
                        Date_t::max(),
                        secondaryNamespaces);

    ASSERT(opCtx1->lockState()->isRSTLLocked());
    ASSERT(opCtx1->lockState()->isWriteLocked());  // Global lock check
    ASSERT(opCtx1->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
    ASSERT(opCtx1->lockState()->isDbLockedForMode(kSecondaryNss1.db(), MODE_IX));
    ASSERT(opCtx1->lockState()->isDbLockedForMode(kSecondaryNss2.db(), MODE_IX));
    ASSERT(opCtx1->lockState()->isCollectionLockedForMode(nss, MODE_IX));
    ASSERT(opCtx1->lockState()->isCollectionLockedForMode(kSecondaryNss1, MODE_IX));
    ASSERT(opCtx1->lockState()->isCollectionLockedForMode(kSecondaryNss2, MODE_IX));

    ASSERT(!opCtx1->lockState()->isRSTLExclusive());
    ASSERT(!opCtx1->lockState()->isGlobalLockedRecursively());
    ASSERT(!opCtx1->lockState()->isDbLockedForMode(kSecondaryNssOtherDb1.db(), MODE_IX));
    ASSERT(!opCtx1->lockState()->isDbLockedForMode(kSecondaryNssOtherDb2.db(), MODE_IX));
    ASSERT(!opCtx1->lockState()->isCollectionLockedForMode(kSecondaryNssOtherDb1, MODE_IX));
    ASSERT(!opCtx1->lockState()->isCollectionLockedForMode(kSecondaryNssOtherDb2, MODE_IX));

    // All the locks should release.
    autoGetColl.reset();
    ASSERT(!opCtx1->lockState()->isLocked());  // Global lock check
}

TEST_F(CatalogRAIITestFixture, AutoGetDbSecondaryNamespacesSingleDb) {
    auto opCtx1 = client1.second.get();

    boost::optional<AutoGetDb> autoGetDb;
    autoGetDb.emplace(opCtx1, nss.dbName(), MODE_IS, Date_t::max());

    ASSERT(opCtx1->lockState()->isRSTLLocked());
    ASSERT(opCtx1->lockState()->isReadLocked());  // Global lock check
    ASSERT(opCtx1->lockState()->isDbLockedForMode(nss.dbName(), MODE_IS));
    ASSERT(opCtx1->lockState()->isDbLockedForMode(kSecondaryNss1.db(), MODE_IS));
    ASSERT(opCtx1->lockState()->isDbLockedForMode(kSecondaryNss2.db(), MODE_IS));

    ASSERT(!opCtx1->lockState()->isDbLockedForMode(kSecondaryNssOtherDb1.db(), MODE_IS));
    ASSERT(!opCtx1->lockState()->isDbLockedForMode(kSecondaryNssOtherDb2.db(), MODE_IS));
    ASSERT(!opCtx1->lockState()->isRSTLExclusive());
    ASSERT(!opCtx1->lockState()->isGlobalLockedRecursively());
    ASSERT(!opCtx1->lockState()->isWriteLocked());

    // All the locks should release.
    autoGetDb.reset();
    ASSERT(!opCtx1->lockState()->isLocked());  // Global lock check.
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionMultiNssCollLockDeadline) {
    // Take a MODE_X collection lock on kSecondaryNss1.
    boost::optional<AutoGetCollection> autoGetCollWithXLock;
    autoGetCollWithXLock.emplace(client1.second.get(), kSecondaryNss1, MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(kSecondaryNss1.db(), MODE_IX));
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(kSecondaryNss1, MODE_X));

    // Now trying to take a MODE_IS lock on kSecondaryNss1 as a secondary collection should fail.
    const std::vector<NamespaceStringOrUUID> secondaryNamespacesConflict{
        NamespaceStringOrUUID(kSecondaryNss1),
        NamespaceStringOrUUID(kSecondaryNss2),
        NamespaceStringOrUUID(kSecondaryNss3)};
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_IS,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   Date_t::now() + timeoutMs,
                                   secondaryNamespacesConflict);
        },
        timeoutMs);

    {
        // Sanity check that there's no conflict without kSecondaryNss1 that's MODE_X locked.
        const std::vector<NamespaceStringOrUUID> secondaryNamespacesNoConflict{
            NamespaceStringOrUUID(kSecondaryNss2), NamespaceStringOrUUID(kSecondaryNss2)};
        AutoGetCollection collNoConflict(client2.second.get(),
                                         nss,
                                         MODE_IS,
                                         AutoGetCollectionViewMode::kViewsForbidden,
                                         Date_t::now() + timeoutMs,
                                         secondaryNamespacesNoConflict);
    }

    // Now without the MODE_X lock on kSecondaryNss1, should work fine.
    autoGetCollWithXLock.reset();
    AutoGetCollection coll(client2.second.get(),
                           nss,
                           MODE_IS,
                           AutoGetCollectionViewMode::kViewsForbidden,
                           Date_t::max(),
                           secondaryNamespacesConflict);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionLockFreeGlobalLockDeadline) {
    Lock::GlobalLock gLock1(client1.second.get(), MODE_X);
    ASSERT(client1.second->lockState()->isLocked());
    failsWithLockTimeout(
        [&] {
            AutoGetCollectionLockFree coll(
                client2.second.get(),
                nss,
                [](std::shared_ptr<const Collection>&, OperationContext*, UUID) {},
                AutoGetCollectionViewMode::kViewsForbidden,
                Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionLockFreeCompatibleWithCollectionExclusiveLock) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));

    AutoGetCollectionLockFree coll(
        client2.second.get(), nss, [](std::shared_ptr<const Collection>&, OperationContext*, UUID) {
        });
    ASSERT(client2.second->lockState()->isLocked());
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionLockFreeCompatibleWithDatabaseExclusiveLock) {
    Lock::DBLock dbLock1(client1.second.get(), nss.dbName(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.dbName(), MODE_X));

    AutoGetCollectionLockFree coll(
        client2.second.get(), nss, [](std::shared_ptr<const Collection>&, OperationContext*, UUID) {
        });
    ASSERT(client2.second->lockState()->isLocked());
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionLockFreeCompatibleWithRSTLExclusiveLock) {
    Lock::ResourceLock rstl(
        client1.second->lockState(), resourceIdReplicationStateTransitionLock, MODE_X);
    ASSERT(client1.second->lockState()->isRSTLExclusive());

    AutoGetCollectionLockFree coll(
        client2.second.get(), nss, [](std::shared_ptr<const Collection>&, OperationContext*, UUID) {
        });
    ASSERT(client2.second->lockState()->isLocked());
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
    boost::optional<Timestamp> getPointInTimeReadTimestamp(OperationContext* opCtx) override {
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
    _opCtx->setRecoveryUnit(std::make_unique<RecoveryUnitMock>(),
                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
}

TEST_F(ReadSourceScopeTest, RestoreReadSource) {
    opCtx()->recoveryUnit()->setTimestampReadSource(ReadSource::kProvided, Timestamp(1, 2));
    ASSERT_EQ(opCtx()->recoveryUnit()->getTimestampReadSource(), ReadSource::kProvided);
    ASSERT_EQ(opCtx()->recoveryUnit()->getPointInTimeReadTimestamp(opCtx()), Timestamp(1, 2));
    {
        ReadSourceScope scope(opCtx(), ReadSource::kNoTimestamp);
        ASSERT_EQ(opCtx()->recoveryUnit()->getTimestampReadSource(), ReadSource::kNoTimestamp);

        opCtx()->recoveryUnit()->setTimestampReadSource(ReadSource::kNoOverlap);
        ASSERT_EQ(opCtx()->recoveryUnit()->getTimestampReadSource(), ReadSource::kNoOverlap);
        ASSERT_EQ(opCtx()->recoveryUnit()->getPointInTimeReadTimestamp(opCtx()), boost::none);
    }
    ASSERT_EQ(opCtx()->recoveryUnit()->getTimestampReadSource(), ReadSource::kProvided);
    ASSERT_EQ(opCtx()->recoveryUnit()->getPointInTimeReadTimestamp(opCtx()), Timestamp(1, 2));
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDifferentTenantsConflictingNamespaces) {
    auto db = "db1";
    auto tenant1 = TenantId(OID::gen());
    auto tenant2 = TenantId(OID::gen());

    DatabaseName dbName1(tenant1, db);
    DatabaseName dbName2(tenant2, db);

    AutoGetDb db1(client1.second.get(), dbName1, MODE_X);
    AutoGetDb db2(client2.second.get(), dbName2, MODE_X);

    ASSERT(client1.second->lockState()->isDbLockedForMode(dbName1, MODE_X));
    ASSERT(client2.second->lockState()->isDbLockedForMode(dbName2, MODE_X));
}

TEST_F(CatalogRAIITestFixture, AutoGetDBWithTenantHitsDeadline) {
    auto db = "db1";
    DatabaseName dbName(TenantId(OID::gen()), db);

    Lock::DBLock dbLock1(client1.second.get(), dbName, MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(dbName, MODE_X));
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), dbName, MODE_X, Date_t::now() + timeoutMs); },
        timeoutMs);
}

}  // namespace
}  // namespace mongo
