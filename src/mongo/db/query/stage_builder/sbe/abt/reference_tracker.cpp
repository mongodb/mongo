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

#include "mongo/db/query/stage_builder/sbe/abt/reference_tracker.h"

#include "mongo/db/query/algebra/operator.h"
#include "mongo/db/query/stage_builder/sbe/abt/containers.h"
#include "mongo/util/assert_util.h"

#include <utility>
#include <vector>


namespace mongo::abt {

/**
 * While analyzing an ABT tree via the Collector transport class, there is a need
 * for state that is 'global' for the duration of the analysis and is accessible to
 * all Collector::transport methods. This class represents such a state.
 * Notice:
 * This is a struct instead of simple 'using' because at least one more future task
 * will add more state to it - namely, SERVER-80954.
 */
struct CollectorState {
    /**
     * All resolved variables, regardless of visibility in the ABT.
     */
    std::unique_ptr<ResolvedVariablesMap> resolvedVariablesMap;
};

/**
 * Information collected by each Collector::transport method for each ABT node in a tree.
 * The Collector passes the CollectedInfo of a node's children to the parent's node
 * transport method, where the child/children CollectedInfo is typically merged into
 * the parent's CollectedInfo.
 */
struct CollectedInfo {
    CollectedInfo(CollectorState& collr) : collector(collr) {};

    using VarRefsMap = ProjectionNameMap<opt::unordered_map<const Variable*, bool>>;

    /**
     * All free variables (i.e. so far not resolved) seen so far, regardless of visibility in the
     * ABT. Maps from projection name to all Variable instances referencing that name. Variables
     * move from 'freeVars' to 'Collector::resolvedVariables' when they are resolved.
     */
    ProjectionNameMap<std::vector<std::reference_wrapper<const Variable>>> freeVars;

    /**
     * The collector transport class stores global information that is updated by some
     * CollectedInfo methods. Hence we need a pointer to the collector.
     */
    CollectorState& collector;

    /**
     * This is a destructive merge, the 'other' will be siphoned out.
     */
    void merge(CollectedInfo&& other) {
        for (auto&& [name, vars] : other.freeVars) {
            auto& v = freeVars[name];
            v.insert(v.end(), vars.begin(), vars.end());
        }
        other.freeVars.clear();
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
     * Resolve any free Variables matching the given the name with the corresponding definition.
     */
    void resolveFreeVars(const ProjectionName& name, const Definition& def) {
        if (auto it = freeVars.find(name); it != freeVars.end()) {
            for (const Variable& var : it->second) {
                collector.resolvedVariablesMap->emplace(&var, def);
            }
            freeVars.erase(it);
        }
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

    void transport(const MultiLet& op, const std::vector<abt::ABT>& /*nodes*/) {
        for (auto&& name : op.varNames()) {
            _variableDefinitionCallback(name);
        }
    }

private:
    // Callback used on each Variable in the ABT.
    const std::function<void(const Variable&)>& _variableCallback;

    // Callback used on any defined variable name (via a Let or Lambda) in the ABT.
    const std::function<void(const ProjectionName&)>& _variableDefinitionCallback;
};

struct Collector {
    explicit Collector() {
        collectorState.resolvedVariablesMap = std::make_unique<ResolvedVariablesMap>();
    }

    template <typename T, typename... Ts>
    CollectedInfo transport(const ABT&, const T& op, Ts&&... ts) {
        // The default behavior resolves free variables, merges known definitions and propagates
        // them up unmodified.
        CollectedInfo result{collectorState};
        (result.merge(std::forward<Ts>(ts)), ...);

        return result;
    }

    CollectedInfo transport(const ABT& n, const Variable& variable) {
        CollectedInfo result{collectorState};

        // Every variable starts as a free variable until it is resolved.
        result.freeVars[variable.name()].push_back(variable);

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const Let& let,
                            CollectedInfo bindResult,
                            CollectedInfo inResult) {
        CollectedInfo result{collectorState};

        result.merge(std::move(bindResult));

        // resolve any free variables manually.
        inResult.resolveFreeVars(let.varName(), Definition{n.ref(), let.bind().ref()});
        result.merge(std::move(inResult));

        return result;
    }

    CollectedInfo transport(const ABT& n,
                            const MultiLet& multiLet,
                            std::vector<CollectedInfo> results) {
        auto& result = results.back();

        for (int idx = multiLet.numBinds() - 1; idx >= 0; --idx) {
            result.resolveFreeVars(multiLet.varName(idx),
                                   Definition{n.ref(), multiLet.bind(idx).ref()});
            result.merge(std::move(results[idx]));
        }

        return result;
    }

    CollectedInfo transport(const ABT& n, const LambdaAbstraction& lam, CollectedInfo inResult) {
        CollectedInfo result{collectorState};

        // resolve any free variables manually.
        inResult.resolveFreeVars(lam.varName(), Definition{n.ref(), ABT::reference_type{}});
        result.merge(std::move(inResult));

        return result;
    }

    CollectedInfo collect(const ABT& n) {
        return algebra::transport<true>(n, *this);
    }
    /**
     * The collector transport class stores here global information that is updated by
     * some CollectedInfo methods. This object is passed to each CollectedInfo, so that
     * it can update the collectorState.
     */
    CollectorState collectorState;
};

/**
 * Finds Variable references that are safe to mark moveFrom in SBE. See 'LastRefsSet'.
 */
struct LastRefsTransporter {
    /**
     * Maps each name that occurs free to the set of its last references.
     * If a name has one or more free occurrences, but none are last, then the set is empty.
     * If a name has no free occurrences at all, then there won't be a map entry.
     *
     * This distinction is important when combining results from two subtrees.
     * For example, in 'let a = f(x) in g()', we know that 'x' does not occur in 'g()',
     * so the 'x' in 'f(x)' is a last reference. On other hand in 'let a = f(x) in g(x, x)',
     * 'x' does occur in 'g(x, x)', so the one in 'f(x)' is not a last reference--even if we don't
     * know which use of 'x' in 'g(x, x)' is last.
     */
    using Result = ProjectionNameMap<LastRefsSet>;

    explicit LastRefsTransporter(LastRefsSet& lastRefs) : _lastRefs(lastRefs) {}

    /**
     * Merge results, siphoning out r2 and putting results in r1.
     *
     * Variables that only occur on one side have their last-references preserved.
     * Variables that occur on both sides have their last-references cleared, because we don't know
     * which side is evaluated last.
     */
    static void merge(Result& result, Result& other) {
        mergeImpl(result, other, false /*resetOther*/, true /*resetBoth*/);
    }

    /**
     * Same as merge() but accepts a vector; this allows the generic transport() to handle Result
     * and vector<Result> uniformly.
     *
     * Both of these overloads make no assumptions about evaluation order: if a variable occurs in
     * more than one Result then all its last-references are cleared.
     */
    static void merge(Result& result, std::vector<Result>& others) {
        for (auto& other : others) {
            merge(result, other);
        }
    }

    /**
     * Like merge(), but assumes the left-hand side 'result' will be evaluated after 'other'.
     *
     * This means when a variable occurs on both sides, its last-references in 'result' can be
     * preserved.
     */
    static void mergeKeepLastRefs(Result& result, Result& other) {
        mergeImpl(result, other, true /*resetOther*/, false /*resetBoth*/);
    }

    /**
     * Merges variable references from 'other' and keeps the last-references from both sides.
     *
     * This is appropriate when neither side is evaluated after the other. For example the two
     * branches of a conditional.
     */
    static void unionLastRefs(Result& result, Result& other) {
        mergeImpl(result, other, false /*resetOther*/, false /*resetBoth*/);
    }

    /**
     * Combine all the occurrences from 'result' and 'other' into 'result'.
     *
     * The boolean flags control what to do when a variable occurs in both arguments:
     * - 'resetOther' means preserve the last-references of 'result' but clear the ones in 'other'.
     * - 'resetBoth' means clear the last-references flags of both 'result' and 'other'.
     *
     * If both flags are false then all last-references flags are preserved.
     */
    static void mergeImpl(Result& result, Result& other, bool resetOther, bool resetBoth) {
        for (auto otherIt = other.begin(), end = other.end(); otherIt != end;) {
            if (auto localIt = result.find(otherIt->first); localIt != result.end()) {
                // This variable is referenced in both sets.

                // If requested, stop treating occurrences in 'other' as last references.
                if (resetOther) {
                    otherIt->second.clear();
                }

                // Combine the last references from each side.
                // Each side may contribute zero or more.
                localIt->second.merge(otherIt->second);
                other.erase(otherIt++);

                // If requested, stop treating occurrences in the combined set as last references.
                if (resetBoth) {
                    localIt->second.clear();
                }
            } else {
                // This variable is only referenced in 'other', so preserve its last-references.
                // Preserve it by not erasing: we'll combine these not-erased entries at the end.
                ++otherIt;
            }
        }
        // Combine all the not-erased map entries.
        result.merge(other);
    }

    /**
     * Should be called once we know that we've seen all occurrences of a variable: removes all
     * information about this variable from 'result', and records the last-references in the global
     * set '_lastRefs'.
     */
    void finalizeLastRefs(Result& result, const ProjectionName& name) {
        if (auto it = result.find(name); it != result.end()) {
            _lastRefs.merge(it->second);
            result.erase(it);
        }
    }

    template <typename T>
    Result transport(const ABT&, const T& op, auto... ts) {
        Result result{};
        (merge(result, ts), ...);

        return result;
    }

    Result transport(const ABT& n, const Variable& variable) {
        Result result{};

        // Every variable starts as a last reference until proven otherwise.
        result[variable.name()].emplace(&variable);

        return result;
    }

    Result transport(const ABT& n, const Let& let, Result bindResult, Result inResult) {
        // The 'in' portion of the Let will execute after the bind, so its last refs should be kept
        // over the last refs from the bind. Then, it's safe to finalize the last ref for the
        // variable defined by the Let here (we know it can't be referenced elsewhere in the ABT).
        mergeKeepLastRefs(inResult, bindResult);
        finalizeLastRefs(inResult, let.varName());

        return inResult;
    }

    Result transport(const ABT& n, const MultiLet& multiLet, std::vector<Result> results) {
        auto& inResult = results.back();

        for (int idx = multiLet.numBinds() - 1; idx >= 0; --idx) {
            mergeKeepLastRefs(inResult, results[idx]);
            finalizeLastRefs(inResult, multiLet.varName(idx));
        }

        return inResult;
    }

    Result transport(const ABT& n, const LambdaAbstraction& lam, Result inResult) {
        // As in the Let case, we can finalize the last ref for the local variable.
        finalizeLastRefs(inResult, lam.varName());

        return inResult;
    }

    Result transport(
        const ABT& n, const If&, Result condResult, Result thenResult, Result elseResult) {
        Result result{};

        // Only one of the 'then' or 'else' will be executed, so it's safe to union the last refs.
        // Since the condition will be executed before either of the then/else, its last refs should
        // be reset if there's a collision.
        unionLastRefs(result, thenResult);
        unionLastRefs(result, elseResult);
        mergeKeepLastRefs(result, condResult);

        return result;
    }

    Result transport(const ABT& n, const Switch&, std::vector<Result> branchResults) {
        Result result{};

        // Only one of the 'then' or 'else' will be executed, so it's safe to union the last refs.
        // Since the condition will be executed before either of the then/else, its last refs should
        // be reset if there's a collision. We work backwards from the last condition to the first
        // one.
        size_t lastCond = branchResults.size() - 3;
        unionLastRefs(result, branchResults[lastCond + 1]);
        unionLastRefs(result, branchResults[lastCond + 2]);
        mergeKeepLastRefs(result, branchResults[lastCond]);
        while (lastCond >= 2) {
            lastCond -= 2;
            unionLastRefs(result, branchResults[lastCond + 1]);
            mergeKeepLastRefs(result, branchResults[lastCond]);
        }

        return result;
    }

    void collect(const ABT& n) {
        algebra::transport<true>(n, *this);
    }

private:
    /**
     * The set of Variable occurrences that are safe to move from.
     *
     * We only add to this map once a variable is resolved, because that's when we know we've seen
     * all of its uses.
     */
    LastRefsSet& _lastRefs;
};

VariableEnvironment VariableEnvironment::build(const ABT& root, bool computeLastRefs) {
    Collector c;
    auto info = std::make_unique<CollectedInfo>(c.collect(root));

    boost::optional<LastRefsSet> lastRefs;
    if (computeLastRefs) {
        lastRefs.emplace();
        LastRefsTransporter lrt(*lastRefs);
        lrt.collect(root);
    }

    return VariableEnvironment{
        std::move(info), std::move(lastRefs), std::move(c.collectorState.resolvedVariablesMap)};
}

void VariableEnvironment::rebuild(const ABT& root) {
    Collector c;
    _info = std::make_unique<CollectedInfo>(c.collect(root));

    if (_lastRefs) {
        _lastRefs->clear();
        LastRefsTransporter lrt(*_lastRefs);
        lrt.collect(root);
    }

    // Reset the Variable map to the newly computed one.
    _resolvedVariablesMap = std::move(c.collectorState.resolvedVariablesMap);
}

VariableEnvironment::VariableEnvironment(std::unique_ptr<CollectedInfo> info,
                                         boost::optional<LastRefsSet> lastRefs,
                                         std::unique_ptr<ResolvedVariablesMap> resVarMap)
    : _info(std::move(info)),
      _lastRefs(std::move(lastRefs)),
      _resolvedVariablesMap(std::move(resVarMap)) {}

VariableEnvironment::~VariableEnvironment() {}

Definition VariableEnvironment::getDefinition(const Variable& var) const {
    if (auto it = _resolvedVariablesMap->find(&var); it != _resolvedVariablesMap->cend()) {
        return it->second;
    }
    return {};
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
    return _lastRefs && _lastRefs->contains(&var);
}

void VariableEnvironment::walkVariables(
    const ABT& n,
    const std::function<void(const Variable&)>& variableCallback,
    const std::function<void(const ProjectionName&)>& variableDefinitionCallback) {
    VariableTransporter transporter(variableCallback, variableDefinitionCallback);
    algebra::transport<false>(n, transporter);
}


}  // namespace mongo::abt
