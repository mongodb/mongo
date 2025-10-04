/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_geo_parser.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::evaluate_matcher_geo_test {

class InternalBucketGeoWithinExpression : public mongo::unittest::Test {
public:
    auto getExpCtx() {
        return _expCtx;
    }

    BSONObj createBucketObj(BSONElement minLoc, BSONElement maxLoc) {
        BSONObjBuilder bob(fromjson(R"({
            _id : 1,
            meta : { field : "a" },
            data : {
                time : {
                    "0" : 1626739090
                },
                _id : {
                    "0" : 0
                }
            }
        })"));

        BSONObjBuilder controlBob(bob.subobjStart("control"));
        controlBob.appendNumber("version", 1);
        BSONObjBuilder minObj(controlBob.subobjStart("min"));
        minObj.appendDate("time", Date_t());
        minObj.appendNumber("_id", 0);
        minObj.append(minLoc);
        minObj.doneFast();

        BSONObjBuilder maxObj(controlBob.subobjStart("max"));
        maxObj.appendDate("time", Date_t());
        maxObj.appendNumber("_id", 1);
        maxObj.append(maxLoc);
        maxObj.doneFast();
        controlBob.doneFast();

        return bob.obj();
    }

    std::unique_ptr<MatchExpression> getDummyBucketGeoExpr() {
        auto bucketGeoExpr = fromjson(R"(
            {$_internalBucketGeoWithin: {
                withinRegion: {
                    $geometry: {
                        type : "Polygon",
                        coordinates : [[[ 0, 0 ], [ 0, 5 ], [ 5, 5 ], [ 5, 0 ], [ 0, 0 ]]]
                    }
                },
                field: "loc"
            }})");
        auto expr = MatchExpressionParser::parse(bucketGeoExpr, _expCtx);
        ASSERT_OK(expr.getStatus());

        return std::move(expr.getValue());
    }

    std::unique_ptr<MatchExpression> getDummyBucketGeoExprLegacy() {
        auto bucketGeoExpr = fromjson(R"(
            {$_internalBucketGeoWithin: {
                withinRegion: {
                    $box: [
                        [0, 0],
                        [5, 5] 
                    ]
                },
                field: "loc"
            }})");
        auto expr = MatchExpressionParser::parse(bucketGeoExpr, _expCtx);
        ASSERT_OK(expr.getStatus());

        return std::move(expr.getValue());
    }

private:
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx = new ExpressionContextForTest();
};

TEST_F(InternalBucketGeoWithinExpression, BoxPolygonOverlap) {
    auto expr = getDummyBucketGeoExpr();

    auto obj = createBucketObj(BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(0 << 0)))
                                   .firstElement(),
                               BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(5 << 5)))
                                   .firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, ShouldFilterOutBucketDisjoint) {
    auto expr = getDummyBucketGeoExpr();

    auto obj = createBucketObj(BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(10 << 10)))
                                   .firstElement(),
                               BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(20 << 20)))
                                   .firstElement());

    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, ShouldFilterOutBucketDisjointLegacyPoints) {
    auto expr = getDummyBucketGeoExprLegacy();

    auto obj = createBucketObj(BSON("loc" << BSON_ARRAY(10 << 10)).firstElement(),
                               BSON("loc" << BSON_ARRAY(20 << 20)).firstElement());

    ASSERT_FALSE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, ContainsAllPointsInTheBucket) {
    auto expr = getDummyBucketGeoExpr();

    auto obj = createBucketObj(BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(1 << 1)))
                                   .firstElement(),
                               BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(3 << 3)))
                                   .firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, ContainsAllPointsInTheBucketLegacy) {
    auto expr = getDummyBucketGeoExprLegacy();

    auto obj = createBucketObj(BSON("loc" << BSON_ARRAY(1 << 1)).firstElement(),
                               BSON("loc" << BSON_ARRAY(3 << 3)).firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, BBoxContainsWithinRegion) {
    auto expr = getDummyBucketGeoExpr();

    auto obj = createBucketObj(BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(-3 << -3)))
                                   .firstElement(),
                               BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(10 << 10)))
                                   .firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, BBoxContainsWithinRegionLegacy) {
    auto expr = getDummyBucketGeoExprLegacy();

    auto obj = createBucketObj(BSON("loc" << BSON_ARRAY(-3 << -3)).firstElement(),
                               BSON("loc" << BSON_ARRAY(10 << 10)).firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, BBoxIntersectsWithinRegion) {
    auto expr = getDummyBucketGeoExpr();

    auto obj = createBucketObj(BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(3 << 3)))
                                   .firstElement(),
                               BSON("loc" << BSON("type" << "Point"
                                                         << "coordinates" << BSON_ARRAY(10 << 10)))
                                   .firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, BBoxIntersectsWithinRegionLegacy) {
    auto expr = getDummyBucketGeoExprLegacy();

    auto obj = createBucketObj(BSON("loc" << BSON_ARRAY(3 << 3)).firstElement(),
                               BSON("loc" << BSON_ARRAY(10 << 10)).firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, BigBBoxContainsWithinRegion) {
    auto expr = getDummyBucketGeoExpr();

    auto obj =
        createBucketObj(BSON("loc" << BSON("type" << "Point"
                                                  << "coordinates" << BSON_ARRAY(-179 << -89)))
                            .firstElement(),
                        BSON("loc" << BSON("type" << "Point"
                                                  << "coordinates" << BSON_ARRAY(179 << 89)))
                            .firstElement());

    // This big spherical bounding box should contain the prime meridian(0° longitude) instead of
    // the anti-meridian(180° longitude).
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, BucketContainsNonPointType) {
    auto expr = getDummyBucketGeoExpr();

    auto obj = createBucketObj(
        BSON("loc" << BSON("type" << "Point"
                                  << "coordinates" << BSON_ARRAY(1 << 1)))
            .firstElement(),
        BSON("loc" << BSON("type" << "LineString"
                                  << "coordinates"
                                  << BSON_ARRAY(BSON_ARRAY(2 << 2) << BSON_ARRAY(3 << 3))))
            .firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST_F(InternalBucketGeoWithinExpression, BucketContainsNonPointTypeLegacy) {
    auto expr = getDummyBucketGeoExprLegacy();

    auto obj = createBucketObj(
        BSON("loc" << BSON("$box" << BSON_ARRAY(BSON_ARRAY(0 << 0) << BSON_ARRAY(5 << 5))))
            .firstElement(),
        BSON("loc" << BSON_ARRAY(10 << 10)).firstElement());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.get(), obj));
}

TEST(ExpressionGeoTest, Geo1) {
    BSONObj query = fromjson("{loc:{$within:{$box:[{x: 4, y:4},[6,6]]}}}");

    std::unique_ptr<GeoExpression> gq(new GeoExpression);
    ASSERT_OK(parsers::matcher::parseGeoExpressionFromBSON(query["loc"].Obj(), *gq));

    GeoMatchExpression ge("a"_sd, gq.release(), query);

    ASSERT(!exec::matcher::matchesBSON(&ge, fromjson("{a: [3,4]}")));
    ASSERT(exec::matcher::matchesBSON(&ge, fromjson("{a: [4,4]}")));
    ASSERT(exec::matcher::matchesBSON(&ge, fromjson("{a: [5,5]}")));
    ASSERT(exec::matcher::matchesBSON(&ge, fromjson("{a: [5,5.1]}")));
    ASSERT(exec::matcher::matchesBSON(&ge, fromjson("{a: {x: 5, y:5.1}}")));
}

TEST(MatchExpressionParserGeo, WithinBox) {
    BSONObj query = fromjson("{a:{$within:{$box:[{x: 4, y:4},[6,6]]}}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(
        query, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: [3,4]}")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: [4,4]}")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: [5,5]}")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: [5,5.1]}")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), fromjson("{a: {x: 5, y:5.1}}")));
}


}  // namespace mongo::evaluate_matcher_geo_test
