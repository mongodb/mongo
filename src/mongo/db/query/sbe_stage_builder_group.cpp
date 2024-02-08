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

// Return true iff 'accStmt' is a $topN or $bottomN operator.
bool isTopBottomN(const AccumulationStatement& accStmt) {
    return accStmt.expr.name == AccumulatorTopBottomN<kTop, true>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kBottom, true>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kTop, false>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kBottom, false>::getName();
}

// Return true iff 'accStmt' is one of $topN, $bottomN, $minN, $maxN, $firstN or $lastN.
bool isAccumulatorN(const AccumulationStatement& accStmt) {
    return accStmt.expr.name == AccumulatorTopBottomN<kTop, true>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kBottom, true>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kTop, false>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kBottom, false>::getName() ||
        accStmt.expr.name == AccumulatorMinN::getName() ||
        accStmt.expr.name == AccumulatorMaxN::getName() ||
        accStmt.expr.name == AccumulatorFirstN::getName() ||
        accStmt.expr.name == AccumulatorLastN::getName();
}

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

// Compute what values 'groupNode' will need from its child node in order to build expressions for
// the group-by key ("_id") and the accumulators.
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

// Collect the FieldPath expressions referenced by a GroupNode that should be exposed in a slot for
// the group stage to work properly.
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

// Given a list of field path expressions used in the group-by ('_id') and accumulator expressions
// of a $group, populate a slot in 'outputs' for each path found. Each slot is bound to an SBE
// EExpression (via a ProjectStage) that evaluates the path traversal.
MONGO_COMPILER_NOINLINE
SbStage projectPathTraversalsForGroupBy(
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

template <TopBottomSense sense, bool single>
SbExpr getSortSpecFromTopBottomN(const AccumulatorTopBottomN<sense, single>* acc,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);

    tassert(5807013, "Accumulator state must not be null", acc);
    auto sortPattern =
        acc->getSortPattern().serialize(SortPattern::SortKeySerialization::kForExplain).toBson();
    auto sortSpec = std::make_unique<sbe::SortSpec>(sortPattern);
    auto sortSpecExpr = b.makeConstant(sbe::value::TypeTags::sortSpec,
                                       sbe::value::bitcastFrom<sbe::SortSpec*>(sortSpec.release()));
    return sortSpecExpr;
}

SbExpr getSortSpecFromTopBottomN(const AccumulationStatement& accStmt, StageBuilderState& state) {
    auto acc = accStmt.expr.factory();
    if (accStmt.expr.name == AccumulatorTopBottomN<kTop, true>::getName()) {
        return getSortSpecFromTopBottomN(
            dynamic_cast<AccumulatorTopBottomN<kTop, true>*>(acc.get()), state);
    } else if (accStmt.expr.name == AccumulatorTopBottomN<kBottom, true>::getName()) {
        return getSortSpecFromTopBottomN(
            dynamic_cast<AccumulatorTopBottomN<kBottom, true>*>(acc.get()), state);
    } else if (accStmt.expr.name == AccumulatorTopBottomN<kTop, false>::getName()) {
        return getSortSpecFromTopBottomN(
            dynamic_cast<AccumulatorTopBottomN<kTop, false>*>(acc.get()), state);
    } else if (accStmt.expr.name == AccumulatorTopBottomN<kBottom, false>::getName()) {
        return getSortSpecFromTopBottomN(
            dynamic_cast<AccumulatorTopBottomN<kBottom, false>*>(acc.get()), state);
    } else {
        MONGO_UNREACHABLE;
    }
}

using AccumulatorArgs = std::variant<SbExpr, StringDataMap<SbExpr>>;

AccumulatorArgs generateAccumulatorArgs(StageBuilderState& state,
                                        const AccumulationStatement& accStmt,
                                        const PlanStageSlots& outputs) {
    SbExprBuilder b(state);

    auto rootSlot = outputs.getResultObjIfExists();
    auto collatorSlot = state.getCollatorSlot();

    // One accumulator may be translated to multiple accumulator expressions. For example, The
    // $avg will have two accumulators expressions, a sum(..) and a count which is implemented
    // as sum(1).

    // $topN/$bottomN accumulators require multiple arguments to the accumulator builder.
    if (isTopBottomN(accStmt)) {
        StringDataMap<SbExpr> accArgs;

        auto sortSpecExpr = getSortSpecFromTopBottomN(accStmt, state);
        accArgs.emplace(AccArgs::kTopBottomNSortSpec, sortSpecExpr.clone());

        // Build the key expression for the accumulator.
        tassert(5807014,
                str::stream() << accStmt.expr.name << " accumulator must have the root slot set",
                rootSlot);
        auto key = collatorSlot
            ? b.makeFunction("generateCheapSortKey",
                             std::move(sortSpecExpr),
                             SbVar{*rootSlot},
                             SbVar{*collatorSlot})
            : b.makeFunction("generateCheapSortKey", std::move(sortSpecExpr), SbVar{*rootSlot});
        accArgs.emplace(AccArgs::kTopBottomNKey,
                        b.makeFunction("sortKeyComponentVectorToArray", std::move(key)));

        // Build the value expression for the accumulator.
        if (auto expObj = dynamic_cast<ExpressionObject*>(accStmt.expr.argument.get())) {
            for (auto& [key, value] : expObj->getChildExpressions()) {
                if (key == AccumulatorN::kFieldNameOutput) {
                    auto outputExpr = generateExpression(state, value.get(), rootSlot, outputs);
                    accArgs.emplace(AccArgs::kTopBottomNValue,
                                    b.makeFillEmptyNull(std::move(outputExpr)));
                    break;
                }
            }
        } else if (auto expConst = dynamic_cast<ExpressionConstant*>(accStmt.expr.argument.get())) {
            auto objConst = expConst->getValue();
            tassert(7767100,
                    str::stream() << accStmt.expr.name
                                  << " accumulator must have an object argument",
                    objConst.isObject());
            auto objBson = objConst.getDocument().toBson();
            auto outputField = objBson.getField(AccumulatorN::kFieldNameOutput);
            if (outputField.ok()) {
                auto [outputTag, outputVal] = sbe::bson::convertFrom<false /* View */>(outputField);
                auto outputExpr = b.makeConstant(outputTag, outputVal);
                accArgs.emplace(AccArgs::kTopBottomNValue,
                                b.makeFillEmptyNull(std::move(outputExpr)));
            }
        } else {
            tasserted(5807015,
                      str::stream()
                          << accStmt.expr.name << " accumulator must have an object argument");
        }
        tassert(5807016,
                str::stream() << accStmt.expr.name
                              << " accumulator must have an output field in the argument",
                accArgs.find(AccArgs::kTopBottomNValue) != accArgs.end());

        return AccumulatorArgs{std::move(accArgs)};
    }

    auto argExpr = generateExpression(state, accStmt.expr.argument.get(), rootSlot, outputs);

    return AccumulatorArgs{std::move(argExpr)};
}

std::vector<AccumulatorArgs> generateAllAccumulatorArgs(StageBuilderState& state,
                                                        const GroupNode& groupNode,
                                                        const PlanStageSlots& outputs) {
    std::vector<AccumulatorArgs> accArgsVec;
    for (const auto& accStmt : groupNode.accumulators) {
        accArgsVec.emplace_back(generateAccumulatorArgs(state, accStmt, outputs));
    }

    return accArgsVec;
}

size_t generateAccumulator(StageBuilderState& state,
                           const AccumulationStatement& accStmt,
                           const PlanStageSlots& outputs,
                           AccumulatorArgs accArgs,
                           boost::optional<SbSlot> initRootSlot,
                           SbAggExprVector& sbAggExprs) {
    SbExprBuilder b(state);

    SbExpr::Vector aggs;
    auto collatorSlot = state.getCollatorSlot();

    if (holds_alternative<SbExpr>(accArgs)) {
        SbExpr& arg = get<SbExpr>(accArgs);
        aggs = stage_builder::buildAccumulator(accStmt, std::move(arg), collatorSlot, state);
    } else {
        StringDataMap<SbExpr>& args = get<StringDataMap<SbExpr>>(accArgs);
        aggs = stage_builder::buildAccumulator(accStmt, std::move(args), collatorSlot, state);
    }

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
            inits.size() == aggs.size());

    for (size_t i = 0; i < aggs.size(); i++) {
        auto& init = inits[i];
        auto& agg = aggs[i];

        sbAggExprs.emplace_back(SbAggExpr{std::move(init), SbExpr{}, std::move(agg)}, boost::none);
    }

    return aggs.size();
}

std::pair<SbAggExprVector, std::vector<size_t>> generateAllAccumulators(
    StageBuilderState& state,
    const GroupNode& groupNode,
    const PlanStageSlots& childOutputs,
    std::vector<AccumulatorArgs> accArgsVec,
    boost::optional<SbSlot> initRootSlot) {
    // Loop over 'groupNode.accumulators' and populate 'sbAggExprs' and 'accNumSbAggExprs'.
    SbAggExprVector sbAggExprs;
    std::vector<size_t> accNumSbAggExprs;

    size_t i = 0;
    for (const auto& accStmt : groupNode.accumulators) {
        size_t n = generateAccumulator(
            state, accStmt, childOutputs, std::move(accArgsVec[i]), initRootSlot, sbAggExprs);

        accNumSbAggExprs.emplace_back(n);

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
            mergeArgs.emplace(AccArgs::kTopBottomNSortSpec,
                              getSortSpecFromTopBottomN(accStmt, state));
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

// Given a sequence 'groupBySlots' of slot ids, return a new sequence that contains all slots ids in
// 'groupBySlots' but without any duplicate ids.
SbSlotVector dedupGroupBySlots(const SbSlotVector& groupBySlots) {
    stdx::unordered_set<sbe::value::SlotId> uniqueSlots;
    SbSlotVector dedupedGroupBySlots;

    for (auto slot : groupBySlots) {
        if (!uniqueSlots.contains(slot.getId())) {
            dedupedGroupBySlots.emplace_back(slot);
            uniqueSlots.insert(slot.getId());
        }
    }

    return dedupedGroupBySlots;
}

std::tuple<SbStage, std::vector<std::string>, SbSlotVector> generateGroupFinalStage(
    StageBuilderState& state,
    SbStage groupStage,
    const PlanStageSlots& outputs,
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
        idFinalExpr = std::move(idConstantValue);
    } else if (idIsSingleKey) {
        idFinalExpr = SbExpr{groupBySlots[0]};
    } else {
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

    SbExprOptSbSlotVector projects;
    projects.emplace_back(std::move(idFinalExpr), boost::none);

    const auto& accStmts = groupNode.accumulators;

    std::vector<std::string> fieldNames{"_id"};
    size_t idxAccFirstSlot = 0;
    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        // Gathers field names for the output object from accumulator statements.
        fieldNames.push_back(accStmts[idxAcc].fieldName);

        const auto& accStmt = accStmts[idxAcc];
        SbExpr finalExpr;

        if (isTopBottomN(accStmt)) {
            StringDataMap<SbExpr> finalArgs;
            finalArgs.emplace(AccArgs::kTopBottomNSortSpec,
                              getSortSpecFromTopBottomN(accStmt, state));
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

    // Gathers all accumulator results.
    auto [retStage, finalSlots] = b.makeProject(
        std::move(groupStage), buildVariableTypes(outputs, individualSlots), std::move(projects));

    individualSlots.insert(individualSlots.end(), finalSlots.begin(), finalSlots.end());

    return {std::move(retStage), std::move(fieldNames), std::move(finalSlots)};
}

// Generate the accumulator expressions and HashAgg operator used to compute a $group pipeline
// stage.
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
    PlanNodeId nodeId) {
    SbBuilder b(state, nodeId);

    SbExprOptSbSlotVector projects;
    for (auto& expr : groupByExprs) {
        projects.emplace_back(std::move(expr), boost::none);
    }

    auto [outStage, groupBySlots] = b.makeProject(
        std::move(stage), buildVariableTypes(childOutputs, individualSlots), std::move(projects));
    stage = std::move(outStage);

    individualSlots.insert(individualSlots.end(), groupBySlots.begin(), groupBySlots.end());

    // There might be duplicated expressions and slots. Dedup them before creating a HashAgg
    // because it would complain about duplicated slots and refuse to be created, which is
    // reasonable because duplicated expressions would not contribute to grouping.
    auto dedupedGroupBySlots = dedupGroupBySlots(groupBySlots);

    // Builds a group stage with accumulator expressions and group-by slot(s).
    auto [hashAggStage, groupOutSlots] =
        b.makeHashAgg(std::move(stage),
                      buildVariableTypes(childOutputs, individualSlots),
                      dedupedGroupBySlots,
                      std::move(sbAggExprs),
                      state.getCollatorSlot(),
                      allowDiskUse,
                      std::move(mergingExprs));
    stage = std::move(hashAggStage);

    return {std::move(stage), std::move(groupBySlots), std::move(groupOutSlots)};
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
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildGroup(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023414, "buildGroup() does not support kSortKey", !reqs.hasSortKeys());

    auto groupNode = static_cast<const GroupNode*>(root);

    tassert(
        5851600, "should have one and only one child for GROUP", groupNode->children.size() == 1);
    tassert(
        6360401,
        "GROUP cannot propagate a record id slot, but the record id was requested by the parent",
        !reqs.has(kRecordId));

    const auto& childNode = groupNode->children[0].get();

    // Builds the child and gets the child result slot. If we don't need the full result object, we
    // can process block values.
    auto [childStage, childOutputs] = build(
        childNode,
        computeChildReqsForGroup(reqs, *groupNode).setCanProcessBlockValues(!reqs.hasResultObj()));

    // Build the group stage in a separate helper method, so that the variables that are not needed
    // to setup the recursive call to build() don't consume precious stack.
    return buildGroupImpl(groupNode, reqs, std::move(childStage), std::move(childOutputs));
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildGroupImpl(
    const GroupNode* groupNode,
    const PlanStageReqs& reqs,
    std::unique_ptr<sbe::PlanStage> stage,
    PlanStageSlots childOutputs) {
    const auto nodeId = groupNode->nodeId();
    SbBuilder b(_state, nodeId);

    const auto& idExpr = groupNode->groupByExpression;
    const auto& accStmts = groupNode->accumulators;

    tassert(5851601, "GROUP should have had group-by key expression", idExpr);

    // Map of field paths referenced by group. Useful for de-duplicating fields and clearing the
    // slots corresponding to fields in 'childOutputs' so that they are not mistakenly referenced by
    // parent stages.
    StringMap<const ExpressionFieldPath*> groupFieldMap = collectFieldPaths(groupNode);
    if (!groupFieldMap.empty()) {
        // If we have block values in input, convert them to scalar values before computing the
        // projection.
        if (childOutputs.hasBlockOutput()) {
            stage = buildBlockToRow(std::move(stage), childOutputs);
        }

        stage = projectPathTraversalsForGroupBy(
            _state, *groupNode, std::move(stage), childOutputs, groupFieldMap);
    }

    bool hasVariableGroupInit = false;
    for (const auto& accStmt : groupNode->accumulators) {
        hasVariableGroupInit =
            hasVariableGroupInit || !ExpressionConstant::isNullOrConstant(accStmt.expr.initializer);
    }

    SbExpr::Vector groupByExprs = generateGroupByKeyExprs(_state, idExpr.get(), childOutputs);

    auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr.get());
    bool idIsSingleKey = idExprObj == nullptr;
    bool vectorizedGroupByExprs = false;

    if (childOutputs.hasBlockOutput()) {
        // If we have a single expression, try to vectorize it.
        if (idIsSingleKey) {
            SbExpr groupByBlockExpr =
                buildVectorizedExpr(_state, groupByExprs[0].clone(), childOutputs, false);

            auto typeSig = groupByBlockExpr.getTypeSignature();

            if (groupByBlockExpr && typeSig && TypeSignature::kBlockType.isSubset(*typeSig)) {
                // If vectorization succeed, store the vectorized expression into 'groupByExprs'.
                groupByExprs[0] = std::move(groupByBlockExpr);
                vectorizedGroupByExprs = true;
            }
        }

        // If this expression wasn't eligible for vectorization or if vectorization didn't succeed,
        // then we need to stop block processing now.
        if (!vectorizedGroupByExprs) {
            stage = buildBlockToRow(std::move(stage), childOutputs);

            // buildBlockToRow() just made a bunch of changes to 'childOutputs', so we need
            // to re-generate 'groupByExprs'.
            groupByExprs = generateGroupByKeyExprs(_state, idExpr.get(), childOutputs);
        }
    }

    SbSlotVector individualSlots;

    // For now we unconditionally end the block processing pipeline here.
    if (childOutputs.hasBlockOutput()) {
        SbExprOptSbSlotVector projects;
        for (size_t i = 0; i < groupByExprs.size(); ++i) {
            projects.emplace_back(std::move(groupByExprs[i]), boost::none);
        }

        auto [projectStage, groupBySlots] =
            b.makeProject(std::move(stage), buildVariableTypes(childOutputs), std::move(projects));

        auto [outStage, outSlots] =
            buildBlockToRow(std::move(projectStage), childOutputs, std::move(groupBySlots));
        stage = std::move(outStage);

        for (size_t i = 0; i < groupByExprs.size(); ++i) {
            groupByExprs[i] = outSlots[i];
        }

        individualSlots = std::move(outSlots);
    }

    // If 'idIsSingleKey' is true and we didn't vectorize the sole groupBy expression, then we
    // call optimize() on the groupBy expression so that the call to "isConstantExpr()" below
    // can recognize more cases where the groupBy expr is constant.
    if (!vectorizedGroupByExprs && idIsSingleKey) {
        groupByExprs[0].optimize(_state, buildVariableTypes(childOutputs));
    }

    // Generate all the input arg expressions for all of the accumulators.
    std::vector<AccumulatorArgs> accArgsVec =
        generateAllAccumulatorArgs(_state, *groupNode, childOutputs);

    // If one or more accumulators has a variable initializer, then we need to set up
    // 'initRootSlot'.
    boost::optional<SbSlot> initRootSlot;

    if (hasVariableGroupInit) {
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
        auto [projectStage, projectOutSlots] = b.makeProject(
            std::move(stage), buildVariableTypes(childOutputs), std::move(groupByExpr));
        stage = std::move(projectStage);

        groupByExpr = SbExpr{projectOutSlots[0]};
        individualSlots.emplace_back(projectOutSlots[0]);

        // As per the mql semantics add a project expression 'isObject(id) ? id : {}'
        // which will be provided as root to initializer expression.
        if (idIsKnownToBeObj) {
            initRootSlot.emplace(projectOutSlots[0]);
        } else {
            auto [emptyObjTag, emptyObjVal] = sbe::value::makeNewObject();
            auto idOrEmptyObjExpr = b.makeIf(b.makeFunction("isObject"_sd, groupByExpr.clone()),
                                             groupByExpr.clone(),
                                             b.makeConstant(emptyObjTag, emptyObjVal));

            auto [outStage, outSlots] =
                b.makeProject(std::move(stage),
                              buildVariableTypes(childOutputs, individualSlots),
                              std::move(idOrEmptyObjExpr));
            stage = std::move(outStage);

            outSlots[0].setTypeSignature(TypeSignature::kObjectType);
            individualSlots.emplace_back(outSlots[0]);

            initRootSlot.emplace(outSlots[0]);
        }
    }

    // Generate agg expressions and merging expressions for all the accumulators.
    auto [sbAggExprs, accNumSbAggExprs] = generateAllAccumulators(
        _state, *groupNode, childOutputs, std::move(accArgsVec), initRootSlot);

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

    // Build the HashAggStage.
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
                              nodeId);
    stage = std::move(outStage);

    individualSlots = groupBySlots;
    individualSlots.insert(individualSlots.end(), groupOutSlots.begin(), groupOutSlots.end());

    // Builds the final stage(s) over the collected accumulators.
    PlanStageSlots outputs;

    auto [groupFinalStage, fieldNames, finalSlots] =
        generateGroupFinalStage(_state,
                                std::move(stage),
                                outputs,
                                individualSlots,
                                std::move(groupBySlots),
                                std::move(groupOutSlots),
                                accNumSbAggExprs,
                                *groupNode,
                                idIsSingleKey,
                                std::move(idConstantValue));
    stage = std::move(groupFinalStage);

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
        outputs.setResultObj(slot);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
