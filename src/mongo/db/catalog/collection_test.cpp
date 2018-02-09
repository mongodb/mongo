/**
 *    Copyright 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

class CollectionTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    void makeCapped(NamespaceString nss, long long cappedSize = 8192);
    // Use StorageInterface to access storage features below catalog interface.
    std::unique_ptr<repl::StorageInterface> _storage;
    ServiceContext::UniqueOperationContext _opCtxOwner;
    OperationContext* _opCtx = nullptr;
};

void CollectionTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();

    // Set up ReplicationCoordinator and ensure that we are primary.
    auto replCoord = stdx::make_unique<repl::ReplicationCoordinatorMock>(service);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    repl::ReplicationCoordinator::set(service, std::move(replCoord));

    _storage = stdx::make_unique<repl::StorageInterfaceImpl>();
    _opCtxOwner = cc().makeOperationContext();
    _opCtx = _opCtxOwner.get();
}

void CollectionTest::tearDown() {
    _storage = {};
    _opCtxOwner = {};

    // Tear down mongod.
    ServiceContextMongoDTest::tearDown();
}

void CollectionTest::makeCapped(NamespaceString nss, long long cappedSize) {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = cappedSize;  // Maximum size of capped collection in bytes.
    ASSERT_OK(_storage->createCollection(_opCtx, nss, options));
}

TEST_F(CollectionTest, CappedNotifierKillAndIsDead) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(_opCtx, nss);
    Collection* col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    ASSERT_FALSE(notifier->isDead());
    notifier->kill();
    ASSERT(notifier->isDead());
}

TEST_F(CollectionTest, CappedNotifierTimeouts) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(_opCtx, nss);
    Collection* col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    ASSERT_EQ(notifier->getVersion(), 0u);

    auto before = Date_t::now();
    notifier->waitUntil(0u, before + Milliseconds(25));
    auto after = Date_t::now();
    ASSERT_GTE(after - before, Milliseconds(25));
    ASSERT_EQ(notifier->getVersion(), 0u);
}

TEST_F(CollectionTest, CappedNotifierWaitAfterNotifyIsImmediate) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(_opCtx, nss);
    Collection* col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();

    auto prevVersion = notifier->getVersion();
    notifier->notifyAll();
    auto thisVersion = prevVersion + 1;
    ASSERT_EQ(notifier->getVersion(), thisVersion);

    auto before = Date_t::now();
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
}

TEST_F(CollectionTest, CappedNotifierWaitUntilAsynchronousNotifyAll) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(_opCtx, nss);
    Collection* col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    stdx::thread thread([before, prevVersion, &notifier] {
        notifier->waitUntil(prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->notifyAll();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), thisVersion);
}

TEST_F(CollectionTest, CappedNotifierWaitUntilAsynchronousKill) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(_opCtx, nss);
    Collection* col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();

    auto before = Date_t::now();
    stdx::thread thread([before, prevVersion, &notifier] {
        notifier->waitUntil(prevVersion, before + Milliseconds(25));
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        notifier->kill();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), prevVersion);
}

TEST_F(CollectionTest, HaveCappedWaiters) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(_opCtx, nss);
    Collection* col = acfr.getCollection();
    ASSERT_FALSE(col->haveCappedWaiters());
    {
        auto notifier = col->getCappedInsertNotifier();
        ASSERT(col->haveCappedWaiters());
    }
    ASSERT_FALSE(col->haveCappedWaiters());
}

TEST_F(CollectionTest, NotifyCappedWaitersIfNeeded) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(_opCtx, nss);
    Collection* col = acfr.getCollection();
    col->notifyCappedWaitersIfNeeded();
    {
        auto notifier = col->getCappedInsertNotifier();
        ASSERT_EQ(notifier->getVersion(), 0u);
        col->notifyCappedWaitersIfNeeded();
        ASSERT_EQ(notifier->getVersion(), 1u);
    }
}

TEST_F(CollectionTest, AsynchronouslyNotifyCappedWaitersIfNeeded) {
    NamespaceString nss("test.t");
    makeCapped(nss);

    AutoGetCollectionForRead acfr(_opCtx, nss);
    Collection* col = acfr.getCollection();
    auto notifier = col->getCappedInsertNotifier();
    auto prevVersion = notifier->getVersion();
    auto thisVersion = prevVersion + 1;

    auto before = Date_t::now();
    notifier->waitUntil(prevVersion, before + Milliseconds(25));
    stdx::thread thread([before, prevVersion, col] {
        auto after = Date_t::now();
        ASSERT_GTE(after - before, Milliseconds(25));
        col->notifyCappedWaitersIfNeeded();
    });
    notifier->waitUntil(prevVersion, before + Seconds(25));
    auto after = Date_t::now();
    ASSERT_LT(after - before, Seconds(25));
    ASSERT_GTE(after - before, Milliseconds(25));
    thread.join();
    ASSERT_EQ(notifier->getVersion(), thisVersion);
}
}  // namespace
