/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_geo_parser.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <s2cellid.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/matcher"};

void verifyDebugString(unittest::GoldenTestContext& gctx, MatchExpression* match, StringData name) {
    // Verify the untagged case.
    gctx.outStream() << "==== VARIATION: matchExpression=" << name << std::endl;
    StringBuilder debug;
    match->debugString(debug);
    gctx.outStream() << debug.str() << std::endl;

    // Verify the tagged case.
    gctx.outStream() << "==== VARIATION: taggedMatchExpression=" << name << std::endl;
    debug.reset();
    match->setTag(new IndexTag(2));
    match->debugString(debug);
    gctx.outStream() << debug.str() << std::endl;
}

TEST(DebugStringTest, ExpressionAlwaysBoolean) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);

    // AlwaysFalseMatchExpression.
    auto falseExpr = std::make_unique<AlwaysFalseMatchExpression>();
    verifyDebugString(gctx, falseExpr.get(), "AlwaysFalseMatchExpression");

    // AlwaysTrueMatchExpression.
    auto trueExpr = std::make_unique<AlwaysTrueMatchExpression>();
    verifyDebugString(gctx, trueExpr.get(), "AlwaysTrueMatchExpression");
}

TEST(DebugStringTest, ArrayMatchExpressions) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);

    // ElemMatchObjectExpression.
    auto baseOperandObj = BSON("1" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("1"_sd, baseOperandObj["1"]);
    auto elemMatchObjExpr = std::make_unique<ElemMatchObjectMatchExpression>("a"_sd, std::move(eq));
    verifyDebugString(gctx, elemMatchObjExpr.get(), "ElemMatchObjectExpression");

    // ElemMatchValueExpression.
    auto baseOperandVal = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperandVal["$gt"]);
    auto elemMatchValExpr = std::make_unique<ElemMatchValueMatchExpression>("a"_sd);
    elemMatchValExpr->add(std::move(gt));
    verifyDebugString(gctx, elemMatchValExpr.get(), "ElemMatchValueExpression");

    // SizeMatchExpression.
    auto sizeExpr = std::make_unique<SizeMatchExpression>("a"_sd, 5);
    verifyDebugString(gctx, sizeExpr.get(), "SizeMatchExpression");
}

TEST(DebugStringTest, ExpressionExpr) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    auto expr = BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a" << 5)));
    auto exprMatchExpression =
        uassertStatusOK(MatchExpressionParser::parse(expr, new ExpressionContextForTest()));
    verifyDebugString(gctx, exprMatchExpression.get(), "ExpressionExpr");
}

TEST(DebugStringTest, ExpressionGeo) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // GeoMatchExpression.
    BSONObj query = fromjson("{loc:{$within:{$box:[{x: 4, y:4},[6,6]]}}}");

    std::unique_ptr<GeoExpression> gq(new GeoExpression);
    ASSERT_OK(parsers::matcher::parseGeoExpressionFromBSON(query["loc"].Obj(), *gq));

    GeoMatchExpression ge("a"_sd, gq.release(), query);
    verifyDebugString(gctx, &ge, "GeoMatchExpression");

    // GeoNearMatchExpression.
    query = fromjson(
        "{loc:{$near:{$maxDistance:100, "
        "$geometry:{type:\"Point\", coordinates:[0,0]}}}}");

    std::unique_ptr<GeoNearExpression> nq(new GeoNearExpression);
    ASSERT_OK(parsers::matcher::parseGeoNearExpressionFromBSON(query["loc"].Obj(), *nq));

    GeoNearMatchExpression gne("a"_sd, nq.release(), query);
    verifyDebugString(gctx, &gne, "GeoNearMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalBucketGeoWithin) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
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

    auto expr = uassertStatusOK(
        MatchExpressionParser::parse(bucketGeoExpr, new ExpressionContextForTest()));
    verifyDebugString(gctx, expr.get(), "ExpressionInternalBucketGeoWithin");
}

TEST(DebugStringTest, ExpressionLeaf) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);

    // ComparisonMatchExpression (EQ, GT, GTE, LT, LTE).
    auto baseOperandEq = BSON("1" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("1"_sd, baseOperandEq["1"]);
    verifyDebugString(gctx, eq.get(), "ComparisonMatchExpressionEq");

    auto baseOperandGt = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperandGt["$gt"]);
    verifyDebugString(gctx, gt.get(), "ComparisonMatchExpressionGt");

    auto baseOperandGte = BSON("$gte" << 5);
    auto gte = std::make_unique<GTEMatchExpression>(""_sd, baseOperandGte["$gte"]);
    verifyDebugString(gctx, gte.get(), "ComparisonMatchExpressionGte");

    auto baseOperandLt = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>(""_sd, baseOperandLt["$lt"]);
    verifyDebugString(gctx, lt.get(), "ComparisonMatchExpressionLt");

    auto baseOperandLte = BSON("$lte" << 5);
    auto lte = std::make_unique<LTEMatchExpression>(""_sd, baseOperandLte["$lte"]);
    verifyDebugString(gctx, lte.get(), "ComparisonMatchExpressionLte");

    // RegexMatchExpression.
    RegexMatchExpression regex(""_sd, "^ab", "");
    // Additional space before regex since path is not defined.
    verifyDebugString(gctx, &regex, "RegexMatchExpression");

    // ModMatchExpression.
    ModMatchExpression mod(""_sd, 3, 1);
    verifyDebugString(gctx, &mod, "ModMatchExpression");

    // ExistsMatchExpression.
    ExistsMatchExpression exists("a"_sd);
    verifyDebugString(gctx, &exists, "ExistsMatchExpression");

    // InMatchExpression.
    InMatchExpression in("a"_sd);
    verifyDebugString(gctx, &in, "InMatchExpression");

    // BitTestMatchExpression.
    std::vector<uint32_t> bitPositions;
    BitsAllSetMatchExpression balls("a"_sd, bitPositions);
    verifyDebugString(gctx, &balls, "BitsAllSetMatchExpression");
}

TEST(DebugStringTest, ExpressionText) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    auto expr = BSON("$text" << BSON("$search" << "something"));
    auto tme = uassertStatusOK(
        MatchExpressionParser::parse(expr,
                                     new ExpressionContextForTest(),
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));
    verifyDebugString(gctx, tme.get(), "TextMatchExpression");
}

TEST(DebugStringTest, ExpressionTree) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    auto baseOperand1 = BSON("$lt" << "z1");
    auto baseOperand2 = BSON("$gt" << "a1");
    auto sub1 = std::make_unique<LTMatchExpression>("a"_sd, baseOperand1["$lt"]);
    auto sub2 = std::make_unique<GTMatchExpression>("a"_sd, baseOperand2["$gt"]);
    auto sub3 = std::make_unique<RegexMatchExpression>("a"_sd, "1", "");

    // AndMatchExpression.
    auto andOp = AndMatchExpression{};
    andOp.add(sub1->clone());
    andOp.add(sub2->clone());
    andOp.add(sub3->clone());
    verifyDebugString(gctx, &andOp, "AndMatchExpression");

    // OrMatchExpression.
    auto orOp = OrMatchExpression{};
    orOp.add(sub1->clone());
    orOp.add(sub2->clone());
    orOp.add(sub3->clone());
    verifyDebugString(gctx, &orOp, "OrMatchExpression");

    // NorMatchExpression.
    auto norOp = NorMatchExpression{};
    norOp.add(sub1->clone());
    norOp.add(sub2->clone());
    norOp.add(sub3->clone());
    verifyDebugString(gctx, &norOp, "NorMatchExpression");

    // NotMatchExpression.
    auto notOp = NotMatchExpression{std::move(sub1)};
    verifyDebugString(gctx, &notOp, "NotMatchExpression");
}

TEST(DebugStringTest, ExpressionType) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;

    TypeMatchExpression typeExpr("a"_sd, typeSet);
    verifyDebugString(gctx, &typeExpr, "TypeMatchExpression");

    InternalSchemaTypeExpression schemaType("a"_sd, typeSet);
    verifyDebugString(gctx, &schemaType, "InternalSchemaTypeExpression");

    InternalSchemaBinDataSubTypeExpression binType(""_sd, BinDataType::BinDataGeneral);
    verifyDebugString(gctx, &binType, "InternalSchemaBinDataSubTypeExpression");
}

TEST(DebugStringTest, ExpressionWhere) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    auto expr = BSON("$where" << "function() {print(where);}");
    auto whereExpr = uassertStatusOK(
        MatchExpressionParser::parse(expr,
                                     new ExpressionContextForTest(),
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));
    verifyDebugString(gctx, whereExpr.get(), "WhereMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaAllowedProperties) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    auto filter = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a', 'b'],"
        "namePlaceholder: 'i', patternProperties: [], otherwise: {i: 0}}}");
    auto expr =
        uassertStatusOK(MatchExpressionParser::parse(filter, new ExpressionContextForTest()));
    verifyDebugString(gctx, expr.get(), "InternalSchemaAllowedPropertiesMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaAllElemMatchFromIndex) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {}]}}");
    auto expr =
        uassertStatusOK(MatchExpressionParser::parse(query, new ExpressionContextForTest()));
    verifyDebugString(gctx, expr.get(), "InternalSchemaAllElemMatchFromIndexMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaEqMatchExpression) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    BSONObj numberOperand = BSON("a" << 5);
    InternalSchemaEqMatchExpression eqNumberOperand("a"_sd, numberOperand["a"]);
    verifyDebugString(gctx, &eqNumberOperand, "InternalSchemaEqMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaFmod) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    auto query =
        fromjson("{a: {$_internalSchemaFmod: [NumberDecimal('2.3'), NumberDecimal('1.1')]}}");
    auto expr =
        uassertStatusOK(MatchExpressionParser::parse(query, new ExpressionContextForTest()));
    verifyDebugString(gctx, expr.get(), "InternalSchemaFmodMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaMatchArrayIndex) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    auto query = fromjson(
        "{a: {$_internalSchemaMatchArrayIndex: {index: 2, namePlaceholder: 'i', expression: {i: "
        "{$lt: 3}}}}}");
    auto expr =
        uassertStatusOK(MatchExpressionParser::parse(query, new ExpressionContextForTest()));
    verifyDebugString(gctx, expr.get(), "InternalSchemaMatchArrayIndexMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaNumArrayItems) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    InternalSchemaMaxItemsMatchExpression maxItems("a"_sd, 2);
    verifyDebugString(gctx, &maxItems, "InternalSchemaMaxItemsMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaNumProperties) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    InternalSchemaMaxPropertiesMatchExpression maxProperties(2);
    verifyDebugString(gctx, &maxProperties, "InternalSchemaMaxPropertiesMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaObjectMatch) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    BSONObj query = fromjson("{x: {$_internalSchemaObjectMatch: {y: 1}}}");
    auto expr = uassertStatusOK(
        MatchExpressionParser::parse(query,
                                     new ExpressionContextForTest(),
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));
    verifyDebugString(gctx, expr.get(), "InternalSchemaObjectMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaRootDocEq) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    InternalSchemaRootDocEqMatchExpression rootDocEq(BSON("a" << 1 << "b" << BSON("c" << 1)));
    verifyDebugString(gctx, &rootDocEq, "InternalSchemaRootDocEqMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaStrLength) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    InternalSchemaMaxLengthMatchExpression maxLength("a"_sd, 1);
    verifyDebugString(gctx, &maxLength, "InternalSchemaMaxLengthMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaUniqueItems) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    InternalSchemaUniqueItemsMatchExpression expr("foo"_sd);
    verifyDebugString(gctx, &expr, "InternalSchemaUniqueItemsMatchExpression");
}

TEST(DebugStringTest, ExpressionInternalSchemaXor) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    BSONObj matchPredicate =
        fromjson("{$_internalSchemaXor: [{a: { $gt: 10 }}, {a: { $lt: 0 }}, {b: 0}]}");
    auto expr = uassertStatusOK(
        MatchExpressionParser::parse(matchPredicate, new ExpressionContextForTest()));
    verifyDebugString(gctx, expr.get(), "InternalSchemaXorMatchExpression");
}

}  // namespace mongo
