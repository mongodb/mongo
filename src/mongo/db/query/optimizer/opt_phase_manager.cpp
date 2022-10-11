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

#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/cascades/cost_derivation.h"
#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/explain.h"
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
                                 Metadata metadata,
                                 DebugInfo debugInfo,
                                 QueryHints queryHints)
    : OptPhaseManager(std::move(phaseSet),
                      prefixId,
                      false /*requireRID*/,
                      std::move(metadata),
                      std::make_unique<HeuristicCE>(),
                      std::make_unique<DefaultCosting>(),
                      {} /*pathToInterval*/,
                      std::move(debugInfo),
                      std::move(queryHints)) {}

OptPhaseManager::OptPhaseManager(OptPhaseManager::PhaseSet phaseSet,
                                 PrefixId& prefixId,
                                 const bool requireRID,
                                 Metadata metadata,
                                 std::unique_ptr<CEInterface> ceDerivation,
                                 std::unique_ptr<CostingInterface> costDerivation,
                                 PathToIntervalFn pathToInterval,
                                 DebugInfo debugInfo,
                                 QueryHints queryHints)
    : _phaseSet(std::move(phaseSet)),
      _debugInfo(std::move(debugInfo)),
      _hints(std::move(queryHints)),
      _metadata(std::move(metadata)),
      _memo(),
      _logicalPropsDerivation(std::make_unique<DefaultLogicalPropsDerivation>()),
      _ceDerivation(std::move(ceDerivation)),
      _costDerivation(std::move(costDerivation)),
      _pathToInterval(std::move(pathToInterval)),
      _physicalNodeId(),
      _requireRID(requireRID),
      _ridProjections(),
      _prefixId(prefixId) {
    uassert(6624093, "Empty Cost derivation", _costDerivation.get());

    for (const auto& entry : _metadata._scanDefs) {
        _ridProjections.emplace(entry.first, _prefixId.getNextId("rid"));
    }
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

    tassert(6808709, "Environment has free variables.", !env.hasFreeVariables());
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

    tassert(6808701, "Environment has free variables.", !env.hasFreeVariables());
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
    const bool useHeuristicCE = phase == OptPhase::MemoSubstitutionPhase;
    HeuristicCE heuristicCE;
    logicalRewriter =
        std::make_unique<LogicalRewriter>(_metadata,
                                          _memo,
                                          _prefixId,
                                          rewriteSet,
                                          _debugInfo,
                                          _hints,
                                          _pathToInterval,
                                          *_logicalPropsDerivation,
                                          useHeuristicCE ? heuristicCE : *_ceDerivation);
    rootGroupId = logicalRewriter->addRootNode(input);

    if (runStandalone) {
        const bool fixPointRewritten = logicalRewriter->rewriteToFixPoint();
        tassert(6808702, "Logical writer failed to rewrite fix point.", fixPointRewritten);

        input = extractLatestPlan(_memo, rootGroupId);
        env.rebuild(input);
    }

    tassert(6808703, "Environment has free variables.", !env.hasFreeVariables());
}

void OptPhaseManager::runMemoPhysicalRewrite(const OptPhase phase,
                                             VariableEnvironment& env,
                                             const GroupIdType rootGroupId,
                                             std::unique_ptr<LogicalRewriter>& logicalRewriter,
                                             ABT& input) {
    using namespace properties;

    if (!hasPhase(phase)) {
        return;
    }

    tassert(6808704,
            "Nothing is inserted in the memo, logical rewrites may not have ran.",
            rootGroupId >= 0);
    // By default we require centralized result.
    // Also by default we do not require projections: the Root node will add those.
    PhysProps physProps = makePhysProps(DistributionRequirement(DistributionType::Centralized));
    if (_requireRID) {
        const auto& rootLogicalProps = _memo.getGroup(rootGroupId)._logicalProperties;
        tassert(6808705,
                "We cannot optain rid for this query.",
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
                              rootGroupId,
                              _debugInfo,
                              _hints,
                              _ridProjections,
                              *_costDerivation,
                              _pathToInterval,
                              logicalRewriter);

    auto optGroupResult =
        rewriter.optimizeGroup(rootGroupId, std::move(physProps), _prefixId, CostType::kInfinity);
    tassert(6808706, "Optimization failed.", optGroupResult._success);

    _physicalNodeId = {rootGroupId, optGroupResult._index};
    std::tie(input, _nodeToGroupPropsMap) = extractPhysicalPlan(_physicalNodeId, _metadata, _memo);

    env.rebuild(input);

    tassert(6808707, "Environment has free variables.", !env.hasFreeVariables());
}

void OptPhaseManager::runMemoRewritePhases(VariableEnvironment& env, ABT& input) {
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


    runMemoPhysicalRewrite(
        OptPhase::MemoImplementationPhase, env, rootGroupId, logicalRewriter, input);
}

void OptPhaseManager::optimize(ABT& input) {
    VariableEnvironment env = VariableEnvironment::build(input);

    std::string freeVariables = "";
    if (env.hasFreeVariables()) {
        bool first = true;
        for (auto& name : env.freeVariableNames()) {
            if (first) {
                first = false;
            } else {
                freeVariables += ", ";
            }
            freeVariables += name;
        }
    }

    tassert(6808711,
            "Environment has the following free variables: " + freeVariables + ".",
            !env.hasFreeVariables());

    const auto sargableCheckFn = [this](const ABT& expr) {
        return convertExprToPartialSchemaReq(expr, false /*isFilterContext*/, _pathToInterval)
            .has_value();
    };

    runStructuralPhases<OptPhase::ConstEvalPre, OptPhase::PathFuse, ConstEval, PathFusion>(
        ConstEval{env, sargableCheckFn}, PathFusion{env}, env, input);

    runMemoRewritePhases(env, input);

    runStructuralPhase<OptPhase::PathLower, PathLowering>(PathLowering{_prefixId, env}, env, input);

    ProjectionNameSet erasedProjNames;

    runStructuralPhase<OptPhase::ConstEvalPost, ConstEval>(
        ConstEval{env, {} /*disableInline*/, &erasedProjNames}, env, input);

    if (!erasedProjNames.empty()) {
        // If we have erased some eval nodes, make sure to delete the corresponding projection names
        // from the node property map.
        for (auto& [nodePtr, props] : _nodeToGroupPropsMap) {
            if (properties::hasProperty<properties::ProjectionRequirement>(props._physicalProps)) {
                auto& requiredProjNames =
                    properties::getProperty<properties::ProjectionRequirement>(props._physicalProps)
                        .getProjections();
                for (const ProjectionName& projName : erasedProjNames) {
                    requiredProjNames.erase(projName);
                }
            }
        }
    }

    env.rebuild(input);
    tassert(6808710, "Environment has free variables.", !env.hasFreeVariables());
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

PrefixId& OptPhaseManager::getPrefixId() const {
    return _prefixId;
}

const NodeToGroupPropsMap& OptPhaseManager::getNodeToGroupPropsMap() const {
    return _nodeToGroupPropsMap;
}

NodeToGroupPropsMap& OptPhaseManager::getNodeToGroupPropsMap() {
    return _nodeToGroupPropsMap;
}

const RIDProjectionsMap& OptPhaseManager::getRIDProjections() const {
    return _ridProjections;
}

}  // namespace mongo::optimizer
