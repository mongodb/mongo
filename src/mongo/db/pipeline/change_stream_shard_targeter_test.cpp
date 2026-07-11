// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_reader_context_mock.h"
#include "mongo/db/pipeline/change_stream_shard_targeter_mock.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const ChangeStreamShardTargeterMock::ReaderContextCallback kEmptyCallback =
    [](ChangeStreamShardTargeterMock::TimestampOrDocument, ChangeStreamReaderContext&) {
    };

DocumentSourceChangeStreamSpec buildChangeStreamSpec(Timestamp ts) {
    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */));
    return spec;
}

class ChangeStreamShardTargeterTest : public ServiceContextTest {
public:
    ChangeStreamShardTargeterTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(false /* shouldSetupTL */)) {
        _opCtx = makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>(_opCtx.get());
    }

    auto getOpCtx() {
        return _opCtx.get();
    }

    auto getExpCtx() {
        return _expCtx;
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

using ChangeStreamShardTargeterTestDeathTest = ChangeStreamShardTargeterTest;
DEATH_TEST_REGEX_F(ChangeStreamShardTargeterTestDeathTest,
                   TargeterFailsWhenQueueIsEmpty,
                   "Tripwire assertion.*10767900") {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    ChangeStreamShardTargeterMock mock;
    ASSERT_THROWS_CODE(
        mock.initialize(getOpCtx(), Timestamp(42, 1), context), AssertionException, 10767900);
}

DEATH_TEST_REGEX_F(ChangeStreamShardTargeterTestDeathTest,
                   TargeterFailsWhenInitializeIsCalledWithoutTimestamp,
                   "Tripwire assertion.*10767901") {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    std::vector<ChangeStreamShardTargeterMock::Response> responses;
    responses.emplace_back(
        Document{}, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, kEmptyCallback);

    ChangeStreamShardTargeterMock mock;
    mock.bufferResponses(responses);

    ASSERT_THROWS_CODE(
        mock.initialize(getOpCtx(), Timestamp(42, 1), context), AssertionException, 10767901);
}

DEATH_TEST_REGEX_F(ChangeStreamShardTargeterTestDeathTest,
                   InitializeCalledWithWrongTimestamp,
                   "Tripwire assertion.*10767902") {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    std::vector<ChangeStreamShardTargeterMock::Response> responses;
    responses.emplace_back(Timestamp(42, 0),
                           ShardTargeterDecision::kContinue,
                           boost::optional<Timestamp>{},
                           kEmptyCallback);

    ChangeStreamShardTargeterMock mock;
    mock.bufferResponses(responses);

    ASSERT_THROWS_CODE(
        mock.initialize(getOpCtx(), Timestamp(42, 1), context), AssertionException, 10767902);
}

DEATH_TEST_REGEX_F(ChangeStreamShardTargeterTestDeathTest,
                   TargeterFailsWhenHandleEventIsCalledWithoutDocument,
                   "Tripwire assertion.*10767903") {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    std::vector<ChangeStreamShardTargeterMock::Response> responses;
    responses.emplace_back(Timestamp(42, 0),
                           ShardTargeterDecision::kContinue,
                           boost::optional<Timestamp>{},
                           kEmptyCallback);

    ChangeStreamShardTargeterMock mock;
    mock.bufferResponses(responses);

    ASSERT_THROWS_CODE(
        mock.handleEvent(getOpCtx(), Document{BSONObj()}, context), AssertionException, 10767903);
}

TEST_F(ChangeStreamShardTargeterTest, InitializeReturnsContinue) {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    const auto decision = ShardTargeterDecision::kContinue;
    const Timestamp ts = Timestamp(42, 0);
    std::vector<ChangeStreamShardTargeterMock::Response> responses;
    responses.emplace_back(ts, decision, boost::optional<Timestamp>{}, kEmptyCallback);

    ChangeStreamShardTargeterMock mock;
    mock.bufferResponses(responses);

    auto shardTargeterDecision = mock.initialize(getOpCtx(), ts, context);
    ASSERT_EQ(decision, shardTargeterDecision);
}

TEST_F(ChangeStreamShardTargeterTest, InitializeReturnsSwitchToV1) {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    const auto decision = ShardTargeterDecision::kSwitchToV1;
    const Timestamp ts = Timestamp(42, 0);
    std::vector<ChangeStreamShardTargeterMock::Response> responses;
    responses.emplace_back(ts, decision, boost::optional<Timestamp>{}, kEmptyCallback);

    ChangeStreamShardTargeterMock mock;
    mock.bufferResponses(responses);

    auto shardTargeterDecision = mock.initialize(getOpCtx(), ts, context);
    ASSERT_EQ(decision, shardTargeterDecision);
}


TEST_F(ChangeStreamShardTargeterTest, StartChangeStreamSegmentReturnsContinue) {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    const auto decision = ShardTargeterDecision::kContinue;
    const Timestamp ts = Timestamp(42, 0);
    const Timestamp endTs = Timestamp(42, 99);

    std::vector<ChangeStreamShardTargeterMock::Response> responses;
    responses.emplace_back(ts, decision, endTs, kEmptyCallback);

    ChangeStreamShardTargeterMock mock;
    mock.bufferResponses(responses);

    auto shardTargeterDecision = mock.startChangeStreamSegment(getOpCtx(), ts, context);
    ASSERT_EQ(decision, shardTargeterDecision.first);
    ASSERT_TRUE(shardTargeterDecision.second.has_value());
    ASSERT_EQ(endTs, *shardTargeterDecision.second);
}

TEST_F(ChangeStreamShardTargeterTest, HandleEventReturnsContinue) {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    const auto decision = ShardTargeterDecision::kContinue;

    std::vector<ChangeStreamShardTargeterMock::Response> responses;
    responses.emplace_back(Document{}, decision, boost::optional<Timestamp>{}, kEmptyCallback);

    ChangeStreamShardTargeterMock mock;
    mock.bufferResponses(responses);

    auto shardTargeterDecision = mock.handleEvent(getOpCtx(), Document{BSONObj()}, context);
    ASSERT_EQ(decision, shardTargeterDecision);
}

TEST_F(ChangeStreamShardTargeterTest, HandleEventReturnsSwitchToV1) {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    const auto decision = ShardTargeterDecision::kSwitchToV1;

    std::vector<ChangeStreamShardTargeterMock::Response> responses;
    responses.emplace_back(Document{}, decision, boost::optional<Timestamp>{}, kEmptyCallback);

    ChangeStreamShardTargeterMock mock;
    mock.bufferResponses(responses);

    auto shardTargeterDecision = mock.handleEvent(getOpCtx(), Document{BSONObj()}, context);
    ASSERT_EQ(decision, shardTargeterDecision);
}

}  // namespace
}  // namespace mongo
