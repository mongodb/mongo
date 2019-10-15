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
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface_standalone.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/scripting/engine.h"

namespace mongo {
namespace {

class MapReduceFixture : public ServiceContextMongoDTest {
protected:
    MapReduceFixture() : _expCtx((new ExpressionContextForTest())) {
        _expCtx->mongoProcessInterface = std::make_shared<MongoInterfaceStandalone>(_expCtx->opCtx);
    }

    boost::intrusive_ptr<ExpressionContextForTest>& getExpCtx() {
        return _expCtx;
    }

private:
    void setUp() override;
    void tearDown() override;

    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};


void MapReduceFixture::setUp() {
    setTestCommandsEnabled(true);
    ServiceContextMongoDTest::setUp();
    ScriptEngine::setup();
}

void MapReduceFixture::tearDown() {
    ScriptEngine::dropScopeCache();
    ServiceContextMongoDTest::tearDown();
}

namespace InternalJsReduce {

template <typename AccName>
static void assertProcessFailsWithCode(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       Value processArgument,
                                       int code) {
    auto accum = AccName::create(expCtx);
    ASSERT_THROWS_CODE(accum->process(processArgument, false), AssertionException, code);
}

template <typename AccName>
static void assertExpectedResults(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  std::string eval,
                                  std::vector<Value> data,
                                  Value expectedResult) {
    // Asserts that result equals expected result when not sharded.
    {
        auto accum = AccName::create(expCtx);
        for (auto&& val : data) {
            auto input = Value(DOC("eval" << eval << "data" << val));
            accum->process(input, false);
        }
        Value result = accum->getValue(false);
        ASSERT_VALUE_EQ(expectedResult, result);
        ASSERT_EQUALS(expectedResult.getType(), result.getType());
    }

    // Asserts that result equals expected result when all input is on one shard.
    {
        auto accum = AccName::create(expCtx);
        auto shard = AccName::create(expCtx);
        for (auto&& val : data) {
            auto input = Value(DOC("eval" << eval << "data" << val));
            shard->process(input, false);
        }
        accum->process(shard->getValue(true), true);
        Value result = accum->getValue(false);
        ASSERT_VALUE_EQ(expectedResult, result);
        ASSERT_EQUALS(expectedResult.getType(), result.getType());
    }

    // Asserts that result equals expected result when each input is on a separate shard.
    {
        auto accum = AccName::create(expCtx);
        for (auto&& val : data) {
            auto input = Value(DOC("eval" << eval << "data" << val));
            auto shard = AccName::create(expCtx);
            shard->process(input, false);
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
    auto accum = AccumulatorInternalJsReduce::create(getExpCtx());

    // A non-idempotent Javascript function will produce non-idempotent results. In this case a
    // single document reduce causes a change in value.
    auto input =
        Value(DOC("eval" << std::string("function(key, values) { return Array.sum(values) + 1; };")
                         << "data" << Value(DOC("k" << std::string("foo") << "v" << Value(5)))));
    auto expectedResult = Value(6.0);

    accum->process(input, false);
    Value result = accum->getValue(false);

    ASSERT_VALUE_EQ(expectedResult, result);
    ASSERT_EQUALS(expectedResult.getType(), result.getType());
}

TEST_F(MapReduceFixture, InternalJsReduceFailsWhenEvalContainsInvalidJavascript) {
    // Multiple source documents.
    {
        auto accum = AccumulatorInternalJsReduce::create(getExpCtx());

        auto input = Value(DOC("eval" << std::string("INVALID_JAVASCRIPT") << "data"
                                      << Value(DOC("k" << Value(1) << "v" << Value(2)))));
        accum->process(input, false);
        accum->process(input, false);

        ASSERT_THROWS_CODE(accum->getValue(false), DBException, ErrorCodes::JSInterpreterFailure);
    }

    // Single source document.
    {
        auto accum = AccumulatorInternalJsReduce::create(getExpCtx());

        auto input = Value(DOC("eval" << std::string("INVALID_JAVASCRIPT") << "data"
                                      << Value(DOC("k" << Value(1) << "v" << Value(2)))));
        accum->process(input, false);

        ASSERT_THROWS_CODE(accum->getValue(false), DBException, ErrorCodes::JSInterpreterFailure);
    }
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfArgumentNotDocument) {

    auto argument = Value(2);
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), argument, 31242);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfEvalAndDataArgumentsNotProvided) {
    // Data argument missing.
    auto argument =
        Value(DOC("eval" << std::string("function(key, values) { return Array.sum(values); };")));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), argument, 31243);

    // Eval argument missing.
    argument = Value(DOC("data" << Value(DOC("k" << Value(1) << "v" << Value(2)))));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), argument, 31243);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfEvalArgumentNotOfTypeStringOrCode) {
    auto argument = Value(
        DOC("eval" << 1 << "data" << Value(DOC("k" << std::string("foo") << "v" << Value(2)))));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), argument, 31244);

    // MapReduce does not accept JavaScript function of BSON type CodeWScope.
    BSONObjBuilder objBuilder;
    objBuilder.appendCodeWScope(
        "eval", "function(key, values) { return Array.sum(values); };", BSONObj());
    objBuilder.append("data", BSON("k" << std::string("foo") << "v" << Value(2)));

    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(
        getExpCtx(), Value(objBuilder.obj()), 31244);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfDataArgumentNotDocument) {
    auto argument =
        Value(DOC("eval" << std::string("function(key, values) { return Array.sum(values); };")
                         << "data" << Value(2)));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), argument, 31245);
}

TEST_F(MapReduceFixture, InternalJsReduceFailsIfDataArgumentDoesNotContainExpectedFields) {
    // No "k" field.
    auto argument = Value(
        DOC("eval" << std::string("function(key, values) { return Array.sum(values); };") << "data"
                   << Value(DOC("foo" << std::string("keyVal") << "v" << Value(2)))));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), argument, 31251);

    // No "v" field.
    argument = Value(
        DOC("eval" << std::string("function(key, values) { return Array.sum(values); };") << "data"
                   << Value(DOC("k" << std::string("keyVal") << "bar" << Value(2)))));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), argument, 31251);

    // Both "k" and "v" fields missing.
    argument =
        Value(DOC("eval" << std::string("function(key, values) { return Array.sum(values); };")
                         << "data" << Value(Document())));
    assertProcessFailsWithCode<AccumulatorInternalJsReduce>(getExpCtx(), argument, 31251);
}

}  // namespace InternalJsReduce
}  // namespace
}  // namespace mongo
