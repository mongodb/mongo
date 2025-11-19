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

#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/change_stream_handle_topology_change_v2_stage.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_pipeline_helpers.h"
#include "mongo/db/pipeline/change_stream_reader_builder_mock.h"
#include "mongo/db/pipeline/change_stream_shard_targeter_mock.h"
#include "mongo/db/pipeline/change_stream_stage_test_fixture.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service_mock.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <deque>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using V2Stage = exec::agg::ChangeStreamHandleTopologyChangeV2Stage;

// A callback function used inside the shard targeter mock that does nothing.
const ChangeStreamShardTargeterMock::ReaderContextCallback kEmptyShardTargeterCallback =
    [](ChangeStreamShardTargeterMock::TimestampOrDocument, ChangeStreamReaderContext&) {
    };

// Empty set of shards.
const stdx::unordered_set<ShardId> kNoShards = {};

constexpr int kDefaultMinAllocationToShardsPollPeriodSecs = 1;

class CursorManagerMock : public V2Stage::CursorManager {
public:
    CursorManagerMock(const ChangeStream& changeStream, ChangeStreamReaderBuilder* readerBuilder)
        : _changeStream(changeStream), _readerBuilder(readerBuilder) {}

    void initialize(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    V2Stage* stage,
                    const ResumeTokenData& resumeTokenData) override {
        _resumeToken.emplace(ResumeToken(resumeTokenData));
    }

    void openCursorsOnDataShards(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 OperationContext* opCtx,
                                 Timestamp atClusterTime,
                                 const stdx::unordered_set<ShardId>& shardIds) override {
        throwShardNotFoundExceptionIfRequired();
        std::for_each(shardIds.begin(), shardIds.end(), [&](const ShardId& shardId) {
            _currentlyTargetedDataShards.insert(shardId);
        });
    }

    void openCursorOnConfigServer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  OperationContext* opCtx,
                                  Timestamp atClusterTime) override {
        throwShardNotFoundExceptionIfRequired();
        _cursorOpenedOnConfigServer = true;
    }

    void closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardIds) override {
        std::for_each(shardIds.begin(), shardIds.end(), [&](const ShardId& shardId) {
            _currentlyTargetedDataShards.erase(shardId);
        });
    }

    void closeCursorOnConfigServer(OperationContext* opCtx) override {
        _cursorOpenedOnConfigServer = false;
    }

    const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const override {
        return _currentlyTargetedDataShards;
    }

    const ChangeStream& getChangeStream() const override {
        return _changeStream;
    }

    void enableUndoNextMode() override {
        _undoNextMode.emplace(true);
    }

    void disableUndoNextMode() override {
        _undoNextMode.emplace(false);
    }

    void undoGetNextAndSetHighWaterMark(Timestamp highWaterMark) override {
        _undoNextHighWaterMark.emplace(highWaterMark);
    }

    boost::optional<bool> getUndoNextMode() const {
        return _undoNextMode;
    }

    boost::optional<Timestamp> getUndoGetNextHighWaterMark() const {
        return _undoNextHighWaterMark;
    }

    Timestamp getTimestampFromCurrentHighWaterMark() const override {
        tassert(10657540,
                "expecting high watermark timestamp to be set in test",
                _highWaterMarkTimestamp.has_value());
        return *_highWaterMarkTimestamp;
    }

    void setTimestampForCurrentHighWaterMark(Timestamp ts) {
        _highWaterMarkTimestamp = ts;
    }

    bool isInitialized() const {
        return _resumeToken.has_value();
    }

    ResumeToken getResumeToken() const {
        return *_resumeToken;
    }

    bool cursorOpenedOnConfigServer() const {
        return _cursorOpenedOnConfigServer;
    }

    void setThrowShardNotFoundException(bool value) {
        _throwShardNotFoundException = value;
    }

private:
    void throwShardNotFoundExceptionIfRequired() const {
        if (_throwShardNotFoundException) {
            error_details::throwExceptionForStatus(
                Status(ErrorCodes::ShardNotFound, "shard not found"));
        }
    }

    const ChangeStream _changeStream;
    ChangeStreamReaderBuilder* _readerBuilder;
    stdx::unordered_set<ShardId> _currentlyTargetedDataShards;

    // Will be set to true if a request was made to open a cursor on the config server.
    bool _cursorOpenedOnConfigServer = false;

    // IF set to 'true', any attempt to open a cursor will throw a 'ShardNotFound' exception.
    bool _throwShardNotFoundException = false;

    // Resume token used when initializing the 'CursorManager'.
    boost::optional<ResumeToken> _resumeToken;

    boost::optional<Timestamp> _highWaterMarkTimestamp;

    // Calls to enable/disable undo mode will be recorded here.
    boost::optional<bool> _undoNextMode;

    // The timestamp used in a call to 'undoGetNextAndSetHighWaterMark()' will be recorded here.
    boost::optional<Timestamp> _undoNextHighWaterMark;
};

class DeadlineWaiterMock : public V2Stage::DeadlineWaiter {
public:
    void waitUntil(OperationContext* opCtx, Date_t deadline) override {
        _lastUsedDeadline = deadline;
        if (!_status.isOK()) {
            error_details::throwExceptionForStatus(_status);
        }
    }

    Date_t getLastUsedDeadline() const {
        return _lastUsedDeadline;
    }

    void setStatus(Status status) {
        _status = status;
    }

private:
    Status _status = Status::OK();
    Date_t _lastUsedDeadline;
};

// Helper functions used in the tests.
// -----------------------------------

BSONObj buildHighWaterMarkToken(Timestamp ts) {
    return ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */).toDocument().toBson();
}

DocumentSourceChangeStreamSpec buildChangeStreamSpec(Timestamp ts, ChangeStreamReadMode mode) {
    DocumentSourceChangeStreamSpec spec;
    spec.setIgnoreRemovedShards(mode == ChangeStreamReadMode::kIgnoreRemovedShards);
    spec.setResumeAfter(ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */));
    return spec;
}

BSONObj getStageSpec() {
    return BSON(DocumentSourceChangeStreamHandleTopologyChangeV2::kStageName << BSONObj());
}

std::shared_ptr<V2Stage::Parameters> buildParametersForTest(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    int minAllocationToShardsPollPeriodSecs,
    ChangeStreamReaderBuilder* changeStreamReaderBuilder,
    DataToShardsAllocationQueryService* dataToShardsAllocationQueryService) {
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(expCtx);
    return std::make_shared<V2Stage::Parameters>(
        changeStream,
        change_stream::resolveResumeTokenFromSpec(expCtx, *expCtx->getChangeStreamSpec()),
        minAllocationToShardsPollPeriodSecs,
        std::make_unique<DeadlineWaiterMock>(),
        std::make_unique<CursorManagerMock>(changeStream, changeStreamReaderBuilder),
        changeStreamReaderBuilder,
        dataToShardsAllocationQueryService);
}

DataToShardsAllocationQueryServiceMock* getDataToShardsAllocationQueryServiceMock(
    std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<DataToShardsAllocationQueryServiceMock*>(
        params->dataToShardsAllocationQueryService);
}

ChangeStreamReaderBuilderMock* getChangeStreamReaderBuilderMock(
    std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<ChangeStreamReaderBuilderMock*>(params->changeStreamReaderBuilder);
}

ChangeStreamShardTargeterMock* getChangeStreamShardTargeterMock(
    std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<ChangeStreamShardTargeterMock*>(
        getChangeStreamReaderBuilderMock(params)->getShardTargeter());
}

CursorManagerMock* getCursorManagerMock(std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<CursorManagerMock*>(params->cursorManager.get());
}

DeadlineWaiterMock* getDeadlineWaiterMock(std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<DeadlineWaiterMock*>(params->deadlineWaiter.get());
}

ClockSourceMock* getPreciseClockSource(ServiceContext* serviceContext) {
    return dynamic_cast<ClockSourceMock*>(serviceContext->getPreciseClockSource());
}

// Common stage property tests.
// ----------------------------

TEST_F(ChangeStreamStageTest, DSV2CreateFromInvalidInput) {
    // Test invalid top-level BSON type.
    // The stage parameters are not be needed for this.
    const BSONObj spec =
        BSON(DocumentSourceChangeStreamHandleTopologyChangeV2::kStageName << BSON("foo" << "bar"));
    ASSERT_THROWS_CODE(DocumentSourceChangeStreamHandleTopologyChangeV2::createFromBson(
                           spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10612600);
}

TEST_F(ChangeStreamStageTest, DSV2HandleInputs) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(ts,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    // Test that the stage returns all inputs as they are.
    const BSONObj doc1 = BSON("operationType" << "test1" << "foo" << "bar");
    const BSONObj doc2 = BSON("operationType" << "test2" << "test" << "value");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc1),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc2),
        DocumentSource::GetNextResult::makeEOF(),
    };

    auto stage = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(stage.get());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isPaused());

    next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(doc1), next.getDocument());

    next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isPaused());

    next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(doc2), next.getDocument());

    next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(ChangeStreamStageTest, DSV2Serialization) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // The only valid spec for this stage is an empty object.
    const BSONObj spec = getStageSpec();
    auto handleTopologyChangeStage =
        DocumentSourceChangeStreamHandleTopologyChangeV2::createFromBson(spec.firstElement(),
                                                                         getExpCtx());

    std::vector<Value> serialization;
    handleTopologyChangeStage->serializeToArray(serialization);

    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);
    ASSERT_BSONOBJ_EQ(
        serialization[0].getDocument().toBson(),
        BSON(DocumentSourceChangeStreamHandleTopologyChangeV2::kStageName << BSONObj()));
}

TEST_F(ChangeStreamStageTestNoSetup, DSV2EmptyForQueryStats) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));

    auto docSource = DocumentSourceChangeStreamHandleTopologyChangeV2::create(getExpCtx());

    ASSERT_BSONOBJ_EQ(  // NOLINT
        getStageSpec(),
        docSource->serialize().getDocument().toBson());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

// State and state transition tests.
// ---------------------------------

// This empty class has no additional functionality compared the generic change stream stage test,
// but we use it here to make the DSV2 stage tests accessible in their own namespace.
class DSV2StageTest : public ChangeStreamStageTestNoSetup {};

// Invalid state transition tests.
// -------------------------------

// Tests that a previous exception must have been registered when running the state machine when the
// start state is kFinal.
using DSV2StageTestDeathTest = DSV2StageTest;
DEATH_TEST_REGEX_F(DSV2StageTestDeathTest,
                   StateMachineFailsOnStateFinal,
                   "Tripwire assertion.*10657532") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kFinal, false /* validateStateTransition */);
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657532);
}

// Tests that an exception is thrown when the state is set to the existing state using 'setState()'
// / 'setState_forTest()'.
DEATH_TEST_REGEX_F(DSV2StageTestDeathTest, CheckRepeatedState, "Tripwire assertion.*10657503") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), nullptr);

    // Check state invalid transition from Final to kWaiting to kWaiting.
    auto state = V2Stage::State::kWaiting;
    docSource->setState_forTest(state, false /* validateStateTransition */);

    ASSERT_THROWS_CODE(docSource->setState_forTest(state, true /* validateStateTransition */),
                       AssertionException,
                       10657503);
}

// Tests that an exception is thrown when trying to change the state from the end state kFinal to
// another state.
DEATH_TEST_REGEX_F(DSV2StageTestDeathTest,
                   CheckStateTransitionBackFromFinalState,
                   "Tripwire assertion.*10657504") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), nullptr);

    docSource->setState_forTest(V2Stage::State::kFinal, false /* validateStateTransition */);

    for (auto state : {V2Stage::State::kWaiting,
                       V2Stage::State::kFetchingInitialization,
                       V2Stage::State::kDowngrading}) {
        ASSERT_THROWS_CODE(docSource->setState_forTest(state, true /* validateStateTransition */),
                           AssertionException,
                           10657504);
    }
}

// Tests that an exception is thrown when trying to set the state back to kUninitialized.
DEATH_TEST_REGEX_F(DSV2StageTestDeathTest,
                   CheckStateTransitionBackToUninitialized,
                   "Tripwire assertion.*10657505") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    for (auto state : {V2Stage::State::kWaiting,
                       V2Stage::State::kFetchingInitialization,
                       V2Stage::State::kDowngrading}) {
        docSource->setState_forTest(state, false /* validateStateTransition */);

        ASSERT_THROWS_CODE(docSource->setState_forTest(V2Stage::State::kUninitialized,
                                                       true /* validateStateTransition */),
                           AssertionException,
                           10657505);
    }
}

// State tests.
// ------------

// Tests state machine for input state kUninitialized, for a cluster time for which there is no
// data-to-shards allocation information present. The change stream is expected to fail in this
// case.
TEST_F(DSV2StageTest, StateUninitializedAllocationNotAvailable) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(
        std::make_pair(Timestamp(23, 0), AllocationToShardsStatus::kNotAvailable));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kUninitialized,
                                false /* validateStateTransition */);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    ASSERT_FALSE(getCursorManagerMock(params)->isInitialized());

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    // Cursor manager should not have been initialized.
    ASSERT_FALSE(getCursorManagerMock(params)->isInitialized());

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());

    // Last request time should have been updated due to the query-to-shards-allocation request.
    ASSERT_EQ(now.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());
}

// Tests state machine for input state kUninitialized, for a cluster time for which there is
// data-to-shards allocation information present. The state machine is supposed to go into state
// kFetchingInitialization.
TEST_F(DSV2StageTest, StateUninitializedAllocationOk) {
    const Timestamp ts = Timestamp(23, 0);
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    // Timestamp of ResumeToken is in the past. We simulate that no data-to-shards allocation is
    // available anymore.
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kUninitialized,
                                false /* validateStateTransition */);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);
    ASSERT_FALSE(getCursorManagerMock(params)->isInitialized());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingInitialization, docSource->getState_forTest());

    // Cursor manager should have been initialized.
    ASSERT_TRUE(getCursorManagerMock(params)->isInitialized());
    ASSERT_BSONOBJ_EQ(ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */).toBSON(),
                      getCursorManagerMock(params)->getResumeToken().toBSON());

    // Last request time should have been updated due to the query-to-shards-allocation request.
    ASSERT_EQ(now.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());
}

// Tests state machine for input state kUninitialized, for a cluster time which is in the future.
// The state machine is supposed to go into state kWaiting.
TEST_F(DSV2StageTest, StateUninitializedAllocationFutureClusterTime) {
    const Timestamp ts = Timestamp(42, 23);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    // Timestamp of ResumeToken is in the past. We simulate that no data-to-shards allocation is
    // available anymore.
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kFutureClusterTime));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kUninitialized,
                                false /* validateStateTransition */);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    ASSERT_FALSE(getCursorManagerMock(params)->isInitialized());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());

    // Cursor manager should have been initialized.
    ASSERT_TRUE(getCursorManagerMock(params)->isInitialized());
    ASSERT_BSONOBJ_EQ(ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */).toBSON(),
                      getCursorManagerMock(params)->getResumeToken().toBSON());

    // Last request time should have been updated due to the query-to-shards-allocation request.
    ASSERT_EQ(now.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());
}

// Tests state machine for input state kWaiting, when the data-to-shards allocation query service
// returns that no placement information is available.
TEST_F(DSV2StageTest, StateWaitingNoPlacementInfoAvailable) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(ts,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(
        getExpCtx(), 10, changeStreamReaderBuilder.get(), dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kNotAvailable));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Set deadline to a few milliseconds in the future.
    Date_t deadline = now + Milliseconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());

    // Calling 'getNext()' again must return the same pre-recorded error:
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when the data-to-shards allocation query service
// returns that the cluster time is still in the future.
TEST_F(DSV2StageTest, StateWaitingFutureClusterTime) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(ts,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kFutureClusterTime));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Set deadline to a few milliseconds in the future.
    Date_t deadline = now + Milliseconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());

    // State should remain in kWaiting, as no progress has been made.
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when the data-to-shards allocation query service
// returns that the allocation is available.
TEST_F(DSV2StageTest, StateWaitingTransitioningToFetching) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(ts,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* allocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Set deadline to a few milliseconds in the future.
    Date_t deadline = now + Milliseconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // State should have transitioned to kFetchingInitialization.
    ASSERT_EQ(V2Stage::State::kFetchingInitialization, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when the deadline for the next data-to-shards
// allocation query is earlier than the deadline on the OperationContext.
TEST_F(DSV2StageTest, StateWaitingBehaviorPollBeforeOperationContextDeadline) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    Date_t lastAllocationToShardsRequestTime = now - Seconds(2);
    docSource->setLastAllocationToShardsRequestTime_forTest(lastAllocationToShardsRequestTime);
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // Set deadline to further away than the next poll date/time.
    Date_t deadline = now + Seconds(params->minAllocationToShardsPollPeriodSecs);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // The waiter should have been called with a deadline that is equal to now plus the poll period.
    ASSERT_EQ(
        (lastAllocationToShardsRequestTime + Seconds(params->minAllocationToShardsPollPeriodSecs))
            .toMillisSinceEpoch(),
        getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());

    // Last request time shouldn't have been modified.
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when the deadline for the next data-to-shards
// allocation query is later than the deadline on the OperationContext.
TEST_F(DSV2StageTest, StateWaitingBehaviorPollAfterOperationContextDeadline) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    Date_t lastAllocationToShardsRequestTime = now - Seconds(2);
    docSource->setLastAllocationToShardsRequestTime_forTest(lastAllocationToShardsRequestTime);
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // Set deadline to earlier than the next poll date/time.
    Date_t deadline = now + Seconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // The waiter should have been called with a deadline that is equal to the next poll time.
    ASSERT_EQ(
        (lastAllocationToShardsRequestTime + Seconds(params->minAllocationToShardsPollPeriodSecs))
            .toMillisSinceEpoch(),
        getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());

    // Last request time shouldn't have been modified.
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when we wait on the OperationContext and the wait
// function returns a timeout error status.
TEST_F(DSV2StageTest, StateWaitingBehaviorWhenWaitReturnsTimeoutError) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    Date_t lastAllocationToShardsRequestTime = now;
    docSource->setLastAllocationToShardsRequestTime_forTest(lastAllocationToShardsRequestTime);
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // Set arbitrary deadline.
    Date_t deadline = now + Seconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    // Make waiting fail with a non-OK status.
    getDeadlineWaiterMock(params)->setStatus(
        Status(ErrorCodes::ExceededTimeLimit, "timelimit exceeded!"));

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());

    // The waiter should have been called with a deadline that is equal to the next poll time.
    ASSERT_EQ((now + Seconds(params->minAllocationToShardsPollPeriodSecs)).toMillisSinceEpoch(),
              getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());

    // Last request time shouldn't have been modified.
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // State should not have changed.
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when we wait on the OperationContext and the wait
// function returns a non-timeout error status.
TEST_F(DSV2StageTest, StateWaitingBehaviorWhenWaitReturnsNonTimeoutError) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    Date_t lastAllocationToShardsRequestTime = now;
    docSource->setLastAllocationToShardsRequestTime_forTest(lastAllocationToShardsRequestTime);
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // Set arbitrary deadline.
    Date_t deadline = now + Seconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    // Make waiting fail with a non-OK status.
    getDeadlineWaiterMock(params)->setStatus(Status(ErrorCodes::ShutdownInProgress, "shutdown!"));

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::ShutdownInProgress);

    // The waiter should have been called with a deadline that is equal to the next poll time.
    ASSERT_EQ((now + Seconds(params->minAllocationToShardsPollPeriodSecs)).toMillisSinceEpoch(),
              getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());

    // Last request time shouldn't have been modified.
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // State should have transitioned to kFinal, as waiting threw an exception.
    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization, when the shard targeter returns
// kContinue. Expects the state to transition to kFetchingGettingChangeEvent.
TEST_F(DSV2StageTest, StateFetchingInitializationStrictModeContinue) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingInitialization,
                                false /* validateStateTransition */);

    // Assuming not to have segment start or end timestamps before calling the state machine.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // Assuming not to have segment start or end timestamps after the call because we are in strict
    // mode.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization, when the shard targeter returns
// kSwitchToV1. Expects the state to transition to kDowngrading.
TEST_F(DSV2StageTest, StateFetchingInitializationStrictModeSwitchToV1) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kSwitchToV1,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingInitialization,
                                false /* validateStateTransition */);

    // Assuming not to have a segment start timestamp before calling the state machine.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());

    // Assuming not to have segment start or end timestamps after the call because we are in strict
    // mode.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kDowngrading, docSource->getState_forTest());

    // When state is kDowngrading, the expected result upon next state machine invocation is an
    // exception.
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization for non-control events.
TEST_F(DSV2StageTest, StateFetchingInitializationStrictModeGettingChangeEventNonControlEvents) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingGettingChangeEvent,
                                false /* validateStateTransition */);

    // Test that the stage returns all inputs as they are.
    const BSONObj doc1 = BSON("operationType" << "test1" << "foo" << "bar");
    const BSONObj doc2 = BSON("operationType" << "test2" << "test" << "value");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc1),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc2),
        DocumentSource::GetNextResult::makeEOF(),
    };

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());

    // Check return value 1 (pause).
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    // Check return value 2 (doc1).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc1, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    // Check return value 3 (pause).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    // Check return value 4 (doc2).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc2, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    // Check return value 5 (eof).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization for a control event.
TEST_F(DSV2StageTest, StateFetchingInitializationStrictModeGettingChangeEventWithControlEvent) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    const BSONObj event = BSON("operationType" << "test" << "foo" << "bar"
                                               << Document::metaFieldChangeStreamControlEvent << 1);

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Document{event},
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingGettingChangeEvent,
                                false /* validateStateTransition */);

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(
            Document::fromBsonWithMetaData(event))};

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization in ignoreRemovedShards mode. This is
// supposed to set the start time of the change stream segment and transition to state
// kFetchingStartingChangeStreamSegment.
TEST_F(DSV2StageTest, StateFetchingInitializationIgnoreRemovedShards) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingInitialization,
                                false /* validateStateTransition */);

    // Assuming not to have segment start or end timestamps before calling the state machine.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // After calling the state machine, the segment start timestamp must be set to the resume
    // token's timestamp.
    const auto& segmentStartTimestamp = docSource->getSegmentStartTimestamp_forTest();
    ASSERT_TRUE(segmentStartTimestamp.has_value());
    ASSERT_EQ(ts, *segmentStartTimestamp);

    // Still assuming no end timestamp.
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, without the segment
// start timestamp being set.
DEATH_TEST_REGEX_F(DSV2StageTestDeathTest,
                   StateFetchingStartingChangeStreamSegmentWithoutStartTimestamp,
                   "Tripwire assertion.*10657518") {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    // Intentionally do not set the segment start timestamp before entering the state to trigger the
    // following tassert.
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657518);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when the shard targeter
// returns kSwitchToV1.
TEST_F(DSV2StageTest, StateFetchingStartingChangeStreamSegmentSwitchToV1) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kSwitchToV1,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    docSource->setSegmentStartTimestamp_forTest(ts);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());

    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kDowngrading, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when the shard targeter
// returns kContinue and no end timestamp for the segment.
TEST_F(DSV2StageTest, StateFetchingStartingChangeStreamSegmentContinueWithoutEndTimestamp) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    docSource->setSegmentStartTimestamp_forTest(ts);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when the shard targeter
// returns kContinue and an end timestamp for the segment.
TEST_F(DSV2StageTest, StateFetchingStartingChangeStreamSegmentContinueAndEndTimestamp) {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(42, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kContinue,
                                        segmentEndTimestamp,
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    docSource->setSegmentStartTimestamp_forTest(ts);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    ASSERT_TRUE(docSource->getSegmentEndTimestamp_forTest().has_value());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());

    // Undo mode must have been turned on when entering the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when we try to open a
// new cursor on a shard and this fails with 'ShardNotFound' exceptions.
TEST_F(DSV2StageTest, StateFetchingStartingChangeStreamSegmentOpenCursorFailsWithShardNotFound) {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(42, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    ChangeStreamShardTargeterMock::Response shardTargeterResponse(
        Timestamp(23, 0),
        ShardTargeterDecision::kContinue,
        segmentEndTimestamp,
        [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
           ChangeStreamReaderContext& readerContext) {
            readerContext.openCursorsOnDataShards(std::get<Timestamp>(tsOrDoc),
                                                  stdx::unordered_set<ShardId>{{"shardA"}});
        });
    // Add the same response twice, as we will make the cursor manager throw an exception upon the
    // first attempt, and the shard targeter will be asked again.
    shardTargeterResponses.push_back(shardTargeterResponse);
    shardTargeterResponses.push_back(shardTargeterResponse);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    docSource->setSegmentStartTimestamp_forTest(ts);

    // Makes cursor manager throw a 'ShardNotFound' exception when trying to open a cursor.
    getCursorManagerMock(params)->setThrowShardNotFoundException(true);
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // State should not have changed!
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    // Remove exception throwing again.
    getCursorManagerMock(params)->setThrowShardNotFoundException(false);
    result = docSource->runGetNextStateMachine_forTest();
    // boost::none is returned because of the state transition.
    ASSERT_FALSE(result.has_value());

    ASSERT_TRUE(docSource->getSegmentEndTimestamp_forTest().has_value());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());

    // Undo mode must have been turned on when entering the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when we try to open a
// new cursor on a shard and this fails with 'ShardNotFound' exceptions repeatedly until the max
// number of consecutive failures is reached.
DEATH_TEST_REGEX_F(
    DSV2StageTestDeathTest,
    StateFetchingStartingChangeStreamSegmentOpenCursorFailsWithShardNotFoundRepeatedly,
    "Tripwire assertion.*10657541") {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;

    // Insert as many responses as we need to nudge the stage into triggering the tassert for too
    // many consecutive "ShardNotFound" errors.
    for (int i = 0; i <= V2Stage::kMaxShardNotFoundFailuresInARow; ++i) {
        shardTargeterResponses.emplace_back(
            ts,
            ShardTargeterDecision::kContinue,
            boost::optional<Timestamp>{},
            [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
               ChangeStreamReaderContext& readerContext) {
                readerContext.openCursorsOnDataShards(std::get<Timestamp>(tsOrDoc),
                                                      stdx::unordered_set<ShardId>{{"shardA"}});
            });
    }

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(ts);

    getCursorManagerMock(params)->setThrowShardNotFoundException(true);
    for (int i = 0; i < V2Stage::kMaxShardNotFoundFailuresInARow; ++i) {
        auto result = docSource->runGetNextStateMachine_forTest();
        ASSERT_FALSE(result.has_value());

        ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment,
                  docSource->getState_forTest());
        ASSERT_EQ(boost::optional<Timestamp>(), docSource->getSegmentEndTimestamp_forTest());
    }

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657541);
}

// Tests state machine for input state kFetchingNormalGettingChangeEvent for non-control events.
TEST_F(DSV2StageTest, StateFetchingNormalGettingChangeEventNonControlEvents) {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(42, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kContinue,
                                        segmentEndTimestamp,
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    // Test that the stage returns all inputs as they are.
    const BSONObj doc1 = BSON("operationType" << "test1" << "foo" << "bar");
    const BSONObj doc2 = BSON("operationType" << "test2" << "test" << "value");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc1),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc2),
        DocumentSource::GetNextResult::makeEOF(),
    };

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());

    // Check return value 1 (pause).
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 2 (doc1).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc1, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 3 (pause).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 4 (doc2).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc2, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 5 (eof).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetching for a control event.
TEST_F(DSV2StageTest, StateFetchingNormalGettingChangeEventControlEvent) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event = BSON("operationType" << "test" << "foo" << "bar" << "_id"
                                               << buildHighWaterMarkToken(Timestamp(23, 1))
                                               << Document::metaFieldChangeStreamControlEvent << 1);

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Document{event},
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(
            Document::fromBsonWithMetaData(event))};

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetching for a control event, when we try to open a new
// cursor on a shard and this fails with 'ShardNotFound' exceptions.
TEST_F(DSV2StageTest,
       StateFetchingNormalGettingChangeEventControlEventOpenCursorFailsWithShardNotFound) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event = BSON("operationType" << "test" << "foo" << "bar" << "_id"
                                               << buildHighWaterMarkToken(Timestamp(23, 1))
                                               << Document::metaFieldChangeStreamControlEvent << 1);

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        Document{event},
        ShardTargeterDecision::kContinue,
        boost::optional<Timestamp>{},
        [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
           ChangeStreamReaderContext& readerContext) {
            readerContext.openCursorsOnDataShards(
                V2Stage::extractTimestampFromDocument(std::get<Document>(tsOrDoc)),
                stdx::unordered_set<ShardId>{{"shardA"}});
        });

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(
            Document::fromBsonWithMetaData(event))};

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());

    getCursorManagerMock(params)->setThrowShardNotFoundException(true);
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 2), *docSource->getSegmentEndTimestamp_forTest());

    // Undo mode must have been turned on when entering the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());

    // Disable exceptions again. Calling the state machine will start a new segment.
    getCursorManagerMock(params)->setThrowShardNotFoundException(false);
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 3));
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 2), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>(), docSource->getSegmentEndTimestamp_forTest());
}

// Tests state machine for input state kFetchingNormalGettingChangeEvent and the shard targeter
// returning 'kSwitchToV1'.
TEST_F(DSV2StageTest, StateFetchingNormalGettingChangeEventShardTargeterReturnsDowngrading) {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(23, 99);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event =
        BSON("operationType" << "test1" << "foo" << "bar" << "clusterTime" << Timestamp(23, 2));

    MutableDocument docBuilder(Document{event});
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc = docBuilder.freeze();

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(doc,
                                        ShardTargeterDecision::kSwitchToV1,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(std::move(doc))};

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kDowngrading, docSource->getState_forTest());

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);
    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent for non-control events.
TEST_F(DSV2StageTest, StateFetchingDegradedGettingChangeEventNonControlEvents) {
    // The change stream segments in this test are [ts(23, 0), ts(42, 1)) and [ts(42, 1), inf).
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(42, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(segmentEndTimestamp,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);

    // Test that the stage returns all inputs as they are.
    const BSONObj doc1 = BSON("operationType" << "test1" << "foo" << "bar" << "_id"
                                              << buildHighWaterMarkToken(Timestamp(23, 1)));
    const BSONObj doc2 = BSON("operationType" << "test2" << "test" << "value" << "_id"
                                              << buildHighWaterMarkToken(Timestamp(42, 1)));
    const BSONObj doc3 = BSON("operationType" << "test3" << "test" << "value" << "_id"
                                              << buildHighWaterMarkToken(Timestamp(43, 1)));

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc1),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc2),
        Document::fromBsonWithMetaData(doc3),
        DocumentSource::GetNextResult::makeEOF(),
    };

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());
    docSource->setSegmentStartTimestamp_forTest(ts);
    docSource->setSegmentEndTimestamp_forTest(segmentEndTimestamp);

    // Check return value 1 (pause).
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 1));
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    // Check return value 2 (doc1).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc1, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    // Check return value 3 (pause).
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 2));
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    // Check return value 4 (doc2). This also transitions the state.
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(42, 1));
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc2, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());

    // Segment start timestamp should change here.
    ASSERT_EQ(Timestamp(42, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // 'undoNextReady()' should have been called on the 'CursorManager' for this transition.
    ASSERT_EQ(Timestamp(42, 1), *getCursorManagerMock(params)->getUndoGetNextHighWaterMark());

    // Undo mode must have been turned off when exiting the degraded fetching state.
    ASSERT_FALSE(*getCursorManagerMock(params)->getUndoNextMode());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(42, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // Check return value 5 (doc3).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_BSONOBJ_EQ(doc3, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(42, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // Check return value 6 (eof).
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(43, 2));
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(42, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent for Pause and EOF events.
TEST_F(DSV2StageTest, StateFetchingDegradedGettingChangeEventPauseAndEOFEvents) {
    // The change stream segments in this test are [ts(23, 0), ts(23, 1)) and [ts(23, 1), inf).
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(23, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(segmentEndTimestamp,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makePauseExecution(),
        DocumentSource::GetNextResult::makeEOF(),
    };

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());
    docSource->setSegmentStartTimestamp_forTest(ts);
    docSource->setSegmentEndTimestamp_forTest(segmentEndTimestamp);

    // Check return value 1 (pause).
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 1));
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());

    // Segment start timestamp should change here.
    ASSERT_EQ(Timestamp(23, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>(), docSource->getSegmentEndTimestamp_forTest());

    // 'undoNextReady()' should have been called on the 'CursorManager' for this transition.
    ASSERT_EQ(Timestamp(23, 1), *getCursorManagerMock(params)->getUndoGetNextHighWaterMark());

    // Undo mode must have been turned off when exiting the degraded fetching state.
    ASSERT_FALSE(*getCursorManagerMock(params)->getUndoNextMode());

    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 2));
    // Transitions from starting change stream segment to degraded mode.
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 2 (eof).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent for control events.
TEST_F(DSV2StageTest, StateFetchingDegradedGettingChangeEventControlEvent) {
    // The change stream segments in this test are [ts(23, 0), ts(23, 99)) and [ts(23, 99), inf).
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(23, 99);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event1 = BSON("operationType" << "test1" << "foo" << "bar" << "_id"
                                                << buildHighWaterMarkToken(Timestamp(23, 2)));
    const BSONObj event2 = BSON("operationType" << "test2" << "foo" << "bar" << "_id"
                                                << buildHighWaterMarkToken(Timestamp(24, 0)));

    MutableDocument docBuilder(Document{event1});
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc1 = docBuilder.freeze();

    docBuilder.reset(Document{event2});
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc2 = docBuilder.freeze();

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(doc1,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    shardTargeterResponses.emplace_back(doc2,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    shardTargeterResponses.emplace_back(Timestamp(23, 99),
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(std::move(doc1)),
        DocumentSource::GetNextResult::makeAdvancedControlDocument(std::move(doc2))};

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());
    docSource->setSegmentStartTimestamp_forTest(ts);
    docSource->setSegmentEndTimestamp_forTest(segmentEndTimestamp);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 99), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // 'undoNextReady()' should have been called on the 'CursorManager' for this transition.
    ASSERT_EQ(Timestamp(23, 99), *getCursorManagerMock(params)->getUndoGetNextHighWaterMark());

    // Undo mode must have been turned off when exiting the degraded fetching state.
    ASSERT_FALSE(*getCursorManagerMock(params)->getUndoNextMode());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 99), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent and the shard targeter
// returning 'kSwitchToV1'.
TEST_F(DSV2StageTest, StateFetchingDegradedGettingChangeEventShardTargeterReturnsDowngrading) {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(23, 99);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event = BSON("operationType" << "test1" << "foo" << "bar" << "_id"
                                               << buildHighWaterMarkToken(Timestamp(23, 2)));

    MutableDocument docBuilder(Document{event});
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc = docBuilder.freeze();

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(doc,
                                        ShardTargeterDecision::kSwitchToV1,
                                        boost::optional<Timestamp>{},
                                        kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(std::move(doc))};

    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    docSource->setSource(source.get());
    docSource->setSegmentStartTimestamp_forTest(ts);
    docSource->setSegmentEndTimestamp_forTest(segmentEndTimestamp);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kDowngrading, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    // Undo mode must have been turned off when exiting the degraded fetching state.
    ASSERT_FALSE(*getCursorManagerMock(params)->getUndoNextMode());

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);
    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kDowngrading. The change stream is expected to fail with an
// error in this case.
TEST_F(DSV2StageTest, StateDowngrading) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kDowngrading, false /* validateStateTransition */);

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);
    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests opening cursor on the config server.
TEST_F(DSV2StageTest, OpenCursorOnConfigServer) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            Timestamp openTs = std::get<Timestamp>(tsOrDocument);
            ASSERT_EQ(ts, openTs);
            context.openCursorOnConfigServer(ts);
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(source.get());

    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Cursor must have been opened on the config server and no data shards.
    ASSERT_TRUE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());
}

// Tests closing cursor on the config server.
TEST_F(DSV2StageTest, CloseCursorOnConfigServer) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
           ChangeStreamReaderContext& context) {
            context.closeCursorOnConfigServer();
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(source.get());

    getCursorManagerMock(params)->openCursorOnConfigServer(getExpCtx(), getOpCtx(), ts);
    ASSERT_TRUE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Cursor must have been closed on the config server.
    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());
}

// Tests opening cursors on data shards.
TEST_F(DSV2StageTest, OpenCursorsOnDataShards) {
    const Timestamp ts = Timestamp(42, 0);
    const stdx::unordered_set<ShardId> shardSet = {ShardId("abc"), ShardId("xyz")};

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            Timestamp openTs = std::get<Timestamp>(tsOrDocument);
            ASSERT_EQ(ts, openTs);
            context.openCursorsOnDataShards(openTs, shardSet);
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(source.get());

    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Cursor must have been opened on the data shards, but not the config server.
    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(shardSet, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());
}

// Tests closing cursors on data shards.
TEST_F(DSV2StageTest, CloseCursorsOnDataShards) {
    const Timestamp ts = Timestamp(42, 0);
    const stdx::unordered_set<ShardId> shardSet = {ShardId("abc"), ShardId("xyz")};

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            context.closeCursorsOnDataShards(shardSet);
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(source.get());
    getCursorManagerMock(params)->openCursorsOnDataShards(getExpCtx(), getOpCtx(), ts, shardSet);

    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(shardSet, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Cursor must have been closed on the data shards, and remain closed on the config server.
    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());
}

// Tests that a 'ShardNotFound' error is converted to a 'ShardRemovedError' in strict mode.
TEST_F(DSV2StageTest, ShardNotFoundErrorIsConvertedToShardRemovedErrorInStrictMode) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            error_details::throwExceptionForStatus(
                Status(ErrorCodes::ShardNotFound, "did not find shard!"));
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(source.get());

    // 'ShardNotFound' error must be translated into 'ShardRemovedError'.
    ASSERT_THROWS_CODE(
        handleTopologyChangeStage->getNext(), AssertionException, ErrorCodes::ShardRemovedError);

    ASSERT_EQ(V2Stage::State::kFinal, handleTopologyChangeStage->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(
        handleTopologyChangeStage->getNext(), AssertionException, ErrorCodes::ShardRemovedError);
}

// Tests that a 'ShardNotFound' error is converted to a 'RetryChangeStream' error in
// ignoreRemovedShards mode.
TEST_F(DSV2StageTest, ShardNotFoundErrorIsConvertedToRetryChangeStreamInIgnoreRemovedShardsMode) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            error_details::throwExceptionForStatus(
                Status(ErrorCodes::ShardNotFound, "did not find shard!"));
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(source.get());

    // 'ShardNotFound' error must be translated into 'RetryChangeStream' error.
    ASSERT_THROWS_CODE(
        handleTopologyChangeStage->getNext(), AssertionException, ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, handleTopologyChangeStage->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(
        handleTopologyChangeStage->getNext(), AssertionException, ErrorCodes::RetryChangeStream);
}

// Tests that any non-'ShardNotFound' error is rethrown as is in strict mode.
TEST_F(DSV2StageTest, ErrorOtherThanShardNotFoundErrorIsRethrownInStrictMode) {
    const Timestamp ts = Timestamp(42, 0);
    const auto errorCode = ErrorCodes::ChangeStreamFatalError;

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            error_details::throwExceptionForStatus(Status(errorCode, "some error occurred"));
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(source.get());

    // The expect error code should be returned as-is.
    ASSERT_THROWS_CODE(handleTopologyChangeStage->getNext(), AssertionException, errorCode);

    ASSERT_EQ(V2Stage::State::kFinal, handleTopologyChangeStage->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(handleTopologyChangeStage->getNext(), AssertionException, errorCode);
}

// Tests that any non-'ShardNotFound' error is rethrown as is in ignoreRemovedShards mode.
TEST_F(DSV2StageTest, ErrorOtherThanShardNotFoundErrorIsRethrownInIgnoreRemovedShardsMode) {
    const Timestamp ts = Timestamp(42, 0);
    const auto errorCode = ErrorCodes::ChangeStreamHistoryLost;

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            error_details::throwExceptionForStatus(Status(errorCode, "some error occurred"));
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         kDefaultMinAllocationToShardsPollPeriodSecs,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = exec::agg::MockStage::createForTest(inputDocs, getExpCtx());
    handleTopologyChangeStage->setSource(source.get());

    // The expect error code should be returned as-is.
    ASSERT_THROWS_CODE(handleTopologyChangeStage->getNext(), AssertionException, errorCode);

    ASSERT_EQ(V2Stage::State::kFinal, handleTopologyChangeStage->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(handleTopologyChangeStage->getNext(), AssertionException, errorCode);
}

// Tests pipeline building for config server cursor.
TEST_F(DSV2StageTest, BuildPipelineForConfigServerV2) {
    const Timestamp ts = Timestamp(123, 45);

    DocumentSourceChangeStreamSpec spec = buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict);
    spec.setVersion(ChangeStreamReaderVersionEnum::kV2);
    getExpCtx()->setChangeStreamSpec(spec);
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) { return nullptr; },
        [](OperationContext*, const ChangeStream&) { return BSONObj(); },
        [](OperationContext*, const ChangeStream&) { return std::set<std::string>{}; },
        [](OperationContext*, const ChangeStream&) {
            return BSON("$or" << BSON_ARRAY(BSON("o2.eventType1" << BSON("$exists" << true))
                                            << BSON("o2.eventType2" << BSON("$exists" << true))));
        },
        [](OperationContext*, const ChangeStream&) {
            return std::set<std::string>{"eventType1", "eventType2"};
        });

    std::vector<BSONObj> pipeline = change_stream::pipeline_helpers::buildPipelineForConfigServerV2(
        getExpCtx(),
        getOpCtx(),
        ts,
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        changeStream,
        changeStreamReaderBuilder.get());

    ASSERT_EQ(5, pipeline.size());

    // { $_internalChangeStreamOplogMatch: { filter: { $and: [ { ts: { $gte: Timestamp(123, 45) } },
    // { fromMigrate: { $not: { $eq: true } } }, { $or: [ { o2.eventType1: { $exists: true } }, {
    // o2.eventType2: { $exists: true } }, { $and: [ { $or: [ { o.applyOps: { $elemMatch: { $or: [ {
    // o2.eventType1: { $exists: true } }, { o2.eventType2: { $exists: true } } ] } } }, {
    // prevOpTime: { $not: { $eq: { ts: Timestamp(0, 0), t: -1 } } } } ] }, { op: { $eq: "c" } }, {
    // o.partialTxn: { $not: { $eq: true } } }, { o.prepare: { $not: { $eq: true } } }, {
    // o.applyOps: { $type: [ 4 ] } } ] }, { $and: [ { o.commitTransaction: { $eq: 1 } }, { op: {
    // $eq: "c" } } ] } ] } ] } } }
    auto elem = pipeline[0].firstElement();
    ASSERT_EQ("$_internalChangeStreamOplogMatch"_sd, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());
    ASSERT_BSONOBJ_EQ(
        BSON(
            "filter" << BSON(
                "$and" << BSON_ARRAY(
                    BSON("ts" << BSON("$gte" << ts))
                    << BSON("fromMigrate" << BSON("$not" << BSON("$eq" << true)))
                    << BSON(
                           "$or" << BSON_ARRAY(
                               BSON("o2.eventType1" << BSON("$exists" << true))
                               << BSON("o2.eventType2" << BSON("$exists" << true))
                               << BSON("$and" << BSON_ARRAY(
                                           BSON("$or" << BSON_ARRAY(
                                                    BSON("o.applyOps" << BSON(
                                                             "$elemMatch" << BSON(
                                                                 "$or" << BSON_ARRAY(
                                                                     BSON("o2.eventType1" << BSON(
                                                                              "$exists" << true))
                                                                     << BSON("o2.eventType2"
                                                                             << BSON("$exists"
                                                                                     << true))))))
                                                    << BSON("prevOpTime"
                                                            << BSON("$not" << BSON(
                                                                        "$eq" << BSON(
                                                                            "ts" << Timestamp(0, 0)
                                                                                 << "t" << -1))))))
                                           << BSON("op" << BSON("$eq" << "c"))
                                           << BSON("o.partialTxn"
                                                   << BSON("$not" << BSON("$eq" << true)))
                                           << BSON("o.prepare"
                                                   << BSON("$not" << BSON("$eq" << true)))
                                           << BSON("o.applyOps" << BSON("$type" << BSON_ARRAY(4)))))
                               << BSON("$and"
                                       << BSON_ARRAY(BSON("o.commitTransaction" << BSON("$eq" << 1))
                                                     << BSON("op" << BSON("$eq" << "c"))))))))),
        pipeline[0].getField("$_internalChangeStreamOplogMatch").Obj());

    // { $_internalChangeStreamUnwindTransaction: { filter: { $and: [ { fromMigrate: { $not: { $eq:
    // true } } }, { $or: [ { o2.eventType1: { $exists: true } }, { o2.eventType2: { $exists: true }
    // } ] } ] } } }
    elem = pipeline[1].firstElement();
    ASSERT_EQ("$_internalChangeStreamUnwindTransaction"_sd, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());
    ASSERT_BSONOBJ_EQ(
        BSON("filter" << BSON(
                 "$and" << BSON_ARRAY(
                     BSON("fromMigrate" << BSON("$not" << BSON("$eq" << true)))
                     << BSON("$or"
                             << BSON_ARRAY(BSON("o2.eventType1" << BSON("$exists" << true))
                                           << BSON("o2.eventType2" << BSON("$exists" << true))))))),
        pipeline[1].getField("$_internalChangeStreamUnwindTransaction").Obj());

    // { $_internalChangeStreamTransform: { startAtOperationTime: Timestamp(123, 45), fullDocument:
    // "default", fullDocumentBeforeChange: "off", allowToRunOnConfigDB: true, ignoreRemovedShards:
    // false, supportedEvents: [ "eventType1", "eventType2" ] } }
    elem = pipeline[2].firstElement();
    ASSERT_EQ("$_internalChangeStreamTransform"_sd, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());
    ASSERT_BSONOBJ_EQ(BSON("startAtOperationTime"
                           << ts << "fullDocument" << "default" << "fullDocumentBeforeChange"
                           << "off" << "allowToRunOnConfigDB" << true << "supportedEvents"
                           << BSON_ARRAY("eventType1" << "eventType2")),
                      pipeline[2].getField("$_internalChangeStreamTransform").Obj());

    // { $_internalChangeStreamCheckResumability: {}}
    elem = pipeline[3].firstElement();
    ASSERT_EQ("$_internalChangeStreamCheckResumability"_sd, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());

    // { $_internalChangeStreamInjectControlEvents: { actions: { eventType1:
    // "transformToControlEvent", eventType2: "transformToControlEvent" } } }
    elem = pipeline[4].firstElement();
    ASSERT_EQ("$_internalChangeStreamInjectControlEvents"_sd, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());
    ASSERT_BSONOBJ_EQ(
        BSON("actions" << BSON("eventType1" << "transformToControlEvent" << "eventType2"
                                            << "transformToControlEvent")),
        pipeline[4].getField("$_internalChangeStreamInjectControlEvents").Obj());
}

// Tests pipeline building for config server cursor.
DEATH_TEST_REGEX_F(DSV2StageTestDeathTest,
                   BuildPipelineForConfigServerWithV1ExpressionContext,
                   "Tripwire assertion.*10657555") {
    const Timestamp ts = Timestamp(123, 45);

    DocumentSourceChangeStreamSpec spec = buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict);
    spec.setVersion(ChangeStreamReaderVersionEnum::kV1);
    getExpCtx()->setChangeStreamSpec(spec);
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>();

    ASSERT_THROWS_CODE(change_stream::pipeline_helpers::buildPipelineForConfigServerV2(
                           getExpCtx(),
                           getOpCtx(),
                           ts,
                           NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
                           changeStream,
                           changeStreamReaderBuilder.get()),
                       AssertionException,
                       10657555);
}

}  // namespace
}  // namespace mongo
