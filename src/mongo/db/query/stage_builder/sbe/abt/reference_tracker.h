// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/util/modules.h"

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
