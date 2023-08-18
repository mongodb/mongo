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


#pragma once

#include "mongo/db/query/optimizer/bool_expression.h"
#include "mongo/db/query/optimizer/utils/bool_expression_builder.h"

namespace mongo::optimizer {

/**
 * Converts a BoolExpr either from CNF to DNF or from DNF to CNF. Uses a supplied builder to use
 * during the construction of the converted tree.
 */
template <class T,
          class BuilderType,
          bool toCNF,
          class Expr = BoolExpr<T>,
          class Node = typename Expr::Node>
boost::optional<Node> convertTo(const Node& n,
                                BuilderType builder,
                                boost::optional<size_t> maxClauses) {
    using Atom = typename Expr::Atom;
    using Conj = typename Expr::Conjunction;
    using Disj = typename Expr::Disjunction;

    std::vector<std::vector<T>> newChildren(1 /*size*/);

    // Process the children of 'n' in order. Suppose the input (in CNF) was (a+b).(c+d). After
    // the first child, we have [[a], [b]] in 'newChildren'. After the second child, we have
    // [[a, c], [b, c], [a, d], [b, d]].
    for (const auto& child : n.template cast<std::conditional_t<toCNF, Disj, Conj>>()->nodes()) {
        auto childNode = child.template cast<std::conditional_t<toCNF, Conj, Disj>>();
        auto numGrandChildren = childNode->nodes().size();
        auto frontierSize = newChildren.size();

        if (maxClauses && frontierSize * numGrandChildren > maxClauses) {
            return boost::none;
        }

        // Each child (literal) under 'child' is added to a new copy of the existing vectors...
        for (size_t grandChild = 1; grandChild < numGrandChildren; grandChild++) {
            for (size_t i = 0; i < frontierSize; i++) {
                auto newNodeVec = newChildren.at(i);
                newNodeVec.push_back(
                    childNode->nodes().at(grandChild).template cast<Atom>()->getExpr());
                newChildren.push_back(newNodeVec);
            }
        }

        // Except the first child under 'child', which can modify the vectors in place.
        for (size_t i = 0; i < frontierSize; i++) {
            auto& nv = newChildren.at(i);
            nv.push_back(childNode->nodes().front().template cast<Atom>()->getExpr());
        }
    }

    builder.push(toCNF);
    for (auto& childList : newChildren) {
        builder.push(!toCNF);
        for (auto& child : childList) {
            builder.atom(std::move(child));
        }
        builder.pop();
    }
    return builder.finish();
}

/**
 * Converts a BoolExpr to DNF. Assumes 'n' is in CNF. Returns boost::none if the resulting
 * formula would have more than 'maxClauses' clauses.
 */
template <class T,
          class BuilderType = BoolExprBuilder<T>,
          class Expr = BoolExpr<T>,
          class Node = typename Expr::Node>
boost::optional<Node> convertToDNF(const Node& n,
                                   BuilderType builder = {},
                                   boost::optional<size_t> maxClauses = boost::none) {
    tassert(
        7115100, "Expected Node to be a Conjunction", n.template is<typename Expr::Conjunction>());
    return convertTo<T, BuilderType, false /*toCNF*/>(n, std::move(builder), maxClauses);
}

/**
 * Converts a BoolExpr to CNF. Assumes 'n' is in DNF. Returns boost::none if the resulting
 * formula would have more than 'maxClauses' clauses.
 */
template <class T,
          class BuilderType = BoolExprBuilder<T>,
          class Expr = BoolExpr<T>,
          class Node = typename Expr::Node>
boost::optional<Node> convertToCNF(const Node& n,
                                   BuilderType builder = {},
                                   boost::optional<size_t> maxClauses = boost::none) {
    tassert(
        7115101, "Expected Node to be a Disjunction", n.template is<typename Expr::Disjunction>());
    return convertTo<T, BuilderType, true /*toCNF*/>(n, std::move(builder), maxClauses);
}

}  // namespace mongo::optimizer
