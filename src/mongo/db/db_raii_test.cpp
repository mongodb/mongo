/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/db_raii.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class DBRAIITestFixture : public CatalogTestFixture {
public:
    DBRAIITestFixture() : CatalogTestFixture("wiredTiger") {}
    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

    ClientAndCtx makeClientWithLocker(const std::string& clientName) {
        auto client = getServiceContext()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        client->swapLockState(std::make_unique<LockerImpl>());
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    const NamespaceString nss = NamespaceString("test", "coll");
    const Milliseconds timeoutMs = Seconds(1);
    const ClientAndCtx client1 = makeClientWithLocker("client1");
    const ClientAndCtx client2 = makeClientWithLocker("client2");
};

void failsWithLockTimeout(std::function<void()> func, Milliseconds timeoutMillis) {
    Date_t t1 = Date_t::now();
    try {
        func();
        FAIL("Should have gotten an exception due to timeout");
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>& ex) {
        LOGV2(20578, "{ex}", "ex"_attr = ex);
        Date_t t2 = Date_t::now();
        ASSERT_GTE(t2 - t1, timeoutMillis);
    }
}

TEST_F(DBRAIITestFixture, AutoGetCollectionForReadCollLockDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollectionForRead acfr(client2.second.get(),
                                          nss,
                                          AutoGetCollection::ViewMode::kViewsForbidden,
                                          Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(DBRAIITestFixture, AutoGetCollectionForReadDBLockDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollectionForRead coll(client2.second.get(),
                                          nss,
                                          AutoGetCollection::ViewMode::kViewsForbidden,
                                          Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(DBRAIITestFixture, AutoGetCollectionForReadGlobalLockDeadline) {
    Lock::GlobalLock gLock1(client1.second.get(), MODE_X);
    ASSERT(client1.second->lockState()->isLocked());
    failsWithLockTimeout(
        [&] {
            AutoGetCollectionForRead coll(client2.second.get(),
                                          nss,
                                          AutoGetCollection::ViewMode::kViewsForbidden,
                                          Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(DBRAIITestFixture, AutoGetCollectionForReadDeadlineNow) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));

    failsWithLockTimeout(
        [&] {
            AutoGetCollectionForRead coll(client2.second.get(),
                                          nss,
                                          AutoGetCollection::ViewMode::kViewsForbidden,
                                          Date_t::now());
        },
        Milliseconds(0));
}

TEST_F(DBRAIITestFixture, AutoGetCollectionForReadDeadlineMin) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get(), nss, MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));

    failsWithLockTimeout(
        [&] {
            AutoGetCollectionForRead coll(
                client2.second.get(), nss, AutoGetCollection::ViewMode::kViewsForbidden, Date_t());
        },
        Milliseconds(0));
}

TEST_F(DBRAIITestFixture, AutoGetCollectionForReadDBLockCompatibleXNoCollection) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));

    AutoGetCollectionForRead coll(client2.second.get(), nss);
}

TEST_F(DBRAIITestFixture, AutoGetCollectionForReadDBLockCompatibleXCollectionExists) {
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(
        storageInterface()->createCollection(client1.second.get(), nss, defaultCollectionOptions));

    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));

    AutoGetCollectionForRead coll(client2.second.get(), nss);
}

TEST_F(DBRAIITestFixture, AutoGetCollectionForReadDBLockCompatibleXCollectionExistsReadSource) {
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(
        storageInterface()->createCollection(client1.second.get(), nss, defaultCollectionOptions));

    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    auto opCtx = client2.second.get();
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                                  Timestamp(1, 2));
    ASSERT_THROWS_CODE(
        AutoGetCollectionForRead(opCtx, nss), AssertionException, ErrorCodes::SnapshotUnavailable);
}


TEST_F(DBRAIITestFixture,
       AutoGetCollectionForReadDBLockCompatibleXCollectionExistsSecondaryNoLastApplied) {
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(
        storageInterface()->createCollection(client1.second.get(), nss, defaultCollectionOptions));
    ASSERT_OK(repl::ReplicationCoordinator::get(client1.second.get())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));

    AutoGetCollectionForRead coll(client2.second.get(), nss);
}

TEST_F(DBRAIITestFixture,
       AutoGetCollectionForReadDBLockCompatibleXCollectionExistsSecondaryLastApplied) {
    auto replCoord = repl::ReplicationCoordinator::get(client1.second.get());
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(
        storageInterface()->createCollection(client1.second.get(), nss, defaultCollectionOptions));
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
    repl::OpTime opTime(Timestamp(200, 1), 1);
    replCoord->setMyLastAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds(1)});
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));

    AutoGetCollectionForRead coll(client2.second.get(), nss);
}

TEST_F(DBRAIITestFixture,
       AutoGetCollectionForReadDBLockCompatibleXCollectionExistsSecondaryLastAppliedNested) {
    // This test simulates a nested lock situation where the code would normally attempt to acquire
    // the PBWM, but is stymied.
    auto replCoord = repl::ReplicationCoordinator::get(client1.second.get());
    CollectionOptions defaultCollectionOptions;
    ASSERT_OK(
        storageInterface()->createCollection(client1.second.get(), nss, defaultCollectionOptions));
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
    // Note that when the collection was created, above, the system chooses a minimum snapshot time
    // for the collection.  If we now manually set our last applied time to something very early, we
    // will be guaranteed to hit the logic that triggers when the minimum snapshot time is greater
    // than the read-at time, since we default to reading at last-applied when in SECONDARY state.
    repl::OpTime opTime(Timestamp(2, 1), 1);
    replCoord->setMyLastAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds(1)});
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    AutoGetCollectionForRead coll(client2.second.get(), NamespaceString("local.system.js"));

    // The current code uasserts in this situation, so we confirm that happens here.
    ASSERT_THROWS_CODE(AutoGetCollectionForRead(client2.second.get(), nss),
                       DBException,
                       ErrorCodes::SnapshotUnavailable);
}


}  // namespace
}  // namespace mongo
