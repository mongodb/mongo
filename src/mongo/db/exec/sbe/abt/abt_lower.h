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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {
using SlotVarMap = stdx::unordered_map<std::string, sbe::value::SlotId>;

class SBEExpressionLowering {
public:
    SBEExpressionLowering(const VariableEnvironment& env, SlotVarMap& slotMap)
        : _env(env), _slotMap(slotMap) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    std::unique_ptr<sbe::EExpression> transport(const T&, Ts&&...) {
        uasserted(6624237, "abt tree is not lowered correctly");
        return nullptr;
    }

    std::unique_ptr<sbe::EExpression> transport(const Constant&);
    std::unique_ptr<sbe::EExpression> transport(const Variable& var);
    std::unique_ptr<sbe::EExpression> transport(const Source&);
    std::unique_ptr<sbe::EExpression> transport(const BinaryOp& op,
                                                std::unique_ptr<sbe::EExpression> lhs,
                                                std::unique_ptr<sbe::EExpression> rhs);
    std::unique_ptr<sbe::EExpression> transport(const UnaryOp& op,
                                                std::unique_ptr<sbe::EExpression> arg);
    std::unique_ptr<sbe::EExpression> transport(const If&,
                                                std::unique_ptr<sbe::EExpression> cond,
                                                std::unique_ptr<sbe::EExpression> thenBranch,
                                                std::unique_ptr<sbe::EExpression> elseBranch);

    void prepare(const Let& let);
    std::unique_ptr<sbe::EExpression> transport(const Let& let,
                                                std::unique_ptr<sbe::EExpression> bind,
                                                std::unique_ptr<sbe::EExpression> in);
    void prepare(const LambdaAbstraction& lam);
    std::unique_ptr<sbe::EExpression> transport(const LambdaAbstraction& lam,
                                                std::unique_ptr<sbe::EExpression> body);
    std::unique_ptr<sbe::EExpression> transport(const LambdaApplication&,
                                                std::unique_ptr<sbe::EExpression> lam,
                                                std::unique_ptr<sbe::EExpression> arg);
    std::unique_ptr<sbe::EExpression> transport(
        const FunctionCall& fn, std::vector<std::unique_ptr<sbe::EExpression>> args);

    std::unique_ptr<sbe::EExpression> optimize(const ABT& n);

private:
    const VariableEnvironment& _env;
    SlotVarMap& _slotMap;

    sbe::FrameId _frameCounter{100};
    stdx::unordered_map<const Let*, sbe::FrameId> _letMap;
    stdx::unordered_map<const LambdaAbstraction*, sbe::FrameId> _lambdaMap;
};

class SBENodeLowering {
public:
    SBENodeLowering(const VariableEnvironment& env,
                    SlotVarMap& slotMap,
                    sbe::value::SlotIdGenerator& ids,
                    const Metadata& metadata,
                    const NodeToGroupPropsMap& nodeToGroupPropsMap,
                    const RIDProjectionsMap& ridProjections,
                    const bool randomScan = false)
        : _env(env),
          _slotMap(slotMap),
          _slotIdGenerator(ids),
          _metadata(metadata),
          _nodeToGroupPropsMap(nodeToGroupPropsMap),
          _ridProjections(ridProjections),
          _randomScan(randomScan) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    std::unique_ptr<sbe::PlanStage> walk(const T&, Ts&&...) {
        if constexpr (std::is_base_of_v<ExclusivelyLogicalNode, T>) {
            uasserted(6624238, "A physical plan should not contain exclusively logical nodes.");
        }
        return nullptr;
    }

    std::unique_ptr<sbe::PlanStage> walk(const RootNode& n, const ABT& child, const ABT& refs);
    std::unique_ptr<sbe::PlanStage> walk(const EvaluationNode& n,
                                         const ABT& child,
                                         const ABT& binds);

    std::unique_ptr<sbe::PlanStage> walk(const FilterNode& n, const ABT& child, const ABT& filter);

    std::unique_ptr<sbe::PlanStage> walk(const LimitSkipNode& n, const ABT& child);
    std::unique_ptr<sbe::PlanStage> walk(const ExchangeNode& n, const ABT& child, const ABT& refs);
    std::unique_ptr<sbe::PlanStage> walk(const CollationNode& n, const ABT& child, const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const UniqueNode& n, const ABT& child, const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const GroupByNode& n,
                                         const ABT& child,
                                         const ABT& aggBinds,
                                         const ABT& aggRefs,
                                         const ABT& gbBind,
                                         const ABT& gbRefs);

    std::unique_ptr<sbe::PlanStage> walk(const BinaryJoinNode& n,
                                         const ABT& leftChild,
                                         const ABT& rightChild,
                                         const ABT& filter);
    std::unique_ptr<sbe::PlanStage> walk(const HashJoinNode& n,
                                         const ABT& leftChild,
                                         const ABT& rightChild,
                                         const ABT& refs);
    std::unique_ptr<sbe::PlanStage> walk(const MergeJoinNode& n,
                                         const ABT& leftChild,
                                         const ABT& rightChild,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const UnionNode& n,
                                         const ABTVector& children,
                                         const ABT& binder,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const UnwindNode& n,
                                         const ABT& child,
                                         const ABT& pidBind,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const ScanNode& n, const ABT& /*binds*/);
    std::unique_ptr<sbe::PlanStage> walk(const PhysicalScanNode& n, const ABT& /*binds*/);
    std::unique_ptr<sbe::PlanStage> walk(const CoScanNode& n);

    std::unique_ptr<sbe::PlanStage> walk(const IndexScanNode& n, const ABT& /*binds*/);
    std::unique_ptr<sbe::PlanStage> walk(const SeekNode& n,
                                         const ABT& /*binds*/,
                                         const ABT& /*refs*/);

    std::unique_ptr<sbe::PlanStage> optimize(const ABT& n);

private:
    std::unique_ptr<sbe::PlanStage> lowerScanNode(const Node& n,
                                                  const std::string& scanDefName,
                                                  const FieldProjectionMap& fieldProjectionMap,
                                                  bool useParallelScan);
    void generateSlots(const FieldProjectionMap& fieldProjectionMap,
                       boost::optional<sbe::value::SlotId>& ridSlot,
                       boost::optional<sbe::value::SlotId>& rootSlot,
                       std::vector<std::string>& fields,
                       sbe::value::SlotVector& vars);

    sbe::value::SlotVector convertProjectionsToSlots(const ProjectionNameVector& projectionNames);
    sbe::value::SlotVector convertRequiredProjectionsToSlots(
        const NodeProps& props,
        bool removeRIDProjection,
        const sbe::value::SlotVector& toExclude = {});

    std::unique_ptr<sbe::EExpression> convertBoundsToExpr(
        bool isLower, const IndexDefinition& indexDef, const CompoundIntervalRequirement& interval);

    std::unique_ptr<sbe::PlanStage> generateInternal(const ABT& n);

    const VariableEnvironment& _env;
    SlotVarMap& _slotMap;
    sbe::value::SlotIdGenerator& _slotIdGenerator;

    const Metadata& _metadata;
    const NodeToGroupPropsMap& _nodeToGroupPropsMap;
    const RIDProjectionsMap& _ridProjections;

    // If true, will create scan nodes using a random cursor to support sampling.
    // Currently only supported for single-threaded (non parallel-scanned) mongod collections.
    // TODO: handle cases where we have more than one collection scan.
    const bool _randomScan;
};

}  // namespace mongo::optimizer
