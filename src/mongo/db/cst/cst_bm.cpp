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

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>
#include <boost/intrusive_ptr.hpp>

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_match_translation.h"
#include "mongo/db/cst/parser_gen.hpp"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"

#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"

namespace mongo {
namespace {

std::string getField(int index) {
    const StringData kViableChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"_sd;
    invariant(size_t(index) < kViableChars.size());
    return std::to_string(kViableChars[index]);
}

/**
 * Builds a filter BSON with 'nFields' simple equality predicates.
 */
BSONObj buildSimpleMatch(int nFields) {
    std::vector<std::string> genFields;
    BSONObjBuilder filter;
    filter.append("_id", 1);
    for (auto i = 0; i < nFields; i++) {
        genFields.emplace_back(getField(i));
        filter.append(genFields.back(), i);
    }
    return filter.obj();
}

}  // namespace

void BM_Bison_match_simple(benchmark::State& state) {
    auto filter = buildSimpleMatch(state.range(0));

    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto nss = NamespaceString("test.bm");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx =
        new ExpressionContextForTest(opCtx.get(), nss);

    // This is where recording starts.
    for (auto keepRunning : state) {
        CNode cst;
        BSONLexer lexer{filter, ParserGen::token::START_MATCH};
        ParserGen(lexer, &cst).parse();
        benchmark::DoNotOptimize(cst_match_translation::translateMatchExpression(cst, expCtx));
        benchmark::ClobberMemory();
    }
}

// The baseline benchmark is included here for comparison purposes, there's no interaction between
// this and the CST benchmark.
void BM_baseline_match_simple(benchmark::State& state) {
    auto filter = buildSimpleMatch(state.range(0));

    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto nss = NamespaceString("test.bm");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx =
        new ExpressionContextForTest(opCtx.get(), nss);

    // This is where recording starts.
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(
            MatchExpressionParser::parse(filter,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures));
        benchmark::ClobberMemory();
    }
}

// The argument to the simple filter tests is the number of fields to match on.
BENCHMARK(BM_baseline_match_simple)->Arg(0)->Arg(10);
BENCHMARK(BM_Bison_match_simple)->Arg(0)->Arg(10);

}  // namespace mongo
