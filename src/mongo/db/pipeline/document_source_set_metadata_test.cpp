/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
    auto stage = exec::agg::buildStage(source);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

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
    auto stage = exec::agg::buildStage(source);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

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
    auto stage = exec::agg::buildStage(source);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

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
    auto stage = exec::agg::buildStage(source);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

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
    auto stage = exec::agg::buildStage(source);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

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
    auto stage = exec::agg::buildStage(source);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

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
    auto stage = exec::agg::buildStage(source);
    stage->setSource(mock.get());

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
    SerializationOptions opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

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
    SerializationOptions opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;

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
