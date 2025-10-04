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

#include "mongo/db/query/stage_builder/sbe/gen_expression.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_trigonometric.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <utility>
#include <vector>

namespace mongo::stage_builder {

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
                 StringData test) {
        // TODO SERVER-100579 Remove this when feature flag is removed
        RAIIServerParameterControllerForTest sbeUpgradeBinaryTreesFeatureFlag{
            "featureFlagSbeUpgradeBinaryTrees", true};

        PlanStageSlots slots;
        auto sbExpr = generateExpression(*_state, expr, rootSlot, slots);
        GoldenSbeExprBuilderTestFixture::runTest(std::move(sbExpr), expectedTag, expectedVal, test);
    }

    void runTest(const Expression* expr,
                 boost::optional<SbSlot> rootSlot,
                 Value expected,
                 StringData test) {
        auto [expectedTag, expectedVal] = sbe::value::makeValue(expected);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};
        runTest(expr, rootSlot, expectedTag, expectedVal, test);
    }
};

TEST_F(GoldenGenExpressionTest, TestSimpleExpr) {
    auto root = BSON("_id" << 0 << "field1" << 4 << "field2" << true << "null" << BSONNULL);
    auto rootSlotId = _env->registerSlot("root"_sd,
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
        runTest(&andExpr, rootSlot, val, "ExpressionAnd"_sd);
    }
    {
        Value val = Value(true);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        boost::intrusive_ptr<ExpressionArray> arrayExpr{
            new ExpressionArray(_expCtx.get(), {constExpr, fieldBoolExpr})};
        ExpressionAnyElementTrue anyTrueExpr(_expCtx.get(), {arrayExpr});
        runTest(&anyTrueExpr, rootSlot, val, "ExpressionAnyElementTrue"_sd);
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
        runTest(&obj2arrExpr, rootSlot, arrVal, "ExpressionObjectToArray"_sd);

        auto constArr = ExpressionConstant::create(_expCtx.get(), arrVal);
        ExpressionArrayToObject arr2ObjExpr(_expCtx.get(), {constArr});
        runTest(&arr2ObjExpr, rootSlot, Value(root), "ExpressionArrayToObject"_sd);
    }
    {
        auto varExpr = ExpressionFieldPath::createVarFromString(
            _expCtx.get(), "ROOT", _expCtx->variablesParseState);
        ExpressionBsonSize bsonSizeExpr(_expCtx.get());
        bsonSizeExpr.addOperand(varExpr);
        runTest(&bsonSizeExpr, rootSlot, Value(41), "ExpressionBsonSize"_sd);
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
        runTest(&ifNullExpr, rootSlot, expected, "ExpressionIfNull"_sd);
    }
    {
        Value val = Value(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionIn inExpr(_expCtx.get(), {fieldNumExpr, constExpr});
        runTest(&inExpr, rootSlot, Value(true), "ExpressionIn"_sd);
    }
    {
        ExpressionIsNumber isNumExpr(_expCtx.get());
        isNumExpr.addOperand(fieldNumExpr);
        runTest(&isNumExpr, rootSlot, Value(true), "ExpressionIsNumber"_sd);
    }
    {
        auto bson = fromjson(
            "{$let: {vars: {a: {$add: [ '$field1', 1]}}, in: {$multiply: ['$$a', '$field1']}}}");
        auto letExpr =
            ExpressionLet::parse(_expCtx.get(), bson.firstElement(), _expCtx->variablesParseState);
        runTest(letExpr.get(), rootSlot, Value(20), "ExpressionLet"_sd);
    }
    {
        ExpressionNot notExpr(_expCtx.get(), {fieldBoolExpr});
        runTest(&notExpr, rootSlot, Value(false), "ExpressionNot"_sd);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(1));
        auto objExpr = ExpressionObject::create(
            _expCtx.get(), {{"a", constExpr}, {"b", fieldNumExpr}, {"c", fieldBoolExpr}});
        runTest(objExpr.get(),
                rootSlot,
                Value(BSON("a" << 1 << "b" << 4 << "c" << true)),
                "ExpressionObject"_sd);
    }
    {
        Value val = Value(false), expected = Value(true);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionOr orExpr(_expCtx.get(), {constExpr, fieldNumExpr, fieldBoolExpr});
        runTest(&orExpr, rootSlot, expected, "ExpressionOr"_sd);
    }
    {
        Value start = Value(0), step = Value(2);
        auto startConstExpr = ExpressionConstant::create(_expCtx.get(), start);
        auto stepConstExpr = ExpressionConstant::create(_expCtx.get(), step);
        ExpressionRange rangeExpr(_expCtx.get());
        rangeExpr.addOperand(startConstExpr);
        rangeExpr.addOperand(fieldNumExpr);
        rangeExpr.addOperand(stepConstExpr);
        runTest(&rangeExpr, rootSlot, Value(BSON_ARRAY(0 << 2)), "ExpressionRange"_sd);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        ExpressionSwitch subtractExpr(_expCtx.get(), {fieldBoolExpr, fieldNumExpr, constExpr});
        runTest(&subtractExpr, rootSlot, Value(4), "ExpressionSwitch"_sd);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        auto condExpr =
            ExpressionCond::create(_expCtx.get(), fieldBoolExpr, constExpr, fieldNumExpr);
        runTest(condExpr.get(), rootSlot, Value(10), "ExpressionCond"_sd);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprStr) {
    auto root = BSON("_id" << 1 << "str"
                           << "This is a test."
                           << "pattern"
                           << "test");
    auto rootSlotId = _env->registerSlot("root"_sd,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    auto strFieldExpr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "str", _expCtx->variablesParseState);

    {
        Value val1 = Value("This is"_sd), val2 = Value(" a "_sd);
        auto constExpr1 = ExpressionConstant::create(_expCtx.get(), val1);
        auto constExpr2 = ExpressionConstant::create(_expCtx.get(), val2);
        auto fieldExpr = ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "pattern", _expCtx->variablesParseState);
        ExpressionConcat concatExpr(_expCtx.get(), {constExpr1, constExpr2, fieldExpr});
        runTest(&concatExpr, rootSlot, Value("This is a test"_sd), "ExpressionConcat"_sd);
    }
    {
        Value val = Value(" "_sd);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionSplit splitExpr(_expCtx.get(), {strFieldExpr, constExpr});
        runTest(&splitExpr,
                rootSlot,
                Value(BSON_ARRAY("This" << "is"
                                        << "a"
                                        << "test.")),
                "ExpressionSplit"_sd);
    }
    {
        Value val = Value("this IS a TEST."_sd);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionStrcasecmp strCmpExpr(_expCtx.get(), {constExpr, strFieldExpr});
        runTest(&strCmpExpr, rootSlot, Value(0), "ExpressionStrcasecmp"_sd);
    }
    {
        auto idxExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        auto lenExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionSubstrBytes subStrExpr(_expCtx.get(), {strFieldExpr, idxExpr, lenExpr});
        runTest(&subStrExpr, rootSlot, Value("test"_sd), "ExpressionSubstrBytes"_sd);
    }
    {
        auto idxExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        auto lenExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionSubstrCP subStrExpr(_expCtx.get(), {strFieldExpr, idxExpr, lenExpr});
        runTest(&subStrExpr, rootSlot, Value("test"_sd), "ExpressionSubstrCP"_sd);
    }
    {
        ExpressionStrLenBytes strLenExpr(_expCtx.get(), {strFieldExpr});
        runTest(&strLenExpr, rootSlot, Value(15), "ExpressionStrLenBytes"_sd);
    }
    {
        ExpressionStrLenCP strLenExpr(_expCtx.get(), {strFieldExpr});
        runTest(&strLenExpr, rootSlot, Value(15), "ExpressionStrLenCP"_sd);
    }
    {
        ExpressionToLower lowerExpr(_expCtx.get(), {strFieldExpr});
        runTest(&lowerExpr, rootSlot, Value("this is a test."_sd), "ExpressionToLower"_sd);
    }
    {
        ExpressionToUpper upperExpr(_expCtx.get(), {strFieldExpr});
        runTest(&upperExpr, rootSlot, Value("THIS IS A TEST."_sd), "ExpressionToUpper"_sd);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value("."_sd));
        ExpressionTrim trimExpr(
            _expCtx.get(), ExpressionTrim::TrimType::kBoth, "$trim"_sd, strFieldExpr, constExpr);
        runTest(&trimExpr, rootSlot, Value("This is a test"_sd), "ExpressionTrim"_sd);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value("is"_sd));
        ExpressionIndexOfBytes idxOfBytesExpr(_expCtx.get(), {strFieldExpr, constExpr});
        runTest(&idxOfBytesExpr, rootSlot, Value(2), "ExpressionIndexOfBytes"_sd);
    }
    {
        auto subStrExpr = ExpressionConstant::create(_expCtx.get(), Value("is"_sd));
        auto startIdxExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionIndexOfCP idxOfCPExpr(_expCtx.get(), {strFieldExpr, subStrExpr, startIdxExpr});
        runTest(&idxOfCPExpr, rootSlot, Value(5), "ExpressionIndexOfCP"_sd);
    }
    {
        auto subStrExpr = ExpressionConstant::create(_expCtx.get(), Value(" "_sd));
        auto startIdxExpr = ExpressionConstant::create(_expCtx.get(), Value(1));
        auto endIdxExpr = ExpressionConstant::create(_expCtx.get(), Value(3));
        ExpressionIndexOfCP idxOfCPExpr(_expCtx.get(),
                                        {strFieldExpr, subStrExpr, startIdxExpr, endIdxExpr});
        runTest(&idxOfCPExpr, rootSlot, Value(-1), "ExpressionIndexOfCP"_sd);
    }
    {
        auto pattern = ExpressionConstant::create(_expCtx.get(), Value("test"_sd));
        ExpressionRegexFind regFindExpr(
            _expCtx.get(), strFieldExpr, pattern, nullptr, "$regexFind"_sd);
        runTest(&regFindExpr,
                rootSlot,
                Value(BSON("match" << "test"
                                   << "idx" << 10 << "captures" << BSONArray())),
                "ExpressionRegexFind"_sd);
    }
    {
        auto pattern = ExpressionFieldPath::createPathFromString(
            _expCtx.get(), "pattern", _expCtx->variablesParseState);
        ExpressionRegexFindAll regFindAllExpr(
            _expCtx.get(), strFieldExpr, pattern, nullptr, "$regexFindAll"_sd);
        runTest(&regFindAllExpr,
                rootSlot,
                Value(BSON_ARRAY(BSON("match" << "test"
                                              << "idx" << 10 << "captures" << BSONArray()))),
                "ExpressionRegexFindAll"_sd);
    }
    {
        auto pattern = ExpressionConstant::create(_expCtx.get(), Value("test"_sd));
        ExpressionRegexMatch regMatchExpr(
            _expCtx.get(), strFieldExpr, pattern, nullptr, "$regexMatch"_sd);
        runTest(&regMatchExpr, rootSlot, Value(true), "ExpressionRegexMatch"_sd);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprMath) {
    auto root = BSON("_id" << 1 << "field1" << 4 << "field2" << 0.75);
    auto rootSlotId = _env->registerSlot("root"_sd,
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
        runTest(&absExpr, rootSlot, expected, "ExpressionAbs"_sd);
    }
    {
        Value val = Value(100), expected = Value(104);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionAdd addExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&addExpr, rootSlot, expected, "ExpressionAdd"_sd);
    }
    {
        Value val = Value(9.25), expected = Value(10);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionCeil ceilExpr(_expCtx.get(), {constExpr});
        runTest(&ceilExpr, rootSlot, expected, "ExpressionCeil"_sd);
    }
    {
        Value val = Value(9), expected = Value(2.25);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionDivide divideExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&divideExpr, rootSlot, expected, "ExpressionDivide"_sd);
    }
    {
        Value val = Value(0), expected = Value(1);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionExp expExpr(_expCtx.get(), {constExpr});
        runTest(&expExpr, rootSlot, expected, "ExpressionExp"_sd);
    }
    {
        Value val = Value(9.25), expected = Value(9);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionFloor floorExpr(_expCtx.get(), {constExpr});
        runTest(&floorExpr, rootSlot, expected, "ExpressionFloor"_sd);
    }
    {
        Value val = Value(1);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionLn lnExpr(_expCtx.get(), {constExpr});
        runTest(&lnExpr, rootSlot, Value(0), "ExpressionLn"_sd);
    }
    {
        Value val = Value(10);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionLog10 log10Expr(_expCtx.get(), {constExpr});
        runTest(&log10Expr, rootSlot, Value(1), "ExpressionLog10"_sd);
    }
    {
        Value val = Value(19), expected = Value(3);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionMod modExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&modExpr, rootSlot, expected, "ExpressionMod"_sd);
    }
    {
        Value val = Value(4.5), expected = Value(18);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionMultiply multiplyExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&multiplyExpr, rootSlot, expected, "ExpressionMultiply"_sd);
    }
    {
        Value val = Value(4), expected = Value(256);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionPow powExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&powExpr, rootSlot, expected, "ExpressionPow"_sd);
    }
    {
        Value val = Value(1.123456), expected = Value(1.1235);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
        ExpressionRound roundExpr(_expCtx.get(), {constExpr, field1Expr});
        runTest(&roundExpr, rootSlot, expected, "ExpressionRound"_sd);
    }
    {
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(10));
        ExpressionSubtract subtractExpr(_expCtx.get(), {field1Expr, constExpr});
        runTest(&subtractExpr, rootSlot, Value(-6), "ExpressionSubtract"_sd);
    }
    {
        ExpressionSqrt sqrtExpr(_expCtx.get(), {field1Expr});
        runTest(&sqrtExpr, rootSlot, Value(2), "ExpressionSqrt"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionCosine> cosExpr{
            new ExpressionCosine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {cosExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(-0.6536), "ExpressionCosine"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionSine> sinExpr{
            new ExpressionSine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {sinExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(-0.7568), "ExpressionSine"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionTangent> tanExpr{
            new ExpressionTangent(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {tanExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(1.1578), "ExpressionTangent"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionArcCosine> acosExpr{
            new ExpressionArcCosine(_expCtx.get(), {field2Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {acosExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.7227), "ExpressionArcCosine"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionArcSine> asinExpr{
            new ExpressionArcSine(_expCtx.get(), {field2Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {asinExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.848), "ExpressionArcSine"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionArcTangent> atanExpr{
            new ExpressionArcTangent(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {atanExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(1.3258), "ExpressionArcTangent"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionArcTangent2> atan2Expr{
            new ExpressionArcTangent2(_expCtx.get(), {field1Expr, field2Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {atan2Expr, constExpr});
        runTest(&truncExpr, rootSlot, Value(1.3854), "ExpressionArcTangent2"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicArcTangent> atanhExpr{
            new ExpressionHyperbolicArcTangent(_expCtx.get(), {field2Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {atanhExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.9729), "ExpressionHyperbolicArcTangent"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicArcCosine> acoshExpr{
            new ExpressionHyperbolicArcCosine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {acoshExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(2.0634), "ExpressionHyperbolicArcCosine"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicArcSine> asinhExpr{
            new ExpressionHyperbolicArcSine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {asinhExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(2.0947), "ExpressionHyperbolicArcSine"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicCosine> coshExpr{
            new ExpressionHyperbolicCosine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {coshExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(27.3082), "ExpressionHyperbolicCosine"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicSine> sinhExpr{
            new ExpressionHyperbolicSine(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {sinhExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(27.2899), "ExpressionHyperbolicSine"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionHyperbolicTangent> tanhExpr{
            new ExpressionHyperbolicTangent(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {tanhExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.9993), "ExpressionHyperbolicTangent"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionDegreesToRadians> dtrExpr{
            new ExpressionDegreesToRadians(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {dtrExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(0.0698), "ExpressionDegreesToRadians"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionRadiansToDegrees> rtdExpr{
            new ExpressionRadiansToDegrees(_expCtx.get(), {field1Expr})};
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(3));
        ExpressionTrunc truncExpr(_expCtx.get(), {rtdExpr, constExpr});
        runTest(&truncExpr, rootSlot, Value(229.183), "ExpressionRadiansToDegrees"_sd);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprCmp) {
    auto root = BSON("_id" << 0 << "bar" << 5);
    auto rootSlotId = _env->registerSlot("root"_sd,
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
        runTest(&cmpExpr, rootSlot, Value(false), "ExpressionCompareEQ"_sd);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::NE, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(true), "ExpressionCompareNE"_sd);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::GT, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(true), "ExpressionCompareGT"_sd);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::GTE, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(true), "ExpressionCompareGTE"_sd);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::LT, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(false), "ExpressionCompareLT"_sd);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::LTE, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(false), "ExpressionCompareLTE"_sd);
    }
    {
        ExpressionCompare cmpExpr(_expCtx.get(), ExpressionCompare::CMP, {constExpr, fieldExpr});
        runTest(&cmpExpr, rootSlot, Value(1), "ExpressionCompareCMP"_sd);
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
    auto rootSlotId = _env->registerSlot("root"_sd,
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
            ExpressionConstant::create(_expCtx.get(), Value("quarter"_sd)) /* unit */,
            ExpressionConstant::create(_expCtx.get(), Value(1)) /* amount */,
            ExpressionConstant::create(_expCtx.get(), Value("America/New_York"_sd)) /* timezone */,
            "$dateAdd"_sd);
        // expect 02/18/2025 10:43:44 PM GMT
        runTest(&dateAddExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1739918624000)),
                "ExpressionDateAdd"_sd);
    }
    {
        ExpressionDateSubtract dateSubExpr(
            _expCtx.get(),
            date1 /* startDate */,
            ExpressionConstant::create(_expCtx.get(), Value("week"_sd)) /* unit */,
            ExpressionConstant::create(_expCtx.get(), Value(1)) /* amount */,
            ExpressionConstant::create(_expCtx.get(), Value("America/New_York"_sd)) /* timezone */,
            "$dateAdd"_sd);
        // expect 11/11/2024 10:43:44 PM
        runTest(&dateSubExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1731365024000)),
                "ExpressionDateSubtract"_sd);
    }
    {
        ExpressionDateDiff dateDiffExpr(
            _expCtx.get(),
            date1 /* startDate */,
            date2 /* endDate */,
            ExpressionConstant::create(_expCtx.get(), Value("week"_sd)) /* unit */,
            ExpressionConstant::create(_expCtx.get(), Value("America/New_York"_sd)) /* timezone */,
            nullptr /* startOfWeek */);
        runTest(&dateDiffExpr, rootSlot, Value(-53), "ExpressionDateDiff"_sd);
    }
    {
        ExpressionDateFromString dateFromStrExpr(
            _expCtx.get(),
            dateStr,
            ExpressionConstant::create(_expCtx.get(), Value("GMT"_sd)) /* timezone */,
            ExpressionConstant::create(_expCtx.get(), Value("%m/%d/%Y %H:%M:%S"_sd)) /* format */,
            nullptr /* onNull */,
            nullptr /* onError */);
        runTest(&dateFromStrExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1731924900000)),
                "ExpressionDateFromString"_sd);
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
                "ExpressionDateFromParts"_sd);
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
                "ExpressionDateToParts"_sd);
    }
    {
        ExpressionDateToString dateToStrExpr(
            _expCtx.get(),
            date1 /* date */,
            ExpressionConstant::create(_expCtx.get(), Value("%m/%d/%Y %H:%M:%S"_sd)) /* format */,
            nullptr /* timeZone */,
            ExpressionConstant::create(_expCtx.get(), Value(false)) /* onNull */);
        runTest(
            &dateToStrExpr, rootSlot, Value("11/18/2024 22:43:44"_sd), "ExpressionDateToString"_sd);
    }
    {
        ExpressionDateTrunc dateTruncExpr(
            _expCtx.get(),
            date1 /* date */,
            ExpressionConstant::create(_expCtx.get(), Value("week"_sd)) /* unit */,
            nullptr /* binSize */,
            nullptr /* timeZone */,
            nullptr /* startOfWeek */);
        // expected 11/17/2024 12:00:00 AM
        runTest(&dateTruncExpr,
                rootSlot,
                Value(Date_t::fromMillisSinceEpoch(1731801600000)),
                "ExpressionDateTrunc"_sd);
    }
    {
        ExpressionDayOfMonth dayOfMonthExpr(_expCtx.get(), date1);
        runTest(&dayOfMonthExpr, rootSlot, Value(18), "ExpressionDayOfMonth"_sd);
    }
    {
        ExpressionDayOfWeek dayOfWeekExpr(_expCtx.get(), date1);
        runTest(&dayOfWeekExpr, rootSlot, Value(2), "ExpressionDayOfWeek"_sd);
    }
    {
        ExpressionDayOfYear dayOfYearExpr(_expCtx.get(), date1);
        runTest(&dayOfYearExpr, rootSlot, Value(323), "ExpressionDayOfYear"_sd);
    }
    {
        ExpressionHour hourExpr(_expCtx.get(), date1);
        runTest(&hourExpr, rootSlot, Value(22), "ExpressionHour"_sd);
    }
    {
        ExpressionMillisecond millisecExpr(_expCtx.get(), date1);
        runTest(&millisecExpr, rootSlot, Value(0), "ExpressionMillisecond"_sd);
    }
    {
        ExpressionMinute minuteExpr(_expCtx.get(), date1);
        runTest(&minuteExpr, rootSlot, Value(43), "ExpressionMinute"_sd);
    }
    {
        ExpressionMonth monthExpr(_expCtx.get(), date1);
        runTest(&monthExpr, rootSlot, Value(11), "ExpressionMonth"_sd);
    }
    {
        ExpressionSecond secondExpr(_expCtx.get(), date1);
        runTest(&secondExpr, rootSlot, Value(44), "ExpressionSecond"_sd);
    }
    {
        ExpressionWeek weekExpr(_expCtx.get(), date1);
        runTest(&weekExpr, rootSlot, Value(46), "ExpressionWeek"_sd);
    }
    {
        ExpressionIsoWeekYear isoWeekYearExpr(_expCtx.get(), date1);
        runTest(&isoWeekYearExpr, rootSlot, Value(2024), "ExpressionIsoWeekYear"_sd);
    }
    {
        ExpressionIsoDayOfWeek isoDayOfWeekExpr(_expCtx.get(), date1);
        runTest(&isoDayOfWeekExpr, rootSlot, Value(1), "ExpressionIsoDayOfWeek"_sd);
    }
    {
        ExpressionYear yearExpr(_expCtx.get(), date1);
        runTest(&yearExpr, rootSlot, Value(2024), "ExpressionYear"_sd);
    }
    {
        ExpressionTsSecond tsSecExpr(_expCtx.get(), {ts});
        runTest(&tsSecExpr, rootSlot, Value(328), "ExpressionTsSecond"_sd);
    }
    {
        ExpressionTsIncrement tsIncrExpr(_expCtx.get(), {ts});
        runTest(&tsIncrExpr, rootSlot, Value(3431613912LL), "ExpressionTsIncrement"_sd);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprAccumulator) {
    auto root = BSON("_id" << 0 << "bar" << 5 << "arr" << BSON_ARRAY(1 << 2.5));
    auto rootSlotId = _env->registerSlot("root"_sd,
                                         sbe::value::TypeTags::bsonObject,
                                         sbe::value::bitcastFrom<const char*>(root.objdata()),
                                         false,
                                         &_slotIdGenerator);
    auto rootSlot = SbSlot{rootSlotId, TypeSignature::kObjectType};

    Value val = Value(9.25);
    auto constExpr = ExpressionConstant::create(_expCtx.get(), val);
    auto fieldExpr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "bar", _expCtx->variablesParseState);
    auto arrExpr = ExpressionFieldPath::createPathFromString(
        _expCtx.get(), "arr", _expCtx->variablesParseState);

    {
        ExpressionFromAccumulator<AccumulatorAvg> accumAvgExpr(_expCtx.get());
        accumAvgExpr.addOperand(fieldExpr);
        accumAvgExpr.addOperand(constExpr);
        runTest(
            &accumAvgExpr, rootSlot, Value(7.125), "ExpressionFromAccumulator<AccumulatorAvg>"_sd);
    }
    {
        ExpressionFromAccumulator<AccumulatorMax> accumMaxExpr(_expCtx.get());
        accumMaxExpr.addOperand(arrExpr);
        runTest(
            &accumMaxExpr, rootSlot, Value(2.5), "ExpressionFromAccumulator<AccumulatorMax>"_sd);
    }
    {
        ExpressionFromAccumulator<AccumulatorMin> accumMinExpr(_expCtx.get());
        accumMinExpr.addOperand(fieldExpr);
        accumMinExpr.addOperand(constExpr);
        runTest(&accumMinExpr, rootSlot, Value(5), "ExpressionFromAccumulator<AccumulatorMin>"_sd);
    }
    {
        ExpressionFromAccumulator<AccumulatorStdDevPop> accumStdDevPopExpr(_expCtx.get());
        accumStdDevPopExpr.addOperand(arrExpr);
        runTest(&accumStdDevPopExpr,
                rootSlot,
                Value(0.75),
                "ExpressionFromAccumulator<AccumulatorStdDevPop>"_sd);
    }
    {
        boost::intrusive_ptr<ExpressionFromAccumulator<AccumulatorStdDevSamp>> accumStdDevSampExpr{
            new ExpressionFromAccumulator<AccumulatorStdDevSamp>(_expCtx.get())};
        accumStdDevSampExpr->addOperand(arrExpr);
        auto constExpr = ExpressionConstant::create(_expCtx.get(), Value(4));
        ExpressionTrunc truncExpr(_expCtx.get(), {accumStdDevSampExpr, constExpr});
        runTest(&truncExpr,
                rootSlot,
                Value(1.0606),
                "ExpressionFromAccumulator<AccumulatorStdDevSamp>"_sd);
    }
    {
        ExpressionFromAccumulator<AccumulatorSum> accumSumExpr(_expCtx.get());
        accumSumExpr.addOperand(fieldExpr);
        accumSumExpr.addOperand(constExpr);
        runTest(
            &accumSumExpr, rootSlot, Value(14.25), "ExpressionFromAccumulator<AccumulatorSum>"_sd);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprArraySet) {
    auto root = BSON("_id" << 0 << "arr1" << BSONArray() << "arr2" << BSON_ARRAY(1 << 2.5 << "str")
                           << "arr3" << BSON_ARRAY(1 << 5 << "str2"));
    auto rootSlotId = _env->registerSlot("root"_sd,
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
        runTest(&concatArrExpr, rootSlot, Value(expected), "ExpressionConcatArrays"_sd);
    }
    {
        ExpressionSetDifference setDiffExpr(_expCtx.get(), {fieldArr2Expr, fieldArr3Expr});
        auto [tag, val] = sbe::value::makeNewArraySet();
        sbe::value::ValueGuard valGuard{tag, val};
        sbe::value::getArraySetView(val)->push_back(sbe::value::makeValue(Value(2.5)));
        sbe::value::getArraySetView(val)->push_back(sbe::value::makeValue(Value("str"_sd)));
        runTest(&setDiffExpr, rootSlot, tag, val, "ExpressionSetDifference"_sd);
    }
    {
        ExpressionSetEquals setEqExpr(_expCtx.get(), {fieldArr2Expr, fieldArr3Expr});
        runTest(&setEqExpr, rootSlot, Value(false), "ExpressionSetEquals"_sd);
    }
    {
        ExpressionSetIntersection setInterExpr(_expCtx.get(), {fieldArr2Expr, fieldArr3Expr});
        auto expected = BSON_ARRAY(1);
        runTest(&setInterExpr, rootSlot, Value(expected), "ExpressionSetIntersection"_sd);
    }
    {
        ExpressionSetIsSubset setIsSubsetExpr(_expCtx.get(), {constArrExpr, fieldArr2Expr});
        runTest(&setIsSubsetExpr, rootSlot, Value(true), "ExpressionSetIsSubset"_sd);
    }
    {
        ExpressionSetUnion setUnionExpr(_expCtx.get(), {fieldArr2Expr, fieldArr3Expr});
        auto [tag, val] = sbe::value::makeNewArraySet();
        sbe::value::ValueGuard valGuard{tag, val};
        sbe::value::getArraySetView(val)->push_back(sbe::value::makeValue(Value(1)));
        sbe::value::getArraySetView(val)->push_back(sbe::value::makeValue(Value(2.5)));
        sbe::value::getArraySetView(val)->push_back(sbe::value::makeValue(Value("str"_sd)));
        sbe::value::getArraySetView(val)->push_back(sbe::value::makeValue(Value(5)));
        sbe::value::getArraySetView(val)->push_back(sbe::value::makeValue(Value("str2"_sd)));
        runTest(&setUnionExpr, rootSlot, tag, val, "ExpressionSetUnion"_sd);
    }
    {
        ExpressionReverseArray arrReverseExpr(_expCtx.get());
        arrReverseExpr.addOperand(fieldArr2Expr);
        auto expected = BSON_ARRAY("str" << 2.5 << 1);
        runTest(&arrReverseExpr, rootSlot, Value(expected), "ExpressionReverseArray"_sd);
    }
    {
        BSONObj expr = fromjson("{ $sortArray: { input: '$arr2', sortBy: -1 } }");
        auto arrReverseExpr = ExpressionSortArray::parse(
            _expCtx.get(), expr.firstElement(), _expCtx->variablesParseState);
        auto expected = BSON_ARRAY("str" << 2.5 << 1);
        runTest(arrReverseExpr.get(), rootSlot, Value(expected), "ExpressionSortArray"_sd);
    }
    {
        ExpressionIsArray isArrExpr(_expCtx.get(), {fieldArr2Expr});
        runTest(&isArrExpr, rootSlot, Value(true), "ExpressionIsArray"_sd);
    }
}

TEST_F(GoldenGenExpressionTest, TestExprMultiplyLarge) {
    auto root = BSON("_id" << 0 << "bar" << 0.5);
    auto rootSlotId = _env->registerSlot("root"_sd,
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
    runTest(&multiplyExpr, rootSlot, expected, "ExpressionMultiply"_sd);
}

TEST_F(GoldenGenExpressionTest, TestExprAddLarge) {
    auto root = BSON("_id" << 0 << "bar" << 5);
    auto rootSlotId = _env->registerSlot("root"_sd,
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
    runTest(&addExpr, rootSlot, expected, "ExpressionAdd"_sd);
}

}  // namespace mongo::stage_builder
