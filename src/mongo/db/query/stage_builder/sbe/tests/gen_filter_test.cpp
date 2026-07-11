// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/gen_filter.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::stage_builder {
using namespace std::literals::string_view_literals;

class GoldenSbeFilterBuilderTestFixture : public GoldenSbeExprBuilderTestFixture {
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
                 std::string_view test,
                 PlanStageSlots slots = {}) {
        auto sbExpr = generateFilter(*_state, expr, rootSlot, slots, isFilterOverIxscan);

        auto [expectedTag, expectedVal] = sbe::value::makeValue(Value(expected));
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        GoldenSbeExprBuilderTestFixture::runTest(std::move(sbExpr), expectedTag, expectedVal, test);
    }
};

TEST_F(GoldenSbeFilterBuilderTestFixture, TestSimpleExpr) {
    auto root =
        BSON("_id" << 0 << "field1" << 5 << "arr" << BSON_ARRAY(4 << BSON("a" << 5)) << "str"
                   << "abc");
    auto rootSlotId = _env->registerSlot("root"sv,
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
                "AlwaysFalseMatchExpression"sv);
    }
    {
        AlwaysTrueMatchExpression alwaysTrueExpr{};
        runTest(&alwaysTrueExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "AlwaysTrueMatchExpression"sv);
    }
    {
        auto eq = std::make_unique<EqualityMatchExpression>("a"sv, Value(5));
        ElemMatchObjectMatchExpression elemMatchObjExpr("arr"sv, std::move(eq));
        runTest(&elemMatchObjExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "ElemMatchObjectMatchExpression"sv);
    }
    {
        auto gt = std::make_unique<GTMatchExpression>(""sv, Value(3));
        ElemMatchValueMatchExpression elemMatchValExpr("arr"sv, std::move(gt));
        runTest(&elemMatchValExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "ElemMatchValueMatchExpression"sv);
    }
    {
        ExistsMatchExpression existsExpr("not-exist"sv);
        runTest(&existsExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "ExistsMatchExpression"sv);
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
                "ExprMatchExpression"sv);
    }
    {
        InMatchExpression inExpr("field1"sv);
        BSONArray arr = BSON_ARRAY(3 << 4 << 5);
        ASSERT_OK(inExpr.setEqualitiesArray(std::move(arr)));
        runTest(&inExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InMatchExpression"sv);
    }
    {
        InMatchExpression inExpr("str"sv);
        ASSERT_OK(inExpr.addRegex(std::make_unique<RegexMatchExpression>(""sv, "ABc", "i")));
        BSONArray arr = BSON_ARRAY("3" << "4" << BSONNULL);
        ASSERT_OK(inExpr.setEqualitiesArray(std::move(arr)));
        runTest(&inExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InMatchExpressionRegex"sv);
    }
    {
        ModMatchExpression modExpr("field1"sv, 4, 1);
        runTest(&modExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "ModMatchExpression"sv);
    }
    {
        NorMatchExpression norExpr{};
        norExpr.add(std::make_unique<EqualityMatchExpression>("field1"sv, Value(4)));
        runTest(&norExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "NorMatchExpression"sv);
    }
    {
        RegexMatchExpression regexExpr("str"sv, "ABc", "i");
        runTest(&regexExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "RegexMatchExpression"sv);
    }
    {
        SizeMatchExpression sizeExpr("str"sv, 4);
        runTest(&sizeExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "SizeMatchExpression"sv);
    }
    {
        TypeMatchExpression typeExpr("field1"sv, MatcherTypeSet{BSONType::numberInt});
        runTest(&typeExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "TypeMatchExpression"sv);
    }
}

TEST_F(GoldenSbeFilterBuilderTestFixture, TestBitsExpr) {
    auto root = BSON("_id" << 0 << "field1" << 5);
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};
    {
        // 35 has 0, 1, 5 bit positions set.
        BitsAllClearMatchExpression bitsAllClearExp("field1"sv, 35);
        runTest(&bitsAllClearExp,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "BitsAllClearMatchExpression"sv);
    }
    {
        BitsAllSetMatchExpression bitsAllSetExp("field1"sv, {0, 2});
        runTest(&bitsAllSetExp,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "BitsAllSetMatchExpression"sv);
    }
    {
        // 137 has 0, 3, 7 bit positions set.
        BitsAnyClearMatchExpression bitsAnyClearExp("field1"sv, 137);
        runTest(&bitsAnyClearExp,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "BitsAnyClearMatchExpression"sv);
    }
    {
        BitsAnySetMatchExpression bitsAnySetExp("field1"sv, {0, 2});
        runTest(&bitsAnySetExp,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "BitsAnySetMatchExpression"sv);
    }
}

TEST_F(GoldenSbeFilterBuilderTestFixture, TestCompExpr) {
    auto root = BSON("_id" << 0 << "field1" << 5);
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};
    {
        auto idxFieldSlotId = _env->registerSlot(
            "field1"sv, sbe::value::TypeTags::NumberInt32, 5 /* val */, true, &_slotIdGenerator);
        PlanStageSlots slots;
        slots.set(std::make_pair(PlanStageSlots::kField, "field1"sv), SbSlot{idxFieldSlotId});
        GTEMatchExpression gteExpr("field1"sv, Value(1));
        runTest(&gteExpr,
                rootSlot,
                true /* isFilterOverIxscan */,
                true /* expected */,
                "GTEMatchExpression_isFilterOverIxscan"sv,
                slots);
    }
    {
        GTEMatchExpression gteExpr("field1"sv, Value(MinKeyLabeler{}));
        runTest(&gteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "GTEMatchExpression_MinKey"sv);
    }
    {
        GTEMatchExpression gteExpr("field1"sv, Value(MaxKeyLabeler{}));
        runTest(&gteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "GTEMatchExpression_MaxKey"sv);
    }
    {
        GTMatchExpression gtExpr("field1"sv, Value(MinKeyLabeler{}));
        runTest(&gtExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "GTMatchExpression_MinKey"sv);
    }
    {
        GTMatchExpression gtExpr("field1"sv, Value(MaxKeyLabeler{}));
        runTest(&gtExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "GTMatchExpression_MaxKey"sv);
    }
    {
        InternalExprEqMatchExpression eqExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&eqExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InternalExprEqMatchExpression"sv);
    }
    {
        InternalExprGTMatchExpression gtExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&gtExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "InternalExprGTMatchExpression"sv);
    }
    {
        InternalExprGTEMatchExpression gteExpr(root.firstElement().fieldNameStringData(),
                                               root.firstElement());
        runTest(&gteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InternalExprGTEMatchExpression"sv);
    }
    {
        InternalExprLTMatchExpression ltExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&ltExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "InternalExprLTMatchExpression"sv);
    }
    {
        InternalExprLTEMatchExpression lteExpr(root.firstElement().fieldNameStringData(),
                                               root.firstElement());
        runTest(&lteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "InternalExprLTEMatchExpression"sv);
    }
    {
        LTEMatchExpression lteExpr("field1"sv, Value(10));
        runTest(&lteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "LTEMatchExpression"sv);
    }
    {
        LTEMatchExpression lteExpr("field1"sv, Value(MaxKeyLabeler{}));
        runTest(&lteExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "LTEMatchExpression_MaxKey"sv);
    }
    {
        LTMatchExpression ltExpr("field1"sv, Value(10));
        runTest(&ltExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                true /* expected */,
                "LTMatchExpression"sv);
    }
    {
        LTMatchExpression ltExpr("field1"sv, Value(MinKeyLabeler{}));
        runTest(&ltExpr,
                rootSlot,
                false /* isFilterOverIxscan */,
                false /* expected */,
                "LTMatchExpression_MinKey"sv);
    }
}

TEST_F(GoldenSbeFilterBuilderTestFixture, InternalExprEqOnDottedPathOverIxscan) {
    auto abSlotId = _env->registerSlot("aDotB"sv,
                                       sbe::value::TypeTags::NumberInt32,
                                       0 /* val */,
                                       true /* owned */,
                                       &_slotIdGenerator);
    PlanStageSlots slots;
    slots.set(std::make_pair(PlanStageSlots::kField, "a.b"sv), SbSlot{abSlotId});

    auto rhs = BSON("" << 0);
    InternalExprEqMatchExpression eqExpr("a.b"sv, rhs.firstElement());

    runTest(&eqExpr,
            boost::none /* rootSlot, as at the IXSCAN level */,
            true /* isFilterOverIxscan */,
            true /* expected: slot value 0 == 0 */,
            "InternalExprEqOnDottedPathOverIxscan"sv,
            std::move(slots));
}

class GoldenSbeFilterBuilderArraynessTestFixture : public GoldenSbeFilterBuilderTestFixture {
public:
    void setUp() override {
        GoldenSbeExprBuilderTestFixture::setUp();
        _gctx->validateOnClose(true);

        auto pathArrayness = std::make_shared<PathArrayness>();
        pathArrayness->addPath(FieldPath("a.b"), MultikeyComponents{}, true /* isFullRebuild */);
        _expCtx->setPathArraynessForNss(_expCtx->getNamespaceString(), std::move(pathArrayness));
    }

    void runTestWithPathArrayness(const MatchExpression* expr,
                                  boost::optional<SbSlot> rootSlot,
                                  bool expected,
                                  std::string_view test,
                                  PlanStageSlots slots = {}) {
        auto sbExpr = generateFilter(*_state,
                                     expr,
                                     rootSlot,
                                     slots,
                                     /*isFilterOverIxscan*/ false,
                                     /*canUsePathArrayness*/ true);

        auto [expectedTag, expectedVal] = sbe::value::makeValue(Value(expected));
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        GoldenSbeExprBuilderTestFixture::runTest(std::move(sbExpr), expectedTag, expectedVal, test);
    }
};

TEST_F(GoldenSbeFilterBuilderArraynessTestFixture, TestPathArraynessTraverseFElision) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    auto root = BSON("a" << BSON("b" << 1) << "c" << BSON("d" << 1));
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    {
        EqualityMatchExpression eqExpr("a.b"sv, Value(1));
        runTestWithPathArrayness(
            &eqExpr, rootSlot, true /* expected */, "TraverseFElided_KnownNonArrayPath"sv);
    }
    {
        EqualityMatchExpression eqExpr("c.d"sv, Value(1));
        runTestWithPathArrayness(
            &eqExpr, rootSlot, true /* expected */, "TraverseFRetained_UnknownPath"sv);
    }
}

TEST_F(GoldenSbeFilterBuilderArraynessTestFixture, TestNothingCheckWithPathArrayness) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    // Document with scalar intermediate: "a" is 42, so getField(42, "b") returns Nothing.
    // All null-matching predicates should return true (field path doesn't exist).
    {
        auto root = BSON("a" << 42);
        auto rootSlotId = _env->registerSlot("scalarRoot"sv,
                                             sbe::value::TypeTags::bsonObject,
                                             sbe::value::bitcastFrom<const char*>(root.objdata()),
                                             false,
                                             &_slotIdGenerator);
        auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

        {
            EqualityMatchExpression eqExpr("a.b"sv, Value(BSONNULL));
            runTestWithPathArrayness(
                &eqExpr, rootSlot, true /* expected */, "NothingCheck_EqNull_ScalarIntermediate"sv);
        }
        {
            LTEMatchExpression lteExpr("a.b"sv, Value(BSONNULL));
            runTestWithPathArrayness(&lteExpr,
                                     rootSlot,
                                     true /* expected */,
                                     "NothingCheck_LteNull_ScalarIntermediate"sv);
        }
        {
            GTEMatchExpression gteExpr("a.b"sv, Value(BSONNULL));
            runTestWithPathArrayness(&gteExpr,
                                     rootSlot,
                                     true /* expected */,
                                     "NothingCheck_GteNull_ScalarIntermediate"sv);
        }
        {
            InMatchExpression inExpr("a.b"sv);
            BSONArray arr = BSON_ARRAY(BSONNULL);
            ASSERT_OK(inExpr.setEqualitiesArray(std::move(arr)));
            runTestWithPathArrayness(
                &inExpr, rootSlot, true /* expected */, "NothingCheck_InNull_ScalarIntermediate"sv);
        }
    }

    // Document with object intermediate: "a.b" is 1, not null.
    {
        auto root = BSON("a" << BSON("b" << 1));
        auto rootSlotId = _env->registerSlot("objectRoot"sv,
                                             sbe::value::TypeTags::bsonObject,
                                             sbe::value::bitcastFrom<const char*>(root.objdata()),
                                             false,
                                             &_slotIdGenerator);
        auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

        {
            EqualityMatchExpression eqExpr("a.b"sv, Value(BSONNULL));
            runTestWithPathArrayness(&eqExpr,
                                     rootSlot,
                                     false /* expected */,
                                     "NothingCheck_EqNull_ObjectIntermediate"sv);
        }
    }

    // Document with missing field: "a" doesn't exist, so path is entirely absent.
    {
        auto root = BSONObj();
        auto rootSlotId = _env->registerSlot("emptyRoot"sv,
                                             sbe::value::TypeTags::bsonObject,
                                             sbe::value::bitcastFrom<const char*>(root.objdata()),
                                             false,
                                             &_slotIdGenerator);
        auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

        {
            EqualityMatchExpression eqExpr("a.b"sv, Value(BSONNULL));
            runTestWithPathArrayness(
                &eqExpr, rootSlot, true /* expected */, "NothingCheck_EqNull_MissingField"sv);
        }
    }
}

TEST_F(GoldenSbeFilterBuilderArraynessTestFixture, TestNothingCheckWithPathArraynessDisabledFix) {
    unittest::ServerParameterGuard disableFix{"internalQueryLegacyDottedPathNullSemantics", true};
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};
    // Reinitialize _state so it captures the knob value set above.
    reinitState();

    // With the fix disabled, an array containing scalars at an intermediate path does NOT match a
    // null predicate on the dotted path. The pre-SERVER-36681 behavior only matches when the
    // intermediate value is an object or array.
    {
        auto root = BSON("a" << BSON_ARRAY(42));
        auto rootSlotId = _env->registerSlot("arrayScalarRoot"sv,
                                             sbe::value::TypeTags::bsonObject,
                                             sbe::value::bitcastFrom<const char*>(root.objdata()),
                                             false,
                                             &_slotIdGenerator);
        auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

        EqualityMatchExpression eqExpr("a.b"sv, Value(BSONNULL));
        runTestWithPathArrayness(
            &eqExpr,
            rootSlot,
            false /* expected: original behavior returns false for scalar in array */,
            "DisabledFix_EqNull_ArrayWithScalarIntermediate"sv);
    }

    // With the fix disabled, an empty array at an intermediate path does NOT match a null
    // predicate on the dotted path.
    {
        auto root = BSON("a" << BSONArray());
        auto rootSlotId = _env->registerSlot("emptyArrayRoot"sv,
                                             sbe::value::TypeTags::bsonObject,
                                             sbe::value::bitcastFrom<const char*>(root.objdata()),
                                             false,
                                             &_slotIdGenerator);
        auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

        EqualityMatchExpression eqExpr("a.b"sv, Value(BSONNULL));
        runTestWithPathArrayness(
            &eqExpr,
            rootSlot,
            false /* expected: original behavior returns false for empty array */,
            "DisabledFix_EqNull_EmptyArrayIntermediate"sv);
    }

    // With the fix disabled, a scalar (non-array) intermediate still returns true because the
    // pre-fix code falls back to checking if the parent field exists.
    {
        auto root = BSON("a" << 42);
        auto rootSlotId = _env->registerSlot("scalarRoot2"sv,
                                             sbe::value::TypeTags::bsonObject,
                                             sbe::value::bitcastFrom<const char*>(root.objdata()),
                                             false,
                                             &_slotIdGenerator);
        auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

        EqualityMatchExpression eqExpr("a.b"sv, Value(BSONNULL));
        runTestWithPathArrayness(
            &eqExpr,
            rootSlot,
            true /* expected: scalar intermediate still matches (exists check on parent) */,
            "DisabledFix_EqNull_ScalarIntermediate"sv);
    }
}

}  // namespace mongo::stage_builder
