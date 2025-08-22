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

#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

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
    auto geoDistStage = exec::agg::buildStage(geoDist);

    auto mock = exec::agg::MockStage::createForTest(
        {DOC("loc" << DOC("type" << "Point"_sd
                                 << "coordinates" << DOC_ARRAY(1 << 1)))},
        getExpCtx());

    geoDistStage->setSource(mock.get());
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
    auto geoDistStage = exec::agg::buildStage(geoDist);

    auto mock = exec::agg::MockStage::createForTest(
        {DOC("loc" << DOC("type" << "Point"_sd
                                 << "coordinates" << DOC_ARRAY(0 << 0)))},
        getExpCtx());

    geoDistStage->setSource(mock.get());
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
    auto geoDistStage = exec::agg::buildStage(geoDist);

    auto mock = exec::agg::MockStage::createForTest({DOC("loc" << DOC_ARRAY(0 << 0))}, getExpCtx());

    geoDistStage->setSource(mock.get());
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
    auto geoDistStage = exec::agg::buildStage(geoDist);

    auto mock = exec::agg::MockStage::createForTest({DOC("loc" << DOC_ARRAY(0 << 0))}, getExpCtx());

    geoDistStage->setSource(mock.get());
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

}  // namespace
}  // namespace mongo
