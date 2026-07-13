// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/cluster_client_cursor_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/s/query/exec/router_stage_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"

#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

/**
 * RouterExecStage that throws a specified error code when next() is called. Models mongos-internal
 * stages that throw rather than returning a non-OK Status (defense-in-depth path).
 */
class RouterStageThrowingMock final : public RouterExecStage {
public:
    RouterStageThrowingMock(OperationContext* opCtx, ErrorCodes::Error code)
        : RouterExecStage(opCtx), _code(code) {}

    StatusWith<ClusterQueryResult> next() final {
        uasserted(_code, "test-induced error");
        MONGO_UNREACHABLE;
    }

    void kill(OperationContext*) final {}
    bool remotesExhausted() const final {
        return false;
    }

private:
    ErrorCodes::Error _code;
};

/**
 * RouterExecStage that returns a non-OK StatusWith when next() is called. Models the production
 * path where shard errors travel back to mongos as Status values via AsyncResultsMerger, not as
 * thrown exceptions.
 */
class RouterStageReturningErrorMock final : public RouterExecStage {
public:
    RouterStageReturningErrorMock(OperationContext* opCtx, ErrorCodes::Error code)
        : RouterExecStage(opCtx), _code(code) {}

    StatusWith<ClusterQueryResult> next() final {
        return Status(_code, "test-induced status error");
    }

    void kill(OperationContext*) final {}
    bool remotesExhausted() const final {
        return false;
    }

private:
    ErrorCodes::Error _code;
};

class ClusterClientCursorImplTest : public ClockSourceMockServiceContextTest {
protected:
    ClusterClientCursorImplTest() {
        _opCtx = makeOperationContext();
    }

    ClockSourceMock* useClock() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource());
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ClusterClientCursorImplTest, NumReturnedSoFar) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    for (int i = 1; i < 10; ++i) {
        mockStage->queueResult(BSON("a" << i));
    }

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    ASSERT_EQ(cursor.getNumReturnedSoFar(), 0);

    for (int i = 1; i < 10; ++i) {
        auto result = cursor.next();
        ASSERT(result.isOK());
        ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), BSON("a" << i));
        ASSERT_EQ(cursor.getNumReturnedSoFar(), i);
    }
    // Now check that if nothing is fetched the getNumReturnedSoFar stays the same.
    auto result = cursor.next();
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().isEOF());
    ASSERT_EQ(cursor.getNumReturnedSoFar(), 9LL);
}

TEST_F(ClusterClientCursorImplTest, QueueResult) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 4));

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    auto firstResult = cursor.next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));

    cursor.queueResult(BSON("a" << 2));
    cursor.queueResult(BSON("a" << 3));

    auto secondResult = cursor.next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 2));

    auto thirdResult = cursor.next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*thirdResult.getValue().getResult(), BSON("a" << 3));

    auto fourthResult = cursor.next();
    ASSERT_OK(fourthResult.getStatus());
    ASSERT(fourthResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*fourthResult.getValue().getResult(), BSON("a" << 4));

    auto fifthResult = cursor.next();
    ASSERT_OK(fifthResult.getStatus());
    ASSERT(fifthResult.getValue().isEOF());

    ASSERT_EQ(cursor.getNumReturnedSoFar(), 4LL);
}

TEST_F(ClusterClientCursorImplTest, RemotesExhausted) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->markRemotesExhausted();

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);
    ASSERT_TRUE(cursor.remotesExhausted());

    auto firstResult = cursor.next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));
    ASSERT_TRUE(cursor.remotesExhausted());

    auto secondResult = cursor.next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 2));
    ASSERT_TRUE(cursor.remotesExhausted());

    auto thirdResult = cursor.next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT_TRUE(thirdResult.getValue().isEOF());
    ASSERT_TRUE(cursor.remotesExhausted());

    ASSERT_EQ(cursor.getNumReturnedSoFar(), 2LL);
}

TEST_F(ClusterClientCursorImplTest, RemoteTimeoutPartialResultsDisallowed) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::MaxTimeMSExpired, "timeout"));
    mockStage->markRemotesExhausted();

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);
    ASSERT_TRUE(cursor.remotesExhausted());

    auto firstResult = cursor.next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));
    ASSERT_TRUE(cursor.remotesExhausted());

    auto thirdResult = cursor.next();
    ASSERT_EQ(thirdResult.getStatus().code(), ErrorCodes::MaxTimeMSExpired);
    ASSERT_TRUE(cursor.remotesExhausted());
    ASSERT_FALSE(cursor.partialResultsReturned());
    ASSERT_EQ(cursor.getNumReturnedSoFar(), 1LL);
}

TEST_F(ClusterClientCursorImplTest, RemoteTimeoutPartialResultsAllowed) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::MaxTimeMSExpired, "timeout"));
    mockStage->markRemotesExhausted();

    auto params =
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient());
    params.isAllowPartialResults = true;

    ClusterClientCursorImpl cursor(
        _opCtx.get(), std::move(mockStage), std::move(params), boost::none);
    ASSERT_TRUE(cursor.remotesExhausted());

    auto firstResult = cursor.next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));
    ASSERT_TRUE(cursor.remotesExhausted());

    auto thirdResult = cursor.next();
    ASSERT_EQ(thirdResult.getStatus().code(), ErrorCodes::MaxTimeMSExpired);
    ASSERT_TRUE(cursor.remotesExhausted());
    ASSERT_TRUE(cursor.partialResultsReturned());
    ASSERT_EQ(cursor.getNumReturnedSoFar(), 1LL);
}

TEST_F(ClusterClientCursorImplTest, ForwardsAwaitDataTimeout) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    auto mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);
    ASSERT_OK(cursor.setAwaitDataTimeout(Milliseconds(789)));

    auto awaitDataTimeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(awaitDataTimeout.getStatus());
    ASSERT_EQ(789, durationCount<Milliseconds>(awaitDataTimeout.getValue()));
}

TEST_F(ClusterClientCursorImplTest, ChecksForInterrupt) {
    auto mockStage = std::make_unique<RouterStageMock>(nullptr);
    for (int i = 1; i < 2; ++i) {
        mockStage->queueResult(BSON("a" << i));
    }

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    // Pull one result out of the cursor.
    auto result = cursor.next();
    ASSERT(result.isOK());
    ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), BSON("a" << 1));

    // Now interrupt the opCtx which the cursor is running under.
    {
        std::lock_guard<Client> lk(*_opCtx->getClient());
        _opCtx->markKilled(ErrorCodes::CursorKilled);
    }

    // Now check that a subsequent call to next() will fail.
    result = cursor.next();
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus(), ErrorCodes::CursorKilled);
}

TEST_F(ClusterClientCursorImplTest, LogicalSessionIdsOnCursors) {
    // Make a cursor with no lsid
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                     APIParameters(),
                                     boost::none /* ReadPreferenceSetting */,
                                     boost::none /* repl::ReadConcernArgs */,
                                     OperationSessionInfoFromClient());
    ClusterClientCursorImpl cursor{
        _opCtx.get(), std::move(mockStage), std::move(params), boost::none};
    ASSERT(!cursor.getLsid());

    // Make a cursor with an lsid
    auto mockStage2 = std::make_unique<RouterStageMock>(_opCtx.get());
    ClusterClientCursorParams params2(NamespaceString::createNamespaceString_forTest("test"),
                                      APIParameters(),
                                      boost::none /* ReadPreferenceSetting */,
                                      boost::none /* repl::ReadConcernArgs */,
                                      OperationSessionInfoFromClient());
    auto lsid = makeLogicalSessionIdForTest();
    ClusterClientCursorImpl cursor2{_opCtx.get(), std::move(mockStage2), std::move(params2), lsid};
    ASSERT(*(cursor2.getLsid()) == lsid);
}

TEST_F(ClusterClientCursorImplTest, ShouldStoreLSIDIfSetOnOpCtx) {
    std::shared_ptr<executor::TaskExecutor> nullExecutor;

    {
        // Make a cursor with no lsid or txnNumber.
        ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                         APIParameters(),
                                         boost::none /* ReadPreferenceSetting */,
                                         boost::none /* repl::ReadConcernArgs */,
                                         [&] {
                                             if (!_opCtx->getLogicalSessionId())
                                                 return OperationSessionInfoFromClient();
                                             return OperationSessionInfoFromClient{
                                                 *_opCtx->getLogicalSessionId(),
                                                 _opCtx->getTxnNumber()};
                                         }());

        auto cursor = ClusterClientCursorImpl::make(_opCtx.get(), nullExecutor, std::move(params));
        ASSERT_FALSE(cursor->getLsid());
        ASSERT_FALSE(cursor->getTxnNumber());
    }

    const auto lsid = makeLogicalSessionIdForTest();
    _opCtx->setLogicalSessionId(lsid);

    {
        // Make a cursor with an lsid and no txnNumber.
        ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                         APIParameters(),
                                         boost::none /* ReadPreferenceSetting */,
                                         boost::none /* repl::ReadConcernArgs */,
                                         [&] {
                                             if (!_opCtx->getLogicalSessionId())
                                                 return OperationSessionInfoFromClient();
                                             return OperationSessionInfoFromClient{
                                                 *_opCtx->getLogicalSessionId(),
                                                 _opCtx->getTxnNumber()};
                                         }());

        auto cursor = ClusterClientCursorImpl::make(_opCtx.get(), nullExecutor, std::move(params));
        ASSERT_EQ(*cursor->getLsid(), lsid);
        ASSERT_FALSE(cursor->getTxnNumber());
    }

    const TxnNumber txnNumber = 5;
    _opCtx->setTxnNumber(txnNumber);

    {
        // Make a cursor with an lsid and txnNumber.
        ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                         APIParameters(),
                                         boost::none /* ReadPreferenceSetting */,
                                         boost::none /* repl::ReadConcernArgs */,
                                         [&] {
                                             if (!_opCtx->getLogicalSessionId())
                                                 return OperationSessionInfoFromClient();
                                             return OperationSessionInfoFromClient{
                                                 *_opCtx->getLogicalSessionId(),
                                                 _opCtx->getTxnNumber()};
                                         }());

        auto cursor = ClusterClientCursorImpl::make(_opCtx.get(), nullExecutor, std::move(params));
        ASSERT_EQ(*cursor->getLsid(), lsid);
        ASSERT_EQ(*cursor->getTxnNumber(), txnNumber);
    }
}

TEST_F(ClusterClientCursorImplTest, ShouldStoreAPIParameters) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());

    APIParameters apiParams = APIParameters();
    apiParams.setAPIVersion("2");
    apiParams.setAPIStrict(true);
    apiParams.setAPIDeprecationErrors(true);

    ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                     apiParams,
                                     boost::none /* ReadPreferenceSetting */,
                                     boost::none /* repl::ReadConcernArgs */,
                                     OperationSessionInfoFromClient());
    ClusterClientCursorImpl cursor(
        _opCtx.get(), std::move(mockStage), std::move(params), boost::none);

    auto storedAPIParams = cursor.getAPIParameters();
    ASSERT_EQ("2", *storedAPIParams.getAPIVersion());
    ASSERT_TRUE(*storedAPIParams.getAPIStrict());
    ASSERT_TRUE(*storedAPIParams.getAPIDeprecationErrors());
}

TEST_F(ClusterClientCursorImplTest, IsEOF) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    RouterStageMock* mockStagePtr = mockStage.get();
    mockStagePtr->queueResult(BSON("a" << 1));

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    // Stage has a buffered result and remotes are not exhausted.
    ASSERT_FALSE(cursor.isEOF());

    auto result = cursor.next();
    ASSERT_OK(result.getStatus());
    ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), BSON("a" << 1));

    // Stage buffer is empty but remotes are still not exhausted.
    ASSERT_FALSE(cursor.isEOF());

    mockStagePtr->queueResult(BSON("a" << 2));
    mockStagePtr->markRemotesExhausted();
    // Remotes are exhausted, but stage has a buffered results.
    ASSERT_FALSE(cursor.isEOF());

    result = cursor.next();
    ASSERT_OK(result.getStatus());
    ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), BSON("a" << 2));
    // Remotes are exhausted, and stage has no buffered results.
    ASSERT_TRUE(cursor.isEOF());

    cursor.queueResult(BSON("a" << 3));
    // The stage is empty and exhausted, but the cursor itself has a stash.
    ASSERT_FALSE(cursor.isEOF());
    result = cursor.next();
    ASSERT_OK(result.getStatus());
    ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), BSON("a" << 3));
    ASSERT_TRUE(cursor.isEOF());
}

TEST_F(ClusterClientCursorImplTest, CheckChangeStreamServerStatusCursorMetrics) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;

    if (!capturer.canReadMetrics()) {
        return;
    }

    // total_opened starts at zero.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsTotalOpened), 0);

    auto makeChangeStreamCursor = [&]() {
        return ClusterClientCursorImpl(
            _opCtx.get(),
            std::make_unique<RouterStageMock>(_opCtx.get()),
            ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                      APIParameters(),
                                      boost::none /* ReadPreferenceSetting */,
                                      boost::none /* repl::ReadConcernArgs */,
                                      OperationSessionInfoFromClient()),
            boost::none);
    };

    CurOp::get(_opCtx.get())->debug().isChangeStreamQuery = true;

    auto cursor = makeChangeStreamCursor();

    // Opening the cursor increments the total opened count to 1, but does not update the lifespan.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsTotalOpened), 1);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsOpenTotal), 1);

    // Advance the mock clock so the cursor has a measurable lifespan.
    const Milliseconds lifespanMs{200};
    useClock()->advance(lifespanMs);

    // Killing the cursor records its lifespan in the histogram; total_opened remains at 1.
    cursor.kill(_opCtx.get());
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsTotalOpened), 1);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsOpenTotal), 0);

    auto histogram1 = capturer.readInt64Histogram(MetricNames::kChangeStreamCursorsLifespan);
    ASSERT_EQ(histogram1.count, 1);
    ASSERT_EQ(histogram1.sum, durationCount<Milliseconds>(lifespanMs) * 1000);

    // Open a second cursor and check that the total cursor count has been incremented to 2.
    auto cursor2 = makeChangeStreamCursor();

    // Opening the cursor increments the total opened count to 1, but does not update the lifespan.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsTotalOpened), 2);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsOpenTotal), 1);
    histogram1 = capturer.readInt64Histogram(MetricNames::kChangeStreamCursorsLifespan);
    ASSERT_EQ(histogram1.count, 1);

    // Kill second cursor and check the metrics are correctly updated.
    const Milliseconds lifespan2Ms{400};
    useClock()->advance(lifespan2Ms);

    cursor2.kill(_opCtx.get());
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsTotalOpened), 2);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorsOpenTotal), 0);

    auto histogram2 = capturer.readInt64Histogram(MetricNames::kChangeStreamCursorsLifespan);
    ASSERT_EQ(histogram2.count, 2);
    // 200 ms + 400 ms = 600 000 µs cumulative.
    ASSERT_EQ(histogram2.sum, 600'000);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamCursorThroughputMetrics) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;

    if (!capturer.canReadMetrics()) {
        return;
    }

    // Throughput counters start at zero.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorDocsReturned), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBytesReturned), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBatchesReturned), 0);

    CurOp::get(_opCtx.get())->debug().isChangeStreamQuery = true;

    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    const int64_t kNumDocs = 5;
    std::vector<int64_t> docSizes;
    int64_t expectedBytes = 0;
    for (int i = 1; i <= kNumDocs; ++i) {
        auto doc = BSON("_id" << i << "fullDocument" << BSON("a" << i));
        expectedBytes += doc.objsize();
        docSizes.push_back(doc.objsize());
        mockStage->queueResult(doc);
    }

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    // Deliver the results across two getMore batches: 3 documents, then 2. The throughput counters
    // advance as each batch is returned to the client, not only when the cursor is killed.
    const int64_t kFirstBatchDocs = 3;
    int64_t firstBatchBytes = 0;
    for (int i = 0; i < kFirstBatchDocs; ++i) {
        auto result = cursor.next();
        ASSERT_OK(result.getStatus());
        ASSERT(result.getValue().getResult());
        firstBatchBytes += docSizes[i];
    }
    cursor.recordChangeStreamThroughputMetricsForBatch();

    // The first batch is reflected immediately.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorDocsReturned),
              kFirstBatchDocs);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBytesReturned),
              firstBatchBytes);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBatchesReturned), 1);

    // Second batch: consume the remaining documents, then EOF.
    for (int i = kFirstBatchDocs; i < kNumDocs; ++i) {
        auto result = cursor.next();
        ASSERT_OK(result.getStatus());
        ASSERT(result.getValue().getResult());
    }
    auto eof = cursor.next();
    ASSERT_OK(eof.getStatus());
    ASSERT_TRUE(eof.getValue().isEOF());
    ASSERT_EQ(cursor.getNumReturnedSoFar(), kNumDocs);
    cursor.recordChangeStreamThroughputMetricsForBatch();

    // Only the second batch's delta is added on top of the first.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorDocsReturned), kNumDocs);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBytesReturned),
              expectedBytes);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBatchesReturned), 2);

    // Killing the cursor does not re-count throughput that was already recorded per batch.
    cursor.kill(_opCtx.get());
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorDocsReturned), kNumDocs);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBytesReturned),
              expectedBytes);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBatchesReturned), 2);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamCursorStashedDocumentCountedOnceInThroughput) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;

    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorDocsReturned), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBytesReturned), 0);

    CurOp::get(_opCtx.get())->debug().isChangeStreamQuery = true;

    const auto docA = BSON("_id" << 1 << "fullDocument" << BSON("a" << 1));
    const auto docB = BSON("_id" << 2 << "fullDocument" << BSON("b" << 2));

    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(docA);
    mockStage->queueResult(docB);

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    // Simulate the production flow where a document does not fit into the current batch: pull docA
    // out of the merge plan, then stash it back via queueResult() (as cluster_find.cpp does). This
    // batch delivers no documents to the client.
    {
        auto result = cursor.next();
        ASSERT_OK(result.getStatus());
        ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), docA);
        cursor.queueResult(*result.getValue().getResult());
    }
    cursor.recordChangeStreamThroughputMetricsForBatch();

    // Nothing was delivered, so no throughput and no batch is recorded for the empty batch.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorDocsReturned), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBytesReturned), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBatchesReturned), 0);

    // Next batch: re-serve the stashed docA (must NOT be counted a second time), then docB.
    {
        auto result = cursor.next();
        ASSERT_OK(result.getStatus());
        ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), docA);
    }
    {
        auto result = cursor.next();
        ASSERT_OK(result.getStatus());
        ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), docB);
    }
    cursor.recordChangeStreamThroughputMetricsForBatch();

    // docA was produced by next(), stashed, then re-served: it must be counted exactly once. So the
    // stream has returned 2 distinct documents (docA + docB), not 3. Note getNumReturnedSoFar()
    // legitimately reports 3, since it counts every next() that yields a document (including the
    // re-serve) — the throughput counters intentionally diverge from it here.
    ASSERT_EQ(cursor.getNumReturnedSoFar(), 3);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorDocsReturned), 2);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBytesReturned),
              docA.objsize() + docB.objsize());
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBatchesReturned), 1);

    cursor.kill(_opCtx.get());
}

TEST_F(ClusterClientCursorImplTest, NonChangeStreamCursorDoesNotRecordThroughputMetrics) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;

    if (!capturer.canReadMetrics()) {
        return;
    }

    // isChangeStreamQuery defaults to false — this is not a change stream cursor.
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    for (int i = 1; i <= 5; ++i) {
        mockStage->queueResult(BSON("a" << i));
    }

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    for (int i = 1; i <= 5; ++i) {
        ASSERT_OK(cursor.next().getStatus());
    }
    // Recording per batch is a no-op for a non-change-stream cursor.
    cursor.recordChangeStreamThroughputMetricsForBatch();
    cursor.kill(_opCtx.get());

    // No change stream throughput should be recorded for a non-change-stream cursor.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorDocsReturned), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBytesReturned), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamCursorBatchesReturned), 0);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamCursorCanBeDisposedEvenIfTimeGoesBackwards) {
    otel::metrics::OtelMetricsCapturer capturer;

    const Date_t createdDate = Date_t::fromMillisSinceEpoch(1000 * 1000);
    const Date_t beforeCreatedDate = Date_t::fromMillisSinceEpoch(1000);

    auto clock = useClock();
    clock->reset(createdDate);

    CurOp::get(_opCtx.get())->debug().isChangeStreamQuery = true;
    auto cursor = ClusterClientCursorImpl(
        _opCtx.get(),
        std::make_unique<RouterStageMock>(_opCtx.get()),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    // Move time backwards.
    clock->reset(beforeCreatedDate);

    // Enable log capture.
    unittest::LogCaptureGuard logs;

    // Kill the cursor. This should not fail.
    cursor.kill(_opCtx.get());

    // Expect 0 massert log messages to be captured about invalid histogram values.
    ASSERT_EQ(0, logs.countBSONContainingSubset(BSON("id" << 23077)));
}

TEST_F(ClusterClientCursorImplTest, UpdateCursorMetricsStoresOptimeForChangeStream) {
    CurOp::get(_opCtx.get())->debug().isChangeStreamQuery = true;
    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::make_unique<RouterStageMock>(_opCtx.get()),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    ChangeStreamCursorMetrics csMetrics;
    csMetrics.setOptime(Timestamp(100, 1));
    cursor.updateMetrics(csMetrics);

    ASSERT(cursor.getChangeStreamMetrics().has_value());
    ASSERT_EQ(Timestamp(100, 1), *cursor.getChangeStreamMetrics()->getOptime());
}

TEST_F(ClusterClientCursorImplTest, UpdateCursorMetricsIgnoredForNonChangeStreamCursor) {
    // isChangeStreamQuery defaults to false — cursor is not a change stream cursor.
    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::make_unique<RouterStageMock>(_opCtx.get()),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    ChangeStreamCursorMetrics csMetrics;
    csMetrics.setOptime(Timestamp(100, 1));
    cursor.updateMetrics(csMetrics);

    ASSERT_FALSE(cursor.getChangeStreamMetrics().has_value());
}

TEST_F(ClusterClientCursorImplTest, UpdateCursorMetricsIgnoresNullOptime) {
    CurOp::get(_opCtx.get())->debug().isChangeStreamQuery = true;
    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::make_unique<RouterStageMock>(_opCtx.get()),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    ChangeStreamCursorMetrics csMetrics;  // optime not set (boost::none)
    cursor.updateMetrics(csMetrics);

    ASSERT_FALSE(cursor.getChangeStreamMetrics().has_value());
}

// Helper used by change stream error counter tests.
// Sets isChangeStreamQuery = true and constructs a cursor backed by a stage that throws 'code'.
// The caller owns the cursor.
ClusterClientCursorImpl makeThrowingChangeStreamCursor(OperationContext* opCtx,
                                                       ErrorCodes::Error code) {
    CurOp::get(opCtx)->debug().isChangeStreamQuery = true;
    return ClusterClientCursorImpl(
        opCtx,
        std::make_unique<RouterStageThrowingMock>(opCtx, code),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamNonRetriableHistoryLostCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableHistoryLost), 0);

    auto cursor = makeThrowingChangeStreamCursor(_opCtx.get(), ErrorCodes::ChangeStreamHistoryLost);
    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::ChangeStreamHistoryLost);

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableHistoryLost), 1);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamNonRetriableFatalErrorCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableFatalError), 0);

    auto cursor = makeThrowingChangeStreamCursor(_opCtx.get(), ErrorCodes::ChangeStreamFatalError);
    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::ChangeStreamFatalError);

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableFatalError), 1);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamNonRetriableBsonObjectTooLargeCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(
        capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableBsonObjectTooLarge),
        0);

    auto cursor = makeThrowingChangeStreamCursor(_opCtx.get(), ErrorCodes::BSONObjectTooLarge);
    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::BSONObjectTooLarge);

    ASSERT_EQ(
        capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableBsonObjectTooLarge),
        1);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamNonRetriableOtherCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther), 0);

    // BadValue is a non-retriable error that is not a named change stream error.
    auto cursor = makeThrowingChangeStreamCursor(_opCtx.get(), ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::BadValue);

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther), 1);
}

TEST_F(ClusterClientCursorImplTest,
       ChangeStreamRetriableInterruptedDueToReplStateChangeCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamErrorRetriableInterruptedDueToReplStateChange),
              0);

    auto cursor =
        makeThrowingChangeStreamCursor(_opCtx.get(), ErrorCodes::InterruptedDueToReplStateChange);
    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::InterruptedDueToReplStateChange);

    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamErrorRetriableInterruptedDueToReplStateChange),
              1);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamRetriableOtherCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther), 0);

    // NetworkTimeout is a retriable error but not a named change stream error.
    auto cursor = makeThrowingChangeStreamCursor(_opCtx.get(), ErrorCodes::NetworkTimeout);
    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::NetworkTimeout);

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther), 1);
}

TEST_F(ClusterClientCursorImplTest, NonChangeStreamCursorDoesNotIncrementErrorCounters) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    // isChangeStreamQuery is false by default — do NOT set it.
    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::make_unique<RouterStageThrowingMock>(_opCtx.get(), ErrorCodes::BadValue),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none,
                                  boost::none,
                                  OperationSessionInfoFromClient()),
        boost::none);

    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::BadValue);

    // No counter should have incremented.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther), 0);
}

// --- Lifecycle event exclusion tests ---
// CloseChangeStream and ChangeStreamInvalidated are normal cursor lifecycle transitions,
// not errors. They must not increment any error counter.

TEST_F(ClusterClientCursorImplTest, CloseChangeStreamDoesNotIncrementErrorCounters) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const int64_t beforeNonRetriableOther =
        capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther);
    const int64_t beforeRetriableOther =
        capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther);

    auto cursor = makeThrowingChangeStreamCursor(_opCtx.get(), ErrorCodes::CloseChangeStream);
    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::CloseChangeStream);

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther),
              beforeNonRetriableOther);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther),
              beforeRetriableOther);
}

// ChangeStreamInvalidated is not tested here because it requires ChangeStreamInvalidationInfo
// extra data; constructing a Status without it is a fatal error in debug builds. In production
// this exception is always thrown with proper extra info from the pipeline stages.

// =============================================================================
// Status-path tests (the production path)
//
// In production, shard errors return to mongos via AsyncResultsMerger as non-OK StatusWith
// values — they are NOT thrown as exceptions. The tests above exercise the exception path
// (defense-in-depth). The tests below exercise the StatusWith path via
// RouterStageReturningErrorMock, which mirrors what AsyncResultsMerger::nextReady() does.
// =============================================================================

// Helper: like makeThrowingChangeStreamCursor but backed by a stage that returns a non-OK Status.
ClusterClientCursorImpl makeReturningErrorChangeStreamCursor(OperationContext* opCtx,
                                                             ErrorCodes::Error code) {
    CurOp::get(opCtx)->debug().isChangeStreamQuery = true;
    return ClusterClientCursorImpl(
        opCtx,
        std::make_unique<RouterStageReturningErrorMock>(opCtx, code),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamHistoryLostViaStatusCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableHistoryLost), 0);

    auto cursor =
        makeReturningErrorChangeStreamCursor(_opCtx.get(), ErrorCodes::ChangeStreamHistoryLost);
    ASSERT_NOT_OK(cursor.next());

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableHistoryLost), 1);
}

TEST_F(ClusterClientCursorImplTest, ChangeStreamRetriableOtherViaStatusCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther), 0);

    // NetworkTimeout is a retriable error not in the named switch cases.
    auto cursor = makeReturningErrorChangeStreamCursor(_opCtx.get(), ErrorCodes::NetworkTimeout);
    ASSERT_NOT_OK(cursor.next());

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther), 1);
}

TEST_F(ClusterClientCursorImplTest, CloseChangeStreamViaStatusDoesNotIncrementErrorCounters) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const int64_t beforeNonRetriableOther =
        capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther);
    const int64_t beforeRetriableOther =
        capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther);

    auto cursor = makeReturningErrorChangeStreamCursor(_opCtx.get(), ErrorCodes::CloseChangeStream);
    ASSERT_NOT_OK(cursor.next());

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther),
              beforeNonRetriableOther);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther),
              beforeRetriableOther);
}

// ChangeStreamInvalidated via Status is not tested here because constructing
// Status(ChangeStreamInvalidated, ...) without ChangeStreamInvalidationInfo is fatal in debug
// builds. In production this always propagates with proper extra info from the ARM.

TEST_F(ClusterClientCursorImplTest, NonChangeStreamCursorViaStatusDoesNotIncrementErrorCounters) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    // isChangeStreamQuery is false by default — do NOT set it.
    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::make_unique<RouterStageReturningErrorMock>(_opCtx.get(), ErrorCodes::BadValue),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none,
                                  boost::none,
                                  OperationSessionInfoFromClient()),
        boost::none);

    ASSERT_NOT_OK(cursor.next());

    // No counter should have incremented.
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther), 0);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorRetriableOther), 0);
}

// --- MaxTimeMSExpired exclusion tests ---
// MaxTimeMSExpired is a routine awaitData getMore timeout that the ARM surfaces as a non-OK
// Status. It is already tracked via _maxTimeMSExpired and must not pollute error counters.

TEST_F(ClusterClientCursorImplTest, MaxTimeMSExpiredViaStatusDoesNotIncrementErrorCounters) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const int64_t beforeNonRetriableOther =
        capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther);

    auto cursor = makeReturningErrorChangeStreamCursor(_opCtx.get(), ErrorCodes::MaxTimeMSExpired);
    ASSERT_NOT_OK(cursor.next());

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther),
              beforeNonRetriableOther);
}

TEST_F(ClusterClientCursorImplTest, MaxTimeMSExpiredViaExceptionDoesNotIncrementErrorCounters) {
    otel::metrics::OtelMetricsCapturer capturer;
    using otel::metrics::MetricNames;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const int64_t beforeNonRetriableOther =
        capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther);

    auto cursor = makeThrowingChangeStreamCursor(_opCtx.get(), ErrorCodes::MaxTimeMSExpired);
    ASSERT_THROWS_CODE(cursor.next(), DBException, ErrorCodes::MaxTimeMSExpired);

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamErrorNonRetriableOther),
              beforeNonRetriableOther);
}

}  // namespace
}  // namespace mongo
