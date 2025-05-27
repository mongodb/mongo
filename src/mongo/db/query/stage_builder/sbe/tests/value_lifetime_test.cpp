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

#include "mongo/db/query/stage_builder/sbe/value_lifetime.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/tests/abt_unit_test_utils.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <absl/container/node_hash_map.h>

namespace mongo::stage_builder {
namespace {

using namespace abt;

TEST(ValueLifetimeTest, ProcessTraverseOnGlobal) {
    // Simulate a "$inputVar.a.b" traversal - no makeOwn expected
    auto tree = make<FunctionCall>(
        "traverseP",
        makeSeq(
            make<FunctionCall>(
                "traverseP",
                makeSeq(make<Variable>("inputVar"),
                        make<LambdaAbstraction>(
                            ProjectionName{"y"},
                            make<FunctionCall>("getField",
                                               makeSeq(make<Variable>("y"), Constant::str("a")))),
                        Constant::nothing())),
            make<LambdaAbstraction>(
                ProjectionName{"x"},
                make<FunctionCall>("getField", makeSeq(make<Variable>("x"), Constant::str("b")))),
            Constant::nothing()));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"traverseP\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"traverseP\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"LambdaAbstraction\", \n"
        "                    variable: \"y\", \n"
        "                    input: {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"getField\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"y\"\n"
        "                            }, \n"
        "                            {\n"
        "                                nodeType: \"Const\", \n"
        "                                tag: \"StringSmall\", \n"
        "                                value: \"a\"\n"
        "                            }\n"
        "                        ]\n"
        "                    }\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"Nothing\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"LambdaAbstraction\", \n"
        "            variable: \"x\", \n"
        "            input: {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"getField\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"x\"\n"
        "                    }, \n"
        "                    {\n"
        "                        nodeType: \"Const\", \n"
        "                        tag: \"StringSmall\", \n"
        "                        value: \"b\"\n"
        "                    }\n"
        "                ]\n"
        "            }\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Nothing\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        tree);
}

TEST(ValueLifetimeTest, ProcessTraverseOnLocal) {
    // Simulate a "$inputVar.a.b" traversal - no makeOwn expected
    auto tree = make<FunctionCall>(
        "traverseP",
        makeSeq(
            make<FunctionCall>(
                "traverseP",
                makeSeq(
                    make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
                    make<LambdaAbstraction>(
                        ProjectionName{"y"},
                        make<FunctionCall>("getField",
                                           makeSeq(make<Variable>("y"), Constant::str("a")))),
                    Constant::nothing())),
            make<LambdaAbstraction>(
                ProjectionName{"x"},
                make<FunctionCall>("getField", makeSeq(make<Variable>("x"), Constant::str("b")))),
            Constant::nothing()));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"traverseP\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"traverseP\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"newObj\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Const\", \n"
        "                            tag: \"StringSmall\", \n"
        "                            value: \"a\"\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"Const\", \n"
        "                            tag: \"NumberInt32\", \n"
        "                            value: 9\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"LambdaAbstraction\", \n"
        "                    variable: \"y\", \n"
        "                    input: {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"getField\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"y\"\n"
        "                            }, \n"
        "                            {\n"
        "                                nodeType: \"Const\", \n"
        "                                tag: \"StringSmall\", \n"
        "                                value: \"a\"\n"
        "                            }\n"
        "                        ]\n"
        "                    }\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"Nothing\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"LambdaAbstraction\", \n"
        "            variable: \"x\", \n"
        "            input: {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"getField\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"x\"\n"
        "                    }, \n"
        "                    {\n"
        "                        nodeType: \"Const\", \n"
        "                        tag: \"StringSmall\", \n"
        "                        value: \"b\"\n"
        "                    }\n"
        "                ]\n"
        "            }\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Nothing\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        tree);
}

TEST(ValueLifetimeTest, ProcessTraverseOnLocalVariable) {
    // Simulate a "$inputVar.a.b" traversal - makeOwn is expected
    auto tree = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<Let>(
            ProjectionName{"inputVar2"},
            make<Variable>("inputVar"),
            make<FunctionCall>(
                "traverseP",
                makeSeq(make<FunctionCall>(
                            "traverseP",
                            makeSeq(make<Variable>("inputVar2"),
                                    make<LambdaAbstraction>(
                                        ProjectionName{"y"},
                                        make<FunctionCall>(
                                            "getField",
                                            makeSeq(make<Variable>("y"), Constant::str("a")))),
                                    Constant::nothing())),
                        make<LambdaAbstraction>(
                            ProjectionName{"x"},
                            make<FunctionCall>("getField",
                                               makeSeq(make<Variable>("x"), Constant::str("b")))),
                        Constant::nothing()))));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"Let\", \n"
        "        variable: \"inputVar2\", \n"
        "        bind: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        expression: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"traverseP\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"traverseP\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"inputVar2\"\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"LambdaAbstraction\", \n"
        "                            variable: \"y\", \n"
        "                            input: {\n"
        "                                nodeType: \"FunctionCall\", \n"
        "                                name: \"getField\", \n"
        "                                arguments: [\n"
        "                                    {\n"
        "                                        nodeType: \"Variable\", \n"
        "                                        name: \"y\"\n"
        "                                    }, \n"
        "                                    {\n"
        "                                        nodeType: \"Const\", \n"
        "                                        tag: \"StringSmall\", \n"
        "                                        value: \"a\"\n"
        "                                    }\n"
        "                                ]\n"
        "                            }\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"Const\", \n"
        "                            tag: \"Nothing\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"LambdaAbstraction\", \n"
        "                    variable: \"x\", \n"
        "                    input: {\n"
        "                        nodeType: \"FunctionCall\", \n"
        "                        name: \"getField\", \n"
        "                        arguments: [\n"
        "                            {\n"
        "                                nodeType: \"Variable\", \n"
        "                                name: \"x\"\n"
        "                            }, \n"
        "                            {\n"
        "                                nodeType: \"Const\", \n"
        "                                tag: \"StringSmall\", \n"
        "                                value: \"b\"\n"
        "                            }\n"
        "                        ]\n"
        "                    }\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"Nothing\"\n"
        "                }\n"
        "            ]\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree);
}

TEST(ValueLifetimeTest, ProcessTraverseOnFillEmpty) {
    // Simulate a "($global.a ?: Null).b" traversal - makeOwn is not expected
    auto tree = make<FunctionCall>(
        "traverseF",
        makeSeq(
            make<Let>(
                ProjectionName{"localVar"},
                make<BinaryOp>(Operations::FillEmpty,
                               make<FunctionCall>(
                                   "getField", makeSeq(make<Variable>("s1"), Constant::str("a"))),
                               Constant::null()),
                make<If>(make<FunctionCall>(
                             "typeMatch", makeSeq(make<Variable>("localVar"), Constant::int32(24))),
                         make<Variable>("localVar"),
                         Constant::nothing())),
            make<LambdaAbstraction>(
                ProjectionName{"x"},
                make<FunctionCall>("getField", makeSeq(make<Variable>("x"), Constant::str("b")))),
            Constant::nothing()));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"FunctionCall\", \n"
        "    name: \"traverseF\", \n"
        "    arguments: [\n"
        "        {\n"
        "            nodeType: \"Let\", \n"
        "            variable: \"localVar\", \n"
        "            bind: {\n"
        "                nodeType: \"BinaryOp\", \n"
        "                op: \"FillEmpty\", \n"
        "                left: {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"getField\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"s1\"\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"Const\", \n"
        "                            tag: \"StringSmall\", \n"
        "                            value: \"a\"\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                right: {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"Null\", \n"
        "                    value: null\n"
        "                }\n"
        "            }, \n"
        "            expression: {\n"
        "                nodeType: \"If\", \n"
        "                condition: {\n"
        "                    nodeType: \"FunctionCall\", \n"
        "                    name: \"typeMatch\", \n"
        "                    arguments: [\n"
        "                        {\n"
        "                            nodeType: \"Variable\", \n"
        "                            name: \"localVar\"\n"
        "                        }, \n"
        "                        {\n"
        "                            nodeType: \"Const\", \n"
        "                            tag: \"NumberInt32\", \n"
        "                            value: 24\n"
        "                        }\n"
        "                    ]\n"
        "                }, \n"
        "                then: {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"localVar\"\n"
        "                }, \n"
        "                else: {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"Nothing\"\n"
        "                }\n"
        "            }\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"LambdaAbstraction\", \n"
        "            variable: \"x\", \n"
        "            input: {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"getField\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"x\"\n"
        "                    }, \n"
        "                    {\n"
        "                        nodeType: \"Const\", \n"
        "                        tag: \"StringSmall\", \n"
        "                        value: \"b\"\n"
        "                    }\n"
        "                ]\n"
        "            }\n"
        "        }, \n"
        "        {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Nothing\"\n"
        "        }\n"
        "    ]\n"
        "}\n",
        tree);
}

TEST(ValueLifetimeTest, ProcessIfOnReferences) {
    // Both branches return references - no makeOwn expected
    auto tree =
        make<Let>(ProjectionName{"inputVar"},
                  make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
                  make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                           make<Variable>("inputVar"),
                           make<Variable>("inputVar")));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"If\", \n"
        "        condition: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"exists\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        then: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }, \n"
        "        else: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree);
}

TEST(ValueLifetimeTest, ProcessIfOnMixedReferences) {
    // One branch return reference, the other a constant - makeOwn expected
    auto tree1 =
        make<Let>(ProjectionName{"inputVar"},
                  make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
                  make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                           make<Variable>("inputVar"),
                           Constant::null()));

    ValueLifetime{}.validate(tree1);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"If\", \n"
        "        condition: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"exists\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        then: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        else: {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Null\", \n"
        "            value: null\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree1);

    auto tree2 = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<If>(make<UnaryOp>(Operations::Not,
                               make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar")))),
                 Constant::null(),
                 make<Variable>("inputVar")));

    ValueLifetime{}.validate(tree2);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"If\", \n"
        "        condition: {\n"
        "            nodeType: \"UnaryOp\", \n"
        "            op: \"Not\", \n"
        "            input: {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"exists\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"inputVar\"\n"
        "                    }\n"
        "                ]\n"
        "            }\n"
        "        }, \n"
        "        then: {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Null\", \n"
        "            value: null\n"
        "        }, \n"
        "        else: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree2);
}

TEST(ValueLifetimeTest, ProcessSwitchOnReferences) {
    // Both branches return references - no makeOwn expected
    auto tree = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<Switch>(makeSeq(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                             make<Variable>("inputVar"),
                             make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                             make<Variable>("inputVar"),
                             make<Variable>("inputVar"))));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"Switch\", \n"
        "        condition0: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"exists\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        then0: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }, \n"
        "        condition1: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"exists\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        then1: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }, \n"
        "        else: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree);
}

TEST(ValueLifetimeTest, ProcessSwitchOnMixedReferences) {
    // One branch return reference, the other a constant - makeOwn expected
    auto tree1 = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<Switch>(makeSeq(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                             make<Variable>("inputVar"),
                             make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                             make<Variable>("inputVar"),
                             Constant::null())));

    ValueLifetime{}.validate(tree1);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"Switch\", \n"
        "        condition0: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"exists\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        then0: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        condition1: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"exists\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        then1: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        else: {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Null\", \n"
        "            value: null\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree1);
}

TEST(ValueLifetimeTest, ProcessFillEmtpyOnReferences) {
    // Both branches return references - no makeOwn expected
    auto tree = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<BinaryOp>(
            Operations::FillEmpty, make<Variable>("inputVar"), make<Variable>("inputVar")));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"BinaryOp\", \n"
        "        op: \"FillEmpty\", \n"
        "        left: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }, \n"
        "        right: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"inputVar\"\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree);
}

TEST(ValueLifetimeTest, ProcessFillEmtpyOnMixedReferences) {
    // One branch returns reference, the other a constant - makeOwn expected
    auto tree1 = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<BinaryOp>(Operations::FillEmpty, make<Variable>("inputVar"), Constant::null()));

    ValueLifetime{}.validate(tree1);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"BinaryOp\", \n"
        "        op: \"FillEmpty\", \n"
        "        left: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        right: {\n"
        "            nodeType: \"Const\", \n"
        "            tag: \"Null\", \n"
        "            value: null\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree1);

    auto tree2 = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<BinaryOp>(
            Operations::FillEmpty, make<Variable>("inputVar"), make<Variable>("globalVar")));

    ValueLifetime{}.validate(tree2);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"BinaryOp\", \n"
        "        op: \"FillEmpty\", \n"
        "        left: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        right: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"globalVar\"\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree2);

    auto tree3 = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<BinaryOp>(
            Operations::FillEmpty, make<Variable>("globalVar"), make<Variable>("inputVar")));

    ValueLifetime{}.validate(tree3);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"BinaryOp\", \n"
        "        op: \"FillEmpty\", \n"
        "        left: {\n"
        "            nodeType: \"Variable\", \n"
        "            name: \"globalVar\"\n"
        "        }, \n"
        "        right: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"inputVar\"\n"
        "                }\n"
        "            ]\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree3);
}

TEST(ValueLifetimeTest, ProcessFillTypeOnReferences) {
    // Both branches return references - no makeOwn expected
    auto tree = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<FunctionCall>(
            "fillType",
            makeSeq(make<Variable>("inputVar"), Constant::int32(4), make<Variable>("inputVar"))));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"fillType\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"inputVar\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 4\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"inputVar\"\n"
        "            }\n"
        "        ]\n"
        "    }\n"
        "}\n",
        tree);
}

TEST(ValueLifetimeTest, ProcessFillTypeOnMixedReferences) {
    // One branch returns reference, the other a constant - makeOwn expected
    auto tree1 = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<FunctionCall>(
            "fillType", makeSeq(make<Variable>("inputVar"), Constant::int32(4), Constant::null())));

    ValueLifetime{}.validate(tree1);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"fillType\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"makeOwn\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"inputVar\"\n"
        "                    }\n"
        "                ]\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 4\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"Null\", \n"
        "                value: null\n"
        "            }\n"
        "        ]\n"
        "    }\n"
        "}\n",
        tree1);

    auto tree2 = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<FunctionCall>(
            "fillType",
            makeSeq(make<Variable>("inputVar"), Constant::int32(4), make<Variable>("globalVar"))));

    ValueLifetime{}.validate(tree2);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"fillType\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"makeOwn\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"inputVar\"\n"
        "                    }\n"
        "                ]\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 4\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"globalVar\"\n"
        "            }\n"
        "        ]\n"
        "    }\n"
        "}\n",
        tree2);

    auto tree3 = make<Let>(
        ProjectionName{"inputVar"},
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<FunctionCall>(
            "fillType",
            makeSeq(make<Variable>("globalVar"), Constant::int32(4), make<Variable>("inputVar"))));

    ValueLifetime{}.validate(tree3);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"inputVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"fillType\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Variable\", \n"
        "                name: \"globalVar\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 4\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"FunctionCall\", \n"
        "                name: \"makeOwn\", \n"
        "                arguments: [\n"
        "                    {\n"
        "                        nodeType: \"Variable\", \n"
        "                        name: \"inputVar\"\n"
        "                    }\n"
        "                ]\n"
        "            }\n"
        "        ]\n"
        "    }\n"
        "}\n",
        tree3);
}

TEST(ValueLifetimeTest, ProcessMultiLet) {
    auto tree = make<Let>(
        "letVar",
        make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(9))),
        make<MultiLet>(
            std::vector<std::pair<ProjectionName, ABT>>{
                {ProjectionName{"multiLetVar1"},
                 make<FunctionCall>("newObj", makeSeq(Constant::str("a"), Constant::int32(5)))},
                {ProjectionName{"multiLetVar2"}, make<Variable>("letVar")}},
            make<FunctionCall>("newObj",
                               makeSeq(Constant::str("a"),
                                       make<Variable>("letVar1"),
                                       Constant::str("b"),
                                       make<Variable>("letVar2")))));

    ValueLifetime{}.validate(tree);

    ASSERT_EXPLAIN_BSON_AUTO(
        "{\n"
        "    nodeType: \"Let\", \n"
        "    variable: \"letVar\", \n"
        "    bind: {\n"
        "        nodeType: \"FunctionCall\", \n"
        "        name: \"newObj\", \n"
        "        arguments: [\n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"StringSmall\", \n"
        "                value: \"a\"\n"
        "            }, \n"
        "            {\n"
        "                nodeType: \"Const\", \n"
        "                tag: \"NumberInt32\", \n"
        "                value: 9\n"
        "            }\n"
        "        ]\n"
        "    }, \n"
        "    expression: {\n"
        "        nodeType: \"MultiLet\", \n"
        "        variable0: \"multiLetVar1\", \n"
        "        variable1: \"multiLetVar2\", \n"
        "        bind0: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"newObj\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"StringSmall\", \n"
        "                    value: \"a\"\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"NumberInt32\", \n"
        "                    value: 5\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        bind1: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"makeOwn\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"letVar\"\n"
        "                }\n"
        "            ]\n"
        "        }, \n"
        "        expression: {\n"
        "            nodeType: \"FunctionCall\", \n"
        "            name: \"newObj\", \n"
        "            arguments: [\n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"StringSmall\", \n"
        "                    value: \"a\"\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"letVar1\"\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Const\", \n"
        "                    tag: \"StringSmall\", \n"
        "                    value: \"b\"\n"
        "                }, \n"
        "                {\n"
        "                    nodeType: \"Variable\", \n"
        "                    name: \"letVar2\"\n"
        "                }\n"
        "            ]\n"
        "        }\n"
        "    }\n"
        "}\n",
        tree);
}

}  // namespace
}  // namespace mongo::stage_builder
