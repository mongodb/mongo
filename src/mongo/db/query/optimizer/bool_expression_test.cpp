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

#include "mongo/db/query/optimizer/bool_expression.h"

#include <algorithm>

#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {
namespace {

TEST(BoolExpr, IntervalCNFtoDNF) {
    using namespace unit_test_abt_literals;

    {
        // Convert to CNF: singular DNF becomes singular CNF
        auto interval = _disj(_conj(_interval(_incl("v1"_var), _incl("v2"_var))));
        ASSERT_INTERVAL_AUTO(
            "{\n"
            "    {\n"
            "        {[Variable [v1], Variable [v2]]}\n"
            "    }\n"
            "}\n",
            interval);

        auto res = BoolExpr<IntervalRequirement>::convertToCNF(interval);
        ASSERT(res.has_value());
        ASSERT_INTERVAL_AUTO(
            "{\n"
            "    {\n"
            "        {[Variable [v1], Variable [v2]]}\n"
            "    }\n"
            "}\n",
            *res);
    }

    {
        // Convert to CNF: OR of singleton conjunctions becomes singleton AND.
        auto interval = _disj(_conj(_interval(_incl("v1"_var), _incl("v2"_var))),
                              _conj(_interval(_incl("v1"_var), _incl("v2"_var))));
        ASSERT_INTERVAL_AUTO(
            "{\n"
            "    {\n"
            "        {[Variable [v1], Variable [v2]]}\n"
            "    }\n"
            " U \n"
            "    {\n"
            "        {[Variable [v1], Variable [v2]]}\n"
            "    }\n"
            "}\n",
            interval);

        auto res = BoolExpr<IntervalRequirement>::convertToCNF(interval);
        ASSERT(res.has_value());
        ASSERT_INTERVAL_AUTO(
            "{\n"
            "    {\n"
            "        {[Variable [v1], Variable [v2]]}\n"
            "     U \n"
            "        {[Variable [v1], Variable [v2]]}\n"
            "    }\n"
            "}\n",
            *res);
    }

    {
        // Convert to DNF: AND is distributed across two ORs
        auto interval = _conj(_disj(_interval(_incl("v1"_var), _incl("v3"_var)),
                                    _interval(_incl("v2"_var), _incl("v4"_var))),
                              _disj(_interval(_incl("v10"_var), _incl("v30"_var)),
                                    _interval(_incl("v20"_var), _incl("v40"_var))));

        ASSERT_INTERVAL_AUTO(
            "{\n"
            "    {\n"
            "        {[Variable [v1], Variable [v3]]}\n"
            "     U \n"
            "        {[Variable [v2], Variable [v4]]}\n"
            "    }\n"
            " ^ \n"
            "    {\n"
            "        {[Variable [v10], Variable [v30]]}\n"
            "     U \n"
            "        {[Variable [v20], Variable [v40]]}\n"
            "    }\n"
            "}\n",
            interval);

        auto res = BoolExpr<IntervalRequirement>::convertToDNF(interval);
        ASSERT(res.has_value());
        ASSERT_INTERVAL_AUTO(
            "{\n"
            "    {\n"
            "        {[Variable [v1], Variable [v3]]}\n"
            "     ^ \n"
            "        {[Variable [v10], Variable [v30]]}\n"
            "    }\n"
            " U \n"
            "    {\n"
            "        {[Variable [v2], Variable [v4]]}\n"
            "     ^ \n"
            "        {[Variable [v10], Variable [v30]]}\n"
            "    }\n"
            " U \n"
            "    {\n"
            "        {[Variable [v1], Variable [v3]]}\n"
            "     ^ \n"
            "        {[Variable [v20], Variable [v40]]}\n"
            "    }\n"
            " U \n"
            "    {\n"
            "        {[Variable [v2], Variable [v4]]}\n"
            "     ^ \n"
            "        {[Variable [v20], Variable [v40]]}\n"
            "    }\n"
            "}\n",
            *res);

        // Test conversion clause limit: the same conversion succeeds with a max limit of 4 clauses
        // but fails with a limit of 3.
        ASSERT(BoolExpr<IntervalRequirement>::convertToDNF(interval, 4).has_value());
        ASSERT_FALSE(BoolExpr<IntervalRequirement>::convertToDNF(interval, 3).has_value());
    }
}

using IntBoolExpr = BoolExpr<int>;
class BoolVariableEvaluator {
public:
    BoolVariableEvaluator(const int assignment) : _assignment(assignment){};

    bool transport(const IntBoolExpr::Atom& node) {
        const auto& expr = node.getExpr();
        return (_assignment >> expr) & 1;
    }

    bool transport(const IntBoolExpr::Conjunction& node, std::vector<bool> childResults) {
        return std::all_of(childResults.begin(), childResults.end(), [](bool v) { return v; });
    }

    bool transport(const IntBoolExpr::Disjunction& node, std::vector<bool> childResults) {
        return std::any_of(childResults.begin(), childResults.end(), [](bool v) { return v; });
    }

    bool evaluate(const IntBoolExpr::Node& n) {
        return algebra::transport<false>(n, *this);
    }

private:
    const int _assignment;
};

// Builds a BoolExpr according to input 'permutation'. The root will have 'rootChildren' children,
// and each child will itself have between [1, maxBranching] atom children (variables with int Ids).
template <bool buildCNF>
std::pair<IntBoolExpr::Node, int> buildExpr(int rootChildren, int permutation, int maxBranching) {
    auto getNextNumChildren = [&permutation, &maxBranching]() {
        const int result = permutation % maxBranching;
        permutation /= maxBranching;
        return result + 1;
    };

    int varId = 0;
    IntBoolExpr::Builder builder;
    builder.push(buildCNF);
    for (int i = 0; i < rootChildren; i++) {
        builder.push(!buildCNF);
        const int numAtomsForChild = getNextNumChildren();
        for (int j = 0; j < numAtomsForChild; j++) {
            builder.atom(varId++);
        }
        builder.pop();
    }
    return {std::move(*builder.finish()), varId};
}

// For every assignment to the 'n' variables, 'expr' and 'transformed' should have the same result.
void assertEquiv(const IntBoolExpr::Node& expr, const IntBoolExpr::Node& transformed, int n) {
    for (int assignment = 0; assignment < 1 << n; assignment++) {
        BoolVariableEvaluator bve{assignment};
        auto expected = bve.evaluate(expr);
        auto result = bve.evaluate(transformed);
        ASSERT_EQ(expected, result);
    }
}

TEST(BoolExpr, BoolExprPermutations) {
    // Test for BoolExpr CNF<->DNF. Generates all BoolExprs in CNF and DNF where each internal node
    // has a maximum of maxBranching children. The leaves of the BoolExpr are variables. Converts
    // each BoolExpr to DNF or CNF, respectively, and asserts that for every assignment to the
    // variables, the two formulas have the same result.
    constexpr int maxBranching = 3;

    for (int rootNumChildren = 1; rootNumChildren <= maxBranching; rootNumChildren++) {
        // For each root child, we choose a number of children in [1, maxBranching] based on the
        // permutation. So, it should have rootNumChildren values each of max value maxBranching.
        int permutations = pow(maxBranching, rootNumChildren);
        for (int permutation = 0; permutation < permutations; permutation++) {
            // DNF -> CNF
            {
                auto [expr, numVars] =
                    buildExpr<false /*CNF*/>(rootNumChildren, permutation, maxBranching);
                auto transformed = IntBoolExpr::convertToCNF(expr);
                ASSERT(transformed.has_value());
                assertEquiv(expr, *transformed, numVars);
            }

            // CNF -> DNF
            {
                auto [expr, numVars] =
                    buildExpr<true /*CNF*/>(rootNumChildren, permutation, maxBranching);
                auto transformed = IntBoolExpr::convertToDNF(expr);
                ASSERT(transformed.has_value());
                assertEquiv(expr, *transformed, numVars);
            }
        }
    }
}
}  // namespace
}  // namespace mongo::optimizer
