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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
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
    ServiceContext::UniqueOperationContext _opCtx;

private:
    void setUp() override;
    void tearDown() override;
};

void OplogBufferCollectionTest::setUp() {
    ServiceContextMongoDTest::setUp();
    auto service = getServiceContext();

    // AutoGetCollectionForReadCommand requires a valid replication coordinator in order to check
    // the shard version.
    ReplicationCoordinator::set(service, std::make_unique<ReplicationCoordinatorMock>(service));

    auto storageInterface = std::make_unique<StorageInterfaceImpl>();
    _storageInterface = storageInterface.get();
    StorageInterface::set(service, std::move(storageInterface));

    _opCtx = makeOperationContext();
}

void OplogBufferCollectionTest::tearDown() {
    _opCtx.reset();
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
    return BSON("ts" << Timestamp(t, t) << "ns"
                     << "a.a"
                     << "v" << 2 << "op"
                     << "i"
                     << "o" << BSON("_id" << t << "a" << t));
}

TEST_F(OplogBufferCollectionTest, DefaultNamespace) {
    ASSERT_EQUALS(OplogBufferCollection::getDefaultNamespace(),
                  OplogBufferCollection(_storageInterface).getNamespace());
}

TEST_F(OplogBufferCollectionTest, GetNamespace) {
    auto nss = makeNamespace(_agent);
    ASSERT_EQUALS(nss, OplogBufferCollection(_storageInterface, nss).getNamespace());
}

void testStartupCreatesCollection(OperationContext* opCtx,
                                  StorageInterface* storageInterface,
                                  const NamespaceString& nss) {
    OplogBufferCollection oplogBuffer(storageInterface, nss);

    // Collection should not exist until startup() is called.
    ASSERT_FALSE(AutoGetCollectionForReadCommand(opCtx, nss).getCollection());

    oplogBuffer.startup(opCtx);
    ASSERT_TRUE(AutoGetCollectionForReadCommand(opCtx, nss).getCollection());
}

TEST_F(OplogBufferCollectionTest, StartupWithDefaultNamespaceCreatesCollection) {
    auto nss = OplogBufferCollection::getDefaultNamespace();
    ASSERT_FALSE(nss.isOplog());
    testStartupCreatesCollection(_opCtx.get(), _storageInterface, nss);
}

TEST_F(OplogBufferCollectionTest, StartupWithUserProvidedNamespaceCreatesCollection) {
    testStartupCreatesCollection(_opCtx.get(), _storageInterface, makeNamespace(_agent));
}

TEST_F(OplogBufferCollectionTest, StartupWithOplogNamespaceTriggersUassert) {
    ASSERT_THROWS_CODE(testStartupCreatesCollection(
                           _opCtx.get(), _storageInterface, NamespaceString("local.oplog.Z")),
                       DBException,
                       28838);
}

TEST_F(OplogBufferCollectionTest, ShutdownDropsCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    ASSERT_TRUE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());
    oplogBuffer.shutdown(_opCtx.get());
    ASSERT_FALSE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());
}

TEST_F(OplogBufferCollectionTest, extractEmbeddedOplogDocumentChangesIdToTimestamp) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    const BSONObj expectedOp = makeOplogEntry(1);
    BSONObj originalOp = BSON("_id" << Timestamp(1, 1) << "entry" << expectedOp);
    ASSERT_BSONOBJ_EQ(expectedOp, OplogBufferCollection::extractEmbeddedOplogDocument(originalOp));
}

void _assertOplogDocAndTimestampEquals(
    const BSONObj& oplog, const std::tuple<BSONObj, Timestamp>& actualDocTimestampTuple) {
    auto expectedTimestamp = oplog["ts"].timestamp();
    auto expectedDoc = BSON("_id" << BSON("ts" << expectedTimestamp) << "entry" << oplog);
    ASSERT_BSONOBJ_EQ(expectedDoc, std::get<0>(actualDocTimestampTuple));
    ASSERT_EQUALS(expectedTimestamp, std::get<1>(actualDocTimestampTuple));
}

void _assertDocAndTimestampEquals(const Timestamp& expectedTimestamp,
                                  const std::tuple<BSONObj, Timestamp>& actualDocTimestampTuple) {
    auto expectedDoc = BSON("_id" << BSON("ts" << expectedTimestamp));
    ASSERT_BSONOBJ_EQ(expectedDoc, std::get<0>(actualDocTimestampTuple));
    ASSERT_EQUALS(expectedTimestamp, std::get<1>(actualDocTimestampTuple));
}

TEST_F(OplogBufferCollectionTest, addIdToDocumentChangesTimestampToId) {
    const BSONObj originalOp = makeOplogEntry(1);
    _assertOplogDocAndTimestampEquals(originalOp,
                                      OplogBufferCollection::addIdToDocument(originalOp));
}

DEATH_TEST_REGEX_F(OplogBufferCollectionTest,
                   addIdToDocumentWithMissingTimestampFieldTriggersInvariantFailure,
                   R"#(Invariant failure.*!ts.isNull\(\))#") {
    OplogBufferCollection::addIdToDocument(BSON("x" << 1));
}

/**
 * Check collection contents.
 */
void _assertDocumentsInCollectionEquals(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const std::vector<BSONObj>& docs) {
    std::vector<BSONObj> transformedDocs;
    Timestamp ts;
    for (const auto& doc : docs) {
        auto previousTimestamp = ts;
        BSONObj newDoc;
        std::tie(newDoc, ts) = OplogBufferCollection::addIdToDocument(doc);
        transformedDocs.push_back(newDoc);
        if (doc.isEmpty()) {
            ASSERT_EQUALS(previousTimestamp, ts);
            continue;
        }
        ASSERT_GT(ts, previousTimestamp);
    }
    CollectionReader reader(opCtx, nss);
    for (const auto& doc : transformedDocs) {
        ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(reader.next()));
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, reader.next().getStatus());
}

TEST_F(OplogBufferCollectionTest, StartupWithExistingCollectionInitializesCorrectly) {
    auto nss = makeNamespace(_agent);
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, CollectionOptions()));
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(),
        nss,
        TimestampedBSONObj{std::get<0>(OplogBufferCollection::addIdToDocument(oplog[0])),
                           Timestamp(0)},
        OpTime::kUninitializedTerm));
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_NOT_EQUALS(oplogBuffer.getSize(), 0UL);
    ASSERT_EQUALS(Timestamp(1, 1), oplogBuffer.getLastPushedTimestamp());
    ASSERT_EQUALS(Timestamp(0, 0), oplogBuffer.getLastPoppedTimestamp_forTest());
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);

    auto lastPushed = oplogBuffer.lastObjectPushed(_opCtx.get());
    ASSERT_BSONOBJ_EQ(*lastPushed, oplog[0]);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);

    ASSERT_FALSE(oplogBuffer.isEmpty());

    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);

    ASSERT_TRUE(oplogBuffer.isEmpty());
}

TEST_F(OplogBufferCollectionTest, StartupWithEmptyExistingCollectionInitializesCorrectly) {
    auto nss = makeNamespace(_agent);
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, CollectionOptions()));
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {});

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), 0UL);
    ASSERT_EQUALS(Timestamp(0, 0), oplogBuffer.getLastPushedTimestamp());
    ASSERT_EQUALS(Timestamp(0, 0), oplogBuffer.getLastPoppedTimestamp_forTest());
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {});

    auto lastPushed = oplogBuffer.lastObjectPushed(_opCtx.get());
    ASSERT_FALSE(lastPushed);

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());

    ASSERT_FALSE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());

    ASSERT_TRUE(oplogBuffer.isEmpty());
}

TEST_F(OplogBufferCollectionTest, ShutdownWithDropCollectionAtShutdownFalseDoesNotDropCollection) {
    auto nss = makeNamespace(_agent);
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    {
        OplogBufferCollection::Options opts;
        opts.dropCollectionAtShutdown = false;
        OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);

        oplogBuffer.startup(_opCtx.get());
        ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
        oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
        ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

        oplogBuffer.shutdown(_opCtx.get());
    }
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());

    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
}

DEATH_TEST_REGEX_F(OplogBufferCollectionTest,
                   StartupWithExistingCollectionFailsWhenEntryHasNoId,
                   "Fatal assertion.*40348.*IndexNotFound: Index not found, "
                   "ns:local.OplogBufferCollectionTest_"
                   "StartupWithExistingCollectionFailsWhenEntryHasNoId, index: _id_") {
    auto nss = makeNamespace(_agent);
    CollectionOptions collOpts;
    collOpts.setNoIdIndex();
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, collOpts));
    ASSERT_OK(_storageInterface->insertDocument(_opCtx.get(),
                                                nss,
                                                TimestampedBSONObj{makeOplogEntry(1), Timestamp(0)},
                                                OpTime::kUninitializedTerm));

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());
}

DEATH_TEST_REGEX_F(OplogBufferCollectionTest,
                   StartupWithExistingCollectionFailsWhenEntryHasNoTimestamp,
                   R"#(Fatal assertion.*40405.*NoSuchKey: Missing expected field \\"ts\\")#") {
    auto nss = makeNamespace(_agent);
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, CollectionOptions()));
    ASSERT_OK(_storageInterface->insertDocument(_opCtx.get(),
                                                nss,
                                                TimestampedBSONObj{BSON("_id" << 1), Timestamp(0)},
                                                OpTime::kUninitializedTerm));

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());
}

TEST_F(OplogBufferCollectionTest, PeekWithExistingCollectionReturnsEmptyObjectWhenEntryHasNoEntry) {
    auto nss = makeNamespace(_agent);
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, CollectionOptions()));
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(),
        nss,
        TimestampedBSONObj{BSON("_id" << BSON("ts" << Timestamp(1, 1) << "s" << 0)), Timestamp(1)},
        OpTime::kUninitializedTerm));

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
}

TEST_F(OplogBufferCollectionTest,
       StartupDefaultDropsExistingCollectionBeforeCreatingNewCollection) {
    auto nss = makeNamespace(_agent);
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, CollectionOptions()));
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(),
        nss,
        TimestampedBSONObj{std::get<0>(OplogBufferCollection::addIdToDocument(oplog[0])),
                           Timestamp(0)},
        OpTime::kUninitializedTerm));

    OplogBufferCollection::Options opts;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), 0UL);
    ASSERT_EQUALS(Timestamp(0, 0), oplogBuffer.getLastPushedTimestamp());
    ASSERT_EQUALS(Timestamp(0, 0), oplogBuffer.getLastPoppedTimestamp_forTest());
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {});
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushAllNonBlockingAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_opCtx.get(), oplog.begin(), oplog.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);
}

TEST_F(OplogBufferCollectionTest,
       PushAllNonBlockingIgnoresOperationContextIfNoDocumentsAreProvided) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> emptyOplogEntries;
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(nullptr, emptyOplogEntries.begin(), emptyOplogEntries.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
}

TEST_F(OplogBufferCollectionTest, PeekDoesNotRemoveDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog1 = {makeOplogEntry(1)};
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_opCtx.get(), oplog1.cbegin(), oplog1.cend());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    // _peekOneSide should provide correct bound inclusion to storage engine when collection has one
    // document.
    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog1[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    const std::vector<BSONObj> oplog2 = {makeOplogEntry(2)};
    oplogBuffer.push(_opCtx.get(), oplog2.cbegin(), oplog2.cend());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    // _peekOneSide should return same result after adding new oplog entry.
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog1[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {oplog1[0], oplog2[0]});
}

TEST_F(OplogBufferCollectionTest, PeekingFromExistingCollectionReturnsDocument) {
    auto nss = makeNamespace(_agent);
    const auto entry1 = makeOplogEntry(1);
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, CollectionOptions()));
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(),
        nss,
        TimestampedBSONObj{std::get<0>(OplogBufferCollection::addIdToDocument(entry1)),
                           Timestamp(0)},
        OpTime::kUninitializedTerm));

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_NOT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_FALSE(oplogBuffer.isEmpty());
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {entry1});

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, entry1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    const std::vector<BSONObj> oplog = {makeOplogEntry(2)};
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, entry1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {makeOplogEntry(1), makeOplogEntry(2)});
}

TEST_F(OplogBufferCollectionTest, PeekWithNoDocumentsReturnsFalse) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {});
}

TEST_F(OplogBufferCollectionTest, PopDoesNotRemoveDocumentFromCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {oplog});
}

TEST_F(OplogBufferCollectionTest, PopWithNoDocumentsReturnsFalse) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {});
}

TEST_F(OplogBufferCollectionTest, PopAndPeekReturnDocumentsInOrder) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1),
        makeOplogEntry(2),
        makeOplogEntry(3),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_opCtx.get(), oplog.begin(), oplog.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    // tryPop does not remove documents from collection.
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);
}

TEST_F(OplogBufferCollectionTest, LastObjectPushedReturnsNewestOplogEntry) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1),
        makeOplogEntry(2),
        makeOplogEntry(3),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_opCtx.get(), oplog.begin(), oplog.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    auto doc = oplogBuffer.lastObjectPushed(_opCtx.get());
    ASSERT_BSONOBJ_EQ(*doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);
}

TEST_F(OplogBufferCollectionTest,
       LastObjectPushedReturnsNewestOplogEntryWhenStartedWithExistingCollection) {
    auto nss = makeNamespace(_agent);
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, CollectionOptions()));
    auto firstDoc = makeOplogEntry(1);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(),
        nss,
        TimestampedBSONObj{std::get<0>(OplogBufferCollection::addIdToDocument(firstDoc)),
                           Timestamp(0)},
        OpTime::kUninitializedTerm));
    auto secondDoc = makeOplogEntry(2);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(),
        nss,
        TimestampedBSONObj{std::get<0>(OplogBufferCollection::addIdToDocument(secondDoc)),
                           Timestamp(0)},
        OpTime::kUninitializedTerm));

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(3),
        makeOplogEntry(4),
        makeOplogEntry(5),
    };
    ASSERT_BSONOBJ_EQ(*oplogBuffer.lastObjectPushed(_opCtx.get()), secondDoc);

    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);
    oplogBuffer.push(_opCtx.get(), oplog.begin(), oplog.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 5UL);

    ASSERT_BSONOBJ_EQ(*oplogBuffer.lastObjectPushed(_opCtx.get()), oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 5UL);
}

TEST_F(OplogBufferCollectionTest, LastObjectPushedReturnsNoneWithNoEntries) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());

    auto doc = oplogBuffer.lastObjectPushed(_opCtx.get());
    ASSERT_FALSE(doc);
}

TEST_F(OplogBufferCollectionTest, IsEmptyReturnsTrueWhenEmptyAndFalseWhenNot) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    ASSERT_TRUE(oplogBuffer.isEmpty());
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    ASSERT_FALSE(oplogBuffer.isEmpty());
}

TEST_F(OplogBufferCollectionTest, ClearClearsCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), 0UL);
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPushedTimestamp());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), std::size_t(oplog[0].objsize()));
    ASSERT_EQUALS(oplog[0]["ts"].timestamp(), oplogBuffer.getLastPushedTimestamp());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {oplog});

    const std::vector<BSONObj> oplog2 = {makeOplogEntry(2)};
    oplogBuffer.push(_opCtx.get(), oplog2.cbegin(), oplog2.cend());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), std::size_t(oplog[0].objsize() + oplog2[0].objsize()));
    ASSERT_EQUALS(oplog2[0]["ts"].timestamp(), oplogBuffer.getLastPushedTimestamp());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {oplog[0], oplog2[0]});

    BSONObj poppedDoc;
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &poppedDoc));
    ASSERT_BSONOBJ_EQ(oplog[0], poppedDoc);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), std::size_t(oplog2[0].objsize()));
    ASSERT_EQUALS(oplog2[0]["ts"].timestamp(), oplogBuffer.getLastPushedTimestamp());
    ASSERT_EQUALS(oplog[0]["ts"].timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {oplog[0], oplog2[0]});

    oplogBuffer.clear(_opCtx.get());
    ASSERT_TRUE(AutoGetCollectionForReadCommand(_opCtx.get(), nss).getCollection());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_EQUALS(oplogBuffer.getSize(), 0UL);
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPushedTimestamp());
    ASSERT_EQUALS(Timestamp(), oplogBuffer.getLastPoppedTimestamp_forTest());

    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, {});

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_FALSE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
}

TEST_F(OplogBufferCollectionTest, WaitForDataReturnsImmediatelyWhenStartedWithExistingCollection) {
    auto nss = makeNamespace(_agent);
    ASSERT_OK(_storageInterface->createCollection(_opCtx.get(), nss, CollectionOptions()));
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(),
        nss,
        TimestampedBSONObj{std::get<0>(OplogBufferCollection::addIdToDocument(makeOplogEntry(1))),
                           Timestamp(0)},
        OpTime::kUninitializedTerm));

    OplogBufferCollection::Options opts;
    opts.dropCollectionAtStartup = false;
    OplogBufferCollection oplogBuffer(_storageInterface, nss, opts);
    oplogBuffer.startup(_opCtx.get());

    ASSERT_TRUE(oplogBuffer.waitForData(Seconds(30)));
}

TEST_F(OplogBufferCollectionTest, WaitForDataBlocksAndFindsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_opCtx.get());

    unittest::Barrier barrier(2U);
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
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
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    peekingThread.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success);
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(count, 1UL);
}

TEST_F(OplogBufferCollectionTest, TwoWaitForDataInvocationsBlockAndFindSameDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_opCtx.get());

    unittest::Barrier barrier(3U);
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
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
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    peekingThread1.join();
    peekingThread2.join();
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);
    ASSERT_TRUE(success1);
    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(doc, oplog[0]);
    ASSERT_EQUALS(count1, 1UL);
    ASSERT_TRUE(success2);
    ASSERT_EQUALS(count2, 1UL);
}

TEST_F(OplogBufferCollectionTest, WaitForDataBlocksAndTimesOutWhenItDoesNotFindDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_opCtx.get());

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
    ASSERT_FALSE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_EQUALS(count, 0UL);
}

DEATH_TEST_REGEX_F(OplogBufferCollectionTest,
                   PushAllNonBlockingWithOutOfOrderDocumentsTriggersInvariantFailure,
                   R"#(Invariant failure.*ts > previousTimestamp)#") {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(2),
        makeOplogEntry(1),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_opCtx.get(), oplog.begin(), oplog.end());
}

TEST_F(OplogBufferCollectionTest, PreloadAllNonBlockingSucceeds) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);

    oplogBuffer.startup(_opCtx.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(2),
        makeOplogEntry(1),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.preload(_opCtx.get(), oplog.begin(), oplog.end());
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);
    ASSERT_NOT_EQUALS(oplogBuffer.getSize(), 0UL);
    ASSERT_EQUALS(Timestamp(2, 2), oplogBuffer.getLastPushedTimestamp());
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
    oplogBuffer.startup(_opCtx.get());

    std::vector<BSONObj> oplog;
    for (int i = 0; i < 5; ++i) {
        oplog.push_back(makeOplogEntry(i + 1));
    };
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);

    // Before any peek operations, peek cache should be empty.
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());

    // First peek operation should trigger a read of 'peekCacheSize' documents from the collection.
    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
    _assertDocumentsEqualCache({oplog[0], oplog[1], oplog[2]}, oplogBuffer.getPeekCache_forTest());

    // Repeated peek operation should not modify the cache.
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
    _assertDocumentsEqualCache({oplog[0], oplog[1], oplog[2]}, oplogBuffer.getPeekCache_forTest());

    // Pop operation should remove the first element in the cache
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
    _assertDocumentsEqualCache({oplog[1], oplog[2]}, oplogBuffer.getPeekCache_forTest());

    // Next peek operation should not modify the cache.
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[1], doc);
    _assertDocumentsEqualCache({oplog[1], oplog[2]}, oplogBuffer.getPeekCache_forTest());

    // Pop the rest of the items in the cache.
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[1], doc);
    _assertDocumentsEqualCache({oplog[2]}, oplogBuffer.getPeekCache_forTest());

    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[2], doc);
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());

    // Next peek operation should replenish the cache.
    // Cache size will be less than the configured 'peekCacheSize' because
    // there will not be enough documents left unread in the collection.
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[3], doc);
    _assertDocumentsEqualCache({oplog[3], oplog[4]}, oplogBuffer.getPeekCache_forTest());

    // Pop the remaining documents from the buffer.
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[3], doc);
    _assertDocumentsEqualCache({oplog[4]}, oplogBuffer.getPeekCache_forTest());

    // Verify state of cache between pops using peek.
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[4], doc);
    _assertDocumentsEqualCache({oplog[4]}, oplogBuffer.getPeekCache_forTest());

    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[4], doc);
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());

    // Nothing left in the collection.
    ASSERT_FALSE(oplogBuffer.peek(_opCtx.get(), &doc));
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());

    ASSERT_FALSE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    _assertDocumentsEqualCache({}, oplogBuffer.getPeekCache_forTest());
}

TEST_F(OplogBufferCollectionTest, FindByTimestampFindsDocuments) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_opCtx.get());

    std::vector<BSONObj> oplog;
    for (int i = 0; i < 5; ++i) {
        oplog.push_back(makeOplogEntry(i + 1));
    };
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);

    // We should find the documents before popping them.
    for (int i = 0; i < 5; ++i) {
        Timestamp ts(i + 1, i + 1);
        auto docWithStatus = oplogBuffer.findByTimestamp(_opCtx.get(), ts);
        ASSERT_OK(docWithStatus.getStatus());
        ASSERT_BSONOBJ_EQ(oplog[i], docWithStatus.getValue());
    }

    // We can still pop the documents
    for (int i = 0; i < 5; ++i) {
        BSONObj doc;
        ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
        ASSERT_BSONOBJ_EQ(oplog[i], doc);
    }

    // We should find the documents after popping them.
    for (int i = 0; i < 5; ++i) {
        Timestamp ts(i + 1, i + 1);
        auto docWithStatus = oplogBuffer.findByTimestamp(_opCtx.get(), ts);
        ASSERT_OK(docWithStatus.getStatus());
        ASSERT_BSONOBJ_EQ(oplog[i], docWithStatus.getValue());
    }
}

TEST_F(OplogBufferCollectionTest, FindByTimestampNotFound) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_opCtx.get());

    std::vector<BSONObj> oplog;
    oplog.push_back(makeOplogEntry(2));
    oplog.push_back(makeOplogEntry(3));
    oplog.push_back(makeOplogEntry(5));
    oplog.push_back(makeOplogEntry(6));
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    _assertDocumentsInCollectionEquals(_opCtx.get(), nss, oplog);

    // Timestamp 1 not found.
    Timestamp ts1(1, 1);
    auto docWithStatus = oplogBuffer.findByTimestamp(_opCtx.get(), ts1);
    ASSERT_EQ(ErrorCodes::NoSuchKey, docWithStatus.getStatus().code());

    // Timestamp 4 not found.
    Timestamp ts4(4, 4);
    docWithStatus = oplogBuffer.findByTimestamp(_opCtx.get(), ts4);
    ASSERT_EQ(ErrorCodes::NoSuchKey, docWithStatus.getStatus().code());

    // Timestamp 7 not found.
    Timestamp ts7(7, 7);
    docWithStatus = oplogBuffer.findByTimestamp(_opCtx.get(), ts7);
    ASSERT_EQ(ErrorCodes::NoSuchKey, docWithStatus.getStatus().code());
}

TEST_F(OplogBufferCollectionTest, SeekToTimestamp) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss, _makeOptions(3));
    oplogBuffer.startup(_opCtx.get());

    std::vector<BSONObj> oplog;
    oplog.push_back(makeOplogEntry(2));
    oplog.push_back(makeOplogEntry(3));
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    oplog.push_back(makeOplogEntry(5));
    oplog.push_back(makeOplogEntry(6));
    oplogBuffer.push(_opCtx.get(), oplog.cbegin() + 2, oplog.cend());

    BSONObj doc;

    // Seek to last entry..
    ASSERT_OK(oplogBuffer.seekToTimestamp(_opCtx.get(), Timestamp(6, 6)));
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[3], doc);
    ASSERT_FALSE(oplogBuffer.tryPop(_opCtx.get(), &doc));

    // Seek to middle and read entire buffer.
    ASSERT_OK(oplogBuffer.seekToTimestamp(_opCtx.get(), Timestamp(3, 3)));
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[1], doc);
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[2], doc);
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[3], doc);
    ASSERT_FALSE(oplogBuffer.tryPop(_opCtx.get(), &doc));

    // Seek to beginning.
    ASSERT_OK(oplogBuffer.seekToTimestamp(_opCtx.get(), Timestamp(2, 2)));
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[1], doc);

    // With the readahead cache containing documents, seek forward.
    _assertDocumentsEqualCache({oplog[2], oplog[3]}, oplogBuffer.getPeekCache_forTest());
    ASSERT_OK(oplogBuffer.seekToTimestamp(_opCtx.get(), Timestamp(5, 5)));
    ASSERT_TRUE(oplogBuffer.tryPop(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[2], doc);
}

TEST_F(OplogBufferCollectionTest, SeekToTimestampFails) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss, _makeOptions(3));
    oplogBuffer.startup(_opCtx.get());

    std::vector<BSONObj> oplog;
    oplog.push_back(makeOplogEntry(2));
    oplog.push_back(makeOplogEntry(3));
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    oplog.push_back(makeOplogEntry(5));
    oplog.push_back(makeOplogEntry(6));
    oplogBuffer.push(_opCtx.get(), oplog.cbegin() + 2, oplog.cend());

    BSONObj doc;

    // Seek past end.
    auto status = oplogBuffer.seekToTimestamp(_opCtx.get(), Timestamp(7, 7));
    ASSERT_EQ(ErrorCodes::NoSuchKey, status.code());
    // Failed seeks do not affect the oplog buffer.
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);

    // Seek to non-existent timestamp in middle.
    status = oplogBuffer.seekToTimestamp(_opCtx.get(), Timestamp(4, 4));
    ASSERT_EQ(ErrorCodes::NoSuchKey, status.code());
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);

    // Seek before beginning
    status = oplogBuffer.seekToTimestamp(_opCtx.get(), Timestamp(1, 1));
    ASSERT_EQ(ErrorCodes::NoSuchKey, status.code());
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
}

TEST_F(OplogBufferCollectionTest, SeekToTimestampInexact) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss, _makeOptions(3));
    oplogBuffer.startup(_opCtx.get());

    std::vector<BSONObj> oplog;
    oplog.push_back(makeOplogEntry(2));
    oplog.push_back(makeOplogEntry(3));
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());
    oplog.push_back(makeOplogEntry(5));
    oplog.push_back(makeOplogEntry(6));
    oplogBuffer.push(_opCtx.get(), oplog.cbegin() + 2, oplog.cend());

    BSONObj doc;

    // Seek past end.
    ASSERT_OK(oplogBuffer.seekToTimestamp(
        _opCtx.get(), Timestamp(7, 7), RandomAccessOplogBuffer::SeekStrategy::kInexact));
    ASSERT_FALSE(oplogBuffer.peek(_opCtx.get(), &doc));

    // Seek to non-existent timestamp in middle.
    ASSERT_OK(oplogBuffer.seekToTimestamp(
        _opCtx.get(), Timestamp(4, 4), RandomAccessOplogBuffer::SeekStrategy::kInexact));
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[2], doc);

    // Seek before beginning
    ASSERT_OK(oplogBuffer.seekToTimestamp(
        _opCtx.get(), Timestamp(1, 1), RandomAccessOplogBuffer::SeekStrategy::kInexact));
    ASSERT_TRUE(oplogBuffer.peek(_opCtx.get(), &doc));
    ASSERT_BSONOBJ_EQ(oplog[0], doc);
}

TEST_F(OplogBufferCollectionTest, CannotGetSizeAfterSeek) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(_storageInterface, nss);
    oplogBuffer.startup(_opCtx.get());

    std::vector<BSONObj> oplog;
    oplog.push_back(makeOplogEntry(2));
    oplog.push_back(makeOplogEntry(3));
    oplogBuffer.push(_opCtx.get(), oplog.cbegin(), oplog.cend());

    // Seek to last entry..
    ASSERT_OK(oplogBuffer.seekToTimestamp(_opCtx.get(), Timestamp(3, 3)));
    ASSERT_THROWS(oplogBuffer.getSize(), AssertionException);
}

}  // namespace
