/**
 *    Copyright 2017 MongoDB Inc.
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

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class DropPendingCollectionReaperTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    /**
     * Returns true if collection exists.
     */
    bool collectionExists(OperationContext* opCtx, const NamespaceString& nss);

    std::unique_ptr<StorageInterface> _storageInterface;
};

void DropPendingCollectionReaperTest::setUp() {
    ServiceContextMongoDTest::setUp();
    _storageInterface = stdx::make_unique<StorageInterfaceImpl>();
    auto service = getServiceContext();
    ReplicationCoordinator::set(service, stdx::make_unique<ReplicationCoordinatorMock>(service));
}

void DropPendingCollectionReaperTest::tearDown() {
    _storageInterface = {};
    ServiceContextMongoDTest::tearDown();
}

bool DropPendingCollectionReaperTest::collectionExists(OperationContext* opCtx,
                                                       const NamespaceString& nss) {
    return _storageInterface->getCollectionCount(opCtx, nss).isOK();
}

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

TEST_F(DropPendingCollectionReaperTest, ServiceContextDecorator) {
    auto serviceContext = getServiceContext();
    ASSERT_FALSE(DropPendingCollectionReaper::get(serviceContext));
    DropPendingCollectionReaper* reaper = new DropPendingCollectionReaper(_storageInterface.get());
    DropPendingCollectionReaper::set(serviceContext,
                                     std::unique_ptr<DropPendingCollectionReaper>(reaper));
    ASSERT_TRUE(reaper == DropPendingCollectionReaper::get(serviceContext));
    ASSERT_TRUE(reaper == DropPendingCollectionReaper::get(*serviceContext));
    ASSERT_TRUE(reaper == DropPendingCollectionReaper::get(makeOpCtx().get()));
}

TEST_F(DropPendingCollectionReaperTest, GetEarliestDropOpTimeReturnsBoostNoneOnEmptyNamespaces) {
    ASSERT_FALSE(DropPendingCollectionReaper(_storageInterface.get()).getEarliestDropOpTime());
}

TEST_F(DropPendingCollectionReaperTest, AddDropPendingNamespaceAcceptsNullDropOpTime) {
    OpTime nullDropOpTime;
    auto dpns = NamespaceString("test.foo").makeDropPendingNamespace(nullDropOpTime);
    DropPendingCollectionReaper reaper(_storageInterface.get());
    reaper.addDropPendingNamespace(nullDropOpTime, dpns);
    ASSERT_EQUALS(nullDropOpTime, *reaper.getEarliestDropOpTime());
}

TEST_F(DropPendingCollectionReaperTest,
       AddDropPendingNamespaceWithDuplicateDropOpTimeButDifferentNamespace) {
    StorageInterfaceMock storageInterfaceMock;
    std::size_t numCollectionsDropped = 0U;
    storageInterfaceMock.dropCollFn = [&](OperationContext*, const NamespaceString&) {
        numCollectionsDropped++;
        return Status::OK();
    };
    DropPendingCollectionReaper reaper(&storageInterfaceMock);

    OpTime opTime({Seconds(100), 0}, 1LL);
    auto dpns = NamespaceString("test.foo").makeDropPendingNamespace(opTime);
    reaper.addDropPendingNamespace(opTime, dpns);
    reaper.addDropPendingNamespace(opTime,
                                   NamespaceString("test.bar").makeDropPendingNamespace(opTime));

    // Drop all collections managed by reaper and confirm number of drops.
    auto opCtx = makeOpCtx();
    reaper.dropCollectionsOlderThan(opCtx.get(), opTime);
    ASSERT_EQUALS(2U, numCollectionsDropped);
}

DEATH_TEST_F(DropPendingCollectionReaperTest,
             AddDropPendingNamespaceTerminatesOnDuplicateDropOpTimeAndNamespace,
             "Failed to add drop-pending collection") {
    OpTime opTime({Seconds(100), 0}, 1LL);
    auto dpns = NamespaceString("test.foo").makeDropPendingNamespace(opTime);
    DropPendingCollectionReaper reaper(_storageInterface.get());
    reaper.addDropPendingNamespace(opTime, dpns);
    reaper.addDropPendingNamespace(opTime, dpns);
}

TEST_F(DropPendingCollectionReaperTest,
       DropCollectionsOlderThanDropsCollectionsWithDropOpTimeBeforeOrAtCommittedOpTime) {
    auto opCtx = makeOpCtx();

    // Generate optimes with secs: 10, 20, ..., 50.
    // Create corresponding drop-pending collections.
    const int n = 5U;
    OpTime opTime[n];
    NamespaceString ns[n];
    NamespaceString dpns[n];
    for (int i = 0; i < n; ++i) {
        opTime[i] = OpTime({Seconds((i + 1) * 10), 0}, 1LL);
        ns[i] = NamespaceString("test", str::stream() << "coll" << i);
        dpns[i] = ns[i].makeDropPendingNamespace(opTime[i]);
        _storageInterface->createCollection(opCtx.get(), dpns[i], {}).transitional_ignore();
    }

    // Add drop-pending namespaces with drop optimes out of order and check that
    // getEarliestDropOpTime() returns earliest optime.
    DropPendingCollectionReaper reaper(_storageInterface.get());
    ASSERT_FALSE(reaper.getEarliestDropOpTime());
    reaper.addDropPendingNamespace(opTime[1], dpns[1]);
    reaper.addDropPendingNamespace(opTime[0], dpns[0]);
    reaper.addDropPendingNamespace(opTime[2], dpns[2]);
    reaper.addDropPendingNamespace(opTime[3], dpns[3]);
    reaper.addDropPendingNamespace(opTime[4], dpns[4]);
    ASSERT_EQUALS(opTime[0], *reaper.getEarliestDropOpTime());

    // Committed optime before first drop optime has no effect.
    reaper.dropCollectionsOlderThan(opCtx.get(), OpTime({Seconds(5), 0}, 1LL));
    ASSERT_EQUALS(opTime[0], *reaper.getEarliestDropOpTime());

    // Committed optime matching second drop optime will result in the first two drop-pending
    // collections being removed.
    reaper.dropCollectionsOlderThan(opCtx.get(), opTime[1]);
    ASSERT_EQUALS(opTime[2], *reaper.getEarliestDropOpTime());
    ASSERT_FALSE(collectionExists(opCtx.get(), dpns[0]));
    ASSERT_FALSE(collectionExists(opCtx.get(), dpns[1]));
    ASSERT_TRUE(collectionExists(opCtx.get(), dpns[2]));

    // Committed optime between third and fourth optimes will result in the third collection being
    // removed.
    reaper.dropCollectionsOlderThan(opCtx.get(), OpTime({Seconds(35), 0}, 1LL));
    ASSERT_EQUALS(opTime[3], *reaper.getEarliestDropOpTime());
    ASSERT_FALSE(collectionExists(opCtx.get(), dpns[2]));
    ASSERT_TRUE(collectionExists(opCtx.get(), dpns[3]));

    // Committed optime after last optime will result in all drop-pending collections being removed.
    reaper.dropCollectionsOlderThan(opCtx.get(), OpTime({Seconds(100), 0}, 1LL));
    ASSERT_FALSE(reaper.getEarliestDropOpTime());
    ASSERT_FALSE(collectionExists(opCtx.get(), dpns[3]));
    ASSERT_FALSE(collectionExists(opCtx.get(), dpns[4]));
}

TEST_F(DropPendingCollectionReaperTest, DropCollectionsOlderThanHasNoEffectIfCollectionIsMissing) {
    OpTime optime({Seconds{1}, 0}, 1LL);
    NamespaceString ns("test.foo");
    auto dpns = ns.makeDropPendingNamespace(optime);

    DropPendingCollectionReaper reaper(_storageInterface.get());
    reaper.addDropPendingNamespace(optime, dpns);

    auto opCtx = makeOpCtx();
    reaper.dropCollectionsOlderThan(opCtx.get(), optime);
}

TEST_F(DropPendingCollectionReaperTest, DropCollectionsOlderThanLogsDropCollectionError) {
    OpTime optime({Seconds{1}, 0}, 1LL);
    NamespaceString ns("test.foo");
    auto dpns = ns.makeDropPendingNamespace(optime);

    // StorageInterfaceMock::dropCollection() returns IllegalOperation.
    StorageInterfaceMock storageInterfaceMock;

    DropPendingCollectionReaper reaper(&storageInterfaceMock);
    reaper.addDropPendingNamespace(optime, dpns);

    auto opCtx = makeOpCtx();
    startCapturingLogMessages();
    reaper.dropCollectionsOlderThan(opCtx.get(), optime);
    stopCapturingLogMessages();

    ASSERT_EQUALS(1LL, countLogLinesContaining("Failed to remove drop-pending collection"));
}

TEST_F(DropPendingCollectionReaperTest,
       DropCollectionsOlderThanDisablesReplicatedWritesWhenDroppingCollection) {
    OpTime optime({Seconds{1}, 0}, 1LL);
    NamespaceString ns("test.foo");
    auto dpns = ns.makeDropPendingNamespace(optime);

    // Override dropCollection to confirm that writes are not replicated when dropping the
    // drop-pending collection.
    StorageInterfaceMock storageInterfaceMock;
    decltype(dpns) droppedNss;
    bool writesAreReplicatedDuringDrop = true;
    storageInterfaceMock.dropCollFn = [&droppedNss, &writesAreReplicatedDuringDrop](
        OperationContext* opCtx, const NamespaceString& nss) {
        droppedNss = nss;
        writesAreReplicatedDuringDrop = opCtx->writesAreReplicated();
        return Status::OK();
    };

    DropPendingCollectionReaper reaper(&storageInterfaceMock);
    reaper.addDropPendingNamespace(optime, dpns);

    auto opCtx = makeOpCtx();
    reaper.dropCollectionsOlderThan(opCtx.get(), optime);

    ASSERT_EQUALS(dpns, droppedNss);
    ASSERT_FALSE(writesAreReplicatedDuringDrop);
}

TEST_F(DropPendingCollectionReaperTest, RollBackDropPendingCollectionRenamesCollection) {
    auto opCtx = makeOpCtx();

    // Generates optimes with secs: 10, 20, 30.
    // Creates corresponding drop-pending collections.
    const int n = 3U;
    OpTime opTime[n];
    NamespaceString ns[n];
    NamespaceString dpns[n];
    for (int i = 0; i < n; ++i) {
        opTime[i] = OpTime({Seconds((i + 1) * 10), 0}, 1LL);
        ns[i] = NamespaceString("test", str::stream() << "coll" << i);
        dpns[i] = ns[i].makeDropPendingNamespace(opTime[i]);
        ASSERT_OK(_storageInterface->createCollection(opCtx.get(), dpns[i], {}));
    }

    DropPendingCollectionReaper reaper(_storageInterface.get());
    reaper.addDropPendingNamespace(opTime[0], dpns[0]);
    reaper.addDropPendingNamespace(opTime[1], dpns[1]);
    reaper.addDropPendingNamespace(opTime[2], dpns[2]);

    // Rename at an optime not in the list returns false.
    ASSERT_FALSE(
        reaper.rollBackDropPendingCollection(opCtx.get(), OpTime({Seconds(5), 0}, 1LL), ns[0]));
    ASSERT_EQUALS(opTime[0], *reaper.getEarliestDropOpTime());
    ASSERT_TRUE(collectionExists(opCtx.get(), dpns[0]));
    ASSERT_TRUE(collectionExists(opCtx.get(), dpns[1]));
    ASSERT_TRUE(collectionExists(opCtx.get(), dpns[2]));
    ASSERT_FALSE(collectionExists(opCtx.get(), ns[0]));
    ASSERT_FALSE(collectionExists(opCtx.get(), ns[1]));
    ASSERT_FALSE(collectionExists(opCtx.get(), ns[2]));

    // Rename at an optime in the middle of the list renames the collection.
    ASSERT_TRUE(
        reaper.rollBackDropPendingCollection(opCtx.get(), OpTime({Seconds(20), 0}, 1LL), ns[1]));
    ASSERT_EQUALS(opTime[0], *reaper.getEarliestDropOpTime());
    ASSERT_TRUE(collectionExists(opCtx.get(), dpns[0]));
    ASSERT_FALSE(collectionExists(opCtx.get(), dpns[1]));
    ASSERT_TRUE(collectionExists(opCtx.get(), dpns[2]));
    ASSERT_FALSE(collectionExists(opCtx.get(), ns[0]));
    ASSERT_TRUE(collectionExists(opCtx.get(), ns[1]));
    ASSERT_FALSE(collectionExists(opCtx.get(), ns[2]));

    // Rename at an optime at the front of the list renames the collection.
    ASSERT_TRUE(
        reaper.rollBackDropPendingCollection(opCtx.get(), OpTime({Seconds(10), 0}, 1LL), ns[0]));
    ASSERT_EQUALS(opTime[2], *reaper.getEarliestDropOpTime());
    ASSERT_FALSE(collectionExists(opCtx.get(), dpns[0]));
    ASSERT_FALSE(collectionExists(opCtx.get(), dpns[1]));
    ASSERT_TRUE(collectionExists(opCtx.get(), dpns[2]));
    ASSERT_TRUE(collectionExists(opCtx.get(), ns[0]));
    ASSERT_TRUE(collectionExists(opCtx.get(), ns[1]));
    ASSERT_FALSE(collectionExists(opCtx.get(), ns[2]));
}

}  // namespace
