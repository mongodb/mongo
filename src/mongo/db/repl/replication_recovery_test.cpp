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

#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/sync_tail_test_fixture.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

const auto& oplogNs = NamespaceString::kRsOplogNamespace;
const NamespaceString testNs("a.a");

class StorageInterfaceRecovery : public StorageInterfaceImpl {
public:
    boost::optional<Timestamp> getRecoveryTimestamp(ServiceContext* serviceCtx) const override {
        stdx::lock_guard<Latch> lock(_mutex);
        return _recoveryTimestamp;
    }

    void setRecoveryTimestamp(Timestamp recoveryTimestamp) {
        stdx::lock_guard<Latch> lock(_mutex);
        _recoveryTimestamp = recoveryTimestamp;
    }

    bool supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const override {
        stdx::lock_guard<Latch> lock(_mutex);
        return _supportsRecoverToStableTimestamp;
    }

    void setSupportsRecoverToStableTimestamp(bool supports) {
        stdx::lock_guard<Latch> lock(_mutex);
        _supportsRecoverToStableTimestamp = supports;
    }

    bool supportsRecoveryTimestamp(ServiceContext* serviceCtx) const override {
        stdx::lock_guard<Latch> lock(_mutex);
        return _supportsRecoveryTimestamp;
    }

    void setSupportsRecoveryTimestamp(bool supports) {
        stdx::lock_guard<Latch> lock(_mutex);
        _supportsRecoveryTimestamp = supports;
    }

    void setPointInTimeReadTimestamp(Timestamp pointInTimeReadTimestamp) {
        stdx::lock_guard<Latch> lock(_mutex);
        _pointInTimeReadTimestamp = pointInTimeReadTimestamp;
    }

    Timestamp getPointInTimeReadTimestamp(OperationContext* opCtx) const override {
        stdx::lock_guard<Latch> lock(_mutex);
        return _pointInTimeReadTimestamp;
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("StorageInterfaceRecovery::_mutex");
    Timestamp _initialDataTimestamp = Timestamp::min();
    boost::optional<Timestamp> _recoveryTimestamp = boost::none;
    Timestamp _pointInTimeReadTimestamp = {};
    bool _supportsRecoverToStableTimestamp = true;
    bool _supportsRecoveryTimestamp = true;
};

class ReplicationRecoveryTestObObserver : public OpObserverNoop {
public:
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid,
                                  std::uint64_t numRecords,
                                  const CollectionDropType dropType) override {
        // If the oplog is not disabled for this namespace, then we need to reserve an op time for
        // the drop.
        if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
            OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
        }
        return {};
    }

    const repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};
};

class ReplicationRecoveryTest : public ServiceContextMongoDTest {
protected:
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    StorageInterface* getStorageInterface() {
        return _storageInterface;
    }

    StorageInterfaceRecovery* getStorageInterfaceRecovery() {
        return _storageInterface;
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

        auto service = getServiceContext();
        StorageInterface::set(service, stdx::make_unique<StorageInterfaceRecovery>());
        _storageInterface = static_cast<StorageInterfaceRecovery*>(StorageInterface::get(service));

        _createOpCtx();
        _consistencyMarkers = stdx::make_unique<ReplicationConsistencyMarkersMock>();


        ReplicationCoordinator::set(
            service, stdx::make_unique<ReplicationCoordinatorMock>(service, getStorageInterface()));

        ASSERT_OK(
            ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));

        ASSERT_OK(_storageInterface->createCollection(
            getOperationContext(), testNs, generateOptionsWithUuid()));

        MongoDSessionCatalog::onStepUp(_opCtx.get());

        auto observerRegistry = checked_cast<OpObserverRegistry*>(service->getOpObserver());
        observerRegistry->addObserver(std::make_unique<ReplicationRecoveryTestObObserver>());

        repl::DropPendingCollectionReaper::set(
            service, stdx::make_unique<repl::DropPendingCollectionReaper>(_storageInterface));

        setOplogCollectionName(service);
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        _consistencyMarkers.reset();

        ServiceContextMongoDTest::tearDown();
        storageGlobalParams.readOnly = false;
        gTakeUnstableCheckpointOnShutdown = false;
    }

    void _createOpCtx() {
        _opCtx = cc().makeOperationContext();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    StorageInterfaceRecovery* _storageInterface;
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
                            boost::none,                      // hash
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
 * Creates a transaction oplog entry.
 */
repl::OplogEntry _makeTransactionOplogEntry(repl::OpTime opTime,
                                            repl::OpTypeEnum opType,
                                            BSONObj object,
                                            repl::OpTime prevOpTime,
                                            StmtId stmtId,
                                            OperationSessionInfo sessionInfo,
                                            Date_t wallTime) {
    BSONObjBuilder builder;
    sessionInfo.serialize(&builder);
    builder.append("ts", opTime.getTimestamp());
    builder.append("t", opTime.getTerm());
    builder.append("v", repl::OplogEntry::kOplogVersion);
    builder.append("op", "c");
    builder.append("ns", testNs.toString());
    builder.append("o", object);
    builder.append("wall", wallTime);
    builder.append("stmtId", stmtId);
    builder.append("prevOpTime", prevOpTime.toBSON());

    return uassertStatusOK(repl::OplogEntry::parse(builder.obj()));
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

    // Initialize the cached pointer to the oplog collection.
    acquireOplogCollectionForLogging(opCtx);
}

/**
 * Check collection contents. OplogInterface returns documents in reverse natural order.
 */
void _assertDocumentsInCollectionEquals(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const std::vector<BSONObj>& docs) {
    CollectionReader reader(opCtx, nss);
    for (const auto& doc : docs) {
        ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(reader.next()));
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, reader.next().getStatus());
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

TEST_F(ReplicationRecoveryTest, PrepareTransactionOplogEntryCorrectlyUpdatesConfigTransactions) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    const auto appliedThrough = OpTime(Timestamp(1, 1), 1);
    getStorageInterfaceRecovery()->setPointInTimeReadTimestamp(Timestamp(1, 0));
    getStorageInterfaceRecovery()->setSupportsRecoverToStableTimestamp(true);
    getStorageInterfaceRecovery()->setRecoveryTimestamp(appliedThrough.getTimestamp());
    getConsistencyMarkers()->setAppliedThrough(opCtx, appliedThrough);
    _setUpOplog(opCtx, getStorageInterface(), {1});

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    const auto lastDate = Date_t::now();
    const auto prepareOp =
        _makeTransactionOplogEntry({Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kCommand,
                                   BSON("applyOps" << BSONArray() << "prepare" << true),
                                   OpTime(Timestamp(0, 0), -1),
                                   0,
                                   sessionInfo,
                                   lastDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {prepareOp.toBSON(), Timestamp(2, 0)}, 1));

    recovery.recoverFromOplog(opCtx, boost::none);

    SessionTxnRecord expectedTxnRecord;
    expectedTxnRecord.setSessionId(*sessionInfo.getSessionId());
    expectedTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
    expectedTxnRecord.setLastWriteOpTime({Timestamp(2, 0), 1});
    expectedTxnRecord.setLastWriteDate(lastDate);
    expectedTxnRecord.setStartOpTime({{Timestamp(2, 0), 1}});
    expectedTxnRecord.setState(DurableTxnStateEnum::kPrepared);

    std::vector<BSONObj> expectedTxnColl{expectedTxnRecord.toBSON()};

    // Make sure that the transaction table shows that the transaction is prepared.
    _assertDocumentsInCollectionEquals(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(2, 0), 1));
}

TEST_F(ReplicationRecoveryTest, AbortTransactionOplogEntryCorrectlyUpdatesConfigTransactions) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    const auto appliedThrough = OpTime(Timestamp(1, 1), 1);
    getStorageInterfaceRecovery()->setSupportsRecoverToStableTimestamp(true);
    getStorageInterfaceRecovery()->setRecoveryTimestamp(appliedThrough.getTimestamp());
    getConsistencyMarkers()->setAppliedThrough(opCtx, appliedThrough);
    _setUpOplog(opCtx, getStorageInterface(), {1});

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    const auto prepareDate = Date_t::now();
    const auto prepareOp =
        _makeTransactionOplogEntry({Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kCommand,
                                   BSON("applyOps" << BSONArray() << "prepare" << true),
                                   OpTime(Timestamp(0, 0), -1),
                                   0,
                                   sessionInfo,
                                   prepareDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {prepareOp.toBSON(), Timestamp(2, 0)}, 1));

    const auto abortDate = Date_t::now();
    const auto abortOp = _makeTransactionOplogEntry({Timestamp(3, 0), 1},
                                                    repl::OpTypeEnum::kCommand,
                                                    BSON("abortTransaction" << 1),
                                                    OpTime(Timestamp(2, 0), 1),
                                                    1,
                                                    sessionInfo,
                                                    abortDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {abortOp.toBSON(), Timestamp(3, 0)}, 1));

    recovery.recoverFromOplog(opCtx, boost::none);

    // Leave startTimestamp unset. The assert below tests there's no startTimestamp in the
    // config.transactions record after the prepared transaction is aborted.
    SessionTxnRecord expectedTxnRecord;
    expectedTxnRecord.setSessionId(*sessionInfo.getSessionId());
    expectedTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
    expectedTxnRecord.setLastWriteOpTime({Timestamp(3, 0), 1});
    expectedTxnRecord.setLastWriteDate(abortDate);
    expectedTxnRecord.setState(DurableTxnStateEnum::kAborted);

    std::vector<BSONObj> expectedTxnColl{expectedTxnRecord.toBSON()};

    // Make sure that the transaction table shows that the transaction is aborted.
    _assertDocumentsInCollectionEquals(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 0), 1));
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoveryFailsWithPrepareAndEnableReadConcernMajorityFalse,
             "Fatal Assertion 51146") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    const auto appliedThrough = OpTime(Timestamp(1, 1), 1);
    getStorageInterfaceRecovery()->setSupportsRecoverToStableTimestamp(true);
    getStorageInterfaceRecovery()->setRecoveryTimestamp(appliedThrough.getTimestamp());
    getConsistencyMarkers()->setAppliedThrough(opCtx, appliedThrough);
    _setUpOplog(opCtx, getStorageInterface(), {1});

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    const auto lastDate = Date_t::now();
    const auto prepareOp =
        _makeTransactionOplogEntry({Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kCommand,
                                   BSON("applyOps" << BSONArray() << "prepare" << true),
                                   OpTime(Timestamp(0, 0), -1),
                                   0,
                                   sessionInfo,
                                   lastDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {prepareOp.toBSON(), Timestamp(2, 0)}, 1));

    serverGlobalParams.enableMajorityReadConcern = false;

    recovery.recoverFromOplog(opCtx, boost::none);
}

TEST_F(ReplicationRecoveryTest, CommitTransactionOplogEntryCorrectlyUpdatesConfigTransactions) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    const auto appliedThrough = OpTime(Timestamp(1, 1), 1);
    getStorageInterfaceRecovery()->setSupportsRecoverToStableTimestamp(true);
    getStorageInterfaceRecovery()->setRecoveryTimestamp(appliedThrough.getTimestamp());
    getConsistencyMarkers()->setAppliedThrough(opCtx, appliedThrough);
    _setUpOplog(opCtx, getStorageInterface(), {1});

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    const auto txnOperations = BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << testNs.toString() << "o"
                                               << BSON("_id" << 1)));
    const auto prepareDate = Date_t::now();
    const auto prepareOp =
        _makeTransactionOplogEntry({Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kCommand,
                                   BSON("applyOps" << txnOperations << "prepare" << true),
                                   OpTime(Timestamp(0, 0), -1),
                                   0,
                                   sessionInfo,
                                   prepareDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {prepareOp.toBSON(), Timestamp(2, 0)}, 1));

    const auto commitDate = Date_t::now();
    const auto commitOp = _makeTransactionOplogEntry(
        {Timestamp(3, 0), 1},
        repl::OpTypeEnum::kCommand,
        BSON("commitTransaction" << 1 << "commitTimestamp" << Timestamp(2, 1)),
        OpTime(Timestamp(2, 0), 1),
        1,
        sessionInfo,
        commitDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {commitOp.toBSON(), Timestamp(3, 0)}, 1));

    recovery.recoverFromOplog(opCtx, boost::none);

    // Leave startTimestamp unset. The assert below tests there's no startTimestamp in the
    // config.transactions record after the prepared transaction is committed.
    SessionTxnRecord expectedTxnRecord;
    expectedTxnRecord.setSessionId(*sessionInfo.getSessionId());
    expectedTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
    expectedTxnRecord.setLastWriteOpTime({Timestamp(3, 0), 1});
    expectedTxnRecord.setLastWriteDate(commitDate);
    expectedTxnRecord.setState(DurableTxnStateEnum::kCommitted);

    std::vector<BSONObj> expectedTxnColl{expectedTxnRecord.toBSON()};

    // Make sure that the transaction table shows that the transaction is commited.
    _assertDocumentsInCollectionEquals(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    // Make sure the data from the transaction is applied.
    std::vector<BSONObj> expectedColl{BSON("_id" << 1)};
    _assertDocumentsInCollectionEquals(opCtx, testNs, expectedColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 0), 1));
}

TEST_F(ReplicationRecoveryTest,
       CommitTransactionBeforeRecoveryTimestampCorrectlyUpdatesConfigTransactions) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    // Make the appliedThrough optime to be after the commit timestamp but before the
    // commitTransaction oplog entry. This way we can check that there are no idempotency concerns
    // when updating the transactions table during startup recovery when the table already reflects
    // the committed transaction.
    const auto appliedThrough = OpTime(Timestamp(2, 2), 1);
    getStorageInterfaceRecovery()->setSupportsRecoverToStableTimestamp(true);
    getStorageInterfaceRecovery()->setRecoveryTimestamp(appliedThrough.getTimestamp());
    getConsistencyMarkers()->setAppliedThrough(opCtx, appliedThrough);
    _setUpOplog(opCtx, getStorageInterface(), {1});

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(3);

    const auto txnOperations = BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << testNs.toString() << "o"
                                               << BSON("_id" << 1)));
    const auto prepareDate = Date_t::now();
    const auto prepareOp =
        _makeTransactionOplogEntry({Timestamp(2, 0), 1},
                                   repl::OpTypeEnum::kCommand,
                                   BSON("applyOps" << txnOperations << "prepare" << true),
                                   OpTime(Timestamp(0, 0), -1),
                                   0,
                                   sessionInfo,
                                   prepareDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {prepareOp.toBSON(), Timestamp(2, 0)}, 1));

    // Add an operation here so that we can have the appliedThrough time be in-between the commit
    // timestamp and the commitTransaction oplog entry.
    const auto insertOp = _makeOplogEntry({Timestamp(2, 2), 1},
                                          repl::OpTypeEnum::kInsert,
                                          BSON("_id" << 2),
                                          boost::none,
                                          sessionInfo,
                                          Date_t::now());

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {insertOp.toBSON(), Timestamp(2, 2)}, 1));

    const auto commitDate = Date_t::now();
    const auto commitOp = _makeTransactionOplogEntry(
        {Timestamp(3, 0), 1},
        repl::OpTypeEnum::kCommand,
        BSON("commitTransaction" << 1 << "commitTimestamp" << Timestamp(2, 1)),
        OpTime(Timestamp(2, 0), 1),
        1,
        sessionInfo,
        commitDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {commitOp.toBSON(), Timestamp(3, 0)}, 1));

    recovery.recoverFromOplog(opCtx, boost::none);

    SessionTxnRecord expectedTxnRecord;
    expectedTxnRecord.setSessionId(*sessionInfo.getSessionId());
    expectedTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
    expectedTxnRecord.setLastWriteOpTime({Timestamp(3, 0), 1});
    expectedTxnRecord.setLastWriteDate(commitDate);
    expectedTxnRecord.setState(DurableTxnStateEnum::kCommitted);

    std::vector<BSONObj> expectedTxnColl{expectedTxnRecord.toBSON()};

    // Make sure that the transaction table shows that the transaction is commited.
    _assertDocumentsInCollectionEquals(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 0), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToBeforeEndOfOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 3, 4, 5, 6, 7, 8, 9, 10});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    // Recovers operations with timestamps: 3, 4, 5.
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 5));
    _assertDocsInTestCollection(opCtx, {3, 4, 5});
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));

    // Recovers operations with timestamps: 6, 7, 8, 9.
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(9, 9));
    _assertDocsInTestCollection(opCtx, {3, 4, 5, 6, 7, 8, 9});
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(9, 9), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToEndOfOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 3, 4, 5, 6, 7, 8, 9, 10});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    // Recovers all operations
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(10, 10));
    _assertDocsInTestCollection(opCtx, {3, 4, 5, 6, 7, 8, 9, 10});
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(10, 10), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToInvalidEndPoint) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 3, 4, 5});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    ASSERT_THROWS_CODE(
        recovery.recoverFromOplogUpTo(opCtx, Timestamp(1, 1)), DBException, ErrorCodes::BadValue);
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(2, 2));

    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(0, 0), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToWithEmptyOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    startCapturingLogMessages();
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 5));
    stopCapturingLogMessages();

    ASSERT_EQUALS(1, countLogLinesContaining("No stored oplog entries to apply for recovery"));
    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(0, 0), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToFailsWithInitialSyncFlag) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setInitialSyncFlag(opCtx);
    _setUpOplog(opCtx, getStorageInterface(), {5});

    ASSERT_THROWS_CODE(recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 5)),
                       DBException,
                       ErrorCodes::InitialSyncActive);
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToDoesNotExceedEndPoint) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 5, 10});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    recovery.recoverFromOplogUpTo(opCtx, Timestamp(9, 9));
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));

    recovery.recoverFromOplogUpTo(opCtx, Timestamp(15, 15));
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(10, 10), 1));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToWithNoOperationsToRecover) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 1580148188, std::numeric_limits<int>::max()});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(1580148188, 1580148188));

    startCapturingLogMessages();
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(1580148193, 1));
    stopCapturingLogMessages();

    ASSERT_EQUALS(1,
                  countLogLinesContaining("No stored oplog entries to apply for recovery between"));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToReconstructsPreparedTransactions) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 2});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(1, 1));

    const auto sessionId = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(sessionId);

    {
        MongoDOperationContextSession ocs(opCtx);

        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(sessionId);
        sessionInfo.setTxnNumber(1);

        const auto lastDate = Date_t::now();
        const auto prepareOp =
            _makeTransactionOplogEntry({Timestamp(3, 3), 1},
                                       repl::OpTypeEnum::kCommand,
                                       BSON("applyOps" << BSONArray() << "prepare" << true),
                                       OpTime(Timestamp(0, 0), -1),
                                       0,
                                       sessionInfo,
                                       lastDate);
        ASSERT_OK(getStorageInterface()->insertDocument(
            opCtx, oplogNs, {prepareOp.toBSON(), Timestamp(3, 3)}, 1));
    }

    recovery.recoverFromOplogUpTo(opCtx, Timestamp(3, 3));

    {
        MongoDOperationContextSession ocs(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_EQ(txnParticipant.getPrepareOpTime().getTimestamp(), Timestamp(3, 3));
        ASSERT_TRUE(txnParticipant.transactionIsPrepared());
    }
}

TEST_F(ReplicationRecoveryTest,
       RecoverFromOplogUpToWithEmptyOplogReconstructsPreparedTransactions) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(1, 1));

    const auto sessionId = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(sessionId);

    {
        MongoDOperationContextSession ocs(opCtx);

        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(sessionId);
        sessionInfo.setTxnNumber(1);

        const auto lastDate = Date_t::now();
        const auto prepareOp =
            _makeTransactionOplogEntry({Timestamp(1, 1), 1},
                                       repl::OpTypeEnum::kCommand,
                                       BSON("applyOps" << BSONArray() << "prepare" << true),
                                       OpTime(Timestamp(0, 0), -1),
                                       0,
                                       sessionInfo,
                                       lastDate);
        ASSERT_OK(getStorageInterface()->insertDocument(
            opCtx, oplogNs, {prepareOp.toBSON(), Timestamp(1, 1)}, 1));

        const BSONObj doc =
            BSON("_id" << sessionId.toBSON() << "txnNum" << static_cast<long long>(1)
                       << "lastWriteOpTime" << OpTime(Timestamp(1, 1), 1) << "lastWriteDate"
                       << lastDate << "state"
                       << "prepared");
        ASSERT_OK(getStorageInterface()->insertDocument(
            opCtx, NamespaceString::kSessionTransactionsTableNamespace, {doc, Timestamp(1, 1)}, 1));
    }

    startCapturingLogMessages();
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 1));
    stopCapturingLogMessages();

    ASSERT_EQUALS(1,
                  countLogLinesContaining("No stored oplog entries to apply for recovery between"));

    {
        MongoDOperationContextSession ocs(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_EQ(txnParticipant.getPrepareOpTime().getTimestamp(), Timestamp(1, 1));
        ASSERT_TRUE(txnParticipant.transactionIsPrepared());
    }
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoverFromOplogUpToWithoutStableCheckpoint,
             "Fatal Assertion 31399") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 5));
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoverFromOplogAsStandaloneFailsWithoutStableCheckpoint,
             "Fatal Assertion 31229") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoverFromOplogAsStandaloneFailsWithNullStableCheckpoint,
             "Fatal Assertion 50806") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(0, 0));
    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoverFromOplogUpToFailsWithNullStableCheckpoint,
             "Fatal Assertion 50806") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(0, 0));
    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogUpTo(opCtx, Timestamp::max());
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogAsStandaloneRecoversOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 5});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(2, 2), 1));

    recovery.recoverFromOplogAsStandalone(opCtx);
    _assertDocsInTestCollection(opCtx, {5});
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));

    // Test the node is readOnly.
    ASSERT_THROWS(getStorageInterface()->insertDocument(opCtx, testNs, {_makeInsertDocument(2)}, 1),
                  AssertionException);
}

TEST_F(ReplicationRecoveryTest,
       RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownRecoversOplog) {
    gTakeUnstableCheckpointOnShutdown = true;
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 5});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(2, 2), 1));

    recovery.recoverFromOplogAsStandalone(opCtx);
    _assertDocsInTestCollection(opCtx, {5});
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));
}

TEST_F(ReplicationRecoveryTest,
       RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownIsIdempotent) {
    gTakeUnstableCheckpointOnShutdown = true;
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2});

    recovery.recoverFromOplogAsStandalone(opCtx);
    _assertDocsInTestCollection(opCtx, {});
}

DEATH_TEST_F(
    ReplicationRecoveryTest,
    RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithInitialSyncFlag,
    "Fatal Assertion 31362") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setInitialSyncFlag(opCtx);
    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_F(
    ReplicationRecoveryTest,
    RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithOplogTruncateAfterPoint,
    "Fatal Assertion 31363") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithEmptyOplog,
             "Fatal Assertion 31364") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_F(
    ReplicationRecoveryTest,
    RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithMismatchedAppliedThrough,
    "Fatal Assertion 31365") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(2, 2), 1));

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithHighMinValid,
             "Fatal Assertion 31366") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});
    getConsistencyMarkers()->setMinValid(opCtx, OpTime(Timestamp(20, 20), 1));

    recovery.recoverFromOplogAsStandalone(opCtx);
}

}  // namespace
