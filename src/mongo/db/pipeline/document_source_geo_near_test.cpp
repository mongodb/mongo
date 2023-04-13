/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceGeoNearTest = AggregationContextFixture;

TEST_F(DocumentSourceGeoNearTest, FailToParseIfKeyFieldNotAString) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], key: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfKeyIsTheEmptyString) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], key: ''}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfDistanceMultiplierIsNegative) {
    auto stageObj =
        fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], distanceMultiplier: -1.0}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfLimitFieldSpecified) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], limit: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       50858);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfNumFieldSpecified) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], num: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       50857);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfStartOptionIsSpecified) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], start: {}}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       50856);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfRequiredNearIsMissing) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist'}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       5860400);
}

TEST_F(DocumentSourceGeoNearTest, CanParseAndSerializeKeyField) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], key: 'a.b'}}");
    auto geoNear = DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx());
    std::vector<Value> serialized;
    geoNear->optimize();
    geoNear->serializeToArray(serialized);
    ASSERT_EQ(serialized.size(), 1u);
    auto expectedSerialization =
        Value{Document{{"$geoNear",
                        Value{Document{{"key", "a.b"_sd},
                                       {"near", std::vector<Value>{Value{0}, Value{0}}},
                                       {"distanceField", "dist"_sd},
                                       {"query", BSONObj()},
                                       {"spherical", false}}}}}};
    ASSERT_VALUE_EQ(expectedSerialization, serialized[0]);
}

TEST_F(DocumentSourceGeoNearTest, RedactionWithGeoJSONPoint) {
    auto spec = fromjson(R"({
        $geoNear: {
            distanceField: "a",
            maxDistance: 2,
            minDistance: 1,
            near: {
                type: "Point",
                coordinates: [ -23.484, 28.3913 ]
            },
            query: { foo : "bar" },
            spherical: true
        }
    })");
    auto docSource = DocumentSourceGeoNear::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$geoNear": {
                "near": "?object",
                "distanceField": "HASH<a>",
                "maxDistance": "?",
                "minDistance": "?",
                "query": {
                    "HASH<foo>": {
                        "$eq": "?string"
                    }
                },
                "spherical": "?"
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGeoNearTest, RedactionWithGeoJSONLineString) {
    auto spec = fromjson(R"({
        $geoNear: {
            distanceField: "a",
            near: {
                type: "LineString",
                coordinates: [[0,0], [-1,-1]]
            },
            minDistance: 0.5
        }
    })");
    auto docSource = DocumentSourceGeoNear::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$geoNear": {
                "near": "?object",
                "distanceField": "HASH<a>",
                "minDistance": "?",
                "query": {},
                "spherical": "?"
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGeoNearTest, RedactionWithLegacyCoordinates) {
    auto spec = fromjson(R"({
        $geoNear: {
            distanceField: "foo",
            distanceMultiplier: 3.14,
            includeLocs: "bar.baz",
            near: [10, 10],
            key: "z",
            query: {
                a : { $gt: 10 }
            },
            spherical: false
        }
    })");
    auto docSource = DocumentSourceGeoNear::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$geoNear": {
                "key": "HASH<z>",
                "near": "?array<?number>",
                "distanceField": "HASH<foo>",
                "query": {
                    "HASH<a>": {
                        "$gt": "?number"
                    }
                },
                "spherical": "?",
                "distanceMultiplier": "?",
                "includeLocs": "HASH<bar>.HASH<baz>"
            }
        })",
        redact(*docSource));
}

}  // namespace
}  // namespace mongo
