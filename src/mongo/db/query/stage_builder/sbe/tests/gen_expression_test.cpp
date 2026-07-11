// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/gen_expression.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_trigonometric.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <utility>
#include <vector>

namespace mongo::stage_builder {
using namespace std::literals::string_view_literals;

class GoldenGenExpressionTest : public GoldenSbeExprBuilderTestFixture {
public:
    void setUp() override {
        GoldenSbeExprBuilderTestFixture::setUp();
        _gctx->validateOnClose(true);
    }

    void tearDown() override {
        GoldenSbeExprBuilderTestFixture::tearDown();
    }

    void runTest(const Expression* expr,
                 boost::optional<SbSlot> rootSlot,
                 sbe::value::TypeTags expectedTag,
                 sbe::value::Value expectedVal,
                 std::string_view test) {
        // TODO SERVER-100579 Remove this when feature flag is removed
        unittest::ServerParameterGuard sbeUpgradeBinaryTreesFeatureFlag{
            "featureFlagSbeUpgradeBinaryTrees", true};

        PlanStageSlots slots;
        auto sbExpr = generateExpression(*_state, expr, rootSlot, slots);
        GoldenSbeExprBuilderTestFixture::runTest(std::move(sbExpr), expectedTag, expectedVal, test);
    }

    void runTest(const Expression* expr,
                 boost::optional<SbSlot> rootSlot,
                 Value expected,
                 std::string_view test) {
        auto [expectedTag, expectedVal] = sbe::value::makeValue(expected);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        runTest(expr, rootSlot, expectedTag, expectedVal, test);
    }
};

TEST_F(GoldenGenExpressionTest, TestSimpleExpr) {
    auto root = BSON("_id" << 0 << "field1" << 4 << "field2" << true << "null" << BSONNULL);
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    auto fieldNumExpr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "field1", _expCtx->variablesParseState);
    auto fieldBoolExpr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "field2", _expCtx->variablesParseState);
    auto fieldNullExpr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "null", _expCtx->variablesParseState);

    {
        Value val = Value(true);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionAnd andExpr(_expCtx.get(), {constExpr, fieldBoolExpr});
        runTest(&andExpr, rootSlot, val, "ExpressionAnd"sv);
    }
    {
        Value val = Value(true);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        boost::intrusive_ptr<ExpressionArray> arrayExpr{
            new ExpressionArray(_expCtx.get(), {constExpr, fieldBoolExpr})};
        ExpressionAnyElementTrue anyTrueExpr(_expCtx.get(), {arrayExpr});
        runTest(&anyTrueExpr, rootSlot, val, "ExpressionAnyElementTrue"sv);
    }
    {
        Value arrVal = Value(BSON_ARRAY(BSON("k" << "_id"
                                                 << "v" << 0)
                                        << BSON("k" << "field1"
                                                    << "v" << 4)
                                        << BSON("k" << "field2"
                                                    << "v" << true)
                                        << BSON("k" << "null"
                                                    << "v" << BSONNULL)));
        auto varExpr = ExpressionFieldPath::createVarFromString(
            _expCtx.get(), "ROOT", _expCtx->variablesParseState);
        ExpressionObjectToArray obj2arrExpr(_expCtx.get());
        obj2arrExpr.addOperand(varExpr);
        runTest(&obj2arrExpr, rootSlot, arrVal, "ExpressionObjectToArray"sv);

        auto constArr = ExpressionConstant::create(_expCtx.get(), arrVal);
        ExpressionArrayToObject arr2ObjExpr(_expCtx.get(), {constArr});
        runTest(&arr2ObjExpr, rootSlot, Value(root), "ExpressionArrayToObject"sv);
    }
    {
        auto varExpr = ExpressionFieldPath::createVarFromString(
            _expCtx.get(), "ROOT", _expCtx->variablesParseState);
        ExpressionBsonSize bsonSizeExpr(_expCtx.get());
        bsonSizeExpr.addOperand(varExpr);
        runTest(&bsonSizeExpr, rootSlot, Value(41), "ExpressionBsonSize"sv);
    }
    {
        Value val = Value(NullLabeler{}), expected = Value(4);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        auto nullFieldExpr = ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "null", _expCtx->variablesParseState);
        ExpressionIfNull ifNullExpr(_expCtx.get());
        ifNullExpr.addOperand(nullFieldExpr);
        ifNullExpr.addOperand(constExpr);
        ifNullExpr.addOperand(fieldNumExpr);
        ifNullExpr.addOperand(nullFieldExpr);
        runTest(&ifNullExpr, rootSlot, expected, "ExpressionIfNull"sv);
    }
    {
        Value val = Value(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionIn inExpr(_expCtx.get(), {fieldNumExpr, constExpr});
        runTest(&inExpr, rootSlot, Value(true), "ExpressionIn"sv);
    }
    {
        ExpressionIsNumber isNumExpr(_expCtx.get());
        isNumExpr.addOperand(fieldNumExpr);
        runTest(&isNumExpr, rootSlot, Value(true), "ExpressionIsNumber"sv);
    }
    {
        auto bson = fromjson(
            "{$let: {vars: {a: {$add: [ '$field1', 1]}}, in: {$multiply: ['$$a', '$field1']}}}");
        auto letExpr =
            ExpressionLet::parse(_expCtx.get(), bson.firstElement(), _expCtx->variablesParseState);
        runTest(letExpr.get(), rootSlot, Value(20), "ExpressionLet"sv);
    }
    {
        ExpressionNot notExpr(_expCtx.get(), {fieldBoolExpr});
        runTest(&notExpr, rootSlot, Value(false), "ExpressionNot"sv);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(1));
        auto objExpr = ExpressionObject::create(
            _expCtx.get(), {{"a", constExpr}, {"b", fieldNumExpr}, {"c", fieldBoolExpr}});
        runTest(objExpr.get(),
                rootSlot,
                Value(BSON("a" << 1 << "b" << 4 << "c" << true)),
                "ExpressionObject"sv);
    }
    {
        Value val = Value(false), expected = Value(true);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionOr orExpr(_expCtx.get(), {constExpr, fieldNumExpr, fieldBoolExpr});
        runTest(&orExpr, rootSlot, expected, "ExpressionOr"sv);
    }
    {
        Value start = Value(0), step = Value(2);
        auto startConstExpr = ExpressionConstant::create(_expCtx.get(), start);
        auto stepConstExpr = ExpressionConstant::create(_expCtx.get(), step);
        ExpressionRange rangeExpr(_expCtx.get());
        rangeExpr.addOperand(startConstExpr);
        rangeExpr.addOperand(fieldNumExpr);
        rangeExpr.addOperand(stepConstExpr);
        runTest(&rangeExpr, rootSlot, Value(BSON_ARRAY(0 << 2)), "ExpressionRange"sv);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        ExpressionSwitch subtractExpr(_expCtx.get(), {fieldBoolExpr, fieldNumExpr, constExpr});
        runTest(&subtractExpr, rootSlot, Value(4), "ExpressionSwitch"sv);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        auto condExpr =
            ExpressionCond::create(_expCtx.get(), fieldBoolExpr, constExpr, fieldNumExpr);
        runTest(condExpr.get(), rootSlot, Value(10), "ExpressionCond"sv);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprStr) {
    auto root = BSON("_id" << 1 << "str"
                           << "This is a test."
                           << "pattern"
                           << "test");
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    auto strFieldExpr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "str", _expCtx->variablesParseState);

    {
        Value val1 = Value("This is"sv), val2 = Value(" a "sv);
        auto constExpr1 = ExpressionConstant::create(_expCtx.get(), val1);
        auto constExpr2 = ExpressionConstant::create(_expCtx.get(), val2);
        auto fieldExpr = ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "pattern", _expCtx->variablesParseState);
        ExpressionConcat concatExpr(_expCtx.get(), {constExpr1, constExpr2, fieldExpr});
        runTest(&concatExpr, rootSlot, Value("This is a test"sv), "ExpressionConcat"sv);
    }
    {
        Value val = Value(" "sv);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionSplit splitExpr(_expCtx.get(), {strFieldExpr, constExpr});
        runTest(&splitExpr,
                rootSlot,
                Value(BSON_ARRAY("This" << "is"
                                        << "a"
                                        << "test.")),
                "ExpressionSplit"sv);
    }
    {
        Value val = Value("this IS a TEST."sv);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionStrcasecmp strCmpExpr(_expCtx.get(), {constExpr, strFieldExpr});
        runTest(&strCmpExpr, rootSlot, Value(0), "ExpressionStrcasecmp"sv);
    }
    {
        auto idxExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        auto lenExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionSubstrBytes subStrExpr(_expCtx.get(), {strFieldExpr, idxExpr, lenExpr});
        runTest(&subStrExpr, rootSlot, Value("test"sv), "ExpressionSubstrBytes"sv);
    }
    {
        auto idxExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        auto lenExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionSubstrCP subStrExpr(_expCtx.get(), {strFieldExpr, idxExpr, lenExpr});
        runTest(&subStrExpr, rootSlot, Value("test"sv), "ExpressionSubstrCP"sv);
    }
    {
        ExpressionStrLenBytes strLenExpr(_expCtx.get(), {strFieldExpr});
        runTest(&strLenExpr, rootSlot, Value(15), "ExpressionStrLenBytes"sv);
    }
    {
        ExpressionStrLenCP strLenExpr(_expCtx.get(), {strFieldExpr});
        runTest(&strLenExpr, rootSlot, Value(15), "ExpressionStrLenCP"sv);
    }
    {
        ExpressionToLower lowerExpr(_expCtx.get(), {strFieldExpr});
        runTest(&lowerExpr, rootSlot, Value("this is a test."sv), "ExpressionToLower"sv);
    }
    {
        ExpressionToUpper upperExpr(_expCtx.get(), {strFieldExpr});
        runTest(&upperExpr, rootSlot, Value("THIS IS A TEST."sv), "ExpressionToUpper"sv);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value("."sv));
        ExpressionTrim trimExpr(
            _expCtx.get(), ExpressionTrim::TrimType::kBoth, "$trim"sv, strFieldExpr, constExpr);
        runTest(&trimExpr, rootSlot, Value("This is a test"sv), "ExpressionTrim"sv);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value("is"sv));
        ExpressionIndexOfBytes idxOfBytesExpr(_expCtx.get(), {strFieldExpr, constExpr});
        runTest(&idxOfBytesExpr, rootSlot, Value(2), "ExpressionIndexOfBytes"sv);
    }
    {
        auto subStrExpr = ExpressionConstant::create(_expCtx.get(), Value("is"sv));
        auto startIdxExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionIndexOfCP idxOfCPExpr(_expCtx.get(), {strFieldExpr, subStrExpr, startIdxExpr});
        runTest(&idxOfCPExpr, rootSlot, Value(5), "ExpressionIndexOfCP"sv);
    }
    {
        auto subStrExpr = ExpressionConstant::create(_expCtx.get(), Value(" "sv));
        auto startIdxExpr = ExpressionConstant::create(_expCtx.get(), Value(1));
        auto endIdxExpr = ExpressionConstant::create(_expCtx.get(), Value(3));
        ExpressionIndexOfCP idxOfCPExpr(_expCtx.get(),
                                        {strFieldExpr, subStrExpr, startIdxExpr, endIdxExpr});
        runTest(&idxOfCPExpr, rootSlot, Value(-1), "ExpressionIndexOfCP"sv);
    }
    {
        auto pattern = ExpressionConstant::create(_expCtx.get(), Value("test"sv));
        ExpressionRegexFind regFindExpr(
            _expCtx.get(), strFieldExpr, pattern, nullptr, "$regexFind"sv);
        runTest(&regFindExpr,
                rootSlot,
                Value(BSON("match" << "test"
                                   << "idx" << 10 << "captures" << BSONArray())),
                "ExpressionRegexFind"sv);
    }
    {
        auto pattern = ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "pattern", _expCtx->variablesParseState);
        ExpressionRegexFindAll regFindAllExpr(
            _expCtx.get(), strFieldExpr, pattern, nullptr, "$regexFindAll"sv);
        runTest(&regFindAllExpr,
                rootSlot,
                Value(BSON_ARRAY(BSON("match" << "test"
                                              << "idx" << 10 << "captures" << BSONArray()))),
                "ExpressionRegexFindAll"sv);
    }
    {
        auto pattern = ExpressionConstant::create(_expCtx.get(), Value("test"sv));
        ExpressionRegexMatch regMatchExpr(
            _expCtx.get(), strFieldExpr, pattern, nullptr, "$regexMatch"sv);
        runTest(&regMatchExpr, rootSlot, Value(true), "ExpressionRegexMatch"sv);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprMath) {
    auto root = BSON("_id" << 1 << "field1" << 4 << "field2" << 0.75);
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    auto field1Expr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "field1", _expCtx->variablesParseState);
    auto field2Expr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "field2", _expCtx->variablesParseState);

    {
        Value val1 = Value(-100), expected = Value(100);
        auto constExpr1 = ExpressionConstant::create(_expCtx.get(), val1);
        ExpressionAbs absExpr(_expCtx.get(), {constExpr1});
        runTest(&absExpr, rootSlot, expected, "ExpressionAbs"sv);
    }
    {
        Value val = Value(100), expected = Value(104);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionAdd addExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&addExpr, rootSlot, expected, "ExpressionAdd"sv);
    }
    {
        Value val = Value(9.25), expected = Value(10);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionCeil ceilExpr(_expCtx.get(), {constExpr});
        runTest(&ceilExpr, rootSlot, expected, "ExpressionCeil"sv);
    }
    {
        Value val = Value(9), expected = Value(2.25);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionDivide divideExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&divideExpr, rootSlot, expected, "ExpressionDivide"sv);
    }
    {
        Value val = Value(0), expected = Value(1);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionExp expExpr(_expCtx.get(), {constExpr});
        runTest(&expExpr, rootSlot, expected, "ExpressionExp"sv);
    }
    {
        Value val = Value(9.25), expected = Value(9);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionFloor floorExpr(_expCtx.get(), {constExpr});
        runTest(&floorExpr, rootSlot, expected, "ExpressionFloor"sv);
    }
    {
        Value val = Value(1);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionLn lnExpr(_expCtx.get(), {constExpr});
        runTest(&lnExpr, rootSlot, Value(0), "ExpressionLn"sv);
    }
    {
        Value val = Value(10);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionLog10 log10Expr(_expCtx.get(), {constExpr});
        runTest(&log10Expr, rootSlot, Value(1), "ExpressionLog10"sv);
    }
    {
        Value val = Value(19), expected = Value(3);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionMod modExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&modExpr, rootSlot, expected, "ExpressionMod"sv);
    }
    {
        Value val = Value(4.5), expected = Value(18);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionMultiply multiplyExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&multiplyExpr, rootSlot, expected, "ExpressionMultiply"sv);
    }
    {
        Value val = Value(4), expected = Value(256);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionPow powExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&powExpr, rootSlot, expected, "ExpressionPow"sv);
    }
    {
        Value val = Value(1.123456), expected = Value(1.1235);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionRound roundExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&roundExpr, rootSlot, expected, "ExpressionRound"sv);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        ExpressionSubtract subtractExpr(_expCtx.get(), {field1Expr, constExpr});
        runTest(&subtractExpr, rootSlot, Value(-6), "ExpressionSubtract"sv);
    }
    {
        ExpressionSqrt sqrtExpr(_expCtx.get(), {field1Expr});
        runTest(&sqrtExpr, rootSlot, Value(2), "ExpressionSqrt"sv);
    }
    {
        boost::intrusive_ptr<ExpressionCosine> cosExpr{
            new ExpressionCosine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {cosExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(-0.6536), "ExpressionCosine"sv);
    }
    {
        boost::intrusive_ptr<ExpressionSine> sinExpr{
            new ExpressionSine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {sinExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(-0.7568), "ExpressionSine"sv);
    }
    {
        boost::intrusive_ptr<ExpressionTangent> tanExpr{
            new ExpressionTangent(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {tanExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(1.1578), "ExpressionTangent"sv);
    }
    {
        boost::intrusive_ptr<ExpressionArcCosine> acosExpr{
            new ExpressionArcCosine(_expCtx.get(), {field2Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {acosExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.7227), "ExpressionArcCosine"sv);
    }
    {
        boost::intrusive_ptr<ExpressionArcSine> asinExpr{
            new ExpressionArcSine(_expCtx.get(), {field2Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {asinExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.848), "ExpressionArcSine"sv);
    }
    {
        boost::intrusive_ptr<ExpressionArcTangent> atanExpr{
            new ExpressionArcTangent(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {atanExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(1.3258), "ExpressionArcTangent"sv);
    }
    {
        boost::intrusive_ptr<ExpressionArcTangent2> atan2Expr{
            new ExpressionArcTangent2(_expCtx.get(), {field1Expr, field2Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {atan2Expr, constExpr});
        runTest(&truncExpr, rootSlot, Value(1.3854), "ExpressionArcTangent2"sv);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicArcTangent> atanhExpr{
            new ExpressionHyperbolicArcTangent(_expCtx.get(), {field2Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {atanhExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.9729), "ExpressionHyperbolicArcTangent"sv);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicArcCosine> acoshExpr{
            new ExpressionHyperbolicArcCosine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {acoshExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(2.0634), "ExpressionHyperbolicArcCosine"sv);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicArcSine> asinhExpr{
            new ExpressionHyperbolicArcSine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {asinhExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(2.0947), "ExpressionHyperbolicArcSine"sv);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicCosine> coshExpr{
            new ExpressionHyperbolicCosine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {coshExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(27.3082), "ExpressionHyperbolicCosine"sv);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicSine> sinhExpr{
            new ExpressionHyperbolicSine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {sinhExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(27.2899), "ExpressionHyperbolicSine"sv);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicTangent> tanhExpr{
            new ExpressionHyperbolicTangent(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {tanhExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.9993), "ExpressionHyperbolicTangent"sv);
    }
    {
        boost::intrusive_ptr<ExpressionDegreesToRadians> dtrExpr{
            new ExpressionDegreesToRadians(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {dtrExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.0698), "ExpressionDegreesToRadians"sv);
    }
    {
        boost::intrusive_ptr<ExpressionRadiansToDegrees> rtdExpr{
            new ExpressionRadiansToDegrees(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(3));
        ExpressionTrunc truncExpr(_expCtx.get(), {rtdExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(229.183), "ExpressionRadiansToDegrees"sv);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprCmp) {
    auto root = BSON("_id" << 0 << "bar" << 5);
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    Value val = Value(9.25);
    auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
    auto fieldExpr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "bar", _expCtx->variablesParseState);
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::EQ, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(false), "ExpressionCompareEQ"sv);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::NE, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(true), "ExpressionCompareNE"sv);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::GT, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(true), "ExpressionCompareGT"sv);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::GTE, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(true), "ExpressionCompareGTE"sv);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::LT, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(false), "ExpressionCompareLT"sv);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::LTE, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(false), "ExpressionCompareLTE"sv);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::CMP, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(1), "ExpressionCompareCMP"sv);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprDate) {
    // date1: 11/18/2024 10:43:44 PM GMT
    // date2: 11/18/2023 10:43:44 PM GMT
    // ts: 10/01/2014 04:28:07 PM GMT
    auto root = BSON("_id" << 0 << "date1" << Date_t::fromMillisSinceEpoch(1731969824000) << "date2"
                           << Date_t::fromMillisSinceEpoch(1700347463000) << "ts"
                           << Timestamp(Date_t::fromMillisSinceEpoch(1412180887000)) << "dateStr"
                           << "11/18/2024 10:15:00");
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    auto& vps = _expCtx->variablesParseState;
    auto date1 = ExpressionFieldPath::createPathFromString(_expCtx.get(), "date1", vps);
    auto date2 = ExpressionFieldPath::createPathFromString(_expCtx.get(), "date2", vps);
    auto ts = ExpressionFieldPath::createPathFromString(_expCtx.get(), "ts", vps);
    auto dateStr = ExpressionFieldPath::createPathFromString(_expCtx.get(), "dateStr", vps);
    {
        ExpressionDateAdd dateAddExpr(
            _expCtx.get(),
            date1 /* startDate */,
            ExpressionConstant::create(_expCtx.get(), Value("quarter"sv)) /* unit */,
            ExpressionConstant::create(_expCtx.get(), Value(1)) /* amount */,
            ExpressionConstant::create(_expCtx.get(), Value("America/New_York"sv)) /* timezone */,
            "$dateAdd"sv);
        // expect 02/18/2025 10:43:44 PM GMT
        runTest(&dateAddExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1739918624000)),
                "ExpressionDateAdd"sv);
    }
    {
        ExpressionDateSubtract dateSubExpr(
            _expCtx.get(),
            date1 /* startDate */,
            ExpressionConstant::create(_expCtx.get(), Value("week"sv)) /* unit */,
            ExpressionConstant::create(_expCtx.get(), Value(1)) /* amount */,
            ExpressionConstant::create(_expCtx.get(), Value("America/New_York"sv)) /* timezone */,
            "$dateAdd"sv);
        // expect 11/11/2024 10:43:44 PM
        runTest(&dateSubExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1731365024000)),
                "ExpressionDateSubtract"sv);
    }
    {
        ExpressionDateDiff dateDiffExpr(
            _expCtx.get(),
            date1 /* startDate */,
            date2 /* endDate */,
            ExpressionConstant::create(_expCtx.get(), Value("week"sv)) /* unit */,
            ExpressionConstant::create(_expCtx.get(), Value("America/New_York"sv)) /* timezone */,
            nullptr /* startOfWeek */);
        runTest(&dateDiffExpr, rootSlot, Value(-53), "ExpressionDateDiff"sv);
    }
    {
        ExpressionDateFromString dateFromStrExpr(
            _expCtx.get(),
            dateStr,
            ExpressionConstant::create(_expCtx.get(), Value("GMT"sv)) /* timezone */,
            ExpressionConstant::create(_expCtx.get(), Value("%m/%d/%Y %H:%M:%S"sv)) /* format */,
            nullptr /* onNull */,
            nullptr /* onError */);
        runTest(&dateFromStrExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1731924900000)),
                "ExpressionDateFromString"sv);
    }
    {

        ExpressionDateFromParts dateFromPartExpr(
            _expCtx.get(),
            ExpressionConstant::create(_expCtx.get(), Value(2024)) /* year */,
            ExpressionConstant::create(_expCtx.get(), Value(11)) /* month */,
            ExpressionConstant::create(_expCtx.get(), Value(18)) /* day */,
            ExpressionConstant::create(_expCtx.get(), Value(14)) /* hour */,
            ExpressionConstant::create(_expCtx.get(), Value(30)) /* minute */,
            ExpressionConstant::create(_expCtx.get(), Value(15)) /* second */,
            nullptr /* millisecond */,
            nullptr /* isoWeekYear */,
            nullptr /* isoWeek */,
            nullptr /* isoDayOfWeek */,
            nullptr /* timeZone */);
        runTest(&dateFromPartExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1731940215000)),
                "ExpressionDateFromParts"sv);
    }
    {
        ExpressionDateToParts dateToPartExpr(
            _expCtx.get(),
            date1 /* date */,
            nullptr /* timeZone */,
            ExpressionConstant::create(_expCtx.get(), Value(false)) /* iso8601 */);
        runTest(&dateToPartExpr,
                rootSlot,
                Value(BSON("year" << 2024 << "month" << 11 << "day" << 18 << "hour" << 22
                                  << "minute" << 43 << "second" << 44 << "millisecond" << 0)),
                "ExpressionDateToParts"sv);
    }
    {
        ExpressionDateToString dateToStrExpr(
            _expCtx.get(),
            date1 /* date */,
            ExpressionConstant::create(_expCtx.get(), Value("%m/%d/%Y %H:%M:%S"sv)) /* format */,
            nullptr /* timeZone */,
            ExpressionConstant::create(_expCtx.get(), Value(false)) /* onNull */);
        runTest(
            &dateToStrExpr, rootSlot, Value("11/18/2024 22:43:44"sv), "ExpressionDateToString"sv);
    }
    {
        ExpressionDateTrunc dateTruncExpr(
            _expCtx.get(),
            date1 /* date */,
            ExpressionConstant::create(_expCtx.get(), Value("week"sv)) /* unit */,
            nullptr /* binSize */,
            nullptr /* timeZone */,
            nullptr /* startOfWeek */);
        // expected 11/17/2024 12:00:00 AM
        runTest(&dateTruncExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1731801600000)),
                "ExpressionDateTrunc"sv);
    }
    {
        ExpressionDayOfMonth dayOfMonthExpr(_expCtx.get(), date1);
        runTest(&dayOfMonthExpr, rootSlot, Value(18), "ExpressionDayOfMonth"sv);
    }
    {
        ExpressionDayOfWeek dayOfWeekExpr(_expCtx.get(), date1);
        runTest(&dayOfWeekExpr, rootSlot, Value(2), "ExpressionDayOfWeek"sv);
    }
    {
        ExpressionDayOfYear dayOfYearExpr(_expCtx.get(), date1);
        runTest(&dayOfYearExpr, rootSlot, Value(323), "ExpressionDayOfYear"sv);
    }
    {
        ExpressionHour hourExpr(_expCtx.get(), date1);
        runTest(&hourExpr, rootSlot, Value(22), "ExpressionHour"sv);
    }
    {
        ExpressionMillisecond millisecExpr(_expCtx.get(), date1);
        runTest(&millisecExpr, rootSlot, Value(0), "ExpressionMillisecond"sv);
    }
    {
        ExpressionMinute minuteExpr(_expCtx.get(), date1);
        runTest(&minuteExpr, rootSlot, Value(43), "ExpressionMinute"sv);
    }
    {
        ExpressionMonth monthExpr(_expCtx.get(), date1);
        runTest(&monthExpr, rootSlot, Value(11), "ExpressionMonth"sv);
    }
    {
        ExpressionSecond secondExpr(_expCtx.get(), date1);
        runTest(&secondExpr, rootSlot, Value(44), "ExpressionSecond"sv);
    }
    {
        ExpressionWeek weekExpr(_expCtx.get(), date1);
        runTest(&weekExpr, rootSlot, Value(46), "ExpressionWeek"sv);
    }
    {
        ExpressionIsoWeekYear isoWeekYearExpr(_expCtx.get(), date1);
        runTest(&isoWeekYearExpr, rootSlot, Value(2024), "ExpressionIsoWeekYear"sv);
    }
    {
        ExpressionIsoDayOfWeek isoDayOfWeekExpr(_expCtx.get(), date1);
        runTest(&isoDayOfWeekExpr, rootSlot, Value(1), "ExpressionIsoDayOfWeek"sv);
    }
    {
        ExpressionYear yearExpr(_expCtx.get(), date1);
        runTest(&yearExpr, rootSlot, Value(2024), "ExpressionYear"sv);
    }
    {
        ExpressionTsSecond tsSecExpr(_expCtx.get(), {ts});
        runTest(&tsSecExpr, rootSlot, Value(328), "ExpressionTsSecond"sv);
    }
    {
        ExpressionTsIncrement tsIncrExpr(_expCtx.get(), {ts});
        runTest(&tsIncrExpr, rootSlot, Value(3431613912LL), "ExpressionTsIncrement"sv);
    }
}


TEST_F(GoldenGenExpressionTest, TestExprArraySet) {
    auto root = BSON("_id" << 0 << "arr1" << BSONArray() << "arr2" << BSON_ARRAY(1 << 2.5 << "str")
                           << "arr3" << BSON_ARRAY(1 << 5 << "str2"));
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};
    Value val = Value(BSON_ARRAY(2.5 << "str"));
    auto constArrExpr = ExpressionConstant::create(_expCtx.get(), val);
    auto fieldArr1Expr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "arr1", _expCtx->variablesParseState);
    auto fieldArr2Expr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "arr2", _expCtx->variablesParseState);
    auto fieldArr3Expr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "arr3", _expCtx->variablesParseState);
    {
        ExpressionConcatArrays concatArrExpr(_expCtx.get(),
                                             {fieldArr1Expr, constArrExpr, fieldArr2Expr});
        auto expected = BSON_ARRAY(2.5 << "str" << 1 << 2.5 << "str");
        runTest(&concatArrExpr, rootSlot, Value(expected), "ExpressionConcatArrays"sv);
    }
    {
        ExpressionSetDifference setDiffExpr(_expCtx.get(), {fieldArr2Expr, fieldArr3Expr});
        auto [tag, val] = sbe::value::makeNewArraySet();
        sbe::value::ValueGuard valGuard{tag, val};
        sbe::value::getArraySetView(val)->push_back_raw(sbe::value::makeValue(Value(2.5)));
        sbe::value::getArraySetView(val)->push_back_raw(sbe::value::makeValue(Value("str"sv)));
        runTest(&setDiffExpr, rootSlot, tag, val, "ExpressionSetDifference"sv);
    }
    {
        ExpressionSetEquals setEqExpr(_expCtx.get(), {fieldArr2Expr, fieldArr3Expr});
        runTest(&setEqExpr, rootSlot, Value(false), "ExpressionSetEquals"sv);
    }
    {
        ExpressionSetIntersection setInterExpr(_expCtx.get(), {fieldArr2Expr, fieldArr3Expr});
        auto expected = BSON_ARRAY(1);
        runTest(&setInterExpr, rootSlot, Value(expected), "ExpressionSetIntersection"sv);
    }
    {
        ExpressionSetIsSubset setIsSubsetExpr(_expCtx.get(), {constArrExpr, fieldArr2Expr});
        runTest(&setIsSubsetExpr, rootSlot, Value(true), "ExpressionSetIsSubset"sv);
    }
    {
        ExpressionSetUnion setUnionExpr(_expCtx.get(), {fieldArr2Expr, fieldArr3Expr});
        auto [tag, val] = sbe::value::makeNewArraySet();
        sbe::value::ValueGuard valGuard{tag, val};
        sbe::value::getArraySetView(val)->push_back_raw(sbe::value::makeValue(Value(1)));
        sbe::value::getArraySetView(val)->push_back_raw(sbe::value::makeValue(Value(2.5)));
        sbe::value::getArraySetView(val)->push_back_raw(sbe::value::makeValue(Value("str"sv)));
        sbe::value::getArraySetView(val)->push_back_raw(sbe::value::makeValue(Value(5)));
        sbe::value::getArraySetView(val)->push_back_raw(sbe::value::makeValue(Value("str2"sv)));
        runTest(&setUnionExpr, rootSlot, tag, val, "ExpressionSetUnion"sv);
    }
    {
        ExpressionReverseArray arrReverseExpr(_expCtx.get());
        arrReverseExpr.addOperand(fieldArr2Expr);
        auto expected = BSON_ARRAY("str" << 2.5 << 1);
        runTest(&arrReverseExpr, rootSlot, Value(expected), "ExpressionReverseArray"sv);
    }
    {
        BSONObj expr = fromjson("{ $sortArray: { input: '$arr2', sortBy: -1 } }");
        auto arrReverseExpr = ExpressionSortArray::parse(
            _expCtx.get(), expr.firstElement(), _expCtx->variablesParseState);
        auto expected = BSON_ARRAY("str" << 2.5 << 1);
        runTest(arrReverseExpr.get(), rootSlot, Value(expected), "ExpressionSortArray"sv);
    }
    {
        BSONObj expr = fromjson("{ $topN: { n: 2, input: '$arr2', sortBy: -1 } }");
        auto topNExpr =
            ExpressionTopN::parse(_expCtx.get(), expr.firstElement(), _expCtx->variablesParseState);
        runTest(topNExpr.get(), rootSlot, Value(BSON_ARRAY("str" << 2.5)), "ExpressionTopN"sv);
    }
    {
        BSONObj expr = fromjson("{ $top: { input: '$arr2', sortBy: -1 } }");
        auto topExpr =
            ExpressionTop::parse(_expCtx.get(), expr.firstElement(), _expCtx->variablesParseState);
        runTest(topExpr.get(), rootSlot, Value("str"sv), "ExpressionTop"sv);
    }
    {
        BSONObj expr = fromjson("{ $bottomN: { n: 2, input: '$arr2', sortBy: -1 } }");
        auto bottomNExpr = ExpressionBottomN::parse(
            _expCtx.get(), expr.firstElement(), _expCtx->variablesParseState);
        runTest(bottomNExpr.get(), rootSlot, Value(BSON_ARRAY(2.5 << 1)), "ExpressionBottomN"sv);
    }
    {
        BSONObj expr = fromjson("{ $bottom: { input: '$arr2', sortBy: -1 } }");
        auto bottomExpr = ExpressionBottom::parse(
            _expCtx.get(), expr.firstElement(), _expCtx->variablesParseState);
        runTest(bottomExpr.get(), rootSlot, Value(1), "ExpressionBottom"sv);
    }
    {
        ExpressionIsArray isArrExpr(_expCtx.get(), {fieldArr2Expr});
        runTest(&isArrExpr, rootSlot, Value(true), "ExpressionIsArray"sv);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprMultiplyLarge) {
    auto root = BSON("_id" << 0 << "bar" << 0.5);
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    std::vector<boost::intrusive_ptr<Expression>> exprs;
    for (long long i = 0; i < 50; ++i) {
        exprs.emplace_back(ExpressionConstant::create(_expCtx.get(), Value((i % 2 + 1) * 2)));
        exprs.emplace_back(ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "bar", _expCtx->variablesParseState));
    }
    Value expected = Value(33554432);  // 2^25
    ExpressionMultiply multiplyExpr(_expCtx.get(), std::move(exprs));
    runTest(&multiplyExpr, rootSlot, expected, "ExpressionMultiply"sv);
}

TEST_F(GoldenGenExpressionTest, TestExprAddLarge) {
    auto root = BSON("_id" << 0 << "bar" << 5);
    auto rootSlotId = _env->registerSlot("root"sv,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    std::vector<boost::intrusive_ptr<Expression>> exprs;
    for (long long i = 0; i < 50; ++i) {
        exprs.emplace_back(ExpressionConstant::create(_expCtx.get(), Value(i)));
        exprs.emplace_back(ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "bar", _expCtx->variablesParseState));
    }
    Value expected = Value(1475);  // 50*49/2+5*50 = 1475
    ExpressionAdd addExpr(_expCtx.get(), std::move(exprs));
    runTest(&addExpr, rootSlot, expected, "ExpressionAdd"sv);
}

}  // namespace mongo::stage_builder
