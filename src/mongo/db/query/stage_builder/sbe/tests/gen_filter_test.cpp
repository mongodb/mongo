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

#include "mongo/db/query/stage_builder/sbe/gen_filter.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace mongo::stage_builder {

class GoldenGenFilternTest : public GoldenSbeExprBuilderTestFixture {
public:
    void setUp() override {
        GoldenSbeExprBuilderTestFixture::setUp();
        _gctx->validateOnClose(true);
    }

    void tearDown() override {
        GoldenSbeExprBuilderTestFixture::tearDown();
    }

    void runTest(const MatchExpression* expr,
                 boost::optional<SbSlot> rootSlot,
                 bool isFilterOverIxscan,
                 bool expected,
                 StringData test,
                 PlanStageSlots slots = {}) {
        auto sbExpr = generateFilter(*_state, expr, rootSlot, slots, isFilterOverIxscan);

        auto [expectedTag, expectedVal] = sbe::value::makeValue(Value(expected));
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        GoldenSbeExprBuilderTestFixture::runTest(std::move(sbExpr), expectedTag, expectedVal, test);
    }
};

TEST_F(GoldenGenFilternTest, TestSimpleExpr) {
    auto root =
        BSON("_id" << 0 << "field1" << 5 << "arr" << BSON_ARRAY(4 << BSON("a" << 5)) << "str"
                   << "abc");
    auto rootSlotId = _env->registerSlot("root"_sd,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    {
        AlwaysFalseMatchExpression alwaysFalseExpr{};
        runTest(&alwaysFalseExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "AlwaysFalseMatchExpression"_sd);
    }
    {
        AlwaysTrueMatchExpression alwaysTrueExpr{};
        runTest(&alwaysTrueExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "AlwaysTrueMatchExpression"_sd);
    }
    {
        auto eq = std::make_unique<EqualityMatchExpression>("a"_sd, Value(5));
        ElemMatchObjectMatchExpression elemMatchObjExpr("arr"_sd, std::move(eq));
        runTest(&elemMatchObjExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "ElemMatchObjectMatchExpression"_sd);
    }
    {
        auto gt = std::make_unique<GTMatchExpression>(""_sd, Value(3));
        ElemMatchValueMatchExpression elemMatchValExpr("arr"_sd, std::move(gt));
        runTest(&elemMatchValExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "ElemMatchValueMatchExpression"_sd);
    }
    {
        ExistsMatchExpression existsExpr("not-exist"_sd);
        runTest(&existsExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "ExistsMatchExpression"_sd);
    }
    {
        auto field1Expr = ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "field1", _expCtx->variablesParseState);
        Value val = Value(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        boost::intrusive_ptr<ExpressionIn> inExpr{
            new ExpressionIn(_expCtx.get(), {field1Expr, constExpr})};
        ExprMatchExpression exprExpr(inExpr, _expCtx.get());
        runTest(&exprExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "ExprMatchExpression"_sd);
    }
    {
        InMatchExpression inExpr("field1"_sd);
        BSONArray arr = BSON_ARRAY(3 << 4 << 5);
        ASSERT_OK(inExpr.setEqualitiesArray(std::move(arr)));
        runTest(&inExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InMatchExpression"_sd);
    }
    {
        InMatchExpression inExpr("str"_sd);
        ASSERT_OK(inExpr.addRegex(std::make_unique<RegexMatchExpression>(""_sd, "ABc", "i")));
        BSONArray arr = BSON_ARRAY("3" << "4" << BSONNULL);
        ASSERT_OK(inExpr.setEqualitiesArray(std::move(arr)));
        runTest(&inExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InMatchExpressionRegex"_sd);
    }
    {
        ModMatchExpression modExpr("field1"_sd, 4, 1);
        runTest(&modExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "ModMatchExpression"_sd);
    }
    {
        NorMatchExpression norExpr{};
        norExpr.add(std::make_unique<EqualityMatchExpression>("field1"_sd, Value(4)));
        runTest(&norExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "NorMatchExpression"_sd);
    }
    {
        RegexMatchExpression regexExpr("str"_sd, "ABc", "i");
        runTest(&regexExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "RegexMatchExpression"_sd);
    }
    {
        SizeMatchExpression sizeExpr("str"_sd, 4);
        runTest(&sizeExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "SizeMatchExpression"_sd);
    }
    {
        TypeMatchExpression typeExpr("field1"_sd, MatcherTypeSet{BSONType::numberInt});
        runTest(&typeExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "TypeMatchExpression"_sd);
    }
}

TEST_F(GoldenGenFilternTest, TestBitsExpr) {
    auto root = BSON("_id" << 0 << "field1" << 5);
    auto rootSlotId = _env->registerSlot("root"_sd,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};
    {
        // 35 has 0, 1, 5 bit positions set.
        BitsAllClearMatchExpression bitsAllClearExp("field1"_sd, 35);
        runTest(&bitsAllClearExp,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "BitsAllClearMatchExpression"_sd);
    }
    {
        BitsAllSetMatchExpression bitsAllSetExp("field1"_sd, {0, 2});
        runTest(&bitsAllSetExp,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "BitsAllSetMatchExpression"_sd);
    }
    {
        // 137 has 0, 3, 7 bit positions set.
        BitsAnyClearMatchExpression bitsAnyClearExp("field1"_sd, 137);
        runTest(&bitsAnyClearExp,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "BitsAnyClearMatchExpression"_sd);
    }
    {
        BitsAnySetMatchExpression bitsAnySetExp("field1"_sd, {0, 2});
        runTest(&bitsAnySetExp,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "BitsAnySetMatchExpression"_sd);
    }
}

TEST_F(GoldenGenFilternTest, TestCompExpr) {
    auto root = BSON("_id" << 0 << "field1" << 5);
    auto rootSlotId = _env->registerSlot("root"_sd,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};
    {
        auto idxFieldSlotId = _env->registerSlot(
            "field1"_sd, sbe::value::TypeTags::NumberInt32, 5 /* val */, true, &_slotIdGenerator);
        PlanStageSlots slots;
        slots.set(std::make_pair(PlanStageSlots::kField, "field1"_sd), SbSlot{idxFieldSlotId});
        GTEMatchExpression gteExpr("field1"_sd, Value(1));
        runTest(&gteExpr,
                rootSlot,
                true /* isFilterOverIxscan */,
                true /* expected */,
                "GTEMatchExpression_isFilterOverIxscan"_sd,
                slots);
    }
    {
        GTEMatchExpression gteExpr("field1"_sd, Value(MinKeyLabeler{}));
        runTest(&gteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "GTEMatchExpression_MinKey"_sd);
    }
    {
        GTEMatchExpression gteExpr("field1"_sd, Value(MaxKeyLabeler{}));
        runTest(&gteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "GTEMatchExpression_MaxKey"_sd);
    }
    {
        GTMatchExpression gtExpr("field1"_sd, Value(MinKeyLabeler{}));
        runTest(&gtExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "GTMatchExpression_MinKey"_sd);
    }
    {
        GTMatchExpression gtExpr("field1"_sd, Value(MaxKeyLabeler{}));
        runTest(&gtExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "GTMatchExpression_MaxKey"_sd);
    }
    {
        InternalExprEqMatchExpression eqExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&eqExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InternalExprEqMatchExpression"_sd);
    }
    {
        InternalExprGTMatchExpression gtExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&gtExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "InternalExprGTMatchExpression"_sd);
    }
    {
        InternalExprGTEMatchExpression gteExpr(root.firstElement().fieldNameStringData(),
                                               root.firstElement());
        runTest(&gteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InternalExprGTEMatchExpression"_sd);
    }
    {
        InternalExprLTMatchExpression ltExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&ltExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "InternalExprLTMatchExpression"_sd);
    }
    {
        InternalExprLTEMatchExpression lteExpr(root.firstElement().fieldNameStringData(),
                                               root.firstElement());
        runTest(&lteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InternalExprLTEMatchExpression"_sd);
    }
    {
        LTEMatchExpression lteExpr("field1"_sd, Value(10));
        runTest(&lteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "LTEMatchExpression"_sd);
    }
    {
        LTEMatchExpression lteExpr("field1"_sd, Value(MaxKeyLabeler{}));
        runTest(&lteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "LTEMatchExpression_MaxKey"_sd);
    }
    {
        LTMatchExpression ltExpr("field1"_sd, Value(10));
        runTest(&ltExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "LTMatchExpression"_sd);
    }
    {
        LTMatchExpression ltExpr("field1"_sd, Value(MinKeyLabeler{}));
        runTest(&ltExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "LTMatchExpression_MinKey"_sd);
    }
}

}  // namespace mongo::stage_builder
