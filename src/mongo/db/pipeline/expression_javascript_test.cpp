/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/pipeline/expression_js_emit.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/scripting/engine.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {

class MapReduceFixture : public ServiceContextMongoDTest {
protected:
    MapReduceFixture()
        : _expCtx((new ExpressionContextForTest())), _vps(_expCtx->variablesParseState) {
        _expCtx->mongoProcessInterface = std::make_shared<StandaloneProcessInterface>(nullptr);
    }

    auto& getExpCtx() {
        return _expCtx;
    }
    auto getExpCtxRaw() {
        return _expCtx.get();
    }

    const VariablesParseState& getVPS() {
        return _vps;
    }

    Variables* getVariables() {
        return &_expCtx->variables;
    }

private:
    void setUp() override;
    void tearDown() override;

    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
    VariablesParseState _vps;
};


void MapReduceFixture::setUp() {
    ServiceContextMongoDTest::setUp();
    ScriptEngine::setup(ExecutionEnvironment::Server);
}

void MapReduceFixture::tearDown() {
    ScriptEngine::dropScopeCache();
    ServiceContextMongoDTest::tearDown();
}

TEST_F(MapReduceFixture, ExpressionFunctionProducesExpectedResult) {
    auto bsonExpr = BSON("expr" << BSON("body"
                                        << "function(first, second) {return first + second;};"
                                        << "args" << BSON_ARRAY("$a" << 4) << "lang"
                                        << ExpressionFunction::kJavaScript));

    auto expr = ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());

    Value result = expr->evaluate(Document{BSON("a" << 2)}, getVariables());
    ASSERT_VALUE_EQ(result, Value(6));

    bsonExpr =
        BSON("expr" << BSON("body"
                            << "function(first, second, third) {return first + second + third;};"
                            << "args" << BSON_ARRAY(1 << 2 << 4) << "lang"
                            << ExpressionFunction::kJavaScript));
    expr = ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());
    result = expr->evaluate(Document{BSONObj{}}, getVariables());
    ASSERT_VALUE_EQ(result, Value(7));

    bsonExpr = BSON("expr" << BSON("body"
                                   << "function(first) {return first;};"
                                   << "args" << BSON_ARRAY(1) << "lang"
                                   << ExpressionFunction::kJavaScript));
    expr = ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());
    result = expr->evaluate(Document{BSONObj{}}, getVariables());
    ASSERT_VALUE_EQ(result, Value(1));
}

TEST_F(MapReduceFixture, ExpressionFunctionFailsIfArgsDoesNotEvaluateToArray) {
    auto bsonExpr = BSON("expr" << BSON("body"
                                        << "function(first, second) {return first + second;};"
                                        << "args" << BSON("a" << 1) << "lang"
                                        << ExpressionFunction::kJavaScript));
    auto expr = ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());

    ASSERT_THROWS_CODE(expr->evaluate({}, getVariables()), AssertionException, 31266);
}

TEST_F(MapReduceFixture, ExpressionFunctionFailsWithInvalidFunction) {
    auto bsonExpr = BSON("expr" << BSON("body"
                                        << "INVALID"
                                        << "args" << BSON_ARRAY(1 << 2) << "lang"
                                        << ExpressionFunction::kJavaScript));
    auto expr = ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());

    ASSERT_THROWS_CODE(
        expr->evaluate({}, getVariables()), AssertionException, ErrorCodes::JSInterpreterFailure);
}

TEST_F(MapReduceFixture, ExpressionFunctionFailsIfArgumentIsNotObject) {
    auto bsonExpr = BSON("expr" << 1);
    ASSERT_THROWS_CODE(ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
                       AssertionException,
                       31260);
}

TEST_F(MapReduceFixture, ExpressionFunctionFailsIfBodyNotSpecified) {
    auto bsonExpr = BSON(
        "expr" << BSON("args" << BSON_ARRAY(1 << 2) << "lang" << ExpressionFunction::kJavaScript));
    ASSERT_THROWS_CODE(ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
                       AssertionException,
                       31261);
}

TEST_F(MapReduceFixture, ExpressionFunctionFailsIfBodyIsNotConstantExpression) {
    auto bsonExpr = BSON("expr" << BSON("body" << BSONObj() << "args" << BSON_ARRAY(1 << 2)
                                               << "lang" << ExpressionFunction::kJavaScript));
    ASSERT_THROWS_CODE(ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
                       AssertionException,
                       31432);
}

TEST_F(MapReduceFixture, ExpressionFunctionFailsIfBodyIsNotCorrectType) {
    auto bsonExpr = BSON("expr" << BSON("body" << 1 << "args" << BSON_ARRAY(1 << 2) << "lang"
                                               << ExpressionFunction::kJavaScript));
    ASSERT_THROWS_CODE(ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
                       AssertionException,
                       31262);
}

TEST_F(MapReduceFixture, ExpressionFunctionFailsIfArgsIsNotSpecified) {
    auto bsonExpr = BSON("expr" << BSON("body"
                                        << "function(first) {return first;};"
                                        << "lang" << ExpressionFunction::kJavaScript));
    ASSERT_THROWS_CODE(ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
                       AssertionException,
                       31263);
}

TEST_F(MapReduceFixture, ExpressionFunctionFailsIfLangIsNotSpecified) {
    auto bsonExpr = BSON("expr" << BSON("body"
                                        << "function(first) {return first;};"
                                        << "args" << BSON_ARRAY(1 << 2)));
    ASSERT_THROWS_CODE(ExpressionFunction::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
                       AssertionException,
                       31418);
}

TEST_F(MapReduceFixture, ExpressionInternalJsEmitProducesExpectedResult) {
    auto bsonExpr = BSON("expr" << BSON("this"
                                        << "$$ROOT"
                                        << "eval"
                                        << "function() {emit(this.a, 1); emit(this.b, 1)};"));

    auto expr = ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());

    Value result = expr->evaluate(Document{BSON("a" << 3 << "b" << 6)}, getVariables());
    ASSERT_VALUE_EQ(result,
                    Value(BSON_ARRAY(BSON("k" << 3 << "v" << 1) << BSON("k" << 6 << "v" << 1))));
}

TEST_F(MapReduceFixture, ExpressionInternalJsEmitFailsIfThisArgumentNotSpecified) {
    auto bsonExpr = BSON("expr" << BSON("eval"
                                        << "function() {emit(this.a, 1); emit(this.b, 1)};"));
    ASSERT_THROWS_CODE(
        ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
        AssertionException,
        31223);
}

TEST_F(MapReduceFixture, ExpressionInternalJsEmitFailsIfThisArgumentIsNotAnObject) {
    auto bsonExpr =
        BSON("expr" << BSON("this" << 123 << "eval"
                                   << "function() {emit(this.a, 1); emit(this.b, 1)};"));
    auto expr = ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());

    ASSERT_THROWS_CODE(expr->evaluate({}, getVariables()), AssertionException, 31225);
}

TEST_F(MapReduceFixture, ExpressionInternalJsEmitFailsWithInvalidFunction) {
    auto bsonExpr = BSON("expr" << BSON("this"
                                        << "$$ROOT"
                                        << "eval"
                                        << "INVALID"));
    auto expr = ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());

    ASSERT_THROWS_CODE(
        expr->evaluate({}, getVariables()), AssertionException, ErrorCodes::JSInterpreterFailure);
}

TEST_F(MapReduceFixture, ExpressionInternalJsEmitFailsWithInvalidNumberOfEvalArguments) {
    auto bsonExpr = BSON("expr" << BSON("this"
                                        << "$$ROOT"
                                        << "eval"
                                        << "function() {emit(this.a);};"));
    auto expr = ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());

    ASSERT_THROWS_CODE(
        expr->evaluate(Document{BSON("a" << 3)}, getVariables()), AssertionException, 31220);
}

TEST_F(MapReduceFixture, ExpressionInternalJsEmitFailsIfArgumentIsNotObject) {
    auto bsonExpr = BSON("expr" << 1);
    ASSERT_THROWS_CODE(
        ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
        AssertionException,
        31221);
}

TEST_F(MapReduceFixture, ExpressionInternalJsEmitFailsIfEvalNotSpecified) {
    auto bsonExpr = BSON("expr" << BSON("this"
                                        << "$$ROOT"));
    ASSERT_THROWS_CODE(
        ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
        AssertionException,
        31222);
}

TEST_F(MapReduceFixture, ExpressionInternalJsEmitFailsIfEvalIsNotCorrectType) {
    auto bsonExpr = BSON("expr" << BSON("this"
                                        << "$$ROOT"
                                        << "eval" << 12.3));
    ASSERT_THROWS_CODE(
        ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS()),
        AssertionException,
        31224);
}

TEST_F(MapReduceFixture, ExpressionInternalJsErrorsIfProducesTooManyDocumentsForNonDefaultValue) {
    internalQueryMaxJsEmitBytes.store(1);
    auto bsonExpr =
        BSON("expr" << BSON("this"
                            << "$$ROOT"
                            << "eval"
                            << "function() {for (var i = 0; i < this.val; ++i) {emit(i, 1);}}"));
    auto expr = ExpressionInternalJsEmit::parse(getExpCtxRaw(), bsonExpr.firstElement(), getVPS());
    ASSERT_THROWS_CODE(
        expr->evaluate(Document{BSON("val" << 1)}, getVariables()), AssertionException, 31292);
}
}  // namespace
}  // namespace mongo
