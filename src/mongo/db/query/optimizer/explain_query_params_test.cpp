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

#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/golden_test_base.h"

namespace mongo::optimizer {
namespace {
unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/optimizer"};
using GoldenTestContext = unittest::GoldenTestContext;
using GoldenTestConfig = unittest::GoldenTestConfig;
using namespace unit_test_abt_literals;

// This tests the ABTPrinter's getQueryParameters serialization function for explain output.
class SerializeQueryParameter : public unittest::Test {
protected:
    void queryParamTest(GoldenTestContext& gctx, const std::string& name, QueryParameterMap qpMap) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }
        stream << "==== VARIATION: " << name << " ====" << std::endl;
        stream << "-- OUTPUT:" << std::endl;

        // The only argument to the ABTPrinter constructor that is used by getQueryParameters is the
        // QueryParametersMap.
        auto abtPrinter = ABTPrinter({} /* metadata */,
                                     {make<Blackhole>(), {}} /* planAndProps */,
                                     ExplainVersion::UserFacingExplain /* explainVersion */,
                                     std::move(qpMap));
        stream << abtPrinter.getQueryParameters() << std::endl;
    }
};

TEST_F(SerializeQueryParameter, StringParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *"hello"_cstr._n.cast<Constant>());
    queryParamTest(ctx, "string", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, Int32Param) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *"5"_cint32._n.cast<Constant>());
    queryParamTest(ctx, "int32", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, Int64Param) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *"5"_cint64._n.cast<Constant>());
    queryParamTest(ctx, "int64", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, DoubleParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *"5.5"_cdouble._n.cast<Constant>());
    queryParamTest(ctx, "double", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, NanParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cNaN()._n.cast<Constant>());
    queryParamTest(ctx, "nan", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, TimestampParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *Constant::timestamp(Timestamp::min()).cast<Constant>());
    queryParamTest(ctx, "timestamp", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, DateParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *Constant::date(Date_t::min()).cast<Constant>());
    // Note that this will print the date as a number. In practice, creating a Date() in a query
    // causes a conversion early on to a string.
    queryParamTest(ctx, "date", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, EmptyObjParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cempobj()._n.cast<Constant>());
    queryParamTest(ctx, "empty obj", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, EmptyArrParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cemparray()._n.cast<Constant>());
    queryParamTest(ctx, "empty arr", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, NothingParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cnothing()._n.cast<Constant>());
    queryParamTest(ctx, "nothing", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, NullParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cnull()._n.cast<Constant>());
    queryParamTest(ctx, "null", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, BoolTrueParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cbool(true)._n.cast<Constant>());
    queryParamTest(ctx, "bool true", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, BoolFalseParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cbool(false)._n.cast<Constant>());
    queryParamTest(ctx, "bool false", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, MinKeyParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cminKey()._n.cast<Constant>());
    queryParamTest(ctx, "MinKey", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, MaxKeyParam) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    qpMap.emplace(0, *_cmaxKey()._n.cast<Constant>());
    queryParamTest(ctx, "MaxKey", std::move(qpMap));
}

TEST_F(SerializeQueryParameter, MultipleParams) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    QueryParameterMap qpMap;
    // Insert out of order to explicitly test sorting functionality.
    qpMap.emplace(0, *"5"_cint32._n.cast<Constant>());
    qpMap.emplace(1, *"hello"_cstr._n.cast<Constant>());
    qpMap.emplace(3, *_cempobj()._n.cast<Constant>());
    qpMap.emplace(2, *"7.5"_cdouble._n.cast<Constant>());
    queryParamTest(ctx, "multiple params", std::move(qpMap));
}

}  // namespace
}  // namespace mongo::optimizer
