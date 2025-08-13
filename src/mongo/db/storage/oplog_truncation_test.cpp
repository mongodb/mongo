/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/storage/oplog_truncation.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace repl {
const auto& oplogNs = NamespaceString::kRsOplogNamespace;

class OplogTruncationTest : public ServiceContextMongoDTest {
protected:
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    StorageInterface& getStorage() {
        return _storage;
    }

    ReplicationConsistencyMarkers* getConsistencyMarkers() {
        return _consistencyMarkers.get();
    }

    BSONObj makeBSONObjWithSize(unsigned int seconds, unsigned int t, int size, char fill = 'x') {
        Timestamp opTime{seconds, t};
        Date_t wallTime = Date_t::fromMillisSinceEpoch(t);
        BSONObj objTemplate = BSON("ts" << opTime << "wall" << wallTime << "str"
                                        << "");
        ASSERT_LTE(objTemplate.objsize(), size);
        std::string str(size - objTemplate.objsize(), fill);

        BSONObj obj = BSON("ts" << opTime << "wall" << wallTime << "str" << str);
        ASSERT_EQ(size, obj.objsize());

        return obj;
    }

    BSONObj makeBSONObjWithSize(unsigned int t, int size, char fill = 'x') {
        return makeBSONObjWithSize(1, t, size, fill);
    }

    BSONObj insertOplog(unsigned int seconds, unsigned int t, int size) {
        auto obj = makeBSONObjWithSize(seconds, t, size);
        AutoGetOplogFastPath oplogWrite(_opCtx.get(), OplogAccessMode::kWrite);
        const auto& oplog = oplogWrite.getCollection();
        std::vector<Record> records{{RecordId(), RecordData(obj.objdata(), obj.objsize())}};
        std::vector<Timestamp> timestamps{Timestamp()};
        WriteUnitOfWork wuow(_opCtx.get());
        ASSERT_OK(internal::insertDocumentsForOplog(_opCtx.get(), oplog, &records, timestamps));
        wuow.commit();
        return obj;
    }

    BSONObj insertOplog(unsigned int t, int size) {
        return insertOplog(1, t, size);
    }

    /**
     * Advances the stable timestamp of the engine.
     */
    void advanceStableTimestamp(Timestamp newTimestamp) {
        // Disable the callback for oldest active transaction as it blocks the timestamps from
        // advancing.
        auto service = getServiceContext();
        service->getStorageEngine()->setOldestActiveTransactionTimestampCallback(
            StorageEngine::OldestActiveTransactionTimestampCallback{});
        _storage.setInitialDataTimestamp(service, newTimestamp);
        _storage.setStableTimestamp(service, newTimestamp, true);
        service->getStorageEngine()->checkpoint();
    }

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = cc().makeOperationContext();
        _consistencyMarkers = std::make_unique<ReplicationConsistencyMarkersMock>();
        auto service = getServiceContext();
        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(service);
        ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(_opCtx.get());
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        _consistencyMarkers.reset();
        ServiceContextMongoDTest::tearDown();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    StorageInterfaceImpl _storage;
    std::unique_ptr<ReplicationConsistencyMarkersMock> _consistencyMarkers;
};

/**
 * Insert records into an oplog and verify the number of truncate markers that are created.
 */
TEST_F(OplogTruncationTest, OplogTruncateMarkers_CreateNewMarker) {
    auto opCtx = getOperationContext();

    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());

    // Inserting a record smaller than 'minBytesPerTruncateMarker' shouldn't create a new oplog
    // truncate marker.
    insertOplog(1, 99);
    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(99, oplogTruncateMarkers->currentBytes_forTest());

    // Inserting another record such that their combined size exceeds
    // 'minBytesPerTruncateMarker' should cause a new truncate marker to be created.
    insertOplog(2, 51);
    ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());

    // Inserting a record such that the combined size of this record and the previously inserted
    // one exceed 'minBytesPerTruncateMarker' shouldn't cause a new truncate marker to be
    // created because we've started filling a new truncate marker.
    insertOplog(3, 50);
    ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());

    // Inserting a record such that the combined size of this record and the previously inserted
    // one is exactly equal to 'minBytesPerTruncateMarker' should cause a new truncate marker to
    // be created.
    insertOplog(4, 50);
    ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());

    // Inserting a single record that exceeds 'minBytesPerTruncateMarker' should cause a new
    // truncate marker to be created.
    insertOplog(5, 101);
    ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
}

/**
 * Insert multiple records and truncate the oplog collection. The operation
 * should leave no truncate markers, including the partially filled one.
 */
TEST_F(OplogTruncationTest, OplogTruncateMarkers_Truncate) {
    auto opCtx = getOperationContext();
    auto& storage = getStorage();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    unsigned int count = 3;
    int size = 50;
    for (unsigned int t = 1; t <= count; t++) {
        insertOplog(t, size);
    }

    ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(size, oplogTruncateMarkers->currentBytes_forTest());

    ASSERT_EQ(count, rs->numRecords());
    ASSERT_EQ(size * count, rs->dataSize());

    ASSERT_OK(storage.truncateCollection(opCtx, oplogNs));

    ASSERT_EQ(0, rs->dataSize());
    ASSERT_EQ(0, rs->numRecords());
    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
}

/**
 * Insert records into an oplog and try to update them. The updates shouldn't succeed if the size of
 * record is changed.
 */
TEST_F(OplogTruncationTest, OplogTruncateMarkers_UpdateRecord) {
    auto opCtx = getOperationContext();
    auto& storage = getStorage();

    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    // Insert two records such that one makes up a full truncate marker and the other is a part of
    // the truncate marker currently being filled.
    auto obj1 = insertOplog(1, 100);
    auto obj2 = insertOplog(2, 50);
    storage.oplogDiskLocRegister(opCtx, Timestamp{1, 2}, true);

    ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());

    // Attempts to grow the records should fail.
    {
        BSONObj changed1 = makeBSONObjWithSize(1, 101);
        TimestampedBSONObj update1 = {BSON("$set" << changed1), {}};
        ASSERT_NOT_OK(storage.updateSingleton(opCtx, oplogNs, BSON("ts" << obj1["ts"]), update1));

        BSONObj changed2 = makeBSONObjWithSize(1, 51);
        TimestampedBSONObj update2 = {BSON("$set" << changed2), {}};
        ASSERT_NOT_OK(storage.updateSingleton(opCtx, oplogNs, BSON("ts" << obj2["ts"]), update2));
    }

    // Attempts to shrink the records should also fail.
    {
        BSONObj changed1 = makeBSONObjWithSize(1, 99);
        TimestampedBSONObj update1 = {BSON("$set" << changed1), {}};
        ASSERT_NOT_OK(storage.updateSingleton(opCtx, oplogNs, BSON("ts" << obj1["ts"]), update1));

        BSONObj changed2 = makeBSONObjWithSize(1, 49);
        TimestampedBSONObj update2 = {BSON("$set" << changed2), {}};
        ASSERT_NOT_OK(storage.updateSingleton(opCtx, oplogNs, BSON("ts" << obj2["ts"]), update2));
    }

    // Changing the contents of the records without changing their size should succeed.
    {
        BSONObj changed1 = makeBSONObjWithSize(1, 100, 'y');
        TimestampedBSONObj update1 = {BSON("$set" << changed1), {}};
        ASSERT_OK(storage.updateSingleton(opCtx, oplogNs, BSON("ts" << obj1["ts"]), update1));

        BSONObj changed2 = makeBSONObjWithSize(1, 50, 'z');
        TimestampedBSONObj update2 = {BSON("$set" << changed2), {}};
        ASSERT_OK(storage.updateSingleton(opCtx, oplogNs, BSON("ts" << obj2["ts"]), update2));

        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }
}

/**
 * Insert multiple records, truncate the oplog using RecordStore::Capped::truncateAfter(), and
 * verify that the metadata for each truncate marker is updated. If a full truncate marker is
 * partially truncated, then it should become the truncate marker currently being filled.
 */
TEST_F(OplogTruncationTest, OplogTruncateMarkers_CappedTruncateAfter) {
    auto opCtx = getOperationContext();
    auto& storage = getStorage();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    oplogTruncateMarkers->setMinBytesPerMarker(1000);

    {
        insertOplog(1, 400);
        insertOplog(2, 800);
        insertOplog(3, 200);
        insertOplog(4, 250);
        insertOplog(5, 300);
        insertOplog(6, 350);
        insertOplog(7, 50);
        insertOplog(8, 100);
        insertOplog(9, 150);
        storage.oplogDiskLocRegister(opCtx, Timestamp{1, 9}, true);

        ASSERT_EQ(9, rs->numRecords());
        ASSERT_EQ(2600, rs->dataSize());
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(3, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(300, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Make sure all are visible.
    storage.waitForAllEarlierOplogWritesToBeVisible(opCtx, rs);

    // Truncate data using an inclusive RecordId that exists inside the truncate marker currently
    // being filled.
    {
        WriteUnitOfWork wunit(opCtx);
        RecordStore::Capped::TruncateAfterResult result =
            rs->capped()->truncateAfter(opCtx,
                                        *shard_role_details::getRecoveryUnit(opCtx),
                                        RecordId(1, 8),
                                        true /* inclusive */);
        wunit.commit();
        oplogTruncateMarkers->updateMarkersAfterCappedTruncateAfter(
            result.recordsRemoved, result.bytesRemoved, result.firstRemovedId);

        ASSERT_EQ(7, rs->numRecords());
        ASSERT_EQ(2350, rs->dataSize());
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate data using an inclusive RecordId that refers to the 'lastRecord' of a full truncate
    // marker.
    // The truncate marker should become the one currently being filled.
    {
        WriteUnitOfWork wunit(opCtx);
        RecordStore::Capped::TruncateAfterResult result =
            rs->capped()->truncateAfter(opCtx,
                                        *shard_role_details::getRecoveryUnit(opCtx),
                                        RecordId(1, 6),
                                        true /* inclusive */);
        wunit.commit();
        oplogTruncateMarkers->updateMarkersAfterCappedTruncateAfter(
            result.recordsRemoved, result.bytesRemoved, result.firstRemovedId);

        ASSERT_EQ(5, rs->numRecords());
        ASSERT_EQ(1950, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(3, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(750, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Now test the high level truncateOplogToTimestamp API.
    ReplicationRecoveryImpl recovery(&storage, getConsistencyMarkers());

    // Truncate data using a non-inclusive RecordId that exists inside the truncate marker currently
    // being filled.
    {
        recovery.truncateOplogToTimestamp(opCtx, Timestamp(1, 3));
        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(1400, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(200, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate data using a non-inclusive RecordId that refers to the 'lastRecord' of a full
    // truncate marker.
    // The truncate marker should remain intact.
    {
        recovery.truncateOplogToTimestamp(opCtx, Timestamp(1, 2));
        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(1200, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate data using a non-inclusive RecordId that exists inside a full truncate marker. The
    // truncate marker should become the one currently being filled.
    {
        recovery.truncateOplogToTimestamp(opCtx, Timestamp(1, 1));
        ASSERT_EQ(1, rs->numRecords());
        ASSERT_EQ(400, rs->dataSize());
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(400, oplogTruncateMarkers->currentBytes_forTest());
    }
}

/**
 * Verify that oplog truncate markers are reclaimed when cappedMaxSize is exceeded.
 */
TEST_F(OplogTruncationTest, ReclaimTruncateMarkers) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
    auto engine = getServiceContext()->getStorageEngine();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    ASSERT_OK(rs->oplog()->updateSize(230));

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    {
        insertOplog(1, 100);
        insertOplog(2, 110);
        insertOplog(3, 120);

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(330, rs->dataSize());
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Fail to truncate the truncate marker when cappedMaxSize is exceeded, but the persisted
    // timestamp is before the truncation point (i.e: leaves a gap that replication recovery would
    // rely on).
    {
        advanceStableTimestamp(Timestamp(1, 0));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo = oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);

        ASSERT_EQ(RecordId(), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 1), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(330, rs->dataSize());
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate a truncate marker when cappedMaxSize is exceeded.
    {
        advanceStableTimestamp(Timestamp(1, 3));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo = oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 1), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 2), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(230, rs->dataSize());
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    {
        insertOplog(4, 130);
        insertOplog(5, 140);
        insertOplog(6, 50);

        ASSERT_EQ(5, rs->numRecords());
        ASSERT_EQ(550, rs->dataSize());
        ASSERT_EQ(4U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Truncate multiple truncate markers if necessary.
    {
        advanceStableTimestamp(Timestamp(1, 6));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo = oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 4), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 5), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(190, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // No-op if dataSize <= cappedMaxSize.
    {
        advanceStableTimestamp(Timestamp(1, 6));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo = oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);

        ASSERT_EQ(RecordId(), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 5), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(190, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Don't truncate the last truncate marker before the truncate point, even if the truncate point
    // is ahead of it.
    {
        insertOplog(7, 190);
        insertOplog(9, 120);

        ASSERT_EQ(4, rs->numRecords());
        ASSERT_EQ(500, rs->dataSize());
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }
    {
        advanceStableTimestamp(Timestamp(1, 8));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo = oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 5), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 6), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(360, rs->dataSize());
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // Don't truncate entire oplog.
    {
        insertOplog(10, 90);
        insertOplog(11, 210);

        ASSERT_EQ(5, rs->numRecords());
        ASSERT_EQ(660, rs->dataSize());
        ASSERT_EQ(3U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    {
        advanceStableTimestamp(Timestamp(1, 12));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo = oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 9), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 10), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(2, rs->numRecords());
        ASSERT_EQ(300, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }

    // OK to truncate all truncate markers if there are records in the oplog that are before or at
    // the truncate-up-to point, that have not yet created a truncate marker.
    {
        // Use timestamp (1, 13) as we can't commit at the stable timestamp (1, 12).
        insertOplog(13, 90);

        ASSERT_EQ(3, rs->numRecords());
        ASSERT_EQ(390, rs->dataSize());
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(90, oplogTruncateMarkers->currentBytes_forTest());
    }
    {
        advanceStableTimestamp(Timestamp(1, 13));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        auto truncatedUpTo = oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);

        ASSERT_EQ(RecordId(1, 11), truncatedUpTo);
        ASSERT_EQ(Timestamp(1, 13), rs->oplog()->getEarliestTimestamp(ru).getValue());

        ASSERT_EQ(1, rs->numRecords());
        ASSERT_EQ(90, rs->dataSize());
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(90, oplogTruncateMarkers->currentBytes_forTest());
    }
}

/**
 * Verify that an oplog truncate marker isn't created if it would cause the logical representation
 * of the records to not be in increasing order.
 */
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AscendingOrder) {
    auto opCtx = getOperationContext();

    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    {
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        insertOplog(2, 2, 50);  // Timestamp(2, 2)
        ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());

        // Inserting a record that has a smaller RecordId than the previously inserted record should
        // be able to create a new truncate marker when no truncate markers already exist.
        insertOplog(2, 1, 50);  // Timestamp(2, 1)
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());

        // However, inserting a record that has a smaller RecordId than most recently created
        // truncate marker's last record shouldn't cause a new truncate marker to be created, even
        // if the size of the inserted record exceeds 'minBytesPerTruncateMarker'.
        insertOplog(1, 100);  // Timestamp(1, 1)
        ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(100, oplogTruncateMarkers->currentBytes_forTest());

        // Inserting a record that has a larger RecordId than the most recently created truncate
        // marker's last record should then cause a new truncate marker to be created.
        insertOplog(2, 3, 50);  // Timestamp(2, 3)
        ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
        ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
    }
}

// When the oplog collection is non-empty, but no OplogTruncateMarkers are
// generated because the estimated 'dataSize' is smaller than the minimum size for a truncate
// marker, tests that
//  (1) The oplog is scanned
//  (2) OplogTruncateMarkers::currentBytes_forTest() reflects the actual size of the oplog instead
//  of the estimated size.
TEST_F(OplogTruncationTest, OplogTruncateMarkers_NoMarkersGeneratedFromScanning) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
    auto wtRS = static_cast<WiredTigerRecordStore::Oplog*>(rs);

    int realNumRecords = 4;
    int realSizePerRecord = 100;
    for (int i = 1; i <= realNumRecords; i++) {
        insertOplog(i, realSizePerRecord);
    }

    // Force the estimates of 'dataSize' and 'numRecords' to be lower than the real values.
    wtRS->setNumRecords(realNumRecords - 1);
    wtRS->setDataSize((realNumRecords - 1) * realSizePerRecord);

    // Re-initialize the truncate markers.
    LocalOplogInfo::get(opCtx)->setRecordStore(opCtx, rs);
    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    // Confirm that small oplogs are processed by scanning.
    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::Scanning,
              oplogTruncateMarkers->getMarkersCreationMethod());
    ASSERT_GTE(oplogTruncateMarkers->getCreationProcessingTime().count(), 0);
    auto numMarkers = oplogTruncateMarkers->numMarkers_forTest();
    ASSERT_EQ(numMarkers, 0U);

    // A forced scan over the RecordStore should force the 'currentBytes' to be accurate in the
    // truncate markers as well as the RecordStore's 'numRecords' and 'dataSize'.
    ASSERT_EQ(oplogTruncateMarkers->currentBytes_forTest(), realNumRecords * realSizePerRecord);
    ASSERT_EQ(wtRS->dataSize(), realNumRecords * realSizePerRecord);
    ASSERT_EQ(wtRS->numRecords(), realNumRecords);
}

// Ensure that if we sample and create duplicate oplog truncate markers, perform truncation
// correctly, and with no crashing behavior. This scenario may be possible if the same record is
// sampled multiple times during startup, which can be very likely if the size storer is very
// inaccurate.
TEST_F(OplogTruncationTest, OplogTruncateMarkers_Duplicates) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
    auto wtRS = static_cast<WiredTigerRecordStore::Oplog*>(rs);
    auto engine = getServiceContext()->getStorageEngine();

    {
        // Before initializing the RecordStore, populate with a few records.
        insertOplog(1, 100);
        insertOplog(2, 100);
        insertOplog(3, 100);
        insertOplog(4, 100);
    }

    {
        // Force initialize the oplog truncate markers to use sampling by providing very large,
        // inaccurate sizes. This should cause us to over sample the records in the oplog.
        ASSERT_OK(wtRS->updateSize(1024 * 1024 * 1024));
        wtRS->setNumRecords(1024 * 1024);
        wtRS->setDataSize(1024 * 1024 * 1024);
    }

    // Confirm that some truncate markers were generated.
    LocalOplogInfo::get(opCtx)->setRecordStore(opCtx, rs);
    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::Sampling,
              oplogTruncateMarkers->getMarkersCreationMethod());
    ASSERT_GTE(oplogTruncateMarkers->getCreationProcessingTime().count(), 0);
    auto truncateMarkersBefore = oplogTruncateMarkers->numMarkers_forTest();
    ASSERT_GT(truncateMarkersBefore, 0U);
    ASSERT_GT(oplogTruncateMarkers->currentBytes_forTest(), 0);

    {
        // Reclaiming should do nothing because the data size is still under the maximum.
        advanceStableTimestamp(Timestamp(1, 4));
        auto mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);
        ASSERT_EQ(truncateMarkersBefore, oplogTruncateMarkers->numMarkers_forTest());

        // Reduce the oplog size to ensure we create a truncate marker and truncate on the next
        // insert.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        AutoGetCollection coll(opCtx, oplogNs, MODE_X);
        CollectionWriter writer{opCtx, coll};
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(writer.getWritableCollection(opCtx)->updateCappedSize(
            opCtx, 400, /*newCappedMax=*/boost::none));
        wuow.commit();

        // Inserting these records should meet the requirements for truncation. That is: there is a
        // record, 5, after the last truncate marker, 4, and before the truncation point, 6.
        insertOplog(5, 100);
        insertOplog(6, 100);

        // Ensure every truncate marker has been cleaned up except for the last one ending in 6.
        advanceStableTimestamp(Timestamp(1, 6));
        mayTruncateUpTo = RecordId(engine->getPinnedOplog().asULL());
        oplog_truncation::reclaimOplog(opCtx, *rs, mayTruncateUpTo);
        ASSERT_EQ(1, oplogTruncateMarkers->numMarkers_forTest());

        // The original oplog should have rolled over and the size and count should be accurate.
        ASSERT_EQ(1, wtRS->numRecords());
        ASSERT_EQ(100, wtRS->dataSize());
    }
}

// In async mode, markers are created as expected.
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeMarkerCreation) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);


    auto oplogTruncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
    ASSERT(oplogTruncateMarkers);

    oplogTruncateMarkers->setMinBytesPerMarker(100);

    // Inserting a record smaller than 'minBytesPerTruncateMarker' shouldn't create a new oplog
    // truncate marker.
    insertOplog(1, 99);
    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(99, oplogTruncateMarkers->currentBytes_forTest());

    // Inserting another record such that their combined size exceeds
    // 'minBytesPerTruncateMarker' should cause a new truncate marker to be created.
    insertOplog(2, 51);
    ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());

    // Inserting a record such that the combined size of this record and the previously inserted
    // one exceed 'minBytesPerTruncateMarker' shouldn't cause a new truncate marker to be
    // created because we've started filling a new truncate marker.
    insertOplog(3, 50);
    ASSERT_EQ(1U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(1, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(50, oplogTruncateMarkers->currentBytes_forTest());

    // Inserting a record such that the combined size of this record and the previously inserted
    // one is exactly equal to 'minBytesPerTruncateMarker' should cause a new truncate marker to
    // be created.
    insertOplog(4, 50);
    ASSERT_EQ(2U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());

    ASSERT_EQ(4, rs->numRecords());
    ASSERT_EQ(250, rs->dataSize());
}

// In async mode, sampleAndUpdate is called seperately from createOplogTruncateMarkers, and
// creates the initial set of markers.
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeSampleAndUpdate) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);

    // Populate oplog to force marker creation to occur
    int realNumRecords = 4;
    int realSizePerRecord = 1024 * 1024;
    for (int i = 1; i <= realNumRecords; i++) {
        insertOplog(i, realSizePerRecord);
    }

    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers = OplogTruncateMarkers::sampleAndUpdate(opCtx, *rs);

    // Confirm that some truncate markers were generated.
    ASSERT_LT(0U, oplogTruncateMarkers->numMarkers_forTest());
}

// In async mode, during startup but before sampling finishes, creation method is InProgress. This
// should then resolve to either the Scanning or Sampling method once initial marker creation has
// finished
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeInProgressState) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);

    // Populate oplog to so that initial marker creation method is not EmptyCollection
    insertOplog(1, 100);

    // Note if in async mode, at this point we have not yet sampled.
    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    // Confirm that we are in InProgress state since sampling/scanning has not begun.
    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::InProgress,
              oplogTruncateMarkers->getMarkersCreationMethod());

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers = OplogTruncateMarkers::sampleAndUpdate(opCtx, *rs);

    // Check that the InProgress state has now been resolved.
    ASSERT(oplogTruncateMarkers->getMarkersCreationMethod() ==
           CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
}

// In async mode, we are still able to sample when expected, and some markers can be created.
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeSampling) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
    auto wtRS = static_cast<WiredTigerRecordStore::Oplog*>(rs);

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);

    {
        // Before initializing the RecordStore, populate with a few records.
        insertOplog(1, 100);
        insertOplog(2, 100);
        insertOplog(3, 100);
        insertOplog(4, 100);
    }

    {
        // Force initialize the oplog truncate markers to use sampling by providing very large,
        // inaccurate sizes. This should cause us to over sample the records in the oplog.
        ASSERT_OK(wtRS->updateSize(1024 * 1024 * 1024));
        wtRS->setNumRecords(1024 * 1024);
        wtRS->setDataSize(1024 * 1024 * 1024);
    }

    LocalOplogInfo::get(opCtx)->setRecordStore(opCtx, rs);
    // Note if in async mode, at this point we have not yet sampled.
    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers = OplogTruncateMarkers::sampleAndUpdate(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    // Confirm that we can in fact sample
    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::Sampling,
              oplogTruncateMarkers->getMarkersCreationMethod());
    // Confirm that some truncate markers were generated.
    ASSERT_GTE(oplogTruncateMarkers->getCreationProcessingTime().count(), 0);
    auto truncateMarkersBefore = oplogTruncateMarkers->numMarkers_forTest();
    ASSERT_GT(truncateMarkersBefore, 0U);
    ASSERT_GT(oplogTruncateMarkers->currentBytes_forTest(), 0);
}

// In async mode, markers are not created during createOplogTruncateMarkers (which instead returns
// empty OplogTruncateMarkers object)
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeCreateOplogTruncateMarkers) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);

    // Note if in async mode, at this point we have not yet sampled.
    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
}
}  // namespace repl
}  // namespace mongo
