// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_queue.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using QueueStageTest = AggregationContextFixture;


TEST_F(QueueStageTest, QueueStageDeserialization) {
    auto queueDoc = BSON("$queue" << BSON_ARRAY(BSONObj()));
    auto queueStage = DocumentSourceQueue::createFromBson(queueDoc.firstElement(), getExpCtx());
    ASSERT_TRUE(queueStage);

    auto expectedResult = Document{{"a"sv, 1}};
    auto queueDoc1 = BSON("$queue" << BSON_ARRAY(BSON("a" << 1)));
    auto queueSource1 = DocumentSourceQueue::createFromBson(queueDoc1.firstElement(), getExpCtx());
    auto queueStage1 = exec::agg::buildStage(queueSource1);
    ASSERT_TRUE(queueStage1);
    auto next = queueStage1->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto result = next.releaseDocument();
    ASSERT_DOCUMENT_EQ(result, expectedResult);
    ASSERT(queueStage1->getNext().isEOF());
}

TEST_F(QueueStageTest, QueueStageDeserializationFails) {
    auto queueDoc = BSON("$queue" << BSONObj());
    ASSERT_THROWS_CODE(DocumentSourceQueue::createFromBson(queueDoc.firstElement(), getExpCtx()),
                       AssertionException,
                       5858201);

    auto queueDoc2 = BSON("$queue" << BSON_ARRAY(1 << 2 << 3));
    ASSERT_THROWS_CODE(DocumentSourceQueue::createFromBson(queueDoc2.firstElement(), getExpCtx()),
                       AssertionException,
                       5858202);
}


TEST_F(QueueStageTest, QueueStageSerialize) {
    auto queueStage = DocumentSourceQueue::create(getExpCtx());
    queueStage->emplace_back(DOC("a1" << 1));
    queueStage->emplace_back(DOC("a2" << 2));

    ASSERT_TRUE(queueStage);

    auto res = queueStage->serialize();

    ASSERT_VALUE_EQ(res, Value{DOC("$queue" << DOC_ARRAY(DOC("a1" << 1) << DOC("a2" << 2)))});
}

TEST_F(QueueStageTest, RedactsCorrectly) {
    auto queueDoc = BSON("$queue" << BSON_ARRAY(BSON("a" << 1)));
    auto queueStage = DocumentSourceQueue::createFromBson(queueDoc.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$queue":"?array<?object>"})",
        redact(*queueStage));
}

}  // namespace
}  // namespace mongo
