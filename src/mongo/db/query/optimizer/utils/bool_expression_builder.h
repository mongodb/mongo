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

namespace mongo::optimizer {

template <class T>
struct TassertNegator {
    void operator()(T& v) const {
        tassert(7453909, "No negator specified", false);
    }
};

enum class BuilderNodeType { Conj, Disj };

/**
 * Default functor to simplify a list of children and create an appropriate node to return to the
 * parent level, or as a result of the builder. By default we skip adding nodes for empty child
 * lists, and we do not perform any simplification to the children.
 */
template <class T, class Expr = BoolExpr<T>>
struct DefaultSimplifyAndCreateNode {
    boost::optional<typename Expr::Node> operator()(const BuilderNodeType type,
                                                    typename Expr::NodeVector v) const {
        if (v.empty()) {
            return boost::none;
        }
        if (type == BuilderNodeType::Conj) {
            return Expr::template make<typename Expr::Conjunction>(std::move(v));
        } else {
            return Expr::template make<typename Expr::Disjunction>(std::move(v));
        }
    }
};

/**
 * Builder which is used to create BoolExpr trees. It supports negation, which is translated
 * internally to conjunction and disjunction via deMorgan elimination. The following template
 * parameters need to be supplied:
 *   1. Simplification and create node function. This function will accept a list of children of
 * conjunction or disjunction, and create an appropriate node.
 *   2. Negation function. Used for deMorgan transformation. For example "not (x and y) is
 * simplified to neg(x) or neg(y).
 *
 *  Usage:
 *    1. use .pushConj() or .pushDisj() to begin a new conjunction / disjunction.
 *    2. use .atom() to add elements to the current conjunction / disjunction.
 *    3. use .pop() when done adding elements to the current conjunction / disjunction, and
 * implicitly move to adding elements to the parent.
 *    4. When we are done, call .finish(). Finish returns an empty result if no elements have
 * been added to the root level, and we do not simplify singular conjunction/disjunctions.
 */
template <class T,
          class SimplifyAndCreateNode = DefaultSimplifyAndCreateNode<T>,
          class Negator = TassertNegator<T>>
class BoolExprBuilder {
    using BoolExprType = BoolExpr<T>;
    using Node = typename BoolExprType::Node;
    using NodeVector = typename BoolExprType::NodeVector;
    using Atom = typename BoolExprType::Atom;
    using Conj = typename BoolExprType::Conjunction;
    using Disj = typename BoolExprType::Disjunction;

    struct StackEntry {
        // What is the type of the node we're currently adding to.
        BuilderNodeType _type;
        // Is the subtree negated.
        bool _negated;
        // List of children for the current node.
        NodeVector _vector;
    };

public:
    BoolExprBuilder() : _result(), _stack(), _currentNegated(false) {}

    template <typename... Ts>
    BoolExprBuilder& atom(Ts&&... pack) {
        return atom(T{std::forward<Ts>(pack)...});
    }

    BoolExprBuilder& atom(T value) {
        if (applyNegation()) {
            Negator{}(value);
        }
        _result = make<Atom>(std::move(value));
        maybeAddToParent();
        return *this;
    }

    BoolExprBuilder& subtree(Node expr) {
        tassert(6902603, "BoolExprBuilder::subtree does not support negation", !applyNegation());
        _result = std::move(expr);
        maybeAddToParent();
        return *this;
    }

    BoolExprBuilder& push(const bool isConjunction) {
        const bool negated = applyNegation();
        _stack.push_back(
            {(negated == isConjunction) ? BuilderNodeType::Disj : BuilderNodeType::Conj,
             negated,
             NodeVector{}});
        return *this;
    }

    BoolExprBuilder& pushConj() {
        return push(true /*isConjunction*/);
    }

    BoolExprBuilder& pushDisj() {
        return push(false /*isConjunction*/);
    }

    BoolExprBuilder& negate() {
        _currentNegated = !_currentNegated;
        return *this;
    }

    BoolExprBuilder& pop() {
        auto [type, negated, v] = std::move(_stack.back());
        _stack.pop_back();
        _result = SimplifyAndCreateNode{}(type, std::move(v));

        maybeAddToParent();
        return *this;
    }

    boost::optional<Node> finish() {
        while (!_stack.empty()) {
            pop();
        }
        return std::move(_result);
    }

private:
    void maybeAddToParent() {
        if (_stack.empty() || !_result) {
            return;
        }

        _stack.back()._vector.push_back(std::move(*_result));
        _result = boost::none;
    }

    bool applyNegation() {
        const bool negated = (!_stack.empty() && _stack.back()._negated) ^ _currentNegated;
        _currentNegated = false;
        return negated;
    }

    template <typename T1, typename... Args>
    static auto make(Args&&... args) {
        return BoolExprType::template make<T1>(std::forward<Args>(args)...);
    }

    boost::optional<Node> _result;
    std::vector<StackEntry> _stack;
    bool _currentNegated;
};

}  // namespace mongo::optimizer
