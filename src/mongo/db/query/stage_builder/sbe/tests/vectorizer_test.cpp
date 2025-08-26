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

#include "mongo/db/query/stage_builder/sbe/vectorizer.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/tests/abt_unit_test_utils.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <absl/container/node_hash_map.h>

namespace mongo::stage_builder {
namespace {

using namespace abt;

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
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockSub\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt64\", \n"
            "            value: 0\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"valueBlockDateDiff\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"Nothing\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"inputVar\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"timezoneDB\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"StringBig\", \n"
            "                    value: \"2024-01-01T01:00:00\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"StringSmall\", \n"
            "                    value: \"hour\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"StringSmall\", \n"
            "                    value: \"UTC\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }
}

TEST(VectorizerTest, ConvertGt) {
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    {
        auto tree1 = make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(9));
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

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
    {
        auto tree1 = make<BinaryOp>(Operations::Gt, Constant::int32(9), make<Variable>("inputVar"));
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockLtScalar\", \n"
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
}

TEST(VectorizerTest, ConvertGtOnCell) {
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    {
        auto tree1 = make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(9));
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

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
    {
        auto tree1 = make<BinaryOp>(Operations::Gt, Constant::int32(9), make<Variable>("inputVar"));
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"cellFoldValues_F\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"valueBlockLtScalar\", \n"
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
}

TEST(VectorizerTest, ConvertLte) {
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    {
        auto tree1 =
            make<BinaryOp>(Operations::Lte, make<Variable>("inputVar"), Constant::int32(9));
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockLteScalar\", \n"
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
    {
        auto tree1 =
            make<BinaryOp>(Operations::Lte, Constant::int32(9), make<Variable>("inputVar"));
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockGteScalar\", \n"
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
}

TEST(VectorizerTest, ConvertCmp3w) {
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    {
        auto tree1 =
            make<BinaryOp>(Operations::Cmp3w, make<Variable>("inputVar"), Constant::int32(9));
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockCmp3wScalar\", \n"
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
    {
        auto tree1 =
            make<BinaryOp>(Operations::Cmp3w, Constant::int32(9), make<Variable>("inputVar"));
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockSub\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 0\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"valueBlockCmp3wScalar\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"inputVar\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 9\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }
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

TEST(VectorizerTest, ConvertBooleanNaryAndOnCell) {
    auto tree1 = make<NaryOp>(
        Operations::And,
        makeSeq(make<BinaryOp>(Operations::Lte, make<Variable>("inputVar"), Constant::int32(59)),
                make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(9))));

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

TEST(VectorizerTest, ConvertBooleanNaryOrOnCell) {
    auto tree1 = make<NaryOp>(
        Operations::Or,
        makeSeq(make<BinaryOp>(Operations::Lte, make<Variable>("inputVar"), Constant::int32(59)),
                make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(9))));

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
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto tmpVar = SbVar::makeProjectionName(7, 0);

    {
        auto tree1 = make<FunctionCall>(
            "blockTraverseFPlaceholder",
            makeSeq(make<Variable>("inputVar"),
                    make<LambdaAbstraction>(tmpVar,
                                            make<BinaryOp>(Operations::FillEmpty,
                                                           make<BinaryOp>(Operations::Gt,
                                                                          make<Variable>(tmpVar),
                                                                          Constant::int32(9)),
                                                           Constant::boolean(false)))));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

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

    {
        auto tree1 = make<FunctionCall>(
            "blockTraverseFPlaceholder",
            makeSeq(make<Variable>("inputVar"),
                    make<LambdaAbstraction>(
                        tmpVar,
                        make<BinaryOp>(Operations::FillEmpty,
                                       make<FunctionCall>("coerceToBool",
                                                          makeSeq(make<FunctionCall>(
                                                              "trunc",
                                                              makeSeq(make<Variable>("inputVar"),
                                                                      Constant::int32(2))))),
                                       Constant::boolean(false)))));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree1, bindings, boost::none);

        ASSERT_FALSE(processed.expr.has_value());
    }
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
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    {
        auto tree = make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                             make<Variable>("inputVar"),
                             Constant::boolean(false));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"Let\", \n"
            "    variable: \"__l1_0\", \n"
            "    bind: {\n"
            "        nodeType: \"Const\", \n"
            "        tag: \"Nothing\"\n"
            "    }, \n"
            "    expression: {\n"
            "        nodeType: \"Let\", \n"
            "        variable: \"__l2_0\", \n"
            "        bind: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"cellFoldValues_F\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"valueBlockExists\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }\n"
            "                    ]\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"inputVar\"\n"
            "                }\n"
            "            ]\n"
            "        }, \n"
            "        expression: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"valueBlockCombine\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"cellFoldValues_P\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        {\n"
            "                            nodeType: \"Variable\", \n"
            "                            name: \"inputVar\"\n"
            "                        }\n"
            "                    ]\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Let\", \n"
            "                    variable: \"__l3_0\", \n"
            "                    bind: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockLogicalNot\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Variable\", \n"
            "                                name: \"__l2_0\"\n"
            "                            }\n"
            "                        ]\n"
            "                    }, \n"
            "                    expression: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockNewFill\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Const\", \n"
            "                                tag: \"Boolean\", \n"
            "                                value: false\n"
            "                            }, \n"
            "                            {\n"
            "                                nodeType: \"FunctionCall\", \n"
            "                                name: \"valueBlockSize\", \n"
            "                                arguments: [\n"
            "                                    {\n"
            "                                        nodeType: \"Variable\", \n"
            "                                        name: \"__l3_0\"\n"
            "                                    }\n"
            "                                ]\n"
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"__l2_0\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    }\n"
            "}\n",
            *processed.expr);
    }

    {
        // With incoming bitmask.

        auto tree = make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                             make<Variable>("inputVar"),
                             Constant::boolean(false));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, SbSlot{100});

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"Let\", \n"
            "    variable: \"__l4_0\", \n"
            "    bind: {\n"
            "        nodeType: \"Variable\", \n"
            "        name: \"__s100\"\n"
            "    }, \n"
            "    expression: {\n"
            "        nodeType: \"Let\", \n"
            "        variable: \"__l5_0\", \n"
            "        bind: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"cellFoldValues_F\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"valueBlockExists\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }\n"
            "                    ]\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"inputVar\"\n"
            "                }\n"
            "            ]\n"
            "        }, \n"
            "        expression: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"valueBlockCombine\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"cellFoldValues_P\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        {\n"
            "                            nodeType: \"Variable\", \n"
            "                            name: \"inputVar\"\n"
            "                        }\n"
            "                    ]\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Let\", \n"
            "                    variable: \"__l6_0\", \n"
            "                    bind: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockLogicalNot\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Variable\", \n"
            "                                name: \"__l5_0\"\n"
            "                            }\n"
            "                        ]\n"
            "                    }, \n"
            "                    expression: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockNewFill\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Const\", \n"
            "                                tag: \"Boolean\", \n"
            "                                value: false\n"
            "                            }, \n"
            "                            {\n"
            "                                nodeType: \"FunctionCall\", \n"
            "                                name: \"valueBlockSize\", \n"
            "                                arguments: [\n"
            "                                    {\n"
            "                                        nodeType: \"Variable\", \n"
            "                                        name: \"__l6_0\"\n"
            "                                    }\n"
            "                                ]\n"
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"__l5_0\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    }\n"
            "}\n",
            *processed.expr);
    }

    {
        // Returning a scalar variable instead of a constant.

        auto tree = make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                             make<Variable>("inputVar"),
                             make<Variable>("scalarVar"));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, SbSlot{100});

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"Let\", \n"
            "    variable: \"__l7_0\", \n"
            "    bind: {\n"
            "        nodeType: \"Variable\", \n"
            "        name: \"__s100\"\n"
            "    }, \n"
            "    expression: {\n"
            "        nodeType: \"Let\", \n"
            "        variable: \"__l8_0\", \n"
            "        bind: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"cellFoldValues_F\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"valueBlockExists\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }\n"
            "                    ]\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"inputVar\"\n"
            "                }\n"
            "            ]\n"
            "        }, \n"
            "        expression: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"valueBlockCombine\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"cellFoldValues_P\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        {\n"
            "                            nodeType: \"Variable\", \n"
            "                            name: \"inputVar\"\n"
            "                        }\n"
            "                    ]\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Let\", \n"
            "                    variable: \"__l9_0\", \n"
            "                    bind: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockLogicalNot\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Variable\", \n"
            "                                name: \"__l8_0\"\n"
            "                            }\n"
            "                        ]\n"
            "                    }, \n"
            "                    expression: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockNewFill\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"If\", \n"
            "                                condition: {\n"
            "                                    nodeType: \"FunctionCall\", \n"
            "                                    name: \"valueBlockNone\", \n"
            "                                    arguments: [\n"
            "                                        {\n"
            "                                            nodeType: \"FunctionCall\", \n"
            "                                            name: \"valueBlockLogicalAnd\", \n"
            "                                            arguments: [\n"
            "                                                {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"__l7_0\"\n"
            "                                                }, \n"
            "                                                {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"__l9_0\"\n"
            "                                                }\n"
            "                                            ]\n"
            "                                        }, \n"
            "                                        {\n"
            "                                            nodeType: \"Const\", \n"
            "                                            tag: \"Boolean\", \n"
            "                                            value: true\n"
            "                                        }\n"
            "                                    ]\n"
            "                                }, \n"
            "                                then: {\n"
            "                                    nodeType: \"Const\", \n"
            "                                    tag: \"Nothing\"\n"
            "                                }, \n"
            "                                else: {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"scalarVar\"\n"
            "                                }\n"
            "                            }, \n"
            "                            {\n"
            "                                nodeType: \"FunctionCall\", \n"
            "                                name: \"valueBlockSize\", \n"
            "                                arguments: [\n"
            "                                    {\n"
            "                                        nodeType: \"Variable\", \n"
            "                                        name: \"__l9_0\"\n"
            "                                    }\n"
            "                                ]\n"
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"__l8_0\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    }\n"
            "}\n",
            *processed.expr);
    }
}

TEST(VectorizerTest, ConvertMultipleBlockIf) {
    {
        auto tree = make<If>(
            make<BinaryOp>(Operations::Gt, make<Variable>("inputVar1"), Constant::int32(1000)),
            make<If>(
                make<BinaryOp>(Operations::Gt, make<Variable>("inputVar1"), Constant::int32(2000)),
                make<If>(make<BinaryOp>(
                             Operations::Lt, make<Variable>("inputVar1"), Constant::int32(3000)),
                         make<Variable>("scalarVar3000"),
                         make<BinaryOp>(
                             Operations::Add, make<Variable>("inputVar1"), Constant::int32(4000))),
                make<Variable>("scalarVar2000")),
            make<Variable>("scalarVar1000"));

        sbe::value::FrameIdGenerator generator;
        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "inputVar1"_sd,
            std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                           boost::none));


        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        const std::string expectedPlan(
            "{\n"
            "    nodeType: \"Let\", \n"
            "    variable: \"__l1_0\", \n"
            "    bind: {\n"
            "        nodeType: \"Const\", \n"
            "        tag: \"Nothing\"\n"
            "    }, \n"
            "    expression: {\n"
            "        nodeType: \"Let\", \n"
            "        variable: \"__l2_0\", \n"
            "        bind: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"cellFoldValues_F\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"valueBlockGtScalar\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar1\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        {\n"
            "                            nodeType: \"Const\", \n"
            "                            tag: \"NumberInt32\", \n"
            "                            value: 1000\n"
            "                        }\n"
            "                    ]\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"inputVar1\"\n"
            "                }\n"
            "            ]\n"
            "        }, \n"
            "        expression: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"valueBlockCombine\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Let\", \n"
            "                    variable: \"__l3_0\", \n"
            "                    bind: {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"__l2_0\"\n"
            "                    }, \n"
            "                    expression: {\n"
            "                        nodeType: \"Let\", \n"
            "                        variable: \"__l4_0\", \n"
            "                        bind: {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellFoldValues_F\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"FunctionCall\", \n"
            "                                    name: \"valueBlockGtScalar\", \n"
            "                                    arguments: [\n"
            "                                        {\n"
            "                                            nodeType: \"FunctionCall\", \n"
            "                                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                                            arguments: [\n"
            "                                                {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"inputVar1\"\n"
            "                                                }\n"
            "                                            ]\n"
            "                                        }, \n"
            "                                        {\n"
            "                                            nodeType: \"Const\", \n"
            "                                            tag: \"NumberInt32\", \n"
            "                                            value: 2000\n"
            "                                        }\n"
            "                                    ]\n"
            "                                }, \n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar1\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        expression: {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"valueBlockCombine\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Let\", \n"
            "                                    variable: \"__l5_0\", \n"
            "                                    bind: {\n"
            "                                        nodeType: \"FunctionCall\", \n"
            "                                        name: \"valueBlockLogicalAnd\", \n"
            "                                        arguments: [\n"
            "                                            {\n"
            "                                                nodeType: \"Variable\", \n"
            "                                                name: \"__l2_0\"\n"
            "                                            }, \n"
            "                                            {\n"
            "                                                nodeType: \"Variable\", \n"
            "                                                name: \"__l4_0\"\n"
            "                                            }\n"
            "                                        ]\n"
            "                                    }, \n"
            "                                    expression: {\n"
            "                                        nodeType: \"Let\", \n"
            "                                        variable: \"__l6_0\", \n"
            "                                        bind: {\n"
            "                                            nodeType: \"FunctionCall\", \n"
            "                                            name: \"cellFoldValues_F\", \n"
            "                                            arguments: [\n"
            "                                                {\n"
            "                                                    nodeType: \"FunctionCall\", \n"
            "                                                    name: \"valueBlockLtScalar\", \n"
            "                                                    arguments: [\n"
            "                                                        {\n"
            "                                                            nodeType: "
            "\"FunctionCall\", \n"
            "                                                            name: "
            "\"cellBlockGetFlatValuesBlock\", \n"
            "                                                            arguments: [\n"
            "                                                                {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: "
            "\"inputVar1\"\n"
            "                                                                }\n"
            "                                                            ]\n"
            "                                                        }, \n"
            "                                                        {\n"
            "                                                            nodeType: \"Const\", \n"
            "                                                            tag: \"NumberInt32\", \n"
            "                                                            value: 3000\n"
            "                                                        }\n"
            "                                                    ]\n"
            "                                                }, \n"
            "                                                {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"inputVar1\"\n"
            "                                                }\n"
            "                                            ]\n"
            "                                        }, \n"
            "                                        expression: {\n"
            "                                            nodeType: \"FunctionCall\", \n"
            "                                            name: \"valueBlockCombine\", \n"
            "                                            arguments: [\n"
            "                                                {\n"
            "                                                    nodeType: \"FunctionCall\", \n"
            "                                                    name: \"valueBlockNewFill\", \n"
            "                                                    arguments: [\n"
            "                                                        {\n"
            "                                                            nodeType: \"If\", \n"
            "                                                            condition: {\n"
            "                                                                nodeType: "
            "\"FunctionCall\", \n"
            "                                                                name: "
            "\"valueBlockNone\", \n"
            "                                                                arguments: [\n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"FunctionCall\", \n"
            "                                                                        name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                                        arguments: [\n"
            "                                                                            {\n"
            "                                                                                "
            "nodeType: \"Variable\", \n"
            "                                                                                name: "
            "\"__l5_0\"\n"
            "                                                                            }, \n"
            "                                                                            {\n"
            "                                                                                "
            "nodeType: \"Variable\", \n"
            "                                                                                name: "
            "\"__l6_0\"\n"
            "                                                                            }\n"
            "                                                                        ]\n"
            "                                                                    }, \n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"Const\", \n"
            "                                                                        tag: "
            "\"Boolean\", \n"
            "                                                                        value: true\n"
            "                                                                    }\n"
            "                                                                ]\n"
            "                                                            }, \n"
            "                                                            then: {\n"
            "                                                                nodeType: \"Const\", "
            "\n"
            "                                                                tag: \"Nothing\"\n"
            "                                                            }, \n"
            "                                                            else: {\n"
            "                                                                nodeType: "
            "\"Variable\", \n"
            "                                                                name: "
            "\"scalarVar3000\"\n"
            "                                                            }\n"
            "                                                        }, \n"
            "                                                        {\n"
            "                                                            nodeType: "
            "\"FunctionCall\", \n"
            "                                                            name: \"valueBlockSize\", "
            "\n"
            "                                                            arguments: [\n"
            "                                                                {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: \"__l6_0\"\n"
            "                                                                }\n"
            "                                                            ]\n"
            "                                                        }\n"
            "                                                    ]\n"
            "                                                }, \n"
            "                                                {\n"
            "                                                    nodeType: \"Let\", \n"
            "                                                    variable: \"__l7_0\", \n"
            "                                                    bind: {\n"
            "                                                        nodeType: \"FunctionCall\", \n"
            "                                                        name: "
            "\"valueBlockLogicalNot\", \n"
            "                                                        arguments: [\n"
            "                                                            {\n"
            "                                                                nodeType: "
            "\"Variable\", \n"
            "                                                                name: \"__l6_0\"\n"
            "                                                            }\n"
            "                                                        ]\n"
            "                                                    }, \n"
            "                                                    expression: {\n"
            "                                                        nodeType: \"FunctionCall\", \n"
            "                                                        name: \"valueBlockAdd\", \n"
            "                                                        arguments: [\n"
            "                                                            {\n"
            "                                                                nodeType: "
            "\"FunctionCall\", \n"
            "                                                                name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                                arguments: [\n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"FunctionCall\", \n"
            "                                                                        name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                                        arguments: [\n"
            "                                                                            {\n"
            "                                                                                "
            "nodeType: \"Variable\", \n"
            "                                                                                name: "
            "\"__l2_0\"\n"
            "                                                                            }, \n"
            "                                                                            {\n"
            "                                                                                "
            "nodeType: \"Variable\", \n"
            "                                                                                name: "
            "\"__l4_0\"\n"
            "                                                                            }\n"
            "                                                                        ]\n"
            "                                                                    }, \n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"Variable\", \n"
            "                                                                        name: "
            "\"__l7_0\"\n"
            "                                                                    }\n"
            "                                                                ]\n"
            "                                                            }, \n"
            "                                                            {\n"
            "                                                                nodeType: "
            "\"FunctionCall\", \n"
            "                                                                name: "
            "\"cellFoldValues_P\", \n"
            "                                                                arguments: [\n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"FunctionCall\", \n"
            "                                                                        name: "
            "\"cellBlockGetFlatValuesBlock\", \n"
            "                                                                        arguments: [\n"
            "                                                                            {\n"
            "                                                                                "
            "nodeType: \"Variable\", \n"
            "                                                                                name: "
            "\"inputVar1\"\n"
            "                                                                            }\n"
            "                                                                        ]\n"
            "                                                                    }, \n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"Variable\", \n"
            "                                                                        name: "
            "\"inputVar1\"\n"
            "                                                                    }\n"
            "                                                                ]\n"
            "                                                            }, \n"
            "                                                            {\n"
            "                                                                nodeType: \"Const\", "
            "\n"
            "                                                                tag: \"NumberInt32\", "
            "\n"
            "                                                                value: 4000\n"
            "                                                            }\n"
            "                                                        ]\n"
            "                                                    }\n"
            "                                                }, \n"
            "                                                {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"__l6_0\"\n"
            "                                                }\n"
            "                                            ]\n"
            "                                        }\n"
            "                                    }\n"
            "                                }, \n"
            "                                {\n"
            "                                    nodeType: \"Let\", \n"
            "                                    variable: \"__l8_0\", \n"
            "                                    bind: {\n"
            "                                        nodeType: \"FunctionCall\", \n"
            "                                        name: \"valueBlockLogicalNot\", \n"
            "                                        arguments: [\n"
            "                                            {\n"
            "                                                nodeType: \"Variable\", \n"
            "                                                name: \"__l4_0\"\n"
            "                                            }\n"
            "                                        ]\n"
            "                                    }, \n"
            "                                    expression: {\n"
            "                                        nodeType: \"FunctionCall\", \n"
            "                                        name: \"valueBlockNewFill\", \n"
            "                                        arguments: [\n"
            "                                            {\n"
            "                                                nodeType: \"If\", \n"
            "                                                condition: {\n"
            "                                                    nodeType: \"FunctionCall\", \n"
            "                                                    name: \"valueBlockNone\", \n"
            "                                                    arguments: [\n"
            "                                                        {\n"
            "                                                            nodeType: "
            "\"FunctionCall\", \n"
            "                                                            name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                            arguments: [\n"
            "                                                                {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: \"__l3_0\"\n"
            "                                                                }, \n"
            "                                                                {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: \"__l8_0\"\n"
            "                                                                }\n"
            "                                                            ]\n"
            "                                                        }, \n"
            "                                                        {\n"
            "                                                            nodeType: \"Const\", \n"
            "                                                            tag: \"Boolean\", \n"
            "                                                            value: true\n"
            "                                                        }\n"
            "                                                    ]\n"
            "                                                }, \n"
            "                                                then: {\n"
            "                                                    nodeType: \"Const\", \n"
            "                                                    tag: \"Nothing\"\n"
            "                                                }, \n"
            "                                                else: {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"scalarVar2000\"\n"
            "                                                }\n"
            "                                            }, \n"
            "                                            {\n"
            "                                                nodeType: \"FunctionCall\", \n"
            "                                                name: \"valueBlockSize\", \n"
            "                                                arguments: [\n"
            "                                                    {\n"
            "                                                        nodeType: \"Variable\", \n"
            "                                                        name: \"__l8_0\"\n"
            "                                                    }\n"
            "                                                ]\n"
            "                                            }\n"
            "                                        ]\n"
            "                                    }\n"
            "                                }, \n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"__l4_0\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }\n"
            "                    }\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Let\", \n"
            "                    variable: \"__l9_0\", \n"
            "                    bind: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockLogicalNot\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Variable\", \n"
            "                                name: \"__l2_0\"\n"
            "                            }\n"
            "                        ]\n"
            "                    }, \n"
            "                    expression: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockNewFill\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"If\", \n"
            "                                condition: {\n"
            "                                    nodeType: \"FunctionCall\", \n"
            "                                    name: \"valueBlockNone\", \n"
            "                                    arguments: [\n"
            "                                        {\n"
            "                                            nodeType: \"Variable\", \n"
            "                                            name: \"__l9_0\"\n"
            "                                        }, \n"
            "                                        {\n"
            "                                            nodeType: \"Const\", \n"
            "                                            tag: \"Boolean\", \n"
            "                                            value: true\n"
            "                                        }\n"
            "                                    ]\n"
            "                                }, \n"
            "                                then: {\n"
            "                                    nodeType: \"Const\", \n"
            "                                    tag: \"Nothing\"\n"
            "                                }, \n"
            "                                else: {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"scalarVar1000\"\n"
            "                                }\n"
            "                            }, \n"
            "                            {\n"
            "                                nodeType: \"FunctionCall\", \n"
            "                                name: \"valueBlockSize\", \n"
            "                                arguments: [\n"
            "                                    {\n"
            "                                        nodeType: \"Variable\", \n"
            "                                        name: \"__l9_0\"\n"
            "                                    }\n"
            "                                ]\n"
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"__l2_0\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    }\n"
            "}\n");
        const std::string actualPlan(ExplainGenerator::explainBSONStr(*processed.expr));
        // This should be done like that or Windows will fail with "error C2026: string too big".
        ASSERT_STR_EQ_AUTO(expectedPlan, actualPlan);
    }

    // scalar test
    {
        auto tree = make<If>(
            make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(1000)),
            make<If>(
                make<BinaryOp>(Operations::Gt, make<Variable>("inputVar"), Constant::int32(2000)),
                make<If>(make<BinaryOp>(
                             Operations::Gt, make<Variable>("inputVar"), Constant::int32(3000)),
                         make<If>(make<BinaryOp>(Operations::Lt,
                                                 make<Variable>("scalarVar"),
                                                 Constant::int32(4000)),
                                  make<Variable>("scalarVar4000"),
                                  make<BinaryOp>(Operations::Add,
                                                 make<Variable>("inputVar"),
                                                 Constant::int32(4000))),
                         make<Variable>("scalarVar3000")),
                make<Variable>("scalarVar2000")),
            make<Variable>("scalarVar1000"));

        sbe::value::FrameIdGenerator generator;
        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "inputVar"_sd,
            std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                           boost::none));


        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        const std::string expectedPlan(
            "{\n"
            "    nodeType: \"Let\", \n"
            "    variable: \"__l1_0\", \n"
            "    bind: {\n"
            "        nodeType: \"Const\", \n"
            "        tag: \"Nothing\"\n"
            "    }, \n"
            "    expression: {\n"
            "        nodeType: \"Let\", \n"
            "        variable: \"__l2_0\", \n"
            "        bind: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"cellFoldValues_F\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"valueBlockGtScalar\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        {\n"
            "                            nodeType: \"Const\", \n"
            "                            tag: \"NumberInt32\", \n"
            "                            value: 1000\n"
            "                        }\n"
            "                    ]\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"inputVar\"\n"
            "                }\n"
            "            ]\n"
            "        }, \n"
            "        expression: {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"valueBlockCombine\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Let\", \n"
            "                    variable: \"__l3_0\", \n"
            "                    bind: {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"__l2_0\"\n"
            "                    }, \n"
            "                    expression: {\n"
            "                        nodeType: \"Let\", \n"
            "                        variable: \"__l4_0\", \n"
            "                        bind: {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"cellFoldValues_F\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"FunctionCall\", \n"
            "                                    name: \"valueBlockGtScalar\", \n"
            "                                    arguments: [\n"
            "                                        {\n"
            "                                            nodeType: \"FunctionCall\", \n"
            "                                            name: \"cellBlockGetFlatValuesBlock\", \n"
            "                                            arguments: [\n"
            "                                                {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"inputVar\"\n"
            "                                                }\n"
            "                                            ]\n"
            "                                        }, \n"
            "                                        {\n"
            "                                            nodeType: \"Const\", \n"
            "                                            tag: \"NumberInt32\", \n"
            "                                            value: 2000\n"
            "                                        }\n"
            "                                    ]\n"
            "                                }, \n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"inputVar\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        expression: {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"valueBlockCombine\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Let\", \n"
            "                                    variable: \"__l5_0\", \n"
            "                                    bind: {\n"
            "                                        nodeType: \"FunctionCall\", \n"
            "                                        name: \"valueBlockLogicalAnd\", \n"
            "                                        arguments: [\n"
            "                                            {\n"
            "                                                nodeType: \"Variable\", \n"
            "                                                name: \"__l2_0\"\n"
            "                                            }, \n"
            "                                            {\n"
            "                                                nodeType: \"Variable\", \n"
            "                                                name: \"__l4_0\"\n"
            "                                            }\n"
            "                                        ]\n"
            "                                    }, \n"
            "                                    expression: {\n"
            "                                        nodeType: \"Let\", \n"
            "                                        variable: \"__l6_0\", \n"
            "                                        bind: {\n"
            "                                            nodeType: \"FunctionCall\", \n"
            "                                            name: \"cellFoldValues_F\", \n"
            "                                            arguments: [\n"
            "                                                {\n"
            "                                                    nodeType: \"FunctionCall\", \n"
            "                                                    name: \"valueBlockGtScalar\", \n"
            "                                                    arguments: [\n"
            "                                                        {\n"
            "                                                            nodeType: "
            "\"FunctionCall\", \n"
            "                                                            name: "
            "\"cellBlockGetFlatValuesBlock\", \n"
            "                                                            arguments: [\n"
            "                                                                {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: "
            "\"inputVar\"\n"
            "                                                                }\n"
            "                                                            ]\n"
            "                                                        }, \n"
            "                                                        {\n"
            "                                                            nodeType: \"Const\", \n"
            "                                                            tag: \"NumberInt32\", \n"
            "                                                            value: 3000\n"
            "                                                        }\n"
            "                                                    ]\n"
            "                                                }, \n"
            "                                                {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"inputVar\"\n"
            "                                                }\n"
            "                                            ]\n"
            "                                        }, \n"
            "                                        expression: {\n"
            "                                            nodeType: \"FunctionCall\", \n"
            "                                            name: \"valueBlockCombine\", \n"
            "                                            arguments: [\n"
            "                                                {\n"
            "                                                    nodeType: \"If\", \n"
            "                                                    condition: {\n"
            "                                                        nodeType: \"BinaryOp\", \n"
            "                                                        op: \"Lt\", \n"
            "                                                        left: {\n"
            "                                                            nodeType: \"Variable\", \n"
            "                                                            name: \"scalarVar\"\n"
            "                                                        }, \n"
            "                                                        right: {\n"
            "                                                            nodeType: \"Const\", \n"
            "                                                            tag: \"NumberInt32\", \n"
            "                                                            value: 4000\n"
            "                                                        }\n"
            "                                                    }, \n"
            "                                                    then: {\n"
            "                                                        nodeType: \"FunctionCall\", \n"
            "                                                        name: \"valueBlockNewFill\", "
            "\n"
            "                                                        arguments: [\n"
            "                                                            {\n"
            "                                                                nodeType: \"If\", \n"
            "                                                                condition: {\n"
            "                                                                    nodeType: "
            "\"FunctionCall\", \n"
            "                                                                    name: "
            "\"valueBlockNone\", \n"
            "                                                                    arguments: [\n"
            "                                                                        {\n"
            "                                                                            nodeType: "
            "\"FunctionCall\", \n"
            "                                                                            name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                                            "
            "arguments: [\n"
            "                                                                                {\n"
            "                                                                                    "
            "nodeType: \"FunctionCall\", \n"
            "                                                                                    "
            "name: \"valueBlockLogicalAnd\", \n"
            "                                                                                    "
            "arguments: [\n"
            "                                                                                      "
            "  "
            "{\n"
            "                                                                                      "
            "   "
            "   nodeType: \"Variable\", \n"
            "                                                                                      "
            "   "
            "   name: \"__l2_0\"\n"
            "                                                                                      "
            "  "
            "}, \n"
            "                                                                                      "
            "  "
            "{\n"
            "                                                                                      "
            "   "
            "   nodeType: \"Variable\", \n"
            "                                                                                      "
            "   "
            "   name: \"__l4_0\"\n"
            "                                                                                      "
            "  "
            "}\n"
            "                                                                                    "
            "]\n"
            "                                                                                }, \n"
            "                                                                                {\n"
            "                                                                                    "
            "nodeType: \"Variable\", \n"
            "                                                                                    "
            "name: \"__l6_0\"\n"
            "                                                                                }\n"
            "                                                                            ]\n"
            "                                                                        }, \n"
            "                                                                        {\n"
            "                                                                            nodeType: "
            "\"Const\", \n"
            "                                                                            tag: "
            "\"Boolean\", \n"
            "                                                                            value: "
            "true\n"
            "                                                                        }\n"
            "                                                                    ]\n"
            "                                                                }, \n"
            "                                                                then: {\n"
            "                                                                    nodeType: "
            "\"Const\", "
            "\n"
            "                                                                    tag: \"Nothing\"\n"
            "                                                                }, \n"
            "                                                                else: {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: "
            "\"scalarVar4000\"\n"
            "                                                                }\n"
            "                                                            }, \n"
            "                                                            {\n"
            "                                                                nodeType: "
            "\"FunctionCall\", \n"
            "                                                                name: "
            "\"valueBlockSize\", \n"
            "                                                                arguments: [\n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"Variable\", \n"
            "                                                                        name: "
            "\"__l6_0\"\n"
            "                                                                    }\n"
            "                                                                ]\n"
            "                                                            }\n"
            "                                                        ]\n"
            "                                                    }, \n"
            "                                                    else: {\n"
            "                                                        nodeType: \"FunctionCall\", \n"
            "                                                        name: \"valueBlockAdd\", \n"
            "                                                        arguments: [\n"
            "                                                            {\n"
            "                                                                nodeType: "
            "\"FunctionCall\", \n"
            "                                                                name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                                arguments: [\n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"FunctionCall\", \n"
            "                                                                        name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                                        arguments: [\n"
            "                                                                            {\n"
            "                                                                                "
            "nodeType: \"Variable\", \n"
            "                                                                                name: "
            "\"__l2_0\"\n"
            "                                                                            }, \n"
            "                                                                            {\n"
            "                                                                                "
            "nodeType: \"Variable\", \n"
            "                                                                                name: "
            "\"__l4_0\"\n"
            "                                                                            }\n"
            "                                                                        ]\n"
            "                                                                    }, \n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"Variable\", \n"
            "                                                                        name: "
            "\"__l6_0\"\n"
            "                                                                    }\n"
            "                                                                ]\n"
            "                                                            }, \n"
            "                                                            {\n"
            "                                                                nodeType: "
            "\"FunctionCall\", \n"
            "                                                                name: "
            "\"cellFoldValues_P\", \n"
            "                                                                arguments: [\n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"FunctionCall\", \n"
            "                                                                        name: "
            "\"cellBlockGetFlatValuesBlock\", \n"
            "                                                                        arguments: [\n"
            "                                                                            {\n"
            "                                                                                "
            "nodeType: \"Variable\", \n"
            "                                                                                name: "
            "\"inputVar\"\n"
            "                                                                            }\n"
            "                                                                        ]\n"
            "                                                                    }, \n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"Variable\", \n"
            "                                                                        name: "
            "\"inputVar\"\n"
            "                                                                    }\n"
            "                                                                ]\n"
            "                                                            }, \n"
            "                                                            {\n"
            "                                                                nodeType: \"Const\", "
            "\n"
            "                                                                tag: \"NumberInt32\", "
            "\n"
            "                                                                value: 4000\n"
            "                                                            }\n"
            "                                                        ]\n"
            "                                                    }\n"
            "                                                }, \n"
            "                                                {\n"
            "                                                    nodeType: \"Let\", \n"
            "                                                    variable: \"__l7_0\", \n"
            "                                                    bind: {\n"
            "                                                        nodeType: \"FunctionCall\", \n"
            "                                                        name: "
            "\"valueBlockLogicalNot\", \n"
            "                                                        arguments: [\n"
            "                                                            {\n"
            "                                                                nodeType: "
            "\"Variable\", \n"
            "                                                                name: \"__l6_0\"\n"
            "                                                            }\n"
            "                                                        ]\n"
            "                                                    }, \n"
            "                                                    expression: {\n"
            "                                                        nodeType: \"FunctionCall\", \n"
            "                                                        name: \"valueBlockNewFill\", "
            "\n"
            "                                                        arguments: [\n"
            "                                                            {\n"
            "                                                                nodeType: \"If\", \n"
            "                                                                condition: {\n"
            "                                                                    nodeType: "
            "\"FunctionCall\", \n"
            "                                                                    name: "
            "\"valueBlockNone\", \n"
            "                                                                    arguments: [\n"
            "                                                                        {\n"
            "                                                                            nodeType: "
            "\"FunctionCall\", \n"
            "                                                                            name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                                            "
            "arguments: [\n"
            "                                                                                {\n"
            "                                                                                    "
            "nodeType: \"Variable\", \n"
            "                                                                                    "
            "name: \"__l5_0\"\n"
            "                                                                                }, \n"
            "                                                                                {\n"
            "                                                                                    "
            "nodeType: \"Variable\", \n"
            "                                                                                    "
            "name: \"__l7_0\"\n"
            "                                                                                }\n"
            "                                                                            ]\n"
            "                                                                        }, \n"
            "                                                                        {\n"
            "                                                                            nodeType: "
            "\"Const\", \n"
            "                                                                            tag: "
            "\"Boolean\", \n"
            "                                                                            value: "
            "true\n"
            "                                                                        }\n"
            "                                                                    ]\n"
            "                                                                }, \n"
            "                                                                then: {\n"
            "                                                                    nodeType: "
            "\"Const\", "
            "\n"
            "                                                                    tag: \"Nothing\"\n"
            "                                                                }, \n"
            "                                                                else: {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: "
            "\"scalarVar3000\"\n"
            "                                                                }\n"
            "                                                            }, \n"
            "                                                            {\n"
            "                                                                nodeType: "
            "\"FunctionCall\", \n"
            "                                                                name: "
            "\"valueBlockSize\", \n"
            "                                                                arguments: [\n"
            "                                                                    {\n"
            "                                                                        nodeType: "
            "\"Variable\", \n"
            "                                                                        name: "
            "\"__l7_0\"\n"
            "                                                                    }\n"
            "                                                                ]\n"
            "                                                            }\n"
            "                                                        ]\n"
            "                                                    }\n"
            "                                                }, \n"
            "                                                {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"__l6_0\"\n"
            "                                                }\n"
            "                                            ]\n"
            "                                        }\n"
            "                                    }\n"
            "                                }, \n"
            "                                {\n"
            "                                    nodeType: \"Let\", \n"
            "                                    variable: \"__l8_0\", \n"
            "                                    bind: {\n"
            "                                        nodeType: \"FunctionCall\", \n"
            "                                        name: \"valueBlockLogicalNot\", \n"
            "                                        arguments: [\n"
            "                                            {\n"
            "                                                nodeType: \"Variable\", \n"
            "                                                name: \"__l4_0\"\n"
            "                                            }\n"
            "                                        ]\n"
            "                                    }, \n"
            "                                    expression: {\n"
            "                                        nodeType: \"FunctionCall\", \n"
            "                                        name: \"valueBlockNewFill\", \n"
            "                                        arguments: [\n"
            "                                            {\n"
            "                                                nodeType: \"If\", \n"
            "                                                condition: {\n"
            "                                                    nodeType: \"FunctionCall\", \n"
            "                                                    name: \"valueBlockNone\", \n"
            "                                                    arguments: [\n"
            "                                                        {\n"
            "                                                            nodeType: "
            "\"FunctionCall\", \n"
            "                                                            name: "
            "\"valueBlockLogicalAnd\", \n"
            "                                                            arguments: [\n"
            "                                                                {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: \"__l3_0\"\n"
            "                                                                }, \n"
            "                                                                {\n"
            "                                                                    nodeType: "
            "\"Variable\", \n"
            "                                                                    name: \"__l8_0\"\n"
            "                                                                }\n"
            "                                                            ]\n"
            "                                                        }, \n"
            "                                                        {\n"
            "                                                            nodeType: \"Const\", \n"
            "                                                            tag: \"Boolean\", \n"
            "                                                            value: true\n"
            "                                                        }\n"
            "                                                    ]\n"
            "                                                }, \n"
            "                                                then: {\n"
            "                                                    nodeType: \"Const\", \n"
            "                                                    tag: \"Nothing\"\n"
            "                                                }, \n"
            "                                                else: {\n"
            "                                                    nodeType: \"Variable\", \n"
            "                                                    name: \"scalarVar2000\"\n"
            "                                                }\n"
            "                                            }, \n"
            "                                            {\n"
            "                                                nodeType: \"FunctionCall\", \n"
            "                                                name: \"valueBlockSize\", \n"
            "                                                arguments: [\n"
            "                                                    {\n"
            "                                                        nodeType: \"Variable\", \n"
            "                                                        name: \"__l8_0\"\n"
            "                                                    }\n"
            "                                                ]\n"
            "                                            }\n"
            "                                        ]\n"
            "                                    }\n"
            "                                }, \n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"__l4_0\"\n"
            "                                }\n"
            "                            ]\n"
            "                        }\n"
            "                    }\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Let\", \n"
            "                    variable: \"__l9_0\", \n"
            "                    bind: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockLogicalNot\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Variable\", \n"
            "                                name: \"__l2_0\"\n"
            "                            }\n"
            "                        ]\n"
            "                    }, \n"
            "                    expression: {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockNewFill\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"If\", \n"
            "                                condition: {\n"
            "                                    nodeType: \"FunctionCall\", \n"
            "                                    name: \"valueBlockNone\", \n"
            "                                    arguments: [\n"
            "                                        {\n"
            "                                            nodeType: \"Variable\", \n"
            "                                            name: \"__l9_0\"\n"
            "                                        }, \n"
            "                                        {\n"
            "                                            nodeType: \"Const\", \n"
            "                                            tag: \"Boolean\", \n"
            "                                            value: true\n"
            "                                        }\n"
            "                                    ]\n"
            "                                }, \n"
            "                                then: {\n"
            "                                    nodeType: \"Const\", \n"
            "                                    tag: \"Nothing\"\n"
            "                                }, \n"
            "                                else: {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"scalarVar1000\"\n"
            "                                }\n"
            "                            }, \n"
            "                            {\n"
            "                                nodeType: \"FunctionCall\", \n"
            "                                name: \"valueBlockSize\", \n"
            "                                arguments: [\n"
            "                                    {\n"
            "                                        nodeType: \"Variable\", \n"
            "                                        name: \"__l9_0\"\n"
            "                                    }\n"
            "                                ]\n"
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"__l2_0\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    }\n"
            "}\n");
        const std::string actualPlan(ExplainGenerator::explainBSONStr(*processed.expr));
        // This should be done like that or Windows will fail with "error C2026: string too big".
        ASSERT_STR_EQ_AUTO(expectedPlan, actualPlan);
    }
}

TEST(VectorizerTest, ConvertMixedScalarIf) {
    // Test conversion of "if" operators on a scalar test expression with only one branch yielding a
    // block value.
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    // Convert an "if" where either one of the branches is a failure function
    {
        auto tree =
            make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("scalarVar"))),
                     make<Variable>("inputVar"),
                     make<FunctionCall>("fail",
                                        makeSeq(Constant::int64(9999),
                                                Constant::str("scalarVar must not be Nothing"))));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"cellFoldValues_F\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"If\", \n"
            "            condition: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"exists\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"scalarVar\"\n"
            "                    }\n"
            "                ]\n"
            "            }, \n"
            "            then: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"cellBlockGetFlatValuesBlock\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"inputVar\"\n"
            "                    }\n"
            "                ]\n"
            "            }, \n"
            "            else: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"fail\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Const\", \n"
            "                        tag: \"NumberInt64\", \n"
            "                        value: 9999\n"
            "                    }, \n"
            "                    {\n"
            "                        nodeType: \"Const\", \n"
            "                        tag: \"StringBig\", \n"
            "                        value: \"scalarVar must not be Nothing\"\n"
            "                    }\n"
            "                ]\n"
            "            }\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }
    {
        auto tree = make<If>(
            make<UnaryOp>(Operations::Not,
                          make<FunctionCall>("exists", makeSeq(make<Variable>("scalarVar")))),
            make<FunctionCall>(
                "fail",
                makeSeq(Constant::int64(9999), Constant::str("scalarVar must not be Nothing"))),
            make<Variable>("inputVar"));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"cellFoldValues_F\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"If\", \n"
            "            condition: {\n"
            "                nodeType: \"UnaryOp\", \n"
            "                op: \"Not\", \n"
            "                input: {\n"
            "                    nodeType: \"FunctionCall\", \n"
            "                    name: \"exists\", \n"
            "                    arguments: [\n"
            "                        {\n"
            "                            nodeType: \"Variable\", \n"
            "                            name: \"scalarVar\"\n"
            "                        }\n"
            "                    ]\n"
            "                }\n"
            "            }, \n"
            "            then: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"fail\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Const\", \n"
            "                        tag: \"NumberInt64\", \n"
            "                        value: 9999\n"
            "                    }, \n"
            "                    {\n"
            "                        nodeType: \"Const\", \n"
            "                        tag: \"StringBig\", \n"
            "                        value: \"scalarVar must not be Nothing\"\n"
            "                    }\n"
            "                ]\n"
            "            }, \n"
            "            else: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"cellBlockGetFlatValuesBlock\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"inputVar\"\n"
            "                    }\n"
            "                ]\n"
            "            }\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }

    // Convert an "if" where either one of the branches is a scalar value
    {
        auto tree = make<If>(
            make<BinaryOp>(Operations::Gt, make<Variable>("scalarVar"), Constant::int32(0)),
            make<Variable>("inputVar"),
            make<Variable>("scalarVar"));

        sbe::value::FrameIdGenerator generator;

        // Test the case when we don't have a bitmap available
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"cellFoldValues_F\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"If\", \n"
            "            condition: {\n"
            "                nodeType: \"BinaryOp\", \n"
            "                op: \"Gt\", \n"
            "                left: {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"scalarVar\"\n"
            "                }, \n"
            "                right: {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 0\n"
            "                }\n"
            "            }, \n"
            "            then: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"cellBlockGetFlatValuesBlock\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"inputVar\"\n"
            "                    }\n"
            "                ]\n"
            "            }, \n"
            "            else: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"valueBlockNewFill\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"scalarVar\"\n"
            "                    }, \n"
            "                    {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockSize\", \n"
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
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                ]\n"
            "            }\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);

        // Test the case when we have a bitmap available
        auto processed2 = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, SbSlot{10});

        ASSERT_TRUE(processed2.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"cellFoldValues_F\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"If\", \n"
            "            condition: {\n"
            "                nodeType: \"BinaryOp\", \n"
            "                op: \"Gt\", \n"
            "                left: {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"scalarVar\"\n"
            "                }, \n"
            "                right: {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 0\n"
            "                }\n"
            "            }, \n"
            "            then: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"cellBlockGetFlatValuesBlock\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"inputVar\"\n"
            "                    }\n"
            "                ]\n"
            "            }, \n"
            "            else: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"valueBlockNewFill\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"If\", \n"
            "                        condition: {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"valueBlockNone\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"__s10\"\n"
            "                                }, \n"
            "                                {\n"
            "                                    nodeType: \"Const\", \n"
            "                                    tag: \"Boolean\", \n"
            "                                    value: true\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        then: {\n"
            "                            nodeType: \"Const\", \n"
            "                            tag: \"Nothing\"\n"
            "                        }, \n"
            "                        else: {\n"
            "                            nodeType: \"Variable\", \n"
            "                            name: \"scalarVar\"\n"
            "                        }\n"
            "                    }, \n"
            "                    {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockSize\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Variable\", \n"
            "                                name: \"__s10\"\n"
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                ]\n"
            "            }\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed2.expr);
    }
    {
        auto tree = make<If>(
            make<BinaryOp>(Operations::Lt, make<Variable>("scalarVar"), Constant::int32(0)),
            make<Variable>("scalarVar"),
            make<Variable>("inputVar"));

        sbe::value::FrameIdGenerator generator;

        // Test the case when we don't have a bitmap available
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"cellFoldValues_F\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"If\", \n"
            "            condition: {\n"
            "                nodeType: \"BinaryOp\", \n"
            "                op: \"Lt\", \n"
            "                left: {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"scalarVar\"\n"
            "                }, \n"
            "                right: {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 0\n"
            "                }\n"
            "            }, \n"
            "            then: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"valueBlockNewFill\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"scalarVar\"\n"
            "                    }, \n"
            "                    {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockSize\", \n"
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
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                ]\n"
            "            }, \n"
            "            else: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"cellBlockGetFlatValuesBlock\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"inputVar\"\n"
            "                    }\n"
            "                ]\n"
            "            }\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);

        // Test the case when we have a bitmap available
        auto processed2 = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, SbSlot{10});

        ASSERT_TRUE(processed2.expr.has_value());
        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"cellFoldValues_F\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"If\", \n"
            "            condition: {\n"
            "                nodeType: \"BinaryOp\", \n"
            "                op: \"Lt\", \n"
            "                left: {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"scalarVar\"\n"
            "                }, \n"
            "                right: {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 0\n"
            "                }\n"
            "            }, \n"
            "            then: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"valueBlockNewFill\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"If\", \n"
            "                        condition: {\n"
            "                            nodeType: \"FunctionCall\", \n"
            "                            name: \"valueBlockNone\", \n"
            "                            arguments: [\n"
            "                                {\n"
            "                                    nodeType: \"Variable\", \n"
            "                                    name: \"__s10\"\n"
            "                                }, \n"
            "                                {\n"
            "                                    nodeType: \"Const\", \n"
            "                                    tag: \"Boolean\", \n"
            "                                    value: true\n"
            "                                }\n"
            "                            ]\n"
            "                        }, \n"
            "                        then: {\n"
            "                            nodeType: \"Const\", \n"
            "                            tag: \"Nothing\"\n"
            "                        }, \n"
            "                        else: {\n"
            "                            nodeType: \"Variable\", \n"
            "                            name: \"scalarVar\"\n"
            "                        }\n"
            "                    }, \n"
            "                    {\n"
            "                        nodeType: \"FunctionCall\", \n"
            "                        name: \"valueBlockSize\", \n"
            "                        arguments: [\n"
            "                            {\n"
            "                                nodeType: \"Variable\", \n"
            "                                name: \"__s10\"\n"
            "                            }\n"
            "                        ]\n"
            "                    }\n"
            "                ]\n"
            "            }, \n"
            "            else: {\n"
            "                nodeType: \"FunctionCall\", \n"
            "                name: \"cellBlockGetFlatValuesBlock\", \n"
            "                arguments: [\n"
            "                    {\n"
            "                        nodeType: \"Variable\", \n"
            "                        name: \"inputVar\"\n"
            "                    }\n"
            "                ]\n"
            "            }\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed2.expr);
    }

    // Attempting to convert an "if" where there is no bitmap or information on the underlying cell.
    {
        auto tree = make<If>(
            make<BinaryOp>(Operations::Lt, make<Variable>("scalarVar"), Constant::int32(0)),
            make<Variable>("scalarVar"),
            make<BinaryOp>(
                Operations::And, make<Variable>("inputVar"), make<Variable>("inputVar")));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(
            tree, bindings, boost::none);

        ASSERT_FALSE(processed.expr.has_value());
    }
}

TEST(VectorizerTest, ConvertProjection) {
    auto tree1 = make<BinaryOp>(
        Operations::FillEmpty, make<Variable>("inputVar"), abt::Constant::emptyObject());

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

void assertArithmeticOperationBlockBlock(const std::string& fnStr,
                                         const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"lInputVar\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"rInputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationBlockScalar(const std::string& fnStr,
                                          const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"lInputVar\"\n"
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

void assertArithmeticOperationScalarBlock(const std::string& fnStr,
                                          const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 9\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"rInputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationScalarScalar(const std::string& opStr,
                                           const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"BinaryOp\", \n"
        "    op: \"" +
            opStr +
            "\", \n"
            "    left: {\n"
            "        nodeType: \"Const\", \n"
            "        tag: \"NumberInt32\", \n"
            "        value: 9\n"
            "    }, \n"
            "    right: {\n"
            "        nodeType: \"Const\", \n"
            "        tag: \"NumberInt32\", \n"
            "        value: 20\n"
            "    }\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationBlockBlockBlock(const std::string& fnStr,
                                              const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"var1\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"" +
            fnStr +
            "\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"Nothing\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"var2\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"var3\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationBlockScalarScalar(const std::string& fnStr,
                                                const std::string& opStr,
                                                const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"var\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"BinaryOp\", \n"
            "            op: \"" +
            opStr +
            "\", \n"
            "            left: {\n"
            "                nodeType: \"Const\", \n"
            "                tag: \"NumberInt32\", \n"
            "                value: 9\n"
            "            }, \n"
            "            right: {\n"
            "                nodeType: \"Const\", \n"
            "                tag: \"NumberInt32\", \n"
            "                value: 20\n"
            "            }\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationScalarBlockScalar(const std::string& fnStr,
                                                const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 9\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"" +
            fnStr +
            "\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"Nothing\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"var\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 20\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationScalarScalarBlock(const std::string& fnStr,
                                                const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 9\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"" +
            fnStr +
            "\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"Nothing\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 20\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"var\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationBlockBlockScalar(const std::string& fnStr,
                                               const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"var1\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"" +
            fnStr +
            "\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"Nothing\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"var2\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 9\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationBlockScalarBlock(const std::string& fnStr,
                                               const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"var1\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"" +
            fnStr +
            "\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"Nothing\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"NumberInt32\", \n"
            "                    value: 9\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"var2\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationScalarBlockBlock(const std::string& fnStr,
                                               const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"" +
            fnStr +
            "\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"Nothing\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 9\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"FunctionCall\", \n"
            "            name: \"" +
            fnStr +
            "\", \n"
            "            arguments: [\n"
            "                {\n"
            "                    nodeType: \"Const\", \n"
            "                    tag: \"Nothing\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"var1\"\n"
            "                }, \n"
            "                {\n"
            "                    nodeType: \"Variable\", \n"
            "                    name: \"var2\"\n"
            "                }\n"
            "            ]\n"
            "        }\n"
            "    ]\n"
            "}\n",
        *processed.expr);
}

void assertArithmeticOperationScalarScalarScalar(const std::string& opStr,
                                                 const Vectorizer::Tree& processed) {
    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"BinaryOp\", \n"
        "    op: \"" +
            opStr +
            "\", \n"
            "    left: {\n"
            "        nodeType: \"Const\", \n"
            "        tag: \"NumberInt32\", \n"
            "        value: 9\n"
            "    }, \n"
            "    right: {\n"
            "        nodeType: \"BinaryOp\", \n"
            "        op: \"" +
            opStr +
            "\", \n"
            "        left: {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 20\n"
            "        }, \n"
            "        right: {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 100\n"
            "        }\n"
            "    }\n"
            "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertAdd) {
    std::string fnStr{"valueBlockAdd"};
    std::string opStr{"Add"};
    auto op = Operations::Add;

    {
        auto treeBlocks =
            make<BinaryOp>(op, make<Variable>("lInputVar"), make<Variable>("rInputVar"));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "lInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "rInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlocks, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockBlock(fnStr, processed);
    }

    {
        auto treeBlockScalar = make<BinaryOp>(op, make<Variable>("lInputVar"), Constant::int32(9));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "lInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockScalar(fnStr, processed);
    }

    {
        auto treeScalarBlock = make<BinaryOp>(op, Constant::int32(9), make<Variable>("rInputVar"));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "rInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarBlock(fnStr, processed);
    }

    {
        auto treeScalarScalar = make<BinaryOp>(op, Constant::int32(9), Constant::int32(20));

        Vectorizer::VariableTypes bindings;

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarScalar(opStr, processed);
    }

    {
        auto treeBlocks = make<NaryOp>(
            op, ABTVector{make<Variable>("var1"), make<Variable>("var2"), make<Variable>("var3")});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var1"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var3"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlocks, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockBlockBlock(fnStr, processed);
    }

    {
        auto treeBlockScalarScalar = make<NaryOp>(
            op, ABTVector{make<Variable>("var"), Constant::int32(9), Constant::int32(20)});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockScalarScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockScalarScalar(fnStr, opStr, processed);
    }

    {
        auto treeScalarBlockScalar = make<NaryOp>(
            op, ABTVector{Constant::int32(9), make<Variable>("var"), Constant::int32(20)});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarBlockScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarBlockScalar(fnStr, processed);
    }

    {
        auto treeScalarScalarBlock = make<NaryOp>(
            op, ABTVector{Constant::int32(9), Constant::int32(20), make<Variable>("var")});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarScalarBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarScalarBlock(fnStr, processed);
    }

    {
        auto treeScalarScalarScalar = make<NaryOp>(
            op, ABTVector{Constant::int32(9), Constant::int32(20), Constant::int32(100)});

        Vectorizer::VariableTypes bindings;

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarScalarScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarScalarScalar(opStr, processed);
    }

    {
        auto treeBlockBlockScalar = make<NaryOp>(
            op, ABTVector{make<Variable>("var1"), make<Variable>("var2"), Constant::int32(9)});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockBlockScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockBlockScalar(fnStr, processed);
    }

    {
        auto treeScalarBlockBlock = make<NaryOp>(
            op, ABTVector{Constant::int32(9), make<Variable>("var1"), make<Variable>("var2")});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarBlockBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarBlockBlock(fnStr, processed);
    }

    {
        auto treeBlockScalarBlock = make<NaryOp>(
            op, ABTVector{make<Variable>("var1"), Constant::int32(9), make<Variable>("var2")});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockScalarBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockScalarBlock(fnStr, processed);
    }
}

TEST(VectorizerTest, ConvertSub) {
    std::string fnStr{"valueBlockSub"};
    std::string opStr{"Sub"};
    auto op = Operations::Sub;

    {
        auto treeBlocks =
            make<BinaryOp>(op, make<Variable>("lInputVar"), make<Variable>("rInputVar"));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "lInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "rInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlocks, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockBlock(fnStr, processed);
    }

    {
        auto treeBlockScalar = make<BinaryOp>(op, make<Variable>("lInputVar"), Constant::int32(9));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "lInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockScalar(fnStr, processed);
    }

    {
        auto treeScalarBlock = make<BinaryOp>(op, Constant::int32(9), make<Variable>("rInputVar"));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "rInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarBlock(fnStr, processed);
    }

    {
        auto treeScalarScalar = make<BinaryOp>(op, Constant::int32(9), Constant::int32(20));

        Vectorizer::VariableTypes bindings;

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarScalar(opStr, processed);
    }
}

TEST(VectorizerTest, ConvertMult) {
    std::string fnStr{"valueBlockMult"};
    std::string opStr{"Mult"};
    auto op = Operations::Mult;

    {
        auto treeBlocks =
            make<BinaryOp>(op, make<Variable>("lInputVar"), make<Variable>("rInputVar"));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "lInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "rInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlocks, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockBlock(fnStr, processed);
    }

    {
        auto treeBlockScalar = make<BinaryOp>(op, make<Variable>("lInputVar"), Constant::int32(9));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "lInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockScalar(fnStr, processed);
    }

    {
        auto treeScalarBlock = make<BinaryOp>(op, Constant::int32(9), make<Variable>("rInputVar"));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "rInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarBlock(fnStr, processed);
    }

    {
        auto treeScalarScalar = make<BinaryOp>(op, Constant::int32(9), Constant::int32(20));

        Vectorizer::VariableTypes bindings;

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarScalar(opStr, processed);
    }

    {
        auto treeBlocks = make<NaryOp>(
            op, ABTVector{make<Variable>("var1"), make<Variable>("var2"), make<Variable>("var3")});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var1"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var3"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlocks, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockBlockBlock(fnStr, processed);
    }

    {
        auto treeBlockScalarScalar = make<NaryOp>(
            op, ABTVector{make<Variable>("var"), Constant::int32(9), Constant::int32(20)});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockScalarScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockScalarScalar(fnStr, opStr, processed);
    }

    {
        auto treeScalarBlockScalar = make<NaryOp>(
            op, ABTVector{Constant::int32(9), make<Variable>("var"), Constant::int32(20)});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarBlockScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarBlockScalar(fnStr, processed);
    }

    {
        auto treeScalarScalarBlock = make<NaryOp>(
            op, ABTVector{Constant::int32(9), Constant::int32(20), make<Variable>("var")});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarScalarBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarScalarBlock(fnStr, processed);
    }

    {
        auto treeScalarScalarScalar = make<NaryOp>(
            op, ABTVector{Constant::int32(9), Constant::int32(20), Constant::int32(100)});

        Vectorizer::VariableTypes bindings;

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarScalarScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarScalarScalar(opStr, processed);
    }

    {
        auto treeBlockBlockScalar = make<NaryOp>(
            op, ABTVector{make<Variable>("var1"), make<Variable>("var2"), Constant::int32(9)});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockBlockScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockBlockScalar(fnStr, processed);
    }

    {
        auto treeScalarBlockBlock = make<NaryOp>(
            op, ABTVector{Constant::int32(9), make<Variable>("var1"), make<Variable>("var2")});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarBlockBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarBlockBlock(fnStr, processed);
    }

    {
        auto treeBlockScalarBlock = make<NaryOp>(
            op, ABTVector{make<Variable>("var1"), Constant::int32(9), make<Variable>("var2")});

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "var"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "var2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockScalarBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockScalarBlock(fnStr, processed);
    }
}

TEST(VectorizerTest, ConvertDiv) {
    std::string fnStr{"valueBlockDiv"};
    std::string opStr{"Div"};
    auto op = Operations::Div;

    {
        auto treeBlocks =
            make<BinaryOp>(op, make<Variable>("lInputVar"), make<Variable>("rInputVar"));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "lInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        bindings.emplace(
            "rInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlocks, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockBlock(fnStr, processed);
    }

    {
        auto treeBlockScalar = make<BinaryOp>(op, make<Variable>("lInputVar"), Constant::int32(9));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "lInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeBlockScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationBlockScalar(fnStr, processed);
    }

    {
        auto treeScalarBlock = make<BinaryOp>(op, Constant::int32(9), make<Variable>("rInputVar"));

        Vectorizer::VariableTypes bindings;
        bindings.emplace(
            "rInputVar"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarBlock, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarBlock(fnStr, processed);
    }

    {
        auto treeScalarScalar = make<BinaryOp>(op, Constant::int32(9), Constant::int32(20));

        Vectorizer::VariableTypes bindings;

        sbe::value::FrameIdGenerator generator;
        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            treeScalarScalar, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());
        assertArithmeticOperationScalarScalar(opStr, processed);
    }
}

TEST(VectorizerTest, ConvertEqMemberOnCell) {
    auto tree = make<BinaryOp>(
        Operations::EqMember,
        make<Variable>("inputVar"),
        Constant::array(sbe::value::makeIntOrLong(9), sbe::value::makeIntOrLong(16)));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace("inputVar"_sd,
                     std::make_pair(TypeSignature::kCellType.include(TypeSignature::kAnyScalarType),
                                    boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Filter}.vectorize(tree, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"cellFoldValues_F\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockIsMember\", \n"
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
        "                    tag: \"Array\", \n"
        "                    value: [\n"
        "                        9, \n"
        "                        16\n"
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

TEST(VectorizerTest, ConvertIsMemberFunction) {
    auto tree = make<FunctionCall>("isMember",
                                   makeSeq(make<Variable>("inputVar"), make<Variable>("inList")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(tree, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"valueBlockIsMember\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inList\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertCoerceToBoolFunction) {
    auto tree = make<FunctionCall>("coerceToBool", makeSeq(make<Variable>("inputVar")));

    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(tree, bindings, boost::none);

    ASSERT_FALSE(processed.expr.has_value());
}

TEST(VectorizerTest, ConvertRound) {
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    {
        auto tree =
            make<FunctionCall>("round", makeSeq(make<Variable>("inputVar"), Constant::int32(2)));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());

        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockRound\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 2\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }

    {
        auto tree = make<FunctionCall>("round", makeSeq(make<Variable>("inputVar")));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());

        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockRound\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }
}

TEST(VectorizerTest, ConvertTrunc) {
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    {
        auto tree =
            make<FunctionCall>("trunc", makeSeq(make<Variable>("inputVar"), Constant::int32(2)));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());

        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockTrunc\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 2\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }

    {
        auto tree = make<FunctionCall>("trunc", makeSeq(make<Variable>("inputVar")));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());

        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockTrunc\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }
}

TEST(VectorizerTest, ConvertMod) {
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    {
        auto tree =
            make<FunctionCall>("mod", makeSeq(make<Variable>("inputVar"), Constant::int32(10)));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());

        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockMod\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 10\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }

    {
        bindings.emplace(
            "inputVar2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        auto tree = make<FunctionCall>(
            "mod", makeSeq(make<Variable>("inputVar"), make<Variable>("inputVar2")));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_FALSE(processed.expr);
    }

    {
        auto tree =
            make<FunctionCall>("mod", makeSeq(Constant::int32(10), make<Variable>("inputVar")));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_FALSE(processed.expr.has_value());
    }
}

TEST(VectorizerTest, ConvertNumConvert) {
    sbe::value::FrameIdGenerator generator;
    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "inputVar"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    {
        auto tree =
            make<FunctionCall>("convert", makeSeq(make<Variable>("inputVar"), Constant::int32(2)));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_TRUE(processed.expr.has_value());

        ASSERT_EXPLAIN_BSON_AUTO(
            "{\n"
            "    nodeType: \"FunctionCall\", \n"
            "    name: \"valueBlockConvert\", \n"
            "    arguments: [\n"
            "        {\n"
            "            nodeType: \"Variable\", \n"
            "            name: \"inputVar\"\n"
            "        }, \n"
            "        {\n"
            "            nodeType: \"Const\", \n"
            "            tag: \"NumberInt32\", \n"
            "            value: 2\n"
            "        }\n"
            "    ]\n"
            "}\n",
            *processed.expr);
    }

    {
        bindings.emplace(
            "inputVar2"_sd,
            std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                           boost::none));
        auto tree = make<FunctionCall>(
            "convert", makeSeq(make<Variable>("inputVar"), make<Variable>("inputVar2")));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_FALSE(processed.expr);
    }

    {
        auto tree =
            make<FunctionCall>("convert", makeSeq(Constant::int32(2), make<Variable>("inputVar")));

        auto processed = Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(
            tree, bindings, boost::none);

        ASSERT_FALSE(processed.expr.has_value());
    }
}

TEST(VectorizerTest, ConvertDateAdd) {
    auto tree = make<FunctionCall>("dateAdd",
                                   makeSeq(make<Variable>("timezoneDB"),
                                           make<Variable>("inputVar"),
                                           Constant::str("hour"),
                                           Constant::int64(5),
                                           Constant::str("UTC")));

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
        "    name: \"valueBlockDateAdd\", \n"
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
        "            tag: \"NumberInt64\", \n"
        "            value: 5\n"
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

TEST(VectorizerTest, ConvertSwitch) {
    auto tree = make<Switch>(
        ABTVector{make<FunctionCall>("isNumber", makeSeq(make<Variable>("inputVar"))),
                  make<BinaryOp>(Operations::Mult, Constant::int32(9), make<Variable>("inputVar")),
                  make<FunctionCall>("isDate", makeSeq(make<Variable>("inputVar"))),
                  make<FunctionCall>("dateAdd",
                                     makeSeq(make<Variable>("timezoneVar"),
                                             make<Variable>("inputVar"),
                                             Constant::str("hour"_sd),
                                             Constant::int32(8),
                                             Constant::str("UTC"_sd))),
                  Constant::null()});

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
        "    nodeType: \"Let\", \n"
        "    variable: \"__l1_0\", \n"
        "    bind: {\n"
        "        nodeType: \"Const\", \n"
        "        tag: \"Nothing\"\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"Let\", \n"
        "        variable: \"__l2_0\", \n"
        "        bind: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockTypeMatch\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 851970\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        expression: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"valueBlockCombine\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"valueBlockMult\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"__l2_0\"\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"Const\", \n"
        "                            tag: \"NumberInt32\", \n"
        "                            value: 9\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Let\", \n"
        "                    variable: \"__l3_0\", \n"
        "                    bind: {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"valueBlockLogicalNot\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"__l2_0\"\n"
        "                            }\n"
        "                        ]\n"
        "                    }, \n"
        "                    expression: {\n"
        "                        nodeType: \"Let\", \n"
        "                        variable: \"__l4_0\", \n"
        "                        bind: {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"__l3_0\"\n"
        "                        }, \n"
        "                        expression: {\n"
        "                            nodeType: \"Let\", \n"
        "                            variable: \"__l5_0\", \n"
        "                            bind: {\n"
        "                                nodeType: \"FunctionCall\", \n"
        "                                name: \"valueBlockTypeMatch\", \n"
        "                                arguments: [\n"
        "                                    {\n"
        "                                        nodeType: \"Variable\", \n"
        "                                        name: \"inputVar\"\n"
        "                                    }, \n"
        "                                    {\n"
        "                                        nodeType: \"Const\", \n"
        "                                        tag: \"NumberInt32\", \n"
        "                                        value: 512\n"
        "                                    }\n"
        "                                ]\n"
        "                            }, \n"
        "                            expression: {\n"
        "                                nodeType: \"FunctionCall\", \n"
        "                                name: \"valueBlockCombine\", \n"
        "                                arguments: [\n"
        "                                    {\n"
        "                                        nodeType: \"FunctionCall\", \n"
        "                                        name: \"valueBlockDateAdd\", \n"
        "                                        arguments: [\n"
        "                                            {\n"
        "                                                nodeType: \"FunctionCall\", \n"
        "                                                name: \"valueBlockLogicalAnd\", \n"
        "                                                arguments: [\n"
        "                                                    {\n"
        "                                                        nodeType: \"Variable\", \n"
        "                                                        name: \"__l3_0\"\n"
        "                                                    }, \n"
        "                                                    {\n"
        "                                                        nodeType: \"Variable\", \n"
        "                                                        name: \"__l5_0\"\n"
        "                                                    }\n"
        "                                                ]\n"
        "                                            }, \n"
        "                                            {\n"
        "                                                nodeType: \"Variable\", \n"
        "                                                name: \"inputVar\"\n"
        "                                            }, \n"
        "                                            {\n"
        "                                                nodeType: \"Variable\", \n"
        "                                                name: \"timezoneVar\"\n"
        "                                            }, \n"
        "                                            {\n"
        "                                                nodeType: \"Const\", \n"
        "                                                tag: \"StringSmall\", \n"
        "                                                value: \"hour\"\n"
        "                                            }, \n"
        "                                            {\n"
        "                                                nodeType: \"Const\", \n"
        "                                                tag: \"NumberInt32\", \n"
        "                                                value: 8\n"
        "                                            }, \n"
        "                                            {\n"
        "                                                nodeType: \"Const\", \n"
        "                                                tag: \"StringSmall\", \n"
        "                                                value: \"UTC\"\n"
        "                                            }\n"
        "                                        ]\n"
        "                                    }, \n"
        "                                    {\n"
        "                                        nodeType: \"Let\", \n"
        "                                        variable: \"__l6_0\", \n"
        "                                        bind: {\n"
        "                                            nodeType: \"FunctionCall\", \n"
        "                                            name: \"valueBlockLogicalNot\", \n"
        "                                            arguments: [\n"
        "                                                {\n"
        "                                                    nodeType: \"Variable\", \n"
        "                                                    name: \"__l5_0\"\n"
        "                                                }\n"
        "                                            ]\n"
        "                                        }, \n"
        "                                        expression: {\n"
        "                                            nodeType: \"FunctionCall\", \n"
        "                                            name: \"valueBlockNewFill\", \n"
        "                                            arguments: [\n"
        "                                                {\n"
        "                                                    nodeType: \"Const\", \n"
        "                                                    tag: \"Null\", \n"
        "                                                    value: null\n"
        "                                                }, \n"
        "                                                {\n"
        "                                                    nodeType: \"FunctionCall\", \n"
        "                                                    name: \"valueBlockSize\", \n"
        "                                                    arguments: [\n"
        "                                                        {\n"
        "                                                            nodeType: \"Variable\", \n"
        "                                                            name: \"__l6_0\"\n"
        "                                                        }\n"
        "                                                    ]\n"
        "                                                }\n"
        "                                            ]\n"
        "                                        }\n"
        "                                    }, \n"
        "                                    {\n"
        "                                        nodeType: \"Variable\", \n"
        "                                        name: \"__l5_0\"\n"
        "                                    }\n"
        "                                ]\n"
        "                            }\n"
        "                        }\n"
        "                    }\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"__l2_0\"\n"
        "                }\n"
        "            ]\n"
        "        }\n"
        "    }\n"
        "}\n",
        *processed.expr);
}

TEST(VectorizerTest, ConvertMultiLet) {
    // TODO SERVER-100579 Remove this when feature flag is removed
    RAIIServerParameterControllerForTest sbeUpgradeBinaryTreesFeatureFlag{
        "featureFlagSbeUpgradeBinaryTrees", true};

    auto tree = make<MultiLet>(
        std::vector<std::pair<ProjectionName, ABT>>{
            {"var1", make<BinaryOp>(Operations::Add, make<Variable>("s1"), make<Variable>("s2"))},
            {"var2", make<BinaryOp>(Operations::Add, make<Variable>("s3"), make<Variable>("s4"))}},
        make<BinaryOp>(Operations::Add, make<Variable>("var1"), make<Variable>("var2")));

    Vectorizer::VariableTypes bindings;
    bindings.emplace(
        "s1"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));
    bindings.emplace(
        "s2"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    bindings.emplace(
        "s3"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));
    bindings.emplace(
        "s4"_sd,
        std::make_pair(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType),
                       boost::none));

    sbe::value::FrameIdGenerator generator;
    auto processed =
        Vectorizer{&generator, Vectorizer::Purpose::Project}.vectorize(tree, bindings, boost::none);

    ASSERT_TRUE(processed.expr.has_value());
    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"MultiLet\", \n"
        "    variable0: \"var1\", \n"
        "    variable1: \"var2\", \n"
        "    bind0: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"valueBlockAdd\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"Nothing\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"s1\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"s2\"\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    bind1: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"valueBlockAdd\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"Nothing\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"s3\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"s4\"\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"valueBlockAdd\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"Nothing\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"var1\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"var2\"\n"
        "            }\n"
        "        ]\n"
        "    }\n"
        "}\n",
        *processed.expr);
}

}  // namespace
}  // namespace mongo::stage_builder
