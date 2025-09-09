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

#include "mongo/db/curop.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/operation_context_options_gen.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

#include <initializer_list>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(CurOpTest, CopyConstructors) {
    OpDebug::AdditiveMetrics a, b;
    a.keysExamined = 1;
    b.keysExamined = 2;

    // Test copy constructor.
    OpDebug::AdditiveMetrics c = a;
    ASSERT_EQ(a.keysExamined, c.keysExamined);

    // Test copy assignment.
    a = b;
    ASSERT_EQ(a.keysExamined, b.keysExamined);
}

TEST(CurOpTest, AddingAdditiveMetricsObjectsTogetherShouldAddFieldsTogether) {
    OpDebug::AdditiveMetrics currentAdditiveMetrics = OpDebug::AdditiveMetrics();
    OpDebug::AdditiveMetrics additiveMetricsToAdd = OpDebug::AdditiveMetrics();

    // Initialize field values for both AdditiveMetrics objects.
    currentAdditiveMetrics.keysExamined = 0;
    additiveMetricsToAdd.keysExamined = 2;
    currentAdditiveMetrics.docsExamined = 4;
    additiveMetricsToAdd.docsExamined = 2;
    currentAdditiveMetrics.nMatched = 5;
    additiveMetricsToAdd.nMatched = 5;
    currentAdditiveMetrics.nreturned = 10;
    additiveMetricsToAdd.nreturned = 5;
    currentAdditiveMetrics.nBatches = 2;
    additiveMetricsToAdd.nBatches = 1;
    currentAdditiveMetrics.nModified = 3;
    additiveMetricsToAdd.nModified = 1;
    currentAdditiveMetrics.ninserted = 4;
    additiveMetricsToAdd.ninserted = 0;
    currentAdditiveMetrics.ndeleted = 3;
    additiveMetricsToAdd.ndeleted = 2;
    currentAdditiveMetrics.nUpserted = 7;
    additiveMetricsToAdd.nUpserted = 8;
    currentAdditiveMetrics.keysInserted = 6;
    additiveMetricsToAdd.keysInserted = 5;
    currentAdditiveMetrics.keysDeleted = 4;
    additiveMetricsToAdd.keysDeleted = 2;
    currentAdditiveMetrics.executionTime = Microseconds{200};
    additiveMetricsToAdd.executionTime = Microseconds{80};
    currentAdditiveMetrics.clusterWorkingTime = Milliseconds{30};
    additiveMetricsToAdd.clusterWorkingTime = Milliseconds{10};
    currentAdditiveMetrics.cpuNanos = Nanoseconds{1000};
    additiveMetricsToAdd.cpuNanos = Nanoseconds{21};
    currentAdditiveMetrics.delinquentAcquisitions = 1;
    additiveMetricsToAdd.delinquentAcquisitions = 2;
    currentAdditiveMetrics.totalAcquisitionDelinquency = Milliseconds{300};
    additiveMetricsToAdd.totalAcquisitionDelinquency = Milliseconds{200};
    currentAdditiveMetrics.maxAcquisitionDelinquency = Milliseconds{300};
    additiveMetricsToAdd.maxAcquisitionDelinquency = Milliseconds{100};
    currentAdditiveMetrics.numInterruptChecks = 1;
    additiveMetricsToAdd.numInterruptChecks = 2;
    currentAdditiveMetrics.overdueInterruptApproxMax = Milliseconds{100};
    additiveMetricsToAdd.overdueInterruptApproxMax = Milliseconds{300};

    // Save the current AdditiveMetrics object before adding.
    OpDebug::AdditiveMetrics additiveMetricsBeforeAdd;
    additiveMetricsBeforeAdd.add(currentAdditiveMetrics);
    currentAdditiveMetrics.add(additiveMetricsToAdd);

    // The following field values should have changed after adding.
    ASSERT_EQ(*currentAdditiveMetrics.keysExamined,
              *additiveMetricsBeforeAdd.keysExamined + *additiveMetricsToAdd.keysExamined);
    ASSERT_EQ(*currentAdditiveMetrics.docsExamined,
              *additiveMetricsBeforeAdd.docsExamined + *additiveMetricsToAdd.docsExamined);
    ASSERT_EQ(*currentAdditiveMetrics.nMatched,
              *additiveMetricsBeforeAdd.nMatched + *additiveMetricsToAdd.nMatched);
    ASSERT_EQ(*currentAdditiveMetrics.nreturned,
              *additiveMetricsBeforeAdd.nreturned + *additiveMetricsToAdd.nreturned);
    ASSERT_EQ(*currentAdditiveMetrics.nBatches,
              *additiveMetricsBeforeAdd.nBatches + *additiveMetricsToAdd.nBatches);
    ASSERT_EQ(*currentAdditiveMetrics.nModified,
              *additiveMetricsBeforeAdd.nModified + *additiveMetricsToAdd.nModified);
    ASSERT_EQ(*currentAdditiveMetrics.ninserted,
              *additiveMetricsBeforeAdd.ninserted + *additiveMetricsToAdd.ninserted);
    ASSERT_EQ(*currentAdditiveMetrics.ndeleted,
              *additiveMetricsBeforeAdd.ndeleted + *additiveMetricsToAdd.ndeleted);
    ASSERT_EQ(*currentAdditiveMetrics.nUpserted,
              *additiveMetricsBeforeAdd.nUpserted + *additiveMetricsToAdd.nUpserted);
    ASSERT_EQ(*currentAdditiveMetrics.keysInserted,
              *additiveMetricsBeforeAdd.keysInserted + *additiveMetricsToAdd.keysInserted);
    ASSERT_EQ(*currentAdditiveMetrics.keysDeleted,
              *additiveMetricsBeforeAdd.keysDeleted + *additiveMetricsToAdd.keysDeleted);
    ASSERT_EQ(*currentAdditiveMetrics.executionTime,
              *additiveMetricsBeforeAdd.executionTime + *additiveMetricsToAdd.executionTime);
    ASSERT_EQ(*currentAdditiveMetrics.clusterWorkingTime,
              *additiveMetricsBeforeAdd.clusterWorkingTime +
                  *additiveMetricsToAdd.clusterWorkingTime);
    ASSERT_EQ(*currentAdditiveMetrics.cpuNanos,
              *additiveMetricsBeforeAdd.cpuNanos + *additiveMetricsToAdd.cpuNanos);
    ASSERT_EQ(*currentAdditiveMetrics.delinquentAcquisitions,
              *additiveMetricsBeforeAdd.delinquentAcquisitions +
                  *additiveMetricsToAdd.delinquentAcquisitions);
    ASSERT_EQ(*currentAdditiveMetrics.totalAcquisitionDelinquency,
              *additiveMetricsBeforeAdd.totalAcquisitionDelinquency +
                  *additiveMetricsToAdd.totalAcquisitionDelinquency);
    ASSERT_EQ(*currentAdditiveMetrics.maxAcquisitionDelinquency,
              std::max(*additiveMetricsBeforeAdd.maxAcquisitionDelinquency,
                       *additiveMetricsToAdd.maxAcquisitionDelinquency));
    ASSERT_EQ(*currentAdditiveMetrics.numInterruptChecks,
              *additiveMetricsBeforeAdd.numInterruptChecks +
                  *additiveMetricsToAdd.numInterruptChecks);
    ASSERT_EQ(*currentAdditiveMetrics.overdueInterruptApproxMax,
              std::max(*additiveMetricsBeforeAdd.overdueInterruptApproxMax,
                       *additiveMetricsToAdd.overdueInterruptApproxMax));
}

TEST(CurOpTest, AddingUninitializedAdditiveMetricsFieldsShouldBeTreatedAsZero) {
    OpDebug::AdditiveMetrics currentAdditiveMetrics = OpDebug::AdditiveMetrics();
    OpDebug::AdditiveMetrics additiveMetricsToAdd = OpDebug::AdditiveMetrics();

    // Initialize field values for both AdditiveMetrics objects.
    additiveMetricsToAdd.keysExamined = 5;
    currentAdditiveMetrics.docsExamined = 4;
    currentAdditiveMetrics.nreturned = 2;
    additiveMetricsToAdd.nBatches = 1;
    currentAdditiveMetrics.nModified = 3;
    additiveMetricsToAdd.ninserted = 0;
    currentAdditiveMetrics.keysInserted = 6;
    additiveMetricsToAdd.keysInserted = 5;
    currentAdditiveMetrics.keysDeleted = 4;
    additiveMetricsToAdd.keysDeleted = 2;
    additiveMetricsToAdd.cpuNanos = Nanoseconds(1);
    additiveMetricsToAdd.delinquentAcquisitions = 1;
    additiveMetricsToAdd.totalAcquisitionDelinquency = Milliseconds(100);
    additiveMetricsToAdd.maxAcquisitionDelinquency = Milliseconds(100);
    additiveMetricsToAdd.numInterruptChecks = 1;
    additiveMetricsToAdd.overdueInterruptApproxMax = Milliseconds(100);

    // Save the current AdditiveMetrics object before adding.
    OpDebug::AdditiveMetrics additiveMetricsBeforeAdd;
    additiveMetricsBeforeAdd.add(currentAdditiveMetrics);
    currentAdditiveMetrics.add(additiveMetricsToAdd);

    // The 'keysExamined' field for the current AdditiveMetrics object was not initialized, so it
    // should be treated as zero.
    ASSERT_EQ(*currentAdditiveMetrics.keysExamined, *additiveMetricsToAdd.keysExamined);

    // The 'docsExamined' field for the AdditiveMetrics object to add was not initialized, so it
    // should be treated as zero.
    ASSERT_EQ(*currentAdditiveMetrics.docsExamined, *additiveMetricsBeforeAdd.docsExamined);

    // The 'nreturned' field for the AdditiveMetrics object to add was not initialized, so it
    // should be treated as zero.
    ASSERT_EQ(*currentAdditiveMetrics.nreturned, *additiveMetricsBeforeAdd.nreturned);

    // The 'nBatches' field for the current AdditiveMetrics object was not initialized, so it
    // should be treated as zero.
    ASSERT_EQ(*currentAdditiveMetrics.nBatches, *additiveMetricsToAdd.nBatches);

    // The 'nMatched' field for both the current AdditiveMetrics object and the AdditiveMetrics
    // object to add were not initialized, so nMatched should still be uninitialized after the add.
    ASSERT_EQ(currentAdditiveMetrics.nMatched, boost::none);

    // The 'nUpserted' field for both the current AdditiveMetrics object and the AdditiveMetrics
    // object to add were not initialized, so nUpserted should still be uninitialized after the add.
    ASSERT_EQ(currentAdditiveMetrics.nUpserted, boost::none);

    // The 'executionTime' field for both the current AdditiveMetrics object and the AdditiveMetrics
    // object to add were not initialized, so executionTime should still be uninitialized after the
    // add.
    ASSERT_EQ(currentAdditiveMetrics.executionTime, boost::none);

    // The following field values should have changed after adding.
    ASSERT_EQ(*currentAdditiveMetrics.keysInserted,
              *additiveMetricsBeforeAdd.keysInserted + *additiveMetricsToAdd.keysInserted);
    ASSERT_EQ(*currentAdditiveMetrics.keysDeleted,
              *additiveMetricsBeforeAdd.keysDeleted + *additiveMetricsToAdd.keysDeleted);

    // The 'cpuNanos' field for the current AdditiveMetrics object was not initialized, so it
    // should be treated as zero.
    ASSERT_EQ(*currentAdditiveMetrics.cpuNanos, *additiveMetricsToAdd.cpuNanos);

    // The delinquency fields for the current AdditiveMetrics object were not initialized, so they
    // should be treated as zero.
    ASSERT_EQ(*currentAdditiveMetrics.delinquentAcquisitions,
              *additiveMetricsToAdd.delinquentAcquisitions);
    ASSERT_EQ(*currentAdditiveMetrics.totalAcquisitionDelinquency,
              *additiveMetricsToAdd.totalAcquisitionDelinquency);
    ASSERT_EQ(*currentAdditiveMetrics.maxAcquisitionDelinquency,
              *additiveMetricsToAdd.maxAcquisitionDelinquency);
    ASSERT_EQ(*currentAdditiveMetrics.numInterruptChecks, *additiveMetricsToAdd.numInterruptChecks);
    ASSERT_EQ(*currentAdditiveMetrics.overdueInterruptApproxMax,
              *additiveMetricsToAdd.overdueInterruptApproxMax);
}

TEST(CurOpTest, AdditiveMetricsFieldsShouldIncrementByN) {
    OpDebug::AdditiveMetrics additiveMetrics = OpDebug::AdditiveMetrics();

    // Initialize field values.
    additiveMetrics.keysInserted = 2;
    additiveMetrics.nreturned = 3;

    // Increment the fields.
    additiveMetrics.incrementKeysInserted(5);
    additiveMetrics.incrementKeysDeleted(0);
    additiveMetrics.incrementNinserted(3);
    additiveMetrics.incrementNUpserted(6);
    additiveMetrics.incrementNreturned(2);
    additiveMetrics.incrementNBatches();

    ASSERT_EQ(*additiveMetrics.keysInserted, 7);
    ASSERT_EQ(*additiveMetrics.keysDeleted, 0);
    ASSERT_EQ(*additiveMetrics.ninserted, 3);
    ASSERT_EQ(*additiveMetrics.nUpserted, 6);
    ASSERT_EQ(*additiveMetrics.nreturned, 5);
    ASSERT_EQ(*additiveMetrics.nBatches, 1);
}

TEST(CurOpTest, AdditiveMetricsShouldAggregateCursorMetrics) {
    OpDebug::AdditiveMetrics additiveMetrics;

    additiveMetrics.keysExamined = 1;
    additiveMetrics.docsExamined = 2;
    additiveMetrics.clusterWorkingTime = Milliseconds(3);
    additiveMetrics.readingTime = Microseconds(4);
    additiveMetrics.bytesRead = 5;
    additiveMetrics.hasSortStage = false;
    additiveMetrics.usedDisk = false;
    additiveMetrics.cpuNanos = Nanoseconds(8);
    additiveMetrics.delinquentAcquisitions = 2;
    additiveMetrics.totalAcquisitionDelinquency = Milliseconds(400);
    additiveMetrics.maxAcquisitionDelinquency = Milliseconds(300);
    additiveMetrics.numInterruptChecks = 2;
    additiveMetrics.overdueInterruptApproxMax = Milliseconds(100);

    CursorMetrics cursorMetrics(3 /* keysExamined */,
                                4 /* docsExamined */,
                                10 /* bytesRead */,
                                11 /* readingTimeMicros */,
                                5 /* workingTimeMillis */,
                                true /* hasSortStage */,
                                false /* usedDisk */,
                                true /* fromMultiPlanner */,
                                false /* fromPlanCache */,
                                9 /* cpuNanos */,
                                3 /* numInterruptChecks */);
    cursorMetrics.setDelinquentAcquisitions(3);
    cursorMetrics.setTotalAcquisitionDelinquencyMillis(400);
    cursorMetrics.setMaxAcquisitionDelinquencyMillis(200);
    cursorMetrics.setOverdueInterruptApproxMaxMillis(200);

    additiveMetrics.aggregateCursorMetrics(cursorMetrics);

    ASSERT_EQ(*additiveMetrics.keysExamined, 4);
    ASSERT_EQ(*additiveMetrics.docsExamined, 6);
    ASSERT_EQ(additiveMetrics.clusterWorkingTime, Milliseconds(8));
    ASSERT_EQ(additiveMetrics.readingTime, Microseconds(15));
    ASSERT_EQ(*additiveMetrics.bytesRead, 15);
    ASSERT_EQ(additiveMetrics.hasSortStage, true);
    ASSERT_EQ(additiveMetrics.usedDisk, false);
    ASSERT_EQ(additiveMetrics.cpuNanos, Nanoseconds(17));
    ASSERT_EQ(*additiveMetrics.delinquentAcquisitions, 5);
    ASSERT_EQ(*additiveMetrics.totalAcquisitionDelinquency, Milliseconds(800));
    ASSERT_EQ(*additiveMetrics.maxAcquisitionDelinquency, Milliseconds(300));
    ASSERT_EQ(*additiveMetrics.numInterruptChecks, 5);
    ASSERT_EQ(*additiveMetrics.overdueInterruptApproxMax, Milliseconds(200));
}

TEST(CurOpTest, AdditiveMetricsShouldAggregateNegativeCpuNanos) {
    // CPU time can be negative -1 if the platform doesn't support collecting cpu time.
    OpDebug::AdditiveMetrics additiveMetrics;

    additiveMetrics.cpuNanos = Nanoseconds(-1);

    CursorMetrics cursorMetrics(1 /* keysExamined */,
                                2 /* docsExamined */,
                                3 /* bytesRead */,
                                10 /* workingTimeMillis */,
                                11 /* readingTimeMicros */,
                                true /* hasSortStage */,
                                false /* usedDisk */,
                                true /* fromMultiPlanner */,
                                false /* fromPlanCache */,
                                -1 /* cpuNanos */,
                                3 /* numInterruptChecks */);

    additiveMetrics.aggregateCursorMetrics(cursorMetrics);
    ASSERT_EQ(additiveMetrics.cpuNanos, Nanoseconds(-2));
}

TEST(CurOpTest, AdditiveMetricsAggregateCursorMetricsTreatsNoneAsZero) {
    OpDebug::AdditiveMetrics additiveMetrics;

    additiveMetrics.keysExamined = boost::none;
    additiveMetrics.docsExamined = boost::none;
    additiveMetrics.bytesRead = boost::none;

    CursorMetrics cursorMetrics(1 /* keysExamined */,
                                2 /* docsExamined */,
                                3 /* bytesRead */,
                                10 /* workingTimeMillis */,
                                11 /* readingTimeMicros */,
                                true /* hasSortStage */,
                                false /* usedDisk */,
                                true /* fromMultiPlanner */,
                                false /* fromPlanCache */,
                                10 /* cpuNanos */,
                                3 /* numInterruptChecks */);

    additiveMetrics.aggregateCursorMetrics(cursorMetrics);

    ASSERT_EQ(*additiveMetrics.keysExamined, 1);
    ASSERT_EQ(*additiveMetrics.docsExamined, 2);
    ASSERT_EQ(*additiveMetrics.bytesRead, 3);
}

TEST(CurOpTest, AdditiveMetricsShouldAggregateDataBearingNodeMetrics) {
    OpDebug::AdditiveMetrics additiveMetrics;

    additiveMetrics.keysExamined = 1;
    additiveMetrics.docsExamined = 2;
    additiveMetrics.clusterWorkingTime = Milliseconds(3);
    additiveMetrics.hasSortStage = false;
    additiveMetrics.usedDisk = false;
    additiveMetrics.cpuNanos = Nanoseconds(5);
    additiveMetrics.delinquentAcquisitions = 2;
    additiveMetrics.totalAcquisitionDelinquency = Milliseconds(400);
    additiveMetrics.maxAcquisitionDelinquency = Milliseconds(200);
    additiveMetrics.numInterruptChecks = 2;
    additiveMetrics.overdueInterruptApproxMax = Milliseconds(100);

    query_stats::DataBearingNodeMetrics remoteMetrics;
    remoteMetrics.keysExamined = 3;
    remoteMetrics.docsExamined = 4;
    remoteMetrics.clusterWorkingTime = Milliseconds(5);
    remoteMetrics.hasSortStage = true;
    remoteMetrics.usedDisk = false;
    remoteMetrics.cpuNanos = Nanoseconds(6);
    remoteMetrics.delinquentAcquisitions = 1;
    remoteMetrics.totalAcquisitionDelinquency = Milliseconds(300);
    remoteMetrics.maxAcquisitionDelinquency = Milliseconds(300);
    remoteMetrics.numInterruptChecks = 1;
    remoteMetrics.overdueInterruptApproxMax = Milliseconds(300);

    additiveMetrics.aggregateDataBearingNodeMetrics(remoteMetrics);

    ASSERT_EQ(*additiveMetrics.keysExamined, 4);
    ASSERT_EQ(*additiveMetrics.docsExamined, 6);
    ASSERT_EQ(additiveMetrics.clusterWorkingTime, Milliseconds(8));
    ASSERT_EQ(additiveMetrics.hasSortStage, true);
    ASSERT_EQ(additiveMetrics.usedDisk, false);
    ASSERT_EQ(additiveMetrics.cpuNanos, Nanoseconds(11));
    ASSERT_EQ(*additiveMetrics.delinquentAcquisitions, 3);
    ASSERT_EQ(*additiveMetrics.totalAcquisitionDelinquency, Milliseconds(700));
    ASSERT_EQ(*additiveMetrics.maxAcquisitionDelinquency, Milliseconds(300));
    ASSERT_EQ(*additiveMetrics.numInterruptChecks, 3);
    ASSERT_EQ(*additiveMetrics.overdueInterruptApproxMax, Milliseconds(300));
}

TEST(CurOpTest, AdditiveMetricsAggregateDataBearingNodeMetricsTreatsNoneAsZero) {
    OpDebug::AdditiveMetrics additiveMetrics;

    additiveMetrics.keysExamined = boost::none;
    additiveMetrics.docsExamined = boost::none;

    query_stats::DataBearingNodeMetrics remoteMetrics;
    remoteMetrics.keysExamined = 1;
    remoteMetrics.docsExamined = 2;

    additiveMetrics.aggregateDataBearingNodeMetrics(remoteMetrics);

    ASSERT_EQ(*additiveMetrics.keysExamined, 1);
    ASSERT_EQ(*additiveMetrics.docsExamined, 2);
}

TEST(CurOpTest, AdditiveMetricsShouldAggregateStorageStats) {
    class StorageStatsForTest final : public StorageStats {
        uint64_t _bytesRead;
        Microseconds _readingTime;

    public:
        StorageStatsForTest(uint64_t bytesRead, Microseconds readingTime)
            : _bytesRead(bytesRead), _readingTime(readingTime) {}
        BSONObj toBSON() const final {
            return {};
        }
        uint64_t bytesRead() const final {
            return _bytesRead;
        }
        Microseconds readingTime() const final {
            return _readingTime;
        }
        std::unique_ptr<StorageStats> clone() const final {
            return nullptr;
        }
        StorageStats& operator+=(const StorageStats&) final {
            return *this;
        }
        StorageStats& operator-=(const StorageStats&) final {
            return *this;
        }
    };

    OpDebug::AdditiveMetrics additiveMetrics;

    additiveMetrics.bytesRead = 2;
    additiveMetrics.readingTime = Microseconds(3);

    StorageStatsForTest storageStats{5 /* bytesRead */, Microseconds(7) /* readingTime */};

    additiveMetrics.aggregateStorageStats(storageStats);

    ASSERT_EQ(*additiveMetrics.bytesRead, 7);
    ASSERT_EQ(*additiveMetrics.readingTime, Microseconds(10));
}

TEST(CurOpTest, OptionalAdditiveMetricsNotDisplayedIfUninitialized) {
    // 'basicFields' should always be present in the logs and profiler, for any operation.
    std::vector<std::string> basicFields{
        "op", "ns", "command", "numYield", "locks", "millis", "flowControl"};

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    SingleThreadedLockStats ls;

    auto curop = CurOp::get(*opCtx);
    const OpDebug& od = curop->debug();

    // Create dummy command.
    BSONObj command = BSON("a" << 3);

    // Set dummy 'ns' and 'command'.
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        curop->setGenericOpRequestDetails(
            clientLock,
            NamespaceString::createNamespaceString_forTest("myDb.coll"),
            nullptr,
            command,
            NetworkOp::dbQuery);
    }

    BSONObjBuilder builder;
    od.append(opCtx.get(), ls, {}, {}, 0, false /*omitCommand*/, builder);
    auto bs = builder.done();

    // Append should always include these basic fields.
    for (const std::string& field : basicFields) {
        ASSERT_TRUE(bs.hasField(field));
    }

    // Append should include only the basic fields when just initialized.
    ASSERT_EQ(static_cast<size_t>(bs.nFields()), basicFields.size());
}

TEST(CurOpTest, ShouldUpdateMemoryStats) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               true);

    ASSERT_EQ(0, curop->getInUseTrackedMemoryBytes());
    ASSERT_EQ(0, curop->getPeakTrackedMemoryBytes());

    curop->setMemoryTrackingStats(10 /*inUseTrackedMemoryBytes*/, 15 /*peakTrackedMemoryBytes*/);
    ASSERT_EQ(10, curop->getInUseTrackedMemoryBytes());
    ASSERT_EQ(15, curop->getPeakTrackedMemoryBytes());

    // The max memory usage is updated if the new max is greater than the current max.
    curop->setMemoryTrackingStats(21 /*inUseTrackedMemoryBytes*/, 20 /*peakTrackedMemoryBytes*/);
    ASSERT_EQ(21, curop->getInUseTrackedMemoryBytes());
    ASSERT_EQ(20, curop->getPeakTrackedMemoryBytes());

    // The max memory usage is not updated if the new max is not greater than the current max.
    curop->setMemoryTrackingStats(31 /*inUseTrackedMemoryBytes*/, 15 /*peakTrackedMemoryBytes*/);
    ASSERT_EQ(31, curop->getInUseTrackedMemoryBytes());
    ASSERT_EQ(20, curop->getPeakTrackedMemoryBytes());
}

DEATH_TEST(CurOpTest, RequireFeatureFlagEnabledToUpdateMemoryStats, "tassert") {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               false);

    ASSERT_EQ(0, curop->getInUseTrackedMemoryBytes());
    ASSERT_EQ(0, curop->getPeakTrackedMemoryBytes());
    curop->setMemoryTrackingStats(10 /*inUseTrackedMemoryBytes*/, 15 /*peakTrackedMemoryBytes*/);
}

/**
 * When featureFlagQueryMemoryTracking is enabled, non-zero memory tracking stats should appear in
 * the profiler.
 */
TEST(CurOpTest, MemoryStatsDisplayedIfNonZero) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               true);

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto curop = CurOp::get(*opCtx);
    const OpDebug& opDebug = curop->debug();
    SingleThreadedLockStats ls;

    BSONObjBuilder bob;
    opDebug.append(opCtx.get(), ls, {}, {}, 0, true /*omitCommand*/, bob);

    // If the memory tracker has not updated CurOp, the memory tracking stat should not appear in
    // the profiler output.
    auto res = bob.done();
    ASSERT_EQ(0, curop->getPeakTrackedMemoryBytes());
    ASSERT_FALSE(res.hasField("peakTrackedMemBytes"));

    curop->setMemoryTrackingStats(10 /*inUseTrackedMemoryBytes*/, 15 /*peakTrackedMemoryBytes*/);
    BSONObjBuilder bobWithMemStats;
    opDebug.append(opCtx.get(), ls, {}, {}, 0, true /*omitCommand*/, bobWithMemStats);
    res = bobWithMemStats.done();

    ASSERT_EQ(15, curop->getPeakTrackedMemoryBytes());
    ASSERT_EQ(15, res.getIntField("peakTrackedMemBytes"));
}

TEST(CurOpTest, ReportStateIncludesMemoryStatsIfNonZero) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               true);
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto curOp = CurOp::get(*opCtx);

    // If the memory stats are zero, they are *not* included in the state.
    {
        BSONObjBuilder bob;
        curOp->reportState(&bob, SerializationContext{});
        BSONObj state = bob.obj();
        ASSERT_FALSE(state.hasField("inUseTrackedMemBytes"));
        ASSERT_FALSE(state.hasField("peakTrackedMemBytes"));
    }

    // If the memory stats are not zero, they *are* included in the state.
    {
        BSONObjBuilder bob;
        curOp->setMemoryTrackingStats(128, 256);
        curOp->reportState(&bob, SerializationContext{});
        BSONObj state = bob.obj();
        ASSERT_TRUE(state.hasField("inUseTrackedMemBytes"));
        ASSERT_EQ(state["inUseTrackedMemBytes"].Long(), 128);
        ASSERT_TRUE(state.hasField("peakTrackedMemBytes"));
        ASSERT_EQ(state["peakTrackedMemBytes"].Long(), 256);
    }
}

TEST(CurOpTest, ReportStateIncludesDelinquentStatsIfNonZero) {
    RAIIServerParameterControllerForTest enableDelinquentTracking(
        "featureFlagRecordDelinquentMetrics", true);
    RAIIServerParameterControllerForTest alwaysTrackInterrupts("overdueInterruptCheckSamplingRate",
                                                               1);

    QueryTestServiceContext serviceContext;
    auto tickSourcePtr = dynamic_cast<TickSourceMock<Nanoseconds>*>(
        serviceContext.getServiceContext()->getTickSource());
    tickSourcePtr->advance(Milliseconds{100});

    auto opCtx = serviceContext.makeOperationContext();
    auto curOp = CurOp::get(*opCtx);
    curOp->setTickSource_forTest(tickSourcePtr);
    curOp->ensureStarted();

    // If the delinquent stats are zero, they are *not* included in the state.
    {
        BSONObjBuilder bob;
        curOp->reportState(&bob, SerializationContext{});
        BSONObj state = bob.obj();
        ASSERT_FALSE(state.hasField("delinquencyInfo"));
        // Field numInterruptChecks should be always shown.
        ASSERT_TRUE(state.hasField("numInterruptChecks"));
    }

    // If the delinquent stats are not zero, they *are* included in the state.
    {
        ExecutionAdmissionContext::get(opCtx.get())
            .recordDelinquentReadAcquisition(Milliseconds(20));
        ExecutionAdmissionContext::get(opCtx.get())
            .recordDelinquentReadAcquisition(Milliseconds(10));
        BSONObjBuilder bob;
        curOp->reportState(&bob, SerializationContext{});
        BSONObj state = bob.obj();
        ASSERT_TRUE(state.hasField("delinquencyInfo"));
        ASSERT_EQ(state["delinquencyInfo"]["totalDelinquentAcquisitions"].Long(), 2);
        ASSERT_EQ(state["delinquencyInfo"]["totalAcquisitionDelinquencyMillis"].Long(), 30);
        ASSERT_EQ(state["delinquencyInfo"]["maxAcquisitionDelinquencyMillis"].Long(), 20);
    }

    {
        tickSourcePtr->advance(Milliseconds{200});
        opCtx->checkForInterrupt();
        BSONObjBuilder bob;
        curOp->reportState(&bob, SerializationContext{});
        BSONObj state = bob.obj();

        const Milliseconds interval{gOverdueInterruptCheckIntervalMillis.load()};

        ASSERT_TRUE(state.hasField("numInterruptChecks")) << state.toString();
        ASSERT_EQ(state["numInterruptChecks"].Number(), 1);

        ASSERT_TRUE(state.hasField("delinquencyInfo"));
        ASSERT_EQ(state["delinquencyInfo"]["overdueInterruptChecks"].Number(), 1);
        ASSERT_EQ(state["delinquencyInfo"]["overdueInterruptTotalMillis"].Number(),
                  200 - interval.count());
        ASSERT_EQ(state["delinquencyInfo"]["overdueInterruptApproxMaxMillis"].Number(),
                  200 - interval.count());
    }
}

TEST(CurOpTest, ShouldNotReportFailpointMsgIfNotSet) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto curop = CurOp::get(*opCtx);

    // Test the reported state should _not_ contain 'failpointMsg'.
    BSONObjBuilder reportedStateWithoutFailpointMsg;
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curop->reportState(&reportedStateWithoutFailpointMsg, SerializationContext());
    }
    auto bsonObj = reportedStateWithoutFailpointMsg.done();

    // bsonObj should _not_ contain 'failpointMsg' if a fail point is not set.
    ASSERT_FALSE(bsonObj.hasField("failpointMsg"));
}

TEST(CurOpTest, ShouldReportIsFromUserConnection) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto client = serviceContext.getClient();

    // Mock a client with a user connection.
    transport::TransportLayerMock transportLayer;
    auto clientUserConn = serviceContext.getServiceContext()->getService()->makeClient(
        "userconn", transportLayer.createSession());

    auto curop = CurOp::get(*opCtx);

    BSONObjBuilder curOpObj;
    BSONObjBuilder curOpObjUserConn;
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");

        // Serialization Context on expression context should be non-empty in
        // reportCurrentOpForClient.
        auto sc = SerializationContext(SerializationContext::Source::Command,
                                       SerializationContext::CallerType::Reply,
                                       SerializationContext::Prefix::ExcludePrefix);
        auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), nss, sc);

        curop->reportCurrentOpForClient(expCtx, client, false, &curOpObj);
        curop->reportCurrentOpForClient(expCtx, clientUserConn.get(), false, &curOpObjUserConn);
    }
    auto bsonObj = curOpObj.done();
    auto bsonObjUserConn = curOpObjUserConn.done();

    ASSERT_TRUE(bsonObj.hasField("isFromUserConnection"));
    ASSERT_TRUE(bsonObjUserConn.hasField("isFromUserConnection"));
    ASSERT_FALSE(bsonObj.getField("isFromUserConnection").Bool());
    ASSERT_TRUE(bsonObjUserConn.getField("isFromUserConnection").Bool());
}

TEST(CurOpTest, ElapsedTimeReflectsTickSource) {
    QueryTestServiceContext serviceContext;

    auto tickSourceMock = std::make_unique<TickSourceMock<Microseconds>>();
    // The tick source is initialized to a non-zero value as CurOp equates a value of 0 with a
    // not-started timer.
    tickSourceMock->advance(Milliseconds{100});

    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);
    curop->setTickSource_forTest(tickSourceMock.get());

    ASSERT_FALSE(curop->isStarted());

    curop->ensureStarted();
    ASSERT_TRUE(curop->isStarted());

    tickSourceMock->advance(Milliseconds{20});

    ASSERT_FALSE(curop->isDone());

    curop->done();
    ASSERT_TRUE(curop->isDone());

    ASSERT_EQ(Milliseconds{20}, duration_cast<Milliseconds>(curop->elapsedTimeTotal()));
}

TEST(CurOpTest, CheckNSAgainstSerializationContext) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tid = TenantId(OID::gen());

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto curop = CurOp::get(*opCtx);

    // Create dummy command.
    BSONObj command = BSON("a" << 3);

    // Set dummy 'ns' and 'command'.
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        curop->setGenericOpRequestDetails(
            clientLock,
            NamespaceString::createNamespaceString_forTest(tid, "testDb.coll"),
            nullptr,
            command,
            NetworkOp::dbQuery);
    }

    // Test expectPrefix field.
    for (bool expectPrefix : {false, true}) {
        SerializationContext sc = SerializationContext::stateCommandReply();
        sc.setPrefixState(expectPrefix);

        BSONObjBuilder builder;
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curop->reportState(&builder, sc);
        }
        auto bsonObj = builder.done();

        std::string serializedNs = expectPrefix ? tid.toString() + "_testDb.coll" : "testDb.coll";
        ASSERT_EQ(serializedNs, bsonObj.getField("ns").String());
    }
}

TEST(CurOpTest, GetCursorMetricsProducesValidObject) {
    // This test just checks that the cursor metrics object produced by getCursorMetrics
    // is a valid, serializable object. In particular, it must have all required fields.
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);
    auto metrics = curop->debug().getCursorMetrics();
    ASSERT_DOES_NOT_THROW(metrics.toBSON());
}

TEST(CurOpTest, KilledOperationReportsLatency) {
    QueryTestServiceContext serviceContext(std::make_unique<TickSourceMock<Nanoseconds>>());
    auto opCtx = serviceContext.makeOperationContext();
    auto tickSourcePtr = dynamic_cast<TickSourceMock<Nanoseconds>*>(
        serviceContext.getServiceContext()->getTickSource());

    tickSourcePtr->advance(Nanoseconds(3));
    const int killLatency = 32;

    opCtx->markKilled();
    ASSERT_FALSE(opCtx->checkForInterruptNoAssert().isOK());
    tickSourcePtr->advance(Nanoseconds(killLatency));

    auto curop = CurOp::get(*opCtx);
    const OpDebug& opDebug = curop->debug();
    SingleThreadedLockStats ls;

    BSONObjBuilder bob;
    opDebug.append(opCtx.get(), ls, {}, {}, 0, true /*omitCommand*/, bob);

    auto res = bob.done();
    ASSERT_TRUE(res.hasField("interruptLatencyNanos")) << res.toString();
    ASSERT_EQ(killLatency, res.getIntField("interruptLatencyNanos"));
}

TEST(CurOpTest, SlowLogFinishesWithDuration) {
    // Best effort test to try and verify that durationMillis is the last field reported by
    // report(). This doesn't populate every possible fields but makes some attempt to ensure
    // that there are a few.

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto curop = CurOp::get(*opCtx);

    // Create dummy command.
    BSONObj command = BSON("a" << 3);
    TenantId tid = TenantId(OID::gen());

    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        curop->setGenericOpRequestDetails(
            clientLock,
            NamespaceString::createNamespaceString_forTest(tid, "testDb.coll"),
            nullptr,
            command,
            NetworkOp::dbQuery);
    }

    const OpDebug& opDebug = curop->debug();
    SingleThreadedLockStats lockStats;

    curop->ensureStarted();
    curop->done();
    curop->calculateCpuTime();

    auto pattrs = std::make_unique<logv2::DynamicAttributes>();
    opDebug.report(opCtx.get(), &lockStats, {}, 0, pattrs.get());

    logv2::TypeErasedAttributeStorage attrs{*pattrs};
    ASSERT_GTE(attrs.size(), 1);
    std::string lastName = (attrs.end() - 1)->name;
    ASSERT_EQ("durationMillis", lastName);
}

}  // namespace
}  // namespace mongo
