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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/accumulator_js_reduce.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/scripting/engine.h"

namespace mongo {
namespace {

class MapReduceFixture : public ServiceContextMongoDTest {
protected:
    MapReduceFixture() {
        _expCtx.mongoProcessInterface = std::make_shared<StandaloneProcessInterface>(nullptr);
    }

    auto getExpCtx() {
        return &_expCtx;
    }

private:
    void setUp() override;
    void tearDown() override;

    ExpressionContextForTest _expCtx;
};


void MapReduceFixture::setUp() {
    ServiceContextMongoDTest::setUp();
    ScriptEngine::setup(false);
}

void MapReduceFixture::tearDown() {
    ScriptEngine::dropScopeCache();
    ServiceContextMongoDTest::tearDown();
}

namespace InternalJsReduce {

template <typename AccName>
static void assertProcessFailsWithCode(ExpressionContext* const expCtx,
                                       const std::string& eval,
                                       Value processArgument,
                                       int code) {
    auto accum = AccName::create(expCtx, eval);
    ASSERT_THROWS_CODE(accum->process(processArgument, false), AssertionException, code);
}

static void assertParsingFailsWithCode(ExpressionContext* const expCtx,
                                       BSONElement elem,
                                       int code) {
    ASSERT_THROWS_CODE(AccumulatorInternalJsReduce::parseInternalJsReduce(
                           expCtx, elem, expCtx->variablesParseState),
                       AssertionException,
                       code);
}

template <typename AccName>
static void assertExpectedResults(ExpressionContext* const expCtx,
                                  const std::string& eval,
                                  std::vector<Value> data,
                                  Value expectedResult) {
    // Asserts that result equals expected result when not sharded.
    {
        auto accum = AccName::create(expCtx, eval);
        for (auto&& val : data) {
            accum->process(val, false);
        }
        Value result = accum->getValue(false);
        ASSERT_VALUE_EQ(expectedResult, result);
        ASSERT_EQUALS(expectedResult.getType(), result.getType());
    }

    // Asserts that result equals expected result when all input is on one shard.
    {
        auto accum = AccName::create(expCtx, eval);
        auto shard = AccName::create(expCtx, eval);
        for (auto&& val : data) {
            shard->process(val, false);
        }
        accum->process(shard->getValue(true), true);
        Value result = accum->getValue(false);
        ASSERT_VALUE_EQ(expectedResult, result);
        ASSERT_EQUALS(expectedResult.getType(), result.getType());
    }

    // Asserts that result equals expected result when each input is on a separate shard.
    {
        auto accum = AccName::create(expCtx, eval);
        for (auto&& val : data) {
            auto shard = AccName::create(expCtx, eval);
            shard->process(val, false);
            accum->process(shard->getValue(true), true);
        }
        Value result = accum->getValue(false);
        ASSERT_VALUE_EQ(expectedResult, result);
        ASSERT_EQUALS(expectedResult.getType(), result.getType());
    }
}

TEST_F(MapReduceFixture, InternalJsReduceProducesExpectedResults) {
    // Null value.
    assertExpectedResults<AccumulatorInternalJsReduce>(
        getExpCtx(),
        "function(key, value) { return null; };",
        {Value(DOC("k" << 1 << "v" << Value(BSONNULL)))},
        Value(BSONNULL));

    // Multiple inputs.
    assertExpectedResults<AccumulatorInternalJsReduce>(
        getExpCtx(),
        "function(key, values) { return Array.sum(values); };",
        {Value(DOC("k" << std::string("foo") << "v" << Value(2))),
         Value(DOC("k" << std::string("foo") << "v" << Value(5)))},
        Value(7.0));

    // Multiple inputs, numeric key.
    assertExpectedResults<AccumulatorInternalJsReduce>(
        getExpCtx(),
        "function(key, values) { return Array.sum(values); };",
        {Value(DOC("k" << 1 << "v" << Value(2))), Value(DOC("k" << 1 << "v" << Value(5)))},
        Value(7.0));
}

TEST_F(MapReduceFixture, InternalJsReduceIdempotentOnlyWhenJSFunctionIsIdempotent) {
    std::string eval("function(key, values) { return Array.sum(values) + 1; };");
    auto accum = AccumulatorInternalJsReduce::create(getExpCtx(), eval);

    // A non-idempotent Javascript function will produce non-idempotent results. In this case a
    // single document reduce causes a change in value.
    auto input = Value(DOC("k" << std::string("foo") << "v" << Value(5)));
    auto expectedResult = Value(6.0);

    accum->process(input, false);
    Value result = accum->getValue(false);

    ASSERT_VALUE_EQ(expectedResult, result);
    ASSERT_EQUALS(expectedResult.getType(), result.getType());
}

TEST_F(MapReduceFixture, InternalJsReduceFailsWhenEvalContainsInvalidJavascript) {
    std::string eval("INVALID_JAVASCRIPT");
    // Multiple source documents.
    {
        auto accum = AccumulatorInternalJsReduce::create(getExpCtx(), "INVALID_JAVASCRIPT");
        auto input = Value(DOC("k" << Value(1) << "v" << Value(2)));
        accum->process(input, false);
        accum->process(input, false);

        ASSERT_THROWS_CODE(accum->getValue(false), DBException, ErrorCodes::JSInterpreterFailure);
    }

    // Single source document.
    {
        auto accum = AccumulatorInternalJsReduce::create(getExpCtx(), "INVALID_JAVASCRIPT");

        auto input = Value(DOC("k" << Value(1) << "v" << Value(2)));
        accum->process(input, false);

        ASSERT_THROWS_CODE(accum->getValue(false), DBException, ErrorCodes::JSInterpreterFailure);
    }
}

TEST_F(
    MapReduceFixture,
    InternalJsReduceFailsDependentOnDocumentCountWhenEvalIsInvalidJavascriptWithSingleReduceOpt) {
    RAIIServerParameterControllerForTest flag("mrEnableSingleReduceOptimization", true);
    std::string eval("INVALID_JAVASCRIPT");
    // Multiple source documents should evaluate the passed in function and return an error with
    // invalid javascript.
    {
        auto accum = AccumulatorInternalJsReduce::create(getExpCtx(), "INVALID_JAVASCRIPT");
        auto input = Value(DOC("k" << Value(1) << "v" << Value(2)));
        accum->process(input, false);
        accum->process(input, false);

        ASSERT_THROWS_CODE(accum->getValue(false), DBException, ErrorCodes::JSInterpreterFailure);
    }

    // Single source document. With the reduce optimization, we simply return this document rather
    // than executing the JS engine at all, so no error is thrown.
    {
        auto accum = AccumulatorInternalJsReduce::create(getExpCtx(), "INVALID_JAVASCRIPT");

        auto input = Value(DOC("k" << Value(1) << "v" << Value(2)));
        auto expectedResult = Value(2);

        accum->process(input, false);
        Value result = accum->getValue(false);

        ASSERT_VALUE_EQ(expectedResult, result);
        ASSERT_EQUALS(expectedResult.getType(), result.getType());
    }
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfArgumentNotDocument) {
    auto argument = Value(2);
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(
        getExpCtx(), "function(key, values) { return Array.sum(values); };", argument, 31242);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfEvalAndDataArgumentsNotProvided) {
    // Data argument missing.
    BSONObjBuilder noData;
    noData.append("$_internalJsReduce",
                  BSON("eval"
                       << "function(key, values) { return Array.sum(values); };"));
    assertParsingFailsWithCode(getExpCtx(), noData.obj().getField("$_internalJsReduce"), 31349);

    // Eval argument missing.
    BSONObjBuilder noEval;
    noEval.append("$_internalJsReduce", BSON("data" << BSON("k" << Value(1) << "v" << Value(2))));
    assertParsingFailsWithCode(getExpCtx(), noEval.obj().getField("$_internalJsReduce"), 31245);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfExtraArgumentsAreSpecified) {
    // Data argument missing.
    BSONObjBuilder obj;
    obj.append("eval", "function(key, values) { return Array.sum(values); };");
    obj.append("data", BSON("k" << std::string("foo") << "v" << Value(2)));
    obj.append("extraField", 1);
    BSONObjBuilder wrap;
    wrap.append("$_internalJsReduce", obj.obj());
    assertParsingFailsWithCode(getExpCtx(), wrap.obj().getField("$_internalJsReduce"), 31243);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfEvalArgumentNotOfTypeStringOrCode) {
    BSONObjBuilder codeTypeInt;
    codeTypeInt.appendNumber("eval", 1);
    codeTypeInt.append("data", BSON("k" << std::string("foo") << "v" << Value(2)));
    BSONObjBuilder wrapInt;
    wrapInt.append("$_internalJsReduce", codeTypeInt.obj());
    assertParsingFailsWithCode(getExpCtx(), wrapInt.obj().getField("$_internalJsReduce"), 31244);

    // MapReduce does not accept JavaScript function of BSON type CodeWScope.
    BSONObjBuilder objBuilder;
    objBuilder.appendCodeWScope(
        "eval", "function(key, values) { return Array.sum(values); };", BSONObj());
    objBuilder.append("data", BSON("k" << std::string("foo") << "v" << Value(2)));
    BSONObjBuilder wrapCode;
    wrapCode.append("$_internalJsReduce", objBuilder.obj());
    assertParsingFailsWithCode(getExpCtx(), wrapCode.obj().getField("$_internalJsReduce"), 31244);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfDataArgumentNotDocument) {
    std::string eval("function(key, values) { return Array.sum(values); };");
    auto argument = Value(Value(2));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(
        getExpCtx(), std::move(eval), argument, 31242);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfDataArgumentDoesNotContainExpectedFields) {
    std::string eval("function(key, values) { return Array.sum(values); };");
    // No "k" field.
    auto argument = Value(DOC(
        "eval" << eval << "data" << Value(DOC("foo" << std::string("keyVal") << "v" << Value(2)))));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), eval, argument, 31251);

    // No "v" field.
    argument = Value(
        DOC("eval" << std::string("function(key, values) { return Array.sum(values); };") << "data"
                   << Value(DOC("k" << std::string("keyVal") << "bar" << Value(2)))));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), eval, argument, 31251);

    // Both "k" and "v" fields missing.
    argument =
        Value(DOC("eval" << std::string("function(key, values) { return Array.sum(values); };")
                         << "data" << Value(Document())));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), eval, argument, 31251);
}

}  // namespace InternalJsReduce
}  // namespace
}  // namespace mongo
