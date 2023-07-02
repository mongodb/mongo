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


#pragma once

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/preprocessor/control/iif.hpp>
#include <cstddef>
#include <functional>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

template <class T>
struct TassertNegator {
    T operator()(T v) const {
        tassert(7453909, "No negator specified", false);
        return v;
    }
};


/**
 * Represents a generic boolean expression with arbitrarily nested conjunctions and disjunction
 * elements.
 */
template <class T>
struct BoolExpr {
    class Atom;
    class Conjunction;
    class Disjunction;

    using Node = algebra::PolyValue<Atom, Conjunction, Disjunction>;
    using NodeVector = std::vector<Node>;

    class Atom final : public algebra::OpFixedArity<Node, 0> {
        using Base = algebra::OpFixedArity<Node, 0>;

    public:
        Atom(T expr) : Base(), _expr(std::move(expr)) {}

        bool operator==(const Atom& other) const {
            return _expr == other._expr;
        }

        const T& getExpr() const {
            return _expr;
        }
        T& getExpr() {
            return _expr;
        }

    private:
        T _expr;
    };

    class Conjunction final : public algebra::OpDynamicArity<Node, 0> {
        using Base = algebra::OpDynamicArity<Node, 0>;

    public:
        Conjunction(NodeVector children) : Base(std::move(children)) {
            uassert(6624351, "Must have at least one child", !Base::nodes().empty());
        }

        bool operator==(const Conjunction& other) const {
            return Base::nodes() == other.nodes();
        }
    };

    class Disjunction final : public algebra::OpDynamicArity<Node, 0> {
        using Base = algebra::OpDynamicArity<Node, 0>;

    public:
        Disjunction(NodeVector children) : Base(std::move(children)) {
            uassert(6624301, "Must have at least one child", !Base::nodes().empty());
        }

        bool operator==(const Disjunction& other) const {
            return Base::nodes() == other.nodes();
        }
    };


    /**
     * Utility functions.
     */
    template <typename T1, typename... Args>
    static auto make(Args&&... args) {
        return Node::template make<T1>(std::forward<Args>(args)...);
    }

    template <typename... Args>
    static auto makeSeq(Args&&... args) {
        NodeVector seq;
        (seq.emplace_back(std::forward<Args>(args)), ...);
        return seq;
    }

    template <typename... Args>
    static Node makeSingularDNF(Args&&... args) {
        return make<Disjunction>(
            makeSeq(make<Conjunction>(makeSeq(make<Atom>(T{std::forward<Args>(args)...})))));
    }

    static boost::optional<const T&> getSingularDNF(const Node& n) {
        if (auto disjunction = n.template cast<Disjunction>();
            disjunction != nullptr && disjunction->nodes().size() == 1) {
            if (auto conjunction = disjunction->nodes().front().template cast<Conjunction>();
                conjunction != nullptr && conjunction->nodes().size() == 1) {
                if (auto atom = conjunction->nodes().front().template cast<Atom>();
                    atom != nullptr) {
                    return {atom->getExpr()};
                }
            }
        }
        return {};
    }

    static bool isSingularDNF(const Node& n) {
        return getSingularDNF(n).has_value();
    }

    /**
     * Context present during traversal.
     */
    struct VisitorContext {
        /**
         * Get the index of the child element in the conjunction or disjunction being traversed.
         */
        size_t getChildIndex() const {
            return _childIndex;
        }

        /**
         * Allow the visitor to signal that traversal should end early.
         */
        void returnEarly() const {
            _returnEarly = true;
        }

    private:
        size_t _childIndex = 0;
        mutable bool _returnEarly = false;

        friend struct BoolExpr<T>;
    };

    using AtomPredConst = std::function<bool(const T& expr)>;

    template <typename ListType, typename NodeType, typename Visitor>
    static size_t visitNodes(NodeType&& node, const Visitor& visitor) {
        VisitorContext ctx;
        for (auto&& n : node.template cast<ListType>()->nodes()) {
            visitor(n, ctx);
            ctx._childIndex++;
            if (ctx._returnEarly) {
                break;
            }
        }
        return ctx._childIndex;
    }

    template <typename NodeType, typename Visitor>
    static size_t visitConjuncts(NodeType&& node, const Visitor& visitor) {
        return visitNodes<Conjunction>(node, visitor);
    }

    template <typename NodeType, typename Visitor>
    static size_t visitDisjuncts(NodeType&& node, const Visitor& visitor) {
        return visitNodes<Disjunction>(node, visitor);
    }

    template <typename NodeType, typename Visitor>
    static void visitAtom(NodeType&& node, const Visitor& visitor) {
        const VisitorContext ctx;
        visitor(node.template cast<Atom>()->getExpr(), ctx);
    }

    template <typename NodeType, typename Visitor>
    static void visitCNF(NodeType&& node, const Visitor& visitor) {
        visitConjuncts(node, [&](const Node& child, const VisitorContext& conjCtx) {
            visitDisjuncts(child, [&](const Node& grandChild, const VisitorContext& disjCtx) {
                visitor(grandChild.template cast<Atom>()->getExpr(), disjCtx);
                if (disjCtx._returnEarly) {
                    conjCtx.returnEarly();
                }
            });
        });
    }

    template <typename NodeType, typename Visitor>
    static void visitDNF(NodeType&& node, const Visitor& visitor) {
        visitDisjuncts(node, [&](NodeType&& child, const VisitorContext& disjCtx) {
            visitConjuncts(child, [&](NodeType&& grandChild, const VisitorContext& conjCtx) {
                visitor(grandChild.template cast<Atom>()->getExpr(), conjCtx);
                if (conjCtx._returnEarly) {
                    disjCtx.returnEarly();
                }
            });
        });
    }

    template <typename NodeType, typename Visitor>
    static void visitSingletonDNF(NodeType&& node, const Visitor& visitor) {
        tassert(7382800, "Expected a singleton disjunction", isSingletonDisjunction(node));
        visitDNF(node, visitor);
    }

    template <typename NodeType, typename Visitor>
    static void visitAnyShape(NodeType&& node, const Visitor& atomVisitor) {
        constexpr bool isConst = std::is_const_v<std::remove_reference_t<NodeType>>;
        using VectorT = std::conditional_t<isConst, const NodeVector&, NodeVector&>;
        struct AtomTransport {
            void transport(std::conditional_t<isConst, const Conjunction&, Conjunction&>, VectorT) {
                // noop
            }
            void transport(std::conditional_t<isConst, const Disjunction&, Disjunction&>, VectorT) {
                // noop
            }
            void transport(std::conditional_t<isConst, const Atom&, Atom&> node) {
                const VisitorContext ctx;
                atomVisitor(node.getExpr(), ctx);
            }
            const Visitor& atomVisitor;
        };
        AtomTransport impl{atomVisitor};
        algebra::transport<false>(node, impl);
    }

    template <typename NodeType>
    static T& firstDNFLeaf(NodeType&& node) {
        T* leaf = nullptr;
        visitDNF(node, [&](T& e, const VisitorContext& ctx) {
            leaf = &e;
            ctx.returnEarly();
        });
        tassert(7382801, "Expected a non-empty expression", leaf);
        return *leaf;
    }

    static bool any(const Node& node, const AtomPredConst& atomPred) {
        bool result = false;
        visitAnyShape(node, [&](const T& atom, const VisitorContext& ctx) {
            if (atomPred(atom)) {
                result = true;
                ctx.returnEarly();
            }
        });
        return result;
    }

    static bool all(const Node& node, const AtomPredConst& atomPred) {
        bool result = true;
        visitAnyShape(node, [&](const T& atom, const VisitorContext& ctx) {
            if (!atomPred(atom)) {
                result = false;
                ctx.returnEarly();
            }
        });
        return result;
    }

    static bool isCNF(const Node& n) {
        if (n.template is<Conjunction>()) {
            bool disjunctions = true;
            visitConjuncts(n, [&](const Node& child, const VisitorContext& ctx) {
                if (!child.template is<Disjunction>()) {
                    disjunctions = false;
                    ctx.returnEarly();
                }
            });
            return disjunctions;
        }
        return false;
    }

    static bool isDNF(const Node& n) {
        if (n.template is<Disjunction>()) {
            bool conjunctions = true;
            visitDisjuncts(n, [&](const Node& child, const VisitorContext& ctx) {
                if (!child.template is<Conjunction>()) {
                    conjunctions = false;
                    ctx.returnEarly();
                }
            });
            return conjunctions;
        }
        return false;
    }

    static bool isSingletonDisjunction(const Node& node) {
        auto* disjunction = node.template cast<Disjunction>();
        return disjunction && disjunction->nodes().size() == 1;
    }

    static size_t numLeaves(const Node& n) {
        return NumLeavesTransporter().countLeaves(n);
    }

    /**
     * Builder which is used to create BoolExpr trees. It supports negation, which is translated
     * internally to conjunction and disjunction via deMorgan elimination. The following template
     * parameters need to be supplied:
     *   1. Flag to enable empty or singular conjunction/disjunction simplifications. For example
     * or-ing 0 elements results in the default constructed value of T (T{}).
     *   2. Flag to allow removing of duplicate predicates. For example "x and x" is simplified to
     * just "x".
     *   3. Negation function. Used for deMorgan transformation. For example "not (x and y) is
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
    template <bool simplifyEmptyOrSingular = false,
              bool removeDups = false,
              class Negator = TassertNegator<T>>
    class Builder {
        enum class NodeType { Conj, Disj };

        struct StackEntry {
            NodeType _type;
            bool _negated;
            NodeVector _vector;
        };

    public:
        Builder() : _result(), _stack(), _currentNegated(false) {}

        template <typename... Ts>
        Builder& atom(Ts&&... pack) {
            return atom(T{std::forward<Ts>(pack)...});
        }

        Builder& atom(T value) {
            if (isCurrentlyNegated()) {
                value = Negator{}(std::move(value));
            }
            _result = make<Atom>(std::move(value));
            maybeAddToParent();
            return *this;
        }

        Builder& subtree(BoolExpr::Node expr) {
            tassert(6902603,
                    "BoolExpr::Builder::subtree does not support negation",
                    !isCurrentlyNegated());
            _result = std::move(expr);
            maybeAddToParent();
            return *this;
        }

        Builder& push(const bool isConjunction) {
            const bool negated = isCurrentlyNegated();
            _stack.push_back({(negated == isConjunction) ? NodeType::Disj : NodeType::Conj,
                              negated,
                              NodeVector{}});
            return *this;
        }

        Builder& pushConj() {
            return push(true /*isConjunction*/);
        }

        Builder& pushDisj() {
            return push(false /*isConjunction*/);
        }

        Builder& negate() {
            _currentNegated = !_currentNegated;
            return *this;
        }

        Builder& pop() {
            auto [type, negated, v] = std::move(_stack.back());
            _stack.pop_back();

            if constexpr (simplifyEmptyOrSingular) {
                if (v.empty()) {
                    // Empty set of children: return either default constructed T{} or its negation.
                    _result = make<Atom>(type == NodeType::Conj ? Negator{}(T{}) : T{});
                } else if (v.size() == 1) {
                    // Eliminate singular conjunctions / disjunctions.
                    _result = std::move(v.front());
                } else {
                    createNode(type, std::move(v));
                }
            } else if (v.empty()) {
                _result = boost::none;
            } else {
                createNode(type, std::move(v));
            }

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

            auto& parentVector = _stack.back()._vector;
            if (!removeDups ||
                std::find(parentVector.cbegin(), parentVector.cend(), *_result) ==
                    parentVector.cend()) {
                // Eliminate duplicate elements.
                parentVector.push_back(std::move(*_result));
            }
            _result = boost::none;
        }

        void createNode(const NodeType type, NodeVector v) {
            if (type == NodeType::Conj) {
                _result = make<Conjunction>(std::move(v));
            } else {
                _result = make<Disjunction>(std::move(v));
            }
        }

        bool isCurrentlyNegated() {
            const bool negated = (!_stack.empty() && _stack.back()._negated) ^ _currentNegated;
            _currentNegated = false;
            return negated;
        }

        boost::optional<Node> _result;
        std::vector<StackEntry> _stack;
        bool _currentNegated;
    };

    /**
     * Converts a BoolExpr to DNF. Assumes 'n' is in CNF. Returns boost::none if the resulting
     * formula would have more than 'maxClauses' clauses.
     */
    static boost::optional<Node> convertToDNF(const Node& n,
                                              boost::optional<size_t> maxClauses = boost::none) {
        tassert(7115100, "Expected Node to be a Conjunction", n.template is<Conjunction>());
        return convertTo<false /*toCNF*/>(n, maxClauses);
    }

    /**
     * Converts a BoolExpr to CNF. Assumes 'n' is in DNF. Returns boost::none if the resulting
     * formula would have more than 'maxClauses' clauses.
     */
    static boost::optional<Node> convertToCNF(const Node& n,
                                              boost::optional<size_t> maxClauses = boost::none) {
        tassert(7115101, "Expected Node to be a Disjunction", n.template is<Disjunction>());
        return convertTo<true /*toCNF*/>(n, maxClauses);
    }

private:
    class NumLeavesTransporter {
    public:
        size_t transport(const Atom& node) {
            return 1;
        }

        size_t transport(const Conjunction& node, std::vector<size_t> childResults) {
            return std::reduce(childResults.begin(), childResults.end());
        }

        size_t transport(const Disjunction& node, std::vector<size_t> childResults) {
            return std::reduce(childResults.begin(), childResults.end());
        }

        size_t countLeaves(const Node& expr) {
            return algebra::transport<false>(expr, *this);
        }
    };

    template <bool toCNF,
              class TopLevel = std::conditional_t<toCNF, Conjunction, Disjunction>,
              class SecondLevel = std::conditional_t<toCNF, Disjunction, Conjunction>>
    static boost::optional<Node> convertTo(const Node& n, boost::optional<size_t> maxClauses) {
        std::vector<NodeVector> newChildren;
        newChildren.push_back({});

        // Process the children of 'n' in order. Suppose the input (in CNF) was (a+b).(c+d). After
        // the first child, we have [[a], [b]] in 'newChildren'. After the second child, we have
        // [[a, c], [b, c], [a, d], [b, d]].
        for (const auto& child : n.template cast<SecondLevel>()->nodes()) {
            auto childNode = child.template cast<TopLevel>();
            auto numGrandChildren = childNode->nodes().size();
            auto frontierSize = newChildren.size();

            if (maxClauses.has_value() && frontierSize * numGrandChildren > maxClauses) {
                return boost::none;
            }

            // Each child (literal) under 'child' is added to a new copy of the existing vectors...
            for (size_t grandChild = 1; grandChild < numGrandChildren; grandChild++) {
                for (size_t i = 0; i < frontierSize; i++) {
                    NodeVector newNodeVec = newChildren.at(i);
                    newNodeVec.push_back(childNode->nodes().at(grandChild));
                    newChildren.push_back(newNodeVec);
                }
            }

            // Except the first child under 'child', which can modify the vectors in place.
            for (size_t i = 0; i < frontierSize; i++) {
                NodeVector& nv = newChildren.at(i);
                nv.push_back(childNode->nodes().front());
            }
        }

        NodeVector res;
        for (size_t i = 0; i < newChildren.size(); i++) {
            res.push_back(make<SecondLevel>(std::move(newChildren[i])));
        }
        return make<TopLevel>(res);
    }
};

}  // namespace mongo::optimizer
