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

#include <iostream>

#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/reference_tracker.h"

namespace mongo::optimizer {

struct CollectedInfo {
    using VarRefsMap = opt::unordered_map<std::string, opt::unordered_map<const Variable*, bool>>;

    /**
     * All resolved variables so far
     */
    opt::unordered_map<const Variable*, Definition> useMap;

    /**
     * Definitions available for use in ancestor nodes (projections)
     */
    DefinitionsMap defs;

    /**
     * Free variables (i.e. so far not resolved)
     */
    opt::unordered_map<std::string, std::vector<const Variable*>> freeVars;

    /**
     * Per relational node projections.
     */
    opt::unordered_map<const Node*, DefinitionsMap> nodeDefs;

    /**
     * Support for tracking of the last local variable reference.
     */
    VarRefsMap varLastRefs;
    opt::unordered_set<const Variable*> lastRefs;

    /**
     * This is a destructive merge, the 'other' will be siphoned out.
     */
    void merge(CollectedInfo&& other) {
        // Incoming (other) info has some definitions. So let's try to resolve our free variables.
        if (!other.defs.empty() && !freeVars.empty()) {
            for (auto&& [name, def] : other.defs) {
                resolveFreeVars(name, def);
            }
        }

        // We have some definitions so let try to resolve other's free variables.
        if (!defs.empty() && !other.freeVars.empty()) {
            for (auto&& [name, def] : defs) {
                other.resolveFreeVars(name, def);
            }
        }

        useMap.merge(other.useMap);

        // It should be impossible to have duplicate Variable pointer so everything should be
        // copied.
        uassert(6624024, "use map is not empty", other.useMap.empty());

        defs.merge(other.defs);

        // Projection names are globally unique so everything should be copied.
        uassert(6624025, "duplicate projections", other.defs.empty());

        for (auto&& [name, vars] : other.freeVars) {
            auto& v = freeVars[name];
            v.insert(v.end(), vars.begin(), vars.end());
        }
        other.freeVars.clear();

        nodeDefs.merge(other.nodeDefs);

        // It should be impossible to have duplicate Node pointer so everything should be
        // copied.
        uassert(6624026, "duplicate nodes", other.nodeDefs.empty());

        // Merge last references.
        mergeLastRefs(std::move(other.varLastRefs));
        lastRefs.merge(other.lastRefs);
        uassert(6624027, "duplicate lastRefs", other.lastRefs.empty());
    }

    /**
     * Merges variable references from 'other' and adjust last references as needed.
     */
    void mergeLastRefs(VarRefsMap&& other) {
        mergeLastRefsImpl(std::move(other), false, true);
    }

    /**
     * Merges variable references from 'other' but keeps the last references from 'this'; i.e. it
     * resets the 'other' side.
     */
    void mergeKeepLastRefs(VarRefsMap&& other) {
        mergeLastRefsImpl(std::move(other), true, false);
    }

    /**
     * Merges variable references from 'other' and keeps the last references from both sides.
     */
    void unionLastRefs(VarRefsMap&& other) {
        mergeLastRefsImpl(std::move(other), false, false);
    }

    void mergeLastRefsImpl(VarRefsMap&& other, bool resetOther, bool resetBoth) {
        for (auto otherIt = other.begin(), end = other.end(); otherIt != end;) {
            if (auto localIt = varLastRefs.find(otherIt->first); localIt != varLastRefs.end()) {
                // This variable is referenced in both sets and we resetOther when adjust it
                // accordingly.
                if (resetOther) {
                    for (auto& [k, isLastRef] : otherIt->second) {
                        isLastRef = false;
                    }
                }

                // Merge the maps.
                localIt->second.merge(otherIt->second);

                // This variable is referenced in both sets so it will be marked as NOT last if
                // resetBoth is set.
                if (resetBoth) {
                    for (auto& [k, isLastRef] : localIt->second) {
                        isLastRef = false;
                    }
                }
                other.erase(otherIt++);
            } else {
                ++otherIt;
            }
        }
        varLastRefs.merge(other);
        uassert(6624098, "varLastRefs must be empty", other.empty());
    }

    /**
     * Records collected last variable references for a specific variable.
     */
    void finalizeLastRefs(const std::string& name) {
        if (auto it = varLastRefs.find(name); it != varLastRefs.end()) {
            for (auto& [var, isLastRef] : it->second) {
                if (isLastRef) {
                    lastRefs.emplace(var);
                }
            }

            // After the finalization the map is not needed anymore.
            varLastRefs.erase(it);
        }
    }

    /**
     * This is a destructive merge, the 'others' will be siphoned out.
     */
    void merge(std::vector<CollectedInfo>&& others) {
        for (auto& other : others) {
            merge(std::move(other));
        }
    }

    /**
     * A special merge asserting that the 'other' has no defined projections. Expressions do not
     * project anything, only Nodes do.
     *
     * We still have to track free variables though.
     */
    void mergeNoDefs(CollectedInfo&& other) {
        other.assertEmptyDefs();
        merge(std::move(other));
    }

    static ProjectionNameSet getProjections(const DefinitionsMap& defs) {
        ProjectionNameSet result;

        for (auto&& [k, v] : defs) {
            result.emplace(k);
        }
        return result;
    }

    ProjectionNameSet getProjections() const {
        return getProjections(defs);
    }

    void resolveFreeVars(const ProjectionName& name, const Definition& def) {
        if (auto it = freeVars.find(name); it != freeVars.end()) {
            for (const auto var : it->second) {
                useMap.emplace(var, def);
            }
            freeVars.erase(it);
        }
    }

    void assertEmptyDefs() {
        uassert(6624028, "Definitions must be empty", defs.empty());
    }
};

/**
 * Collect all Variables into a set.
 */
class VariableCollector {
public:
    template <typename T, typename... Ts>
    void transport(const T& /*op*/, Ts&&... /*ts*/) {}

    void transport(const Variable& op) {
        _result._variables.emplace(&op);
    }

    void transport(const LambdaAbstraction& op, const ABT& /*bind*/) {
        _result._definedVars.insert(op.varName());
    }

    void transport(const Let& op, const ABT& /*bind*/, const ABT& /*expr*/) {
        _result._definedVars.insert(op.varName());
    }

    static VariableCollectorResult collect(const ABT& n) {
        VariableCollector collector;
        collector.collectInternal(n);
        return std::move(collector._result);
    }

private:
    void collectInternal(const ABT& n) {
        algebra::transport<false>(n, *this);
    }

    VariableCollectorResult _result;
};

struct Collector {
    explicit Collector(const cascades::Memo* memo) : _memo(memo) {}

    template <typename T, typename... Ts>
    CollectedInfo transport(const ABT&, const T& op, Ts&&... ts) {
        CollectedInfo result{};
        (result.merge(std::forward<Ts>(ts)), ...);

        if constexpr (std::is_base_of_v<Node, T>) {
            result.nodeDefs[&op] = result.defs;
        }

        return result;
    }

    CollectedInfo transport(const ABT& n, const Variable& variable) {
        CollectedInfo result{};

        // Every variable starts as a free variable until it is resolved.
        result.freeVars[variable.name()].push_back(&variable);

        // Similarly, every variable starts as the last referencee until proven otherwise.
        result.varLastRefs[variable.name()].emplace(&variable, true);

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const Let& let,
                            CollectedInfo bindResult,
                            CollectedInfo inResult) {
        CollectedInfo result{};

        inResult.mergeKeepLastRefs(std::move(bindResult.varLastRefs));
        inResult.finalizeLastRefs(let.varName());

        result.merge(std::move(bindResult));

        // Local variables are not part of projections (i.e. we do not track them in defs) so
        // resolve any free variables manually.
        inResult.resolveFreeVars(let.varName(), Definition{n.ref(), let.bind().ref()});
        result.merge(std::move(inResult));

        return result;
    }

    CollectedInfo transport(const ABT& n, const LambdaAbstraction& lam, CollectedInfo inResult) {
        CollectedInfo result{};

        inResult.finalizeLastRefs(lam.varName());
        // Local variables are not part of projections (i.e. we do not track them in defs) so
        // resolve any free variables manually.
        inResult.resolveFreeVars(lam.varName(), Definition{n.ref(), ABT::reference_type{}});
        result.merge(std::move(inResult));

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const If&,
                            CollectedInfo condResult,
                            CollectedInfo thenResult,
                            CollectedInfo elseResult) {

        CollectedInfo result{};


        result.unionLastRefs(std::move(thenResult.varLastRefs));
        result.unionLastRefs(std::move(elseResult.varLastRefs));
        result.mergeKeepLastRefs(std::move(condResult.varLastRefs));

        result.merge(std::move(condResult));
        result.merge(std::move(thenResult));
        result.merge(std::move(elseResult));

        return result;
    }

    static CollectedInfo collectForScan(const ABT& n,
                                        const Node& node,
                                        const ExpressionBinder& binder,
                                        CollectedInfo refs) {
        CollectedInfo result{};

        result.mergeNoDefs(std::move(refs));

        for (size_t i = 0; i < binder.names().size(); i++) {
            result.defs[binder.names()[i]] = Definition{n.ref(), binder.exprs()[i].ref()};
        }
        result.nodeDefs[&node] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n, const ScanNode& node, CollectedInfo /*bindResult*/) {
        return collectForScan(n, node, node.binder(), {});
    }

    CollectedInfo transport(const ABT& n, const ValueScanNode& node, CollectedInfo /*bindResult*/) {
        return collectForScan(n, node, node.binder(), {});
    }

    CollectedInfo transport(const ABT& n,
                            const PhysicalScanNode& node,
                            CollectedInfo /*bindResult*/) {
        return collectForScan(n, node, node.binder(), {});
    }

    CollectedInfo transport(const ABT& n, const IndexScanNode& node, CollectedInfo /*bindResult*/) {
        return collectForScan(n, node, node.binder(), {});
    }

    CollectedInfo transport(const ABT& n,
                            const SeekNode& node,
                            CollectedInfo /*bindResult*/,
                            CollectedInfo refResult) {
        return collectForScan(n, node, node.binder(), std::move(refResult));
    }

    CollectedInfo transport(const ABT& n,
                            const MemoLogicalDelegatorNode& memoLogicalDelegatorNode) {
        CollectedInfo result{};

        uassert(6624029, "Uninitialized memo", _memo);

        auto& group = _memo->getGroup(memoLogicalDelegatorNode.getGroupId());

        auto& projectionNames = group.binder().names();
        auto& projections = group.binder().exprs();
        for (size_t i = 0; i < projectionNames.size(); i++) {
            result.defs[projectionNames.at(i)] = Definition{n.ref(), projections[i].ref()};
        }

        result.nodeDefs[&memoLogicalDelegatorNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const EvaluationNode& evaluationNode,
                            CollectedInfo childResult,
                            CollectedInfo exprResult) {
        CollectedInfo result{};

        // Make the definition available upstream.
        uassert(6624030,
                str::stream() << "Cannot overwrite project " << evaluationNode.getProjectionName(),
                childResult.defs.count(evaluationNode.getProjectionName()) == 0);

        result.merge(std::move(childResult));
        result.mergeNoDefs(std::move(exprResult));

        result.defs[evaluationNode.getProjectionName()] =
            Definition{n.ref(), evaluationNode.getProjection().ref()};

        result.nodeDefs[&evaluationNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const SargableNode& node,
                            CollectedInfo childResult,
                            CollectedInfo bindResult,
                            CollectedInfo /*refResult*/) {
        CollectedInfo result{};

        result.merge(std::move(childResult));
        result.mergeNoDefs(std::move(bindResult));

        const auto& projectionNames = node.binder().names();
        const auto& projections = node.binder().exprs();
        for (size_t i = 0; i < projectionNames.size(); i++) {
            result.defs[projectionNames.at(i)] = Definition{n.ref(), projections[i].ref()};
        }

        result.nodeDefs[&node] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const RIDIntersectNode& node,
                            CollectedInfo leftChildResult,
                            CollectedInfo rightChildResult) {
        CollectedInfo result{};

        rightChildResult.defs.erase(node.getScanProjectionName());

        result.merge(std::move(leftChildResult));
        result.merge(std::move(rightChildResult));

        result.nodeDefs[&node] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const BinaryJoinNode& binaryJoinNode,
                            CollectedInfo leftChildResult,
                            CollectedInfo rightChildResult,
                            CollectedInfo filterResult) {
        CollectedInfo result{};

        const ProjectionNameSet& correlatedProjNames =
            binaryJoinNode.getCorrelatedProjectionNames();
        {
            const ProjectionNameSet& leftProjections = leftChildResult.getProjections();
            for (const ProjectionName& boundProjectionName : correlatedProjNames) {
                uassert(6624099,
                        "Correlated projections must exist in left child.",
                        leftProjections.find(boundProjectionName) != leftProjections.cend());
            }
        }

        result.merge(std::move(leftChildResult));

        for (const ProjectionName& correlatedProjName : correlatedProjNames) {
            rightChildResult.resolveFreeVars(correlatedProjName,
                                             result.defs.at(correlatedProjName));
        }
        result.merge(std::move(rightChildResult));

        result.mergeNoDefs(std::move(filterResult));

        result.nodeDefs[&binaryJoinNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const UnionNode& unionNode,
                            std::vector<CollectedInfo> childResults,
                            CollectedInfo bindResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        const auto& names = unionNode.binder().names();

        refsResult.assertEmptyDefs();

        // Merge children but disregard any defined projections.
        // Note that refsResult follows the structure as built by buildUnionReferences.
        size_t counter = 0;
        for (auto& u : childResults) {
            // Manually copy and resolve references of specific child.
            for (const auto& name : names) {
                uassert(6624031, "Union projection does not exist", u.defs.count(name) != 0);
                u.useMap.emplace(refsResult.freeVars[name][counter], u.defs[name]);
            }
            u.defs.clear();
            result.merge(std::move(u));
            ++counter;
        }

        result.mergeNoDefs(std::move(bindResult));

        // Propagate union projections.
        const auto& defs = unionNode.binder().exprs();
        for (size_t idx = 0; idx < names.size(); ++idx) {
            result.defs[names[idx]] = Definition{n.ref(), defs[idx].ref()};
        }

        result.nodeDefs[&unionNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const GroupByNode& groupNode,
                            CollectedInfo childResult,
                            CollectedInfo bindAggResult,
                            CollectedInfo refsAggResult,
                            CollectedInfo bindGbResult,
                            CollectedInfo refsGbResult) {
        CollectedInfo result{};

        // First resolve all variables from the inside point of view; i.e. agg expressions and group
        // by expressions reference variables from the input child.
        result.merge(std::move(refsAggResult));
        result.merge(std::move(refsGbResult));
        // Make a copy of 'childResult' as we need it later and 'merge' is destructive.
        result.merge(CollectedInfo{childResult});

        // GroupBy completely masks projected variables; i.e. outside expressions cannot reach
        // inside the groupby. We will create a brand new set of projections from aggs and gbs here.
        result.defs.clear();

        const auto& aggs = groupNode.getAggregationProjectionNames();
        const auto& gbs = groupNode.getGroupByProjectionNames();
        for (size_t idx = 0; idx < aggs.size(); ++idx) {
            uassert(6624032,
                    "Aggregation overwrites a child projection",
                    childResult.defs.count(aggs[idx]) == 0);
            result.defs[aggs[idx]] =
                Definition{n.ref(), groupNode.getAggregationProjections()[idx].ref()};
        }

        for (size_t idx = 0; idx < gbs.size(); ++idx) {
            uassert(6624033,
                    "Group-by projection does not exist",
                    childResult.defs.count(gbs[idx]) != 0);
            result.defs[gbs[idx]] =
                Definition{n.ref(), groupNode.getGroupByProjections()[idx].ref()};
        }

        result.mergeNoDefs(std::move(bindAggResult));
        result.mergeNoDefs(std::move(bindGbResult));

        result.nodeDefs[&groupNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const UnwindNode& unwindNode,
                            CollectedInfo childResult,
                            CollectedInfo bindResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        // First resolve all variables from the inside point of view.
        result.merge(std::move(refsResult));
        result.merge(std::move(childResult));

        const auto& name = unwindNode.getProjectionName();
        uassert(6624034, "Unwind projection does not exist", result.defs.count(name) != 0);

        // Redefine unwind projection.
        result.defs[name] = Definition{n.ref(), unwindNode.getProjection().ref()};
        // Define unwind PID.
        result.defs[unwindNode.getPIDProjectionName()] =
            Definition{n.ref(), unwindNode.getPIDProjection().ref()};

        result.mergeNoDefs(std::move(bindResult));

        result.nodeDefs[&unwindNode] = result.defs;

        return result;
    }

    CollectedInfo collect(const ABT& n) {
        return algebra::transport<true>(n, *this);
    }

private:
    const cascades::Memo* _memo;
};

VariableEnvironment VariableEnvironment::build(const ABT& root, const cascades::Memo* memo) {

    Collector c(memo);
    auto info = std::make_unique<CollectedInfo>(c.collect(root));

    // std::cout << "useMap size " << info.useMap.size() << "\n";
    // std::cout << "defs size " << info.defs.size() << "\n";
    // std::cout << "freeVars size " << info.freeVars.size() << "\n";

    return VariableEnvironment{std::move(info), memo};
}

void VariableEnvironment::rebuild(const ABT& root) {
    _info = std::make_unique<CollectedInfo>(Collector{_memo}.collect(root));
}

VariableEnvironment::VariableEnvironment(std::unique_ptr<CollectedInfo> info,
                                         const cascades::Memo* memo)
    : _info(std::move(info)), _memo(memo) {}

VariableEnvironment::~VariableEnvironment() {}

Definition VariableEnvironment::getDefinition(const Variable* var) const {
    auto it = _info->useMap.find(var);
    if (it == _info->useMap.end()) {
        return Definition();
    }

    return it->second;
}

const DefinitionsMap& VariableEnvironment::getDefinitions(const Node* node) const {
    auto it = _info->nodeDefs.find(node);
    uassert(6624035, "node does not exist", it != _info->nodeDefs.end());

    return it->second;
}

bool VariableEnvironment::hasDefinitions(const Node* node) const {
    return _info->nodeDefs.find(node) != _info->nodeDefs.cend();
}

ProjectionNameSet VariableEnvironment::getProjections(const Node* node) const {
    return CollectedInfo::getProjections(getDefinitions(node));
}

const DefinitionsMap& VariableEnvironment::getDefinitions(ABT::reference_type node) const {
    uassert(6624036, "Invalid node type", node.is<Node>());
    return getDefinitions(node.cast<Node>());
}

bool VariableEnvironment::hasDefinitions(ABT::reference_type node) const {
    uassert(6624037, "Invalid node type", node.is<Node>());
    return hasDefinitions(node.cast<Node>());
}

ProjectionNameSet VariableEnvironment::topLevelProjections() const {
    return _info->getProjections();
}

bool VariableEnvironment::hasFreeVariables() const {
    return !_info->freeVars.empty();
}

size_t VariableEnvironment::freeOccurences(const std::string& variable) const {
    auto it = _info->freeVars.find(variable);
    if (it == _info->freeVars.end()) {
        return 0;
    }

    return it->second.size();
}

bool VariableEnvironment::isLastRef(const Variable* var) const {
    if (_info->lastRefs.count(var)) {
        return true;
    }

    return false;
}

VariableCollectorResult VariableEnvironment::getVariables(const ABT& n) {
    return VariableCollector::collect(n);
}

}  // namespace mongo::optimizer
