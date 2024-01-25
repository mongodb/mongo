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
// of a $group, populate a slot in 'childOutputs' for each path found. Each slot is bound via a
// ProjectStage to an EExpression that evaluates the path traversal.
MONGO_COMPILER_NOINLINE
SbStage projectPathTraversalsForGroupBy(
    StageBuilderState& state,
    const GroupNode& groupNode,
    SbStage childStage,
    PlanStageSlots& childOutputs,
    const StringMap<const ExpressionFieldPath*>& groupFieldMap) {
    // Slot to EExpression map that tracks path traversal expressions. Note that this only contains
    // expressions corresponding to paths which require traversals (that is, if there exists a
    // top level field slot corresponding to a field, we take care not to add it to 'projects' to
    // avoid rebinding a slot).
    sbe::SlotExprPairVector projects;

    // Populates 'projects' and 'childOutputs' with an expression and/or a slot, respectively,
    // corresponding to the value of 'fieldExpr'.
    for (auto& fp : groupFieldMap) {
        // Either find a slot corresponding to it or generate an expression for it and bind it to a
        // slot.
        TypedSlot slot;
        auto result = stage_builder::generateExpression(
            state, fp.second, childOutputs.getResultObjIfExists(), &childOutputs);

        if (result.hasSlot()) {
            slot = TypedSlot{*result.getSlot(), TypeSignature::kAnyScalarType};
        } else {
            auto newSlot = state.slotId();
            auto expr = result.extractExpr(state);
            projects.emplace_back(newSlot, std::move(expr.expr));
            slot = TypedSlot{newSlot, expr.typeSignature};
        }
        childOutputs.set(std::make_pair(PlanStageSlots::kPathExpr, fp.first), slot);
    }

    if (!projects.empty()) {
        childStage = makeProject(std::move(childStage), std::move(projects), groupNode.nodeId());
    }

    return childStage;
}

MONGO_COMPILER_NOINLINE
std::tuple<sbe::value::SlotVector, SbStage, SbExpr> generateGroupByObjKey(
    StageBuilderState& state,
    ExpressionObject* idExprObj,
    const PlanStageSlots& outputs,
    SbStage stage,
    PlanNodeId nodeId) {
    SbExprBuilder b(state);

    auto rootSlot = outputs.getResultObjIfExists();

    VariableTypes varTypes = buildVariableTypes(outputs);
    sbe::value::SlotVector slots;
    SbExpr::Vector exprs;
    sbe::SlotExprPairVector projects;

    for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
        auto abt = abt::unwrap(
            generateExpression(state, fieldExpr.get(), rootSlot, &outputs).extractABT());

        auto slot = state.slotId();
        projects.emplace_back(slot, abtToExpr(abt, state, &varTypes).expr);

        slots.push_back(slot);
        exprs.emplace_back(b.makeStrConstant(fieldName));
        exprs.emplace_back(SbVar{slot});
    }

    if (!projects.empty()) {
        stage = makeProject(std::move(stage), std::move(projects), nodeId);
    }

    // When there's only one field in the document _id expression, 'Nothing' is converted to
    // 'Null'.
    // TODO SERVER-21992: Remove the following block because this block emulates the classic
    // engine's buggy behavior. With index that can handle 'Nothing' and 'Null' differently,
    // SERVER-21992 issue goes away and the distinct scan should be able to return 'Nothing' and
    // 'Null' separately.
    if (slots.size() == 1) {
        auto slot = state.slotId();

        auto e = b.makeFillEmptyNull(std::move(exprs[1]));
        stage = makeProject(std::move(stage), nodeId, slot, e.extractExpr(state).expr);

        slots[0] = slot;
        exprs[1] = SbVar{slots[0]};
    }

    // Composes the _id document and assigns a slot to the result using 'newObj' function if _id
    // should produce a document.
    auto newObjExpr = b.makeFunction("newObj"_sd, std::move(exprs));
    return {slots, std::move(stage), std::move(newObjExpr)};
}

MONGO_COMPILER_NOINLINE
std::tuple<sbe::value::SlotVector, SbStage, SbExpr> generateGroupBySingleKey(
    StageBuilderState& state,
    const boost::intrusive_ptr<Expression>& idExpr,
    const PlanStageSlots& outputs,
    SbStage stage,
    PlanNodeId nodeId) {
    auto rootSlot = outputs.getResultObjIfExists();
    // The group-by field may end up being 'Nothing' and in that case _id: null will be
    // returned. Calling 'makeFillEmptyNull' for the group-by field takes care of that.
    SbExprBuilder b(state);
    auto groupBySbExpr =
        b.makeFillEmptyNull(generateExpression(state, idExpr.get(), rootSlot, &outputs));
    VariableTypes varTypes = buildVariableTypes(outputs);
    auto groupByExpr = groupBySbExpr.extractExpr(state, &varTypes);

    if (auto groupByExprConstant = groupByExpr.expr->as<sbe::EConstant>(); groupByExprConstant) {
        return {sbe::value::SlotVector{}, std::move(stage), std::move(groupByExpr.expr)};
    } else {
        auto slot = state.slotId();
        stage = makeProject(std::move(stage), nodeId, slot, std::move(groupByExpr.expr));
        return {sbe::value::SlotVector{slot}, std::move(stage), SbExpr{}};
    }
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

sbe::value::SlotVector generateAccumulator(StageBuilderState& state,
                                           const AccumulationStatement& accStmt,
                                           const PlanStageSlots& outputs,
                                           sbe::value::SlotIdGenerator* slotIdGenerator,
                                           sbe::AggExprVector& aggSlotExprs,
                                           boost::optional<TypedSlot> initializerRootSlot) {
    SbExprBuilder b(state);

    auto rootSlot = outputs.getResultObjIfExists();
    auto collatorSlot = state.getCollatorSlot();

    // One accumulator may be translated to multiple accumulator expressions. For example, The
    // $avg will have two accumulators expressions, a sum(..) and a count which is implemented
    // as sum(1).
    auto accExprs = [&]() {
        // $topN/$bottomN accumulators require multiple arguments to the accumulator builder.
        if (isTopBottomN(accStmt)) {
            StringDataMap<SbExpr> accArgs;

            auto sortSpecExpr = getSortSpecFromTopBottomN(accStmt, state);
            accArgs.emplace(AccArgs::kTopBottomNSortSpec, sortSpecExpr.clone());

            // Build the key expression for the accumulator.
            tassert(5807014,
                    str::stream() << accStmt.expr.name
                                  << " accumulator must have the root slot set",
                    rootSlot);
            auto key = collatorSlot ? b.makeFunction("generateCheapSortKey",
                                                     std::move(sortSpecExpr),
                                                     SbVar{rootSlot->slotId},
                                                     SbVar{*collatorSlot})
                                    : b.makeFunction("generateCheapSortKey",
                                                     std::move(sortSpecExpr),
                                                     SbVar{rootSlot->slotId});
            accArgs.emplace(AccArgs::kTopBottomNKey,
                            b.makeFunction("sortKeyComponentVectorToArray", std::move(key)));

            // Build the value expression for the accumulator.
            if (auto expObj = dynamic_cast<ExpressionObject*>(accStmt.expr.argument.get())) {
                for (auto& [key, value] : expObj->getChildExpressions()) {
                    if (key == AccumulatorN::kFieldNameOutput) {
                        auto outputExpr =
                            generateExpression(state, value.get(), rootSlot, &outputs);
                        accArgs.emplace(AccArgs::kTopBottomNValue,
                                        b.makeFillEmptyNull(std::move(outputExpr)));
                        break;
                    }
                }
            } else if (auto expConst =
                           dynamic_cast<ExpressionConstant*>(accStmt.expr.argument.get())) {
                auto objConst = expConst->getValue();
                tassert(7767100,
                        str::stream()
                            << accStmt.expr.name << " accumulator must have an object argument",
                        objConst.isObject());
                auto objBson = objConst.getDocument().toBson();
                auto outputField = objBson.getField(AccumulatorN::kFieldNameOutput);
                if (outputField.ok()) {
                    auto [outputTag, outputVal] =
                        sbe::bson::convertFrom<false /* View */>(outputField);
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

            auto accExprs =
                stage_builder::buildAccumulator(accStmt, std::move(accArgs), collatorSlot, state);

            return accExprs;
        } else {
            auto argExpr =
                generateExpression(state, accStmt.expr.argument.get(), rootSlot, &outputs);

            auto accExprs =
                stage_builder::buildAccumulator(accStmt, std::move(argExpr), collatorSlot, state);

            return accExprs;
        }
    }();

    auto initExprs = [&]() {
        StringDataMap<SbExpr> initExprArgs;
        if (isAccumulatorN(accStmt)) {
            initExprArgs.emplace(
                AccArgs::kMaxSize,
                generateExpression(
                    state, accStmt.expr.initializer.get(), initializerRootSlot, nullptr));
            initExprArgs.emplace(
                AccArgs::kIsGroupAccum,
                b.makeConstant(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true)));
        } else {
            initExprArgs.emplace(
                "",
                generateExpression(
                    state, accStmt.expr.initializer.get(), initializerRootSlot, nullptr));
        }
        return initExprArgs;
    }();
    auto accInitExprs = [&]() {
        if (initExprs.size() == 1) {
            return stage_builder::buildInitialize(
                accStmt, std::move(initExprs.begin()->second), state);
        } else {
            return stage_builder::buildInitialize(accStmt, std::move(initExprs), state);
        }
    }();

    tassert(7567301,
            "The accumulation and initialization expression should have the same length",
            accExprs.size() == accInitExprs.size());
    sbe::value::SlotVector aggSlots;
    for (size_t i = 0; i < accExprs.size(); i++) {
        auto slot = slotIdGenerator->generate();
        aggSlots.push_back(slot);
        aggSlotExprs.push_back(
            std::make_pair(slot,
                           sbe::AggExprPair{std::move(accInitExprs[i].extractExpr(state).expr),
                                            std::move(accExprs[i].extractExpr(state).expr)}));
    }

    return aggSlots;
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
sbe::SlotExprPairVector generateMergingExpressions(StageBuilderState& state,
                                                   const AccumulationStatement& accStmt,
                                                   int numInputSlots) {
    tassert(7039555, "'numInputSlots' must be positive", numInputSlots > 0);
    auto slotIdGenerator = state.slotIdGenerator;
    tassert(7039556, "expected non-null 'slotIdGenerator' pointer", slotIdGenerator);
    auto frameIdGenerator = state.frameIdGenerator;
    tassert(7039557, "expected non-null 'frameIdGenerator' pointer", frameIdGenerator);

    auto spillSlots = slotIdGenerator->generateMultiple(numInputSlots);
    auto collatorSlot = state.getCollatorSlot();

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
    sbe::SlotExprPairVector result;
    result.reserve(spillSlots.size());
    for (size_t i = 0; i < spillSlots.size(); ++i) {
        result.push_back({spillSlots[i], mergingExprs[i].extractExpr(state).expr});
    }
    return result;
}

// Given a sequence 'groupBySlots' of slot ids, return a new sequence that contains all slots ids in
// 'groupBySlots' but without any duplicate ids.
sbe::value::SlotVector dedupGroupBySlots(const sbe::value::SlotVector& groupBySlots) {
    stdx::unordered_set<sbe::value::SlotId> uniqueSlots;
    sbe::value::SlotVector dedupedGroupBySlots;

    for (auto slot : groupBySlots) {
        if (!uniqueSlots.contains(slot)) {
            dedupedGroupBySlots.emplace_back(slot);
            uniqueSlots.insert(slot);
        }
    }

    return dedupedGroupBySlots;
}

std::tuple<std::vector<std::string>, sbe::value::SlotVector, SbStage> generateGroupFinalStage(
    StageBuilderState& state,
    SbStage groupStage,
    sbe::value::SlotVector groupOutSlots,
    SbExpr idFinalExpr,
    sbe::value::SlotVector dedupedGroupBySlots,
    const std::vector<AccumulationStatement>& accStmts,
    const std::vector<sbe::value::SlotVector>& aggSlotsVec,
    PlanNodeId nodeId) {
    sbe::SlotExprPairVector projects;
    // To passthrough the output slots of accumulators with trivial finalizers, we need to find
    // their slot ids. We can do this by sorting 'groupStage.outSlots' because the slot ids
    // correspond to the order in which the accumulators were translated (that is, the order in
    // which they are listed in 'accStmts'). Note, that 'groupStage.outSlots' contains deduped
    // group-by slots at the front and the accumulator slots at the back.
    std::sort(groupOutSlots.begin() + dedupedGroupBySlots.size(), groupOutSlots.end());

    tassert(5995100,
            "The _id expression must either produce an expression or a scalar value",
            idFinalExpr || dedupedGroupBySlots.size() == 1);

    auto finalGroupBySlot = [&]() {
        if (!idFinalExpr) {
            return dedupedGroupBySlots[0];
        } else {
            auto slot = state.slotId();
            projects.emplace_back(slot, idFinalExpr.extractExpr(state).expr);
            return slot;
        }
    }();

    auto collatorSlot = state.getCollatorSlot();
    auto finalSlots{sbe::value::SlotVector{finalGroupBySlot}};
    std::vector<std::string> fieldNames{"_id"};
    size_t idxAccFirstSlot = dedupedGroupBySlots.size();
    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        // Gathers field names for the output object from accumulator statements.
        fieldNames.push_back(accStmts[idxAcc].fieldName);

        auto finalExpr = [&]() {
            const auto& accStmt = accStmts[idxAcc];
            if (isTopBottomN(accStmt)) {
                StringDataMap<SbExpr> finalArgs;
                finalArgs.emplace(AccArgs::kTopBottomNSortSpec,
                                  getSortSpecFromTopBottomN(accStmt, state));
                return buildFinalize(state,
                                     accStmts[idxAcc],
                                     aggSlotsVec[idxAcc],
                                     std::move(finalArgs),
                                     collatorSlot);
            } else {
                return buildFinalize(state, accStmts[idxAcc], aggSlotsVec[idxAcc], collatorSlot);
            }
        }();

        // The final step may not return an expression if it's trivial. For example, $first and
        // $last's final steps are trivial.
        if (finalExpr) {
            auto outSlot = state.slotId();
            finalSlots.push_back(outSlot);
            projects.emplace_back(outSlot, finalExpr.extractExpr(state).expr);
        } else {
            finalSlots.push_back(groupOutSlots[idxAccFirstSlot]);
        }

        // Some accumulator(s) like $avg generate multiple expressions and slots. So, need to
        // advance this index by the number of those slots for each accumulator.
        idxAccFirstSlot += aggSlotsVec[idxAcc].size();
    }

    // Gathers all accumulator results. If there're no project expressions, does not add a project
    // stage.
    auto retStage = projects.empty()
        ? std::move(groupStage)
        : makeProject(std::move(groupStage), std::move(projects), nodeId);

    return {std::move(fieldNames), std::move(finalSlots), std::move(retStage)};
}

// Generate the accumulator expressions and HashAgg operator used to compute a $group pipeline
// stage.
MONGO_COMPILER_NOINLINE
std::tuple<std::vector<std::string>, sbe::value::SlotVector, SbStage> buildGroupAggregation(
    StageBuilderState& state,
    const GroupNode& groupNode,
    bool allowDiskUse,
    SbExpr idFinalExpr,
    const PlanStageSlots& childOutputs,
    SbStage groupByStage,
    sbe::value::SlotVector& groupBySlots) {
    SbExprBuilder b(state);

    auto nodeId = groupNode.nodeId();

    auto initializerRootSlot = [&]() {
        bool isVariableGroupInitializer = false;
        for (const auto& accStmt : groupNode.accumulators) {
            isVariableGroupInitializer = isVariableGroupInitializer ||
                !ExpressionConstant::isNullOrConstant(accStmt.expr.initializer);
        }
        if (!isVariableGroupInitializer) {
            return boost::optional<TypedSlot>{};
        }

        sbe::value::SlotId idSlot;
        // We materialize the groupId before the group stage to provide it as root to
        // initializer expression
        if (idFinalExpr) {
            auto slot = state.slotId();
            groupByStage = makeProject(
                std::move(groupByStage), nodeId, slot, idFinalExpr.extractExpr(state).expr);

            groupBySlots.clear();
            groupBySlots.push_back(slot);
            idFinalExpr.reset();
            idSlot = slot;
        } else {
            idSlot = groupBySlots[0];
        }

        // As per the mql semantics add a project expression 'isObject(id) ? id : {}'
        // which will be provided as root to initializer expression
        auto [emptyObjTag, emptyObjVal] = sbe::value::makeNewObject();
        auto isObjectExpr = b.makeIf(b.makeFunction("isObject"_sd, SbVar{idSlot}),
                                     SbVar{idSlot},
                                     b.makeConstant(emptyObjTag, emptyObjVal));

        auto isObjSlot = state.slotId();
        groupByStage = makeProject(
            std::move(groupByStage), nodeId, isObjSlot, isObjectExpr.extractExpr(state).expr);

        return boost::optional<TypedSlot>(TypedSlot{isObjSlot, TypeSignature::kObjectType});
    }();

    // Translates accumulators which are executed inside the group stage and gets slots for
    // accumulators.
    auto currentStage = std::move(groupByStage);
    sbe::AggExprVector aggSlotExprs;
    std::vector<sbe::value::SlotVector> aggSlotsVec;
    // Since partial accumulator state may be spilled to disk and then merged, we must construct not
    // only the basic agg expressions for each accumulator, but also agg expressions that are used
    // to combine partial aggregates that have been spilled to disk.
    sbe::SlotExprPairVector mergingExprs;
    for (const auto& accStmt : groupNode.accumulators) {
        sbe::value::SlotVector curAggSlots = generateAccumulator(
            state, accStmt, childOutputs, state.slotIdGenerator, aggSlotExprs, initializerRootSlot);

        sbe::SlotExprPairVector curMergingExprs =
            generateMergingExpressions(state, accStmt, curAggSlots.size());

        aggSlotsVec.emplace_back(std::move(curAggSlots));
        mergingExprs.insert(mergingExprs.end(),
                            std::make_move_iterator(curMergingExprs.begin()),
                            std::make_move_iterator(curMergingExprs.end()));
    }

    // There might be duplicated expressions and slots. Dedup them before creating a HashAgg
    // because it would complain about duplicated slots and refuse to be created, which is
    // reasonable because duplicated expressions would not contribute to grouping.
    auto dedupedGroupBySlots = dedupGroupBySlots(groupBySlots);

    auto groupOutSlots = dedupedGroupBySlots;
    for (auto& [slot, _] : aggSlotExprs) {
        groupOutSlots.push_back(slot);
    }

    // Builds a group stage with accumulator expressions and group-by slot(s).
    currentStage = makeHashAgg(std::move(currentStage),
                               dedupedGroupBySlots,
                               std::move(aggSlotExprs),
                               state.getCollatorSlot(),
                               allowDiskUse,
                               std::move(mergingExprs),
                               nodeId);

    tassert(
        5851603,
        "Group stage's output slots must include deduped slots for group-by keys and slots for all "
        "accumulators",
        groupOutSlots.size() ==
            std::accumulate(aggSlotsVec.begin(),
                            aggSlotsVec.end(),
                            dedupedGroupBySlots.size(),
                            [](int sum, const auto& aggSlots) { return sum + aggSlots.size(); }));
    tassert(
        5851604,
        "Group stage's output slots must contain the deduped groupBySlots at the front",
        std::equal(dedupedGroupBySlots.begin(), dedupedGroupBySlots.end(), groupOutSlots.begin()));


    // Builds the final stage(s) over the collected accumulators.
    return generateGroupFinalStage(state,
                                   std::move(currentStage),
                                   std::move(groupOutSlots),
                                   std::move(idFinalExpr),
                                   dedupedGroupBySlots,
                                   groupNode.accumulators,
                                   aggSlotsVec,
                                   nodeId);
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
    std::unique_ptr<sbe::PlanStage> childStage,
    PlanStageSlots childOutputs) {
    auto nodeId = groupNode->nodeId();

    const auto& idExpr = groupNode->groupByExpression;
    tassert(5851601, "GROUP should have had group-by key expression", idExpr);

    const auto& accStmts = groupNode->accumulators;

    // Map of field paths referenced by group. Useful for de-duplicating fields and clearing the
    // slots corresponding to fields in 'childOutputs' so that they are not mistakenly referenced by
    // parent stages.
    StringMap<const ExpressionFieldPath*> groupFieldMap = collectFieldPaths(groupNode);
    if (!groupFieldMap.empty()) {
        // If we have block values in input, convert them to scalar values before computing the
        // projection.
        if (childOutputs.hasBlockOutput()) {
            childStage = buildBlockToRow(std::move(childStage), childOutputs);
        }

        childStage = projectPathTraversalsForGroupBy(
            _state, *groupNode, std::move(childStage), childOutputs, groupFieldMap);
    }

    sbe::value::SlotVector groupBySlots;
    SbStage groupByStage;
    SbExpr idFinalExpr;

    SbExprBuilder b(_state);

    // If we have an object as group id, let's stop block processing immediately and build
    // the required projection to create it. If we have a single expression, we can try to
    // vectorize it.
    if (auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr.get()); idExprObj) {
        if (childOutputs.hasBlockOutput()) {
            childStage = buildBlockToRow(std::move(childStage), childOutputs);
        }
        std::tie(groupBySlots, groupByStage, idFinalExpr) =
            generateGroupByObjKey(_state, idExprObj, childOutputs, std::move(childStage), nodeId);
    } else {
        // Attempt to use a block-enabled project stage.
        if (childOutputs.hasBlockOutput()) {
            // The group-by field may end up being 'Nothing' and in that case _id: null will be
            // returned. Calling 'makeFillEmptyNull' for the group-by field takes care of that.
            auto groupByBlockExpr = buildVectorizedExpr(
                b.makeFillEmptyNull(generateExpression(
                    _state, idExpr.get(), childOutputs.getResultObjIfExists(), &childOutputs)),
                childOutputs,
                false);
            if (groupByBlockExpr.has_value() &&
                TypeSignature::kBlockType.isSubset(groupByBlockExpr->typeSignature)) {

                // Make up a temporary field name so that we can register the slot in the
                // childOutputs, as buildBlockToRow below reads from it to get the slots to convert
                // to scalar. Use a Base64-encoded UUID to minimize the chance of colliding with
                // existing fields.
                std::array<unsigned char, 16> nonce = UUID::gen().data();
                auto groupIdFieldName = std::make_pair(PlanStageSlots::kField,
                                                       base64::encode(nonce.data(), sizeof(nonce)));

                auto slot = _state.slotId();
                groupByStage = makeProject(
                    std::move(childStage), nodeId, slot, std::move(groupByBlockExpr->expr));
                groupBySlots = sbe::value::SlotVector{slot};
                childOutputs.set(groupIdFieldName,
                                 TypedSlot{slot, groupByBlockExpr->typeSignature});

                // TODO: remove this section when $group is able to digest block values.
                // For now, we have to close the block processing pipeline here, remove the
                // slot we just added and replace the slot used as group id with the scalar
                // result.
                groupByStage = buildBlockToRow(std::move(groupByStage), childOutputs);
                groupBySlots[0] = childOutputs.get(groupIdFieldName).slotId;
                childOutputs.clear(groupIdFieldName);
            } else {
                // Vectorization is not possible, stop block processing now.
                childStage = buildBlockToRow(std::move(childStage), childOutputs);
            }
        }

        // If vectorization wasn't needed, or if it failed, create the scalar projection.
        if (!groupByStage) {
            std::tie(groupBySlots, groupByStage, idFinalExpr) = generateGroupBySingleKey(
                _state, idExpr, childOutputs, std::move(childStage), nodeId);
        }
    }

    auto [fieldNames, finalSlots, outStage] = buildGroupAggregation(_state,
                                                                    *groupNode,
                                                                    _cq.getExpCtx()->allowDiskUse,
                                                                    std::move(idFinalExpr),
                                                                    childOutputs,
                                                                    std::move(groupByStage),
                                                                    groupBySlots);
    auto stage = std::move(outStage);

    tassert(5851605,
            "The number of final slots must be as 1 (the final group-by slot) + the number of acc "
            "slots",
            finalSlots.size() == 1 + accStmts.size());

    // Clear all fields needed by this group stage from 'childOutputs' to avoid references to
    // ExpressionFieldPath values that are no longer visible.
    for (const auto& groupField : groupFieldMap) {
        childOutputs.clear({PlanStageSlots::kPathExpr, StringData(groupField.first)});
    }

    auto fieldNamesSet = StringDataSet{fieldNames.begin(), fieldNames.end()};
    auto [fields, additionalFields] =
        splitVector(reqs.getFields(), [&](const std::string& s) { return fieldNamesSet.count(s); });
    auto fieldsSet = StringDataSet{fields.begin(), fields.end()};

    PlanStageSlots outputs;
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
            funcArgs.emplace_back(SbVar{finalSlots[i]});
        }

        StringData newObjFn = groupNode->shouldProduceBson ? "newBsonObj"_sd : "newObj"_sd;
        auto outputExpr = b.makeFunction(newObjFn, std::move(funcArgs));

        auto slot = _slotIdGenerator.generate();
        stage = makeProject(std::move(stage), nodeId, slot, outputExpr.extractExpr(_state).expr);

        outputs.setResultObj(TypedSlot{slot, TypeSignature::kObjectType});
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
