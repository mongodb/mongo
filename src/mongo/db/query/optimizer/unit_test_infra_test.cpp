/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"


namespace mongo::optimizer {
namespace {
using namespace unit_test_abt_literals;

TEST(TestInfra, AutoUpdateExplain) {
    ABT tree = make<BinaryOp>(Operations::Add,
                              Constant::int64(1),
                              make<Variable>("very very very very very very very very very very "
                                             "very very long variable name with \"quotes\""));

    /**
     * To exercise the auto-updating behavior:
     *   1. Induce a failure: change something in the expected output.
     *   2. Run the test binary with the flag "--autoUpdateOptimizerAsserts".
     *   3. Observe afterwards that the test file is updated with the correct output.
     */
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT (test auto-update)
        "BinaryOp [Add]\n"
        "|   Variable [very very very very very very very very very very very very long variable "
        "name with \"quotes\"]\n"
        "Const [1]\n",
        tree);

    // Test for short constant. It should not be inlined. The nolint comment on the string constant
    // itself is auto-generated.
    ABT tree1 = make<Variable>("short name");
    ASSERT_EXPLAIN_V2_AUTO(         // NOLINT (test auto-update)
        "Variable [short name]\n",  // NOLINT (test auto-update)
        tree1);


    // Exercise auto-updating behavior for numbers.
    double number = 0.5;
    ASSERT_NUMBER_EQ_AUTO(  // NOLINT (test auto-update)
        0.5,                // NOLINT (test auto-update)
        number);

    // Exercise range auto-updating behavior. If the actual is outside the expected range, the range
    // is adjusted to +-25%.
    size_t plansExplored = 95;
    ASSERT_BETWEEN_AUTO(  // NOLINT (test auto-update)
        71,
        118,
        plansExplored);
}

TEST(TestInfra, DiffTest) {
    const std::vector<std::string> expected{"line 1\n",
                                            "line 2\n",
                                            "line 3\n",
                                            "line 5\n",
                                            "line 7\n",
                                            "line 8\n",
                                            "line 10\n",
                                            "line 11\n"};
    const std::vector<std::string> actual{"line 2\n",
                                          "line 4\n",
                                          "line 5\n",
                                          "line 6\n",
                                          "line 8\n",
                                          "line 10\n",
                                          "line 11\n",
                                          "line 12\n",
                                          "line 13\n"};

    std::string diffStr;

    {
        std::ostringstream os;
        outputDiff(os, expected, actual, 0);
        diffStr = os.str();
    }
    ASSERT_STR_EQ_AUTO(  // NOLINT (test auto-update)
        "L0: -line 1\n"
        "L1: +line 4\n"
        "L2: -line 3\n"
        "L3: +line 6\n"
        "L4: -line 7\n"
        "L7: +line 12\n"
        "L8: +line 13\n",
        diffStr);

    {
        std::ostringstream os;
        outputDiff(os, {}, {"line1\n", "line2\n", "line3\n"}, 10);
        diffStr = os.str();
    }
    ASSERT_STR_EQ_AUTO(  // NOLINT (test auto-update)
        "L10: +line1\n"
        "L11: +line2\n"
        "L12: +line3\n",
        diffStr);

    {
        std::ostringstream os;
        outputDiff(os, {"line1\n", "line2\n", "line3\n"}, {}, 7);
        diffStr = os.str();
    }
    ASSERT_STR_EQ_AUTO(  // NOLINT (test auto-update)
        "L7: -line1\n"
        "L8: -line2\n"
        "L9: -line3\n",
        diffStr);

    {
        std::ostringstream os;
        outputDiff(os, {}, {}, 10);
        diffStr = os.str();
    }
    ASSERT(diffStr.empty());
}

TEST(TestInfra, ABTLiterals) {
    // Demonstrate shorthand tree initialization using the ABT string literal constructors.

    // Construct inline.
    auto scanNode = _scan("root", "c1");
    auto projectionANode = _eval("pa", _evalp(_get("a", _id()), "root"_var), std::move(scanNode));
    auto filterANode =
        _filter(_evalf(_cmp("Gt", "0"_cint64), "pa"_var), std::move(projectionANode));
    auto projectionBNode =
        _eval("pb", _evalp(_get("b", _id()), "root"_var), std::move(filterANode));
    auto filterBNode =
        _filter(_evalf(_cmp("Gt", "1"_cint64), "pb"_var), std::move(projectionBNode));
    auto groupByNode = _gb(_varnames("pa"), _varnames("pc"), {"pb"_var}, std::move(filterBNode));
    auto rootNode = _root("pc")(std::move(groupByNode));

    ASSERT_EXPLAIN_V2_AUTO(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pc\n"
        "|   RefBlock: \n"
        "|       Variable [pc]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [pa]\n"
        "|   aggregations: \n"
        "|       [pc]\n"
        "|           Variable [pb]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pb]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "Evaluation [{pb}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pa]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [0]\n"
        "Evaluation [{pa}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a]\n"
        "|   PathIdentity []\n"
        "Scan [c1, {root}]\n",
        rootNode);

    // Construct using a builder. Note we construct the tree in a top-to-bottom fashion.
    auto rootNode1 = NodeBuilder{}
                         .root("pc")
                         .gb(_varnames("pa"), _varnames("pc"), {"pb"_var})
                         .filter(_evalf(_cmp("Gt", "1"_cint64), "pb"_var))
                         .eval("pb", _evalp(_get("b", _id()), "root"_var))
                         .filter(_evalf(_cmp("Gt", "0"_cint64), "pa"_var))
                         .eval("pa", _evalp(_get("a", _id()), "root"_var))
                         .finish(_scan("root", "c1"));

    ASSERT_EXPLAIN_V2_AUTO(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pc\n"
        "|   RefBlock: \n"
        "|       Variable [pc]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [pa]\n"
        "|   aggregations: \n"
        "|       [pc]\n"
        "|           Variable [pb]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pb]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "Evaluation [{pb}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pa]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [0]\n"
        "Evaluation [{pa}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a]\n"
        "|   PathIdentity []\n"
        "Scan [c1, {root}]\n",
        rootNode1);
}

TEST(TestInfra, GenerateABTLiterals) {
    // The following code fragment is in the same time used to construct an ABT and to assert that
    // it is explained correctly. Notice we have one space before the new line slashes in the macro.
#define SHORTHAND_EXAMPLE_ABT                             \
    NodeBuilder{}                                         \
        .root("pc")                                       \
        .collation({"pa:1", "pc:-1"})                     \
        .ls(1, 0)                                         \
        .spoolp("Lazy", 1, _varnames("pa"), _cbool(true)) \
        .gb(_varnames("pa"), _varnames("pc"), {"pb"_var}) \
        .filter(_evalf(_cmp("Gt", "1"_cint64), "pb"_var)) \
        .eval("pb", _evalp(_get("b", _id()), "root"_var)) \
        .filter(_evalf(_cmp("Gt", "0"_cint64), "pa"_var)) \
        .eval("pa", _evalp(_get("a", _id()), "root"_var)) \
        .finish(_scan("root", "c1"))

#define SHORTHAND_EXAMPLE_STR(x) #x
#define SHORTHAND_EXAMPLE_XSTR(x) SHORTHAND_EXAMPLE_STR(x)

    // Test compilation of generated shorthand initialization code. Test that explaining the abt
    // generated by the code fragment above results in a string containing the same content.
    const std::string exampleStr = SHORTHAND_EXAMPLE_XSTR(SHORTHAND_EXAMPLE_ABT);
    const ABT exampleNode = SHORTHAND_EXAMPLE_ABT;

    // We insert an extra space between nodes here to match the way the macro is defined. The linter
    // wants to insert one space before the new line symbols.
    const std::string exampleExplain = str::stream()
        << "NodeBuilder{} " << ExplainInShorthand{" "}.explain(exampleNode);

    ASSERT_EQ(exampleStr, exampleExplain);

#undef SHORTHAND_EXAMPLE_STR
#undef SHORTHAND_EXAMPLE_XSTR

#undef SHORTHAND_EXAMPLE_ABT
}

}  // namespace
}  // namespace mongo::optimizer
