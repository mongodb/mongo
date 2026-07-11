// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2_test_helpers.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using namespace test;

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
                                        V2StageTestHelpers::kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

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

    auto stage = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               stage);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, stage.get());

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

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
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

    auto opts = query_shape::SerializationOptions{
        .literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

}  // namespace
}  // namespace mongo

