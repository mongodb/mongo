/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/expression_bm_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

#include <benchmark/benchmark.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class ClassicExpressionBenchmarkFixture : public ExpressionBenchmarkFixture {
    void benchmarkExpression(BSONObj expressionSpec,
                             benchmark::State& state,
                             const std::vector<Document>& documents) final {
        QueryTestServiceContext testServiceContext;
        auto opContext = testServiceContext.makeOperationContext();
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);

        // Build an expression.
        auto expression = Expression::parseExpression(
            exprContext.get(), expressionSpec, exprContext->variablesParseState);

        expression = expression->optimize();

        // Prepare parameters for the 'evaluate()' call.
        auto variables = &(exprContext->variables);

        // Run the test.
        for (auto keepRunning : state) {
            for (const auto& document : documents) {
                benchmark::DoNotOptimize(expression->evaluate(document, variables));
            }
            benchmark::ClobberMemory();
        }
    }

    static const std::string _longHTMLStr;


public:
    void SetUp(benchmark::State& state) final {
        ExpressionBenchmarkFixture::SetUp(state);
        ScriptEngine::setup(ExecutionEnvironment::Server);
    }

    void TearDown(benchmark::State& state) final {
        ScriptEngine::dropScopeCache();
        ExpressionBenchmarkFixture::TearDown(state);
    }

    void benchmarkMQLReplaceOneRegex(benchmark::State& state) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagMqlJsEngineGap",
                                                                   true);
        benchmarkExpression(
            BSON("$replaceOne" << BSON("input" << "$input" << "find" << BSONRegEx("<a.+>")
                                               << "replacement" << "<a>")),
            state,
            std::vector<Document>(1, {{"input"_sd, _longHTMLStr}}));
    }

    void benchmarkJSReplaceOneRegex(benchmark::State& state) {
        benchmarkExpression(
            BSON("$function" << BSON("body"
                                     << "function(input) {return input.replace(/<a.+>/, '<a>');}"
                                     << "args" << BSON_ARRAY("$input") << "lang" << "js")),
            state,
            std::vector<Document>(1, {{"input"_sd, _longHTMLStr}}));
    }

    void benchmarkMQLReplaceAllRegex(benchmark::State& state) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagMqlJsEngineGap",
                                                                   true);
        benchmarkExpression(
            BSON("$replaceAll" << BSON("input" << "$input" << "find" << BSONRegEx("<a.+>")
                                               << "replacement" << "<a>")),
            state,
            std::vector<Document>(1, {{"input"_sd, _longHTMLStr}}));
    }

    void benchmarkJSReplaceAllRegex(benchmark::State& state) {
        benchmarkExpression(
            BSON("$function" << BSON("body"
                                     << "function(input) {return input.replace(/<a.+>/g, '<a>');}"
                                     << "args" << BSON_ARRAY("$input") << "lang" << "js")),
            state,
            std::vector<Document>(1, {{"input"_sd, _longHTMLStr}}));
    }

    void benchmarkMQLSplitRegex(benchmark::State& state) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagMqlJsEngineGap",
                                                                   true);
        benchmarkExpression(BSON("$split" << BSON_ARRAY("$input" << BSONRegEx("<a.+>"))),
                            state,
                            std::vector<Document>(1, {{"input"_sd, _longHTMLStr}}));
    }

    void benchmarkJSSplitRegex(benchmark::State& state) {
        benchmarkExpression(
            BSON("$function" << BSON("body" << "function(input) {return input.split(/<a.+>/);}"
                                            << "args" << BSON_ARRAY("$input") << "lang" << "js")),
            state,
            std::vector<Document>(1, {{"input"_sd, _longHTMLStr}}));
    }
};

const std::string ClassicExpressionBenchmarkFixture::_longHTMLStr =
    "<div class='sidenav'> <a href='#about'>About</a> <a href='#services'>Services</a> <a "
    "href='#clients'>Clients</a> <a href='#contact'>Contact</a> <button "
    "class='dropdown-btn'>Dropdown <i class='fa fa-caret-down'></i> </button> <div "
    "class='dropdown-container'> <a href='#'>Link 1</a> <a href='#'>Link 2</a> <a "
    "href='#'>Link 3</a> </div> <a href='#contact'>Search</a> </div>";

BENCHMARK_EXPRESSIONS(ClassicExpressionBenchmarkFixture)

BENCHMARK_F(ClassicExpressionBenchmarkFixture, MQLReplaceOneRegex)(benchmark::State& state) {
    benchmarkMQLReplaceOneRegex(state);
}

BENCHMARK_F(ClassicExpressionBenchmarkFixture, JSReplaceOneRegex)(benchmark::State& state) {
    benchmarkJSReplaceOneRegex(state);
}

BENCHMARK_F(ClassicExpressionBenchmarkFixture, MQLReplaceAllRegex)(benchmark::State& state) {
    benchmarkMQLReplaceAllRegex(state);
}

BENCHMARK_F(ClassicExpressionBenchmarkFixture, JSReplaceAllRegex)(benchmark::State& state) {
    benchmarkJSReplaceAllRegex(state);
}

BENCHMARK_F(ClassicExpressionBenchmarkFixture, MQLSplitRegex)(benchmark::State& state) {
    benchmarkMQLSplitRegex(state);
}

BENCHMARK_F(ClassicExpressionBenchmarkFixture, JSSplitRegex)(benchmark::State& state) {
    benchmarkJSSplitRegex(state);
}

}  // namespace
}  // namespace mongo
