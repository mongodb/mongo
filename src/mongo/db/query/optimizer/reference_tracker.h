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
     * Pointer to actual definition of variable.
     */
    ABT::reference_type definition;
};

namespace cascades {
class Memo;
}

struct CollectedInfo;
using DefinitionsMap = opt::unordered_map<ProjectionName, Definition>;

struct VariableCollectorResult {
    opt::unordered_set<const Variable*> _variables;
    // TODO: consider using a variable environment instance for this, but does not seem to be always
    // viable, especially with rewrites.
    opt::unordered_set<std::string> _definedVars;
};

class VariableEnvironment {
    VariableEnvironment(std::unique_ptr<CollectedInfo> info, const cascades::Memo* memo);

public:
    /**
     * Build the environment for the given ABT tree. The environment is valid as long as the tree
     * does not change. More specifically, if a variable defining node is removed from the tree then
     * the environment becomes stale and has to be rebuild.
     */
    static VariableEnvironment build(const ABT& root, const cascades::Memo* memo = nullptr);
    void rebuild(const ABT& root);

    ~VariableEnvironment();

    /**
     *
     */
    Definition getDefinition(const Variable* var) const;

    /**
     * We may revisit what we return from here.
     */
    ProjectionNameSet topLevelProjections() const;

    const DefinitionsMap& getDefinitions(const Node* node) const;

    /**
     * Per node projection names
     */
    ProjectionNameSet getProjections(const Node* node) const;
    bool hasDefinitions(const Node* node) const;

    const DefinitionsMap& getDefinitions(ABT::reference_type node) const;
    bool hasDefinitions(ABT::reference_type node) const;

    bool hasFreeVariables() const;
    size_t freeOccurences(const std::string& variable) const;

    bool isLastRef(const Variable* var) const;

    static VariableCollectorResult getVariables(const ABT& n);

private:
    std::unique_ptr<CollectedInfo> _info;
    const cascades::Memo* _memo{nullptr};
};

}  // namespace mongo::optimizer
