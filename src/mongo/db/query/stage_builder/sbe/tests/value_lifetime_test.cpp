// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/value_lifetime.h"

#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/tests/abt_unit_test_utils.h"
#include "mongo/unittest/unittest.h"

#include <string>

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
        "                    variable0: \"y\", \n"
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
        "            variable0: \"x\", \n"
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
        "                    variable0: \"y\", \n"
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
        "            variable0: \"x\", \n"
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
        "                            variable0: \"y\", \n"
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
        "                    variable0: \"x\", \n"
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
        "            variable0: \"x\", \n"
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
