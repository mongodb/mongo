/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_queue.h"

namespace mongo {
namespace {

using QueueStageTest = AggregationContextFixture;


TEST_F(QueueStageTest, QueueStageDeserialization) {
    auto queueDoc = BSON("$queue" << BSON_ARRAY(BSONObj()));
    auto queueStage = DocumentSourceQueue::createFromBson(queueDoc.firstElement(), getExpCtx());
    ASSERT_TRUE(queueStage);

    auto expectedResult = Document{{"a"_sd, 1}};
    auto queueDoc1 = BSON("$queue" << BSON_ARRAY(BSON("a" << 1)));
    auto queueStage1 = DocumentSourceQueue::createFromBson(queueDoc1.firstElement(), getExpCtx());
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

    auto res = queueStage->serialize(boost::none);

    ASSERT_VALUE_EQ(res, Value{DOC("$queue" << DOC_ARRAY(DOC("a1" << 1) << DOC("a2" << 2)))});
}

}  // namespace
}  // namespace mongo
