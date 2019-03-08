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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/catalog/database_holder_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class CatalogRAIITestFixture : public ServiceContextTest {
public:
    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

    ClientAndCtx makeClientWithLocker(const std::string& clientName) {
        auto client = getServiceContext()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        opCtx->swapLockState(stdx::make_unique<LockerImpl>());
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    const NamespaceString nss = NamespaceString("test", "coll");
    const Milliseconds timeoutMs = Seconds(1);
    const ClientAndCtx client1 = makeClientWithLocker("client1");
    const ClientAndCtx client2 = makeClientWithLocker("client2");

private:
    void setUp() override;
};

void CatalogRAIITestFixture::setUp() {
    DatabaseHolder::set(getServiceContext(), std::make_unique<DatabaseHolderMock>());
}

void failsWithLockTimeout(stdx::function<void()> func, Milliseconds timeoutMillis) {
    Date_t t1 = Date_t::now();
    try {
        func();
        FAIL("Should have gotten an exception due to timeout");
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>& ex) {
        log() << ex;
        Date_t t2 = Date_t::now();
        ASSERT_GTE(t2 - t1, timeoutMillis);
    }
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.db(), MODE_X, Date_t::now() + timeoutMs); },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetDBGlobalLockDeadline) {
    Lock::GlobalLock gLock1(
        client1.second.get(), MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(gLock1.isLocked());
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.db(), MODE_X, Date_t::now() + timeoutMs); },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDeadlineNow) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    AutoGetDb db(client2.second.get(), nss.db(), MODE_IX, Date_t::now());
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.db(), MODE_X, Date_t::now()); },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetDBDeadlineMin) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    AutoGetDb db(client2.second.get(), nss.db(), MODE_IX, Date_t::now());
    failsWithLockTimeout(
        [&] { AutoGetDb db(client2.second.get(), nss.db(), MODE_X, Date_t::now()); },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetOrCreateDbDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetOrCreateDb db(client2.second.get(), nss.db(), MODE_X, Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionCollLockDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get()->lockState(), nss.toString(), MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_IX,
                                   MODE_X,
                                   AutoGetCollection::ViewMode::kViewsForbidden,
                                   Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDBLockDeadline) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_X);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_X));
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   MODE_X,
                                   AutoGetCollection::ViewMode::kViewsForbidden,
                                   Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionGlobalLockDeadline) {
    Lock::GlobalLock gLock1(
        client1.second.get(), MODE_X, Date_t::now(), Lock::InterruptBehavior::kThrow);
    ASSERT(client1.second->lockState()->isLocked());
    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_X,
                                   MODE_X,
                                   AutoGetCollection::ViewMode::kViewsForbidden,
                                   Date_t::now() + timeoutMs);
        },
        timeoutMs);
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDeadlineNow) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get()->lockState(), nss.toString(), MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));

    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_IX,
                                   MODE_X,
                                   AutoGetCollection::ViewMode::kViewsForbidden,
                                   Date_t::now());
        },
        Milliseconds(0));
}

TEST_F(CatalogRAIITestFixture, AutoGetCollectionDeadlineMin) {
    Lock::DBLock dbLock1(client1.second.get(), nss.db(), MODE_IX);
    ASSERT(client1.second->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
    Lock::CollectionLock collLock1(client1.second.get()->lockState(), nss.toString(), MODE_X);
    ASSERT(client1.second->lockState()->isCollectionLockedForMode(nss, MODE_X));

    failsWithLockTimeout(
        [&] {
            AutoGetCollection coll(client2.second.get(),
                                   nss,
                                   MODE_IX,
                                   MODE_X,
                                   AutoGetCollection::ViewMode::kViewsForbidden,
                                   Date_t());
        },
        Milliseconds(0));
}

}  // namespace
}  // namespace mongo
