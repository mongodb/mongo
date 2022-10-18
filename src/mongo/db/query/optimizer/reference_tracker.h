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

#include "mongo/db/query/optimizer/cascades/memo_group_binder_interface.h"
#include "mongo/db/query/optimizer/node.h"


namespace mongo::optimizer {

/**
 * Every Variable ABT conceptually references to a point in the ABT tree. The pointed tree is the
 * definition of the variable.
 */
struct Definition {
    /**
     * Pointer to ABT that defines the variable. It can be any Node (e.g. ScanNode, EvaluationNode,
     * etc.) or Expr (e.g. let expression, lambda expression).
     */
    ABT::reference_type definedBy;

    /**
     * Pointer to actual definition of variable (i.e. the appropriate expression under a Binder).
     */
    ABT::reference_type definition;
};

struct CollectedInfo;
using DefinitionsMap = opt::unordered_map<ProjectionName, Definition>;

struct VariableCollectorResult {
    // The Variables referenced by the subtree.
    std::vector<std::reference_wrapper<const Variable>> _variables;
    // The names of locally-defined Variables. These aren't propagated up the tree during normal
    // variable resolution. Tracking these separately allows us to easily check, for example, which
    // variables are referenced in but not defined by the subtree (i.e. variables which should be
    // defined elsewhere in the ABT).
    opt::unordered_set<std::string> _definedVars;
};

/**
 * Helps enforce scoping and validity rules for definitions and Variable references.
 */
class VariableEnvironment {
    VariableEnvironment(std::unique_ptr<CollectedInfo> info,
                        const cascades::MemoGroupBinderInterface* memoInterface);

public:
    /**
     * Build the environment for the given ABT tree. The environment is valid as long as the tree
     * does not change. More specifically, if a variable defining node is removed from the tree then
     * the environment becomes stale and has to be rebuilt.
     */
    static VariableEnvironment build(
        const ABT& root, const cascades::MemoGroupBinderInterface* memoInterface = nullptr);
    void rebuild(const ABT& root);

    /**
     * Get information about Variables in the subtree rooted at 'n', including Variables referenced
     * by the subtree and locally-defined Variables.
     */
    static VariableCollectorResult getVariables(const ABT& n);

    ~VariableEnvironment();

    /**
     * Return the projections available to the ancestors of 'node' and the defintions for those
     * projections.
     */
    const DefinitionsMap& getDefinitions(ABT::reference_type node) const;
    const DefinitionsMap& getDefinitions(const Node& node) const;

    bool hasDefinitions(ABT::reference_type node) const;
    bool hasDefinitions(const Node& node) const;

    /**
     * Returns the projections available to the ancestors of 'node'.
     */
    ProjectionNameSet getProjections(const Node& node) const;
    ProjectionNameSet getProjections(ABT::reference_type node) const;

    /**
     * Returns the projections produced by the root of the ABT.
     */
    ProjectionNameSet topLevelProjections() const;

    /**
     * Returns the defintion of 'var' in the ABT, regardless of the visibility of 'var' in the tree.
     * If there is no definition for 'var', returns an empty Definition.
     */
    Definition getDefinition(const Variable& var) const;

    bool hasFreeVariables() const;

    opt::unordered_set<std::string> freeVariableNames() const;

    /**
     * Returns the number of places in the ABT where there is a free Variable with name 'variable'.
     */
    size_t freeOccurences(const std::string& variable) const;

    /**
     * Returns whether 'var' is guaranteed to be the last access to the projection to which it
     * refers.
     */
    bool isLastRef(const Variable& var) const;

private:
    std::unique_ptr<CollectedInfo> _info;

    // '_memoInterface' is required to track references in an ABT containing
    // MemoLogicalDelegatorNodes.
    const cascades::MemoGroupBinderInterface* _memoInterface{nullptr};
};

}  // namespace mongo::optimizer
