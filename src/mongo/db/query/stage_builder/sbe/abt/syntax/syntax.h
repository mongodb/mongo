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

#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt/containers.h"
#include "mongo/db/query/stage_builder/sbe/abt/strong_alias.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax_fwd_declare.h"
#include "mongo/util/assert_util.h"

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
                               LambdaApplication,
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
