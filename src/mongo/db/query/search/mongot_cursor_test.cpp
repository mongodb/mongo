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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/query/search/mongot_cursor_getmore_strategy.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/executor/task_executor_cursor_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace executor {
namespace {
/**
 * Follows the same pattern as task_executor_cursor_test.cpp, where this MongotCursorTestFixture
 * can be specialized with either pinned or unpinned cursor mechanics.
 */
template <typename Base>
class MongotCursorTestFixture : public Base {
public:
    void setUp() override {
        Base::setUp();
        client = serviceCtx->getService()->makeClient("TaskExecutorCursorTest");
        opCtx = client->makeOperationContext();
        Base::postSetUp();
    }

    void tearDown() override {
        opCtx.reset();
        client.reset();

        Base::tearDown();
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId,
                                             bool expectedPrefetch = true) {
        return Base::scheduleSuccessfulCursorResponse(
            fieldName, start, end, cursorId, expectedPrefetch);
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds,
                                                  bool expectedPrefetch = true) {
        return Base::scheduleSuccessfulMultiCursorResponse(
            fieldName, start, end, cursorIds, expectedPrefetch);
    }

    void scheduleErrorResponse(Status error) {
        return Base::scheduleErrorResponse(error);
    }
    void blackHoleNextOutgoingRequest() {
        return Base::blackHoleNextOutgoingRequest();
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId, bool expectedPrefetch = true) {
        return Base::scheduleSuccessfulKillCursorResponse(cursorId, expectedPrefetch);
    }

    std::unique_ptr<TaskExecutorCursor> makeMongotCursor(
        RemoteCommandRequest rcr,
        boost::optional<long long> startingBatchSize = boost::none,
        DocsNeededBounds bounds = DocsNeededBounds(docs_needed_bounds::Unknown(),
                                                   docs_needed_bounds::Unknown()),
        std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
            searchIdLookupMetrics = nullptr) {
        std::unique_ptr<MongotTaskExecutorCursorGetMoreStrategy> mongotGetMoreStrategy;

        if (startingBatchSize || searchIdLookupMetrics) {
            mongotGetMoreStrategy =
                std::make_unique<MongotTaskExecutorCursorGetMoreStrategy>(startingBatchSize,
                                                                          bounds,
                                                                          /*tenantId*/ boost::none,
                                                                          searchIdLookupMetrics);
        } else {
            // If we have no starting batchSize or idLookupMetrics, use the default GetMoreStrategy.
            mongotGetMoreStrategy = std::make_unique<MongotTaskExecutorCursorGetMoreStrategy>();
        }
        return Base::makeTec(rcr,
                             {gPinTaskExecCursorConns.load(), std::move(mongotGetMoreStrategy)});
    }

    bool hasReadyRequests() {
        return Base::hasReadyRequests();
    }

    bool tryWaitUntilReadyRequests() {
        return Base::tryWaitUntilReadyRequests();
    }

    /**
     * Tests that the TaskExecutorCursor with mongot options applies the docsRequested option on
     * getMore requests, whenever batchSize is disabled.
     */
    void BasicDocsRequestedTest() {
        // Asserting within a spawned thread could crash the unit test due to an uncaught exception.
        // We wrap the test with the threadAssertionMonitoredTest, which will do the work to track
        // assertions not in the main thread and propogate errors.
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search" << "foo"),
                                     opCtx.get());


            // Mock lookup id metrics as batches are processed.
            std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
                searchIdLookupMetrics =
                    std::make_shared<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>();
            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            auto tec = makeMongotCursor(rcr,
                                        /*startingBatchSize*/ boost::none,
                                        DocsNeededBounds(10, 10),
                                        searchIdLookupMetrics);

            // Mock the response for the first batch.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 2, cursorId, /*expectedPrefetch*/ false);

            // Exhaust the first batch.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 2);

            // Increment lookup id metrics. Simulate that neither of the docs are found so that
            // docsRequested stays at 10.
            for (int i = 0; i < 2; ++i) {
                searchIdLookupMetrics->incrementDocsSeenByIdLookup();
            }

            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // As soon as 'getNext()' is invoked, the TaskExecutorCursor will try to send a GetMore
            // and that will block this thread in the NetworkInterfaceMock until there is a
            // scheduled response. However, we cannot schedule the cursor response on the main
            // thread before we call 'getNext()' as that will cause the NetworkInterfaceMock to
            // block until there is request enqueued ('getNext()' is the function which will enqueue
            // such as request). To avoid this deadlock, we start a new thread which will schedule a
            // response on the NetworkInterfaceMock.
            auto responseSchedulerThread = monitor.spawn([&] {
                auto recievedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 3, 4, 0, /*expectedPrefetch*/ false);

                // Assert that the command processed for the above response matches with the
                // lambda to augment the getMore command used during construction of the TEC
                // above.
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("docsRequested" << 10));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, recievedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 3);
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 4);
            ASSERT_FALSE(tec->getNext(opCtx.get()));

            // Joining the thread which schedules the cursor response for the GetMore here forces
            // the destructor of NetworkInterfaceMock::InNetworkGuard to run, which ensures that the
            // 'NetworkInterfaceMock' stops executing as the network thread. This is required before
            // we invoke 'hasReadyRequests()' which enters the network again.
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());
        });
    }

    /**
     * Tests that the TaskExecutorCursor properly computes the docsRequested option using the
     * idLookup metrics across GetMore requests.
     */
    void DecreasingDocsRequestedTest() {
        // See comments in "BasicDocsRequestedTest" for why this thread monitor setup is necessary
        // throughout the test.
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search" << "foo"),
                                     opCtx.get());

            // Mock lookup id metrics as batches are processed.
            std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
                searchIdLookupMetrics =
                    std::make_shared<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>();
            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            auto tec = makeMongotCursor(rcr,
                                        /*startingBatchSize*/ boost::none,
                                        DocsNeededBounds(50, 50),
                                        searchIdLookupMetrics);
            // Mock the response for the first batch.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 50, cursorId, /*expectedPrefetch*/ false);

            // Exhaust the first batch.
            for (int docNum = 1; docNum <= 50; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }

            // Increment lookup id metrics. Simulate that 20/50 of the docs are found so that
            // docsRequested decreases to 30.
            for (int i = 0; i < 50; ++i) {
                searchIdLookupMetrics->incrementDocsSeenByIdLookup();
            }
            for (int i = 0; i < 20; ++i) {
                searchIdLookupMetrics->incrementDocsReturnedByIdLookup();
            }

            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // Schedule another batch, where docsRequested should be set to 50 - 20 = 30;
            auto responseSchedulerThread = monitor.spawn([&] {
                auto recievedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 51, 80, cursorId, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("docsRequested" << 30));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, recievedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 51; docNum <= 80; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }

            // Increment lookup id metrics. Simulate that 20/30 of the docs are found so that
            // docsRequested decreases to 10.
            for (int i = 0; i < 30; ++i) {
                searchIdLookupMetrics->incrementDocsSeenByIdLookup();
            }
            for (int i = 0; i < 20; ++i) {
                searchIdLookupMetrics->incrementDocsReturnedByIdLookup();
            }
            responseSchedulerThread.join();

            // Schedule another batch, where docsRequested should be set to 30 - 20 = 10;
            responseSchedulerThread = monitor.spawn([&] {
                auto recievedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 81, 81, 0, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("docsRequested" << 10));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, recievedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 81);
            ASSERT_FALSE(tec->getNext(opCtx.get()));
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());
        });
    }

    void BatchSizeGrowsExponentiallyFromDefaultStartingSizeTest() {
        // See comments in "BasicDocsRequestedTest" for why this thread monitor setup is necessary
        // throughout the test.
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search" << "foo"),
                                     opCtx.get());
            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            auto tec = makeMongotCursor(rcr);

            // Mock the response for the first batch.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 101, cursorId, /*expectedPrefetch*/ false);

            // Exhaust the first batch.
            for (int docNum = 1; docNum <= 101; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }

            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // Schedule another batch, where the batchSize should have exponentially increased from
            // the first batchSize. The batchSize should be set to kDefaultMongotBatchSize *
            // kInternalSearchBatchSizeGrowthFactor = 152.
            auto responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 102, 253, cursorId, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 152));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 102; docNum <= 253; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            responseSchedulerThread.join();

            // Schedule the final batch, where batchSize should have exponentially increased from
            // the batchSize in the last GetMore request. The batchSize should be set to
            // kDefaultMongotBatchSize * (kInternalSearchBatchSizeGrowthFactor)^2 = 228.
            responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 253, 253, 0, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 228));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 253);
            ASSERT_FALSE(tec->getNext(opCtx.get()));
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());
        });
    }

    void BatchSizeGrowsExponentiallyFromCustomStartingSizeTest() {
        // See comments in "BasicDocsRequestedTest" for why this thread monitor setup is necessary
        // throughout the test.
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search" << "foo"),
                                     opCtx.get());
            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            auto tec = makeMongotCursor(rcr,
                                        /*startingBatchSize*/ 3);
            // Mock the response for the first batch.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 3, cursorId, /*expectedPrefetch*/ false);

            // Exhaust the first batch.
            for (int docNum = 1; docNum <= 3; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }

            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // Schedule another batch, where the batchSize should have exponentially increased from
            // the first batchSize. The batchSize should be set to startingBatchSize *
            // kInternalSearchBatchSizeGrowthFactor.
            auto responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 3, 7, cursorId, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 5));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 3; docNum <= 7; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            responseSchedulerThread.join();

            // Schedule another batch, where batchSize should have exponentially increased from the
            // batchSize in the last GetMore request. The batchSize should be set to
            // startingBatchSize * (kInternalSearchBatchSizeGrowthFactor)^2.
            responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 8, 8, 0, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 8));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 8);
            ASSERT_FALSE(tec->getNext(opCtx.get()));
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());
        });
    }

    void BatchSizePausesGrowthWhenBatchNotFilledTest() {
        // See comments in "BasicDocsRequestedTest" for why this thread monitor setup is necessary
        // throughout the test.
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search" << "foo"),
                                     opCtx.get());
            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            auto tec = makeMongotCursor(rcr,
                                        /*startingBatchSize*/ 20);
            // Mock the response for the first batch, which only returns 15 documents, rather than
            // the requested 20.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 15, cursorId, /*expectedPrefetch*/ false);

            // Exhaust the first batch.
            for (int docNum = 1; docNum <= 15; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }

            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // Schedule another batch, where the batchSize should remain at 20 since the previous
            // batchSize requested wasn't fulfilled. This batch will only return 10 documents.
            auto responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 16, 25, cursorId, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 20));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 16; docNum <= 25; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            responseSchedulerThread.join();

            // Schedule another batch, where the batchSize again remains at 20 since the previous
            // batchSize requested wasn't fulfilled again.
            responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 26, 26, 0, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 20));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 26);
            ASSERT_FALSE(tec->getNext(opCtx.get()));
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());
        });
    }

    void BatchSizeGrowthPausesThenResumesTest() {
        // See comments in "BasicDocsRequestedTest" for why this thread monitor setup is necessary
        // throughout the test.
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search" << "foo"),
                                     opCtx.get());
            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            auto tec = makeMongotCursor(rcr,
                                        /*startingBatchSize*/ 5);
            // Mock the response for the first batch, which fulfills the requested batchSize of 5.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 5, cursorId, /*expectedPrefetch*/ false);

            // Exhaust the first batch.
            for (int docNum = 1; docNum <= 5; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }

            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // Schedule another batch, where the batchSize requested has increased to 8, but it will
            // only return 7.
            auto responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 6, 12, cursorId, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 8));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 6; docNum <= 12; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            responseSchedulerThread.join();

            // Schedule another batch, where the batchSize remains at 8 and returns a filled batch
            // of 8.
            responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 13, 20, cursorId, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 8));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 13; docNum <= 20; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            responseSchedulerThread.join();

            // Schedule the final batch, where the batchSize exponential increase should've resumed
            // and will now request 12.
            responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 20, 32, 0, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << 12));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 20; docNum <= 32; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            ASSERT_FALSE(tec->getNext(opCtx.get()));
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());
        });
    }

    void PrefetchAllGetMoresTest() {
        CursorId cursorId = 1;
        RemoteCommandRequest rcr(HostAndPort("localhost"),
                                 DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                 BSON("search" << "foo"),
                                 opCtx.get());
        // Use NeedAll bounds to trigger pre-fetching for all batches.
        auto tec = makeMongotCursor(
            rcr,
            /*startingBatchSize*/ 5,
            DocsNeededBounds(docs_needed_bounds::NeedAll(), docs_needed_bounds::NeedAll()));
        // Assert the initial request is received.
        ASSERT_TRUE(tryWaitUntilReadyRequests());
        scheduleSuccessfulCursorResponse("firstBatch", 1, 5, cursorId, /*expectedPrefetch*/ true);

        // Populate the cursor to process the initial batch, which should dispatch the pre-fetched
        // request for the first getMore, even before any getNexts.
        tec->populateCursor(opCtx.get());
        // Assert the pre-fetched GetMore was recevied.
        ASSERT_TRUE(tryWaitUntilReadyRequests());
        // Mock the response for the first getMore.
        scheduleSuccessfulCursorResponse("nextBatch", 6, 10, cursorId, /*expectedPrefetch*/ true);

        // Exhaust the first batch, then request the first result of the first getMore, prompting
        // another pre-fetched batch.
        for (int docNum = 1; docNum <= 6; docNum++) {
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
        }
        // Assert another pre-fetched GetMore was recevied.
        ASSERT_TRUE(tryWaitUntilReadyRequests());
        // Mock the response for the second getMore, which returns a closed cursor.
        scheduleSuccessfulCursorResponse(
            "nextBatch", 11, 15, /*cursorId*/ 0, /*expectedPrefetch*/ true);

        // Exhaust the second batch, then request the first result of the second getMore, to ensure
        // no requests are sent since the cursor was closed.
        for (int docNum = 7; docNum <= 11; docNum++) {
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
        }

        // Assert that the TaskExecutorCursor has not pre-fetched a GetMore since the cursor was
        // exhausted.
        ASSERT_FALSE(hasReadyRequests());
    }

    void DefaultStartPrefetchAfterThreeBatchesTest() {
        std::shared_ptr<executor::TaskExecutor> pinnedExecutor;

        // See comments in "BasicDocsRequestedTest" for why this thread monitor setup is necessary
        // throughout the test.
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search" << "foo"),
                                     opCtx.get());
            // Use the default mongot cursor behavior, which should only start pre-fetching after
            // the third batch is received.
            auto tec = makeMongotCursor(rcr);
            // Mock and exhaust the response for the first batch.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 101, cursorId, /*expectedPrefetch*/ false);
            for (int docNum = 1; docNum <= 101; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // Schedule and exhaust the second batch.
            auto responseSchedulerThread = monitor.spawn([&] {
                scheduleSuccessfulCursorResponse(
                    "nextBatch", 102, 303, cursorId, /*expectedPrefetch*/ false);
            });
            for (int docNum = 102; docNum <= 303; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            responseSchedulerThread.join();

            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // Schedule the third batch, and request just the first document from that batch. Upon
            // receipt of the third batch, a request to pre-fetch the fourth batch should be sent.
            responseSchedulerThread = monitor.spawn([&] {
                scheduleSuccessfulCursorResponse(
                    "nextBatch", 304, 707, cursorId, /*expectedPrefetch*/ false);
            });
            ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 304);
            responseSchedulerThread.join();

            // Assert the pre-fetched GetMore was recevied.
            ASSERT_TRUE(tryWaitUntilReadyRequests());
            // Black hole the pre-fetched fourth batch since it won't be necessary.
            blackHoleNextOutgoingRequest();

            // Grab shared ownership of the cursor's executor so that we can drain the killCursors
            // command emitted from the cursor's destructor.
            pinnedExecutor = tec->getExecutor_forTest();
        });

        // Shutdown and drain the cursor's executor to avoid deadlock.
        // TODO SERVER-93757: handle this more gracefully.
        pinnedExecutor->shutdown();
        Base::runReadyNetworkOperations();
        pinnedExecutor->join();
    }

    /*
     * Test that in a non-stored source query that has an extractable limit that the batch size
     * updates properly to mongot if not all the returned documents are found in id lookup.
     */
    void NonStoredSourceExtractableLimitNotAllDocsFoundInLookupTest() {
        unittest::threadAssertionMonitoredTest([&](auto& monitor) {
            CursorId cursorId = 1;
            RemoteCommandRequest rcr(HostAndPort("localhost"),
                                     DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                     BSON("search" << "foo"),
                                     opCtx.get());

            // Mock lookup id metrics as batches are processed.
            std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
                searchIdLookupMetrics =
                    std::make_shared<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>();

            // Construction of the TaskExecutorCursor enqueues a request in the
            // NetworkInterfaceMock.
            auto tec = makeMongotCursor(rcr,
                                        /*startingBatchSize*/ 107,
                                        DocsNeededBounds(100, 100),
                                        searchIdLookupMetrics);

            // Mock the response for the first batch, which fulfills the requested batchSize of 101.
            scheduleSuccessfulCursorResponse(
                "firstBatch", 1, 107, cursorId, /*expectedPrefetch*/ false);

            // Exhaust the first batch.
            for (int docNum = 1; docNum <= 107; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }

            // Increment lookup id metrics. Simulate that not all docs are found and check
            // that the batch size updates properly.
            for (int i = 0; i < 107; ++i) {
                searchIdLookupMetrics->incrementDocsSeenByIdLookup();
            }
            for (int i = 0; i < 82; ++i) {
                searchIdLookupMetrics->incrementDocsReturnedByIdLookup();
            }

            // Compute the expected batch size and ensure that number appears in the cmd message.
            long long expectedBatchSize =
                1.064 * (double(100 - 82) / double(double(82) / double(107)));  // 24

            // Assert that the TaskExecutorCursor has not pre-fetched a GetMore.
            ASSERT_FALSE(hasReadyRequests());

            // Schedule another batch, where the batchSize is now 24.
            auto responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 102, 126, cursorId, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << expectedBatchSize));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 102; docNum <= 126; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());

            // Increment lookup id metrics. Simulate that not all docs are found again and check
            // that the batch size updates properly.
            for (int i = 0; i < 24; ++i) {
                searchIdLookupMetrics->incrementDocsSeenByIdLookup();
            }
            for (int i = 0; i < 16; ++i) {
                searchIdLookupMetrics->incrementDocsReturnedByIdLookup();
            }

            // Compute the expected batch size and ensure that number appears in the cmd message.
            expectedBatchSize = 1.064 *
                (double(100 - (82 + 16)) / double(double(82 + 16) / double(107 + 24)));  // 2

            responseSchedulerThread = monitor.spawn([&] {
                auto receivedGetMoreCmd = scheduleSuccessfulCursorResponse(
                    "nextBatch", 126, 128, /*cursorId*/ 0, /*expectedPrefetch*/ false);
                const auto expectedGetMoreCmd =
                    BSON("getMore" << 1LL << "collection"
                                   << "test"
                                   << "cursorOptions" << BSON("batchSize" << expectedBatchSize));
                ASSERT_BSONOBJ_EQ(expectedGetMoreCmd, receivedGetMoreCmd);
            });

            // Schedules the GetMore request and exhausts the cursor.
            for (int docNum = 126; docNum <= 128; docNum++) {
                ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), docNum);
            }
            responseSchedulerThread.join();

            // Assert no GetMore is requested.
            ASSERT_FALSE(hasReadyRequests());
        });
    }

    ServiceContext::UniqueServiceContext serviceCtx = ServiceContext::make();
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
};

using NonPinningMongotCursorTestFixture =
    MongotCursorTestFixture<NonPinningTaskExecutorCursorTestFixture>;
using PinnedConnMongotCursorTestFixture =
    MongotCursorTestFixture<PinnedConnTaskExecutorCursorTestFixture>;

TEST_F(NonPinningMongotCursorTestFixture, BasicDocsRequestedTest) {
    BasicDocsRequestedTest();
}

TEST_F(PinnedConnMongotCursorTestFixture, BasicDocsRequestedTest) {
    BasicDocsRequestedTest();
}

TEST_F(NonPinningMongotCursorTestFixture, DecreasingDocsRequestedTest) {
    DecreasingDocsRequestedTest();
}

TEST_F(PinnedConnMongotCursorTestFixture, DecreasingDocsRequestedTest) {
    DecreasingDocsRequestedTest();
}

TEST_F(PinnedConnMongotCursorTestFixture, BatchSizeGrowsExponentiallyFromDefaultStartingSizeTest) {
    BatchSizeGrowsExponentiallyFromDefaultStartingSizeTest();
}

TEST_F(NonPinningMongotCursorTestFixture, BatchSizeGrowsExponentiallyFromDefaultStartingSizeTest) {
    BatchSizeGrowsExponentiallyFromDefaultStartingSizeTest();
}

TEST_F(PinnedConnMongotCursorTestFixture, BatchSizeGrowsExponentiallyFromCustomStartingSizeTest) {
    BatchSizeGrowsExponentiallyFromCustomStartingSizeTest();
}

TEST_F(NonPinningMongotCursorTestFixture, BatchSizeGrowsExponentiallyFromCustomStartingSizeTest) {
    BatchSizeGrowsExponentiallyFromCustomStartingSizeTest();
}

TEST_F(PinnedConnMongotCursorTestFixture, BatchSizePausesGrowthWhenBatchNotFilledTest) {
    BatchSizePausesGrowthWhenBatchNotFilledTest();
}

TEST_F(NonPinningMongotCursorTestFixture, BatchSizePausesGrowthWhenBatchNotFilledTest) {
    BatchSizePausesGrowthWhenBatchNotFilledTest();
}

TEST_F(PinnedConnMongotCursorTestFixture, BatchSizeGrowthPausesThenResumesTest) {
    BatchSizeGrowthPausesThenResumesTest();
}

TEST_F(NonPinningMongotCursorTestFixture, BatchSizeGrowthPausesThenResumesTest) {
    BatchSizeGrowthPausesThenResumesTest();
}

TEST_F(PinnedConnMongotCursorTestFixture, PrefetchAllGetMoresTest) {
    PrefetchAllGetMoresTest();
}

TEST_F(NonPinningMongotCursorTestFixture, PrefetchAllGetMoresTest) {
    PrefetchAllGetMoresTest();
}

TEST_F(PinnedConnMongotCursorTestFixture, DefaultStartPrefetchAfterThreeBatchesTest) {
    DefaultStartPrefetchAfterThreeBatchesTest();
}

TEST_F(NonPinningMongotCursorTestFixture, DefaultStartPrefetchAfterThreeBatchesTest) {
    DefaultStartPrefetchAfterThreeBatchesTest();
}

TEST_F(PinnedConnMongotCursorTestFixture,
       NonStoredSourceExtractableLimitNotAllDocsFoundInLookupTest) {
    NonStoredSourceExtractableLimitNotAllDocsFoundInLookupTest();
}

TEST_F(NonPinningMongotCursorTestFixture,
       NonStoredSourceExtractableLimitNotAllDocsFoundInLookupTest) {
    NonStoredSourceExtractableLimitNotAllDocsFoundInLookupTest();
}

TEST(PinConnectionSettingTest, AlwaysSetWithGRPC) {
    RAIIServerParameterControllerForTest globalPinConn("pinTaskExecCursorConns", false);
    RAIIServerParameterControllerForTest grpcForSearch("useGrpcForSearch", true);
    ASSERT_TRUE(mongot_cursor::shouldPinConnection());
}

TEST(PinConnectionSettingTest, SetFromPinTaskExecCursorConns) {
    RAIIServerParameterControllerForTest globalPinConn("pinTaskExecCursorConns", true);
    RAIIServerParameterControllerForTest grpcForSearch("useGrpcForSearch", false);
    ASSERT_TRUE(mongot_cursor::shouldPinConnection());
}

TEST(PinConnectionSettingTest, NeitherParamSet) {
    RAIIServerParameterControllerForTest globalPinConn("pinTaskExecCursorConns", false);
    RAIIServerParameterControllerForTest grpcForSearch("useGrpcForSearch", false);
    ASSERT_FALSE(mongot_cursor::shouldPinConnection());
}

}  // namespace
}  // namespace executor
}  // namespace mongo
