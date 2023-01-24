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

#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo::optimizer {

class SBEExpressionLowering {
public:
    SBEExpressionLowering(const VariableEnvironment& env,
                          SlotVarMap& slotMap,
                          const NamedSlotsProvider& namedSlots)
        : _env(env), _slotMap(slotMap), _namedSlots(namedSlots) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    std::unique_ptr<sbe::EExpression> transport(const T&, Ts&&...) {
        uasserted(6624237,
                  "ABT expression lowering encountered operator which cannot be directly lowered "
                  "to an SBE expression.");
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
    const NamedSlotsProvider& _namedSlots;

    sbe::FrameId _frameCounter{100};
    stdx::unordered_map<const Let*, sbe::FrameId> _letMap;
    stdx::unordered_map<const LambdaAbstraction*, sbe::FrameId> _lambdaMap;
};

enum class ScanOrder {
    Forward,
    Reverse,
    Random  // Uses a random cursor.
};

class SBENodeLowering {
public:
    // TODO: SERVER-69540. Consider avoiding a mutable slotMap argument here.
    SBENodeLowering(const VariableEnvironment& env,
                    SlotVarMap& slotMap,
                    const NamedSlotsProvider& namedSlots,
                    boost::optional<sbe::value::SlotId>& ridSlot,
                    sbe::value::SlotIdGenerator& ids,
                    const Metadata& metadata,
                    const NodeToGroupPropsMap& nodeToGroupPropsMap,
                    const ScanOrder scanOrder)
        : _env(env),
          _slotMap(slotMap),
          _namedSlots(namedSlots),
          _ridSlot(ridSlot),
          _slotIdGenerator(ids),
          _metadata(metadata),
          _nodeToGroupPropsMap(nodeToGroupPropsMap),
          _scanOrder(scanOrder) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    std::unique_ptr<sbe::PlanStage> walk(const T&, Ts&&...) {
        // We should not be seeing a physical delegator node here.
        static_assert(!canBePhysicalNode<T>() || std::is_same_v<MemoPhysicalDelegatorNode, T>,
                      "Physical nodes need to implement lowering");

        uasserted(6624238, "Unexpected node type.");
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

    std::unique_ptr<sbe::PlanStage> walk(const SpoolProducerNode& n,
                                         const ABT& child,
                                         const ABT& filter,
                                         const ABT& binder,
                                         const ABT& refs);
    std::unique_ptr<sbe::PlanStage> walk(const SpoolConsumerNode& n, const ABT& binder);

    std::unique_ptr<sbe::PlanStage> walk(const GroupByNode& n,
                                         const ABT& child,
                                         const ABT& aggBinds,
                                         const ABT& aggRefs,
                                         const ABT& gbBind,
                                         const ABT& gbRefs);

    std::unique_ptr<sbe::PlanStage> walk(const NestedLoopJoinNode& n,
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

    std::unique_ptr<sbe::PlanStage> walk(const SortedMergeNode& n,
                                         const ABTVector& children,
                                         const ABT& binder,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const UnionNode& n,
                                         const ABTVector& children,
                                         const ABT& binder,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const UnwindNode& n,
                                         const ABT& child,
                                         const ABT& pidBind,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const PhysicalScanNode& n, const ABT& /*binds*/);
    std::unique_ptr<sbe::PlanStage> walk(const CoScanNode& n);

    std::unique_ptr<sbe::PlanStage> walk(const IndexScanNode& n, const ABT& /*binds*/);
    std::unique_ptr<sbe::PlanStage> walk(const SeekNode& n,
                                         const ABT& /*binds*/,
                                         const ABT& /*refs*/);

    std::unique_ptr<sbe::PlanStage> optimize(const ABT& n);

private:
    void generateSlots(const FieldProjectionMap& fieldProjectionMap,
                       boost::optional<sbe::value::SlotId>& ridSlot,
                       boost::optional<sbe::value::SlotId>& rootSlot,
                       std::vector<std::string>& fields,
                       sbe::value::SlotVector& vars);

    /**
     * Convert a vector of ProjectionNames to slot IDs from the projections that have already been
     * bound to slots.
     */
    sbe::value::SlotVector convertProjectionsToSlots(const ProjectionNameVector& projectionNames);

    /**
     * During Cascades, projections that a node is required to propagate up the tree are added to
     * the RequiredProjections node property. This function pulls out those projection names and
     * looks up the relevant slot IDs they are bound to. The optional toExclude vector can prevent
     * some slots from being added to the output vector.
     */
    sbe::value::SlotVector convertRequiredProjectionsToSlots(
        const NodeProps& props, const sbe::value::SlotVector& toExclude = {});

    std::unique_ptr<sbe::EExpression> convertBoundsToExpr(
        bool isLower, const IndexDefinition& indexDef, const CompoundIntervalRequirement& interval);

    std::unique_ptr<sbe::PlanStage> generateInternal(const ABT& n);

    /**
     * Maps a projection name to a slot by updating _slotMap field.
     * By default it will tassert rather than overwrite an existing entry--it's the caller's
     * responsibility not to call this twice with the same projName. With 'canOverwrite = true' it
     * is allowed to overwrite an existing entry. This is useful for nodes that intentionally use
     * the same projName for two different values. For example, two independent index scans could
     * both use the same projName for RID. Or, Unwind uses the same projName both for the original
     * array, and the unwound elements.
     */
    void mapProjToSlot(const ProjectionName& projName,
                       sbe::value::SlotId slot,
                       bool canOverwrite = false);

    /**
     * Instantiate an expression lowering transporter for use in node lowering.
     */
    SBEExpressionLowering getExpressionLowering() {
        return SBEExpressionLowering{_env, _slotMap, _namedSlots};
    }

    std::unique_ptr<sbe::EExpression> lowerExpression(const ABT& e) {
        return getExpressionLowering().optimize(e);
    }
    const VariableEnvironment& _env;
    SlotVarMap& _slotMap;
    const NamedSlotsProvider& _namedSlots;
    boost::optional<sbe::value::SlotId>& _ridSlot;
    sbe::value::SlotIdGenerator& _slotIdGenerator;

    const Metadata& _metadata;
    const NodeToGroupPropsMap& _nodeToGroupPropsMap;

    // Specifies the order for any ScanStages. Currently only supported for single-threaded
    // (non parallel-scanned) mongod collections.
    // TODO SERVER-73010: handle cases where we have more than one collection scan.
    const ScanOrder _scanOrder;
};

}  // namespace mongo::optimizer
