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

/** Unit tests for MatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include <memory>

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_geo.h"


namespace mongo {

TEST(ExpressionGeoTest, Geo1) {
    BSONObj query = fromjson("{loc:{$within:{$box:[{x: 4, y:4},[6,6]]}}}");

    std::unique_ptr<GeoExpression> gq(new GeoExpression);
    ASSERT_OK(gq->parseFrom(query["loc"].Obj()));

    GeoMatchExpression ge("a"_sd, gq.release(), query);

    ASSERT(!ge.matchesBSON(fromjson("{a: [3,4]}")));
    ASSERT(ge.matchesBSON(fromjson("{a: [4,4]}")));
    ASSERT(ge.matchesBSON(fromjson("{a: [5,5]}")));
    ASSERT(ge.matchesBSON(fromjson("{a: [5,5.1]}")));
    ASSERT(ge.matchesBSON(fromjson("{a: {x: 5, y:5.1}}")));
}

TEST(ExpressionGeoTest, GeoNear1) {
    BSONObj query = fromjson(
        "{loc:{$near:{$maxDistance:100, "
        "$geometry:{type:\"Point\", coordinates:[0,0]}}}}");
    std::unique_ptr<GeoNearExpression> nq(new GeoNearExpression);
    ASSERT_OK(nq->parseFrom(query["loc"].Obj()));

    GeoNearMatchExpression gne("a"_sd, nq.release(), query);

    // We can't match the data but we can make sure it was parsed OK.
    ASSERT_EQUALS(gne.getData().centroid->crs, SPHERE);
    ASSERT_EQUALS(gne.getData().minDistance, 0);
    ASSERT_EQUALS(gne.getData().maxDistance, 100);
}

std::unique_ptr<GeoMatchExpression> makeGeoMatchExpression(const BSONObj& locQuery) {
    std::unique_ptr<GeoExpression> gq(new GeoExpression);
    ASSERT_OK(gq->parseFrom(locQuery));

    std::unique_ptr<GeoMatchExpression> ge =
        std::make_unique<GeoMatchExpression>("a"_sd, gq.release(), locQuery);

    return ge;
}

std::unique_ptr<GeoNearMatchExpression> makeGeoNearMatchExpression(const BSONObj& locQuery) {
    std::unique_ptr<GeoNearExpression> nq(new GeoNearExpression);
    ASSERT_OK(nq->parseFrom(locQuery));

    std::unique_ptr<GeoNearMatchExpression> gne =
        std::make_unique<GeoNearMatchExpression>("a"_sd, nq.release(), locQuery);

    return gne;
}


/**
 * A bunch of cases in which a geo expression is equivalent() to both itself or to another
 * expression.
 */
TEST(ExpressionGeoTest, GeoEquivalent) {
    {
        BSONObj query = fromjson("{$within: {$box: [{x: 4, y: 4}, [6, 6]]}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));
        ASSERT(ge->equivalent(ge.get()));
    }
    {
        BSONObj query = fromjson(
            "{$within: {$geometry: {type: 'Polygon',"
            "coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));
        ASSERT(ge->equivalent(ge.get()));
    }
    {
        BSONObj query1 = fromjson(
                    "{$within: {$geometry: {type: 'Polygon',"
                    "coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}}"),
                query2 = fromjson(
                    "{$within: {$geometry: {type: 'Polygon',"
                    "coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}}");
        std::unique_ptr<GeoMatchExpression> ge1(makeGeoMatchExpression(query1)),
            ge2(makeGeoMatchExpression(query2));
        ASSERT(ge1->equivalent(ge2.get()));
    }
}

/**
 * A bunch of cases in which a *geoNear* expression is equivalent both to itself or to
 * another expression.
 */
TEST(ExpressionGeoTest, GeoNearEquivalent) {
    {
        BSONObj query = fromjson(
            "{$near: {$maxDistance: 100, "
            "$geometry: {type: 'Point', coordinates: [0, 0]}}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT(gne->equivalent(gne.get()));
    }
    {
        BSONObj query = fromjson(
            "{$near: {$minDistance: 10, $maxDistance: 100,"
            "$geometry: {type: 'Point', coordinates: [0, 0]}}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT(gne->equivalent(gne.get()));
    }
    {
        BSONObj query1 = fromjson(
                    "{$near: {$maxDistance: 100, "
                    "$geometry: {type: 'Point', coordinates: [1, 0]}}}"),
                query2 = fromjson(
                    "{$near: {$maxDistance: 100, "
                    "$geometry: {type: 'Point', coordinates: [1, 0]}}}");
        std::unique_ptr<GeoNearMatchExpression> gne1(makeGeoNearMatchExpression(query1)),
            gne2(makeGeoNearMatchExpression(query2));
        ASSERT(gne1->equivalent(gne2.get()));
    }
}


TEST(ExpressionGeoTest, SerializeGeoExpressions) {
    SerializationOptions opts = {};
    opts.redactIdentifiers = true;
    opts.replacementForLiteralArgs = "?";
    {
        BSONObj query = fromjson("{$within: {$box: [{x: 4, y: 4}, [6, 6]]}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));

        ASSERT_VALUE_EQ_AUTO(                // NOLINT
            "{ $within: { $box: \"?\" } }",  // NOLINT (test
                                             // auto-update)
            ge->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson(
            "{$geoWithin: {$geometry: {type: \"MultiPolygon\", coordinates: [[[[20.0, 70.0],[30.0, "
            "70.0],[30.0, 50.0],[20.0, 50.0],[20.0, 70.0]]]]}}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));
        ASSERT_VALUE_EQ_AUTO(  // NOLINT
            "{ $geoWithin: { $geometry: { type: \"MultiPolygon\", coordinates: \"?\" } } }",  // NOLINT
                                                                                              // (test
                                                                                              // auto-update)
            ge->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson(
            "{$geoIntersects: {$geometry: {type: \"MultiPolygon\",coordinates: [[[[-20.0, "
            "-70.0],[-30.0, -70.0],[-30.0, -50.0],[-20.0, -50.0],[-20.0, -70.0]]]]}}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));
        ASSERT_VALUE_EQ_AUTO(  // NOLINT
            "{ $geoIntersects: { $geometry: { type: \"MultiPolygon\", coordinates: \"?\" } } }",  // NOLINT
                                                                                                  // (test
                                                                                                  // auto-update)
            ge->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query1 = fromjson(
            "{$within: {$geometry: {type: 'Polygon',"
            "coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query1));
        ASSERT_VALUE_EQ_AUTO(                                                         // NOLINT
            "{ $within: { $geometry: { type: \"Polygon\", coordinates: \"?\" } } }",  // NOLINT
                                                                                      // (test
                                                                                      // auto-update)
            ge->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson(
            "{$near: {$maxDistance: 100, "
            "$geometry: {type: 'Point', coordinates: [0, 0]}}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_VALUE_EQ_AUTO(  // NOLINT
            "{ $near: { $maxDistance: \"?\", $geometry: { type: \"Point\", coordinates: \"?\" } } "
            "}",  // NOLINT (test auto-update)
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{ $nearSphere: [0,0], $minDistance: 1, $maxDistance: 3 }");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_VALUE_EQ_AUTO(                                                    // NOLINT
            "{ $nearSphere: \"?\", $minDistance: \"?\", $maxDistance: \"?\" }",  // NOLINT (test
                                                                                 // auto-update)
            gne->getSerializedRightHandSide(opts));
    }

    {
        BSONObj query = fromjson("{$near : [0, 0, 1] } }");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_VALUE_EQ_AUTO(    // NOLINT
            "{ $near: \"?\" }",  // NOLINT (test auto-update)
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{$geoNear: [0, 0, 100]}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_VALUE_EQ_AUTO(       // NOLINT
            "{ $geoNear: \"?\" }",  // NOLINT (test auto-update)
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{$geoNear: [0, 10], $maxDistance: 80 }");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_VALUE_EQ_AUTO(                            // NOLINT
            "{ $geoNear: \"?\", $maxDistance: \"?\" }",  // NOLINT (test auto-update)
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{$geoIntersects: {$geometry: [0, 0]}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));
        ASSERT_VALUE_EQ_AUTO(  // NOLINT
            "{ $geoIntersects: { $geometry: { coordinates: \"?\" } } }",
            ge->getSerializedRightHandSide(opts));
    }
}

/**
 * A geo expression being not equivalent to another expression.
 */
TEST(ExpressionGeoTest, GeoNotEquivalent) {
    BSONObj query1 = fromjson(
                "{$within: {$geometry: {type: 'Polygon',"
                "coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}}}"),
            query2 = fromjson(
                "{$within: {$geometry: {type: 'Polygon',"
                "coordinates: [[[0, 0], [3, 6], [6, 2], [0, 0]]]}}}");
    std::unique_ptr<GeoMatchExpression> ge1(makeGeoMatchExpression(query1)),
        ge2(makeGeoMatchExpression(query2));
    ASSERT(!ge1->equivalent(ge2.get()));
}

/**
 * A *geoNear* expression being not equivalent to another expression.
 */
TEST(ExpressionGeoTest, GeoNearNotEquivalent) {
    BSONObj query1 = fromjson(
                "{$near: {$maxDistance: 100, "
                "$geometry: {type: 'Point', coordinates: [0, 0]}}}"),
            query2 = fromjson(
                "{$near: {$maxDistance: 100, "
                "$geometry: {type: 'Point', coordinates: [1, 0]}}}");
    std::unique_ptr<GeoNearMatchExpression> gne1(makeGeoNearMatchExpression(query1)),
        gne2(makeGeoNearMatchExpression(query2));
    ASSERT(!gne1->equivalent(gne2.get()));
}
}  // namespace mongo
