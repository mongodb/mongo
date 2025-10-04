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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_geo_parser.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

TEST(MatchExpressionParserGeoNear, ParseNear) {
    BSONObj query = fromjson(
        "{loc:{$near:{$maxDistance:100, "
        "$geometry:{type:\"Point\", coordinates:[5,7]}}}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQUALS(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQUALS(gnexp->getData().maxDistance, 100);
    ASSERT_EQUALS(gnexp->getData().centroid->crs, CRS::SPHERE);
    ASSERT_EQUALS(gnexp->getData().centroid->oldPoint.x, 5);
    ASSERT_EQUALS(gnexp->getData().centroid->oldPoint.y, 7);
}

// $near must be the only field in the expression object.
TEST(MatchExpressionParserGeoNear, ParseNearExtraField) {
    BSONObj query = fromjson(
        "{loc:{$near:{$maxDistance:100, "
        "$geometry:{type:\"Point\", coordinates:[0,0]}}, foo: 1}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                               expCtx,
                                               ExtensionsCallbackNoop(),
                                               MatchExpressionParser::kAllowAllSpecialFeatures)
                      .getStatus());
}

// For $near, $nearSphere, and $geoNear syntax of:
// {
//   $near/$nearSphere/$geoNear: [ <x>, <y> ],
//   $minDistance: <distance in radians>,
//   $maxDistance: <distance in radians>
// }
TEST(MatchExpressionParserGeoNear, ParseValidNear) {
    BSONObj query = fromjson("{loc: {$near: [1,0], $maxDistance: 100, $minDistance: 50}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQ(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQ(gnexp->getData().maxDistance, 100);
    ASSERT_EQ(gnexp->getData().minDistance, 50);
    ASSERT_EQUALS(gnexp->getData().centroid->crs, CRS::FLAT);
    ASSERT_EQ(gnexp->getData().centroid->oldPoint.x, 1);
    ASSERT_EQ(gnexp->getData().centroid->oldPoint.y, 0);
}

TEST(MatchExpressionParserGeoNear, ParseInvalidNear) {
    {
        BSONObj query = fromjson("{loc: {$maxDistance: 100, $near: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$minDistance: 100, $near: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$near: [0,0], $maxDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$near: [0,0], $minDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$near: [0,0], $eq: 40}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$eq: 40, $near: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson(
            "{loc: {$near: [0,0], $geoWithin: {$geometry: {type: \"Polygon\", coordinates: []}}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$near: {$foo: 1}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$minDistance: 10}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
}

TEST(MatchExpressionParserGeoNear, ParseValidGeoNear) {
    BSONObj query = fromjson("{loc: {$geoNear: [0,10], $maxDistance: 100, $minDistance: 50}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQ(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQ(gnexp->getData().maxDistance, 100);
    ASSERT_EQ(gnexp->getData().minDistance, 50);
    ASSERT_EQUALS(gnexp->getData().centroid->crs, CRS::FLAT);
    ASSERT_EQUALS(gnexp->getData().centroid->oldPoint.x, 0);
    ASSERT_EQUALS(gnexp->getData().centroid->oldPoint.y, 10);
}

TEST(MatchExpressionParserGeoNear, ParseValidGeoNearLegacyCoordinates) {
    // Although this is very misleading, this is a legal way to query near the point [5,10]
    // with a max distance of 2.
    BSONObj query =
        fromjson("{loc: {$geoNear: {$geometry: 5, $maxDistance: 10, $minDistance: 2}}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQ(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQ(gnexp->getData().maxDistance, 2);
    ASSERT_EQUALS(gnexp->getData().centroid->crs, CRS::FLAT);
    ASSERT_EQ(gnexp->getData().centroid->oldPoint.x, 5);
    ASSERT_EQ(gnexp->getData().centroid->oldPoint.y, 10);
}

TEST(MatchExpressionParserGeoNear, ParseInvalidGeoNear) {
    {
        BSONObj query = fromjson("{loc: {$maxDistance: 100, $geoNear: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$minDistance: 100, $geoNear: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$geoNear: [0,0], $eq: 1}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$geoNear: [0,0], $maxDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$geoNear: [0,0], $minDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$geoNear: [0,0], $invalidArgument: 0}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
}

TEST(MatchExpressionParserGeoNear, ParseValidNearSphere) {
    BSONObj query = fromjson("{loc: {$nearSphere: [0,1], $maxDistance: 100, $minDistance: 50}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    MatchExpression* exp = result.getValue().get();
    ASSERT_EQ(MatchExpression::GEO_NEAR, exp->matchType());

    GeoNearMatchExpression* gnexp = static_cast<GeoNearMatchExpression*>(exp);
    ASSERT_EQ(gnexp->getData().maxDistance, 100);
    ASSERT_EQ(gnexp->getData().minDistance, 50);
    ASSERT_EQUALS(gnexp->getData().centroid->crs, CRS::SPHERE);
    ASSERT_EQ(gnexp->getData().centroid->oldPoint.x, 0);
    ASSERT_EQ(gnexp->getData().centroid->oldPoint.y, 1);
}

TEST(MatchExpressionParserGeoNear, ParseInvalidNearSphere) {
    {
        BSONObj query = fromjson("{loc: {$maxDistance: 100, $nearSphere: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$minDistance: 100, $nearSphere: [0,0]}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(query,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        ASSERT_FALSE(result.isOK());
    }
    {
        BSONObj query = fromjson("{loc: {$nearSphere: [0,0], $maxDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$nearSphere: [0,0], $minDistance: {}}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
    {
        BSONObj query = fromjson("{loc: {$nearSphere: [0,0], $eq: 1}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_NOT_OK(MatchExpressionParser::parse(query,
                                                   expCtx,
                                                   ExtensionsCallbackNoop(),
                                                   MatchExpressionParser::kAllowAllSpecialFeatures)
                          .getStatus());
    }
}

TEST(ExpressionGeoTest, GeoNear1) {
    BSONObj query = fromjson(
        "{loc:{$near:{$maxDistance:100, "
        "$geometry:{type:\"Point\", coordinates:[0,0]}}}}");
    std::unique_ptr<GeoNearExpression> nq(new GeoNearExpression);
    ASSERT_OK(parsers::matcher::parseGeoNearExpressionFromBSON(query["loc"].Obj(), *nq));

    GeoNearMatchExpression gne("a"_sd, nq.release(), query);

    // We can't match the data but we can make sure it was parsed OK.
    ASSERT_EQUALS(gne.getData().centroid->crs, SPHERE);
    ASSERT_EQUALS(gne.getData().minDistance, 0);
    ASSERT_EQUALS(gne.getData().maxDistance, 100);
}

std::unique_ptr<GeoMatchExpression> makeGeoMatchExpression(const BSONObj& locQuery) {
    std::unique_ptr<GeoExpression> gq(new GeoExpression);
    ASSERT_OK(parsers::matcher::parseGeoExpressionFromBSON(locQuery, *gq));

    std::unique_ptr<GeoMatchExpression> ge =
        std::make_unique<GeoMatchExpression>("a"_sd, gq.release(), locQuery);

    return ge;
}

std::unique_ptr<GeoNearMatchExpression> makeGeoNearMatchExpression(const BSONObj& locQuery) {
    std::unique_ptr<GeoNearExpression> nq(new GeoNearExpression);
    ASSERT_OK(parsers::matcher::parseGeoNearExpressionFromBSON(locQuery, *nq));

    std::unique_ptr<GeoNearMatchExpression> gne =
        std::make_unique<GeoNearMatchExpression>("a"_sd, nq.release(), locQuery);

    return gne;
}

void assertGeoNearParseReturnsError(const BSONObj& locQuery) {
    std::unique_ptr<GeoNearExpression> nq(new GeoNearExpression);
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  parsers::matcher::parseGeoNearExpressionFromBSON(locQuery, *nq));
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

TEST(ExpressionGeoTest, SerializeGeoNearUnchanged) {
    {
        BSONObj query = fromjson("{$geoNear: [0, 0, 100]}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear":[0, 0, 100]})",
            gne->getSerializedRightHandSide());
    }
    {
        BSONObj query = fromjson("{$geoNear: {x: 0, y: 0, maxDist: 100}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear":{x: 0, y: 0, maxDist: 100}})",
            gne->getSerializedRightHandSide());
    }
    {
        BSONObj query = fromjson("{$geoNear: [0, 10], $maxDistance: 80 }");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear": [0,10],"$maxDistance": 80})",
            gne->getSerializedRightHandSide());
    }
    {
        BSONObj query = fromjson("{$geoNear: {x: 5, y: 10}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear": {x: 5, y: 10}})",
            gne->getSerializedRightHandSide());
    }
    {
        // Although this is very misleading, this is a legal way to query near the point [5,10]
        // since we allow any field names when representing a point as an embedded document.
        BSONObj query = fromjson("{$geoNear: {$geometry: 5, $minDistance: 10}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear": {$geometry: 5, $minDistance: 10}})",
            gne->getSerializedRightHandSide());
    }
    {
        // Although this is very misleading, this is a legal way to query near the point [5,10]
        // with a max distance of 2.
        BSONObj query = fromjson("{$geoNear: {$geometry: 5, $maxDistance: 10, $minDistance: 2}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear": {$geometry: 5, $maxDistance: 10, $minDistance: 2}})",
            gne->getSerializedRightHandSide());
    }
}


TEST(ExpressionGeoTest, SerializeGeoExpressions) {
    SerializationOptions opts = {};
    opts.transformIdentifiers = true;
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    {
        BSONObj query = fromjson("{$within: {$box: [{x: 4, y: 4}, [6, 6]]}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));

        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$within":{"$box":"?array<>"}})",
            ge->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson(
            "{$geoWithin: {$geometry: {type: \"MultiPolygon\", coordinates: [[[[20.0, 70.0],[30.0, "
            "70.0],[30.0, 50.0],[20.0, 50.0],[20.0, 70.0]]]]}}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({
                "$geoWithin": {
                    "$geometry": {
                        "type": "MultiPolygon",
                        "coordinates": "?array<?array>"
                    }
                }
            })",
            ge->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson(
            R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "MultiPolygon",
                        "coordinates": [[[
                            [-20.0, -70.0],
                            [-30.0, -70.0],
                            [-30.0, -50.0],
                            [-20.0, -50.0],
                            [-20.0, -70.0]
                        ]]]
                    }
                }
            })");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "MultiPolygon",
                        "coordinates": "?array<?array>"
                    }
                }
            })",
            ge->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query1 = fromjson(
            R"({$within: {
                    $geometry: {
                        type: 'Polygon',
                        coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]
                    }
            }})");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query1));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$within":{"$geometry":{"type":"Polygon","coordinates":"?array<?array>"}}})",
            ge->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson(
            "{$near: {$maxDistance: 100, "
            "$geometry: {type: 'Point', coordinates: [0, 0]}}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({
                "$near": {
                    "$maxDistance": "?number",
                    "$geometry": {
                        "type": "Point",
                        "coordinates": "?array<?number>"
                    }
                }
            })",
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{ $nearSphere: [0,0], $minDistance: 1, $maxDistance: 3 }");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({
                "$nearSphere": "?array<?number>",
                "$minDistance": "?number",
                "$maxDistance": "?number"
            })",
            gne->getSerializedRightHandSide(opts));
    }

    {
        BSONObj query = fromjson("{$near : [0, 0, 1] }");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$near":"?array<?number>"})",
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{$geoNear: [0, 0, 100]}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear":"?array<?number>"})",
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{$geoNear: {x: 5, y: 10, z: 12}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear":"?array<?number>"})",
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{$geoNear: [0, 10], $maxDistance: 80 }");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear":"?array<?number>","$maxDistance":"?number"})",
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{$geoNear: {x: 5, y: 10}}");
        std::unique_ptr<GeoNearMatchExpression> gne(makeGeoNearMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoNear":"?array<?number>"})",
            gne->getSerializedRightHandSide(opts));
    }
    {
        BSONObj query = fromjson("{$geoIntersects: {$geometry: [0, 0]}}");
        std::unique_ptr<GeoMatchExpression> ge(makeGeoMatchExpression(query));
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({"$geoIntersects":{"$geometry":["?number","?number"]}})",
            ge->getSerializedRightHandSide(opts));
    }
    {
        // Make sure we reject arrays with <2 or >2 elements.
        BSONObj query = fromjson("{$geoIntersects: {$geometry: [0, 0, 1]}}");
        std::unique_ptr<GeoExpression> gq(new GeoExpression);
        ASSERT_NOT_OK(parsers::matcher::parseGeoExpressionFromBSON(query, *gq));
        query = fromjson("{$geoIntersects: {$geometry: [0]}}");
        ASSERT_NOT_OK(parsers::matcher::parseGeoExpressionFromBSON(query, *gq));
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

TEST(ExpressionGeoTest, SerializeWithCRSIFSpecifiedWithChangedOptions) {
    BSONObj query1 = fromjson(
        "{$within: {$geometry: {type: 'Polygon',"
        "coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]],"
        "crs: {"
        "type: 'name',"
        "properties: { name: 'urn:x-mongodb:crs:strictwinding:EPSG:4326' }"
        "}}}}");
    std::unique_ptr<GeoMatchExpression> ge1(makeGeoMatchExpression(query1));
    auto opts = SerializationOptions{LiteralSerializationPolicy::kToRepresentativeParseableValue};
    auto serialized = ge1->getSerializedRightHandSide(opts);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "$within": {
                "$geometry": {
                    "type": "Polygon",
                    "coordinates": [
                        [
                            [
                                0,
                                0
                            ],
                            [
                                0,
                                1
                            ],
                            [
                                1,
                                1
                            ],
                            [
                                0,
                                0
                            ]
                        ]
                    ],
                    "crs": {
                        "type": "name",
                        "properties": {
                            "name": "urn:x-mongodb:crs:strictwinding:EPSG:4326"
                        }
                    }
                }
            }
        })",
        serialized);
    serialized = ge1->getSerializedRightHandSide(opts);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({
            "$within": {
                "$geometry": {
                    "type": "Polygon",
                    "coordinates": [
                        [
                            [
                                0,
                                0
                            ],
                            [
                                0,
                                1
                            ],
                            [
                                1,
                                1
                            ],
                            [
                                0,
                                0
                            ]
                        ]
                    ],
                    "crs": {
                        "type": "name",
                        "properties": {
                            "name": "urn:x-mongodb:crs:strictwinding:EPSG:4326"
                        }
                    }
                }
            }
        })",
        serialized);
}

template <typename CreateFn>
void assertRepresentativeShapeIsStable(BSONObj inputExpr,
                                       BSONObj expectedRepresentativeExpr,
                                       CreateFn createFn) {
    auto opts = SerializationOptions{LiteralSerializationPolicy::kToRepresentativeParseableValue};
    auto ge(createFn(inputExpr));

    auto serializedExpr = ge->getSerializedRightHandSide(opts);
    ASSERT_BSONOBJ_EQ(serializedExpr, expectedRepresentativeExpr);

    auto roundTripped = createFn(serializedExpr);
    ASSERT_BSONOBJ_EQ(roundTripped->getSerializedRightHandSide(opts), serializedExpr);
}

void assertRepresentativeGeoShapeIsStable(BSONObj inputExpr, BSONObj expectedRepresentativeExpr) {
    assertRepresentativeShapeIsStable(
        inputExpr, expectedRepresentativeExpr, [](const BSONObj& input) {
            return makeGeoMatchExpression(input);
        });
}

void assertRepresentativeGeoNearShapeIsStable(BSONObj inputExpr,
                                              BSONObj expectedRepresentativeExpr) {
    assertRepresentativeShapeIsStable(
        inputExpr, expectedRepresentativeExpr, [](const BSONObj& input) {
            return makeGeoNearMatchExpression(input);
        });
}

TEST(ExpressionGeoTest, RoundTripSerializeGeoExpressions) {
    assertRepresentativeGeoShapeIsStable(fromjson("{$within: {$box: [{x: 4, y: 4}, [6, 6]]}}"),
                                         fromjson("{$within: {$box: [[1, 1],[1, 1]]}}"));

    assertRepresentativeGeoShapeIsStable(
        fromjson(
            R"({$geoWithin: {$geometry: {type: "MultiPolygon", coordinates: [[[[20.0, 70.0],[30.0, 70.0],[30.0, 50.0],[20.0, 50.0],[20.0, 70.0]]]]}}})"),
        fromjson(
            R"({$geoWithin: {$geometry: {type: "MultiPolygon", coordinates: [[[[0, 0],[0, 1],[1, 1],[0, 0]]]]}}})"));

    assertRepresentativeGeoShapeIsStable(fromjson(R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "MultiPolygon",
                        "coordinates": [[[
                            [-20.0, -70.0],
                            [-30.0, -70.0],
                            [-30.0, -50.0],
                            [-20.0, -50.0],
                            [-20.0, -70.0]
                        ]]]
                    }
                }
            })"),
                                         fromjson(R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "MultiPolygon",
                        "coordinates": [[[[0, 0],[0, 1],[1, 1],[0, 0]]]]
                    }
                }
            })"));

    assertRepresentativeGeoShapeIsStable(fromjson(R"({$within: {
                    $geometry: {
                        type: 'Polygon',
                        coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]
                    }
            }})"),
                                         fromjson(R"({$within: {
                    $geometry: {
                        type: 'Polygon',
                        coordinates: [[[0, 0],[0, 1],[1, 1],[0, 0]]]
                    }
            }})"));

    assertRepresentativeGeoNearShapeIsStable(
        fromjson("{$near: {$maxDistance: 100, $geometry: {type: 'Point', coordinates: [0, 0]}}}"),
        fromjson("{$near: {$maxDistance: 1, $geometry: {type: 'Point', coordinates: [1, 1]}}}"));

    assertRepresentativeGeoNearShapeIsStable(
        fromjson("{$nearSphere: [0,0], $minDistance: 2, $maxDistance: 4 }"),
        fromjson("{$nearSphere: [1,1], $minDistance: 1, $maxDistance: 1 }"));

    assertRepresentativeGeoNearShapeIsStable(
        fromjson("{$minDistance: 2, $maxDistance: 4, $nearSphere: [0,0]}"),
        fromjson("{$minDistance: 1, $maxDistance: 1, $nearSphere: [1,1]}"));

    assertRepresentativeGeoNearShapeIsStable(fromjson("{$near: [0, 0, 1]}"),
                                             fromjson("{$near: [1, 1]}"));

    assertRepresentativeGeoNearShapeIsStable(fromjson("{$geoNear: [0, 0, 100]}"),
                                             fromjson("{$geoNear: [1, 1]}"));

    assertRepresentativeGeoNearShapeIsStable(fromjson("{$geoNear: [0, 10], $maxDistance: 80 }"),
                                             fromjson("{$geoNear: [1, 1], $maxDistance: 1}"));

    assertRepresentativeGeoShapeIsStable(fromjson("{$geoIntersects: {$geometry: [0, 0]}}"),
                                         fromjson("{$geoIntersects: {$geometry: [1, 1]}}"));
    // Test scenario with new $geometry query not specifying the geometry type.
    assertRepresentativeGeoNearShapeIsStable(
        fromjson("{$geoNear: { $geometry: {coordinates: [0, 10]}}}"),
        fromjson("{$geoNear: { $geometry: {coordinates: [1, 1]}}}"));

    // $geometry operator in $geoNear should accept only Point type.
    assertRepresentativeGeoNearShapeIsStable(
        fromjson(R"({$geoNear: { $geometry: {"type": "Point", "coordinates": [0, 10]}}})"),
        fromjson(R"({$geoNear: { $geometry: {"type": "Point", coordinates: [1, 1]}}})"));
    assertGeoNearParseReturnsError(
        fromjson(R"({$geoNear: { $geometry: {"type": "LineString", "coordinates": [0, 10]}}})"));
    assertGeoNearParseReturnsError(fromjson(
        R"({$geoNear: { $geometry: {"type": "LineString", "coordinates": [[1, 2], [3, 4]]}}})"));
    assertGeoNearParseReturnsError(fromjson(
        R"({$geoNear: { $geometry: {"type": "MultiPoint", "coordinates": [[0, 0], [1, 1]]}}})"));

    // Test scenario with $nearSphere without $geometry and no type specified
    assertRepresentativeGeoNearShapeIsStable(fromjson(R"({"$nearSphere":{"coordinates":[0,0]}})"),
                                             fromjson(R"({"$nearSphere":{"coordinates":[1,1]}})"));

    // Test case with first field of $geometry as numeric field, arbitrary coordinate naming.
    assertRepresentativeGeoShapeIsStable(
        fromjson(R"({"$geoIntersects":{"$geometry":{"shardOptions":40,"y":5}}})"),
        fromjson(R"({"$geoIntersects":{"$geometry":{"shardOptions":1,"y":1}}})"));

    assertRepresentativeGeoShapeIsStable(fromjson(R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "MultiLineString",
                        "coordinates": [[
                            [2, 0],
                            [2, 2]
                        ], [
                            [0, 4],
                            [1, 4]
                        ]]
                    }
                }
            })"),
                                         fromjson(R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "MultiLineString",
                        "coordinates": [[[0, 0], [1, 1]],[[0, 0], [1, 1]]]
                    }
                }
            })"));

    assertRepresentativeGeoShapeIsStable(fromjson(R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "LineString",
                        "coordinates": [
                            [2, 0],
                            [2, 2]
                        ]
                    }
                }
            })"),
                                         fromjson(R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "LineString",
                        "coordinates": [[0, 0], [1, 1]]
                    }
                }
            })"));

    assertRepresentativeGeoShapeIsStable(fromjson(R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "GeometryCollection",
                        "geometries": [{
                            "type": "LineString",
                            "coordinates": [
                                [2, 0],
                                [2, 2]
                            ]
                        }, {
                            type: 'Point', coordinates: [2, 2]
                        }]
                    }
                }
            })"),
                                         fromjson(R"({
                "$geoIntersects": {
                    "$geometry": {
                        "type": "GeometryCollection",
                        "geometries": [{
                            "type": "LineString",
                            "coordinates": [[0, 0], [1, 1]]
                        }, {
                            type: 'Point', coordinates: [1, 1]
                        }]
                    }
                }
            })"));

    assertRepresentativeGeoNearShapeIsStable(fromjson("{$near: {x: 100, y: 0.1}}"),
                                             fromjson("{$near: [1,1]}"));

    assertRepresentativeGeoNearShapeIsStable(
        fromjson("{$near: {$maxDistance: 100, $geometry: 0.1}}"), fromjson("{$near: [1,1]}"));

    assertRepresentativeGeoNearShapeIsStable(fromjson("{$near: {x: 0, y: 100 , z: 10}}"),
                                             fromjson("{$near: [1,1]}"));

    assertRepresentativeGeoNearShapeIsStable(fromjson("{$near: {$geometry: 100, y: 0.1, z: 10}}"),
                                             fromjson("{$near: [1,1]}"));
}

}  // namespace mongo
