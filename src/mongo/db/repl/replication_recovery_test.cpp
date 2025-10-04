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

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace repl {
namespace {

const auto& oplogNs = NamespaceString::kRsOplogNamespace;
const NamespaceString testNs = NamespaceString::createNamespaceString_forTest("a.a");

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

    void setPointInTimeReadTimestamp(Timestamp pointInTimeReadTimestamp) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _pointInTimeReadTimestamp = pointInTimeReadTimestamp;
    }

    Timestamp getPointInTimeReadTimestamp(OperationContext* opCtx) const override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _pointInTimeReadTimestamp;
    }

    void setInitialDataTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _initialDataTimestamp = snapshotName;
    }

    Timestamp getInitialDataTimestamp(ServiceContext* serviceCtx) const override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _initialDataTimestamp;
    };


private:
    mutable stdx::mutex _mutex;
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
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isViewlessTimeseries) override {
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
    ReplicationRecoveryTest() : ServiceContextMongoDTest(Options{}.useReplSettings(true)) {}

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
        StorageInterface::set(service, std::make_unique<StorageInterfaceRecovery>());
        _storageInterface = static_cast<StorageInterfaceRecovery*>(StorageInterface::get(service));
        DurableHistoryRegistry::set(service, std::make_unique<DurableHistoryRegistry>());

        _createOpCtx();
        _consistencyMarkers = std::make_unique<ReplicationConsistencyMarkersMock>();


        ReplicationCoordinator::set(
            service, std::make_unique<ReplicationCoordinatorMock>(service, getStorageInterface()));

        ASSERT_OK(
            ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));

        repl::createOplog(_opCtx.get());

        {
            // This fixture sets up some replication, but notably omits installing an
            // OpObserverImpl. This state causes collection creation to timestamp catalog writes,
            // but secondary index creation does not. We use an UnreplicatedWritesBlock to avoid
            // timestamping any of the catalog setup.
            repl::UnreplicatedWritesBlock noRep(_opCtx.get());
            ASSERT_OK(_storageInterface->createCollection(
                getOperationContext(), testNs, generateOptionsWithUuid()));

            MongoDSessionCatalog::set(
                _opCtx->getServiceContext(),
                std::make_unique<MongoDSessionCatalog>(
                    std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

            auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx.get());
            mongoDSessionCatalog->onStepUp(_opCtx.get());
        }

        auto observerRegistry = checked_cast<OpObserverRegistry*>(service->getOpObserver());
        observerRegistry->addObserver(std::make_unique<ReplicationRecoveryTestObObserver>());
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        _consistencyMarkers.reset();

        ServiceContextMongoDTest::tearDown();
        gTakeUnstableCheckpointOnShutdown = false;
    }

    void _createOpCtx() {
        _opCtx = cc().makeOperationContext();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    StorageInterfaceRecovery* _storageInterface = nullptr;
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
                                 Date_t wallTime = Date_t()) {
    return {
        repl::DurableOplogEntry(opTime,                           // optime
                                opType,                           // opType
                                testNs,                           // namespace
                                boost::none,                      // uuid
                                boost::none,                      // fromMigrate
                                boost::none,                      // checkExistenceForDiffInsert
                                boost::none,                      // versionContext
                                repl::OplogEntry::kOplogVersion,  // version
                                object,                           // o
                                object2,                          // o2
                                sessionInfo,                      // sessionInfo
                                boost::none,                      // isUpsert
                                wallTime,                         // wall clock time
                                {},                               // statement ids
                                boost::none,    // optime of previous write within same transaction
                                boost::none,    // pre-image optime
                                boost::none,    // post-image optime
                                boost::none,    // ShardId of resharding recipient
                                boost::none,    // _id
                                boost::none)};  // needsRetryImage
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
    builder.append("ns", testNs.toString_forTest());
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
    return {entry.getEntry().toBSON(), Timestamp(t)};
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
 * Creates an oplog with insert entries at the given timestamps, which must be in increasing order.
 */
void _setUpOplog(OperationContext* opCtx, StorageInterface* storage, std::vector<int> timestamps) {
    for (int ts : timestamps) {
        ASSERT_OK(storage->insertDocument(
            opCtx, oplogNs, _makeInsertOplogEntry(ts), OpTime::kUninitializedTerm));
    }
    if (!timestamps.empty()) {
        // Use the highest inserted timestamp to update oplog visibility so that all of the inserted
        // oplog entries are visible.
        storage->oplogDiskLocRegister(opCtx, Timestamp(timestamps.back(), timestamps.back()), true);
    }
}

/**
 * Check collection contents. OplogInterface returns documents in reverse natural order.
 */
void _assertDocumentsInCollectionEqualsOrdered(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const std::vector<BSONObj>& docs) {
    CollectionReader reader(opCtx, nss);
    for (const auto& doc : docs) {
        ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(reader.next()));
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, reader.next().getStatus());
}

void _assertDocumentsInCollectionEqualsUnordered(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const SimpleBSONObjSet& docs) {
    SimpleBSONObjSet actualDocs;
    CollectionReader reader(opCtx, nss);
    for (std::size_t i = 0; i < docs.size(); ++i) {
        actualDocs.insert(unittest::assertGet(reader.next()).getOwned());
    }
    auto docIt = docs.begin();
    auto docEnd = docs.end();
    auto actualIt = actualDocs.begin();
    for (; docIt != docEnd; ++docIt, ++actualIt) {
        ASSERT_BSONOBJ_EQ(*docIt, *actualIt);
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
    _assertDocumentsInCollectionEqualsOrdered(opCtx, oplogNs, expectedOplog);
}

/**
 * Asserts that the documents in the test collection have the given ids.
 */
void _assertDocsInTestCollection(OperationContext* opCtx, std::vector<int> ids) {
    SimpleBSONObjSet expectedColl;
    std::transform(ids.begin(),
                   ids.end(),
                   std::inserter(expectedColl, expectedColl.begin()),
                   [](int id) { return _makeInsertDocument(id); });
    _assertDocumentsInCollectionEqualsUnordered(opCtx, testNs, expectedColl);
}

TEST_F(ReplicationRecoveryTest, RecoveryWithNoOplogSucceeds) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    // Create the database.
    ASSERT_OK(getStorageInterface()->createCollection(
        opCtx,
        NamespaceString::createNamespaceString_forTest("local.other"),
        generateOptionsWithUuid()));

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {});
    _assertDocsInTestCollection(opCtx, {});
}

TEST_F(ReplicationRecoveryTest, RecoveryWithNoOplogSucceedsWithStableTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    // Create the database.
    ASSERT_OK(getStorageInterface()->createCollection(
        opCtx,
        NamespaceString::createNamespaceString_forTest("local.other"),
        generateOptionsWithUuid()));

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
             RecoveryInvariantsIfStableTimestampAndDoesNotSupportRecoveryTimestamp,
             "Invariant failure") {
    getStorageInterfaceRecovery()->setSupportsRecoveryTimestamp(false);
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1});

    recovery.recoverFromOplog(opCtx, Timestamp(1, 1));
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest, TruncateEntireOplogFasserts, "Fatal assertion.*40296") {
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

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(3, 3));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

TEST_F(ReplicationRecoveryTest, RecoveryDoesNotTruncateOplogAtOrBeforeStableTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(2, 2));
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(4, 4), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5, 6});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4});
    _assertDocsInTestCollection(opCtx, {});

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

TEST_F(ReplicationRecoveryTest, RecoveryTruncatesNothingIfNothingIsAfterStableTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(2, 2));
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(4, 4), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4});
    _assertDocsInTestCollection(opCtx, {});

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

TEST_F(ReplicationRecoveryTest,
       RecoveryTruncatesNothingIfTruncatePointEqualsStableWithNoLaterEntries) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(4, 4), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4});
    _assertDocsInTestCollection(opCtx, {});

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

TEST_F(ReplicationRecoveryTest, RecoveryTruncatesAfterStableIfTruncatePointEqualsStable) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(4, 4), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5, 6});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4});
    _assertDocsInTestCollection(opCtx, {});

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
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
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWithNoAppliedThroughAndStableTimestampIsBehind) {
    testRecoveryToStableAppliesDocumentsWithNoAppliedThrough(true);
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWithNoAppliedThroughAndStableCheckpointIsBehind) {
    testRecoveryToStableAppliesDocumentsWithNoAppliedThrough(false);
}

CollectionAcquisition getCollectionForRead(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     mongo::AcquisitionPrerequisites::kRead),
        MODE_IS);
}

TEST_F(ReplicationRecoveryTest, UnstableRecoveryIgnoresDroppedCollections) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    ASSERT_OK(getStorageInterface()->dropCollection(opCtx, testNs));
    {
        const auto coll = getCollectionForRead(opCtx, testNs);
        ASSERT_FALSE(coll.exists());
    }

    // Not setting a stable timestamp in order to perform unstable recovery,
    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3, 4, 5});
    {
        const auto coll = getCollectionForRead(opCtx, testNs);
        ASSERT_FALSE(coll.exists());
    }
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   StableRecoveryCrashOnDroppedCollectionsInTests,
                   "Fatal assertion.*5415000") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    ASSERT_OK(getStorageInterface()->dropCollection(opCtx, testNs));
    {
        const auto coll = getCollectionForRead(opCtx, testNs);
        ASSERT_FALSE(coll.exists());
    }

    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));
    recovery.recoverFromOplog(opCtx, boost::none);
}

TEST_F(ReplicationRecoveryTest, RecoveryAppliesDocumentsWhenAppliedThroughIsBehindAfterTruncation) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(3, 3));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);

    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {2, 3});
    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

void ReplicationRecoveryTest::testRecoveryAppliesDocumentsWithNoAppliedThroughAfterTruncation(
    bool hasStableTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(3, 3));
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
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWhenStableTimestampIsBehindAfterTruncation) {
    testRecoveryAppliesDocumentsWithNoAppliedThroughAfterTruncation(true);
}

TEST_F(ReplicationRecoveryTest,
       RecoveryAppliesDocumentsWhenRecoveryTimestampIsBehindAfterTruncation) {
    testRecoveryAppliesDocumentsWithNoAppliedThroughAfterTruncation(false);
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   AppliedThroughBehindOplogFasserts,
                   "Fatal assertion.*5466601") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(1, 1));
    _setUpOplog(opCtx, getStorageInterface(), {3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   AppliedThroughAheadOfTopOfOplogCausesFassert,
                   "Fatal assertion.*40313") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(9, 9), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});

    recovery.recoverFromOplog(opCtx, boost::none);
}

TEST_F(ReplicationRecoveryTest, AppliedThroughNotInOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(3, 3));
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
        {_makeUpdateOplogEntry(ts,
                               BSON("_id" << 1),
                               update_oplog_entry::makeDeltaOplogEntry(
                                   BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 7}"))))
             .getEntry()
             .toBSON(),
         Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeDeleteOplogEntry(ts, BSON("_id" << 1)).getEntry().toBSON(), Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    // Test that updates and deletes on a document succeed.
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeUpdateOplogEntry(ts,
                               BSON("_id" << 2),
                               update_oplog_entry::makeDeltaOplogEntry(
                                   BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 7}"))))
             .getEntry()
             .toBSON(),
         Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeDeleteOplogEntry(ts, BSON("_id" << 2)).getEntry().toBSON(), Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    // Test that updates on a document succeed.
    ts++;
    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeUpdateOplogEntry(ts,
                               BSON("_id" << 3),
                               update_oplog_entry::makeDeltaOplogEntry(
                                   BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 7}"))))
             .getEntry()
             .toBSON(),
         Timestamp(ts, ts)},
        OpTime::kUninitializedTerm));
    getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(ts, ts), true);

    recovery.recoverFromOplog(opCtx, boost::none);

    SimpleBSONObjSet expectedColl{BSON("_id" << 3 << "a" << 7)};
    _assertDocumentsInCollectionEqualsUnordered(opCtx, testNs, expectedColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

DEATH_TEST_F(ReplicationRecoveryTest, RecoveryFailsWithBadOp, "terminate() called") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(1, 1), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1});

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx,
        oplogNs,
        {_makeUpdateOplogEntry(2,
                               BSON("bad_op" << 1),
                               update_oplog_entry::makeDeltaOplogEntry(
                                   BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 7}"))))
             .getEntry()
             .toBSON(),
         Timestamp(2, 2)},
        OpTime::kUninitializedTerm));
    getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(2, 2), true);

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
        opCtx, oplogNs, {insertOp.getEntry().toBSON(), Timestamp(2, 0)}, 1));

    auto lastDate = Date_t::now();
    auto insertOp2 = _makeOplogEntry({Timestamp(3, 0), 1},
                                     repl::OpTypeEnum::kInsert,
                                     BSON("_id" << 2),
                                     boost::none,
                                     sessionInfo,
                                     lastDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {insertOp2.getEntry().toBSON(), Timestamp(3, 0)}, 1));
    getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(3, 0), true);

    recovery.recoverFromOplog(opCtx, boost::none);

    SimpleBSONObjSet expectedColl{BSON("_id" << 1), BSON("_id" << 2)};
    _assertDocumentsInCollectionEqualsUnordered(opCtx, testNs, expectedColl);

    SessionTxnRecord expectedTxnRecord;
    expectedTxnRecord.setSessionId(*sessionInfo.getSessionId());
    expectedTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
    expectedTxnRecord.setLastWriteOpTime({Timestamp(3, 0), 1});
    expectedTxnRecord.setLastWriteDate(lastDate);

    std::vector<BSONObj> expectedTxnColl{expectedTxnRecord.toBSON()};
    _assertDocumentsInCollectionEqualsOrdered(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
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
        opCtx, oplogNs, {prepareOp.getEntry().toBSON(), Timestamp(2, 0)}, 1));
    getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(2, 0), true);

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
    _assertDocumentsInCollectionEqualsOrdered(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
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
        opCtx, oplogNs, {prepareOp.getEntry().toBSON(), Timestamp(2, 0)}, 1));

    const auto abortDate = Date_t::now();
    const auto abortOp = _makeTransactionOplogEntry({Timestamp(3, 0), 1},
                                                    repl::OpTypeEnum::kCommand,
                                                    BSON("abortTransaction" << 1),
                                                    OpTime(Timestamp(2, 0), 1),
                                                    1,
                                                    sessionInfo,
                                                    abortDate);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {abortOp.getEntry().toBSON(), Timestamp(3, 0)}, 1));
    getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(3, 0), true);

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
    _assertDocumentsInCollectionEqualsOrdered(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
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

    const auto txnOperations =
        BSON_ARRAY(BSON("op" << "i"
                             << "ns" << testNs.toString_forTest() << "o" << BSON("_id" << 1)));
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
        opCtx, oplogNs, {prepareOp.getEntry().toBSON(), Timestamp(2, 0)}, 1));

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
        opCtx, oplogNs, {commitOp.getEntry().toBSON(), Timestamp(3, 0)}, 1));
    getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(3, 0), true);

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
    _assertDocumentsInCollectionEqualsOrdered(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    // Make sure the data from the transaction is applied.
    std::vector<BSONObj> expectedColl{BSON("_id" << 1)};
    _assertDocumentsInCollectionEqualsOrdered(opCtx, testNs, expectedColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
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

    const auto txnOperations =
        BSON_ARRAY(BSON("op" << "i"
                             << "ns" << testNs.toString_forTest() << "o" << BSON("_id" << 1)));
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
        opCtx, oplogNs, {prepareOp.getEntry().toBSON(), Timestamp(2, 0)}, 1));

    // Add an operation here so that we can have the appliedThrough time be in-between the commit
    // timestamp and the commitTransaction oplog entry.
    const auto insertOp = _makeOplogEntry({Timestamp(2, 2), 1},
                                          repl::OpTypeEnum::kInsert,
                                          BSON("_id" << 2),
                                          boost::none,
                                          sessionInfo,
                                          Date_t::now());

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx, oplogNs, {insertOp.getEntry().toBSON(), Timestamp(2, 2)}, 1));

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
        opCtx, oplogNs, {commitOp.getEntry().toBSON(), Timestamp(3, 0)}, 1));
    getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(3, 0), true);

    recovery.recoverFromOplog(opCtx, boost::none);

    SessionTxnRecord expectedTxnRecord;
    expectedTxnRecord.setSessionId(*sessionInfo.getSessionId());
    expectedTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
    expectedTxnRecord.setLastWriteOpTime({Timestamp(3, 0), 1});
    expectedTxnRecord.setLastWriteDate(commitDate);
    expectedTxnRecord.setState(DurableTxnStateEnum::kCommitted);

    std::vector<BSONObj> expectedTxnColl{expectedTxnRecord.toBSON()};

    // Make sure that the transaction table shows that the transaction is commited.
    _assertDocumentsInCollectionEqualsOrdered(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, expectedTxnColl);

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpTo) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 3, 4, 5, 6, 7, 8, 9, 10});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    // Recovers operations with timestamps: 3, 4, 5.
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 5));
    _assertDocsInTestCollection(opCtx, {3, 4, 5});
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToBeforeEndOfOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 3, 4, 5, 6, 7, 8, 9, 10});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    // Recovers operations with timestamps: 3, 4, 5, 6, 7, 8, 9.
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(9, 9));
    _assertDocsInTestCollection(opCtx, {3, 4, 5, 6, 7, 8, 9});
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToEndOfOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2, 3, 4, 5, 6, 7, 8, 9, 10});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    // Recovers all operations
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(10, 10));
    _assertDocsInTestCollection(opCtx, {3, 4, 5, 6, 7, 8, 9, 10});
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
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToWithEmptyOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {2});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));

    unittest::LogCaptureGuard logs;
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 5));
    logs.stop();

    ASSERT_EQUALS(1, logs.countTextContaining("No stored oplog entries to apply for recovery"));
    _assertDocsInTestCollection(opCtx, {});
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

    recovery.recoverFromOplogUpTo(opCtx, Timestamp(15, 15));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToWithNoOperationsToRecover) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 1580148188, std::numeric_limits<int>::max()});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(1580148188, 1580148188));

    unittest::LogCaptureGuard logs;
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(1580148193, 1));
    logs.stop();

    ASSERT_EQUALS(
        1, logs.countTextContaining("No stored oplog entries to apply for recovery between"));
}

TEST_F(ReplicationRecoveryTest, RecoverFromOplogUpToReconstructsPreparedTransactions) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 2});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(1, 1));

    const auto sessionId = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(sessionId);

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    {
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

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
            opCtx, oplogNs, {prepareOp.getEntry().toBSON(), Timestamp(3, 3)}, 1));
        getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(3, 3), true);
    }

    recovery.recoverFromOplogUpTo(opCtx, Timestamp(3, 3));

    {
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_EQ(txnParticipant.getPrepareOpTime().getTimestamp(), Timestamp(3, 3));
        ASSERT_TRUE(txnParticipant.transactionIsPrepared());
        txnParticipant.abortTransaction(opCtx);
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

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    {
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

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
            opCtx, oplogNs, {prepareOp.getEntry().toBSON(), Timestamp(1, 1)}, 1));
        getStorageInterface()->oplogDiskLocRegister(opCtx, Timestamp(1, 1), true);

        const BSONObj doc =
            BSON("_id" << sessionId.toBSON() << "txnNum" << static_cast<long long>(1)
                       << "lastWriteOpTime" << OpTime(Timestamp(1, 1), 1) << "lastWriteDate"
                       << lastDate << "state"
                       << "prepared");
        ASSERT_OK(getStorageInterface()->insertDocument(
            opCtx, NamespaceString::kSessionTransactionsTableNamespace, {doc, Timestamp(1, 1)}, 1));
    }

    unittest::LogCaptureGuard logs;
    recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 1));
    logs.stop();

    ASSERT_EQUALS(
        1, logs.countTextContaining("No stored oplog entries to apply for recovery between"));

    {
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT_EQ(txnParticipant.getPrepareOpTime().getTimestamp(), Timestamp(1, 1));
        ASSERT_TRUE(txnParticipant.transactionIsPrepared());
        txnParticipant.abortTransaction(opCtx);
    }
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   RecoverFromOplogUpToWithoutStableCheckpoint,
                   "Fatal assertion.*31399") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogUpTo(opCtx, Timestamp(5, 5));
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   RecoverFromOplogAsStandaloneFailsWithoutStableCheckpoint,
                   "Fatal assertion.*31229") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   RecoverFromOplogAsStandaloneFailsWithNullStableCheckpoint,
                   "Fatal assertion.*50806") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(0, 0));
    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   RecoverFromOplogUpToFailsWithNullStableCheckpoint,
                   "Fatal assertion.*50806") {
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

DEATH_TEST_REGEX_F(
    ReplicationRecoveryTest,
    RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithInitialSyncFlag,
    "Fatal assertion.*31362") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setInitialSyncFlag(opCtx);
    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_REGEX_F(
    ReplicationRecoveryTest,
    RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithOplogTruncateAfterPoint,
    "Fatal assertion.*31363") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(4, 4));
    _setUpOplog(opCtx, getStorageInterface(), {5});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_REGEX_F(
    ReplicationRecoveryTest,
    RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithEmptyOplog,
    "Fatal assertion.*31364") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {});

    recovery.recoverFromOplogAsStandalone(opCtx);
}

DEATH_TEST_REGEX_F(
    ReplicationRecoveryTest,
    RecoverFromOplogAsStandaloneWithTakeUnstableCheckpointOnShutdownFailsWithMismatchedAppliedThrough,
    "Fatal assertion.*31365") {
    gTakeUnstableCheckpointOnShutdown = true;

    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {5});
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(2, 2), 1));

    recovery.recoverFromOplogAsStandalone(opCtx);
}

TEST_F(ReplicationRecoveryTest, RecoverStartFromClosestLTEEntryIfRecoveryTsNotInOplog) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    auto recoveryTs = Timestamp(4, 4);
    getStorageInterfaceRecovery()->setRecoveryTimestamp(recoveryTs);
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 5, 6, 7});
    recovery.recoverFromOplog(opCtx, recoveryTs);
}

TEST_F(ReplicationRecoveryTest, RecoverySetsValidateFeaturesAsPrimaryToFalseWhileApplyingOplog) {
    FailPointEnableBlock failpoint(
        "skipResettingValidateFeaturesAsPrimaryAfterRecoveryOplogApplication");

    // The reset will be skipped due to the failpoint so we make the test do it instead.
    auto validateValue = serverGlobalParams.validateFeaturesAsPrimary.load();
    ON_BLOCK_EXIT(
        [validateValue] { serverGlobalParams.validateFeaturesAsPrimary.store(validateValue); });

    serverGlobalParams.validateFeaturesAsPrimary.store(true);
    ASSERT(serverGlobalParams.validateFeaturesAsPrimary.load());

    auto opCtx = getOperationContext();
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(3, 3), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});
    recovery.recoverFromOplog(opCtx, boost::none /* recoveryTs */);

    ASSERT_FALSE(serverGlobalParams.validateFeaturesAsPrimary.load());
}

TEST_F(ReplicationRecoveryTest, StartupRecoveryRunsCompletionHook) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    getConsistencyMarkers()->setOplogTruncateAfterPoint(opCtx, Timestamp(2, 2));
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(4, 4));
    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(4, 4), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4});

    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kSharding,
                                                              logv2::LogSeverity::Debug(2)};
    unittest::LogCaptureGuard logs;
    recovery.recoverFromOplog(opCtx, boost::none);
    logs.stop();

    ASSERT_EQUALS(
        1, logs.countTextContaining("Recovering all user writes recoverable critical sections"));

    _assertDocsInOplog(opCtx, {1, 2, 3, 4});
    _assertDocsInTestCollection(opCtx, {});

    ASSERT_EQ(getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx), Timestamp());
}

TEST_F(ReplicationRecoveryTest, TruncateOplogToTimestamp) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});
    // We should truncate the oplog to Timestamp(3, 3) without ever setting the truncate after
    // point. The truncate after point should remain as the default value.
    ASSERT_EQUALS(Timestamp(), getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx));
    recovery.truncateOplogToTimestamp(opCtx, Timestamp(3, 3));

    ASSERT_EQUALS(Timestamp(), getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx));
    _assertDocsInOplog(opCtx, {1, 2, 3});
    _assertDocsInTestCollection(opCtx, {});
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   TruncateOplogToTimestampOplogDoesntExist,
                   "Fatal assertion.*34418") {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();
    ASSERT_OK(getStorageInterface()->dropCollection(opCtx, NamespaceString::kRsOplogNamespace));

    // The truncate after point should remain as the default value.
    ASSERT_EQUALS(Timestamp(), getConsistencyMarkers()->getOplogTruncateAfterPoint(opCtx));
    // Without an oplog, the 'truncateOplogToTimestamp' function should hit a fatal assertion.
    recovery.truncateOplogToTimestamp(opCtx, Timestamp(3, 3));
}

class TestServiceForRecovery : public ReplicaSetAwareService<TestServiceForRecovery> {
public:
    static TestServiceForRecovery* get(ServiceContext* serviceContext);
    int numCallsOnConsistentDataAvailable{0};
    SimpleBSONObjSet docsOnConsistentDataAvailable;

protected:
    void onStartup(OperationContext* opCtx) final {}
    void onSetCurrentConfig(OperationContext* opCtx) override {}
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) override {
        numCallsOnConsistentDataAvailable++;
        docsOnConsistentDataAvailable.clear();
        if (!StorageInterface::get(opCtx->getServiceContext())
                 ->getCollectionUUID(opCtx, testNs)
                 .isOK()) {
            // No such collection, return early.
            return;
        }
        CollectionReader reader(opCtx, testNs);
        while (true) {
            auto swDoc = reader.next();
            if (!swDoc.isOK()) {
                ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, swDoc.getStatus());
                return;
            }
            docsOnConsistentDataAvailable.insert(unittest::assertGet(swDoc).getOwned());
        }
    }
    void onStepUpBegin(OperationContext* opCtx, long long term) override {}
    void onStepUpComplete(OperationContext* opCtx, long long term) override {}
    void onStepDown() override {}
    void onRollbackBegin() override {}
    void onBecomeArbiter() override {}
    void onShutdown() override {}

private:
    bool shouldRegisterReplicaSetAwareService() const final {
        return true;
    }
    std::string getServiceName() const final {
        return "TestServiceForRecovery";
    }
};

const auto getTestServiceForRecovery = ServiceContext::declareDecoration<TestServiceForRecovery>();
TestServiceForRecovery* TestServiceForRecovery::get(ServiceContext* serviceContext) {
    return &getTestServiceForRecovery(serviceContext);
}
const ReplicaSetAwareServiceRegistry::Registerer<TestServiceForRecovery>
    testServiceForRecoveryRegisterer("TestServiceForRecovery");

void assertDocsOnConsistentDataAvailable(const SimpleBSONObjSet& actualDocs, std::vector<int> ids) {
    SimpleBSONObjSet expectedDocs;
    std::transform(ids.begin(),
                   ids.end(),
                   std::inserter(expectedDocs, expectedDocs.begin()),
                   [](int id) { return _makeInsertDocument(id); });

    ASSERT_EQ(actualDocs.size(), expectedDocs.size());
    auto docIt = expectedDocs.begin();
    auto docEnd = expectedDocs.end();
    auto actualIt = actualDocs.begin();
    for (; docIt != docEnd; ++docIt, ++actualIt) {
        ASSERT_BSONOBJ_EQ(*docIt, *actualIt);
    }
}

TEST_F(ReplicationRecoveryTest, StableRecoveryCallsOnConsistentDataAvailable) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    auto testService = TestServiceForRecovery::get(opCtx->getServiceContext());

    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(2, 2));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4});

    ASSERT_EQ(0, testService->numCallsOnConsistentDataAvailable);
    recovery.recoverFromOplog(opCtx, boost::none);
    // The onConsistentDataAvailable hook should be called before oplog replay for stable recovery,
    // so we should see no documents when the hook is called.
    assertDocsOnConsistentDataAvailable(testService->docsOnConsistentDataAvailable, {});
    ASSERT_EQ(1, testService->numCallsOnConsistentDataAvailable);
}

TEST_F(ReplicationRecoveryTest, UnstableRecoveryCallsOnConsistentDataAvailable) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    auto testService = TestServiceForRecovery::get(opCtx->getServiceContext());

    getConsistencyMarkers()->setAppliedThrough(opCtx, OpTime(Timestamp(2, 2), 1));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4});

    ASSERT_EQ(0, testService->numCallsOnConsistentDataAvailable);
    recovery.recoverFromOplog(opCtx, boost::none);
    // The onConsistentDataAvailable hook should be called after oplog replay for unstable recovery,
    // so we should see two documents inserted by the oplog replay when the hook is called.
    assertDocsOnConsistentDataAvailable(testService->docsOnConsistentDataAvailable, {3, 4});
    ASSERT_EQ(1, testService->numCallsOnConsistentDataAvailable);
}

TEST_F(ReplicationRecoveryTest, InitialSyncRecoveryDoesNotCallOnConsistentDataAvailable) {
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    auto opCtx = getOperationContext();

    auto testService = TestServiceForRecovery::get(opCtx->getServiceContext());

    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(4, 4));
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4});

    ASSERT_EQ(0, testService->numCallsOnConsistentDataAvailable);
    recovery.recoverFromOplogAsStandalone(opCtx, /*duringInitialSync=*/true);
    ASSERT_EQ(0, testService->numCallsOnConsistentDataAvailable);
}

TEST_F(ReplicationRecoveryTest, ApplyOplogEntriesForRestore) {
    storageGlobalParams.magicRestore = true;
    auto opCtx = getOperationContext();
    getStorageInterface()->setInitialDataTimestamp(opCtx->getServiceContext(),
                                                   Timestamp::kAllowUnstableCheckpointsSentinel);
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(1, 1));
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3});

    recovery.applyOplogEntriesForRestore(opCtx, Timestamp(1, 1));
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime());
    ASSERT_EQ(getStorageInterface()->getInitialDataTimestamp(opCtx->getServiceContext()),
              Timestamp::kAllowUnstableCheckpointsSentinel);
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   ApplyOplogEntriesForRestoreStorageMustSupportRts,
                   "Invariant failure") {
    storageGlobalParams.magicRestore = true;
    auto opCtx = getOperationContext();
    getStorageInterface()->setInitialDataTimestamp(opCtx->getServiceContext(),
                                                   Timestamp::kAllowUnstableCheckpointsSentinel);
    getStorageInterfaceRecovery()->setSupportsRecoveryTimestamp(false);
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});
    recovery.applyOplogEntriesForRestore(opCtx, Timestamp(1, 1));
}

DEATH_TEST_REGEX_F(ReplicationRecoveryTest,
                   ApplyOplogEntriesForRestoreNoOplog,
                   "Fatal assertion.*8290703") {
    storageGlobalParams.magicRestore = true;
    auto opCtx = getOperationContext();
    getStorageInterface()->setInitialDataTimestamp(opCtx->getServiceContext(),
                                                   Timestamp::kAllowUnstableCheckpointsSentinel);
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    recovery.applyOplogEntriesForRestore(opCtx, Timestamp(1, 1));
}

TEST_F(ReplicationRecoveryTest, ApplyOplogEntriesForRestoreStartPointIsAfterOplog) {
    storageGlobalParams.magicRestore = true;
    auto opCtx = getOperationContext();
    getStorageInterface()->setInitialDataTimestamp(opCtx->getServiceContext(),
                                                   Timestamp::kAllowUnstableCheckpointsSentinel);
    ReplicationRecoveryImpl recovery(getStorageInterface(), getConsistencyMarkers());
    _setUpOplog(opCtx, getStorageInterface(), {1, 2, 3, 4, 5});
    getStorageInterfaceRecovery()->setRecoveryTimestamp(Timestamp(7, 7));
    // The function will adjust the Timestamp(7, 7) start point to the top of the oplog.
    unittest::LogCaptureGuard logs;
    ASSERT_DOES_NOT_THROW(recovery.applyOplogEntriesForRestore(opCtx, Timestamp(7, 7)));
    logs.stop();
    ASSERT_EQUALS(
        1,
        logs.countTextContaining("Start point for recovery oplog application not found in "
                                 "oplog. Adjusting start point to earlier entry"));
    ASSERT_EQ(getConsistencyMarkers()->getAppliedThrough(opCtx), OpTime());
    ASSERT_EQ(getStorageInterface()->getInitialDataTimestamp(opCtx->getServiceContext()),
              Timestamp::kAllowUnstableCheckpointsSentinel);
}


}  // namespace
}  // namespace repl
}  // namespace mongo
