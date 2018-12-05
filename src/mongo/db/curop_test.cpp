
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

#include <boost/optional/optional_io.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

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
    currentAdditiveMetrics.nModified = 3;
    additiveMetricsToAdd.nModified = 1;
    currentAdditiveMetrics.ninserted = 4;
    additiveMetricsToAdd.ninserted = 0;
    currentAdditiveMetrics.ndeleted = 3;
    additiveMetricsToAdd.ndeleted = 2;
    currentAdditiveMetrics.nmoved = 0;
    additiveMetricsToAdd.nmoved = 4;
    currentAdditiveMetrics.keysInserted = 6;
    additiveMetricsToAdd.keysInserted = 5;
    currentAdditiveMetrics.keysDeleted = 4;
    additiveMetricsToAdd.keysDeleted = 2;
    currentAdditiveMetrics.prepareReadConflicts = 1;
    additiveMetricsToAdd.prepareReadConflicts = 5;
    currentAdditiveMetrics.writeConflicts = 7;
    additiveMetricsToAdd.writeConflicts = 0;

    // Save the current AdditiveMetrics object before adding.
    OpDebug::AdditiveMetrics additiveMetricsBeforeAdd = currentAdditiveMetrics;
    currentAdditiveMetrics.add(additiveMetricsToAdd);

    // The following field values should have changed after adding.
    ASSERT_EQ(*currentAdditiveMetrics.keysExamined,
              *additiveMetricsBeforeAdd.keysExamined + *additiveMetricsToAdd.keysExamined);
    ASSERT_EQ(*currentAdditiveMetrics.docsExamined,
              *additiveMetricsBeforeAdd.docsExamined + *additiveMetricsToAdd.docsExamined);
    ASSERT_EQ(*currentAdditiveMetrics.nMatched,
              *additiveMetricsBeforeAdd.nMatched + *additiveMetricsToAdd.nMatched);
    ASSERT_EQ(*currentAdditiveMetrics.nModified,
              *additiveMetricsBeforeAdd.nModified + *additiveMetricsToAdd.nModified);
    ASSERT_EQ(*currentAdditiveMetrics.ninserted,
              *additiveMetricsBeforeAdd.ninserted + *additiveMetricsToAdd.ninserted);
    ASSERT_EQ(*currentAdditiveMetrics.ndeleted,
              *additiveMetricsBeforeAdd.ndeleted + *additiveMetricsToAdd.ndeleted);
    ASSERT_EQ(*currentAdditiveMetrics.nmoved,
              *additiveMetricsBeforeAdd.nmoved + *additiveMetricsToAdd.nmoved);
    ASSERT_EQ(*currentAdditiveMetrics.keysInserted,
              *additiveMetricsBeforeAdd.keysInserted + *additiveMetricsToAdd.keysInserted);
    ASSERT_EQ(*currentAdditiveMetrics.keysDeleted,
              *additiveMetricsBeforeAdd.keysDeleted + *additiveMetricsToAdd.keysDeleted);
    ASSERT_EQ(*currentAdditiveMetrics.prepareReadConflicts,
              *additiveMetricsBeforeAdd.prepareReadConflicts +
                  *additiveMetricsToAdd.prepareReadConflicts);
    ASSERT_EQ(*currentAdditiveMetrics.writeConflicts,
              *additiveMetricsBeforeAdd.writeConflicts + *additiveMetricsToAdd.writeConflicts);
}

TEST(CurOpTest, AddingUninitializedAdditiveMetricsFieldsShouldBeTreatedAsZero) {
    OpDebug::AdditiveMetrics currentAdditiveMetrics = OpDebug::AdditiveMetrics();
    OpDebug::AdditiveMetrics additiveMetricsToAdd = OpDebug::AdditiveMetrics();

    // Initialize field values for both AdditiveMetrics objects.
    additiveMetricsToAdd.keysExamined = 5;
    currentAdditiveMetrics.docsExamined = 4;
    currentAdditiveMetrics.nModified = 3;
    additiveMetricsToAdd.ninserted = 0;
    currentAdditiveMetrics.nmoved = 0;
    additiveMetricsToAdd.nmoved = 4;
    currentAdditiveMetrics.keysInserted = 6;
    additiveMetricsToAdd.keysInserted = 5;
    currentAdditiveMetrics.keysDeleted = 4;
    additiveMetricsToAdd.keysDeleted = 2;
    currentAdditiveMetrics.prepareReadConflicts = 1;
    additiveMetricsToAdd.prepareReadConflicts = 5;
    currentAdditiveMetrics.writeConflicts = 7;
    additiveMetricsToAdd.writeConflicts = 0;

    // Save the current AdditiveMetrics object before adding.
    OpDebug::AdditiveMetrics additiveMetricsBeforeAdd = currentAdditiveMetrics;
    currentAdditiveMetrics.add(additiveMetricsToAdd);

    // The 'keysExamined' field for the current AdditiveMetrics object was not initialized, so it
    // should be treated as zero.
    ASSERT_EQ(*currentAdditiveMetrics.keysExamined, *additiveMetricsToAdd.keysExamined);

    // The 'docsExamined' field for the AdditiveMetrics object to add was not initialized, so it
    // should be treated as zero.
    ASSERT_EQ(*currentAdditiveMetrics.docsExamined, *additiveMetricsBeforeAdd.docsExamined);

    // The 'nMatched' field for both the current AdditiveMetrics object and the AdditiveMetrics
    // object to add were not initialized, so nMatched should still be uninitialized after the add.
    ASSERT_EQ(currentAdditiveMetrics.nMatched, boost::none);

    // The following field values should have changed after adding.
    ASSERT_EQ(*currentAdditiveMetrics.nmoved,
              *additiveMetricsBeforeAdd.nmoved + *additiveMetricsToAdd.nmoved);
    ASSERT_EQ(*currentAdditiveMetrics.keysInserted,
              *additiveMetricsBeforeAdd.keysInserted + *additiveMetricsToAdd.keysInserted);
    ASSERT_EQ(*currentAdditiveMetrics.keysDeleted,
              *additiveMetricsBeforeAdd.keysDeleted + *additiveMetricsToAdd.keysDeleted);
    ASSERT_EQ(*currentAdditiveMetrics.prepareReadConflicts,
              *additiveMetricsBeforeAdd.prepareReadConflicts +
                  *additiveMetricsToAdd.prepareReadConflicts);
    ASSERT_EQ(*currentAdditiveMetrics.writeConflicts,
              *additiveMetricsBeforeAdd.writeConflicts + *additiveMetricsToAdd.writeConflicts);
}

TEST(CurOpTest, AdditiveMetricsFieldsShouldIncrementByN) {
    OpDebug::AdditiveMetrics additiveMetrics = OpDebug::AdditiveMetrics();

    // Initialize field values.
    additiveMetrics.writeConflicts = 1;
    additiveMetrics.keysInserted = 2;
    additiveMetrics.prepareReadConflicts = 6;

    // Increment the fields.
    additiveMetrics.incrementWriteConflicts(1);
    additiveMetrics.incrementKeysInserted(5);
    additiveMetrics.incrementKeysDeleted(0);
    additiveMetrics.incrementNmoved(1);
    additiveMetrics.incrementNinserted(3);
    additiveMetrics.incrementPrepareReadConflicts(2);

    ASSERT_EQ(*additiveMetrics.writeConflicts, 2);
    ASSERT_EQ(*additiveMetrics.keysInserted, 7);
    ASSERT_EQ(*additiveMetrics.keysDeleted, 0);
    ASSERT_EQ(*additiveMetrics.nmoved, 1);
    ASSERT_EQ(*additiveMetrics.ninserted, 3);
    ASSERT_EQ(*additiveMetrics.prepareReadConflicts, 8);
}

TEST(CurOpTest, OptionalAdditiveMetricsNotDisplayedIfUninitialized) {
    // 'basicFields' should always be present in the logs and profiler, for any operation.
    std::vector<std::string> basicFields{"op", "ns", "command", "numYield", "locks", "millis"};

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    SingleThreadedLockStats ls;

    auto curop = CurOp::get(*opCtx);
    const OpDebug& od = curop->debug();

    // Create dummy command.
    BSONObj command = BSON("a" << 3);

    // Set dummy 'ns' and 'command'.
    curop->setGenericOpRequestDetails(
        opCtx.get(), NamespaceString("myDb.coll"), nullptr, command, NetworkOp::dbQuery);

    BSONObjBuilder builder;
    od.append(*curop, ls, builder);
    auto bs = builder.done();

    // Append should always include these basic fields.
    for (const std::string& field : basicFields) {
        ASSERT_TRUE(bs.hasField(field));
    }

    // Append should include only the basic fields when just initialized.
    ASSERT_EQ(static_cast<size_t>(bs.nFields()), basicFields.size());

    // 'reportString' should only contain basic fields.
    std::string reportString = od.report(serviceContext.getClient(), *curop, nullptr);
    std::string expectedReportString = "query myDb.coll command: { a: 3 } numYields:0 0ms";

    ASSERT_EQ(reportString, expectedReportString);
}
}  // namespace
}  // namespace mongo
