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
 *
 * The simplifier is made aware if there are children which have been removed from the input vector
 * "v" for reasons of being always-true or always-false. Suppose we are building a DNF tree. First
 * we call .pushDisj(). Then at some point pushConj() was called. Then we added a few atoms with
 * .atom(), and finally call .pop(). At this point the simplifier is called, and receives the list
 * of atoms (v) we tried to add to the conjunction. We may in the extreme case remove all nodes. In
 * particular, if we are building a conjunction and we are left with no nodes but we know we had
 * always-true (but not always-false) children, we could create an always-true node. Conversely when
 * creating a disjunction with always-false (but not always-true) flag set, we'll create an
 * always-false node.
 */
template <class T>
struct DefaultSimplifyAndCreateNode {
    using BoolExprType = BoolExpr<T>;
    using Node = typename BoolExprType::Node;
    using NodeVector = typename BoolExprType::NodeVector;
    using Atom = typename BoolExprType::Atom;
    using Conj = typename BoolExprType::Conjunction;
    using Disj = typename BoolExprType::Disjunction;

    struct Result {
        // Either result node is present, or any of the flags below is set.
        boost::optional<Node> _node;
        bool _isTrue = false;
        bool _isFalse = false;
    };

    Result operator()(const BuilderNodeType type,
                      NodeVector v,
                      const bool hasTrue,
                      const bool hasFalse) const {
        if (v.empty()) {
            // We have no children in 'v', but possibly some children are trivially true or false.
            // So the rule that applies is either an identity, or an absorbing element.

            if (type == BuilderNodeType::Conj) {
                if (hasFalse) {
                    // False is the absorbing element of conjunction.
                    return {._isFalse = true};
                } else {
                    // True is the identity element of conjunction.
                    return {._isTrue = true};
                }
            } else {
                if (hasTrue) {
                    // True is the absorbing element of disjunction.
                    return {._isTrue = true};
                } else {
                    // False is the identity element of disjunction.
                    return {._isFalse = true};
                }
            }
        }

        if (type == BuilderNodeType::Conj) {
            return {BoolExprType::template make<Conj>(std::move(v))};
        } else {
            return {BoolExprType::template make<Disj>(std::move(v))};
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
        // Was there a child removed from the list which was always-true or always-false.
        bool _hasTrue;
        bool _hasFalse;
    };

public:
    BoolExprBuilder(SimplifyAndCreateNode simplifier = SimplifyAndCreateNode{},
                    Negator negator = Negator{})
        : _result(),
          _stack(),
          _currentNegated(false),
          _simplifier(std::move(simplifier)),
          _negator(std::move(negator)) {}

    template <typename... Ts>
    BoolExprBuilder& atom(Ts&&... pack) {
        return atom(T{std::forward<Ts>(pack)...});
    }

    BoolExprBuilder& atom(T value) {
        if (applyNegation()) {
            _negator(value);
        }
        _result = make<Atom>(std::move(value));
        maybeAddToParent();
        return *this;
    }

    BoolExprBuilder& subtree(Node expr) {
        struct Inserter {
            void transport(Atom& node) {
                _builder.atom(std::move(node.getExpr()));
            }

            void prepare(const Conj& /*node*/) {
                _builder.pushConj();
            }
            void transport(const Conj& /*node*/, const std::vector<Node>& /*children*/) {
                _builder.pop();
            }

            void prepare(const Disj& /*node*/) {
                _builder.pushDisj();
            }
            void transport(const Disj& /*node*/, const std::vector<Node>& /*children*/) {
                _builder.pop();
            }

            BoolExprBuilder& _builder;
        };

        Inserter instance{*this};
        algebra::transport<false>(expr, instance);
        return *this;
    }

    BoolExprBuilder& push(const bool isConjunction) {
        const bool negated = applyNegation();
        _stack.push_back(
            {(negated == isConjunction) ? BuilderNodeType::Disj : BuilderNodeType::Conj,
             negated,
             NodeVector{},
             false /*hasTrue*/,
             false /*hasFalse*/});
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
        auto [type, negated, v, hasTrue, hasFalse] = std::move(_stack.back());
        _stack.pop_back();

        auto simplifierResult = _simplifier(type, std::move(v), hasTrue, hasFalse);
        _result = std::move(simplifierResult._node);
        _isTrue = simplifierResult._isTrue;
        _isFalse = simplifierResult._isFalse;

        tassert(7962000,
                "BoolExprBuilder should not have both _result and either _isTrue or _isFalse set",
                !_result || (!_isTrue && !_isFalse));

        maybeAddToParent();
        return *this;
    }

    boost::optional<Node> finish() {
        while (!_stack.empty()) {
            pop();
        }
        return std::move(_result);
    }

    const SimplifyAndCreateNode& getSimplifier() const {
        return _simplifier;
    }

    SimplifyAndCreateNode& getSimplifier() {
        return _simplifier;
    }

    const Negator& getNegator() const {
        return _negator;
    }

    Negator& getNegator() {
        return _negator;
    }

private:
    void maybeAddToParent() {
        if (_stack.empty()) {
            return;
        }

        auto& last = _stack.back();
        if (_result) {
            last._vector.push_back(std::move(*_result));
            _result = boost::none;
        }

        if (_isTrue) {
            last._hasTrue = true;
            _isTrue = false;
        }
        if (_isFalse) {
            last._hasFalse = true;
            _isFalse = false;
        }
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
    bool _isTrue = false;
    bool _isFalse = false;

    std::vector<StackEntry> _stack;
    bool _currentNegated;

    SimplifyAndCreateNode _simplifier;
    Negator _negator;
};

}  // namespace mongo::optimizer
