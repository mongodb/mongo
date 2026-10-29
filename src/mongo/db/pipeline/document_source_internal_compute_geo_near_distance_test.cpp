// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using DocumentSourceInternalGeoNearDistanceTest = AggregationContextFixture;

TEST_F(DocumentSourceInternalGeoNearDistanceTest, DistanceBetweenOverlappingPoints) {
    BSONObj computeGeoSpec = fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: {
                type: "Point",
                coordinates: [1, 1]
            },
            key: "loc",
            distanceMultiplier: 1,
            distanceField: "dist"
        }})");
    auto geoDist = DocumentSourceInternalGeoNearDistance::createFromBson(
        computeGeoSpec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(
        {DOC("loc" << DOC("type" << "Point"sv
                                 << "coordinates" << DOC_ARRAY(1 << 1)))},
        getExpCtx());
    auto geoDistStage = exec::agg::buildStageAndStitch(geoDist, mock);
    auto next = geoDistStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.getDocument();
    ASSERT_EQUALS(doc["dist"].getType(), BSONType::numberDouble);
    ASSERT_EQUALS(doc["dist"].coerceToDouble(), 0);
}

TEST_F(DocumentSourceInternalGeoNearDistanceTest, SphericalDistanceBetweenTwoPoints) {
    BSONObj computeGeoSpec = fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: {
                type: "Point",
                coordinates: [0, 1]
            },
            key: "loc",
            distanceMultiplier: 1,
            distanceField: "dist"
        }})");
    auto geoDist = DocumentSourceInternalGeoNearDistance::createFromBson(
        computeGeoSpec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(
        {DOC("loc" << DOC("type" << "Point"sv
                                 << "coordinates" << DOC_ARRAY(0 << 0)))},
        getExpCtx());
    auto geoDistStage = exec::agg::buildStageAndStitch(geoDist, mock);
    auto next = geoDistStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.getDocument();
    const int meterToLatDegree = 111319;  // Each degree of latitude is approximately 111km.
    ASSERT_EQUALS(doc["dist"].getType(), BSONType::numberDouble);
    ASSERT_APPROX_EQUAL(doc["dist"].coerceToDouble(), meterToLatDegree, 300);
}

TEST_F(DocumentSourceInternalGeoNearDistanceTest, DistanceBetweenTwoLegacyPoints) {
    BSONObj computeGeoSpec = fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: [1, 1],
            key: "loc",
            distanceMultiplier: 1,
            distanceField: "dist"
        }})");
    auto geoDist = DocumentSourceInternalGeoNearDistance::createFromBson(
        computeGeoSpec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest({DOC("loc" << DOC_ARRAY(0 << 0))}, getExpCtx());
    auto geoDistStage = exec::agg::buildStageAndStitch(geoDist, mock);
    auto next = geoDistStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.getDocument();
    ASSERT_EQUALS(doc["dist"].getType(), BSONType::numberDouble);
    ASSERT_APPROX_EQUAL(doc["dist"].coerceToDouble(), 1.41421, 0.01);
}

TEST_F(DocumentSourceInternalGeoNearDistanceTest, DistanceBetweenTwoMixedPointsSphereAndFlat) {
    BSONObj computeGeoSpec = fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: {
                type: "Point",
                coordinates: [0, 1]
            },
            key: "loc",
            distanceMultiplier: 1,
            distanceField: "dist"
        }})");
    auto geoDist = DocumentSourceInternalGeoNearDistance::createFromBson(
        computeGeoSpec.firstElement(), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest({DOC("loc" << DOC_ARRAY(0 << 0))}, getExpCtx());
    auto geoDistStage = exec::agg::buildStageAndStitch(geoDist, mock);
    auto next = geoDistStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.getDocument();
    const int meterToLatDegree = 111319;  // Each degree of latitude is approximately 111km.
    ASSERT_EQUALS(doc["dist"].getType(), BSONType::numberDouble);
    ASSERT_APPROX_EQUAL(doc["dist"].coerceToDouble(), meterToLatDegree, 300);
}

TEST_F(DocumentSourceInternalGeoNearDistanceTest, RedactsCorrectly) {
    BSONObj computeGeoSpec = fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: {
                type: "Point",
                coordinates: [0, 1]
            },
            key: "loc",
            distanceMultiplier: 1,
            distanceField: "dist"
        }})");
    auto geoDist = DocumentSourceInternalGeoNearDistance::createFromBson(
        computeGeoSpec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalComputeGeoNearDistance": {
                "near": "?object",
                "key": "HASH<loc>",
                "distanceField": "HASH<dist>",
                "distanceMultiplier": "?number"
            }
        })",
        redact(*geoDist, true));
}

void assertRepresentativeShapeIsStable(auto expCtx,
                                       BSONObj inputStage,
                                       BSONObj expectedRepresentativeStage) {
    auto parsedStage =
        DocumentSourceInternalGeoNearDistance::createFromBson(inputStage.firstElement(), expCtx);
    std::vector<Value> serialization;
    auto opts = query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    parsedStage->serializeToArray(serialization, opts);

    auto serializedStage = serialization[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedStage, expectedRepresentativeStage);

    auto roundTripped = DocumentSourceInternalGeoNearDistance::createFromBson(
        serializedStage.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization, opts);
    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceInternalGeoNearDistanceTest, RoundtripSerializationPoint) {
    assertRepresentativeShapeIsStable(getExpCtx(),
                                      fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: {
                type: "Point",
                coordinates: [10, 11]
            },
            key: "loc",
            distanceMultiplier: 5,
            distanceField: "dist"
        }})"),
                                      fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: {
                type: "Point",
                coordinates: [1, 1]
            },
            key: "loc",
            distanceField: "dist",
            distanceMultiplier: 1
        }})"));
}

TEST_F(DocumentSourceInternalGeoNearDistanceTest, RoundtripSerializationCoordinate) {
    assertRepresentativeShapeIsStable(getExpCtx(),
                                      fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: [10, 11],
            key: "loc",
            distanceMultiplier: 5,
            distanceField: "dist"
        }})"),
                                      fromjson(R"(
        { $_internalComputeGeoNearDistance: {
            near: [1, 1],
            key: "loc",
            distanceField: "dist",
            distanceMultiplier: 1
        }})"));
}

}  // namespace
}  // namespace mongo
