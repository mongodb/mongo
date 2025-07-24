/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstring>
#include <string>
#include <utility>
#include <wiredtiger.h>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_truncate_markers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {
const auto& oplogNs = NamespaceString::kRsOplogNamespace;

class OplogTruncationTest : public ServiceContextMongoDTest {
protected:
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    repl::StorageInterface& getStorage() {
        return _storage;
    }

    repl::ReplicationConsistencyMarkers* getConsistencyMarkers() {
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
        _consistencyMarkers = std::make_unique<repl::ReplicationConsistencyMarkersMock>();
        auto service = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(_opCtx.get());
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        _consistencyMarkers.reset();
        ServiceContextMongoDTest::tearDown();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    repl::StorageInterfaceImpl _storage;
    std::unique_ptr<repl::ReplicationConsistencyMarkersMock> _consistencyMarkers;
};

// In async mode, markers are created as expected.
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeMarkerCreation) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);

    auto wtRS = static_cast<WiredTigerRecordStore*>(rs);
    auto oplogTruncateMarkers = wtRS->oplogTruncateMarkers();
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

    ASSERT_EQ(4, rs->numRecords(opCtx));
    ASSERT_EQ(250, rs->dataSize(opCtx));
}

// In async mode, sampleAndUpdate is called seperately from createOplogTruncateMarkers, and
// creates the initial set of markers.
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeSampleAndUpdate) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
    auto wtRS = static_cast<WiredTigerRecordStore*>(rs);

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);

    // Populate oplog to force marker creation to occur
    int realNumRecords = 4;
    int realSizePerRecord = 1024 * 1024;
    for (int i = 1; i <= realNumRecords; i++) {
        insertOplog(i, realSizePerRecord);
    }

    auto oplogTruncateMarkers =
        WiredTigerRecordStore::OplogTruncateMarkers::createOplogTruncateMarkers(
            opCtx, wtRS, oplogNs);
    ASSERT(oplogTruncateMarkers);

    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers =
        WiredTigerRecordStore::OplogTruncateMarkers::sampleAndUpdate(opCtx, wtRS, oplogNs);

    // Confirm that some truncate markers were generated.
    ASSERT_LT(0U, oplogTruncateMarkers->numMarkers_forTest());
}

// In async mode, during startup but before sampling finishes, creation method is InProgress. This
// should then resolve to either the Scanning or Sampling method once initial marker creation has
// finished
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeInProgressState) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
    auto wtRS = static_cast<WiredTigerRecordStore*>(rs);

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);

    // Populate oplog to so that initial marker creation method is not EmptyCollection
    insertOplog(1, 100);

    // Note if in async mode, at this point we have not yet sampled.
    auto oplogTruncateMarkers =
        WiredTigerRecordStore::OplogTruncateMarkers::createOplogTruncateMarkers(
            opCtx, wtRS, oplogNs);
    ASSERT(oplogTruncateMarkers);

    // Confirm that we are in InProgress state since sampling/scanning has not begun.
    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::InProgress,
              oplogTruncateMarkers->getMarkersCreationMethod());

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers =
        WiredTigerRecordStore::OplogTruncateMarkers::sampleAndUpdate(opCtx, wtRS, oplogNs);

    // Check that the InProgress state has now been resolved.
    ASSERT(oplogTruncateMarkers->getMarkersCreationMethod() ==
           CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
}

// In async mode, we are still able to sample when expected, and some markers can be created.
TEST_F(OplogTruncationTest, OplogTruncateMarkers_AsynchronousModeSampling) {
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();
    auto wtRS = static_cast<WiredTigerRecordStore*>(rs);

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
        ASSERT_OK(wtRS->updateOplogSize(opCtx, 1024 * 1024 * 1024));
        wtRS->setNumRecords(1024 * 1024);
        wtRS->setDataSize(1024 * 1024 * 1024);
    }

    LocalOplogInfo::get(opCtx)->setRecordStore(rs);
    // Note if in async mode, at this point we have not yet sampled.
    auto oplogTruncateMarkers =
        WiredTigerRecordStore::OplogTruncateMarkers::createOplogTruncateMarkers(
            opCtx, wtRS, oplogNs);
    ASSERT(oplogTruncateMarkers);

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers =
        WiredTigerRecordStore::OplogTruncateMarkers::sampleAndUpdate(opCtx, wtRS, oplogNs);
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
    auto wtRS = static_cast<WiredTigerRecordStore*>(rs);

    // Turn on async mode
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagOplogSamplingAsyncEnabled", true);

    // Note if in async mode, at this point we have not yet sampled.
    auto oplogTruncateMarkers =
        WiredTigerRecordStore::OplogTruncateMarkers::createOplogTruncateMarkers(
            opCtx, wtRS, oplogNs);
    ASSERT(oplogTruncateMarkers);

    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
}
}  // namespace
}  // namespace mongo
