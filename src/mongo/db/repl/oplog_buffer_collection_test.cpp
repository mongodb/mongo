/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class OplogBufferCollectionTest : public ServiceContextMongoDTest {
protected:
    Client* getClient() const;

protected:
    ServiceContext::UniqueOperationContext makeOperationContext() const;

    StorageInterface* _storageInterface = nullptr;
    ServiceContext::UniqueOperationContext _txn;

private:
    void setUp() override;
    void tearDown() override;
};

void OplogBufferCollectionTest::setUp() {
    ServiceContextMongoDTest::setUp();

    // Initializes cc() used in ServiceContextMongoD::_newOpCtx().
    Client::initThreadIfNotAlready("OplogBufferCollectionTest");

    auto serviceContext = getGlobalServiceContext();

    // AutoGetCollectionForRead requires a valid replication coordinator in order to check the shard
    // version.
    ReplSettings replSettings;
    replSettings.setOplogSizeBytes(5 * 1024 * 1024);
    ReplicationCoordinator::set(serviceContext,
                                stdx::make_unique<ReplicationCoordinatorMock>(replSettings));

    auto storageInterface = stdx::make_unique<StorageInterfaceImpl>();
    _storageInterface = storageInterface.get();
    StorageInterface::set(serviceContext, std::move(storageInterface));

    _txn = makeOperationContext();
}

void OplogBufferCollectionTest::tearDown() {
    _txn.reset();

    _storageInterface = nullptr;

    ServiceContextMongoDTest::tearDown();
}

ServiceContext::UniqueOperationContext OplogBufferCollectionTest::makeOperationContext() const {
    return cc().makeOperationContext();
}

Client* OplogBufferCollectionTest::getClient() const {
    return &cc();
}

/**
 * Generates a unique namespace from the test registration agent.
 */
template <typename T>
NamespaceString makeNamespace(const T& t, const char* suffix = "") {
    return NamespaceString("local." + t.getSuiteName() + "_" + t.getTestName() + suffix);
}

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
BSONObj makeOplogEntry(int t) {
    return BSON("ts" << Timestamp(t, t) << "h" << t << "ns"
                     << "a.a"
                     << "v"
                     << 2
                     << "op"
                     << "i"
                     << "o"
                     << BSON("_id" << t << "a" << t));
}

TEST_F(OplogBufferCollectionTest, DefaultNamespace) {
    ASSERT_EQUALS(OplogBufferCollection::getDefaultNamespace(),
                  OplogBufferCollection(_storageInterface).getNamespace());
}

TEST_F(OplogBufferCollectionTest, GetNamespace) {
    auto nss = makeNamespace(_agent);
    ASSERT_EQUALS(nss, OplogBufferCollection(_storageInterface, nss).getNamespace());
}

void testStartupCreatesCollection(OperationContext* txn,
                                  StorageInterface* storageInterface,
                                  const NamespaceString& nss) {
    OplogBufferCollection oplogBuffer(storageInterface, nss);

    // Collection should not exist until startup() is called.
    ASSERT_FALSE(AutoGetCollectionForRead(txn, nss).getCollection());

    oplogBuffer.startup(txn);
    ASSERT_TRUE(AutoGetCollectionForRead(txn, nss).getCollection());
}

TEST_F(OplogBufferCollectionTest, StartupWithDefaultNamespaceCreatesCollection) {
    auto nss = OplogBufferCollection::getDefaultNamespace();
    ASSERT_FALSE(nss.isOplog());
    testStartupCreatesCollection(_txn.get(), _storageInterface, nss);
}

TEST_F(OplogBufferCollectionTest, StartupWithUserProvidedNamespaceCreatesCollection) {
    testStartupCreatesCollection(_txn.get(), _storageInterface, makeNamespace(_agent));
}

DEATH_TEST_F(OplogBufferCollectionTest,
             StartupWithOplogNamespaceTriggersFatalAssertion,
             "Fatal assertion 40154 Location28838: cannot create a non-capped oplog collection") {
    testStartupCreatesCollection(_txn.get(), _storageInterface, NamespaceString("local.oplog.Z"));
}

TEST_F(OplogBufferCollectionTest, ShutdownDropsCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
    oplogBuffer.shutdown(_txn.get());
    ASSERT_FALSE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
}

TEST_F(OplogBufferCollectionTest, extractEmbeddedOplogDocumentChangesIdToTimestamp) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    const BSONObj expectedOp = makeOplogEntry(1);
    BSONObj originalOp = BSON("_id" << Timestamp(1, 1) << "entry" << expectedOp);
    ASSERT_EQUALS(expectedOp, OplogBufferCollection::extractEmbeddedOplogDocument(originalOp));
}

TEST_F(OplogBufferCollectionTest, addIdToDocumentChangesTimestampToId) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    const BSONObj originalOp = makeOplogEntry(1);
    BSONObj expectedOp = BSON("_id" << Timestamp(1, 1) << "entry" << originalOp);
    auto testOpPair = OplogBufferCollection::addIdToDocument(originalOp);
    ASSERT_EQUALS(expectedOp, testOpPair.first);
    ASSERT_EQUALS(Timestamp(1, 1), testOpPair.second);
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushAllNonBlockingAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_TRUE(oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end()));
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[0],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog,
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushEvenIfFullAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    auto sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_EQUALS(sentinels.size(), 0UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog,
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PeekDoesNotRemoveDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog,
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PeekWithNoDocumentsReturnsFalse) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PopRemovesDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PopWithNoDocumentsReturnsFalse) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PopAndPeekReturnDocumentsInOrder) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(2), makeOplogEntry(1), makeOplogEntry(3),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_TRUE(oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end()));
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[2],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[1],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[0],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
}

TEST_F(OplogBufferCollectionTest, LastObjectPushedReturnsNewestOplogEntry) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1), makeOplogEntry(3), makeOplogEntry(2),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_TRUE(oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end()));
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    auto doc = oplogBuffer.lastObjectPushed(_txn.get());
    ASSERT_EQUALS(*doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);
}

TEST_F(OplogBufferCollectionTest, LastObjectPushedReturnsNoneWithNoEntries) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());

    auto doc = oplogBuffer.lastObjectPushed(_txn.get());
    ASSERT_EQUALS(doc, boost::none);
}

TEST_F(OplogBufferCollectionTest, IsEmptyReturnsTrueWhenEmptyAndFalseWhenNot) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_TRUE(oplogBuffer.isEmpty());
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    ASSERT_FALSE(oplogBuffer.isEmpty());
}

TEST_F(OplogBufferCollectionTest, ClearClearsCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    oplogBuffer.clear(_txn.get());
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_FALSE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
}

TEST_F(OplogBufferCollectionTest, BlockingPopBlocksAndRemovesDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(2U);
    BSONObj oplog = makeOplogEntry(1);
    BSONObj doc;
    std::size_t count = 0;

    stdx::thread poppingThread([&]() {
        Client::initThread("poppingThread");
        barrier.countDownAndWait();
        doc = oplogBuffer.blockingPop(makeOperationContext().get());
        count = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.push(_txn.get(), oplog);
    poppingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_EQUALS(doc, oplog);
    ASSERT_EQUALS(count, 0UL);
}

TEST_F(OplogBufferCollectionTest, TwoBlockingPopsBlockAndRemoveDocuments) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(3U);
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1), makeOplogEntry(2), makeOplogEntry(3),
    };
    BSONObj doc1;
    BSONObj doc2;

    stdx::thread poppingThread1([&]() {
        Client::initThread("poppingThread1");
        barrier.countDownAndWait();
        doc1 = oplogBuffer.blockingPop(makeOperationContext().get());
    });

    stdx::thread poppingThread2([&]() {
        Client::initThread("poppingThread2");
        barrier.countDownAndWait();
        doc2 = oplogBuffer.blockingPop(makeOperationContext().get());
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end());
    poppingThread1.join();
    poppingThread2.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_NOT_EQUALS(doc1, doc2);
    ASSERT_TRUE(doc1 == oplog[0] || doc1 == oplog[1]);
    ASSERT_TRUE(doc2 == oplog[0] || doc2 == oplog[1]);
}

TEST_F(OplogBufferCollectionTest, BlockingPeekBlocksAndFindsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(2U);
    BSONObj oplog = makeOplogEntry(1);
    BSONObj doc;
    bool success = false;
    std::size_t count = 0;

    stdx::thread peekingThread([&]() {
        Client::initThread("peekingThread");
        barrier.countDownAndWait();
        success = oplogBuffer.blockingPeek(makeOperationContext().get(), &doc, Seconds(30));
        count = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.push(_txn.get(), oplog);
    peekingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success);
    ASSERT_EQUALS(doc, oplog);
    ASSERT_EQUALS(count, 1UL);
}

TEST_F(OplogBufferCollectionTest, TwoBlockingPeeksBlockAndFindSameDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(3U);
    BSONObj oplog = makeOplogEntry(1);
    BSONObj doc1;
    bool success1 = false;
    std::size_t count1 = 0;

    BSONObj doc2;
    bool success2 = false;
    std::size_t count2 = 0;

    stdx::thread peekingThread1([&]() {
        Client::initThread("peekingThread1");
        barrier.countDownAndWait();
        success1 = oplogBuffer.blockingPeek(makeOperationContext().get(), &doc1, Seconds(30));
        count1 = oplogBuffer.getCount();
    });

    stdx::thread peekingThread2([&]() {
        Client::initThread("peekingThread2");
        barrier.countDownAndWait();
        success2 = oplogBuffer.blockingPeek(makeOperationContext().get(), &doc2, Seconds(30));
        count2 = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.push(_txn.get(), oplog);
    peekingThread1.join();
    peekingThread2.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success1);
    ASSERT_EQUALS(doc1, oplog);
    ASSERT_EQUALS(count1, 1UL);
    ASSERT_TRUE(success2);
    ASSERT_EQUALS(doc2, oplog);
    ASSERT_EQUALS(count2, 1UL);
}

TEST_F(OplogBufferCollectionTest, BlockingPeekBlocksAndTimesOutWhenItDoesNotFindDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    BSONObj doc;
    bool success = false;
    std::size_t count = 0;

    stdx::thread peekingThread([&]() {
        Client::initThread("peekingThread");
        success = oplogBuffer.blockingPeek(makeOperationContext().get(), &doc, Seconds(1));
        count = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    peekingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_FALSE(success);
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(count, 0UL);
}

TEST_F(OplogBufferCollectionTest, PushPushesonSentinelsProperly) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        BSONObj(), makeOplogEntry(1), BSONObj(), BSONObj(), makeOplogEntry(2), BSONObj(),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog[0]);
    oplogBuffer.push(_txn.get(), oplog[1]);
    oplogBuffer.push(_txn.get(), oplog[2]);
    oplogBuffer.push(_txn.get(), oplog[3]);
    oplogBuffer.push(_txn.get(), oplog[4]);
    oplogBuffer.push(_txn.get(), oplog[5]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 6UL);

    auto sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_EQUALS(sentinels.size(), 4UL);
    ASSERT_EQUALS(sentinels.front(), Timestamp(0, 0));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(1, 1));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(1, 1));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(2, 2));

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[4],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[1],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PushEvenIfFullPushesOnSentinelsProperly) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        BSONObj(), makeOplogEntry(1), BSONObj(), BSONObj(), makeOplogEntry(2), BSONObj(),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[0]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[1]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[2]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[3]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[4]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[5]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 6UL);

    auto sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_EQUALS(sentinels.size(), 4UL);
    ASSERT_EQUALS(sentinels.front(), Timestamp(0, 0));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(1, 1));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(1, 1));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(2, 2));

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[4],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[1],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }
}

DEATH_TEST_F(OplogBufferCollectionTest,
             PushAllNonBlockingWithSentinelTriggersInvariantFailure,
             "Invariant failure !orig.isEmpty()") {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        BSONObj(), makeOplogEntry(1),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end());
}

TEST_F(OplogBufferCollectionTest, SentinelInMiddleIsReturnedInOrder) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1), makeOplogEntry(2), BSONObj(), makeOplogEntry(3),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[0]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[1]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[2]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[3]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 4UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[3],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[1],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[0],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }

    auto sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_EQUALS(sentinels.size(), 1UL);
    ASSERT_EQUALS(sentinels.front(), Timestamp(2, 2));

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 4UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_EQUALS(sentinels.size(), 0UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[3]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[3]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
}

TEST_F(OplogBufferCollectionTest, SentinelAtBeginningIsReturnedAtBeginning) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {BSONObj(), makeOplogEntry(1)};
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[0]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[1],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }

    auto sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_EQUALS(sentinels.size(), 1UL);
    ASSERT_EQUALS(sentinels.front(), Timestamp(0, 0));

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
}

TEST_F(OplogBufferCollectionTest, SentinelAtEndIsReturnedAtEnd) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {makeOplogEntry(1), BSONObj()};
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[0]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[0],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }

    auto sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_EQUALS(sentinels.size(), 1UL);
    ASSERT_EQUALS(sentinels.front(), Timestamp(1, 1));

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
}

TEST_F(OplogBufferCollectionTest, MultipleSentinelsAreReturnedInOrder) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        BSONObj(), makeOplogEntry(1), BSONObj(), BSONObj(), makeOplogEntry(2), BSONObj(),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[0]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[1]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[2]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[3]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[4]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[5]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 6UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[4],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[1],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
    }

    auto sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_EQUALS(sentinels.size(), 4UL);
    ASSERT_EQUALS(sentinels.front(), Timestamp(0, 0));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(1, 1));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(1, 1));
    sentinels.pop();
    ASSERT_EQUALS(sentinels.front(), Timestamp(2, 2));

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 6UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 5UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 5UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 4UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 4UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[4]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[4]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
}

TEST_F(OplogBufferCollectionTest, BlockingPopBlocksAndRemovesSentinel) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(2U);
    BSONObj oplog;
    BSONObj doc;
    std::size_t count = 0;

    stdx::thread poppingThread([&]() {
        Client::initThread("poppingThread");
        barrier.countDownAndWait();
        doc = oplogBuffer.blockingPop(makeOperationContext().get());
        count = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    poppingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(count, 0UL);
}

TEST_F(OplogBufferCollectionTest, TwoBlockingPopsBlockAndRemoveDocumentsWithSentinel) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(3U);
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1), BSONObj(), makeOplogEntry(3),
    };
    BSONObj doc1;
    BSONObj doc2;

    stdx::thread poppingThread1([&]() {
        Client::initThread("poppingThread1");
        barrier.countDownAndWait();
        doc1 = oplogBuffer.blockingPop(makeOperationContext().get());
    });

    stdx::thread poppingThread2([&]() {
        Client::initThread("poppingThread2");
        barrier.countDownAndWait();
        doc2 = oplogBuffer.blockingPop(makeOperationContext().get());
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[0]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[1]);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog[2]);

    poppingThread1.join();
    poppingThread2.join();

    auto sentinels = oplogBuffer.getSentinels_forTest();
    ASSERT_TRUE(sentinels.empty());

    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_NOT_EQUALS(doc1, doc2);
    ASSERT_TRUE(doc1 == oplog[0] || doc1.isEmpty());
    ASSERT_TRUE(doc2 == oplog[0] || doc2.isEmpty());
}

TEST_F(OplogBufferCollectionTest, BlockingPeekBlocksAndFindsSentinel) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(2U);
    BSONObj oplog;
    BSONObj doc;
    bool success = false;
    std::size_t count = 0;

    stdx::thread peekingThread([&]() {
        Client::initThread("peekingThread");
        barrier.countDownAndWait();
        success = oplogBuffer.blockingPeek(makeOperationContext().get(), &doc, Seconds(30));
        count = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    peekingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success);
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(count, 1UL);
}

TEST_F(OplogBufferCollectionTest, TwoBlockingPeeksBlockAndFindSameSentinel) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(3U);
    BSONObj oplog;
    BSONObj doc1;
    bool success1 = false;
    std::size_t count1 = 0;

    BSONObj doc2;
    bool success2 = false;
    std::size_t count2 = 0;

    stdx::thread peekingThread1([&]() {
        Client::initThread("peekingThread1");
        barrier.countDownAndWait();
        success1 = oplogBuffer.blockingPeek(makeOperationContext().get(), &doc1, Seconds(30));
        count1 = oplogBuffer.getCount();
    });

    stdx::thread peekingThread2([&]() {
        Client::initThread("peekingThread2");
        barrier.countDownAndWait();
        success2 = oplogBuffer.blockingPeek(makeOperationContext().get(), &doc2, Seconds(30));
        count2 = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    peekingThread1.join();
    peekingThread2.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success1);
    ASSERT_TRUE(doc1.isEmpty());
    ASSERT_EQUALS(count1, 1UL);
    ASSERT_TRUE(success2);
    ASSERT_TRUE(doc2.isEmpty());
    ASSERT_EQUALS(count2, 1UL);
}


}  // namespace
