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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
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

// TODO: remove when there is a proper mock elsewhere.
class ChangeStreamReaderContextMock : public ChangeStreamReaderContext {
public:
    explicit ChangeStreamReaderContextMock(const ChangeStream& changeStream)
        : _changeStream(changeStream) {}
    void openCursorsOnDataShards(Timestamp atClusterTime,
                                 const stdx::unordered_set<ShardId>& shardSet) override {}

    void closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardSet) override {}

    void openCursorOnConfigServer(Timestamp atClusterTime) override {}
    void closeCursorOnConfigServer() override {}

    const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const override {
        return _currentlyTargetedShards;
    }

    const ChangeStream& getChangeStream() const override {
        return _changeStream;
    }
    bool inDegradedMode() const override {
        return false;
    }

private:
    const ChangeStream _changeStream;
    stdx::unordered_set<ShardId> _currentlyTargetedShards;
};

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

DEATH_TEST_REGEX_F(ChangeStreamShardTargeterTest,
                   TargeterFailsWhenQueueIsEmpty,
                   "Tripwire assertion.*10767900") {
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(Timestamp(42, 0)));
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());
    ChangeStreamReaderContextMock context(changeStream);

    ChangeStreamShardTargeterMock mock;
    ASSERT_THROWS_CODE(
        mock.initialize(getOpCtx(), Timestamp(42, 1), context), AssertionException, 10767900);
}

DEATH_TEST_REGEX_F(ChangeStreamShardTargeterTest,
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

DEATH_TEST_REGEX_F(ChangeStreamShardTargeterTest,
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

DEATH_TEST_REGEX_F(ChangeStreamShardTargeterTest,
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
