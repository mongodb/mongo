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

    template <typename... Args>
    static Node makeSingularCNF(Args&&... args) {
        return make<Conjunction>(
            makeSeq(make<Disjunction>(makeSeq(make<Atom>(T{std::forward<Args>(args)...})))));
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

    static boost::optional<const T&> getSingularCNF(const Node& n) {
        if (auto disjunction = n.template cast<Conjunction>();
            disjunction != nullptr && disjunction->nodes().size() == 1) {
            if (auto conjunction = disjunction->nodes().front().template cast<Disjunction>();
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

    static bool isSingularCNF(const Node& n) {
        return getSingularCNF(n).has_value();
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
        tassert(7979100, "Expected conjunction", node.template is<Conjunction>());
        return visitNodes<Conjunction>(node, visitor);
    }

    template <typename NodeType, typename Visitor>
    static size_t visitDisjuncts(NodeType&& node, const Visitor& visitor) {
        tassert(7979101, "Expected disjunction", node.template is<Disjunction>());
        return visitNodes<Disjunction>(node, visitor);
    }

    template <typename NodeType, typename Visitor>
    static size_t visitConjDisj(const bool conjunctive, NodeType&& node, const Visitor& visitor) {
        if (conjunctive) {
            return visitConjuncts(node, visitor);
        } else {
            return visitDisjuncts(node, visitor);
        }
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
     * Pick the first conjunct matching the condition specified by the provided lambda. Assert on
     * non-trivial expression. Result includes the index of the conjunct.
     */
    static auto findFirst(const Node& n, const std::function<bool(const T& entry)>& fn) {
        tassert(7453907,
                "Expected expression to be a singleton disjunction",
                isSingletonDisjunction(n));

        size_t i = 0;
        boost::optional<std::pair<size_t, const T*>> res;
        visitDNF(n, [&](const T& entry, const VisitorContext& ctx) {
            if (fn(entry)) {
                res = {{i, &entry}};
                ctx.returnEarly();
                return;
            }
            ++i;
        });
        return res;
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
};

}  // namespace mongo::optimizer
