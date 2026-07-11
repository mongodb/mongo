// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_geo_near.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"

#include <vector>


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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

TEST_F(DocumentSourceGeoNearTest, CanParseAndSerializeWithoutDistanceField) {
    auto stageObj = fromjson("{$geoNear: {near: [0, 0]}}");
    auto geoNear = DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx());
    std::vector<Value> serialized;
    checked_cast<DocumentSourceGeoNear*>(geoNear.get())->optimize();
    geoNear->serializeToArray(serialized);
    ASSERT_EQ(serialized.size(), 1u);
    auto expectedSerialization =
        Value{Document{{"$geoNear",
                        Value{Document{{"near", std::vector<Value>{Value{0}, Value{0}}},
                                       {"query", BSONObj{}},
                                       {"spherical", false}}}}}};
    ASSERT_VALUE_EQ(expectedSerialization, serialized[0]);
}

TEST_F(DocumentSourceGeoNearTest, CanParseAndSerializeKeyField) {
    auto stageObj =
        fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], key: 'a.b', query: {}}}");
    auto geoNear = DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx());
    std::vector<Value> serialized;
    checked_cast<DocumentSourceGeoNear*>(geoNear.get())->optimize();
    geoNear->serializeToArray(serialized);
    ASSERT_EQ(serialized.size(), 1u);
    auto expectedSerialization =
        Value{Document{{"$geoNear",
                        Value{Document{{"key", "a.b"sv},
                                       {"near", std::vector<Value>{Value{0}, Value{0}}},
                                       {"distanceField", "dist"sv},
                                       {"query", BSONObj{}},
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
                "maxDistance": "?number",
                "minDistance": "?number",
                "query": {
                    "HASH<foo>": {
                        "$eq": "?string"
                    }
                },
                "spherical": "?bool"
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
                "minDistance": "?number",
                "query": {},
                "spherical": "?bool"
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
                "spherical": "?bool",
                "distanceMultiplier": "?number",
                "includeLocs": "HASH<bar>.HASH<baz>"
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfUnkownArg) {
    auto stageObj = fromjson(
        "{$geoNear: {near: {type: 'Point', coordinates: [0, 0]}, distanceField: 'distanceField', "
        "spherical: true, blah: 'blaarghhh'}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}


}  // namespace
}  // namespace mongo
