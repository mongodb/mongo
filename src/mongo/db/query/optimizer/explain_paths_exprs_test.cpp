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
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/golden_test_base.h"

namespace mongo::optimizer {
namespace {
unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/optimizer"};
using GoldenTestContext = unittest::GoldenTestContext;
using GoldenTestConfig = unittest::GoldenTestConfig;
using namespace unit_test_abt_literals;

// This tests the "stringified" paths and expressions in the explain output.
class StringifyPathsExprs : public unittest::Test {
protected:
    void stringify(GoldenTestContext& gctx,
                   const std::string& name,
                   const ABT::reference_type node) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }
        stream << "==== VARIATION: " << name << " ====" << std::endl;
        stream << "-- INPUT:" << std::endl;
        stream << ExplainGenerator::explainV2Compact(node) << std::endl;
        stream << "-- OUTPUT:" << std::endl;
        stream << StringifyPathsAndExprs().stringify(node) << std::endl;
    }
};

TEST_F(StringifyPathsExprs, PathTraverse) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "single level PathTraverse", _traverse1(_keep("a"))._n);
    stringify(ctx, "inf PathTraverse", _traverseN(_keep("a"))._n);
}

TEST_F(StringifyPathsExprs, PathKeepPathDrop) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "keep", _keep("_id", "a")._n);
    stringify(ctx, "drop", _drop("_id", "a")._n);
}

TEST_F(StringifyPathsExprs, PathObjPathArr) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "obj", _obj()._n);
    stringify(ctx, "arr", _arr()._n);
}

TEST_F(StringifyPathsExprs, PathComposeAPathComposeM) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "composeA", _composea(_pconst(_cbool(false)), _pconst(_cbool(true)))._n);
    stringify(ctx, "composeM", _composem(_pconst(_cbool(false)), _pconst(_cbool(true)))._n);
}

TEST_F(StringifyPathsExprs, Variable) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "variable", "varName"_var._n);
}

TEST_F(StringifyPathsExprs, PathCompare) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "compare eq", _cmp("Eq", "0"_cint32)._n);
    stringify(ctx, "compare eqMember", _cmp("EqMember", _carray("0"_cdouble, "1"_cdouble))._n);
    stringify(ctx, "compare neq", _cmp("Neq", "0"_cint32)._n);
    stringify(ctx, "compare gt", _cmp("Gt", "0"_cint32)._n);
    stringify(ctx, "compare gte", _cmp("Gte", "0"_cint32)._n);
    stringify(ctx, "compare lt", _cmp("Lt", "0"_cint32)._n);
    stringify(ctx, "compare lte", _cmp("Lte", "0"_cint32)._n);
    stringify(ctx, "compare cmp3w", _cmp("Cmp3w", "0"_cint32)._n);
}

TEST_F(StringifyPathsExprs, BinaryOpUnaryOp) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "binary op add", _binary("Add", "x"_var, "1"_cint32)._n);
    stringify(ctx, "binary op gt", _binary("Gt", "x"_var, "1"_cint32)._n);

    stringify(ctx, "unary op not", _unary("Not", _cbool(false))._n);
}

TEST_F(StringifyPathsExprs, EvalFilter) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(
        ctx,
        "EvalFilter",
        _evalf(_traverse1(_composem(_cmp("Lt", "5"_cint32), _cmp("Gt", _cNaN()))), "p3"_var)._n);
}

TEST_F(StringifyPathsExprs, EvalPath) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "EvalPath", _evalp(_keep("a", "b"), "p0"_var)._n);
}

TEST_F(StringifyPathsExprs, FunctionCall) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "getParam", _fn(kParameterFunctionName, "0"_cint32, "3"_cint32)._n);
    stringify(ctx, "getField", _fn("getField", "p0"_var, "a"_cstr)._n);
}

TEST_F(StringifyPathsExprs, Constant) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "string constant", "hello"_cstr._n);
    stringify(ctx, "int32 constant", "5"_cint32._n);
    stringify(ctx, "int64 constant", "5"_cint64._n);
    stringify(ctx, "double constant", "5.5"_cdouble._n);
    stringify(ctx, "nan constant", _cNaN()._n);
    stringify(ctx, "timestamp constant", Constant::timestamp(Timestamp::min()));

    // Note that this will print the date as a number. In practice, creating a Date() in a query
    // causes a conversion early on to a string.
    stringify(ctx, "date constant", Constant::date(Date_t::min()));
    stringify(ctx, "empty obj constant", _cempobj()._n);
    stringify(ctx, "empty arr constant", _cemparray()._n);
    stringify(ctx, "nothing constant", _cnothing()._n);
    stringify(ctx, "null constant", _cnull()._n);
    stringify(ctx, "bool constant - true", _cbool(true)._n);
    stringify(ctx, "bool constant - false", _cbool(false)._n);
    stringify(ctx, "MinKey constant", _cminKey()._n);
    stringify(ctx, "MaxKey constant", _cmaxKey()._n);
}

TEST_F(StringifyPathsExprs, PathConstant) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "PathConstant", _pconst("0"_cint32)._n);
}

TEST_F(StringifyPathsExprs, PathIdentityPathDefault) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "PathIdentity", _id()._n);
    stringify(ctx, "PathDefault", _default(_cempobj())._n);
}

TEST_F(StringifyPathsExprs, PathFieldPathGet) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "PathField", _field("a", _pconst(_cempobj()))._n);
    stringify(ctx, "PathGet", _get("a", _arr())._n);
}

TEST_F(StringifyPathsExprs, If) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "isArray", _if(_fn("isArray", "p0"_var), "p0"_var, _cnothing())._n);
}

TEST_F(StringifyPathsExprs, LambdaAbstraction) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(
        ctx, "PathLambda parent", _plambda(_lambda("x", _binary("Add", "x"_var, "1"_cint32)))._n);

    stringify(ctx,
              "LambdaApplication parent",
              _lambdaApp(_lambda("x", _binary("Add", "x"_var, "1"_cint32)), "p0"_var)._n);
}

TEST_F(StringifyPathsExprs, Let) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    stringify(ctx, "let", _let("x", "5"_cint32, _binary("Gt", "x"_var, "10"_cint32))._n);
}
}  // namespace
}  // namespace mongo::optimizer
