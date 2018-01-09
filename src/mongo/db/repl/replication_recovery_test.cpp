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

#include "mongo/db/repl/replication_recovery.h"

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
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
    using OnSetInitialDataTimestampFn = stdx::function<void()>;

    void setInitialDataTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _initialDataTimestamp = snapshotName;
        _onSetInitialDataTimestampFn();
    }

    Timestamp getInitialDataTimestamp() const {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _initialDataTimestamp;
    }

    void setOnSetInitialDataTimestampFn(OnSetInitialDataTimestampFn onSetInitialDataTimestampFn) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _onSetInitialDataTimestampFn = onSetInitialDataTimestampFn;
    }

private:
    mutable stdx::mutex _mutex;
    Timestamp _initialDataTimestamp = Timestamp::min();
    OnSetInitialDataTimestampFn _onSetInitialDataTimestampFn = []() {};
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

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _createOpCtx();
        _storageInterface = stdx::make_unique<StorageInterfaceRecovery>();
        _consistencyMarkers = stdx::make_unique<ReplicationConsistencyMarkersMock>();

        auto service = getServiceContext();
        ReplicationCoordinator::set(service,
                                    stdx::make_unique<ReplicationCoordinatorMock>(service));

        ASSERT_OK(_storageInterface->createCollection(
            getOperationContext(), testNs, CollectionOptions()));
    }

    void tearDown() override {
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
 * Generates oplog entries with the given number used for the timestamp.
 */
TimestampedBSONObj _makeOplogEntry(int t) {
    return {BSON("ts" << Timestamp(t, t) << "h" << t << "ns" << testNs.ns() << "v" << 2 << "op"
                      << "i"
                      << "o"
                      << _makeInsertDocument(t)),
            Timestamp(t)};
}

/**
 * Creates collection options suitable for oplog.
 */
CollectionOptions _createOplogCollectionOptions() {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 64 * 1024 * 1024LL;
    options.autoIndexId = CollectionOptions::NO;
    return options;
}

/**
 * Creates an oplog with insert entries at the given timestamps.
 */
void _setUpOplog(OperationContext* opCtx, StorageInterface* storage, std::vector<int> timestamps) {
    ASSERT_OK(storage->createCollection(opCtx, oplogNs, _createOplogCollectionOptions()));

    for (int ts : timestamps) {
        ASSERT_OK(storage->insertDocument(
            opCtx, oplogNs, _makeOplogEntry(ts), OpTime::kUninitializedTerm));
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
        return _makeOplogEntry(ts).obj;
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
        opCtx, NamespaceString("local.other"), CollectionOptions()));

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp::min());
}

TEST_F(ReplicationRecoveryTest, RecoveryWithEmptyOplogSucceeds) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp::min());
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
        opCtx, NamespaceString("local.other"), CollectionOptions()));

    recovery.recoverFromOplog(opCtx);
}

DEATH_TEST_F(ReplicationRecoveryTest, TruncateEntireOplogFasserts, "Fatal Assertion 40296") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {7, 8, 9});

    recovery.recoverFromOplog(opCtx);
}

TEST_F(ReplicationRecoveryTest, RecoveryTruncatesOplogAtOplogTruncateAfterPoint) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 3), 1));
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp(3, 3));
}

TEST_F(ReplicationRecoveryTest, RecoverySkipsEverythingIfInitialSyncFlagIsSet) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setInitialSyncFlag(opCtx);
    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp(4, 4));
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(1, 1), 1));
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp::min());
}

TEST_F(ReplicationRecoveryTest, RecoveryAppliesDocumentsWhenAppliedThroughIsBehind) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    _assertDocsInTestCollection(opCtx, {4, 5});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp(5, 5));
}

TEST_F(ReplicationRecoveryTest, RecoveryAppliesDocumentsWhenAppliedThroughIsBehindAfterTruncation) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {2, 3});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 3), 1));
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp(3, 3));
}

TEST_F(ReplicationRecoveryTest, RecoveryAppliesDocumentsWhenCheckpointTimestampIsBehind) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->writeCheckpointTimestamp(opCtx, Timestamp(3, 3));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    _assertDocsInTestCollection(opCtx, {4, 5});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(5, 5), 1));
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp(3, 3));
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWhenCheckpointTimestampIsBehindAfterTruncation) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getConsistencyMarkers()->writeCheckpointTimestamp(opCtx, Timestamp(1, 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {2, 3});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(3, 3), 1));
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp(1, 1));
}

DEATH_TEST_F(ReplicationRecoveryTest, AppliedThroughBehindOplogFasserts, "Fatal Assertion 40292") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {3, 4, 5});

    recovery.recoverFromOplog(opCtx);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             AppliedThroughAheadOfTopOfOplogCausesFassert,
             "Fatal Assertion 40313") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(9, 9), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx);
}

DEATH_TEST_F(ReplicationRecoveryTest,
             AppliedThroughNotInOplogCausesFassert,
             "Fatal Assertion 40292") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 4, 5});

    recovery.recoverFromOplog(opCtx);
}

TEST_F(ReplicationRecoveryTest, RecoverySetsInitialDataTimestampToCheckpointTimestampIfItExists) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    // Assert that we set the initial data timestamp before we apply operations.
    getStorageInterfaceRecovery()->setOnSetInitialDataTimestampFn(
        [&]() { ASSERT(getConsistencyMarkers()->getAppliedThrough(opCtx).isNull()); });

    getConsistencyMarkers()->writeCheckpointTimestamp(opCtx, Timestamp(4, 4));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5, 6});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5, 6});
    _assertDocsInTestCollection(opCtx, {5, 6});
    ASSERT(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(6, 6), 6));
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp(4, 4));
}

TEST_F(ReplicationRecoveryTest,
       RecoverySetsInitialDataTimestampToTopOfOplogIfNoCheckpointTimestampAndSingleOp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {5});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull());
    ASSERT(getConsistencyMarkers()->getAppliedThrough(opCtx).isNull());
    ASSERT(getConsistencyMarkers()->getCheckpointTimestamp(opCtx).isNull());
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp(5, 5));
}

TEST_F(ReplicationRecoveryTest,
       RecoverySetsInitialDataTimestampToTopOfOplogIfNoCheckpointTimestampAndMultipleOps) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    // Assert that we set the initial data timestamp after we apply operations.
    getStorageInterfaceRecovery()->setOnSetInitialDataTimestampFn([&]() {
        ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(6, 6), 6));
    });
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(5, 5), 5));

    _setUpOplog(opCtx, getStorageInterface(), {5, 6});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {5, 6});
    _assertDocsInTestCollection(opCtx, {6});
    ASSERT(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull());
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime(Timestamp(6, 6), 6));
    ASSERT(getConsistencyMarkers()->getCheckpointTimestamp(opCtx).isNull());
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp(6, 6));
}

TEST_F(ReplicationRecoveryTest,
       RecoveryDoesNotSetInitialDataTimestampIfNoCheckpointTimestampOrOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {});

    recovery.recoverFromOplog(opCtx);

    _assertDocsInOplog(opCtx, {});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx).isNull());
    ASSERT(getConsistencyMarkers()->getAppliedThrough(opCtx).isNull());
    ASSERT(getConsistencyMarkers()->getCheckpointTimestamp(opCtx).isNull());
    ASSERT_EQ(getStorageInterfaceRecovery()->getInitialDataTimestamp(), Timestamp::min());
}

DEATH_TEST_F(ReplicationRecoveryTest,
             RecoveryFassertsWithNonNullCheckpointTimestampAndAppliedThrough,
             "Fatal Assertion 40603") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    getConsistencyMarkers()->writeCheckpointTimestamp(opCtx, Timestamp(4, 4));

    recovery.recoverFromOplog(opCtx);
}


}  // namespace
