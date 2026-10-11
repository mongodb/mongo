// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/gen_filter.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
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
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/assert.h"
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

        sbe::value::TagValueOwned expectedValue =
            sbe::value::TagValueOwned::fromRaw(sbe::value::makeValue(Value(expected)));
        GoldenSbeExprBuilderTestFixture::runTest(
            std::move(sbExpr), expectedValue.tag(), expectedValue.value(), test);
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
                                     /*isFilterOverIxscan=*/false,
                                     /*canUsePathArrayness=*/true);

        sbe::value::TagValueOwned expectedValue =
            sbe::value::TagValueOwned::fromRaw(sbe::value::makeValue(Value(expected)));
        GoldenSbeExprBuilderTestFixture::runTest(
            std::move(sbExpr), expectedValue.tag(), expectedValue.value(), test);
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
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
                "AlwaysFalseMatchExpression"sv);
    }
    {
        AlwaysTrueMatchExpression alwaysTrueExpr{};
        runTest(&alwaysTrueExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "AlwaysTrueMatchExpression"sv);
    }
    {
        auto eq = std::make_unique<EqualityMatchExpression>("a"sv, Value(5));
        ElemMatchObjectMatchExpression elemMatchObjExpr("arr"sv, std::move(eq));
        runTest(&elemMatchObjExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "ElemMatchObjectMatchExpression"sv);
    }
    {
        auto gt = std::make_unique<GTMatchExpression>(""sv, Value(3));
        ElemMatchValueMatchExpression elemMatchValExpr("arr"sv, std::move(gt));
        runTest(&elemMatchValExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "ElemMatchValueMatchExpression"sv);
    }
    {
        ExistsMatchExpression existsExpr("not-exist"sv);
        runTest(&existsExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
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
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "ExprMatchExpression"sv);
    }
    {
        InMatchExpression inExpr("field1"sv);
        BSONArray arr = BSON_ARRAY(3 << 4 << 5);
        ASSERT_OK(inExpr.setEqualitiesArray(std::move(arr)));
        runTest(&inExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "InMatchExpression"sv);
    }
    {
        InMatchExpression inExpr("str"sv);
        ASSERT_OK(inExpr.addRegex(std::make_unique<RegexMatchExpression>(""sv, "ABc", "i")));
        BSONArray arr = BSON_ARRAY("3" << "4" << BSONNULL);
        ASSERT_OK(inExpr.setEqualitiesArray(std::move(arr)));
        runTest(&inExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "InMatchExpressionRegex"sv);
    }
    {
        ModMatchExpression modExpr("field1"sv, 4, 1);
        runTest(&modExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "ModMatchExpression"sv);
    }
    {
        NorMatchExpression norExpr{};
        norExpr.add(std::make_unique<EqualityMatchExpression>("field1"sv, Value(4)));
        runTest(&norExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "NorMatchExpression"sv);
    }
    {
        RegexMatchExpression regexExpr("str"sv, "ABc", "i");
        runTest(&regexExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "RegexMatchExpression"sv);
    }
    {
        SizeMatchExpression sizeExpr("str"sv, 4);
        runTest(&sizeExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
                "SizeMatchExpression"sv);
    }
    {
        TypeMatchExpression typeExpr("field1"sv, MatcherTypeSet{BSONType::numberInt});
        runTest(&typeExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
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
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
                "BitsAllClearMatchExpression"sv);
    }
    {
        BitsAllSetMatchExpression bitsAllSetExp("field1"sv, {0, 2});
        runTest(&bitsAllSetExp,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "BitsAllSetMatchExpression"sv);
    }
    {
        // 137 has 0, 3, 7 bit positions set.
        BitsAnyClearMatchExpression bitsAnyClearExp("field1"sv, 137);
        runTest(&bitsAnyClearExp,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "BitsAnyClearMatchExpression"sv);
    }
    {
        BitsAnySetMatchExpression bitsAnySetExp("field1"sv, {0, 2});
        runTest(&bitsAnySetExp,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
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
            "field1"sv, sbe::value::TypeTags::NumberInt32, /*val=*/5, true, &_slotIdGenerator);
        PlanStageSlots slots;
        slots.set(std::make_pair(PlanStageSlots::kField, "field1"sv), SbSlot{idxFieldSlotId});
        GTEMatchExpression gteExpr("field1"sv, Value(1));
        runTest(&gteExpr,
                rootSlot,
                /*isFilterOverIxscan=*/true,
                /*expected=*/true,
                "GTEMatchExpression_isFilterOverIxscan"sv,
                slots);
    }
    {
        GTEMatchExpression gteExpr("field1"sv, Value(MinKeyLabeler{}));
        runTest(&gteExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "GTEMatchExpression_MinKey"sv);
    }
    {
        GTEMatchExpression gteExpr("field1"sv, Value(MaxKeyLabeler{}));
        runTest(&gteExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
                "GTEMatchExpression_MaxKey"sv);
    }
    {
        GTMatchExpression gtExpr("field1"sv, Value(MinKeyLabeler{}));
        runTest(&gtExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "GTMatchExpression_MinKey"sv);
    }
    {
        GTMatchExpression gtExpr("field1"sv, Value(MaxKeyLabeler{}));
        runTest(&gtExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
                "GTMatchExpression_MaxKey"sv);
    }
    {
        InternalExprEqMatchExpression eqExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&eqExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "InternalExprEqMatchExpression"sv);
    }
    {
        InternalExprGTMatchExpression gtExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&gtExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
                "InternalExprGTMatchExpression"sv);
    }
    {
        InternalExprGTEMatchExpression gteExpr(root.firstElement().fieldNameStringData(),
                                               root.firstElement());
        runTest(&gteExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "InternalExprGTEMatchExpression"sv);
    }
    {
        InternalExprLTMatchExpression ltExpr(root.firstElement().fieldNameStringData(),
                                             root.firstElement());
        runTest(&ltExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
                "InternalExprLTMatchExpression"sv);
    }
    {
        InternalExprLTEMatchExpression lteExpr(root.firstElement().fieldNameStringData(),
                                               root.firstElement());
        runTest(&lteExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "InternalExprLTEMatchExpression"sv);
    }
    {
        LTEMatchExpression lteExpr("field1"sv, Value(10));
        runTest(&lteExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "LTEMatchExpression"sv);
    }
    {
        LTEMatchExpression lteExpr("field1"sv, Value(MaxKeyLabeler{}));
        runTest(&lteExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "LTEMatchExpression_MaxKey"sv);
    }
    {
        LTMatchExpression ltExpr("field1"sv, Value(10));
        runTest(&ltExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/true,
                "LTMatchExpression"sv);
    }
    {
        LTMatchExpression ltExpr("field1"sv, Value(MinKeyLabeler{}));
        runTest(&ltExpr,
                rootSlot,
                /*isFilterOverIxscan=*/false,
                /*expected=*/false,
                "LTMatchExpression_MinKey"sv);
    }
}

TEST_F(GoldenSbeFilterBuilderTestFixture, InternalExprEqOnDottedPathOverIxscan) {
    auto abSlotId = _env->registerSlot("aDotB"sv,
                                       sbe::value::TypeTags::NumberInt32,
                                       /*val=*/0,
                                       /*owned=*/true,
                                       &_slotIdGenerator);
    PlanStageSlots slots;
    slots.set(std::make_pair(PlanStageSlots::kField, "a.b"sv), SbSlot{abSlotId});

    auto rhs = BSON("" << 0);
    InternalExprEqMatchExpression eqExpr("a.b"sv, rhs.firstElement());

    runTest(&eqExpr,
            boost::none /* rootSlot, as at the IXSCAN level */,
            /*isFilterOverIxscan=*/true,
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
        pathArrayness->addPath(FieldPath("a.b"), MultikeyComponents{}, /*isFullRebuild=*/true);
        _expCtx->setPathArraynessForNss(_expCtx->getNamespaceString(), std::move(pathArrayness));
    }
};

class GoldenSbeFilterBuilderElemMatchShadowFixture : public GoldenSbeFilterBuilderTestFixture {
public:
    void setUp() override {
        GoldenSbeFilterBuilderTestFixture::setUp();

        // Mark the root paths "b", "e", "g" and "c.d" as known non-array, as non-multikey
        // root indexes would. Element-relative paths with the same names inside $elemMatch
        // describe fields of the array elements, not of the root document, and must not
        // inherit this metadata.
        auto pathArrayness = std::make_shared<PathArrayness>();
        pathArrayness->addPath(FieldPath("b"), MultikeyComponents{}, true /* isFullRebuild */);
        pathArrayness->addPath(FieldPath("e"), MultikeyComponents{}, true /* isFullRebuild */);
        pathArrayness->addPath(FieldPath("g"), MultikeyComponents{}, true /* isFullRebuild */);
        pathArrayness->addPath(FieldPath("c.d"), MultikeyComponents{}, true /* isFullRebuild */);
        _expCtx->setPathArraynessForNss(_expCtx->getNamespaceString(), std::move(pathArrayness));
    }

    // Parses 'filter' and runs it against 'doc' with canUsePathArrayness=true.
    void runParsedFilter(std::string_view filter,
                         const BSONObj& doc,
                         bool expected,
                         std::string_view test) {
        auto slot = registerDocSlot("root"sv, doc);
        runParsedFilterOnSlot(filter, slot, expected, test);
    }

    // Parses 'filter' and runs it against the already-registered document in 'slot'.
    void runParsedFilterOnSlot(std::string_view filter,
                               SbSlot slot,
                               bool expected,
                               std::string_view test) {
        // The parsed expression tree references the backing BSON of 'filter' (e.g. comparison
        // RHS elements), so the BSONObj must outlive generateFilter.
        _docs.push_back(fromjson(filter));
        auto expr = unittest::assertGet(MatchExpressionParser::parse(_docs.back(), _expCtx));
        runTestWithPathArrayness(expr.get(), slot, expected, test);
    }

protected:
    // The fixture owns every registered document so the slots stay valid for the whole test.
    SbSlot registerDocSlot(std::string_view name, const BSONObj& doc) {
        _docs.push_back(doc.copy());
        const BSONObj& owned = _docs.back();
        auto slotId = _env->registerSlot(name,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(owned.objdata()),
                                         false,
                                         &_slotIdGenerator);
        return SbSlot{slotId, TypeSignature::kObjectType};
    }

    std::vector<BSONObj> _docs;
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
            &eqExpr, rootSlot, /*expected=*/true, "TraverseFElided_KnownNonArrayPath"sv);
    }
    {
        EqualityMatchExpression eqExpr("c.d"sv, Value(1));
        runTestWithPathArrayness(
            &eqExpr, rootSlot, /*expected=*/true, "TraverseFRetained_UnknownPath"sv);
    }
}

TEST_F(GoldenSbeFilterBuilderArraynessTestFixture, TestPathArraynessTraverseFElisionKnobDisabled) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};
    unittest::ServerParameterGuard knob{"internalEnablePathArraynessTraverseFElision", false};

    // With the knob off, traverseF is retained even on a path known to be non-array.
    auto root = BSON("a" << BSON("b" << 1));
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    EqualityMatchExpression eqExpr("a.b"sv, Value(1));
    runTestWithPathArrayness(
        &eqExpr, rootSlot, true /* expected */, "TraverseFRetained_KnobDisabled"sv);
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
                &eqExpr, rootSlot, /*expected=*/true, "NothingCheck_EqNull_ScalarIntermediate"sv);
        }
        {
            LTEMatchExpression lteExpr("a.b"sv, Value(BSONNULL));
            runTestWithPathArrayness(&lteExpr,
                                     rootSlot,
                                     /*expected=*/true,
                                     "NothingCheck_LteNull_ScalarIntermediate"sv);
        }
        {
            GTEMatchExpression gteExpr("a.b"sv, Value(BSONNULL));
            runTestWithPathArrayness(&gteExpr,
                                     rootSlot,
                                     /*expected=*/true,
                                     "NothingCheck_GteNull_ScalarIntermediate"sv);
        }
        {
            InMatchExpression inExpr("a.b"sv);
            BSONArray arr = BSON_ARRAY(BSONNULL);
            ASSERT_OK(inExpr.setEqualitiesArray(std::move(arr)));
            runTestWithPathArrayness(
                &inExpr, rootSlot, /*expected=*/true, "NothingCheck_InNull_ScalarIntermediate"sv);
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
                                     /*expected=*/false,
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
                &eqExpr, rootSlot, /*expected=*/true, "NothingCheck_EqNull_MissingField"sv);
        }
    }
}

// Object-form $elemMatch predicates on the shadowed element-relative path "b". The
// element's "b" is the array [1, 2] and the root "b" is the scalar 9, so matching the
// array itself or the root value instead of the array's elements flips each expected
// result. $size evaluates the array value at the path without traverseF and doubles as a
// control for that semantics.
TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestElemMatchObjectShadowedPathEq) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    runParsedFilter("{arr: {$elemMatch: {b: 2}}}",
                    fromjson("{b: 9, arr: [{b: [1, 2]}]}"),
                    true,
                    "ShadowedPath_Eq"sv);
}

TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestElemMatchObjectShadowedPathIn) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    runParsedFilter("{arr: {$elemMatch: {b: {$in: [2, 7]}}}}",
                    fromjson("{b: 9, arr: [{b: [1, 2]}]}"),
                    true,
                    "ShadowedPath_In"sv);
}

TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestElemMatchObjectShadowedPathNotEq) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    runParsedFilter("{arr: {$elemMatch: {b: {$not: {$eq: 2}}}}}",
                    fromjson("{b: 9, arr: [{b: [1, 2]}]}"),
                    false,
                    "ShadowedPath_NotEq"sv);
}

TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestElemMatchObjectShadowedPathRegex) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    runParsedFilter("{arr: {$elemMatch: {b: {$regex: '^x'}}}}",
                    fromjson("{b: 9, arr: [{b: ['x', 'y']}]}"),
                    true,
                    "ShadowedPath_Regex"sv);
}

TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestElemMatchObjectShadowedPathSize) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    runParsedFilter("{arr: {$elemMatch: {b: {$size: 2}}}}",
                    fromjson("{b: 9, arr: [{b: [1, 2]}]}"),
                    true,
                    "ShadowedPath_Size"sv);
}

TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestNestedElemMatchShadowedPath) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    // {arr: {$elemMatch: {f: {$elemMatch: {g: 2}}}}}: "g" is element-relative at match
    // frame depth 3 and must not inherit the root metadata for "g".
    runParsedFilter("{arr: {$elemMatch: {f: {$elemMatch: {g: 2}}}}}",
                    fromjson("{g: 1, arr: [{f: [{g: [1, 2]}]}]}"),
                    true,
                    "NestedElemMatch_ShadowedG"sv);
}

TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestElemMatchShadowedDottedPath) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    // {arr: {$elemMatch: {"c.d": 2}}}: the dotted element-relative path must not inherit
    // the root metadata for "c.d".
    runParsedFilter("{arr: {$elemMatch: {'c.d': 2}}}",
                    fromjson("{c: {d: 1}, arr: [{c: {d: [1, 2]}}]}"),
                    true,
                    "DottedElemMatch_ShadowedCD"sv);
}

TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestRootFramePathStillElides) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    // Root-frame predicates on known non-array paths must keep eliding traverseF.
    auto slot = registerDocSlot("shadowRoot"sv, fromjson("{b: 1, c: {d: 1}, arr: [{b: [1, 2]}]}"));
    runParsedFilterOnSlot("{b: 1}", slot, true, "RootFrame_Eq_StillElided"sv);
    runParsedFilterOnSlot("{'c.d': 1}", slot, true, "RootFrame_DottedEq_StillElided"sv);
}

TEST_F(GoldenSbeFilterBuilderElemMatchShadowFixture, TestElemMatchNestedElemMatchForms) {
    unittest::ServerParameterGuard featureFlag{"featureFlagPathArrayness", true};

    // {arr: {$elemMatch: {e: {$elemMatch: {$gt: 1}}}}}: the value-form $elemMatch's own path
    // "e" is element-relative inside the object-form $elemMatch and must not inherit the root
    // metadata for "e". Hand-built because the parser produces pathless children for value-form
    // $elemMatch; the planner assigns their paths before stage building in production.
    {
        auto slot =
            registerDocSlot("valueInObjectRoot"sv, fromjson("{b: 1, e: 1, arr: [{e: [1, 2]}]}"));
        ElemMatchObjectMatchExpression elemMatch(
            "arr"sv,
            std::make_unique<ElemMatchValueMatchExpression>(
                "e"sv, std::make_unique<GTMatchExpression>("e"sv, Value(1))));
        runTestWithPathArrayness(&elemMatch, slot, true, "ValueInObject_ShadowedE"sv);
    }

    // {arr: {$elemMatch: {$elemMatch: {b: 2}}}}: the inner object-form $elemMatch's child "b"
    // is element-relative at match frame depth 3 and must not inherit the root metadata for
    // "b". Hand-built for the same reason as above.
    {
        auto slot =
            registerDocSlot("objectInValueRoot"sv, fromjson("{b: 1, arr: [[{b: [1, 2]}]]}"));
        ElemMatchValueMatchExpression elemMatch(
            "arr"sv,
            std::make_unique<ElemMatchObjectMatchExpression>(
                "arr"sv, std::make_unique<EqualityMatchExpression>("b"sv, Value(2))));
        runTestWithPathArrayness(&elemMatch, slot, true, "ObjectInValue_ShadowedB"sv);
    }
}

}  // namespace mongo::stage_builder
