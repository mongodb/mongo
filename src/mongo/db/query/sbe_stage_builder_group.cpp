/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_stage_builder.h"

#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_impl.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr_helpers.h"

namespace mongo::stage_builder {
namespace {
template <typename F>
struct FieldPathAndCondPreVisitor : public SelectiveConstExpressionVisitorBase {
    // To avoid overloaded-virtual warnings.
    using SelectiveConstExpressionVisitorBase::visit;

    explicit FieldPathAndCondPreVisitor(const F& fn) : _fn(fn) {}

    void visit(const ExpressionFieldPath* expr) final {
        _fn(expr);
    }

    F _fn;
};

/**
 * Walks through the 'expr' expression tree and whenever finds an 'ExpressionFieldPath', calls
 * the 'fn' function. Type requirement for 'fn' is it must have a const 'ExpressionFieldPath'
 * pointer parameter.
 */
template <typename F>
void walkAndActOnFieldPaths(Expression* expr, const F& fn) {
    FieldPathAndCondPreVisitor<F> preVisitor(fn);
    ExpressionWalker walker(&preVisitor, nullptr /*inVisitor*/, nullptr /*postVisitor*/);
    expression_walker::walk(expr, &walker);
}

/**
 * Compute what values 'groupNode' will need from its child node in order to build expressions for
 * the group-by key ("_id") and the accumulators.
 */
MONGO_COMPILER_NOINLINE
PlanStageReqs computeChildReqsForGroup(const PlanStageReqs& reqs, const GroupNode& groupNode) {
    auto childReqs = reqs.copyForChild().setResultObj().clearAllFields();

    // If the group node references any top level fields, we take all of them and add them to
    // 'childReqs'. Note that this happens regardless of whether we need the whole document because
    // it can be the case that this stage references '$$ROOT' as well as some top level fields.
    if (auto topLevelFields = getTopLevelFields(groupNode.requiredFields);
        !topLevelFields.empty()) {
        childReqs.setFields(std::move(topLevelFields));
    }

    if (!groupNode.needWholeDocument) {
        // Tracks whether we need to require our child to produce a materialized result object.
        bool rootDocIsNeeded = false;
        bool sortKeyIsNeeded = false;
        auto referencesRoot = [&](const ExpressionFieldPath* fieldExpr) {
            rootDocIsNeeded = rootDocIsNeeded || fieldExpr->isROOT();
        };

        // Walk over all field paths involved in this $group stage.
        walkAndActOnFieldPaths(groupNode.groupByExpression.get(), referencesRoot);
        for (const auto& accStmt : groupNode.accumulators) {
            walkAndActOnFieldPaths(accStmt.expr.argument.get(), referencesRoot);
            if (isTopBottomN(accStmt)) {
                sortKeyIsNeeded = true;
            }
        }

        // If any accumulator requires generating sort key, we cannot clear the result requirement
        // from 'childReqs'.
        if (!sortKeyIsNeeded) {
            const auto& childNode = *groupNode.children[0];

            // If the group node doesn't have any dependency (e.g. $count) or if the dependency can
            // be satisfied by the child node (e.g. covered index scan), we can clear the result
            // requirement for the child.
            if (groupNode.requiredFields.empty() || !rootDocIsNeeded) {
                childReqs.clearResult();
            } else if (childNode.getType() == StageType::STAGE_PROJECTION_COVERED) {
                auto& childPn = static_cast<const ProjectionNodeCovered&>(childNode);
                std::set<std::string> providedFieldSet;
                for (auto&& elt : childPn.coveredKeyObj) {
                    providedFieldSet.emplace(elt.fieldNameStringData());
                }
                if (std::all_of(groupNode.requiredFields.begin(),
                                groupNode.requiredFields.end(),
                                [&](const std::string& f) { return providedFieldSet.count(f); })) {
                    childReqs.clearResult();
                }
            }
        }
    }

    return childReqs;
}

/**
 * Collect the FieldPath expressions referenced by a GroupNode that should be exposed in a slot for
 * the group stage to work properly.
 */
MONGO_COMPILER_NOINLINE
StringMap<const ExpressionFieldPath*> collectFieldPaths(const GroupNode* groupNode) {
    StringMap<const ExpressionFieldPath*> groupFieldMap;
    auto accumulateFieldPaths = [&](const ExpressionFieldPath* fieldExpr) {
        // We optimize neither a field path for the top-level document itself nor a field path
        // that refers to a variable instead.
        if (fieldExpr->getFieldPath().getPathLength() == 1 || fieldExpr->isVariableReference()) {
            return;
        }

        // Don't generate an expression if we have one already.
        std::string fp = fieldExpr->getFieldPathWithoutCurrentPrefix().fullPath();
        if (groupFieldMap.count(fp)) {
            return;
        }
        // Neither if it's a top level field which already have a slot.
        if (fieldExpr->getFieldPath().getPathLength() != 2) {
            groupFieldMap.emplace(fp, fieldExpr);
        }
    };
    // Walk over all field paths involved in this $group stage.
    walkAndActOnFieldPaths(groupNode->groupByExpression.get(), accumulateFieldPaths);
    for (const auto& accStmt : groupNode->accumulators) {
        walkAndActOnFieldPaths(accStmt.expr.argument.get(), accumulateFieldPaths);
    }
    return groupFieldMap;
}

/**
 * Given a list of field path expressions used in the group-by ('_id') and accumulator expressions
 * of a $group, populate a slot in 'outputs' for each path found. Each slot is bound to an SBE
 * EExpression (via a ProjectStage) that evaluates the path traversal.
 */
MONGO_COMPILER_NOINLINE
SbStage projectFieldPathsToPathExprSlots(
    StageBuilderState& state,
    const GroupNode& groupNode,
    SbStage stage,
    PlanStageSlots& outputs,
    const StringMap<const ExpressionFieldPath*>& groupFieldMap) {
    SbBuilder b(state, groupNode.nodeId());

    SbExprOptSbSlotVector projects;
    for (auto& fp : groupFieldMap) {
        projects.emplace_back(stage_builder::generateExpression(
                                  state, fp.second, outputs.getResultObjIfExists(), outputs),
                              boost::none);
    }

    if (!projects.empty()) {
        auto [outStage, outSlots] =
            b.makeProject(std::move(stage), buildVariableTypes(outputs), std::move(projects));
        stage = std::move(outStage);

        size_t i = 0;
        for (auto& fp : groupFieldMap) {
            auto name = PlanStageSlots::OwnedSlotName(PlanStageSlots::kPathExpr, fp.first);
            outputs.set(std::move(name), outSlots[i]);
            ++i;
        }
    }

    return stage;
}

MONGO_COMPILER_NOINLINE
SbExpr::Vector generateGroupByKeyExprs(StageBuilderState& state,
                                       Expression* idExpr,
                                       const PlanStageSlots& outputs) {
    SbExprBuilder b(state);
    SbExpr::Vector exprs;
    auto rootSlot = outputs.getResultObjIfExists();

    auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr);
    if (idExprObj) {
        for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
            exprs.emplace_back(generateExpression(state, fieldExpr.get(), rootSlot, outputs));
        }
        // When there's only one field in the document _id expression, 'Nothing' is converted to
        // 'Null'.
        // TODO SERVER-21992: Remove the following block because this block emulates the classic
        // engine's buggy behavior. With index that can handle 'Nothing' and 'Null' differently,
        // SERVER-21992 issue goes away and the distinct scan should be able to return 'Nothing'
        // and 'Null' separately.
        if (exprs.size() == 1) {
            exprs[0] = b.makeFillEmptyNull(std::move(exprs[0]));
        }
    } else {
        // The group-by field may end up being 'Nothing' and in that case _id: null will be
        // returned. Calling 'makeFillEmptyNull' for the group-by field takes care of that.
        exprs.emplace_back(
            b.makeFillEmptyNull(generateExpression(state, idExpr, rootSlot, outputs)));
    }

    return exprs;
}

SbExpr getTopBottomNValueExpr(StageBuilderState& state,
                              const AccumulationStatement& accStmt,
                              const PlanStageSlots& outputs) {
    SbExprBuilder b(state);

    auto expObj = dynamic_cast<ExpressionObject*>(accStmt.expr.argument.get());
    auto expConst = dynamic_cast<ExpressionConstant*>(accStmt.expr.argument.get());

    tassert(5807015,
            str::stream() << accStmt.expr.name << " accumulator must have an object argument",
            expObj || (expConst && expConst->getValue().isObject()));

    if (expObj) {
        for (auto& [key, value] : expObj->getChildExpressions()) {
            if (key == AccumulatorN::kFieldNameOutput) {
                auto rootSlot = outputs.getResultObjIfExists();
                auto outputExpr = generateExpression(state, value.get(), rootSlot, outputs);
                return b.makeFillEmptyNull(std::move(outputExpr));
            }
        }
    } else {
        auto objConst = expConst->getValue();
        auto objBson = objConst.getDocument().toBson();
        auto outputField = objBson.getField(AccumulatorN::kFieldNameOutput);
        if (outputField.ok()) {
            auto [outputTag, outputVal] = sbe::bson::convertFrom<false /* View */>(outputField);
            auto outputExpr = b.makeConstant(outputTag, outputVal);
            return b.makeFillEmptyNull(std::move(outputExpr));
        }
    }

    tasserted(5807016,
              str::stream() << accStmt.expr.name
                            << " accumulator must have an output field in the argument");
}

SbExpr getTopBottomNSortByExpr(StageBuilderState& state,
                               const PlanStageSlots& outputs,
                               SbExpr sortSpecExpr) {
    SbExprBuilder b(state);

    auto rootSlot = outputs.getResultObjIfExists();
    auto collatorSlot = state.getCollatorSlot();

    tassert(5807014, "Expected root slot to be set", rootSlot);

    auto key = collatorSlot
        ? b.makeFunction("generateCheapSortKey",
                         std::move(sortSpecExpr),
                         SbVar{*rootSlot},
                         SbVar{*collatorSlot})
        : b.makeFunction("generateCheapSortKey", std::move(sortSpecExpr), SbVar{*rootSlot});

    return b.makeFunction("sortKeyComponentVectorToArray", std::move(key));
}

AccumulatorArgs generateAccumulatorArgs(StageBuilderState& state,
                                        const AccumulationStatement& accStmt,
                                        const PlanStageSlots& outputs) {
    auto rootSlot = outputs.getResultObjIfExists();
    auto collatorSlot = state.getCollatorSlot();

    // For $topN and $bottomN, we need to pass multiple SbExprs to buildAccumulatorArgs()
    // (an "input" expression and a "sortBy" expression).
    if (isTopBottomN(accStmt)) {
        StringDataMap<SbExpr> accArgs;
        auto spec = SbExpr{state.getSortSpecSlot(&accStmt)};

        accArgs.emplace(AccArgs::kValue, getTopBottomNValueExpr(state, accStmt, outputs));
        accArgs.emplace(AccArgs::kSortBy, getTopBottomNSortByExpr(state, outputs, std::move(spec)));
        accArgs.emplace(AccArgs::kSortSpec, SbExpr{state.getSortSpecSlot(&accStmt)});

        return buildAccumulatorArgs(state, accStmt, std::move(accArgs), collatorSlot);
    }

    // For all other accumulators, we call generateExpression() on 'argument' to create an SbExpr
    // and then we pass this SbExpr to buildAccumulatorArgs().
    auto argExpr = generateExpression(state, accStmt.expr.argument.get(), rootSlot, outputs);
    return buildAccumulatorArgs(state, accStmt, std::move(argExpr), collatorSlot);
}

std::vector<AccumulatorArgs> generateAllAccumulatorArgs(StageBuilderState& state,
                                                        const GroupNode& groupNode,
                                                        const PlanStageSlots& outputs) {
    std::vector<AccumulatorArgs> accArgsVec;
    for (const auto& accStmt : groupNode.accumulators) {
        // One accumulator may be translated to multiple accumulator expressions. For example, The
        // $avg will have two accumulators expressions, a sum(..) and a count which is implemented
        // as sum(1).
        accArgsVec.emplace_back(generateAccumulatorArgs(state, accStmt, outputs));
    }

    return accArgsVec;
}

/**
 * This function generates one or more SbAggExprs for the specified accumulator ('accStmt')
 * and appends these SbAggExprs to the end of the 'sbAggExprs' vector. This function returns
 * a 'size_t' indicating how many SbAggExprs were appended to 'sbAggExprs'.
 *
 * If 'genBlockAggs' is true, generateAccumulator() accumulator may fail, in which case it
 * will leave the 'sbAggExprs' vector unmodified and return 0.
 */
size_t generateAccumulator(StageBuilderState& state,
                           const AccumulationStatement& accStmt,
                           const PlanStageSlots& outputs,
                           AccumulatorArgs accArgs,
                           boost::optional<SbSlot> initRootSlot,
                           bool genBlockAggs,
                           boost::optional<SbSlot> bitmapInternalSlot,
                           boost::optional<SbSlot> accInternalSlot,
                           SbAggExprVector& sbAggExprs) {
    SbExprBuilder b(state);

    // Generate the agg expressions (and blockAgg expressions too if 'genBlockAggs' is true).
    std::vector<BlockAggAndRowAgg> blockAggsAndRowAggs;
    auto collatorSlot = state.getCollatorSlot();

    if (!genBlockAggs) {
        // Handle the case where we only want to generate "normal" aggs without blockAggs.
        SbExpr::Vector aggs;

        aggs = stage_builder::buildAccumulator(
            accStmt, std::move(accArgs.first), std::move(accArgs.second), collatorSlot, state);

        for (size_t i = 0; i < aggs.size(); ++i) {
            blockAggsAndRowAggs.emplace_back(BlockAggAndRowAgg{SbExpr{}, std::move(aggs[i])});
        }
    } else {
        // Handle the case where we want to generate aggs _and_ blockAggs.
        tassert(
            8448600, "Expected 'bitmapInternalSlot' to be defined", bitmapInternalSlot.has_value());
        tassert(8448601, "Expected 'accInternalSlot' to be defined", accInternalSlot.has_value());

        blockAggsAndRowAggs = stage_builder::buildBlockAccumulator(accStmt,
                                                                   std::move(accArgs.first),
                                                                   std::move(accArgs.second),
                                                                   *bitmapInternalSlot,
                                                                   *accInternalSlot,
                                                                   collatorSlot,
                                                                   state);

        // If 'genBlockAggs' is true and we weren't able to generate block aggs for 'accStmt',
        // then we return 0 to indicate failure.
        if (blockAggsAndRowAggs.empty()) {
            return 0;
        }
    }

    // Generate the init expressions.
    auto inits = [&]() {
        PlanStageSlots slots;
        if (initRootSlot) {
            slots.setResultObj(*initRootSlot);
        }

        StringDataMap<SbExpr> initExprs;
        if (isAccumulatorN(accStmt)) {
            initExprs.emplace(
                AccArgs::kMaxSize,
                generateExpression(state, accStmt.expr.initializer.get(), initRootSlot, slots));
            initExprs.emplace(
                AccArgs::kIsGroupAccum,
                b.makeConstant(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true)));
        } else {
            initExprs.emplace(
                "", generateExpression(state, accStmt.expr.initializer.get(), initRootSlot, slots));
        }

        if (initExprs.size() == 1) {
            return stage_builder::buildInitialize(
                accStmt, std::move(initExprs.begin()->second), state);
        } else {
            return stage_builder::buildInitialize(accStmt, std::move(initExprs), state);
        }
    }();

    tassert(7567301,
            "The accumulation and initialization expression should have the same length",
            inits.size() == blockAggsAndRowAggs.size());

    // For each 'init' / 'blockAgg' / 'agg' expression tuple, wrap the expressions in
    // an SbAggExpr and append the SbAggExpr to 'sbAggExprs'.
    for (size_t i = 0; i < blockAggsAndRowAggs.size(); i++) {
        auto& init = inits[i];
        auto& blockAgg = blockAggsAndRowAggs[i].blockAgg;
        auto& rowAgg = blockAggsAndRowAggs[i].rowAgg;

        sbAggExprs.emplace_back(SbAggExpr{std::move(init), std::move(blockAgg), std::move(rowAgg)},
                                boost::none);
    }

    // Return the number of SbAggExprs that we appended to 'sbAggExprs'.
    return blockAggsAndRowAggs.size();
}

/**
 * This function generates a vector of SbAggExprs that correspond to the accumulators from
 * the specified GroupNode ('groupNode') and returns it, together with a 'vector<size_t>'
 * which indicates how many SbAggExprs were generated for each accumulator from 'groupNode'.
 *
 * If 'genBlockAggs' is true, generateAllAccumulators() accumulator may fail, in which case
 * it will return a pair of empty vectors.
 */
std::pair<SbAggExprVector, std::vector<size_t>> generateAllAccumulators(
    StageBuilderState& state,
    const GroupNode& groupNode,
    const PlanStageSlots& childOutputs,
    std::vector<AccumulatorArgs> accArgsVec,
    boost::optional<SbSlot> initRootSlot,
    bool genBlockAggs,
    boost::optional<SbSlot> bitmapInternalSlot,
    boost::optional<SbSlot> accInternalSlot) {
    // Loop over 'groupNode.accumulators' and populate 'sbAggExprs' and 'accNumSbAggExprs'.
    SbAggExprVector sbAggExprs;
    std::vector<size_t> accNumSbAggExprs;

    size_t i = 0;
    for (const auto& accStmt : groupNode.accumulators) {
        size_t numSbAggExprs = generateAccumulator(state,
                                                   accStmt,
                                                   childOutputs,
                                                   std::move(accArgsVec[i]),
                                                   initRootSlot,
                                                   genBlockAggs,
                                                   bitmapInternalSlot,
                                                   accInternalSlot,
                                                   sbAggExprs);

        // If 'genBlockAggs' is true and we weren't able to generate block aggs for 'accStmt',
        // then we return a pair of empty vectors to indicate failure. The caller will check
        // if the returned vectors are empty and deal with it as appropriate.
        if (genBlockAggs && numSbAggExprs == 0) {
            return {SbAggExprVector{}, std::vector<size_t>{}};
        }

        tassert(8448602, "Expected accumulator to generate at least one agg", numSbAggExprs > 0);

        accNumSbAggExprs.emplace_back(numSbAggExprs);

        ++i;
    }

    return {std::move(sbAggExprs), std::move(accNumSbAggExprs)};
}

/**
 * Generate a vector of (inputSlot, mergingExpression) pairs. The slot (whose id is allocated by
 * this function) will be used to store spilled partial aggregate values that have been recovered
 * from disk and deserialized. The merging expression is an agg function which combines these
 * partial aggregates.
 *
 * Usually the returned vector will be of length 1, but in some cases the MQL accumulation statement
 * is implemented by calculating multiple separate aggregates in the SBE plan, which are finalized
 * by a subsequent project stage to produce the ultimate value.
 */
SbExprSbSlotVector generateMergingExpressions(StageBuilderState& state,
                                              const AccumulationStatement& accStmt,
                                              int numInputSlots) {
    tassert(7039555, "'numInputSlots' must be positive", numInputSlots > 0);
    auto slotIdGenerator = state.slotIdGenerator;
    tassert(7039556, "expected non-null 'slotIdGenerator' pointer", slotIdGenerator);
    auto frameIdGenerator = state.frameIdGenerator;
    tassert(7039557, "expected non-null 'frameIdGenerator' pointer", frameIdGenerator);

    auto collatorSlot = state.getCollatorSlot();

    SbSlotVector spillSlots;
    for (int i = 0; i < numInputSlots; ++i) {
        spillSlots.emplace_back(SbSlot{slotIdGenerator->generate()});
    }

    auto mergingExprs = [&]() {
        if (isTopBottomN(accStmt)) {
            StringDataMap<SbExpr> mergeArgs;
            mergeArgs.emplace(AccArgs::kSortSpec, SbExpr{state.getSortSpecSlot(&accStmt)});
            return buildCombinePartialAggregates(
                accStmt, spillSlots, std::move(mergeArgs), collatorSlot, state);
        } else {
            return buildCombinePartialAggregates(accStmt, spillSlots, collatorSlot, state);
        }
    }();

    // Zip the slot vector and expression vector into a vector of pairs.
    tassert(7039550,
            "expected same number of slots and input exprs",
            spillSlots.size() == mergingExprs.size());
    SbExprSbSlotVector result;
    result.reserve(spillSlots.size());
    for (size_t i = 0; i < spillSlots.size(); ++i) {
        result.emplace_back(std::pair(std::move(mergingExprs[i]), spillSlots[i]));
    }
    return result;
}

/**
 * This function generates all of the merging expressions needed by the accumulators from the
 * specified GroupNode ('groupNode').
 */
SbExprSbSlotVector generateAllMergingExprs(StageBuilderState& state,
                                           const GroupNode& groupNode,
                                           const std::vector<size_t>& accNumSbAggExprs) {
    // Since partial accumulator state may be spilled to disk and then merged, we must construct not
    // only the basic agg expressions for each accumulator, but also agg expressions that are used
    // to combine partial aggregates that have been spilled to disk.
    SbExprSbSlotVector mergingExprs;
    size_t accIdx = 0;

    for (const auto& accStmt : groupNode.accumulators) {
        size_t n = accNumSbAggExprs[accIdx];
        SbExprSbSlotVector curMergingExprs = generateMergingExpressions(state, accStmt, n);

        mergingExprs.insert(mergingExprs.end(),
                            std::make_move_iterator(curMergingExprs.begin()),
                            std::make_move_iterator(curMergingExprs.end()));
        ++accIdx;
    }

    return mergingExprs;
}

/**
 * This function performs any computations needed after the HashAggStage (or BlockHashAggStage)
 * for the accumulators from 'groupNode'.
 *
 * generateGroupFinalStage() returns a tuple containing the updated SBE stage tree, a list of
 * output field names and a list of output field slots (corresponding to the accumulators from
 * 'groupNode'), and a new empty PlanStageSlots object.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots> generateGroupFinalStage(
    StageBuilderState& state,
    SbStage groupStage,
    PlanStageSlots outputs,
    SbSlotVector& individualSlots,
    SbSlotVector groupBySlots,
    SbSlotVector groupOutSlots,
    const std::vector<size_t>& accNumSbAggExprs,
    const GroupNode& groupNode,
    bool idIsSingleKey,
    SbExpr idConstantValue) {
    SbBuilder b(state, groupNode.nodeId());

    SbExpr idFinalExpr;

    if (idConstantValue) {
        // If '_id' is a constant, use the constant value for 'idExpr'.
        idFinalExpr = std::move(idConstantValue);
    } else if (idIsSingleKey) {
        // Otherwise, if '_id' is a single key, use the sole groupBy slot for 'idExpr'.
        idFinalExpr = SbExpr{groupBySlots[0]};
    } else {
        // Otherwise, create the appropriate "newObj(..)" expression and store it in 'idExpr'.
        const auto& idExpr = groupNode.groupByExpression;
        auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr.get());
        tassert(8620900, "Expected expression of type ExpressionObject", idExprObj != nullptr);

        std::vector<std::string> fieldNames;
        for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
            fieldNames.emplace_back(fieldName);
        }

        SbExpr::Vector exprs;
        size_t i = 0;
        for (const auto& slot : groupBySlots) {
            exprs.emplace_back(b.makeStrConstant(fieldNames[i]));
            exprs.emplace_back(slot);
            ++i;
        }

        idFinalExpr = b.makeFunction("newObj"_sd, std::move(exprs));
    }

    std::vector<SbSlotVector> aggSlotsVec;
    auto groupOutSlotsIt = groupOutSlots.begin();

    for (auto n : accNumSbAggExprs) {
        aggSlotsVec.emplace_back(SbSlotVector(groupOutSlotsIt, groupOutSlotsIt + n));
        groupOutSlotsIt += n;
    }

    auto collatorSlot = state.getCollatorSlot();

    // Prepare to project 'idFinalExpr' to a slot.
    SbExprOptSbSlotVector projects;
    projects.emplace_back(std::move(idFinalExpr), boost::none);

    const auto& accStmts = groupNode.accumulators;

    // Generate all the finalize expressions and prepare to project all these expressions
    // to slots.
    std::vector<std::string> fieldNames{"_id"};
    size_t idxAccFirstSlot = 0;
    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        // Gathers field names for the output object from accumulator statements.
        fieldNames.push_back(accStmts[idxAcc].fieldName);

        const auto& accStmt = accStmts[idxAcc];
        SbExpr finalExpr;

        if (isTopBottomN(accStmt)) {
            StringDataMap<SbExpr> finalArgs;
            finalArgs.emplace(AccArgs::kSortSpec, SbExpr{state.getSortSpecSlot(&accStmt)});
            finalExpr = buildFinalize(
                state, accStmts[idxAcc], aggSlotsVec[idxAcc], std::move(finalArgs), collatorSlot);
        } else {
            finalExpr = buildFinalize(state, accStmts[idxAcc], aggSlotsVec[idxAcc], collatorSlot);
        }

        // buildFinalize() might not return an expression if the final step is trivial.
        // For example, $first and $last's final steps are trivial.
        if (!finalExpr) {
            projects.emplace_back(groupOutSlots[idxAccFirstSlot], boost::none);
        } else {
            projects.emplace_back(std::move(finalExpr), boost::none);
        }

        // Some accumulator(s) like $avg generate multiple expressions and slots. So, need to
        // advance this index by the number of those slots for each accumulator.
        idxAccFirstSlot += aggSlotsVec[idxAcc].size();
    }

    // Project all the aforementioned expressions to slots.
    auto [retStage, finalSlots] = b.makeProject(
        std::move(groupStage), buildVariableTypes(outputs, individualSlots), std::move(projects));

    individualSlots.insert(individualSlots.end(), finalSlots.begin(), finalSlots.end());

    return {std::move(retStage), std::move(fieldNames), std::move(finalSlots), std::move(outputs)};
}

/**
 * This function generates a HashAggStage or a BlockHashAggStage as appropriate for the specified
 * GroupNode ('groupNode').
 *
 * buildGroupAggregation() returns a tuple containing the updated SBE plan tree, the list of
 * slots corresponding to the group by inputs, and the list of accumulator output slots
 * corresponding to the accumulators from 'groupNode'.
 */
MONGO_COMPILER_NOINLINE
std::tuple<SbStage, SbSlotVector, SbSlotVector> buildGroupAggregation(
    StageBuilderState& state,
    const PlanStageSlots& childOutputs,
    SbSlotVector individualSlots,
    SbStage stage,
    bool allowDiskUse,
    SbExpr::Vector groupByExprs,
    SbAggExprVector sbAggExprs,
    const std::vector<size_t>& accNumSbAggExprs,
    SbExprSbSlotVector mergingExprs,
    bool useBlockHashAgg,
    SbExpr::Vector blockAccArgExprs,
    const SbSlotVector& blockAccInternalArgSlots,
    boost::optional<SbSlot> bitmapInternalSlot,
    boost::optional<SbSlot> accInternalSlot,
    PlanYieldPolicy* yieldPolicy,
    PlanNodeId nodeId) {
    constexpr auto kBlockSelectivityBitmap = PlanStageSlots::kBlockSelectivityBitmap;

    SbBuilder b(state, nodeId);

    // Project the group by expressions and the accumulator arg expressions to slots.
    SbExprOptSbSlotVector projects;
    size_t numGroupByExprs = groupByExprs.size();

    for (auto& expr : groupByExprs) {
        projects.emplace_back(std::move(expr), boost::none);
    }
    for (auto& expr : blockAccArgExprs) {
        projects.emplace_back(std::move(expr), boost::none);
    }

    auto [outStage, outSlots] = b.makeProject(
        std::move(stage), buildVariableTypes(childOutputs, individualSlots), std::move(projects));
    stage = std::move(outStage);

    SbSlotVector groupBySlots;
    SbSlotVector blockAccArgSlots;

    for (size_t i = 0; i < numGroupByExprs; ++i) {
        groupBySlots.emplace_back(outSlots[i]);
    }
    for (size_t i = numGroupByExprs; i < outSlots.size(); ++i) {
        blockAccArgSlots.emplace_back(outSlots[i]);
    }

    individualSlots.insert(individualSlots.end(), groupBySlots.begin(), groupBySlots.end());
    individualSlots.insert(individualSlots.end(), blockAccArgSlots.begin(), blockAccArgSlots.end());

    // Builds a group stage with accumulator expressions and group-by slot(s).
    auto [hashAggStage, groupByOutSlots, aggSlots] = [&] {
        if (useBlockHashAgg) {
            tassert(8448603,
                    "Expected 'bitmapInternalSlot' to be defined",
                    bitmapInternalSlot.has_value());
            tassert(
                8448604, "Expected 'accInternalSlot' to be defined", accInternalSlot.has_value());

            return b.makeBlockHashAgg(std::move(stage),
                                      buildVariableTypes(childOutputs, individualSlots),
                                      groupBySlots,
                                      std::move(sbAggExprs),
                                      childOutputs.get(kBlockSelectivityBitmap),
                                      blockAccArgSlots,
                                      blockAccInternalArgSlots,
                                      *bitmapInternalSlot,
                                      *accInternalSlot,
                                      allowDiskUse,
                                      yieldPolicy);
        } else {
            return b.makeHashAgg(std::move(stage),
                                 buildVariableTypes(childOutputs, individualSlots),
                                 groupBySlots,
                                 std::move(sbAggExprs),
                                 state.getCollatorSlot(),
                                 allowDiskUse,
                                 std::move(mergingExprs),
                                 yieldPolicy);
        }
    }();

    stage = std::move(hashAggStage);

    return {std::move(stage), std::move(groupByOutSlots), std::move(aggSlots)};
}

/**
 * This function generates the kResult object at the end of $group when needed.
 */
std::pair<SbStage, SbSlot> generateGroupResultObject(SbStage stage,
                                                     StageBuilderState& state,
                                                     const GroupNode* groupNode,
                                                     const std::vector<std::string>& fieldNames,
                                                     const SbSlotVector& finalSlots) {
    SbBuilder b(state, groupNode->nodeId());

    SbExpr::Vector funcArgs;
    for (size_t i = 0; i < fieldNames.size(); ++i) {
        funcArgs.emplace_back(b.makeStrConstant(fieldNames[i]));
        funcArgs.emplace_back(finalSlots[i]);
    }

    StringData newObjFn = groupNode->shouldProduceBson ? "newBsonObj"_sd : "newObj"_sd;
    auto outputExpr = b.makeFunction(newObjFn, std::move(funcArgs));

    auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(outputExpr));
    stage = std::move(outStage);

    auto slot = outSlots[0];
    slot.setTypeSignature(TypeSignature::kObjectType);

    return {std::move(stage), slot};
}

/**
 * This function generates the "root slot" for initializer expressions when it is needed.
 */
std::tuple<SbStage, SbExpr::Vector, SbSlot> generateInitRootSlot(
    SbStage stage,
    StageBuilderState& state,
    const PlanStageSlots& childOutputs,
    SbSlotVector& individualSlots,
    SbExpr::Vector groupByExprs,
    bool vectorizedGroupByExprs,
    ExpressionObject* idExprObj,
    boost::optional<SbSlot> slotIdForInitRoot,
    PlanNodeId nodeId) {
    SbBuilder b(state, nodeId);

    bool idIsSingleKey = idExprObj == nullptr;

    // If there is more than one groupBy key, combine them all into a single object and
    // then use that object as sole groupBy key.
    if (!idIsSingleKey) {
        std::vector<std::string> fieldNames;
        for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
            fieldNames.emplace_back(fieldName);
        }

        SbExpr::Vector exprs;
        size_t i = 0;
        for (const auto& e : groupByExprs) {
            exprs.emplace_back(b.makeStrConstant(fieldNames[i]));
            exprs.emplace_back(e.clone());
            ++i;
        }

        groupByExprs.clear();
        groupByExprs.emplace_back(b.makeFunction("newObj"_sd, std::move(exprs)));

        idIsSingleKey = true;
    }

    SbExpr& groupByExpr = groupByExprs[0];

    bool idIsKnownToBeObj = [&] {
        if (idExprObj != nullptr) {
            return true;
        } else if (groupByExpr.isConstantExpr() && !vectorizedGroupByExprs) {
            auto [tag, _] = groupByExpr.getConstantValue();
            return stage_builder::getTypeSignature(tag).isSubset(TypeSignature::kObjectType);
        }
        return false;
    }();

    // Project 'groupByExpr' to a slot.
    auto targetSlot = idIsKnownToBeObj ? slotIdForInitRoot : boost::none;
    auto [projectStage, projectOutSlots] =
        b.makeProject(std::move(stage),
                      buildVariableTypes(childOutputs),
                      SbExprOptSbSlotPair{std::move(groupByExpr), targetSlot});
    stage = std::move(projectStage);

    groupByExpr = SbExpr{projectOutSlots[0]};
    individualSlots.emplace_back(projectOutSlots[0]);

    // As per the mql semantics add a project expression 'isObject(_id) ? _id : {}'
    // which will be provided as root to initializer expression.
    if (idIsKnownToBeObj) {
        // If we know '_id' is an object, then we can just use the slot as-is.
        return {std::move(stage), std::move(groupByExprs), projectOutSlots[0]};
    } else {
        // If we're not sure whether '_id' is an object, then we need to project the
        // aforementioned expression to a slot and use that.
        auto [emptyObjTag, emptyObjVal] = sbe::value::makeNewObject();
        auto idOrEmptyObjExpr = b.makeIf(b.makeFunction("isObject"_sd, groupByExpr.clone()),
                                         groupByExpr.clone(),
                                         b.makeConstant(emptyObjTag, emptyObjVal));

        auto [outStage, outSlots] =
            b.makeProject(std::move(stage),
                          buildVariableTypes(childOutputs, individualSlots),
                          SbExprOptSbSlotPair{std::move(idOrEmptyObjExpr), slotIdForInitRoot});
        stage = std::move(outStage);

        outSlots[0].setTypeSignature(TypeSignature::kObjectType);
        individualSlots.emplace_back(outSlots[0]);

        return {std::move(stage), std::move(groupByExprs), outSlots[0]};
    }
}
}  // namespace

/**
 * Translates a 'GroupNode' QSN into a sbe::PlanStage tree. This translation logic assumes that the
 * only child of the 'GroupNode' must return an Object (or 'BSONObject') and the translated sub-tree
 * must return 'BSONObject'. The returned 'BSONObject' will always have an "_id" field for the group
 * key and zero or more field(s) for accumulators.
 *
 * For example, a QSN tree: GroupNode(nodeId=2) over a CollectionScanNode(nodeId=1), we would have
 * the following translated sbe::PlanStage tree. In this example, we assume that the $group pipeline
 * spec is {"_id": "$a", "x": {"$min": "$b"}, "y": {"$first": "$b"}}.
 *
 * [2] mkbson s12 [_id = s8, x = s11, y = s10] true false
 * [2] project [s11 = (s9 ?: null)]
 * [2] group [s8] [s9 = min(
 *   let [
 *      l1.0 = s5
 *  ]
 *  in
 *      if (typeMatch(l1.0, 1088ll) ?: true)
 *      then Nothing
 *      else l1.0
 * ), s10 = first((s5 ?: null))]
 * [2] project [s8 = (s4 ?: null)]
 * [1] scan s6 s7 none none none none [s4 = a, s5 = b] @<collUuid> true false
 */
std::pair<SbStage, PlanStageSlots> SlotBasedStageBuilder::buildGroup(const QuerySolutionNode* root,
                                                                     const PlanStageReqs& reqs) {
    tassert(6023414, "buildGroup() does not support kSortKey", !reqs.hasSortKeys());

    auto groupNode = static_cast<const GroupNode*>(root);

    tassert(
        5851600, "should have one and only one child for GROUP", groupNode->children.size() == 1);
    tassert(
        6360401,
        "GROUP cannot propagate a record id slot, but the record id was requested by the parent",
        !reqs.has(kRecordId));

    const auto& childNode = groupNode->children[0].get();

    // Builds the child and gets the child result slot. If the GroupNode doesn't need the full
    // result object, then we can process block values.
    auto childReqs = computeChildReqsForGroup(reqs, *groupNode);
    childReqs.setCanProcessBlockValues(!childReqs.hasResult());

    auto [childStage, childOutputs] = build(childNode, childReqs);
    auto stage = std::move(childStage);

    // Build the group stage in a separate helper method, so that the variables that are not needed
    // to setup the recursive call to build() don't consume precious stack.
    auto [outStage, fieldNames, finalSlots, outputs] =
        buildGroupImpl(std::move(stage), reqs, std::move(childOutputs), groupNode);
    stage = std::move(outStage);

    const auto& accStmts = groupNode->accumulators;

    tassert(5851605,
            "The number of final slots must be as 1 (the final group-by slot) + the number of acc "
            "slots",
            finalSlots.size() == 1 + accStmts.size());

    auto fieldNamesSet = StringDataSet{fieldNames.begin(), fieldNames.end()};
    auto [fields, additionalFields] =
        splitVector(reqs.getFields(), [&](const std::string& s) { return fieldNamesSet.count(s); });
    auto fieldsSet = StringDataSet{fields.begin(), fields.end()};

    for (size_t i = 0; i < fieldNames.size(); ++i) {
        if (fieldsSet.count(fieldNames[i])) {
            outputs.set(std::make_pair(PlanStageSlots::kField, fieldNames[i]), finalSlots[i]);
        }
    };

    // Builds a stage to create a result object out of a group-by slot and gathered accumulator
    // result slots if the parent node requests so.
    if (reqs.hasResult() || !additionalFields.empty()) {
        auto [outStage, outSlot] =
            generateGroupResultObject(std::move(stage), _state, groupNode, fieldNames, finalSlots);
        stage = std::move(outStage);

        outputs.setResultObj(outSlot);
    }

    return {std::move(stage), std::move(outputs)};
}

/**
 * This function is called by buildGroup() and it contains most of the implementation for $group.
 *
 * It takes the GroupNode, the child's SBE stage tree, and the PlanStageSlots generated by the child
 * as input, and it returns a tuple containing the updated SBE stage tree, a list of output field
 * names and a list of output field slots (corresponding to the accumulators from the GroupNode),
 * and a new empty PlanStageSlots object.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots>
SlotBasedStageBuilder::buildGroupImpl(SbStage stage,
                                      const PlanStageReqs& reqs,
                                      PlanStageSlots childOutputs,
                                      const GroupNode* groupNode) {
    const auto& idExpr = groupNode->groupByExpression;
    const auto nodeId = groupNode->nodeId();
    SbBuilder b(_state, nodeId);

    tassert(5851601, "GROUP should have had group-by key expression", idExpr);

    // Collect all the ExpressionFieldPaths referenced by from 'groupNode'.
    StringMap<const ExpressionFieldPath*> groupFieldMap = collectFieldPaths(groupNode);

    // If 'groupFieldMap' is not empty, then we evaluate all of the ExpressionFieldPaths in
    // 'groupFieldMap', project the results to slots, and finally we put the slots into
    // 'childOutputs' as kPathExpr slots.
    if (!groupFieldMap.empty()) {
        // At present, the kPathExpr optimization is not compatible with block processing, so
        // when 'groupFieldMap' isn't empty we need to close the block processing pipeline here.
        if (childOutputs.hasBlockOutput()) {
            stage = buildBlockToRow(std::move(stage), _state, childOutputs);
        }

        stage = projectFieldPathsToPathExprSlots(
            _state, *groupNode, std::move(stage), childOutputs, groupFieldMap);
    }

    // Check if any of the accumulators have a variable initializer.
    bool hasVariableGroupInit = false;
    for (const auto& accStmt : groupNode->accumulators) {
        hasVariableGroupInit =
            hasVariableGroupInit || !ExpressionConstant::isNullOrConstant(accStmt.expr.initializer);
    }

    // Generate expressions for the group by keys.
    SbExpr::Vector groupByExprs = generateGroupByKeyExprs(_state, idExpr.get(), childOutputs);

    auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr.get());
    bool idIsSingleKey = idExprObj == nullptr;
    bool vectorizedGroupByExprs = false;

    if (childOutputs.hasBlockOutput()) {
        // Try to vectorize all the group keys.
        for (auto& sbExpr : groupByExprs) {
            sbExpr = buildVectorizedExpr(_state, std::move(sbExpr), childOutputs, false);
        }

        // If some expressions could not be vectorized, rebuild everything after transitioning to
        // scalar.
        if (std::any_of(groupByExprs.begin(), groupByExprs.end(), [](const SbExpr& expr) {
                return expr.isNull();
            })) {
            stage = buildBlockToRow(std::move(stage), _state, childOutputs);

            // buildBlockToRow() just made a bunch of changes to 'childOutputs', so we need
            // to re-generate 'groupByExprs'.
            groupByExprs = generateGroupByKeyExprs(_state, idExpr.get(), childOutputs);
        } else {
            vectorizedGroupByExprs = true;
        }
    }

    if (!vectorizedGroupByExprs) {
        // If we didn't vectorize the groupBy expressions call optimize() on them so that the call
        // to "isConstantExpr()" below can recognize more cases where the groupBy expr is constant.
        auto varTypes = buildVariableTypes(childOutputs);
        for (auto& sbExpr : groupByExprs) {
            sbExpr.optimize(_state, varTypes);
        }
    }

    // Generate all the input arg expressions for all of the accumulators.
    std::vector<AccumulatorArgs> accArgsVec =
        generateAllAccumulatorArgs(_state, *groupNode, childOutputs);

    // The 'individualSlots' vector is used to keep track of all the slots that are currently
    // "active" that are not present in 'childOutputs'. This vector is used together with
    // 'childOutputs' when we need to do constant-folding / type analysis and vectorization.
    SbSlotVector individualSlots;

    std::vector<AccumulatorArgs> blockAccArgsVec;
    bool vectorizedAccumulatorArgs = false;

    // TODO SERVER-87560 re-enable block hashagg end-to-end.
    bool tryToVectorizeAccumulatorArgs = false;

    // If the necessary conditions are met, try to vectorize 'accArgsVec'.
    if (tryToVectorizeAccumulatorArgs) {
        bool vectorizationFailed = false;

        // Loop over 'accArgsVec' and try to vectorize each 'AccumulatorArgs' object.
        for (const AccumulatorArgs& accArgs : accArgsVec) {
            SbExpr blockAccArgExpr;
            boost::optional<TypeSignature> typeSig;
            bool isBlockType = false;

            if (accArgs.first.size() == 1) {
                const SbExpr& accArgExpr = accArgs.first[0];

                blockAccArgExpr =
                    buildVectorizedExpr(_state, accArgExpr.clone(), childOutputs, false);
                typeSig = blockAccArgExpr.getTypeSignature();
                isBlockType = typeSig && TypeSignature::kBlockType.isSubset(*typeSig);
            }

            if (blockAccArgExpr && isBlockType) {
                const std::vector<std::string>& accArgNames = accArgs.second;

                blockAccArgsVec.emplace_back(
                    AccumulatorArgs{SbExpr::makeSeq(std::move(blockAccArgExpr)), accArgNames});
            } else {
                // If we couldn't vectorize one of the accumulator args, then we give up entirely
                // on any further vectorization and fallback to using scalar values with the
                // normal HashAggStage.
                blockAccArgsVec.clear();
                vectorizationFailed = true;
                break;
            }
        }

        vectorizedAccumulatorArgs = !vectorizationFailed;
    }

    // If one or more accumulators has a variable initializer, then we will eventually need
    // need to set up 'initRootSlot' later in this function.
    //
    // For now we just reserve a slot ID for 'initRootSlot' so that we can pass the slot ID to
    // generateAllAccumulators(). Later we will make sure that 'initRootSlot' actually gets
    // populated.
    auto slotIdForInitRoot =
        hasVariableGroupInit ? boost::make_optional(SbSlot{_state.slotId()}) : boost::none;

    // Generate agg expressions and block agg expressions for all the accumulators.
    bool genBlockAggs = vectorizedAccumulatorArgs;

    // If we're generating block aggs, then for each SbExpr in 'blockAccArgsVec' we replace
    // the SbExpr with an internal slot, we store the SbExpr into 'blockAccArgExprs', and we
    // store the internal slot into 'blockAccInternalArgSlots'.
    SbExpr::Vector blockAccArgExprs;
    SbSlotVector blockAccInternalArgSlots;

    if (genBlockAggs) {
        for (auto& blockAccArgs : blockAccArgsVec) {
            tassert(8448605, "Expected single expression", blockAccArgs.first.size() == 1);

            blockAccArgExprs.emplace_back(std::move(blockAccArgs.first[0]));

            auto internalSlot = SbSlot{_state.slotId()};
            blockAccInternalArgSlots.emplace_back(internalSlot);

            auto updatedArgsVector = SbExpr::makeSeq(SbExpr{internalSlot});
            std::vector<std::string> accArgNames = std::move(blockAccArgs.second);

            blockAccArgs = AccumulatorArgs{std::move(updatedArgsVector), std::move(accArgNames)};
        }
    }

    // If 'genBlockAggs' is false then we use the normal accumulator args, otherwise we use
    // the transformed "block" args that we created above.
    auto& accArgsParam = genBlockAggs ? blockAccArgsVec : accArgsVec;

    // If 'genBlockAggs' is true, then we need to generate a couple of internal slots for
    // generateAllAccumulators().
    auto bitmapInternalSlot =
        genBlockAggs ? boost::make_optional(SbSlot{_state.slotId()}) : boost::none;
    auto accInternalSlot =
        genBlockAggs ? boost::make_optional(SbSlot{_state.slotId()}) : boost::none;

    // Generate the SbAggExprs for all the accumulators from 'groupNode'.
    auto [sbAggExprs, accNumSbAggExprs] = generateAllAccumulators(_state,
                                                                  *groupNode,
                                                                  childOutputs,
                                                                  std::move(accArgsParam),
                                                                  slotIdForInitRoot,
                                                                  genBlockAggs,
                                                                  bitmapInternalSlot,
                                                                  accInternalSlot);

    // Check if all aggs support block processing.
    bool allAggsHaveBlockExprs = false;

    if (genBlockAggs && !sbAggExprs.empty()) {
        allAggsHaveBlockExprs = std::all_of(sbAggExprs.begin(), sbAggExprs.end(), [](auto&& e) {
            return !e.first.blockAgg.isNull();
        });
    }

    bool useBlockHashAgg = childOutputs.hasBlockOutput() && allAggsHaveBlockExprs;

    // If all the necessary conditions to use BlockHashAggStage are not met, then close the block
    // processing pipeline here.
    if (childOutputs.hasBlockOutput() && !useBlockHashAgg) {
        SbExprOptSbSlotVector projects;
        for (size_t i = 0; i < groupByExprs.size(); ++i) {
            projects.emplace_back(std::move(groupByExprs[i]), boost::none);
        }

        auto [projectStage, groupBySlots] =
            b.makeProject(std::move(stage), buildVariableTypes(childOutputs), std::move(projects));

        auto [outStage, outSlots] =
            buildBlockToRow(std::move(projectStage), _state, childOutputs, std::move(groupBySlots));
        stage = std::move(outStage);

        for (size_t i = 0; i < groupByExprs.size(); ++i) {
            groupByExprs[i] = outSlots[i];
        }

        individualSlots = outSlots;

        // Remove the block projections that have been prepared, we are going to build the scalar
        // version now.
        blockAccArgExprs.clear();

        // buildBlockToRow() just made a bunch of changes to 'childOutputs', so we need
        // to re-generate 'accArgsVec', 'sbAggExprs', and 'accNumSbAggExprs'.
        accArgsVec = generateAllAccumulatorArgs(_state, *groupNode, childOutputs);

        genBlockAggs = false;
        bitmapInternalSlot = boost::none;
        accInternalSlot = boost::none;
        std::tie(sbAggExprs, accNumSbAggExprs) = generateAllAccumulators(_state,
                                                                         *groupNode,
                                                                         childOutputs,
                                                                         std::move(accArgsVec),
                                                                         slotIdForInitRoot,
                                                                         genBlockAggs,
                                                                         bitmapInternalSlot,
                                                                         accInternalSlot);
    }

    // If one or more accumulators has a variable initializer, then we need to set up
    // 'initRootSlot'.
    boost::optional<SbSlot> initRootSlot;

    if (hasVariableGroupInit) {
        auto [outStage, outExprs, outSlot] = generateInitRootSlot(std::move(stage),
                                                                  _state,
                                                                  childOutputs,
                                                                  individualSlots,
                                                                  std::move(groupByExprs),
                                                                  vectorizedGroupByExprs,
                                                                  idExprObj,
                                                                  slotIdForInitRoot,
                                                                  nodeId);
        stage = std::move(outStage);
        groupByExprs = std::move(outExprs);
        initRootSlot.emplace(outSlot);

        idIsSingleKey = true;
    }

    // Generate merging expressions for all the accumulators.
    auto mergingExprs = generateAllMergingExprs(_state, *groupNode, accNumSbAggExprs);

    // If there is a single groupBy key that didn't get vectorized and is constant, and if none of
    // the accumulators had a variable initializer, then we set 'idConstantValue' and we clear the
    // the 'groupByExprs' vector.
    SbExpr idConstantValue;

    if (idIsSingleKey && !vectorizedGroupByExprs && groupByExprs[0].isConstantExpr() &&
        !hasVariableGroupInit) {
        idConstantValue = std::move(groupByExprs[0]);
        groupByExprs.clear();
    }

    // Build the HashAggStage or the BlockHashAggStage.
    auto [outStage, groupBySlots, groupOutSlots] =
        buildGroupAggregation(_state,
                              childOutputs,
                              std::move(individualSlots),
                              std::move(stage),
                              _cq.getExpCtx()->allowDiskUse,
                              std::move(groupByExprs),
                              std::move(sbAggExprs),
                              accNumSbAggExprs,
                              std::move(mergingExprs),
                              useBlockHashAgg,
                              std::move(blockAccArgExprs),
                              blockAccInternalArgSlots,
                              bitmapInternalSlot,
                              accInternalSlot,
                              _yieldPolicy,
                              nodeId);
    stage = std::move(outStage);

    // Initialize a new PlanStageSlots object ('outputs').
    PlanStageSlots outputs;

    // After the HashAgg/BlockHashAgg stage, the only slots that are "active" are the group-by slots
    // ('groupBySlots') and the output slots for the accumulators from groupNode ('groupOutSlots').
    individualSlots = groupBySlots;
    individualSlots.insert(individualSlots.end(), groupOutSlots.begin(), groupOutSlots.end());

    if (useBlockHashAgg) {
        tassert(8448606,
                "Expected at least one group by slot or agg out slot",
                !groupBySlots.empty() || !groupOutSlots.empty());

        // This stage re-maps the selectivity bitset slot.
        outputs.set(PlanStageSlots::kBlockSelectivityBitmap,
                    childOutputs.get(PlanStageSlots::kBlockSelectivityBitmap));
    }

    // For now we unconditionally end the block processing pipeline here.
    if (outputs.hasBlockOutput()) {
        auto hashAggOutSlots = groupBySlots;
        hashAggOutSlots.insert(hashAggOutSlots.end(), groupOutSlots.begin(), groupOutSlots.end());

        auto [outStage, blockToRowOutSlots] =
            buildBlockToRow(std::move(stage), _state, outputs, std::move(hashAggOutSlots));
        stage = std::move(outStage);

        for (size_t i = 0; i < groupBySlots.size(); ++i) {
            groupBySlots[i] = blockToRowOutSlots[i];
        }
        for (size_t i = 0; i < groupOutSlots.size(); ++i) {
            size_t blockToRowOutSlotsIdx = groupBySlots.size() + i;
            groupOutSlots[i] = blockToRowOutSlots[blockToRowOutSlotsIdx];
        }

        // buildBlockToRow() just made a bunch of changes to 'groupBySlots' and 'groupOutSlots',
        // so we need to re-generate 'individualSlots'.
        individualSlots = groupBySlots;
        individualSlots.insert(individualSlots.end(), groupOutSlots.begin(), groupOutSlots.end());
    }

    // Builds the final stage(s) over the collected accumulators.
    return generateGroupFinalStage(_state,
                                   std::move(stage),
                                   std::move(outputs),
                                   individualSlots,
                                   std::move(groupBySlots),
                                   std::move(groupOutSlots),
                                   accNumSbAggExprs,
                                   *groupNode,
                                   idIsSingleKey,
                                   std::move(idConstantValue));
}
}  // namespace mongo::stage_builder
