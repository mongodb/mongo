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
    auto serviceContext = getServiceContext();

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

TEST_F(OplogBufferCollectionTest, StartupDropsExistingCollectionBeforeCreatingNewCollection) {
    auto nss = makeNamespace(_agent);
    ASSERT_OK(_storageInterface->createCollection(_txn.get(), nss, CollectionOptions()));
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
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
    ASSERT_BSONOBJ_EQ(expectedOp, OplogBufferCollection::extractEmbeddedOplogDocument(originalOp));
}

void _assertOplogDocAndTimestampEquals(
    const BSONObj& oplog,
    const std::tuple<BSONObj, Timestamp, std::size_t>& actualDocTimestampSentinelTuple) {
    auto expectedTimestamp = oplog["ts"].timestamp();
    auto expectedDoc =
        BSON("_id" << BSON("ts" << expectedTimestamp << "s" << 0) << "entry" << oplog);
    ASSERT_BSONOBJ_EQ(expectedDoc, std::get<0>(actualDocTimestampSentinelTuple));
    ASSERT_EQUALS(expectedTimestamp, std::get<1>(actualDocTimestampSentinelTuple));
    ASSERT_EQUALS(0U, std::get<2>(actualDocTimestampSentinelTuple));
}

void _assertSentinelDocAndTimestampEquals(
    const Timestamp& expectedTimestamp,
    std::size_t expectedSentinelCount,
    const std::tuple<BSONObj, Timestamp, std::size_t>& actualDocTimestampSentinelTuple) {
    auto expectedDoc =
        BSON("_id" << BSON("ts" << expectedTimestamp << "s" << int(expectedSentinelCount)));
    ASSERT_BSONOBJ_EQ(expectedDoc, std::get<0>(actualDocTimestampSentinelTuple));
    ASSERT_EQUALS(expectedTimestamp, std::get<1>(actualDocTimestampSentinelTuple));
    ASSERT_EQUALS(expectedSentinelCount, std::get<2>(actualDocTimestampSentinelTuple));
}

TEST_F(OplogBufferCollectionTest, addIdToDocumentChangesTimestampToId) {
    const BSONObj originalOp = makeOplogEntry(1);
    _assertOplogDocAndTimestampEquals(originalOp,
                                      OplogBufferCollection::addIdToDocument(originalOp, {}, 0));
}

TEST_F(OplogBufferCollectionTest, addIdToDocumentGeneratesIdForSentinelFromLastPushedTimestamp) {
    const BSONObj oplog1 = makeOplogEntry(1);
    _assertOplogDocAndTimestampEquals(oplog1,
                                      OplogBufferCollection::addIdToDocument(oplog1, {}, 0U));
    auto ts1 = oplog1["ts"].timestamp();

    _assertSentinelDocAndTimestampEquals(
        ts1, 1U, OplogBufferCollection::addIdToDocument({}, ts1, 0U));
    _assertSentinelDocAndTimestampEquals(
        ts1, 2U, OplogBufferCollection::addIdToDocument({}, ts1, 1U));
    _assertSentinelDocAndTimestampEquals(
        ts1, 3U, OplogBufferCollection::addIdToDocument({}, ts1, 2U));

    // Processing valid oplog entry resets the sentinel count.
    const BSONObj oplog2 = makeOplogEntry(2);
    _assertOplogDocAndTimestampEquals(oplog1,
                                      OplogBufferCollection::addIdToDocument(oplog1, ts1, 3U));
    auto ts2 = oplog2["ts"].timestamp();

    _assertSentinelDocAndTimestampEquals(
        ts2, 1U, OplogBufferCollection::addIdToDocument({}, ts2, 0U));
    _assertSentinelDocAndTimestampEquals(
        ts2, 2U, OplogBufferCollection::addIdToDocument({}, ts2, 1U));
    _assertSentinelDocAndTimestampEquals(
        ts2, 3U, OplogBufferCollection::addIdToDocument({}, ts2, 2U));
}

DEATH_TEST_F(OplogBufferCollectionTest,
             addIdToDocumentWithMissingTimestampFieldTriggersInvariantFailure,
             "Invariant failure !ts.isNull()") {
    OplogBufferCollection::addIdToDocument(BSON("x" << 1), {}, 0);
}

/**
 * Check collection contents. OplogInterface returns documents in reverse natural order.
 */
void _assertDocumentsInCollectionEquals(OperationContext* txn,
                                        const NamespaceString& nss,
                                        const std::vector<BSONObj>& docs) {
    std::vector<BSONObj> reversedTransformedDocs;
    Timestamp ts;
    std::size_t sentinelCount = 0;
    for (const auto& doc : docs) {
        auto previousTimestamp = ts;
        BSONObj newDoc;
        std::tie(newDoc, ts, sentinelCount) =
            OplogBufferCollection::addIdToDocument(doc, ts, sentinelCount);
        reversedTransformedDocs.push_back(newDoc);
        if (doc.isEmpty()) {
            ASSERT_EQUALS(previousTimestamp, ts);
            continue;
        }
        ASSERT_GT(ts, previousTimestamp);
    }
    std::reverse(reversedTransformedDocs.begin(), reversedTransformedDocs.end());
    OplogInterfaceLocal oplog(txn, nss.ns());
    auto iter = oplog.makeIterator();
    for (const auto& doc : reversedTransformedDocs) {
        ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(iter->next()).first);
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushAllNonBlockingAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);
}

TEST_F(OplogBufferCollectionTest,
       PushAllNonBlockingIgnoresOperationContextIfNoDocumentsAreProvided) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> emptyOplogEntries;
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushAllNonBlocking(nullptr, emptyOplogEntries.begin(), emptyOplogEntries.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {oplog});
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushEvenIfFullAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_EQUALS(0UL, oplogBuffer.getSentinelCount_forTest());

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {oplog});
}

TEST_F(OplogBufferCollectionTest, PeekDoesNotRemoveDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog1 = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    // _peekOneSide should provide correct bound inclusion to storage engine when collection has one
    // document.
    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    BSONObj oplog2 = makeOplogEntry(2);
    oplogBuffer.push(_txn.get(), oplog2);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    // _peekOneSide should return same result after adding new oplog entry.
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {oplog1, oplog2});
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

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {});
}

TEST_F(OplogBufferCollectionTest, PopDoesNotRemoveDocumentFromCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {oplog});
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

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {});
}

TEST_F(OplogBufferCollectionTest, PopAndPeekReturnDocumentsInOrder) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1), makeOplogEntry(2), makeOplogEntry(3),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    // tryPop does not remove documents from collection.
    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);
}

TEST_F(OplogBufferCollectionTest, LastObjectPushedReturnsNewestOplogEntry) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1), makeOplogEntry(2), makeOplogEntry(3),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    auto doc = oplogBuffer.lastObjectPushed(_txn.get());
    ASSERT_BSONOBJ_EQ(*doc, oplog[2]);
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
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), 0UL);
    ASSERT_EQUALS(0U, oplogBuffer.getSentinelCount_forTest());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPushedTimestamp_forTest());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    BSONObj oplog = makeOplogEntry(1);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), std::size_t(oplog.objsize()));
    ASSERT_EQUALS(0U, oplogBuffer.getSentinelCount_forTest());
    ASSERT_EQUALS(oplog["ts"].timestamp(), oplogBuffer.getLastPushedTimestamp_forTest());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {oplog});

    BSONObj sentinel;
    oplogBuffer.push(_txn.get(), sentinel);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), std::size_t(oplog.objsize() + BSONObj().objsize()));
    ASSERT_EQUALS(1U, oplogBuffer.getSentinelCount_forTest());
    ASSERT_EQUALS(oplog["ts"].timestamp(), oplogBuffer.getLastPushedTimestamp_forTest());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {oplog, sentinel});

    BSONObj oplog2 = makeOplogEntry(2);
    oplogBuffer.push(_txn.get(), oplog2);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);
    ASSERT_EQUALS(oplogBuffer.getSize(),
                  std::size_t(oplog.objsize() + BSONObj().objsize() + oplog2.objsize()));
    ASSERT_EQUALS(0U, oplogBuffer.getSentinelCount_forTest());
    ASSERT_EQUALS(oplog2["ts"].timestamp(), oplogBuffer.getLastPushedTimestamp_forTest());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {oplog, sentinel, oplog2});

    BSONObj poppedDoc;
    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &poppedDoc));
    ASSERT_BSONOBJ_EQ(oplog, poppedDoc);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), std::size_t(BSONObj().objsize() + oplog2.objsize()));
    ASSERT_EQUALS(0U, oplogBuffer.getSentinelCount_forTest());
    ASSERT_EQUALS(oplog2["ts"].timestamp(), oplogBuffer.getLastPushedTimestamp_forTest());
    ASSERT_EQUALS(oplog["ts"].timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {oplog, sentinel, oplog2});

    oplogBuffer.clear(_txn.get());
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), 0UL);
    ASSERT_EQUALS(0U, oplogBuffer.getSentinelCount_forTest());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPushedTimestamp_forTest());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_txn.get(), nss, {});

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_FALSE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
}

TEST_F(OplogBufferCollectionTest, WaitForDataBlocksAndFindsDocument) {
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
        success = oplogBuffer.waitForData(Seconds(30));
        count = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.push(_txn.get(), oplog);
    peekingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success);
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog);
    ASSERT_EQUALS(count, 1UL);
}

TEST_F(OplogBufferCollectionTest, TwoWaitForDataInvocationsBlockAndFindSameDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(3U);
    BSONObj oplog = makeOplogEntry(1);
    bool success1 = false;
    std::size_t count1 = 0;

    bool success2 = false;
    std::size_t count2 = 0;

    stdx::thread peekingThread1([&]() {
        Client::initThread("peekingThread1");
        barrier.countDownAndWait();
        success1 = oplogBuffer.waitForData(Seconds(30));
        count1 = oplogBuffer.getCount();
    });

    stdx::thread peekingThread2([&]() {
        Client::initThread("peekingThread2");
        barrier.countDownAndWait();
        success2 = oplogBuffer.waitForData(Seconds(30));
        count2 = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.push(_txn.get(), oplog);
    peekingThread1.join();
    peekingThread2.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success1);
    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog);
    ASSERT_EQUALS(count1, 1UL);
    ASSERT_TRUE(success2);
    ASSERT_EQUALS(count2, 1UL);
}

TEST_F(OplogBufferCollectionTest, WaitForDataBlocksAndTimesOutWhenItDoesNotFindDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    BSONObj doc;
    bool success = false;
    std::size_t count = 0;

    stdx::thread peekingThread([&]() {
        Client::initThread("peekingThread");
        success = oplogBuffer.waitForData(Seconds(1));
        count = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    peekingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_FALSE(success);
    ASSERT_FALSE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(count, 0UL);
}

void _testPushSentinelsProperly(
    OperationContext* txn,
    const NamespaceString& nss,
    StorageInterface* storageInterface,
    stdx::function<void(OperationContext* txn,
                        OplogBufferCollection* oplogBuffer,
                        const std::vector<BSONObj>& oplog)> pushDocsFn) {
    OplogBufferCollection oplogBuffer(storageInterface, nss);
    oplogBuffer.startup(txn);
    const std::vector<BSONObj> oplog = {
        BSONObj(), makeOplogEntry(1), BSONObj(), BSONObj(), makeOplogEntry(2), BSONObj(),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    pushDocsFn(txn, &oplogBuffer, oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 6UL);
    _assertDocumentsInCollectionEquals(txn, nss, oplog);
}

TEST_F(OplogBufferCollectionTest, PushPushesOnSentinelsProperly) {
    auto nss = makeNamespace(_agent);
    _testPushSentinelsProperly(_txn.get(),
                               nss,
                               _storageInterface,
                               [](OperationContext* txn,
                                  OplogBufferCollection* oplogBuffer,
                                  const std::vector<BSONObj>& oplog) {
                                   oplogBuffer->push(txn, oplog[0]);
                                   ASSERT_EQUALS(1U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->push(txn, oplog[1]);
                                   ASSERT_EQUALS(0U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->push(txn, oplog[2]);
                                   ASSERT_EQUALS(1U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->push(txn, oplog[3]);
                                   ASSERT_EQUALS(2U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->push(txn, oplog[4]);
                                   ASSERT_EQUALS(0U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->push(txn, oplog[5]);
                                   ASSERT_EQUALS(1U, oplogBuffer->getSentinelCount_forTest());
                               });
}

TEST_F(OplogBufferCollectionTest, PushEvenIfFullPushesOnSentinelsProperly) {
    auto nss = makeNamespace(_agent);
    _testPushSentinelsProperly(_txn.get(),
                               nss,
                               _storageInterface,
                               [](OperationContext* txn,
                                  OplogBufferCollection* oplogBuffer,
                                  const std::vector<BSONObj>& oplog) {
                                   oplogBuffer->pushEvenIfFull(txn, oplog[0]);
                                   ASSERT_EQUALS(1U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->pushEvenIfFull(txn, oplog[1]);
                                   ASSERT_EQUALS(0U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->pushEvenIfFull(txn, oplog[2]);
                                   ASSERT_EQUALS(1U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->pushEvenIfFull(txn, oplog[3]);
                                   ASSERT_EQUALS(2U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->pushEvenIfFull(txn, oplog[4]);
                                   ASSERT_EQUALS(0U, oplogBuffer->getSentinelCount_forTest());

                                   oplogBuffer->pushEvenIfFull(txn, oplog[5]);
                                   ASSERT_EQUALS(1U, oplogBuffer->getSentinelCount_forTest());
                               });
}

TEST_F(OplogBufferCollectionTest, PushAllNonBlockingPushesOnSentinelsProperly) {
    auto nss = makeNamespace(_agent);
    _testPushSentinelsProperly(_txn.get(),
                               nss,
                               _storageInterface,
                               [](OperationContext* txn,
                                  OplogBufferCollection* oplogBuffer,
                                  const std::vector<BSONObj>& oplog) {
                                   oplogBuffer->pushAllNonBlocking(
                                       txn, oplog.cbegin(), oplog.cend());
                                   ASSERT_EQUALS(1U, oplogBuffer->getSentinelCount_forTest());
                               });
}

DEATH_TEST_F(
    OplogBufferCollectionTest,
    PushAllNonBlockingWithOutOfOrderDocumentsTriggersInvariantFailure,
    "Invariant failure value.isEmpty() ? ts == previousTimestamp : ts > previousTimestamp") {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(2), makeOplogEntry(1),
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

    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 4UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[3]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[3]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    // tryPop does not remove documents from collection.
    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);
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

    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    // tryPop does not remove documents from collection.
    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);
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

    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    // tryPop does not remove documents from collection.
    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);
}

TEST_F(OplogBufferCollectionTest, MultipleSentinelsAreReturnedInOrder) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        BSONObj(), makeOplogEntry(1), BSONObj(), BSONObj(), makeOplogEntry(2), BSONObj(),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.cbegin(), oplog.cend());
    ASSERT_EQUALS(oplogBuffer.getCount(), 6UL);

    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 6UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 5UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 5UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
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
    ASSERT_BSONOBJ_EQ(doc, oplog[4]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[4]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    // tryPop does not remove documents from collection.
    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);
}

TEST_F(OplogBufferCollectionTest, WaitForDataBlocksAndFindsSentinel) {
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
        success = oplogBuffer.waitForData(Seconds(30));
        count = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    peekingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success);
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(count, 1UL);
}

TEST_F(OplogBufferCollectionTest, TwoWaitForDataInvocationsBlockAndFindSameSentinel) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_txn.get());

    unittest::Barrier barrier(3U);
    BSONObj oplog;
    bool success1 = false;
    std::size_t count1 = 0;

    bool success2 = false;
    std::size_t count2 = 0;

    stdx::thread peekingThread1([&]() {
        Client::initThread("peekingThread1");
        barrier.countDownAndWait();
        success1 = oplogBuffer.waitForData(Seconds(30));
        count1 = oplogBuffer.getCount();
    });

    stdx::thread peekingThread2([&]() {
        Client::initThread("peekingThread2");
        barrier.countDownAndWait();
        success2 = oplogBuffer.waitForData(Seconds(30));
        count2 = oplogBuffer.getCount();
    });

    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    barrier.countDownAndWait();
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    peekingThread1.join();
    peekingThread2.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success1);
    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(count1, 1UL);
    ASSERT_TRUE(success2);
    ASSERT_EQUALS(count2, 1UL);
}

OplogBufferCollection::Options _makeOptions(std::size_t peekCacheSize) {
    OplogBufferCollection::Options options;
    options.peekCacheSize = peekCacheSize;
    return options;
}

/**
 * Converts expectedDocs to collection format (with _id field) and compare with peek cache contents.
 */
void _assertDocumentsEqualCache(const std::vector<BSONObj>& expectedDocs,
                                std::queue<BSONObj> actualDocsInCache) {
    for (const auto& doc : expectedDocs) {
        ASSERT_FALSE(actualDocsInCache.empty());
        ASSERT_BSONOBJ_EQ(
            doc, OplogBufferCollection::extractEmbeddedOplogDocument(actualDocsInCache.front()));
        actualDocsInCache.pop();
    }
    ASSERT_TRUE(actualDocsInCache.empty());
}

TEST_F(OplogBufferCollectionTest, PeekFillsCacheWithDocumentsFromCollection) {
    auto nss = makeNamespace(_agent);
    std::size_t peekCacheSize = 3U;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, _makeOptions(3));
    ASSERT_EQUALS(peekCacheSize, oplogBuffer.getOptions().peekCacheSize);
    oplogBuffer.startup(_txn.get());

    std::vector<BSONObj> oplog;
    for (int i = 0; i < 5; ++i) {
        oplog.push_back(makeOplogEntry(i + 1));
    };
    oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.cbegin(), oplog.cend());
    _assertDocumentsInCollectionEquals(_txn.get(), nss, oplog);

    // Before any peek operations, peek cache should be empty.
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());

    // First peek operation should trigger a read of 'peekCacheSize' documents from the collection.
    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
    _assertDocumentsEqualCache({oplog[0], oplog[1], oplog[2]}, oplogBuffer.getPeekCache_forTest());

    // Repeated peek operation should not modify the cache.
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
    _assertDocumentsEqualCache({oplog[0], oplog[1], oplog[2]}, oplogBuffer.getPeekCache_forTest());

    // Pop operation should remove the first element in the cache
    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
    _assertDocumentsEqualCache({oplog[1], oplog[2]}, oplogBuffer.getPeekCache_forTest());

    // Next peek operation should not modify the cache.
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[1], doc);
    _assertDocumentsEqualCache({oplog[1], oplog[2]}, oplogBuffer.getPeekCache_forTest());

    // Pop the rest of the items in the cache.
    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[1], doc);
    _assertDocumentsEqualCache({oplog[2]}, oplogBuffer.getPeekCache_forTest());

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[2], doc);
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());

    // Next peek operation should replenish the cache.
    // Cache size will be less than the configured 'peekCacheSize' because
    // there will not be enough documents left unread in the collection.
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[3], doc);
    _assertDocumentsEqualCache({oplog[3], oplog[4]}, oplogBuffer.getPeekCache_forTest());

    // Pop the remaining documents from the buffer.
    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[3], doc);
    _assertDocumentsEqualCache({oplog[4]}, oplogBuffer.getPeekCache_forTest());

    // Verify state of cache between pops using peek.
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[4], doc);
    _assertDocumentsEqualCache({oplog[4]}, oplogBuffer.getPeekCache_forTest());

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[4], doc);
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());

    // Nothing left in the collection.
    ASSERT_FALSE(oplogBuffer.peek(_txn.get(), &doc));
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());

    ASSERT_FALSE(oplogBuffer.tryPop(_txn.get(), &doc));
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());
}

}  // namespace
