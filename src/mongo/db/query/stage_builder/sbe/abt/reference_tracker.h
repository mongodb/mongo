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

#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"

#include <cstddef>
#include <functional>
#include <memory>

#include <absl/container/node_hash_map.h>

namespace mongo::abt {

/**
 * Every Variable ABT conceptually references to a point in the ABT tree. The pointed tree is the
 * definition of the variable.
 */
struct Definition {
    /**
     * Pointer to ABT that defines the variable. It can be any Expr (e.g. let expression, lambda
     * expression).
     */
    ABT::reference_type definedBy;

    /**
     * Pointer to actual definition of variable (i.e. the appropriate expression under a Binder).
     */
    ABT::reference_type definition;
};

struct CollectedInfo;
using DefinitionsMap = ProjectionNameMap<Definition>;
using ResolvedVariablesMap = absl::node_hash_map<const Variable*, Definition>;

/**
 * Describes Variable references that are safe to move-from in SBE.
 *
 * The concept of "last reference" depends on evaluation order. For example:
 * - in 'if b then f(x) else g(x)', both uses of 'x' are a last reference, because only one branch
 *   is taken.
 * - in 'x + x' or 'f(x, x)' we would need to know whether the left or right is evaluated first.
 *   If this isn't well defined we have to assume neither is a last reference.
 */
using LastRefsSet = opt::unordered_set<const Variable*>;

/**
 * Helps enforce scoping and validity rules for definitions and Variable references.
 */
class VariableEnvironment {
    VariableEnvironment(std::unique_ptr<CollectedInfo> info,
                        boost::optional<LastRefsSet> lastRefs,
                        std::unique_ptr<ResolvedVariablesMap> resVarMap);

public:
    /**
     * Build the environment for the given ABT tree. The environment is valid as long as the tree
     * does not change. More specifically, if a variable defining node is removed from the tree then
     * the environment becomes stale and has to be rebuilt.
     *
     * Passing 'computeLastRefs=false' lets us skip some analysis, on both build() and rebuild(),
     * but 'isLastRef()' will conservatively return false.
     */
    static VariableEnvironment build(const ABT& root, bool computeLastRefs = true);
    void rebuild(const ABT& root);

    /**
     * Calls 'variableCallback' on each Variable and `variableDefinitionCallback` on each
     * variable name defined via a Let or Lambda in the ABT.
     */
    static void walkVariables(
        const ABT& n,
        const std::function<void(const Variable&)>& variableCallback,
        const std::function<void(const ProjectionName&)>& variableDefinitionCallback =
            [](const ProjectionName&) {});

    ~VariableEnvironment();

    /**
     * Returns the defintion of 'var' in the ABT, regardless of the visibility of 'var' in the tree.
     * If there is no definition for 'var', returns an empty Definition.
     */
    Definition getDefinition(const Variable& var) const;

    bool hasFreeVariables() const;

    ProjectionNameSet freeVariableNames() const;

    /**
     * Returns the number of places in the ABT where there is a free Variable with name 'variable'.
     */
    size_t freeOccurences(const ProjectionName& variable) const;

    /**
     * Returns whether 'var' is guaranteed to be the last access to the projection to which it
     * refers.
     */
    bool isLastRef(const Variable& var) const;


private:
    std::unique_ptr<CollectedInfo> _info;

    // When '_lastRefs' is boost::none it means we did not collect that information,
    // and don't need to invalidate it on rebuild.
    boost::optional<LastRefsSet> _lastRefs;

    // A searchable map of Variables that is used by VariableEnvironment in order to
    // answer efficiently optimizer queries about Variables.
    std::unique_ptr<ResolvedVariablesMap> _resolvedVariablesMap;
};

}  // namespace mongo::abt
