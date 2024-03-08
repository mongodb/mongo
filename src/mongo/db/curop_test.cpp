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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <initializer_list>
#include <mutex>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/curop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

TEST(CurOpTest, CopyConstructors) {
    OpDebug::AdditiveMetrics a, b;
    a.keysExamined = 1;
    b.keysExamined = 2;
    a.prepareReadConflicts.store(1);
    b.prepareReadConflicts.store(2);
    // Test copy constructor.
    OpDebug::AdditiveMetrics c = a;
    ASSERT_EQ(a.keysExamined, c.keysExamined);
    ASSERT_EQ(a.prepareReadConflicts.load(), c.prepareReadConflicts.load());
    // Test copy assignment.
    a = b;
    ASSERT_EQ(a.keysExamined, b.keysExamined);
    ASSERT_EQ(a.prepareReadConflicts.load(), b.prepareReadConflicts.load());
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
    currentAdditiveMetrics.prepareReadConflicts.store(1);
    additiveMetricsToAdd.prepareReadConflicts.store(5);
    currentAdditiveMetrics.writeConflicts.store(7);
    additiveMetricsToAdd.writeConflicts.store(0);

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
    ASSERT_EQ(currentAdditiveMetrics.prepareReadConflicts.load(),
              additiveMetricsBeforeAdd.prepareReadConflicts.load() +
                  additiveMetricsToAdd.prepareReadConflicts.load());
    ASSERT_EQ(currentAdditiveMetrics.writeConflicts.load(),
              additiveMetricsBeforeAdd.writeConflicts.load() +
                  additiveMetricsToAdd.writeConflicts.load());
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
    currentAdditiveMetrics.prepareReadConflicts.store(1);
    additiveMetricsToAdd.prepareReadConflicts.store(5);
    currentAdditiveMetrics.writeConflicts.store(7);
    additiveMetricsToAdd.writeConflicts.store(0);

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
    ASSERT_EQ(currentAdditiveMetrics.prepareReadConflicts.load(),
              additiveMetricsBeforeAdd.prepareReadConflicts.load() +
                  additiveMetricsToAdd.prepareReadConflicts.load());
    ASSERT_EQ(currentAdditiveMetrics.writeConflicts.load(),
              additiveMetricsBeforeAdd.writeConflicts.load() +
                  additiveMetricsToAdd.writeConflicts.load());
}

TEST(CurOpTest, AdditiveMetricsFieldsShouldIncrementByN) {
    OpDebug::AdditiveMetrics additiveMetrics = OpDebug::AdditiveMetrics();

    // Initialize field values.
    additiveMetrics.writeConflicts.store(1);
    additiveMetrics.keysInserted = 2;
    additiveMetrics.prepareReadConflicts.store(6);
    additiveMetrics.nreturned = 3;
    additiveMetrics.executionTime = Microseconds{160};

    // Increment the fields.
    additiveMetrics.incrementWriteConflicts(1);
    additiveMetrics.incrementKeysInserted(5);
    additiveMetrics.incrementKeysDeleted(0);
    additiveMetrics.incrementNinserted(3);
    additiveMetrics.incrementNUpserted(6);
    additiveMetrics.incrementPrepareReadConflicts(2);
    additiveMetrics.incrementNreturned(2);
    additiveMetrics.incrementNBatches();
    additiveMetrics.incrementExecutionTime(Microseconds{120});

    ASSERT_EQ(additiveMetrics.writeConflicts.load(), 2);
    ASSERT_EQ(*additiveMetrics.keysInserted, 7);
    ASSERT_EQ(*additiveMetrics.keysDeleted, 0);
    ASSERT_EQ(*additiveMetrics.ninserted, 3);
    ASSERT_EQ(*additiveMetrics.nUpserted, 6);
    ASSERT_EQ(additiveMetrics.prepareReadConflicts.load(), 8);
    ASSERT_EQ(*additiveMetrics.nreturned, 5);
    ASSERT_EQ(*additiveMetrics.nBatches, 1);
    ASSERT_EQ(*additiveMetrics.executionTime, Microseconds{280});
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
    curop->setGenericOpRequestDetails(NamespaceString::createNamespaceString_forTest("myDb.coll"),
                                      nullptr,
                                      command,
                                      NetworkOp::dbQuery);

    BSONObjBuilder builder;
    od.append(opCtx.get(), ls, {}, builder);
    auto bs = builder.done();

    // Append should always include these basic fields.
    for (const std::string& field : basicFields) {
        ASSERT_TRUE(bs.hasField(field));
    }

    // Append should include only the basic fields when just initialized.
    ASSERT_EQ(static_cast<size_t>(bs.nFields()), basicFields.size());
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
                                       SerializationContext::Prefix::ExcludePrefix,
                                       true);
        auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), nss, sc);

        curop->reportCurrentOpForClient(expCtx, client, false, false, &curOpObj);
        curop->reportCurrentOpForClient(
            expCtx, clientUserConn.get(), false, false, &curOpObjUserConn);
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
    curop->setGenericOpRequestDetails(
        NamespaceString::createNamespaceString_forTest(tid, "testDb.coll"),
        nullptr,
        command,
        NetworkOp::dbQuery);

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

}  // namespace
}  // namespace mongo
