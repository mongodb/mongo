
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

#include "mongo/db/repl/replication_recovery.h"

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

const auto& oplogNs = NamespaceString::kRsOplogNamespace;
const NamespaceString testNs("a.a");

class StorageInterfaceRecovery : public StorageInterfaceImpl {
public:
    boost::optional<Timestamp> getRecoveryTimestamp(ServiceContext* serviceCtx) const override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _recoveryTimestamp;
    }

    void setRecoveryTimestamp(Timestamp recoveryTimestamp) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _recoveryTimestamp = recoveryTimestamp;
    }

    bool supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _supportsRecoverToStableTimestamp;
    }

    void setSupportsRecoverToStableTimestamp(bool supports) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _supportsRecoverToStableTimestamp = supports;
    }

    bool supportsRecoveryTimestamp(ServiceContext* serviceCtx) const override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _supportsRecoveryTimestamp;
    }

    void setSupportsRecoveryTimestamp(bool supports) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _supportsRecoveryTimestamp = supports;
    }

private:
    mutable stdx::mutex _mutex;
    Timestamp _initialDataTimestamp = Timestamp::min();
    boost::optional<Timestamp> _recoveryTimestamp = boost::none;
    bool _supportsRecoverToStableTimestamp = true;
    bool _supportsRecoveryTimestamp = true;
};

class ReplicationRecoveryTest : public ServiceContextMongoDTest {
protected:
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    StorageInterface* getStorageInterface() {
        return _storageInterface.get();
    }

    StorageInterfaceRecovery* getStorageInterfaceRecovery() {
        return _storageInterface.get();
    }

    ReplicationConsistencyMarkers* getConsistencyMarkers() {
        return _consistencyMarkers.get();
    }

    /**
     * Generates a default CollectionOptions object with a UUID. These options should be used
     * when creating a collection in this test because otherwise, collections will not be created
     * with UUIDs. All collections are expected to have UUIDs.
     */
    CollectionOptions generateOptionsWithUuid() {
        CollectionOptions options;
        options.uuid = UUID::gen();
        return options;
    }

    void testRecoveryAppliesDocumentsWhenAppliedThroughIsBehind(bool hasStableTimestamp,
                                                                bool hasStableCheckpoint);
    void testRecoveryToStableAppliesDocumentsWithNoAppliedThrough(bool hasStableTimestamp);
    void testRecoveryAppliesDocumentsWithNoAppliedThroughAfterTruncation(bool hasStableTimestamp);

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _createOpCtx();
        _storageInterface = stdx::make_unique<StorageInterfaceRecovery>();
        _consistencyMarkers = stdx::make_unique<ReplicationConsistencyMarkersMock>();

        auto service = getServiceContext();
        ReplicationCoordinator::set(service,
                                    stdx::make_unique<ReplicationCoordinatorMock>(service));

        ASSERT_OK(
            ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));

        ASSERT_OK(_storageInterface->createCollection(
            getOperationContext(), testNs, generateOptionsWithUuid()));

        SessionCatalog::get(_opCtx->getServiceContext())->onStepUp(_opCtx.get());
    }

    void tearDown() override {
        SessionCatalog::get(_opCtx->getServiceContext())->reset_forTest();

        _opCtx.reset(nullptr);
        _consistencyMarkers.reset();
        _storageInterface.reset();
        ServiceContextMongoDTest::tearDown();
    }

    void _createOpCtx() {
        _opCtx = cc().makeOperationContext();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<StorageInterfaceRecovery> _storageInterface;
    std::unique_ptr<ReplicationConsistencyMarkersMock> _consistencyMarkers;
};

/**
 * Generates a document to be inserted into the test collection.
 */
BSONObj _makeInsertDocument(int t) {
    return BSON("_id" << t << "a" << t);
}

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry _makeOplogEntry(repl::OpTime opTime,
                                 repl::OpTypeEnum opType,
                                 BSONObj object,
                                 boost::optional<BSONObj> object2 = boost::none,
                                 OperationSessionInfo sessionInfo = {},
                                 boost::optional<Date_t> wallTime = boost::none) {
    return repl::OplogEntry(opTime,                           // optime
                            1LL,                              // hash
                            opType,                           // opType
                            testNs,                           // namespace
                            boost::none,                      // uuid
                            boost::none,                      // fromMigrate
                            repl::OplogEntry::kOplogVersion,  // version
                            object,                           // o
                            object2,                          // o2
                            sessionInfo,                      // sessionInfo
                            boost::none,                      // isUpsert
                            wallTime,                         // wall clock time
                            boost::none,                      // statement id
                            boost::none,   // optime of previous write within same transaction
                            boost::none,   // pre-image optime
                            boost::none);  // post-image optime
}

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
TimestampedBSONObj _makeInsertOplogEntry(int t) {
    auto entry = _makeOplogEntry(OpTime(Timestamp(t, t), 1),  // optime
                                 OpTypeEnum::kInsert,         // op type
                                 _makeInsertDocument(t),      // o
                                 boost::none);                // o2
    return {entry.toBSON(), Timestamp(t)};
}

/**
 * Creates a delete oplog entry with the given number used for the timestamp.
 */
OplogEntry _makeDeleteOplogEntry(int t, const BSONObj& documentToDelete) {
    return _makeOplogEntry(OpTime(Timestamp(t, t), 1),  // optime
                           OpTypeEnum::kDelete,         // op type
                           documentToDelete,            // o
                           boost::none);                // o2
}

/**
 * Creates an update oplog entry with the given number used for the timestamp.
 */
OplogEntry _makeUpdateOplogEntry(int t,
                                 const BSONObj& documentToUpdate,
                                 const BSONObj& updatedDocument) {
    return _makeOplogEntry(OpTime(Timestamp(t, t), 1),  // optime
                           OpTypeEnum::kUpdate,         // op type
                           updatedDocument,             // o
                           documentToUpdate);           // o2
}

/**
 * Creates collection options suitable for oplog.
 */
CollectionOptions _createOplogCollectionOptions() {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 64 * 1024 * 1024LL;
    options.autoIndexId = CollectionOptions::NO;
    options.uuid = UUID::gen();
    return options;
}

/**
 * Creates an oplog with insert entries at the given timestamps.
 */
void _setUpOplog(OperationContext* opCtx, StorageInterface* storage, std::vector<int> timestamps) {
    ASSERT_OK(storage->createCollection(opCtx, oplogNs, _createOplogCollectionOptions()));

    for (int ts : timestamps) {
        ASSERT_OK(storage->insertDocument(
            opCtx, oplogNs, _makeInsertOplogEntry(ts), OpTime::kUninitializedTerm));
    }
}

/**
 * Check collection contents. OplogInterface returns documents in reverse natural order.
 */
void _assertDocumentsInCollectionEquals(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const std::vector<BSONObj>& docs) {
    std::vector<BSONObj> reversedDocs(docs);
    std::reverse(reversedDocs.begin(), reversedDocs.end());
    OplogInterfaceLocal oplog(opCtx, nss.ns());
    auto iter = oplog.makeIterator();
    for (const auto& doc : reversedDocs) {
        ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(iter->next()).first);
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

/**
 * Asserts that the documents in the oplog have the given timestamps.
 */
void _assertDocsInOplog(OperationContext* opCtx, std::vector<int> timestamps) {
    std::vector<BSONObj> expectedOplog(timestamps.size());
    std::transform(timestamps.begin(), timestamps.end(), expectedOplog.begin(), [](int ts) {
        return _makeInsertOplogEntry(ts).obj;
    });
    _assertDocumentsInCollectionEquals(opCtx, oplogNs, expectedOplog);
}

/**
 * Asserts that the documents in the test collection have the given ids.
 */
void _assertDocsInTestCollection(OperationContext* opCtx, std::vector<int> ids) {
    std::vector<BSONObj> expectedColl(ids.size());
    std::transform(ids.begin(), ids.end(), expectedColl.begin(), [](int id) {
        return _makeInsertDocument(id);
    });
    _assertDocumentsInCollectionEquals(opCtx, testNs, expectedColl);
}

TEST_F(ReplicationRecoveryTest, RecoveryWithNoOplogSucceeds) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    // Create the database.
    ASSERT_OK(getStorageInterface()->createCollection(
        opCtx, NamespaceString("local.other"), generateOptionsWithUuid()));

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {});
    _assertDocsInTestCollection(opCtx, {});
}

TEST_F(ReplicationRecoveryTest, RecoveryWithNoOplogSucceedsWithStableTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    // Create the database.
    ASSERT_OK(getStorageInterface()->createCollection(
        opCtx, NamespaceString("local.other"), generateOptionsWithUuid()));

    Timestamp stableTimestamp(3, 3);
    recovery.recoverFromOplog(opCtx, stableTimestamp);

    _assertDocsInOplog(opCtx, {});
    _assertDocsInTestCollection(opCtx, {});
}

TEST_F(ReplicationRecoveryTest, RecoveryWithEmptyOplogSucceeds) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {});
    _assertDocsInTestCollection(opCtx, {});
}

TEST_F(ReplicationRecoveryTest, RecoveryWithEmptyOplogSucceedsWithStableTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {});

    Timestamp stableTimestamp(3, 3);
    recovery.recoverFromOplog(opCtx, stableTimestamp);

    _assertDocsInOplog(opCtx, {});
    _assertDocsInTestCollection(opCtx, {});
}

DEATH_TEST_F(ReplicationRecoveryTest,
             TruncateFassertsWithoutOplogCollection,
             "Fatal assertion 34418 NamespaceNotFound: Can't find local.oplog.rs") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));

    // Create the database.
    ASSERT_OK(getStorageInterface()->createCollection(
        opCtx, NamespaceString("local.other"), generateOptionsWithUuid()));

    recovery.recoverFromOplog(opCtx, boost::none);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoveryInvariantsIfStableTimestampAndDoesNotSupportRecoveryTimestamp,
             "Invariant failure") {
    getStorageInterfaceRecovery()->setSupportsRecoveryTimestamp(false);
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1});

    recovery.recoverFromOplog(opCtx, Timestamp(1, 1));
}

DEATH_TEST_F(ReplicationRecoveryTest, TruncateEntireOplogFasserts, "Fatal Assertion 40296") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {7, 8, 9});

    recovery.recoverFromOplog(opCtx, boost::none);
}

TEST_F(ReplicationRecoveryTest, RecoveryTruncatesOplogAtOplogTruncateAfterPoint) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 3), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverySucceedsWithOplogTruncatePointTooHigh) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(6, 6));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    _assertDocsInTestCollection(opCtx, {4, 5});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverySucceedsWithOplogTruncatePointInGap) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(2, 2), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 5, 6});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {3});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 3), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverySkipsEverythingIfInitialSyncFlagIsSet) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setInitialSyncFlag(opCtx);
    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp(4, 4));
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(1, 1), 1));
}

void ReplicationRecoveryTest::testRecoveryAppliesDocumentsWhenAppliedThroughIsBehind(
    bool hasStableTimestamp, bool hasStableCheckpoint) {
    ASSERT(!(hasStableTimestamp && hasStableCheckpoint));

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    auto appliedThroughTS = Timestamp(3, 3);
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(appliedThroughTS, 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    boost::optional<Timestamp> stableTimestamp = boost::none;
    if (hasStableCheckpoint) {
        getStorageInterfaceRecovery()->setRecoveryTimestamp(appliedThroughTS);
    } else if (hasStableTimestamp) {
        stableTimestamp = appliedThroughTS;
    }

    recovery.recoverFromOplog(opCtx, stableTimestamp);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    _assertDocsInTestCollection(opCtx, {4, 5});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());

    auto topTS = Timestamp(5, 5);
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(topTS, 1));
}

TEST_F(ReplicationRecoveryTest, RecoveryAppliesDocumentsWhenAppliedThroughIsBehind) {
    getStorageInterfaceRecovery()->setSupportsRecoverToStableTimestamp(true);
    bool hasStableTimestamp = false;
    bool hasStableCheckpoint = false;
    testRecoveryAppliesDocumentsWhenAppliedThroughIsBehind(hasStableTimestamp, hasStableCheckpoint);
}

TEST_F(ReplicationRecoveryTest, RecoveryAppliesDocumentsWhenAppliedThroughIsBehindNoRTT) {
    getStorageInterfaceRecovery()->setSupportsRecoverToStableTimestamp(false);
    bool hasStableTimestamp = false;
    bool hasStableCheckpoint = false;
    testRecoveryAppliesDocumentsWhenAppliedThroughIsBehind(hasStableTimestamp, hasStableCheckpoint);
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWhenAppliedThroughIsBehindWithStableTimestamp) {
    bool hasStableTimestamp = true;
    bool hasStableCheckpoint = false;
    testRecoveryAppliesDocumentsWhenAppliedThroughIsBehind(hasStableTimestamp, hasStableCheckpoint);
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWhenAppliedThroughIsBehindWithStableCheckpoint) {
    bool hasStableTimestamp = false;
    bool hasStableCheckpoint = true;
    testRecoveryAppliesDocumentsWhenAppliedThroughIsBehind(hasStableTimestamp, hasStableCheckpoint);
}

void ReplicationRecoveryTest::testRecoveryToStableAppliesDocumentsWithNoAppliedThrough(
    bool hasStableTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    auto startingTS = Timestamp(3, 3);
    boost::optional<Timestamp> stableTimestamp = boost::none;
    if (hasStableTimestamp) {
        stableTimestamp = startingTS;
    } else {
        getStorageInterfaceRecovery()->setRecoveryTimestamp(startingTS);
    }
    recovery.recoverFromOplog(opCtx, stableTimestamp);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    _assertDocsInTestCollection(opCtx, {4, 5});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWithNoAppliedThroughAndStableTimestampIsBehind) {
    testRecoveryToStableAppliesDocumentsWithNoAppliedThrough(true);
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWithNoAppliedThroughAndStableCheckpointIsBehind) {
    testRecoveryToStableAppliesDocumentsWithNoAppliedThrough(false);
}

TEST_F(ReplicationRecoveryTest, RecoveryIgnoresDroppedCollections) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    ASSERT_OK(getStorageInterface()->dropCollection(opCtx, testNs));
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, testNs);
        ASSERT_FALSE(autoColl.getCollection());
    }

    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));
    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, testNs);
        ASSERT_FALSE(autoColl.getCollection());
    }
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));
}

TEST_F(ReplicationRecoveryTest, RecoveryAppliesDocumentsWhenAppliedThroughIsBehindAfterTruncation) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {2, 3});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 3), 1));
}

void ReplicationRecoveryTest::testRecoveryAppliesDocumentsWithNoAppliedThroughAfterTruncation(
    bool hasStableTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    auto startingTS = Timestamp(1, 1);
    boost::optional<Timestamp> stableTimestamp = boost::none;
    if (hasStableTimestamp) {
        stableTimestamp = startingTS;
    } else {
        getStorageInterfaceRecovery()->setRecoveryTimestamp(startingTS);
    }
    recovery.recoverFromOplog(opCtx, stableTimestamp);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {2, 3});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 3), 1));
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWhenStableTimestampIsBehindAfterTruncation) {
    testRecoveryAppliesDocumentsWithNoAppliedThroughAfterTruncation(true);
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWhenRecoveryTimestampIsBehindAfterTruncation) {
    testRecoveryAppliesDocumentsWithNoAppliedThroughAfterTruncation(false);
}

DEATH_TEST_F(ReplicationRecoveryTest, AppliedThroughBehindOplogFasserts, "Fatal Assertion 40292") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             AppliedThroughAheadOfTopOfOplogCausesFassert,
             "Fatal Assertion 40313") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(9, 9), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             AppliedThroughNotInOplogCausesFassert,
             "Fatal Assertion 40292") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);
}

TEST_F(ReplicationRecoveryTest, RecoveryDoesNotApplyOperationsIfAppliedThroughIsNull) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});

    const boost::optional<Timestamp> recoverTimestamp = boost::none;
    recovery.recoverFromOplog(opCtx, recoverTimestamp);

    _assertDocsInOplog(opCtx, {5});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull());
    // In 4.0 with RTT, recovering without a `recoverTimestamp` will set `appliedThrough` to the
    // top of oplog.
    ASSERT_EQ(OpTime(Timestamp(5, 5), 1), getConsistencyMarkers()->getAppliedThrough(opCtx));
}

TEST_F(ReplicationRecoveryTest, RecoveryAppliesUpdatesIdempotently) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    ASSERT_OK(getStorageInterface()->insertDocument(opCtx, testNs, {_makeInsertDocument(2)}, 1));
    ASSERT_OK(getStorageInterface()->insertDocument(opCtx, testNs, {_makeInsertDocument(3)}, 1));
    _assertDocsInTestCollection(opCtx, {2, 3});

    auto ts = 1;
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(ts, ts), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1});

    // Test that updates and deletes on a missing document succeed.
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeUpdateOplogEntry(ts, BSON("_id" << 1), BSON("$set" << BSON("a" << 7))).toBSON(),
         Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeDeleteOplogEntry(ts, BSON("_id" << 1)).toBSON(), Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    // Test that updates and deletes on a document succeed.
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeUpdateOplogEntry(ts, BSON("_id" << 2), BSON("$set" << BSON("a" << 7))).toBSON(),
         Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeDeleteOplogEntry(ts, BSON("_id" << 2)).toBSON(), Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    // Test that updates on a document succeed.
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeUpdateOplogEntry(ts, BSON("_id" << 3), BSON("$set" << BSON("a" << 7))).toBSON(),
         Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));

    recovery.recoverFromOplog(opCtx, boost::none);

    std::vector<BSONObj> expectedColl{BSON("_id" << 3 << "a" << 7)};
    _assertDocumentsInCollectionEquals(opCtx, testNs, expectedColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(ts, ts), 1));
}

DEATH_TEST_F(ReplicationRecoveryTest, RecoveryFailsWithBadOp, "terminate() called") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1});

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeUpdateOplogEntry(2, BSON("bad_op" << 1), BSON("$set" << BSON("a" << 7))).toBSON(),
         Timestamp(2, 2)},
        OpTime::kUninitializedTerm));

    recovery.recoverFromOplog(opCtx, boost::none);
}

TEST_F(ReplicationRecoveryTest, CorrectlyUpdatesConfigTransactions) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1});

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    auto insertOp = _makeOplogEntry({Timestamp(2, 0), 1},
                                    repl::OpTypeEnum::kInsert,
                                    BSON("_id" << 1),
                                    boost::none,
                                    sessionInfo,
                                    Date_t::now());

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {insertOp.toBSON(), Timestamp(2, 0)}, 1));

    auto lastDate = Date_t::now();
    auto insertOp2 = _makeOplogEntry({Timestamp(3, 0), 1},
                                     repl::OpTypeEnum::kInsert,
                                     BSON("_id" << 2),
                                     boost::none,
                                     sessionInfo,
                                     lastDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {insertOp2.toBSON(), Timestamp(3, 0)}, 1));

    recovery.recoverFromOplog(opCtx, boost::none);

    std::vector<BSONObj> expectedColl{BSON("_id" << 1), BSON("_id" << 2)};
    _assertDocumentsInCollectionEquals(opCtx, testNs, expectedColl);

    SessionTxnRecord expectedTxnRecord;
    expectedTxnRecord.setSessionId(*sessionInfo.getSessionId());
    expectedTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
    expectedTxnRecord.setLastWriteOpTime({Timestamp(3, 0), 1});
    expectedTxnRecord.setLastWriteDate(lastDate);

    std::vector<BSONObj> expectedTxnColl{expectedTxnRecord.toBSON()};
    _assertDocumentsInCollectionEquals(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 0), 1));
}

}  // namespace
