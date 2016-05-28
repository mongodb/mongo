// expression_geo_test.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/** Unit tests for MatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/stdx/memory.h"

namespace mongo {

TEST(ExpressionGeoTest, Geo1) {
    BSONObj query = fromjson("{loc:{$within:{$box:[{x: 4, y:4},[6,6]]}}}");

    std::unique_ptr<GeoExpression> gq(new GeoExpression);
    ASSERT_OK(gq->parseFrom(query["loc"].Obj()));

    GeoMatchExpression ge;
    ASSERT(ge.init("a", gq.release(), query).isOK());

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

    GeoNearMatchExpression gne;
    ASSERT(gne.init("a", nq.release(), query).isOK());

    // We can't match the data but we can make sure it was parsed OK.
    ASSERT_EQUALS(gne.getData().centroid->crs, SPHERE);
    ASSERT_EQUALS(gne.getData().minDistance, 0);
    ASSERT_EQUALS(gne.getData().maxDistance, 100);
}

std::unique_ptr<GeoMatchExpression> makeGeoMatchExpression(const BSONObj& locQuery) {
    std::unique_ptr<GeoExpression> gq(new GeoExpression);
    ASSERT_OK(gq->parseFrom(locQuery));

    std::unique_ptr<GeoMatchExpression> ge = stdx::make_unique<GeoMatchExpression>();
    ASSERT_OK(ge->init("a", gq.release(), locQuery));

    return ge;
}

std::unique_ptr<GeoNearMatchExpression> makeGeoNearMatchExpression(const BSONObj& locQuery) {
    std::unique_ptr<GeoNearExpression> nq(new GeoNearExpression);
    ASSERT_OK(nq->parseFrom(locQuery));

    std::unique_ptr<GeoNearMatchExpression> gne = stdx::make_unique<GeoNearMatchExpression>();
    ASSERT_OK(gne->init("a", nq.release(), locQuery));

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
}
