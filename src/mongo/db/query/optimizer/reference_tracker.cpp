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

#include "mongo/db/query/optimizer/reference_tracker.h"


namespace mongo::optimizer {

struct CollectedInfo {
    using VarRefsMap = ProjectionNameMap<opt::unordered_map<const Variable*, bool>>;

    /**
     * All resolved variables so far, regardless of visibility in the ABT.
     */
    opt::unordered_map<const Variable*, Definition> useMap;

    /**
     * Current definitions available for use in ancestor nodes (projections).
     */
    DefinitionsMap defs;

    /**
     * All free variables (i.e. so far not resolved) seen so far, regardless of visibility in the
     * ABT. Maps from projection name to all Variable instances referencing that name. Variables
     * move from 'freeVars' to 'useMap' when they are resolved.
     */
    ProjectionNameMap<std::vector<std::reference_wrapper<const Variable>>> freeVars;

    /**
     * Maps from a node to the definitions (projections) available for use in its ancestor nodes.
     */
    opt::unordered_map<const Node*, DefinitionsMap> nodeDefs;

    /**
     * Support for tracking last local variable reference. A Variable moves from 'varLastRefs' to
     * 'lastRefs' while building the environment when we are sure it is a last reference.
     */
    VarRefsMap varLastRefs;
    opt::unordered_set<const Variable*> lastRefs;

    /**
     * This is a destructive merge, the 'other' will be siphoned out.
     */
    template <bool resolveFreeVarsWithOther = true>
    void merge(CollectedInfo&& other) {
        if constexpr (resolveFreeVarsWithOther) {
            // Incoming (other) info has some definitions. So let's try to resolve our free
            // variables.
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
        }

        // It should be impossible to have duplicate Variable pointer so every Variable should be
        // moved from other.
        useMap.merge(other.useMap);
        tassert(6624024, "Found a duplicate Variable pointer", other.useMap.empty());

        // There should not be two projections of the same name propagated up by a single operator,
        // so every definition should be moved from other.
        defs.merge(other.defs);
        tassert(6624025, "Found a duplicate projection name", other.defs.empty());

        for (auto&& [name, vars] : other.freeVars) {
            auto& v = freeVars[name];
            v.insert(v.end(), vars.begin(), vars.end());
        }
        other.freeVars.clear();

        // It should be impossible to have duplicate Node pointer so every Node should be moved from
        // other.
        nodeDefs.merge(other.nodeDefs);
        tassert(6624026, "Found a duplicate Node pointer", other.nodeDefs.empty());

        // Merge last references.
        mergeLastRefs(std::move(other.varLastRefs));
        lastRefs.merge(other.lastRefs);
        tassert(6624027, "Found a duplicate Variable pointer in lastRefs", other.lastRefs.empty());
    }

    /**
     * Merges variable references from 'other' and adjust last references as needed; i.e. it resets
     * any last ref to false if it appears on both sides.
     */
    void mergeLastRefs(VarRefsMap&& other) {
        mergeLastRefsImpl(std::move(other), false, true);
    }

    /**
     * Merges variable references from 'other' but only keeps the last references from 'this'; i.e.
     * it resets the 'other' side.
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
                // This variable is referenced in both sets. If requested, set other's last refs to
                // false.
                if (resetOther) {
                    for (auto& [k, isLastRef] : otherIt->second) {
                        isLastRef = false;
                    }
                }

                // Merge the maps.
                localIt->second.merge(otherIt->second);

                // This variable is referenced in both sets. If requested, set all last refs for the
                // variable to false.
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
        tassert(6624098, "varLastRefs must be empty", other.empty());
    }

    /**
     * Records collected last variable references for a specific variable. Should only be called
     * when the variable is guaranteed not to be referenced again in the ABT.
     */
    void finalizeLastRefs(const ProjectionName& name) {
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

    /**
     * Resolve any free Variables matching the given the name with the corresponding definition.
     */
    void resolveFreeVars(const ProjectionName& name, const Definition& def) {
        if (auto it = freeVars.find(name); it != freeVars.end()) {
            for (const Variable& var : it->second) {
                useMap.emplace(&var, def);
            }
            freeVars.erase(it);
        }
    }

    void assertEmptyDefs() {
        tassert(6624028, "Definitions must be empty", defs.empty());
    }
};

/**
 * Walks over all variables in the ABT and calls a callback for each variable.
 */
class VariableTransporter {
public:
    VariableTransporter(
        const std::function<void(const Variable&)>& variableCallback,
        const std::function<void(const ProjectionName&)>& variableDefinitionCallback)
        : _variableCallback(variableCallback),
          _variableDefinitionCallback(variableDefinitionCallback) {}

    template <typename T, typename... Ts>
    void transport(const T& /*op*/, Ts&&... /*ts*/) {}

    void transport(const Variable& op) {
        _variableCallback(op);
    }

    void transport(const LambdaAbstraction& op, const ABT& /*bind*/) {
        _variableDefinitionCallback(op.varName());
    }

    void transport(const Let& op, const ABT& /*bind*/, const ABT& /*expr*/) {
        _variableDefinitionCallback(op.varName());
    }

private:
    // Callback used on each Variable in the ABT.
    const std::function<void(const Variable&)>& _variableCallback;

    // Callback used on any defined variable name (via a Let or Lambda) in the ABT.
    const std::function<void(const ProjectionName&)>& _variableDefinitionCallback;
};

struct Collector {
    explicit Collector(const cascades::MemoGroupBinderInterface* memoInterface)
        : _memoInterface(memoInterface) {}

    template <typename T, typename... Ts>
    CollectedInfo transport(const ABT&, const T& op, Ts&&... ts) {
        static_assert(!std::is_base_of_v<Node, T>, "Nodes must implement reference tracking");

        // The default behavior resolves free variables, merges known definitions and propagates
        // them up unmodified.
        CollectedInfo result{};
        (result.merge(std::forward<Ts>(ts)), ...);

        return result;
    }

    CollectedInfo transport(const ABT& n, const Variable& variable) {
        CollectedInfo result{};

        // Every variable starts as a free variable until it is resolved.
        result.freeVars[variable.name()].push_back(variable);

        // Similarly, every variable starts as the last reference until proven otherwise.
        result.varLastRefs[variable.name()].emplace(&variable, true);

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const Let& let,
                            CollectedInfo bindResult,
                            CollectedInfo inResult) {
        CollectedInfo result{};

        // The 'in' portion of the Let will execute after the bind, so its last refs should be kept
        // over the last refs from the bind. Then, its safe to finalize the last ref for the
        // variable defined by the Let here (we know it can't be referenced elsewhere in the ABT).
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

        // As in the Let case, we can finalize the last ref for the local variable.
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

        // Only one of the 'then' or 'else' will be executed, so it's safe to union the last refs.
        // Since the condition will be executed before either of the then/else, its last refs should
        // be reset if there's a collision.
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

        // 'refs' should just track references to projections from any children of a Scan/Seek.
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

    CollectedInfo transport(const ABT& n, const CoScanNode& node) {
        CollectedInfo result{};
        result.nodeDefs[&node] = result.defs;
        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const MemoLogicalDelegatorNode& memoLogicalDelegatorNode) {
        CollectedInfo result{};

        tassert(6624029, "Uninitialized memo interface", _memoInterface);
        const auto& binder =
            _memoInterface->getBinderForGroup(memoLogicalDelegatorNode.getGroupId());

        auto& projectionNames = binder.names();
        auto& projections = binder.exprs();
        for (size_t i = 0; i < projectionNames.size(); i++) {
            result.defs[projectionNames.at(i)] = Definition{n.ref(), projections[i].ref()};
        }

        result.nodeDefs[&memoLogicalDelegatorNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n, const MemoPhysicalDelegatorNode& node) {
        tasserted(7088004, "Should not be seeing memo physical delegator in this context");
    }

    CollectedInfo transport(const ABT& n,
                            const FilterNode& filterNode,
                            CollectedInfo childResult,
                            CollectedInfo exprResult) {
        CollectedInfo result{};
        result.merge(std::move(childResult));
        result.mergeNoDefs(std::move(exprResult));
        result.nodeDefs[&filterNode] = result.defs;
        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const EvaluationNode& evaluationNode,
                            CollectedInfo childResult,
                            CollectedInfo exprResult) {
        CollectedInfo result{};

        tassert(6624030,
                str::stream() << "Cannot overwrite project " << evaluationNode.getProjectionName(),
                childResult.defs.count(evaluationNode.getProjectionName()) == 0);

        result.merge(std::move(childResult));
        result.mergeNoDefs(std::move(exprResult));

        // Make the definition available upstream.
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

    template <class T>
    CollectedInfo handleRIDNodeReferences(const T& node,
                                          CollectedInfo leftChildResult,
                                          CollectedInfo rightChildResult) {
        CollectedInfo result{};

        // This is a special case where both children of 'node' have a definition for the scan
        // projection. Remove the definition from one side to avoid running into the conflict of two
        // projections with the same name during the merge step below.
        rightChildResult.defs.erase(node.getScanProjectionName());

        result.merge(std::move(leftChildResult));
        result.merge<false /*resolveFreeVarsWithOther*/>(std::move(rightChildResult));

        result.nodeDefs[&node] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const RIDIntersectNode& node,
                            CollectedInfo leftChildResult,
                            CollectedInfo rightChildResult) {
        return handleRIDNodeReferences(
            node, std::move(leftChildResult), std::move(rightChildResult));
    }

    CollectedInfo transport(const ABT& n,
                            const RIDUnionNode& node,
                            CollectedInfo leftChildResult,
                            CollectedInfo rightChildResult) {
        // TODO SERVER-69026 should determine how the reference tracker for RIDUnionNode will work.
        return handleRIDNodeReferences(
            node, std::move(leftChildResult), std::move(rightChildResult));
    }

    template <class T>
    CollectedInfo handleJoinWithCorrelatedProjs(const T& node,
                                                CollectedInfo leftChildResult,
                                                CollectedInfo rightChildResult,
                                                CollectedInfo filterResult) {
        CollectedInfo result{};

        // Note correlated projections might be coming either from the left child or from the
        // parent.
        const ProjectionNameSet& correlatedProjNames = node.getCorrelatedProjectionNames();

        result.merge(std::move(leftChildResult));

        if (!result.defs.empty() && !rightChildResult.freeVars.empty()) {
            // Manually resolve free variables in the right child using the correlated variables
            // from the left child.
            for (auto&& [name, def] : result.defs) {
                if (correlatedProjNames.count(name) > 0) {
                    rightChildResult.resolveFreeVars(name, def);
                }
            }
        }

        // Do not resolve further free variables. We also need to propagate the right child
        // projections here, since these may be useful to ancestor nodes.
        result.merge<false /*resolveFreeVarsWithOther*/>(std::move(rightChildResult));

        result.mergeNoDefs(std::move(filterResult));

        result.nodeDefs[&node] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const BinaryJoinNode& binaryJoinNode,
                            CollectedInfo leftChildResult,
                            CollectedInfo rightChildResult,
                            CollectedInfo filterResult) {
        return handleJoinWithCorrelatedProjs<BinaryJoinNode>(binaryJoinNode,
                                                             std::move(leftChildResult),
                                                             std::move(rightChildResult),
                                                             std::move(filterResult));
    }

    CollectedInfo transport(const ABT& n,
                            const NestedLoopJoinNode& nestedLoopJoinNode,
                            CollectedInfo leftChildResult,
                            CollectedInfo rightChildResult,
                            CollectedInfo filterResult) {
        return handleJoinWithCorrelatedProjs<NestedLoopJoinNode>(nestedLoopJoinNode,
                                                                 std::move(leftChildResult),
                                                                 std::move(rightChildResult),
                                                                 std::move(filterResult));
    }

    CollectedInfo transport(const ABT& n,
                            const HashJoinNode& hashJoinNode,
                            CollectedInfo leftChildResult,
                            CollectedInfo rightChildResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        result.merge(std::move(leftChildResult));
        // Do not resolve further free variables.
        result.merge<false /*resolveFreeVarsWithOther*/>(std::move(rightChildResult));
        result.mergeNoDefs(std::move(refsResult));

        result.nodeDefs[&hashJoinNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const MergeJoinNode& mergeJoinNode,
                            CollectedInfo leftChildResult,
                            CollectedInfo rightChildResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        result.merge(std::move(leftChildResult));
        // Do not resolve further free variables.
        result.merge<false /*resolveFreeVarsWithOther*/>(std::move(rightChildResult));
        result.mergeNoDefs(std::move(refsResult));

        result.nodeDefs[&mergeJoinNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const SortedMergeNode& node,
                            std::vector<CollectedInfo> childResults,
                            CollectedInfo bindResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        const auto& names = node.binder().names();

        refsResult.assertEmptyDefs();

        // Merge children but disregard any defined projections.
        // Note that refsResult follows the structure as built by buildUnionTypeReferences, meaning
        // it contains a free variable for each name for each child of the sorted merge and no other
        // info.
        size_t counter = 0;
        for (auto& u : childResults) {
            // Manually copy and resolve references of specific child. We do this manually because
            // each Variable must be resolved by the appropriate child's definition.
            for (const auto& name : names) {
                tassert(7063706,
                        str::stream() << "SortedMerge projection does not exist: " << name,
                        u.defs.count(name) != 0);
                u.useMap.emplace(&refsResult.freeVars[name][counter].get(), u.defs[name]);
            }
            u.defs.clear();
            result.merge(std::move(u));
            ++counter;
        }

        result.mergeNoDefs(std::move(bindResult));

        // Propagate sorted merge projections. Note that these are the only defs propagated, since
        // we clear the child defs before merging above.
        const auto& defs = node.binder().exprs();
        for (size_t idx = 0; idx < names.size(); ++idx) {
            result.defs[names[idx]] = Definition{n.ref(), defs[idx].ref()};
        }

        result.nodeDefs[&node] = result.defs;

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
        // Note that refsResult follows the structure as built by buildUnionTypeReferences, meaning
        // it contains a free variable for each name for each child of the union and no other info.
        size_t counter = 0;
        for (auto& u : childResults) {
            // Manually copy and resolve references of specific child. We do this manually because
            // each Variable must be resolved by the appropriate child's definition.
            for (const auto& name : names) {
                tassert(6624031,
                        str::stream() << "Union projection does not exist:  " << name,
                        u.defs.count(name) != 0);
                u.useMap.emplace(&refsResult.freeVars[name][counter].get(), u.defs[name]);
            }
            u.defs.clear();
            result.merge(std::move(u));
            ++counter;
        }

        result.mergeNoDefs(std::move(bindResult));

        // Propagate union projections. Note that these are the only defs propagated, since we clear
        // the child defs before merging above.
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
            tassert(6624032,
                    "Aggregation overwrites a child projection",
                    childResult.defs.count(aggs[idx]) == 0);
            result.defs[aggs[idx]] =
                Definition{n.ref(), groupNode.getAggregationProjections()[idx].ref()};
        }

        for (size_t idx = 0; idx < gbs.size(); ++idx) {
            tassert(6624033,
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
        result.mergeNoDefs(std::move(refsResult));
        result.merge(std::move(childResult));

        const auto& name = unwindNode.getProjectionName();
        tassert(6624034,
                str::stream() << "Unwind projection does not exist: " << name,
                result.defs.count(name) != 0);

        // Redefine unwind projection.
        result.defs[name] = Definition{n.ref(), unwindNode.getProjection().ref()};
        // Define unwind PID.
        result.defs[unwindNode.getPIDProjectionName()] =
            Definition{n.ref(), unwindNode.getPIDProjection().ref()};

        result.mergeNoDefs(std::move(bindResult));

        result.nodeDefs[&unwindNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const UniqueNode& uniqueNode,
                            CollectedInfo childResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        result.merge(std::move(refsResult));
        result.merge(std::move(childResult));

        for (const auto& name : uniqueNode.getProjections()) {
            tassert(6624060,
                    str::stream() << "Unique projection does not exist: " << name,
                    result.defs.count(name) != 0);
        }

        result.nodeDefs[&uniqueNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const CollationNode& collationNode,
                            CollectedInfo childResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        result.mergeNoDefs(std::move(refsResult));
        result.merge(std::move(childResult));

        for (const auto& name : collationNode.getProperty().getAffectedProjectionNames()) {
            tassert(7088001,
                    str::stream() << "Collation projection does not exist: " << name,
                    result.defs.count(name) != 0);
        }

        result.nodeDefs[&collationNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const LimitSkipNode& limitSkipNode,
                            CollectedInfo childResult) {
        CollectedInfo result{};
        result.merge(std::move(childResult));
        result.nodeDefs[&limitSkipNode] = result.defs;
        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const ExchangeNode& exchangeNode,
                            CollectedInfo childResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        result.mergeNoDefs(std::move(refsResult));
        result.merge(std::move(childResult));

        for (const auto& name : exchangeNode.getProperty().getAffectedProjectionNames()) {
            tassert(7088002,
                    str::stream() << "Exchange projection does not exist: " << name,
                    result.defs.count(name) != 0);
        }

        result.nodeDefs[&exchangeNode] = result.defs;
        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const RootNode& rootNode,
                            CollectedInfo childResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        result.mergeNoDefs(std::move(refsResult));
        result.merge(std::move(childResult));

        for (const auto& name : rootNode.getProperty().getAffectedProjectionNames()) {
            tassert(7088003,
                    str::stream() << "Root projection does not exist: " << name,
                    result.defs.count(name) != 0);
        }

        result.nodeDefs[&rootNode] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const SpoolProducerNode& node,
                            CollectedInfo childResult,
                            CollectedInfo filterResult,
                            CollectedInfo bindResult,
                            CollectedInfo refsResult) {
        CollectedInfo result{};

        result.merge(std::move(refsResult));
        result.merge(std::move(childResult));

        const auto& binder = node.binder();
        for (size_t i = 0; i < binder.names().size(); i++) {
            const auto& name = binder.names().at(i);
            tassert(6624138,
                    str::stream() << "Spool projection does not exist: " << name,
                    result.defs.count(name) != 0);

            // Redefine projection.
            result.defs[name] = Definition{n.ref(), binder.exprs()[i].ref()};
        }

        result.mergeNoDefs(std::move(bindResult));
        result.mergeNoDefs(std::move(filterResult));

        result.nodeDefs[&node] = result.defs;

        return result;
    }

    CollectedInfo transport(const ABT& n, const SpoolConsumerNode& node, CollectedInfo bindResult) {
        return collectForScan(n, node, node.binder(), {});
    }

    CollectedInfo collect(const ABT& n) {
        return algebra::transport<true>(n, *this);
    }

private:
    const cascades::MemoGroupBinderInterface* _memoInterface;
};

VariableEnvironment VariableEnvironment::build(
    const ABT& root, const cascades::MemoGroupBinderInterface* memoInterface) {
    Collector c(memoInterface);
    auto info = std::make_unique<CollectedInfo>(c.collect(root));

    return VariableEnvironment{std::move(info), memoInterface};
}

void VariableEnvironment::rebuild(const ABT& root) {
    _info = std::make_unique<CollectedInfo>(Collector{_memoInterface}.collect(root));
}

VariableEnvironment::VariableEnvironment(std::unique_ptr<CollectedInfo> info,
                                         const cascades::MemoGroupBinderInterface* memoInterface)
    : _info(std::move(info)), _memoInterface(memoInterface) {}

VariableEnvironment::~VariableEnvironment() {}

Definition VariableEnvironment::getDefinition(const Variable& var) const {
    auto it = _info->useMap.find(&var);
    if (it == _info->useMap.end()) {
        return Definition();
    }

    return it->second;
}

const DefinitionsMap& VariableEnvironment::getDefinitions(const Node& node) const {
    auto it = _info->nodeDefs.find(&node);
    tassert(6624035, "No definitions found for node", it != _info->nodeDefs.end());

    return it->second;
}

bool VariableEnvironment::hasDefinitions(const Node& node) const {
    return _info->nodeDefs.find(&node) != _info->nodeDefs.cend();
}

ProjectionNameSet VariableEnvironment::getProjections(const Node& node) const {
    return CollectedInfo::getProjections(getDefinitions(node));
}

ProjectionNameSet VariableEnvironment::getProjections(ABT::reference_type node) const {
    tassert(6199000, "Invalid node type", node.is<Node>());
    return CollectedInfo::getProjections(getDefinitions(*node.cast<Node>()));
}

const DefinitionsMap& VariableEnvironment::getDefinitions(ABT::reference_type node) const {
    tassert(6624036, "Invalid node type", node.is<Node>());
    return getDefinitions(*node.cast<Node>());
}

bool VariableEnvironment::hasDefinitions(ABT::reference_type node) const {
    tassert(6624037, "Invalid node type", node.is<Node>());
    return hasDefinitions(*node.cast<Node>());
}

ProjectionNameSet VariableEnvironment::topLevelProjections() const {
    return _info->getProjections();
}

bool VariableEnvironment::hasFreeVariables() const {
    return !_info->freeVars.empty();
}

ProjectionNameSet VariableEnvironment::freeVariableNames() const {
    ProjectionNameSet freeVarNames;
    for (auto&& [name, vars] : _info->freeVars) {
        freeVarNames.insert(name);
    }
    return freeVarNames;
}

size_t VariableEnvironment::freeOccurences(const ProjectionName& variable) const {
    auto it = _info->freeVars.find(variable);
    if (it == _info->freeVars.end()) {
        return 0;
    }

    return it->second.size();
}

bool VariableEnvironment::isLastRef(const Variable& var) const {
    return _info->lastRefs.count(&var) > 0;
}

void VariableEnvironment::walkVariables(
    const ABT& n,
    const std::function<void(const Variable&)>& variableCallback,
    const std::function<void(const ProjectionName&)>& variableDefinitionCallback) {
    VariableTransporter transporter(variableCallback, variableDefinitionCallback);
    algebra::transport<false>(n, transporter);
}


}  // namespace mongo::optimizer
