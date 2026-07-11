// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>


namespace mongo {
namespace {

using DocumentSourceInternalApplyOplogUpdateTest = AggregationContextFixture;

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, ShouldBeAbleToReParseSerializedStage) {
    auto spec = fromjson(
        R"({$_internalApplyOplogUpdate: {oplogUpdate: {"$v": NumberInt(2), diff: {u: {b: 3}}}}})");

    auto stage =
        DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(), getExpCtx());

    std::vector<Value> serialization;
    stage->serializeToArray(serialization);
    auto serializedBSON = serialization[0].getDocument().toBson();
    ASSERT_VALUE_EQ(serialization[0], Value(Document(spec)));

    auto roundTripped = DocumentSourceInternalApplyOplogUpdate::createFromBson(
        serializedBSON.firstElement(), getExpCtx());

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, ShouldRejectNonObjectSpecs) {
    {
        auto spec = fromjson("{$_internalApplyOplogUpdate: 1}");
        ASSERT_THROWS_CODE(
            exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                spec.firstElement(), getExpCtx())),
            DBException,
            6315901);
    }

    {
        auto spec = fromjson("{$_internalApplyOplogUpdate: []}");

        ASSERT_THROWS_CODE(
            exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                spec.firstElement(), getExpCtx())),
            DBException,
            6315901);
    }
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, ShouldRejectMalformedSpecs) {
    auto spec = fromjson(
        R"({$_internalApplyOplogUpdate: {
                oplogUpdate: {"$v": NumberInt(999999999), diff: {u: {b: 3}}}
           }})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       4772600);

    spec = fromjson(
        R"({$_internalApplyOplogUpdate: {
            oplogUpdate: {"$v": "2", diff: {u: {b: 3}}}
           }})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       4772600);

    spec = fromjson(R"({$_internalApplyOplogUpdate: {oplogUpdate: {"$v": NumberInt(2)}}})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       4772601);

    spec = fromjson("{$_internalApplyOplogUpdate: {}}");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       ErrorCodes::IDLFailedToParse);

    spec =
        fromjson(R"({$_internalApplyOplogUpdate: {foo: {"$v": NumberInt(2), diff: {u: {b: 3}}}}})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       40415);

    spec = fromjson(
        R"({$_internalApplyOplogUpdate: {
                oplogUpdate: {"$v": NumberInt(2), diff: {u: {b: 3}}},
                foo: 1
           }})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       40415);
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, UpdateMultipleDocuments) {
    auto spec = fromjson(
        R"({$_internalApplyOplogUpdate: {
                oplogUpdate: {"$v": NumberInt(2), diff: {sb: {u: {c: 3}}}}
           }})");

    auto source =
        DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}},
                                                     Document{{"a", 1}, {"b", 1}},
                                                     Document{{"a", 2}, {"b", Document{{"c", 1}}}},
                                                     Document{{"a", 3}, {"b", Document{{"d", 2}}}}},
                                                    getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(source, mock);

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected = Document{{"a", 0}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    expected = Document{{"a", 1}, {"b", BSONNULL}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    expected = Document{{"a", 2}, {"b", Document{{"c", 3}}}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    expected = Document{{"a", 3}, {"b", Document{{"d", 2}, {"c", 3}}}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_TRUE(stage->getNext().isEOF());
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, ShouldErrorOnInvalidDiffs) {
    {
        auto spec = fromjson(
            R"({$_internalApplyOplogUpdate: {
                    oplogUpdate: {"$v": NumberInt(2), diff: {}}
               }})");
        auto source = DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(),
                                                                             getExpCtx());
        auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}}}, getExpCtx());
        auto stage = exec::agg::buildStageAndStitch(source, mock);
        ASSERT_THROWS_CODE(stage->getNext(), DBException, 4770500);
    }

    {
        auto spec = fromjson(
            R"({$_internalApplyOplogUpdate: {
                    oplogUpdate: {"$v": NumberInt(2), diff: {q: {z: -7}}}
               }})");
        auto source = DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(),
                                                                             getExpCtx());
        auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}}}, getExpCtx());
        auto stage = exec::agg::buildStageAndStitch(source, mock);
        ASSERT_THROWS_CODE(stage->getNext(), DBException, 4770503);
    }

    {
        auto spec = fromjson(
            R"({$_internalApplyOplogUpdate: {
                    oplogUpdate: {"$v": NumberInt(2), diff: {"": {z: -7}}}
               }})");
        auto source = DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(),
                                                                             getExpCtx());
        auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}}}, getExpCtx());
        auto stage = exec::agg::buildStageAndStitch(source, mock);
        ASSERT_THROWS_CODE(stage->getNext(), DBException, 4770505);
    }

    {
        auto spec = fromjson(
            R"({$_internalApplyOplogUpdate: {
                    oplogUpdate: {"$v": NumberInt(2), diff: {sa: []}}
               }})");
        auto source = DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(),
                                                                             getExpCtx());
        auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}}}, getExpCtx());
        auto stage = exec::agg::buildStageAndStitch(source, mock);
        ASSERT_THROWS_CODE(stage->getNext(), DBException, 4770507);
    }
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, RedactsCorrectly) {
    auto spec = fromjson(R"({
        $_internalApplyOplogUpdate: {
            oplogUpdate: {
                $v: 2,
                diff: { sa: [] }
            }
        }
    })");
    auto docSource =
        DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalApplyOplogUpdate": {
                "oplogUpdate":"?object"
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, SerializesRepresentativeValueCorrectly) {
    auto spec = fromjson(R"({
        $_internalApplyOplogUpdate: {
            oplogUpdate: {
                $v: 1,
                diff: { u: { b: 3 } }
            }
        }
    })");
    auto docSource =
        DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(), getExpCtx());

    // Serialize with representative query shape options.
    std::vector<Value> serialization;
    docSource->serializeToArray(
        serialization,
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalApplyOplogUpdate": {
                "oplogUpdate": {}
            }
        })",
        serialization[0].getDocument().toBson());

    // Verify the representative value can be reparsed into a new stage.
    auto serializedBSON = serialization[0].getDocument().toBson();
    auto roundTripped = DocumentSourceInternalApplyOplogUpdate::createFromBson(
        serializedBSON.firstElement(), getExpCtx());

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

}  // namespace
}  // namespace mongo
