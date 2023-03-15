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

#include "mongo/db/query/optimizer/opt_phase_manager.h"

#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"

namespace mongo::optimizer {

OptPhaseManager::PhaseSet OptPhaseManager::_allRewrites = {OptPhase::ConstEvalPre,
                                                           OptPhase::PathFuse,
                                                           OptPhase::MemoSubstitutionPhase,
                                                           OptPhase::MemoExplorationPhase,
                                                           OptPhase::MemoImplementationPhase,
                                                           OptPhase::PathLower,
                                                           OptPhase::ConstEvalPost};

OptPhaseManager::OptPhaseManager(OptPhaseManager::PhaseSet phaseSet,
                                 PrefixId& prefixId,
                                 const bool requireRID,
                                 Metadata metadata,
                                 std::unique_ptr<CardinalityEstimator> explorationCE,
                                 std::unique_ptr<CardinalityEstimator> substitutionCE,
                                 std::unique_ptr<CostEstimator> costEstimator,
                                 PathToIntervalFn pathToInterval,
                                 ConstFoldFn constFold,
                                 const bool supportExplain,
                                 DebugInfo debugInfo,
                                 QueryHints queryHints)
    : _phaseSet(std::move(phaseSet)),
      _supportExplain(supportExplain),
      _debugInfo(std::move(debugInfo)),
      _hints(std::move(queryHints)),
      _metadata(std::move(metadata)),
      _memo(),
      _logicalPropsDerivation(std::make_unique<DefaultLogicalPropsDerivation>()),
      _explorationCE(std::move(explorationCE)),
      _substitutionCE(std::move(substitutionCE)),
      _costEstimator(std::move(costEstimator)),
      _pathToInterval(std::move(pathToInterval)),
      _constFold(std::move(constFold)),
      _physicalNodeId(),
      _postMemoPlan(),
      _requireRID(requireRID),
      _ridProjections(),
      _prefixId(prefixId) {
    uassert(6624093, "Cost derivation is null", _costEstimator);
    uassert(7088900, "Exploration CE is null", _explorationCE);
    uassert(7088901, "Substitution CE is null", _substitutionCE);

    for (const auto& entry : _metadata._scanDefs) {
        _ridProjections.emplace(entry.first, _prefixId.getNextId("rid"));
    }
}

static std::string generateFreeVarsAssertMsg(const VariableEnvironment& env) {
    str::stream os;
    bool first = true;
    for (const auto& name : env.freeVariableNames()) {
        if (first) {
            first = false;
        } else {
            os << ", ";
        }
        os << name;
    }
    return os;
}

template <OptPhase phase, class C>
void OptPhaseManager::runStructuralPhase(C instance, VariableEnvironment& env, ABT& input) {
    if (!hasPhase(phase)) {
        return;
    }

    for (int iterationCount = 0; instance.optimize(input); iterationCount++) {
        tassert(6808708,
                str::stream() << "Iteration limit exceeded while running the following phase: "
                              << OptPhaseEnum::toString[static_cast<int>(phase)] << ".",
                !_debugInfo.exceedsIterationLimit(iterationCount));
    }

    if (env.hasFreeVariables()) {
        tasserted(6808709, "Plan has free variables: " + generateFreeVarsAssertMsg(env));
    }
}

template <OptPhase phase1, OptPhase phase2, class C1, class C2>
void OptPhaseManager::runStructuralPhases(C1 instance1,
                                          C2 instance2,
                                          VariableEnvironment& env,
                                          ABT& input) {
    const bool hasPhase1 = hasPhase(phase1);
    const bool hasPhase2 = hasPhase(phase2);
    if (!hasPhase1 && !hasPhase2) {
        return;
    }

    bool changed = true;
    for (int iterationCount = 0; changed; iterationCount++) {
        // Iteration limit exceeded.
        tassert(6808700,
                str::stream() << "Iteration limit exceeded while running the following phases: "
                              << OptPhaseEnum::toString[static_cast<int>(phase1)] << ", "
                              << OptPhaseEnum::toString[static_cast<int>(phase2)] << ".",
                !_debugInfo.exceedsIterationLimit(iterationCount));


        changed = false;
        if (hasPhase1) {
            changed |= instance1.optimize(input);
        }
        if (hasPhase2) {
            changed |= instance2.optimize(input);
        }
    }

    if (env.hasFreeVariables()) {
        tasserted(6808701, "Plan has free variables: " + generateFreeVarsAssertMsg(env));
    }
}

void OptPhaseManager::runMemoLogicalRewrite(const OptPhase phase,
                                            VariableEnvironment& env,
                                            const LogicalRewriter::RewriteSet& rewriteSet,
                                            GroupIdType& rootGroupId,
                                            const bool runStandalone,
                                            std::unique_ptr<LogicalRewriter>& logicalRewriter,
                                            ABT& input) {
    if (!hasPhase(phase)) {
        return;
    }

    _memo.clear();
    const bool useSubstitutionCE = phase == OptPhase::MemoSubstitutionPhase;
    logicalRewriter =
        std::make_unique<LogicalRewriter>(_metadata,
                                          _memo,
                                          _prefixId,
                                          rewriteSet,
                                          _debugInfo,
                                          _hints,
                                          _pathToInterval,
                                          _constFold,
                                          *_logicalPropsDerivation,
                                          useSubstitutionCE ? *_substitutionCE : *_explorationCE);
    rootGroupId = logicalRewriter->addRootNode(input);

    if (runStandalone) {
        const bool fixPointRewritten = logicalRewriter->rewriteToFixPoint();
        tassert(6808702, "Logical writer failed to rewrite fix point.", fixPointRewritten);

        input = extractLatestPlan(_memo, rootGroupId);
        env.rebuild(input);
    }

    if (env.hasFreeVariables()) {
        tasserted(6808703, "Plan has free variables: " + generateFreeVarsAssertMsg(env));
    }
}

PlanExtractorResult OptPhaseManager::runMemoPhysicalRewrite(
    const OptPhase phase,
    VariableEnvironment& env,
    const GroupIdType rootGroupId,
    const bool includeRejected,
    std::unique_ptr<LogicalRewriter>& logicalRewriter,
    ABT& input) {
    using namespace properties;

    PlanExtractorResult result;
    if (!hasPhase(phase)) {
        // If we are skipping the implementation phase, return the input without a props map.
        result.emplace_back(std::move(input), NodeToGroupPropsMap{});
        return result;
    }

    tassert(6808704,
            "Nothing is inserted in the memo, logical rewrites may not have ran.",
            rootGroupId >= 0);
    // By default we require centralized result.
    // Also by default we do not require projections: the Root node will add those.
    PhysProps physProps = makePhysProps(DistributionRequirement(DistributionType::Centralized));
    if (_requireRID) {
        const auto& rootLogicalProps = _memo.getLogicalProps(rootGroupId);
        tassert(6808705,
                "We cannot obtain rid for this query.",
                hasProperty<IndexingAvailability>(rootLogicalProps));

        const auto& scanDefName =
            getPropertyConst<IndexingAvailability>(rootLogicalProps).getScanDefName();
        const auto& ridProjName = _ridProjections.at(scanDefName);
        setProperty(physProps, ProjectionRequirement{ProjectionNameVector{ridProjName}});

        setProperty(physProps,
                    IndexingRequirement(IndexReqTarget::Complete, true /*dedupRID*/, rootGroupId));
    }

    PhysicalRewriter rewriter(_metadata,
                              _memo,
                              _prefixId,
                              rootGroupId,
                              _debugInfo,
                              _hints,
                              _ridProjections,
                              *_costEstimator,
                              _pathToInterval,
                              logicalRewriter);

    auto optGroupResult =
        rewriter.optimizeGroup(rootGroupId, std::move(physProps), CostType::kInfinity);
    if (!optGroupResult._success) {
        return {};
    }

    _physicalNodeId = {rootGroupId, optGroupResult._index};
    result =
        extractPhysicalPlans(includeRejected, _physicalNodeId, _metadata, _ridProjections, _memo);

    for (const auto& planEntry : result) {
        env.rebuild(planEntry._node);
        if (env.hasFreeVariables()) {
            tasserted(6808707, "Plan has free variables: " + generateFreeVarsAssertMsg(env));
        }
    }
    return result;
}

PlanExtractorResult OptPhaseManager::runMemoRewritePhases(const bool includeRejected,
                                                          VariableEnvironment& env,
                                                          ABT& input) {
    GroupIdType rootGroupId = -1;
    std::unique_ptr<LogicalRewriter> logicalRewriter;

    runMemoLogicalRewrite(OptPhase::MemoSubstitutionPhase,
                          env,
                          LogicalRewriter::getSubstitutionSet(),
                          rootGroupId,
                          true /*runStandalone*/,
                          logicalRewriter,
                          input);

    runMemoLogicalRewrite(OptPhase::MemoExplorationPhase,
                          env,
                          LogicalRewriter::getExplorationSet(),
                          rootGroupId,
                          !hasPhase(OptPhase::MemoImplementationPhase),
                          logicalRewriter,
                          input);


    return runMemoPhysicalRewrite(OptPhase::MemoImplementationPhase,
                                  env,
                                  rootGroupId,
                                  includeRejected,
                                  logicalRewriter,
                                  input);
}

PlanExtractorResult OptPhaseManager::optimizeNoAssert(ABT input, const bool includeRejected) {
    tassert(6624173,
            "Requesting rejected plans without the requiring to keep them first.",
            !includeRejected || _hints._keepRejectedPlans);

    VariableEnvironment env = VariableEnvironment::build(input);
    if (env.hasFreeVariables()) {
        tasserted(6808711, "Plan has free variables: " + generateFreeVarsAssertMsg(env));
    }

    const auto canInlineEvalPre = [this](const EvaluationNode& node) {
        // We are not allowed to inline if we can convert to SargableNode.
        return !convertExprToPartialSchemaReq(
            node.getProjection(), false /*isFilterContext*/, _pathToInterval);
    };

    runStructuralPhases<OptPhase::ConstEvalPre, OptPhase::PathFuse, ConstEval, PathFusion>(
        ConstEval{env, canInlineEvalPre}, PathFusion{env}, env, input);

    auto planExtractionResult = runMemoRewritePhases(includeRejected, env, input);
    // At this point "input" has been siphoned out.

    if (_supportExplain && !planExtractionResult.empty()) {
        // Retain first post-memo plan for explain purposes.
        _postMemoPlan = planExtractionResult.front();
    }

    for (auto& planEntry : planExtractionResult) {
        runStructuralPhase<OptPhase::PathLower, PathLowering>(
            PathLowering{_prefixId, env}, env, planEntry._node);

        // The constant folding phase below may inline or erase projections which are referred to by
        // the plan's projection requirement properties. We need to respond to the ConstEval's
        // actions by removing them or renaming them as appropriate. For this purpose we have a
        // usage map, which keeps track of the projection sets where each projection is present.
        // Note: we currently only update the ProjectionRequirement property which is used for
        // lowering.
        // Key: projection name, value: set of pointers to projection sets which contain it.
        ProjectionNameMap<opt::unordered_set<ProjectionNameOrderPreservingSet*>> usageMap;

        // Populate the initial usage map.
        for (auto& [nodePtr, props] : planEntry._map) {
            using namespace properties;
            if (hasProperty<ProjectionRequirement>(props._physicalProps)) {
                auto& projSet =
                    getProperty<ProjectionRequirement>(props._physicalProps).getProjections();
                for (const auto& projName : projSet.getVector()) {
                    usageMap[projName].insert(&projSet);
                }
            }
        }

        // Respond to projection deletions.
        const auto erasedProjFn = [&](const ProjectionName& target) {
            if (auto it = usageMap.find(target); it != usageMap.cend()) {
                for (const auto& projSet : it->second) {
                    projSet->erase(target);
                }
                usageMap.erase(it);
            }
        };

        // Respond to projection renames.
        const auto renamedProjFn = [&](const ProjectionName& target, const ProjectionName& source) {
            auto it = usageMap.find(target);
            if (it == usageMap.cend()) {
                return;
            }

            auto projSetSet = it->second;
            for (auto& projSet : projSetSet) {
                projSet->erase(target);
                projSet->emplace_back(source);
                usageMap[source].insert(projSet);
            }
            usageMap.erase(target);
        };

        runStructuralPhase<OptPhase::ConstEvalPost, ConstEval>(
            ConstEval{env, {} /*canInlineEvalFn*/, erasedProjFn, renamedProjFn},
            env,
            planEntry._node);

        env.rebuild(planEntry._node);
        if (env.hasFreeVariables()) {
            tasserted(6808710, "Plan has free variables: " + generateFreeVarsAssertMsg(env));
        }
    }

    tassert(6624174,
            "Returning more than one plan without including rejected.",
            planExtractionResult.size() <= 1 || includeRejected);
    return planExtractionResult;
}

void OptPhaseManager::optimize(ABT& input) {
    auto result = optimizeAndReturnProps(std::move(input));
    std::swap(input, result._node);
}

PlanAndProps OptPhaseManager::optimizeAndReturnProps(ABT input) {
    auto result = optimizeNoAssert(std::move(input), false /*includeRejected*/);
    tassert(6808706, "Optimization failed.", result.size() == 1);
    return std::move(result.front());
}

bool OptPhaseManager::hasPhase(const OptPhase phase) const {
    return _phaseSet.find(phase) != _phaseSet.cend();
}

const OptPhaseManager::PhaseSet& OptPhaseManager::getAllRewritesSet() {
    return _allRewrites;
}

MemoPhysicalNodeId OptPhaseManager::getPhysicalNodeId() const {
    return _physicalNodeId;
}

const boost::optional<PlanAndProps>& OptPhaseManager::getPostMemoPlan() const {
    return _postMemoPlan;
}

const QueryHints& OptPhaseManager::getHints() const {
    return _hints;
}

QueryHints& OptPhaseManager::getHints() {
    return _hints;
}

const Memo& OptPhaseManager::getMemo() const {
    return _memo;
}

const PathToIntervalFn& OptPhaseManager::getPathToInterval() const {
    return _pathToInterval;
}

const Metadata& OptPhaseManager::getMetadata() const {
    return _metadata;
}

}  // namespace mongo::optimizer
