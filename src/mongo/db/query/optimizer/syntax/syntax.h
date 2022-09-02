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

#include <string>

#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/syntax/syntax_fwd_declare.h"
#include "mongo/db/query/optimizer/utils/printable_enum.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

/**
 * This is the core typedef that represents an abstract binding tree (ABT). The templated types
 * represent all possible instances for a given ABT operator, each deriving from an Operator class
 * that indicates the number of children nodes.
 *
 * NOTE: If the set of possible types in an ABT changes, please update the corresponding gdb
 * pretty printer.
 */
using ABT = algebra::PolyValue<Blackhole,
                               Constant,  // expressions
                               Variable,
                               UnaryOp,
                               BinaryOp,
                               If,
                               Let,
                               LambdaAbstraction,
                               LambdaApplication,
                               FunctionCall,
                               EvalPath,
                               EvalFilter,
                               Source,
                               PathConstant,  // path elements
                               PathLambda,
                               PathIdentity,
                               PathDefault,
                               PathCompare,
                               PathDrop,
                               PathKeep,
                               PathObj,
                               PathArr,
                               PathTraverse,
                               PathField,
                               PathGet,
                               PathComposeM,
                               PathComposeA,
                               ScanNode,  // nodes
                               PhysicalScanNode,
                               ValueScanNode,
                               CoScanNode,
                               IndexScanNode,
                               SeekNode,
                               MemoLogicalDelegatorNode,
                               MemoPhysicalDelegatorNode,
                               FilterNode,
                               EvaluationNode,
                               SargableNode,
                               RIDIntersectNode,
                               BinaryJoinNode,
                               HashJoinNode,
                               MergeJoinNode,
                               UnionNode,
                               GroupByNode,
                               UnwindNode,
                               UniqueNode,
                               CollationNode,
                               LimitSkipNode,
                               ExchangeNode,
                               RootNode,
                               References,  // utilities
                               ExpressionBinder>;

template <size_t Arity>
using Operator = algebra::OpSpecificArity<ABT, Arity>;

template <size_t Arity>
using OperatorDynamic = algebra::OpSpecificDynamicArity<ABT, Arity>;

using OperatorDynamicHomogenous = OperatorDynamic<0>;

using ABTVector = std::vector<ABT>;

template <typename T, typename... Args>
inline auto make(Args&&... args) {
    return ABT::make<T>(std::forward<Args>(args)...);
}

template <typename... Args>
inline auto makeSeq(Args&&... args) {
    ABTVector seq;
    (seq.emplace_back(std::forward<Args>(args)), ...);
    return seq;
}

inline void assertExprSort(const ABT& e) {
    tassert(6624058, "expression syntax sort expected", e.is<ExpressionSyntaxSort>());
}

inline void assertPathSort(const ABT& e) {
    tassert(6624059, "path syntax sort expected", e.is<PathSyntaxSort>());
}

inline bool operator!=(const ABT& left, const ABT& right) {
    return !(left == right);
}

#define PATHSYNTAX_OPNAMES(F)   \
    /* comparison operations */ \
    F(Eq)                       \
    F(EqMember)                 \
    F(Neq)                      \
    F(Gt)                       \
    F(Gte)                      \
    F(Lt)                       \
    F(Lte)                      \
    F(Cmp3w)                    \
                                \
    /* binary operations */     \
    F(Add)                      \
    F(Sub)                      \
    F(Mult)                     \
    F(Div)                      \
                                \
    /* unary operations */      \
    F(Neg)                      \
                                \
    /* logical operations */    \
    F(And)                      \
    F(Or)                       \
    F(Not)

MAKE_PRINTABLE_ENUM(Operations, PATHSYNTAX_OPNAMES);
MAKE_PRINTABLE_ENUM_STRING_ARRAY(OperationsEnum, Operations, PATHSYNTAX_OPNAMES);
#undef PATHSYNTAX_OPNAMES

inline constexpr bool isUnaryOp(Operations op) {
    return op == Operations::Neg || op == Operations::Not;
}

inline constexpr bool isBinaryOp(Operations op) {
    return !isUnaryOp(op);
}

inline constexpr bool isComparisonOp(Operations op) {
    switch (op) {
        case Operations::Eq:
        case Operations::EqMember:
        case Operations::Neq:
        case Operations::Gt:
        case Operations::Gte:
        case Operations::Lt:
        case Operations::Lte:
        case Operations::Cmp3w:
            return true;
        default:
            return false;
    }
}

inline constexpr Operations reverseComparisonOp(Operations op) {
    switch (op) {
        case Operations::Eq:
        case Operations::Neq:
            return op;

        case Operations::Lt:
            return Operations::Gte;
        case Operations::Lte:
            return Operations::Gt;
        case Operations::Gt:
            return Operations::Lte;
        case Operations::Gte:
            return Operations::Lt;

        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * This is a special inert ABT node. It is used by rewriters to preserve structural properties of
 * nodes during in-place rewriting.
 */
class Blackhole final : public Operator<0> {
public:
    bool operator==(const Blackhole& other) const {
        return true;
    }
};

/**
 * This is a helper structure that represents Node internal references. Some relational nodes
 * implicitly reference named projections from its children.
 *
 * Canonical examples are: GROUP BY "a", ORDER BY "b", etc.
 *
 * We want to capture these references. The rule of ABTs says that the ONLY way to reference a named
 * entity is through the Variable class. The uniformity of the approach makes life much easier for
 * the optimizer developers.
 * On the other hand using Variables everywhere makes writing code more verbose, hence this helper.
 */
class References final : public OperatorDynamicHomogenous {
    using Base = OperatorDynamicHomogenous;

public:
    /**
     * Construct Variable objects out of provided vector of strings.
     */
    References(const std::vector<std::string>& names) : Base(ABTVector{}) {
        // Construct actual Variable objects from names and make them the children of this object.
        for (const auto& name : names) {
            nodes().emplace_back(make<Variable>(name));
        }
    }

    /**
     * Alternatively, construct references out of provided ABTs. This may be useful when the
     * internal references are more complex then a simple string. We may consider e.g. GROUP BY
     * (a+b).
     */
    References(ABTVector refs) : Base(std::move(refs)) {
        for (const auto& node : nodes()) {
            assertExprSort(node);
        }
    }

    bool operator==(const References& other) const {
        return nodes() == other.nodes();
    }
};

/**
 * This class represents a unified way of binding identifiers (strings) to expressions. Every ABT
 * node that introduces a new identifier must use this binder (i.e. all relational nodes adding new
 * projections).
 */
class ExpressionBinder : public OperatorDynamicHomogenous {
    using Base = OperatorDynamicHomogenous;
    std::vector<std::string> _names;

public:
    ExpressionBinder(std::string name, ABT expr) : Base(makeSeq(std::move(expr))) {
        _names.emplace_back(std::move(name));
        for (const auto& node : nodes()) {
            assertExprSort(node);
        }
    }

    ExpressionBinder(std::vector<std::string> names, ABTVector exprs)
        : Base(std::move(exprs)), _names(std::move(names)) {
        for (const auto& node : nodes()) {
            assertExprSort(node);
        }
    }

    bool operator==(const ExpressionBinder& other) const {
        return _names == other._names && exprs() == other.exprs();
    }

    const std::vector<std::string>& names() const {
        return _names;
    }

    const ABTVector& exprs() const {
        return nodes();
    }
};

}  // namespace mongo::optimizer
