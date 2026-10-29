// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt/containers.h"
#include "mongo/db/query/stage_builder/sbe/abt/strong_alias.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax_fwd_declare.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::abt {

/**
 * Representation of a variable name. Cannot be empty.
 */
struct ProjectionNameAliasTag {
    static constexpr bool kAllowEmpty = false;
};
using ProjectionName = StrongStringAlias<ProjectionNameAliasTag>;

using ProjectionNameSet = opt::unordered_set<ProjectionName, ProjectionName::Hasher>;
using ProjectionNameVector = std::vector<ProjectionName>;

template <typename T>
using ProjectionNameMap = opt::unordered_map<ProjectionName, T, ProjectionName::Hasher>;

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
                               NaryOp,
                               If,
                               Let,
                               MultiLet,
                               LambdaAbstraction,
                               FunctionCall,
                               Source,
                               Switch,
                               References,  // utilities
                               ExpressionBinder>;

/**
 * ABT operators which have a fixed arity.
 */
template <size_t Arity>
using ABTOpFixedArity = algebra::OpFixedArity<ABT, Arity>;

/**
 * ABT operators which have a dynamic arity with an optional known minimum.
 */
template <size_t Arity>
using ABTOpDynamicArity = algebra::OpDynamicArity<ABT, Arity>;

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

inline bool operator!=(const ABT& left, const ABT& right) {
    return !(left == right);
}

/**
 * This is a special inert ABT node. It is used by rewriters to preserve structural properties of
 * nodes during in-place rewriting.
 */
class Blackhole final : public ABTOpFixedArity<0> {
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
class References final : public ABTOpDynamicArity<0> {
    using Base = ABTOpDynamicArity<0>;

public:
    /**
     * Construct Variable objects out of provided vector of strings.
     */
    References(const ProjectionNameVector& names) : Base(ABTVector{}) {
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
class ExpressionBinder : public ABTOpDynamicArity<0> {
    using Base = ABTOpDynamicArity<0>;
    ProjectionNameVector _names;

public:
    ExpressionBinder(ProjectionName name, ABT expr) : Base(makeSeq(std::move(expr))) {
        _names.emplace_back(std::move(name));
        for (const auto& node : nodes()) {
            assertExprSort(node);
        }
    }

    ExpressionBinder(ProjectionNameVector names, ABTVector exprs)
        : Base(std::move(exprs)), _names(std::move(names)) {
        for (const auto& node : nodes()) {
            assertExprSort(node);
        }
    }

    bool operator==(const ExpressionBinder& other) const {
        return _names == other._names && exprs() == other.exprs();
    }

    const ProjectionNameVector& names() const {
        return _names;
    }

    const ABTVector& exprs() const {
        return nodes();
    }
};

}  // namespace mongo::abt
