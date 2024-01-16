/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <string>

#include <absl/container/node_hash_map.h>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr.h"
#include "mongo/db/query/sbe_stage_builder_vectorizer.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::stage_builder {
namespace {

using namespace optimizer;

TEST(VectorizerTest, ConvertDateTrunc) {
    auto tree = make<FunctionCall>("dateTrunc",
                                   makeSeq(make<Variable>("timezoneDB"),
                                           make<Variable>("inputVar"),
                                           Constant::str("hour"),
                                           Constant::int32(1),
                                           Constant::str("UTC"),
                                           Constant::str("sunday")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kDateTimeType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(tree, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"valueBlockDateTrunc\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Nothing\"\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"timezoneDB\"\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"StringSmall\", \n"
        "            value: \"hour\"\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"NumberInt32\", \n"
        "            value: 1\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"StringSmall\", \n"
        "            value: \"UTC\"\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"StringSmall\", \n"
        "            value: \"sunday\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertDateDiff) {
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kDateTimeType),
                                    boost::none));
    {
        auto tree = make<FunctionCall>("dateDiff",
                                       makeSeq(make<Variable>("timezoneDB"),
                                               make<Variable>("inputVar"),
                                               Constant::str("2024-01-01T01:00:00"),
                                               Constant::str("hour"),
                                               Constant::str("UTC")));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockDateDiff\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"timezoneDB\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"StringBig\", \n"
            "            value: \"2024-01-01T01:00:00\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"StringSmall\", \n"
            "            value: \"hour\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"StringSmall\", \n"
            "            value: \"UTC\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }
    {
        auto tree = make<FunctionCall>("dateDiff",
                                       makeSeq(make<Variable>("timezoneDB"),
                                               Constant::str("2024-01-01T01:00:00"),
                                               make<Variable>("inputVar"),
                                               Constant::str("hour"),
                                               Constant::str("UTC")));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"UnaryOp\", \n"
            "    op: \"Neg\", \n"
            "    input: {\n"
            "        nodeType: \"FunctionCall\", \n"
            "        name: \"valueBlockDateDiff\", \n"
            "        arguments: [\n"
            "            {\n"
            "                nodeType: \"Const\", \n"
            "                tag: \"Nothing\"\n"
            "            }, \n"
            "            {\n"
            "                nodeType: \"Variable\", \n"
            "                name: \"inputVar\"\n"
            "            }, \n"
            "            {\n"
            "                nodeType: \"Variable\", \n"
            "                name: \"timezoneDB\"\n"
            "            }, \n"
            "            {\n"
            "                nodeType: \"Const\", \n"
            "                tag: \"StringBig\", \n"
            "                value: \"2024-01-01T01:00:00\"\n"
            "            }, \n"
            "            {\n"
            "                nodeType: \"Const\", \n"
            "                tag: \"StringSmall\", \n"
            "                value: \"hour\"\n"
            "            }, \n"
            "            {\n"
            "                nodeType: \"Const\", \n"
            "                tag: \"StringSmall\", \n"
            "                value: \"UTC\"\n"
            "            }\n"
            "        ]\n"
            "    }\n"
            "}\n",
            *processed.expr);
    }
}

TEST(VectorizerTest, ConvertGt) {
    auto tree1 = make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(9));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"valueBlockGtScalar\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"NumberInt32\", \n"
        "            value: 9\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertGtOnCell) {
    auto tree1 = make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(9));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockGtScalar\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 9\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertBooleanAndOnCell) {
    auto tree1 = make<BinaryOp>(
        Operations::And,
        make<BinaryOp>(Operations::Lte, make<Variable>("inputVar"), Constant::int32(59)),
        make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(9)));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"__l1_0\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"cellFoldValues_F\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"valueBlockLteScalar\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"cellBlockGetFlatValuesBlock\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"inputVar\"\n"
        "                            }\n"
        "                        ]\n"
        "                    }, \n"
        "                    {\n"
        "                        nodeType: \"Const\", \n"
        "                        tag: \"NumberInt32\", \n"
        "                        value: 59\n"
        "                    }\n"
        "                ]\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"inputVar\"\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"valueBlockLogicalAnd\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"__l1_0\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"cellFoldValues_F\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"valueBlockGtScalar\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"FunctionCall\", \n"
        "                                name: \"cellBlockGetFlatValuesBlock\", \n"
        "                                arguments: [\n"
        "                                    {\n"
        "                                        nodeType: \"Variable\", \n"
        "                                        name: \"inputVar\"\n"
        "                                    }\n"
        "                                ]\n"
        "                            }, \n"
        "                            {\n"
        "                                nodeType: \"Const\", \n"
        "                                tag: \"NumberInt32\", \n"
        "                                value: 9\n"
        "                            }\n"
        "                        ]\n"
        "                    }, \n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"inputVar\"\n"
        "                    }\n"
        "                ]\n"
        "            }\n"
        "        ]\n"
        "    }\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertBooleanOrOnCell) {
    auto tree1 = make<BinaryOp>(
        Operations::Or,
        make<BinaryOp>(Operations::Lte, make<Variable>("inputVar"), Constant::int32(59)),
        make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(9)));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"__l1_0\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"cellFoldValues_F\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"valueBlockLteScalar\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"cellBlockGetFlatValuesBlock\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"inputVar\"\n"
        "                            }\n"
        "                        ]\n"
        "                    }, \n"
        "                    {\n"
        "                        nodeType: \"Const\", \n"
        "                        tag: \"NumberInt32\", \n"
        "                        value: 59\n"
        "                    }\n"
        "                ]\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"inputVar\"\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"Let\", \n"
        "        variable: \"__l2_0\", \n"
        "        bind: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockLogicalNot\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"valueBlockFillEmpty\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"__l1_0\"\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"Const\", \n"
        "                            tag: \"Boolean\", \n"
        "                            value: false\n"
        "                        }\n"
        "                    ]\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        expression: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockLogicalOr\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"__l1_0\"\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellFoldValues_F\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"FunctionCall\", \n"
        "                            name: \"valueBlockGtScalar\", \n"
        "                            arguments: [\n"
        "                                {\n"
        "                                    nodeType: \"FunctionCall\", \n"
        "                                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                                    arguments: [\n"
        "                                        {\n"
        "                                            nodeType: \"Variable\", \n"
        "                                            name: \"inputVar\"\n"
        "                                        }\n"
        "                                    ]\n"
        "                                }, \n"
        "                                {\n"
        "                                    nodeType: \"Const\", \n"
        "                                    tag: \"NumberInt32\", \n"
        "                                    value: 9\n"
        "                                }\n"
        "                            ]\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }\n"
        "            ]\n"
        "        }\n"
        "    }\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertFilter) {
    auto tmpVar = getABTLocalVariableName(7, 0);
    auto tree1 = make<FunctionCall>(
        "blockTraverseFPlaceholder",
        makeSeq(make<Variable>("inputVar"),
                make<LambdaAbstraction>(
                    tmpVar,
                    make<BinaryOp>(
                        Operations::FillEmpty,
                        make<BinaryOp>(Operations::Gt, make<Variable>(tmpVar), Constant::int32(9)),
                        Constant::boolean(false)))));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"__l7_0\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"cellBlockGetFlatValuesBlock\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"inputVar\"\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"cellFoldValues_F\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"valueBlockFillEmpty\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"valueBlockGtScalar\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"__l7_0\"\n"
        "                            }, \n"
        "                            {\n"
        "                                nodeType: \"Const\", \n"
        "                                tag: \"NumberInt32\", \n"
        "                                value: 9\n"
        "                            }\n"
        "                        ]\n"
        "                    }, \n"
        "                    {\n"
        "                        nodeType: \"Const\", \n"
        "                        tag: \"Boolean\", \n"
        "                        value: false\n"
        "                    }\n"
        "                ]\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"inputVar\"\n"
        "            }\n"
        "        ]\n"
        "    }\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertTypeMatch) {
    auto tree1 =
        make<FunctionCall>("typeMatch", makeSeq(make<Variable>("inputVar"), Constant::int32(1088)));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 1088\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertIsNumber) {
    auto tree1 = make<FunctionCall>("isNumber", makeSeq(make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 851970\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertIsDate) {
    auto tree1 = make<FunctionCall>("isDate", makeSeq(make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 512\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertIsString) {
    auto tree1 = make<FunctionCall>("isString", makeSeq(make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 4\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertIsTimestamp) {
    auto tree1 = make<FunctionCall>("isTimestamp", makeSeq(make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 131072\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertIsArray) {
    auto tree1 = make<FunctionCall>("isArray", makeSeq(make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 16\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertIsObject) {
    auto tree1 = make<FunctionCall>("isObject", makeSeq(make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 8\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertIsNull) {
    auto tree1 = make<FunctionCall>("isNull", makeSeq(make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 1024\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertIsTimezone) {
    auto tree1 = make<FunctionCall>(
        "isTimezone", makeSeq(make<Variable>("timezoneDB"), make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockIsTimezone\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"timezoneDB\"\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertBlockIf) {
    auto tree1 = make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                          make<Variable>("inputVar"),
                          Constant::boolean(false));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"__l1_0\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"cellFoldValues_F\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"valueBlockExists\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"cellBlockGetFlatValuesBlock\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"inputVar\"\n"
        "                            }\n"
        "                        ]\n"
        "                    }\n"
        "                ]\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"inputVar\"\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"valueBlockCombine\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"cellFoldValues_F\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"cellBlockGetFlatValuesBlock\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"inputVar\"\n"
        "                            }\n"
        "                        ]\n"
        "                    }, \n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"inputVar\"\n"
        "                    }\n"
        "                ]\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Let\", \n"
        "                variable: \"__l2_0\", \n"
        "                bind: {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"valueBlockLogicalNot\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"__l1_0\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                expression: {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"valueBlockNewFill\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"If\", \n"
        "                            condition: {\n"
        "                                nodeType: \"FunctionCall\", \n"
        "                                name: \"valueBlockNone\", \n"
        "                                arguments: [\n"
        "                                    {\n"
        "                                        nodeType: \"Variable\", \n"
        "                                        name: \"__l2_0\"\n"
        "                                    }, \n"
        "                                    {\n"
        "                                        nodeType: \"Const\", \n"
        "                                        tag: \"Boolean\", \n"
        "                                        value: true\n"
        "                                    }\n"
        "                                ]\n"
        "                            }, \n"
        "                            then: {\n"
        "                                nodeType: \"Const\", \n"
        "                                tag: \"Nothing\"\n"
        "                            }, \n"
        "                            else: {\n"
        "                                nodeType: \"Const\", \n"
        "                                tag: \"Boolean\", \n"
        "                                value: false\n"
        "                            }\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"FunctionCall\", \n"
        "                            name: \"valueBlockSize\", \n"
        "                            arguments: [\n"
        "                                {\n"
        "                                    nodeType: \"Variable\", \n"
        "                                    name: \"__l2_0\"\n"
        "                                }\n"
        "                            ]\n"
        "                        }\n"
        "                    ]\n"
        "                }\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"__l1_0\"\n"
        "            }\n"
        "        ]\n"
        "    }\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertProjection) {
    auto tree1 = make<BinaryOp>(
        Operations::FillEmpty, make<Variable>("inputVar"), optimizer::Constant::emptyObject());

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    // A projection-style vectorization folds the cell blocks before running the operation.
    auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
        tree1, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"valueBlockFillEmpty\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"cellFoldValues_P\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"cellBlockGetFlatValuesBlock\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Object\", \n"
        "            value: {\n"
        "            }\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

}  // namespace
}  // namespace mongo::stage_builder
