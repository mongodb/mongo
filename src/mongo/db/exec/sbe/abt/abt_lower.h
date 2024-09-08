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

#include <boost/optional/optional.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/abt/slots_provider.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {
constexpr mongo::StringData kshardFiltererSlotName = "shardFilterer"_sd;

/**
 * Structure to represent index field component and its associated collation. The _path field
 * contains the path to the field component, restricted to Get, Traverse, and Id elements.
 * For example, if we have an index on {a.b, c} that contains arrays, the _path for the first entry
 * would be Get "a" Traverse Get "b" Traverse Id, and the _path for the second entry would be
 * Get "c" Traverse Id.
 * Implicitly contains multikey info through Traverse element or lack of Traverse element.
 */
struct LoweringIndexCollationEntry {
    ABT _path;
    CollationOp _op;
};

/**
 * Parameters to a scan node, including collection UUID and other scan definition options.
 */
struct LoweringScanDefinition {
    using ScanDefOptions = stdx::unordered_map<std::string, std::string>;

    // All indexes are treated as version 1 and ordering bits set to 0.
    static constexpr int64_t kIndexVersion = 1;
    static constexpr uint32_t kIndexOrderingBits = 0;

    DatabaseName _dbName;
    ScanDefOptions _options;
    boost::optional<UUID> _uuid;

    // True if the collection exists.
    bool _exists;

    // Shard key of the collection. This is stored as an index entry because the shard key
    // is conceptually an index to the shard which contains a particular key. The only collation ops
    // that are allowed are Ascending and Clustered.
    // Note: Clustered collation op is intended to represent a hashed shard key; however, if two
    // keys hash to the same value, it is possible that an index scan of the hashed index will
    // produce a stream of keys which are not clustered. Hashed indexes are implemented with a
    // B-tree using the hashed value as a key, which makes it sensitive to insertion order.
    std::vector<LoweringIndexCollationEntry> _shardKey;
};

struct LoweringNodeProps {
    // Used to tie to a corresponding SBE stage.
    int32_t _planNodeId;

    boost::optional<std::string> _indexScanDefName;

    // Optional projection requirements.
    boost::optional<ProjectionNameOrderPreservingSet> _projections;

    // Optional skip/limit requirements.
    bool _hasLimitSkip;
    // Max number of documents to return. Maximum integer value means unlimited.
    int64_t _limit;
    // Documents to skip before start returning in result.
    int64_t _skip;

    // Set if we have an RID projection name.
    boost::optional<ProjectionName> _ridProjName;
};

// Map from node to various properties. Used to determine for example which of the available
// projections are used for exchanges.
using LoweringNodeToGroupPropsMap = stdx::unordered_map<const Node*, LoweringNodeProps>;

class VarResolver {
public:
    using LowerFuncT = std::function<std::unique_ptr<sbe::EExpression>(const ProjectionName&)>;

    VarResolver(SlotVarMap& slotMap) : _slotMap(&slotMap) {}

    template <typename FuncT>
    VarResolver(FuncT lowerFn) : _lowerFn(std::move(lowerFn)) {}

    template <typename FuncT>
    VarResolver(SlotVarMap& slotMap, FuncT lowerFn)
        : _slotMap(&slotMap), _lowerFn(std::move(lowerFn)) {}

    std::unique_ptr<sbe::EExpression> operator()(const ProjectionName& name) const;

private:
    SlotVarMap* _slotMap = nullptr;
    LowerFuncT _lowerFn;
};

/*
 * Represent how ABT expression to SBE EExpression lowering should treat the meaning of
 * BinaryOp<Gt/Gte/Lt/Lte/Eq>.
 */
enum class ComparisonOpSemantics {
    // Translate comparisons as full BSON order comparisons. For example, 5 < "str" evaluates to
    // true because numbers are less than strings on the BSON numberline. BinaryOp<Eq> will return
    // false for operands of different types.
    kTotalOrder,
    // Translate comparisons as type-bracketed comparisons. This causes comparisons with operands of
    // different types to evaluate to Nothing. This option will means the ABT BinaryOp operators
    // have the same semantics as 'sbe::EPrimBinary'; for example, numbers have IEEE semantics,
    // where NaN == NaN evaluates to false.
    kTypeBracketing
};

class SBEExpressionLowering {
public:
    SBEExpressionLowering(
        const VariableEnvironment& env,
        VarResolver vr,
        SlotsProvider& providedSlots,
        sbe::value::SlotIdGenerator& ids,
        sbe::InputParamToSlotMap& inputParamToSlotMap,
        stdx::unordered_map<std::string, LoweringScanDefinition>* scanDefs,
        const LoweringNodeProps* np = nullptr,
        ComparisonOpSemantics compOpSemantics = ComparisonOpSemantics::kTotalOrder,
        sbe::value::FrameIdGenerator* frameIdGenerator = nullptr)
        : _env(env),
          _varResolver(vr),
          _providedSlots(providedSlots),
          _slotIdGenerator(ids),
          _inputParamToSlotMap(inputParamToSlotMap),
          _scanDefs(scanDefs),
          _np(np),
          _frameIdGenerator(frameIdGenerator),
          _comparisonOpSemantics(compOpSemantics) {}

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
    sbe::FrameId generateFrameId() {
        if (_frameIdGenerator) {
            return _frameIdGenerator->generate();
        } else {
            return _localFrameIdGenerator.generate();
        }
    }

    const VariableEnvironment& _env;
    VarResolver _varResolver;
    SlotsProvider& _providedSlots;
    sbe::value::SlotIdGenerator& _slotIdGenerator;

    // Map to record newly allocated slots and the parameter ids they were generated from.
    // For more details see PlanStageStaticData::inputParamToSlotMap
    sbe::InputParamToSlotMap& _inputParamToSlotMap;
    stdx::unordered_map<std::string, LoweringScanDefinition>* _scanDefs;
    const LoweringNodeProps* _np;

    // If '_frameIdGenerator' is not null then we use it to generate frame IDs, otherwise we
    // use '_localFrameIdGenerator' to generate frame IDs.
    sbe::value::FrameIdGenerator* const _frameIdGenerator;
    sbe::value::FrameIdGenerator _localFrameIdGenerator{100};

    stdx::unordered_map<const Let*, sbe::FrameId> _letMap;
    stdx::unordered_map<const LambdaAbstraction*, sbe::FrameId> _lambdaMap;
    // Allow SBE stage builders to specify a different meaning for comparison operations. This is
    // mainly offered as a crutch to allow SBE stage builders to continue using this class. The
    // default for Bonsai is that comparison operators form a total order; many rewrites make this
    // assumption.
    ComparisonOpSemantics _comparisonOpSemantics{ComparisonOpSemantics::kTotalOrder};
};

class SBENodeLowering {
public:
    SBENodeLowering(const VariableEnvironment& env,
                    SlotsProvider& providedSlots,
                    sbe::value::SlotIdGenerator& ids,
                    sbe::InputParamToSlotMap& inputParamToSlotMap,
                    stdx::unordered_map<std::string, LoweringScanDefinition> scanDefs,
                    const LoweringNodeToGroupPropsMap& nodeToGroupPropsMap,
                    size_t numberOfPartitions,
                    PlanYieldPolicy* yieldPolicy)
        : _env(env),
          _providedSlots(providedSlots),
          _slotIdGenerator(ids),
          _inputParamToSlotMap(inputParamToSlotMap),
          _scanDefs(std::move(scanDefs)),
          _numberOfPartitions(numberOfPartitions),
          _nodeToGroupPropsMap(nodeToGroupPropsMap),
          _yieldPolicy(yieldPolicy) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const T&,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         Ts&&...) {
        static_assert(!canBePhysicalNode<T>(), "Physical nodes need to implement lowering");

        uasserted(6624238, "Unexpected node type.");
        return nullptr;
    }

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const RootNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& refs);
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const EvaluationNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& binds);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const FilterNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& filter);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const LimitSkipNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child);
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const ExchangeNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& refs);
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const CollationNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const UniqueNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const SpoolProducerNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& filter,
                                         const ABT& binder,
                                         const ABT& refs);
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const SpoolConsumerNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& binder);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const GroupByNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& aggBinds,
                                         const ABT& aggRefs,
                                         const ABT& gbBind,
                                         const ABT& gbRefs);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const NestedLoopJoinNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& leftChild,
                                         const ABT& rightChild,
                                         const ABT& filter);
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const HashJoinNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& leftChild,
                                         const ABT& rightChild,
                                         const ABT& refs);
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const MergeJoinNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& leftChild,
                                         const ABT& rightChild,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const SortedMergeNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABTVector& children,
                                         const ABT& binder,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const UnionNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABTVector& children,
                                         const ABT& binder,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const UnwindNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& child,
                                         const ABT& pidBind,
                                         const ABT& refs);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const PhysicalScanNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& /*binds*/);
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const CoScanNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot);

    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const IndexScanNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& /*binds*/);
    std::unique_ptr<sbe::PlanStage> walk(const ABT& abtn,
                                         const SeekNode& n,
                                         SlotVarMap& slotMap,
                                         boost::optional<sbe::value::SlotId>& ridSlot,
                                         const ABT& /*binds*/,
                                         const ABT& /*refs*/);

    std::unique_ptr<sbe::PlanStage> optimize(const ABT& n,
                                             SlotVarMap& slotMap,
                                             boost::optional<sbe::value::SlotId>& ridSlot);

private:
    void generateSlots(SlotVarMap& slotMap,
                       const FieldProjectionMap& fieldProjectionMap,
                       boost::optional<sbe::value::SlotId>& ridSlot,
                       boost::optional<sbe::value::SlotId>& rootSlot,
                       std::vector<std::string>& fields,
                       sbe::value::SlotVector& vars);

    /**
     * Convert a vector of ProjectionNames to slot IDs from the projections that have already been
     * bound to slots.
     *
     * Preserves the order, and therefore preserves duplicates and the result .size() == the input
     * .size().
     *
     * Even when 'projectionNames' is free of duplicates, the output may have duplicates because two
     * projections can map to the same slot.
     */
    sbe::value::SlotVector convertProjectionsToSlots(
        const SlotVarMap& slotMap, const ProjectionNameVector& projectionNames) const;

    /**
     * During Cascades, projections that a node is required to propagate up the tree are added to
     * the RequiredProjections node property. This function pulls out those projection names and
     * looks up the relevant slot IDs they are bound to. The optional toExclude vector can prevent
     * some slots from being added to the output vector.
     *
     * The output is free of duplicates.
     *
     * Does not guarantee any output order.
     */
    sbe::value::SlotVector convertRequiredProjectionsToSlots(
        const SlotVarMap& slotMap,
        const LoweringNodeProps& props,
        const sbe::value::SlotVector& toExclude = {}) const;

    /**
     * If the node pointer exists in _nodeToGroupPropsMap, then return _planNode from the
     * corresponding entry, otherwise return 0.
     */
    PlanNodeId getPlanNodeId(const Node& node) const;

    std::unique_ptr<sbe::EExpression> convertBoundsToExpr(SlotVarMap& slotMap,
                                                          bool isLower,
                                                          bool reversed,
                                                          const CompoundBoundRequirement& bound);

    std::unique_ptr<sbe::PlanStage> generateInternal(const ABT& n,
                                                     SlotVarMap& slotMap,
                                                     boost::optional<sbe::value::SlotId>& ridSlot);

    /**
     * Maps a projection name to a slot by updating slotMap field.
     * By default it will tassert rather than overwrite an existing entry--it's the caller's
     * responsibility not to call this twice with the same projName. With 'canOverwrite = true' it
     * is allowed to overwrite an existing entry. This is useful for nodes that intentionally use
     * the same projName for two different values. For example, two independent index scans could
     * both use the same projName for RID. Or, Unwind uses the same projName both for the original
     * array, and the unwound elements.
     */
    void mapProjToSlot(SlotVarMap& slotMap,
                       const ProjectionName& projName,
                       sbe::value::SlotId slot,
                       bool canOverwrite = false);


    /**
     * Instantiate an expression lowering transporter for use in node lowering.
     */
    SBEExpressionLowering getExpressionLowering(SlotVarMap& slotMap,
                                                const LoweringNodeProps* np = nullptr) {
        return SBEExpressionLowering{
            _env, slotMap, _providedSlots, _slotIdGenerator, _inputParamToSlotMap, &_scanDefs, np};
    }

    std::unique_ptr<sbe::EExpression> lowerExpression(const ABT& e,
                                                      SlotVarMap& slotMap,
                                                      const LoweringNodeProps* np = nullptr) {
        return getExpressionLowering(slotMap, np).optimize(e);
    }

    void extractAndLowerExpressions(const EvaluationNode& n,
                                    SlotVarMap& slotMap,
                                    sbe::SlotExprPairVector& projectsOut);

    const VariableEnvironment& _env;
    SlotsProvider& _providedSlots;

    sbe::value::SlotIdGenerator& _slotIdGenerator;

    sbe::InputParamToSlotMap& _inputParamToSlotMap;

    stdx::unordered_map<std::string, LoweringScanDefinition> _scanDefs;
    size_t _numberOfPartitions;
    const LoweringNodeToGroupPropsMap& _nodeToGroupPropsMap;

    // Specifies the yielding policy to initialize the corresponding PlanStages with.
    PlanYieldPolicy* _yieldPolicy;

    // Map of <child, parent> evaluation nodes, such that the parent projections can be merged in
    // the same sbe project stage as the child's.
    std::map<const EvaluationNode*, const EvaluationNode*> _evalMap;
};

inline sbe::EPrimUnary::Op getEPrimUnaryOp(optimizer::Operations op) {
    switch (op) {
        case Operations::Neg:
            return sbe::EPrimUnary::negate;
        case Operations::Not:
            return sbe::EPrimUnary::logicNot;
        default:
            MONGO_UNREACHABLE;
    }
}

inline sbe::EPrimBinary::Op getEPrimBinaryOp(optimizer::Operations op) {
    switch (op) {
        case Operations::Eq:
            return sbe::EPrimBinary::eq;
        case Operations::Neq:
            return sbe::EPrimBinary::neq;
        case Operations::Gt:
            return sbe::EPrimBinary::greater;
        case Operations::Gte:
            return sbe::EPrimBinary::greaterEq;
        case Operations::Lt:
            return sbe::EPrimBinary::less;
        case Operations::Lte:
            return sbe::EPrimBinary::lessEq;
        case Operations::Add:
            return sbe::EPrimBinary::add;
        case Operations::Sub:
            return sbe::EPrimBinary::sub;
        case Operations::FillEmpty:
            return sbe::EPrimBinary::fillEmpty;
        case Operations::And:
            return sbe::EPrimBinary::logicAnd;
        case Operations::Or:
            return sbe::EPrimBinary::logicOr;
        case Operations::Cmp3w:
            return sbe::EPrimBinary::cmp3w;
        case Operations::Div:
            return sbe::EPrimBinary::div;
        case Operations::Mult:
            return sbe::EPrimBinary::mul;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace mongo::optimizer
