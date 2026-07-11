// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_set_metadata.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using DocumentSourceSetMetadataTest = AggregationContextFixture;

TEST_F(DocumentSourceSetMetadataTest, ErrorsIfNonObject) {
    auto spec = fromjson(R"({
        $setMetadata: "score"
    })");
    ASSERT_THROWS_CODE(DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceSetMetadataTest, ErrorsIfEmptyObject) {
    auto spec = fromjson(R"({
        $setMetadata: {
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceSetMetadataTest, ErrorsIfMoreThanOneField) {
    auto spec = fromjson(R"({
        $setMetadata: {
            score: "$a",
            scoreDetails: "$b"
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceSetMetadataTest, ErrorsIfInvalidMetaType) {
    auto spec = fromjson(R"({
        $setMetadata: {
            fakeField: "$a"
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       17308);
}

TEST_F(DocumentSourceSetMetadataTest, SetFromFieldPath) {
    auto spec = fromjson(R"({
        $setMetadata: {
            geoNearDistance: "$dist"
        }
    })");

    Document inputDoc = Document{{"dist", 0.4}};
    auto source = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(source, mock);

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());

    ASSERT_EQ(next.releaseDocument().metadata().getGeoNearDistance(), 0.4);

    next = stage->getNext();
    ASSERT(next.isEOF());
}


TEST_F(DocumentSourceSetMetadataTest, SetFromExpression) {
    auto spec = fromjson(R"({
        $setMetadata: {
            textScore: {$multiply: [{$add: ["$foo", 5]}, 0.5]}
        }
    })");

    Document inputDoc = Document{{"foo", 1.6}};
    auto source = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(source, mock);

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());

    ASSERT_EQ(next.releaseDocument().metadata().getTextScore(), 3.3);

    next = stage->getNext();
    ASSERT(next.isEOF());
}

TEST_F(DocumentSourceSetMetadataTest, ErrorsIfExpressionDoesntMatchDateTypeMetaField) {
    auto spec = fromjson(R"({
        $setMetadata: {
            timeseriesBucketMinTime: {$multiply: [{$add: ["$foo", 5]}, 0.5]}
        }
    })");

    Document inputDoc = Document{{"foo", 1.6}};
    auto source = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(source, mock);

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceSetMetadataTest, ErrorsIfExpressionDoesntMatchNumericMetaFieldType) {
    auto spec = fromjson(R"({
        $setMetadata: {
            randVal: {$arrayElemAt: ["$foo", 1]}
        }
    })");

    Document inputDoc = Document{{"foo",
                                  BSON_ARRAY("a" << "b"
                                                 << "c")}};
    auto source = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(source, mock);

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceSetMetadataTest, ErrorsIfExpressionDoesntMatchBSONObjMetaFieldType) {
    auto spec = fromjson(R"({
        $setMetadata: {
            searchScoreDetails: { $median: { input: "$grade", method: "approximate" } }
        }
    })");

    Document inputDoc = Document{{"grade", 5}};
    auto source = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(source, mock);

    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceSetMetadataTest, SetMetadataBSONObj) {
    auto spec = fromjson(R"({
        $setMetadata: {
            searchScoreDetails: {a: "$dist", b: "My String Info", c: {nested: "$dist"}}
        }
    })");

    Document inputDoc = Document{{"dist", 0.4}};
    auto source = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(source, mock);

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());

    BSONObj expectedOutput = fromjson(R"({
        a: 0.4,
        b: "My String Info",
        c: { nested: 0.4 }
    })");
    ASSERT_BSONOBJ_EQ(next.releaseDocument().metadata().getSearchScoreDetails(), expectedOutput);

    next = stage->getNext();
    ASSERT(next.isEOF());
}

TEST_F(DocumentSourceSetMetadataTest, SetMetadataMultipleDocuments) {
    auto spec = fromjson(R"({
        $setMetadata: {
            geoNearDistance: {$multiply: [{$add: ["$dist", 5]}, 0.2]}
        }
    })");

    std::deque<DocumentSource::GetNextResult> results = {Document{{"dist", 0.3}, {"foo", 2}},
                                                         Document{{"dist", 1.1}},
                                                         Document{{"dist", 0.8}, {"bar", 10}}};
    auto mock = exec::agg::MockStage::createForTest(std::move(results), getExpCtx());
    auto source = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
    auto stage = exec::agg::buildStageAndStitch(source, mock);

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.releaseDocument().metadata().getGeoNearDistance(), 1.06);

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.releaseDocument().metadata().getGeoNearDistance(), 1.22);

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.releaseDocument().metadata().getGeoNearDistance(), 1.16);

    next = stage->getNext();
    ASSERT(next.isEOF());
}

TEST_F(DocumentSourceSetMetadataTest, QueryShapeDebugString) {
    query_shape::SerializationOptions opts =
        query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

    {
        BSONObj spec = fromjson(R"({
            $setMetadata: {
                textScore: {$multiply: [{$add: ["$foo", 5]}, 0.5]}
            }
        })");
        auto score = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
            R"({
            "$setMetadata": {
                "textScore": {
                    "$multiply": [
                        {
                            "$add": [
                                "$HASH<foo>",
                                "?number"
                            ]
                        },
                        "?number"
                    ]
                }
            }
        })",
            output.front().getDocument().toBson());
    }

    {
        BSONObj spec = fromjson(R"({
            $setMetadata: {
                searchScoreDetails: {a: "$dist", b: "My String Info", c: {nested: "$dist"}}
            }
        })");
        auto score = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
            R"({
            "$setMetadata": {
                "searchScoreDetails": {
                    "HASH<a>": "$HASH<dist>",
                    "HASH<b>": "?string",
                    "HASH<c>": {
                        "HASH<nested>": "$HASH<dist>"
                    }
                }
            }
        })",
            output.front().getDocument().toBson());
    }
}

TEST_F(DocumentSourceSetMetadataTest, RepresentativeQueryShape) {
    query_shape::SerializationOptions opts =
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions;

    {
        BSONObj spec = fromjson(R"({
            $setMetadata: {
                geoNearPoint: ["$x", "$y"]
            }
        })");
        auto score = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(R"({"$setMetadata":{"geoNearPoint":["$x","$y"]}})",
                               output.front().getDocument().toBson());
    }

    {
        BSONObj spec = fromjson(R"({
            $setMetadata: {
                vectorSearchScore: {$add: [1, "$score"]}
            }
        })");
        auto score = DocumentSourceSetMetadata::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
            R"({
            "$setMetadata": {
                "vectorSearchScore": {"$add": [1,"$score"]}
            }
        })",
            output.front().getDocument().toBson());
    }
}
}  // namespace
}  // namespace mongo
