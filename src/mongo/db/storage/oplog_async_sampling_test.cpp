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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/oplog_cap_maintainer_thread.h"
#include "mongo/db/storage/oplog_truncation.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace repl {
const auto& oplogNs = NamespaceString::kRsOplogNamespace;

class AsyncOplogTruncationTest : public ServiceContextMongoDTest {
protected:
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    StorageInterface& getStorage() {
        return _storage;
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

    std::shared_ptr<OplogTruncateMarkers> beginMarkerCreation(OperationContext* opCtx,
                                                              RecordStore& rs) {
        auto initialSetOfMarkers = OplogTruncateMarkers::beginMarkerCreation(opCtx, rs);
        return std::make_shared<OplogTruncateMarkers>(std::move(initialSetOfMarkers), *rs.oplog());
    }

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = cc().makeOperationContext();
        auto service = getServiceContext();
        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(service);
        ReplicationCoordinator::set(service, std::move(replCoord));
        // Turn on async mode
        RAIIServerParameterControllerForTest oplogSamplingAsyncEnabledController(
            "oplogSamplingAsyncEnabled", true);
        repl::createOplog(_opCtx.get());
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        ServiceContextMongoDTest::tearDown();
    }

    // Use 0ms yield interval (i.e. yield every next()) in tests.
    RAIIServerParameterControllerForTest _zeroMsYield =
        RAIIServerParameterControllerForTest("oplogSamplingAsyncYieldIntervalMs", 0);
    ServiceContext::UniqueOperationContext _opCtx;
    StorageInterfaceImpl _storage;
};

// In async mode, beginMarkerCreation is called separately from createOplogTruncateMarkers, and
// creates the initial set of markers.
TEST_F(AsyncOplogTruncationTest, OplogTruncateMarkers_AsynchronousModeBeginMarkerCreation) {
    // Turn on async mode
    RAIIServerParameterControllerForTest oplogSamplingAsyncEnabledController(
        "oplogSamplingAsyncEnabled", true);
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    // Populate oplog to force marker creation to occur
    int realNumRecords = 4;
    int realSizePerRecord = 1024 * 1024;
    for (int i = 1; i <= realNumRecords; i++) {
        insertOplog(i, realSizePerRecord);
    }

    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers());

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers = beginMarkerCreation(opCtx, *rs);

    // Confirm that some truncate markers were generated.
    ASSERT_LT(0U, oplogTruncateMarkers->numMarkers());
}  // namespace repl

// In async mode, during startup but before sampling finishes,
//  creation method is InProgress.This should then resolve to either the Scanning or
// Sampling method once initial marker creation has finished
TEST_F(AsyncOplogTruncationTest, OplogTruncateMarkers_AsynchronousModeInProgressState) {
    // Turn on async mode
    RAIIServerParameterControllerForTest oplogSamplingAsyncEnabledController(
        "oplogSamplingAsyncEnabled", true);
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    // Populate oplog to so that initial marker creation method is not EmptyCollection
    insertOplog(1, 100);

    // Note if in async mode, at this point we have not yet sampled.
    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    // Confirm that we are in InProgress state since sampling/scanning has not begun.
    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::InProgress,
              oplogTruncateMarkers->getMarkersCreationMethod());

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers = beginMarkerCreation(opCtx, *rs);

    // Check that the InProgress state has now been resolved.
    ASSERT(oplogTruncateMarkers->getMarkersCreationMethod() ==
           CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
}

// In async mode, we are still able to sample when expected, and some markers can be created.
TEST_F(AsyncOplogTruncationTest, OplogTruncateMarkers_AsynchronousModeSampling) {
    // Turn on async mode
    RAIIServerParameterControllerForTest oplogSamplingAsyncEnabledController(
        "oplogSamplingAsyncEnabled", true);
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

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
        ASSERT_OK(rs->oplog()->updateSize(1024 * 1024 * 1024));
        rs->setSize(/*numRecords=*/1024 * 1024, /*dataSize=*/1024 * 1024 * 1024);
    }

    LocalOplogInfo::get(opCtx)->setRecordStore(opCtx, rs);
    // Note if in async mode, at this point we have not yet sampled.
    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    // Continue finishing the initial scan / sample
    oplogTruncateMarkers = beginMarkerCreation(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    // Confirm that we can in fact sample
    ASSERT_EQ(CollectionTruncateMarkers::MarkersCreationMethod::Sampling,
              oplogTruncateMarkers->getMarkersCreationMethod());
    // Confirm that some truncate markers were generated.
    ASSERT_GTE(oplogTruncateMarkers->getCreationProcessingTime().count(), 0);
    auto truncateMarkersBefore = oplogTruncateMarkers->numMarkers();
    ASSERT_GT(truncateMarkersBefore, 0U);
    ASSERT_GT(oplogTruncateMarkers->currentBytes_forTest(), 0);
}

// In async mode, markers are not created during createOplogTruncateMarkers (which instead
// returns empty OplogTruncateMarkers object)
TEST_F(AsyncOplogTruncationTest, OplogTruncateMarkers_AsynchronousModeCreateOplogTruncateMarkers) {
    // Turn on async mode
    RAIIServerParameterControllerForTest oplogSamplingAsyncEnabledController(
        "oplogSamplingAsyncEnabled", true);
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    // Note if in async mode, at this point we have not yet sampled.
    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);

    ASSERT_EQ(0U, oplogTruncateMarkers->numMarkers());
    ASSERT_EQ(0, oplogTruncateMarkers->currentRecords_forTest());
    ASSERT_EQ(0, oplogTruncateMarkers->currentBytes_forTest());
}

// When oplogSamplingAsyncEnabled is false, AttachedPersistenceProvider turns off async marker
// generation; createOplogTruncateMarkers must then build the initial markers synchronously.
TEST_F(AsyncOplogTruncationTest, OplogTruncateMarkers_SynchronousPathWhenAsyncDisabled) {
    RAIIServerParameterControllerForTest oplogSamplingAsyncEnabledController(
        "oplogSamplingAsyncEnabled", false);
    auto opCtx = getOperationContext();
    auto rs = LocalOplogInfo::get(opCtx)->getRecordStore();

    int realNumRecords = 4;
    int realSizePerRecord = 1024 * 1024;
    for (int i = 1; i <= realNumRecords; i++) {
        insertOplog(i, realSizePerRecord);
    }

    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    auto oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    ASSERT(oplogTruncateMarkers);
    ASSERT_LT(0U, oplogTruncateMarkers->numMarkers());
}

// Test that oplog cap maintainer thread kills the truncation markers if it instantiated them for
// async sampling.
TEST_F(AsyncOplogTruncationTest, ShutdownKillsMarkersAndClearsLocalOplogInfo) {
    RAIIServerParameterControllerForTest oplogSamplingAsyncEnabledController(
        "oplogSamplingAsyncEnabled", true);

    // Populate the oplog so initial marker generation produces a real, non-empty markers object.
    insertOplog(1, 100);

    // Pin the thread at the top of its main loop via the existing failpoint. This ensures that
    // by the time we call shutdown(), the thread has finished initial marker creation and
    // installed the markers into LocalOplogInfo, and is no longer touching them from
    // `_deleteExcessDocuments`.
    auto* hangFp = globalFailPointRegistry().find("hangOplogCapMaintainerThread");
    ASSERT(hangFp);
    auto timesEnteredBefore = hangFp->setMode(FailPoint::alwaysOn);

    auto thread = std::make_unique<OplogCapMaintainerThread>();
    auto* threadRaw = thread.get();
    OplogCapMaintainerThread::set(getServiceContext(), std::move(thread));
    threadRaw->go();

    // Wait for initial markers to be installed, then wait for the thread to actually enter the
    // pause so shutdown() isn't racing the thread's loop prologue.
    std::shared_ptr<OplogTruncateMarkers> installedMarkers;
    for (int i = 0; i < 200 && !installedMarkers; ++i) {  // up to ~20s
        installedMarkers = LocalOplogInfo::get(getOperationContext())->getTruncateMarkers();
        if (!installedMarkers) {
            sleepmillis(100);
        }
    }
    ASSERT(installedMarkers) << "Thread did not install truncate markers within timeout";
    ASSERT_FALSE(installedMarkers->isDead());
    hangFp->waitForTimesEntered(timesEnteredBefore + 1);

    // Keep our own reference to the installed markers so we can observe isDead() after
    // shutdown() clears the LocalOplogInfo slot.
    threadRaw->shutdown(
        Status(ErrorCodes::InterruptedDueToReplStateChange, "test step-down reason"));

    // 1. The markers instance we captured was kill()'d during shutdown(). A killed markers
    //    object is the signal that any waiter in awaitHas...OrDead should unwind via _isDead.
    ASSERT_TRUE(installedMarkers->isDead());

    // 2. The LocalOplogInfo slot has been nulled out, so any post-stepdown getTruncateMarkers()
    //    call (e.g. from the oplog write path's invariant check) sees no markers.
    ASSERT_FALSE(LocalOplogInfo::get(getOperationContext())->getTruncateMarkers());

    hangFp->setMode(FailPoint::off);
}

// Tests oplog cap maintainer shutdown without hanging the thread with a failpoint. Runs multiple
// iterations with varying delays so shutdown() races the thread in different states — idle inside
// awaitHas...OrDead, holding the global lock inside _deleteExcessDocuments, between loop
// iterations, etc. Each iteration installs a fresh thread, lets it produce initial markers,
// then immediately shuts it down and checks the post-shutdown state.
TEST_F(AsyncOplogTruncationTest, ShutdownRacesWithLiveCapMaintainerThread) {
    RAIIServerParameterControllerForTest oplogSamplingAsyncEnabledController(
        "oplogSamplingAsyncEnabled", true);

    insertOplog(1, 100);

    // Per-iteration delay between "markers installed" and shutdown(). 0 reproduces "shut
    // down immediately after initial creation"; larger values give the thread time to reach
    // awaitHas...OrDead or cycle through the main loop.
    const std::vector<int> kIterationDelayMillis{0, 0, 1, 5, 10, 25, 50, 100};

    for (size_t i = 0; i < kIterationDelayMillis.size(); ++i) {
        auto thread = std::make_unique<OplogCapMaintainerThread>();
        auto* threadRaw = thread.get();
        OplogCapMaintainerThread::set(getServiceContext(), std::move(thread));
        threadRaw->go();

        // Wait until the thread has finished initial marker creation and installed the
        // markers into LocalOplogInfo. That's the signal that _uniqueCtx is set, so
        // shutdown()'s async-branch code will actually run.
        std::shared_ptr<OplogTruncateMarkers> installedMarkers;

        size_t waitMs = 0;
        for (; waitMs < 2000 && !installedMarkers; waitMs += 2) {
            installedMarkers = LocalOplogInfo::get(getOperationContext())->getTruncateMarkers();
            if (!installedMarkers) {
                sleepmillis(2);
            }
        }
        LOGV2(12511002, "Installed markers", "waitMs"_attr = waitMs - 2);

        ASSERT(installedMarkers) << "i=" << i << ": thread did not install markers within timeout";
        ASSERT_FALSE(installedMarkers->isDead()) << "i=" << i;

        if (kIterationDelayMillis[i] > 0) {
            sleepmillis(kIterationDelayMillis[i]);
        }

        threadRaw->shutdown(
            Status(ErrorCodes::InterruptedDueToReplStateChange, "race-test step-down"));

        // Regardless of which state the thread was in when shutdown() landed, the three
        // end-state invariants must hold: kill() ran on the markers, LocalOplogInfo was
        // nulled out, and the thread has cleanly joined.
        ASSERT_TRUE(installedMarkers->isDead())
            << "i=" << i << " delayMs=" << kIterationDelayMillis[i];
        ASSERT_FALSE(LocalOplogInfo::get(getOperationContext())->getTruncateMarkers())
            << "i=" << i << " delayMs=" << kIterationDelayMillis[i];
        ASSERT_FALSE(threadRaw->running()) << "i=" << i << " delayMs=" << kIterationDelayMillis[i];
    }
}


}  // namespace repl
}  // namespace mongo
