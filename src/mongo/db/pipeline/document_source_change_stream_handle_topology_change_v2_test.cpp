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
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_stage_test_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

TEST_F(ChangeStreamStageTest, DSCSHandleTopologyChangeV2CreateFromInvalidInput) {
    // Test invalid top-level BSON type.
    BSONObj spec = BSON("$_internalChangeStreamHandleTopologyChangeV2" << BSON("foo" << "bar"));
    ASSERT_THROWS_CODE(DocumentSourceChangeStreamHandleTopologyChangeV2::createFromBson(
                           spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10612600);
}

TEST_F(ChangeStreamStageTest, DSCSHandleTopologyChangeV2HandleInputs) {
    // The only valid spec for this stage is an empty object.
    BSONObj spec = BSON("$_internalChangeStreamHandleTopologyChangeV2" << BSONObj());
    auto handleTopologyChangeStage =
        DocumentSourceChangeStreamHandleTopologyChangeV2::createFromBson(spec.firstElement(),
                                                                         getExpCtx());

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

TEST_F(ChangeStreamStageTest, DSCSHandleTopologyChangeV2Serialization) {
    // The only valid spec for this stage is an empty object.
    BSONObj spec = BSON("$_internalChangeStreamHandleTopologyChangeV2" << BSONObj());
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

TEST_F(ChangeStreamStageTestNoSetup,
       DocumentSourceChangeStreamHandleTopologyChangeV2EmptyForQueryStats) {
    auto docSource = DocumentSourceChangeStreamHandleTopologyChangeV2::create(getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalChangeStreamHandleTopologyChangeV2":{}})",
        docSource->serialize().getDocument().toBson());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

}  // namespace
}  // namespace mongo
