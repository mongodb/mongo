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

#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_accumulator.h"
#include "mongo/db/query/stage_builder/sbe/gen_expression.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/util/assert_util.h"

#include <memory>

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
    constexpr bool allowCallGenCheapSortKey = true;

    auto childReqs = reqs.copyForChild().setResultObj().clearAllFields();

    // If the group node references any top level fields, we take all of them and add them to
    // 'childReqs'. Note that this happens regardless of whether we need the whole document because
    // it can be the case that this stage references '$$ROOT' as well as some top level fields.
    if (auto topLevelFields = getTopLevelFields(groupNode.requiredFields);
        !topLevelFields.empty()) {
        childReqs.setFields(std::move(topLevelFields));
    }

    if (!groupNode.needWholeDocument) {
        // Tracks if any sort keys we need to generate depend on the having a materialized
        // result object.

        // Some accumulators (like $top and $bottom) need to generate sort keys. Here we loop
        // over 'groupNode.accumulators' to observe what each accumulator's needs are.
        bool sortKeysNeedRootDoc = false;
        for (const auto& accStmt : groupNode.accumulators) {
            if (auto sortPattern = getSortPattern(accStmt)) {
                auto plan = makeSortKeysPlan(*sortPattern, allowCallGenCheapSortKey);

                if (!plan.fieldsForSortKeys.empty()) {
                    // If this accumulator needs specific top-level fields in slots, add the
                    // appropriate kField reqs to 'childReqs'.
                    childReqs.setFields(std::move(plan.fieldsForSortKeys));
                }

                if (plan.needsResultObj) {
                    // If this accumulator needs the whole result object, set 'sortKeysNeedRootDoc'
                    // to true.
                    sortKeysNeedRootDoc = true;
                }
            }
        }

        // If no accumulator requires the whole result object for generating sort keys, then we
        // can clear the result requirement from 'childReqs'.
        if (!sortKeysNeedRootDoc) {
            childReqs.clearResult();
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

namespace {
struct PartitionedFieldPathExprs {
    StringDataMap<const ExpressionFieldPath*> exprsOnBlockSlots;
    StringDataMap<const ExpressionFieldPath*> exprsOnScalarSlots;
};

/*
 * Returns whether or not the given field path expression references a block. Assumes
 * that we are in block mode.
 */
bool doesExpressionReferenceBlock(const PlanStageSlots& outputs,
                                  const ExpressionFieldPath& expressionFieldPath) {
    tassert(8829002, "Expected outputs to have block output", outputs.hasBlockOutput());

    if (expressionFieldPath.getVariableId() != Variables::kRootId) {
        return false;
    }

    auto& fieldPath = expressionFieldPath.getFieldPath();

    // The first component should be $$CURRENT.
    tassert(
        8829001, "Field path should have more than one component", fieldPath.getPathLength() > 1);

    // Top level field is at index 1.
    auto firstComponent = fieldPath.getFieldName(1);

    // Since we're in block mode, the child MUST provide this kField, as there is no result
    // obj.  Note: in the future, it may be possible that the child provides the full
    // kPathExpr, but not the kField for the top level. This code will have to handle that
    // case.

    auto outputSlot =
        outputs.get(PlanStageReqs::UnownedSlotName(PlanStageReqs::kField, firstComponent));

    // Skip any field path expressions on blocks. Those will be computed after block_to_row.
    if (auto typeSig = outputSlot.getTypeSignature()) {
        if (typeSig->containsAny(TypeSignature::kBlockType.include(TypeSignature::kCellType))) {
            return true;
        }
    }

    return false;
}

/**
 * Splits the given map of field path expressions into those which refer to block fields and those
 * which refer to scalar fields.
 */
MONGO_COMPILER_NOINLINE
PartitionedFieldPathExprs partitionFieldPathExprsByBlock(
    PlanStageSlots& outputs, const StringMap<const ExpressionFieldPath*>& groupFieldMapIn) {

    PartitionedFieldPathExprs out;
    for (auto& [fieldStr, expressionFieldPath] : groupFieldMapIn) {
        if (doesExpressionReferenceBlock(outputs, *expressionFieldPath)) {
            out.exprsOnBlockSlots.emplace(fieldStr, expressionFieldPath);
        } else {
            out.exprsOnScalarSlots.emplace(fieldStr, expressionFieldPath);
        }
    }
    return out;
}
}  // namespace

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
    const StringDataMap<const ExpressionFieldPath*>& groupFieldMap) {
    SbBuilder b(state, groupNode.nodeId());

    SbExprOptSlotVector projects;
    for (auto& fp : groupFieldMap) {
        projects.emplace_back(stage_builder::generateExpression(
                                  state, fp.second, outputs.getResultObjIfExists(), outputs),
                              boost::none);
    }

    if (!projects.empty()) {
        auto [outStage, outSlots] =
            b.makeProject(buildVariableTypes(outputs), std::move(stage), std::move(projects));
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
/**
 * Ensure that all kPathExpr (dotted path field references) reqs are available in slots. Block
 * processing mode does not support dotted path references to block data, so if there are any of
 * those this will add a BlockToRowStage to switch the pipeline to scalar processing.
 */
MONGO_COMPILER_NOINLINE
SbStage makePathExprsAvailableInSlots(
    StageBuilderState& state,
    const GroupNode& groupNode,
    SbStage stage,
    PlanStageSlots& outputs,
    const StringMap<const ExpressionFieldPath*>& groupFieldMapIn) {

    if (groupFieldMapIn.empty()) {
        // No work to do.
        return stage;
    }

    SbBuilder b(state, groupNode.nodeId());

    if (outputs.hasBlockOutput()) {
        // We are currently running in block mode. Some slots will contain blocks, and others may
        // contain scalars. The scalar slots contain values common to the entire block, like the
        // timeseries 'meta' field.

        // First we compute the field path expressions for any expressions which are on scalars. We
        // want to do these before we close the block processing pipeline.

        auto [blockFieldPathExprs, nonBlockFieldPathExprs] =
            partitionFieldPathExprsByBlock(outputs, groupFieldMapIn);

        stage = projectFieldPathsToPathExprSlots(
            state, groupNode, std::move(stage), outputs, nonBlockFieldPathExprs);

        if (blockFieldPathExprs.empty()) {
            // If there are no block field path exprs, we actually don't need to close the block
            // pipeline.
            return stage;
        }

        stage = buildBlockToRow(std::move(stage), state, outputs);


        // Now that we've done the block to row, evaluate the path
        // expressions for the slots that were blocks, and are now scalars.
        stage = projectFieldPathsToPathExprSlots(
            state, groupNode, std::move(stage), outputs, blockFieldPathExprs);
    } else {
        // We have to convert to StringDataMap to call projectFieldPathsToPathExprSlots().
        StringDataMap<const ExpressionFieldPath*> groupFieldMap;
        groupFieldMap.insert(groupFieldMapIn.begin(), groupFieldMapIn.end());
        stage = projectFieldPathsToPathExprSlots(
            state, groupNode, std::move(stage), outputs, groupFieldMap);
    }
    return stage;
}  // makePathExprsAvailableInSlots

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

using TagAndValue = std::pair<sbe::value::TypeTags, sbe::value::Value>;

std::variant<Expression*, TagAndValue> getTopBottomNValueExprHelper(
    const AccumulationStatement& accStmt) {
    auto accOp = AccumOp{accStmt};

    auto expObj = dynamic_cast<ExpressionObject*>(accStmt.expr.argument.get());
    auto expConst =
        !expObj ? dynamic_cast<ExpressionConstant*>(accStmt.expr.argument.get()) : nullptr;

    tassert(5807015,
            str::stream() << accOp.getOpName() << " accumulator must have an object argument",
            expObj || (expConst && expConst->getValue().isObject()));

    if (expObj) {
        for (auto& [key, value] : expObj->getChildExpressions()) {
            if (key == AccumulatorN::kFieldNameOutput) {
                return value.get();
            }
        }
    } else {
        auto objConst = expConst->getValue();
        auto objBson = objConst.getDocument().toBson();
        // `outputField` may reference data in objBson, so must not outlive it.
        auto outputField = objBson.getField(AccumulatorN::kFieldNameOutput);
        if (outputField.ok()) {
            return sbe::bson::convertFrom<false /* not a view */>(outputField);
        }
    }

    tasserted(5807016,
              str::stream() << accOp.getOpName()
                            << " accumulator must have an output field in the argument");
}

SbExpr getTopBottomNValueExpr(StageBuilderState& state,
                              const AccumulationStatement& accStmt,
                              const PlanStageSlots& outputs) {
    SbExprBuilder b(state);

    auto valueExpr = getTopBottomNValueExprHelper(accStmt);

    if (holds_alternative<Expression*>(valueExpr)) {
        auto rootSlot = outputs.getResultObjIfExists();
        auto* expr = get<Expression*>(valueExpr);
        return b.makeFillEmptyNull(generateExpression(state, expr, rootSlot, outputs));
    } else {
        auto [tag, val] = get<TagAndValue>(valueExpr);
        return b.makeConstant(tag, val);
    }
}

std::pair<SbExpr::Vector, bool> getBlockTopBottomNValueExpr(StageBuilderState& state,
                                                            const AccumulationStatement& accStmt,
                                                            const PlanStageSlots& outputs) {
    SbExprBuilder b(state);
    bool isArray = false;

    auto valueExpr = getTopBottomNValueExprHelper(accStmt);

    if (holds_alternative<Expression*>(valueExpr)) {
        auto rootSlot = outputs.getResultObjIfExists();
        auto* expr = get<Expression*>(valueExpr);
        auto* arrayExpr = dynamic_cast<ExpressionArray*>(expr);

        if (arrayExpr) {
            isArray = true;

            // If the output field from the $top/$bottom AccumulationStatement is an
            // ExpressionArray, then we set 'isArray' to true and return a vector of the
            // element expressions.
            SbExpr::Vector sbExprs;
            for (size_t i = 0; i < arrayExpr->getChildren().size(); ++i) {
                auto* elemExpr = arrayExpr->getChildren()[i].get();
                sbExprs.emplace_back(
                    b.makeFillEmptyNull(generateExpression(state, elemExpr, rootSlot, outputs)));
            }

            return {std::move(sbExprs), isArray};
        }

        auto sbExpr = b.makeFillEmptyNull(generateExpression(state, expr, rootSlot, outputs));
        return {SbExpr::makeSeq(std::move(sbExpr)), isArray};
    } else {
        auto [tag, val] = get<TagAndValue>(valueExpr);
        return {SbExpr::makeSeq(b.makeConstant(tag, val)), isArray};
    }
}

SbExpr getTopBottomNSortByExpr(StageBuilderState& state,
                               const AccumulationStatement& accStmt,
                               const PlanStageSlots& outputs,
                               SbExpr sortSpecExpr) {
    constexpr bool allowCallGenCheapSortKey = true;

    SbExprBuilder b(state);

    auto sortPattern = getSortPattern(accStmt);
    tassert(8774900, "Expected sort pattern for $top/$bottom accumulator", sortPattern.has_value());

    auto plan = makeSortKeysPlan(*sortPattern, allowCallGenCheapSortKey);
    auto sortKeys = buildSortKeys(state, plan, *sortPattern, outputs, std::move(sortSpecExpr));

    if (plan.type == BuildSortKeysPlan::kTraverseFields) {
        auto fullKeyExpr = [&] {
            if (sortPattern->size() == 1) {
                // When the sort pattern has only one part, we return the sole part's key expr.
                return std::move(sortKeys.keyExprs[0]);
            } else if (sortPattern->size() > 1) {
                // When the sort pattern has more than one part, we return an array containing
                // each part's key expr (in order).
                return b.makeFunction("newArray", std::move(sortKeys.keyExprs));
            } else {
                MONGO_UNREACHABLE;
            }
        }();

        if (sortKeys.parallelArraysCheckExpr) {
            // If 'parallelArraysCheckExpr' is not null, inject it into 'fullKeyExpr'.
            auto parallelArraysError =
                b.makeFail(ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            fullKeyExpr = b.makeIf(std::move(sortKeys.parallelArraysCheckExpr),
                                   std::move(fullKeyExpr),
                                   std::move(parallelArraysError));
        }

        return fullKeyExpr;
    } else if (plan.type == BuildSortKeysPlan::kCallGenCheapSortKey) {
        // generateCheapSortKey() returns a SortKeyComponentVector, but we need an array of
        // keys (or the sole part's key in cases where the sort pattern has only one part),
        // so we generate a call to sortKeyComponentVectorToArray() to perform the conversion.
        return b.makeFunction("sortKeyComponentVectorToArray", std::move(sortKeys.fullKeyExpr));
    } else {
        MONGO_UNREACHABLE;
    }
}

std::pair<SbExpr::Vector, bool> getBlockTopBottomNSortByExpr(StageBuilderState& state,
                                                             const AccumulationStatement& accStmt,
                                                             const PlanStageSlots& outputs,
                                                             SbExpr sortSpecExpr) {
    constexpr bool allowCallGenCheapSortKey = true;
    bool useMK = false;

    SbExprBuilder b(state);

    auto sortPattern = getSortPattern(accStmt);
    tassert(8448719, "Expected sort pattern for $top/$bottom accumulator", sortPattern.has_value());

    auto plan = makeSortKeysPlan(*sortPattern, allowCallGenCheapSortKey);
    auto sortKeys = buildSortKeys(state, plan, *sortPattern, outputs, std::move(sortSpecExpr));

    if (plan.type == BuildSortKeysPlan::kTraverseFields) {
        auto keyExprs = [&] {
            if (sortPattern->size() == 1) {
                // When the sort pattern has only one part, we return the sole part's key expr.
                return SbExpr::makeSeq(std::move(sortKeys.keyExprs[0]));
            } else if (sortPattern->size() > 1) {
                // When the sort pattern has more than one part, we return an array containing
                // each part's key expr (in order).
                useMK = true;
                return std::move(sortKeys.keyExprs);
            } else {
                return SbExpr::makeSeq(b.makeFunction("newArray"));
            }
        }();

        if (sortKeys.parallelArraysCheckExpr) {
            // If 'parallelArraysCheckExpr' is not null, inject it into 'fullKeyExpr'.
            auto parallelArraysError =
                b.makeFail(ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            tassert(8448720, "Expected vector to be non-empty", !keyExprs.empty());

            keyExprs[0] = b.makeIf(std::move(sortKeys.parallelArraysCheckExpr),
                                   std::move(keyExprs[0]),
                                   std::move(parallelArraysError));
        }

        return {std::move(keyExprs), useMK};
    } else if (plan.type == BuildSortKeysPlan::kCallGenCheapSortKey) {
        // generateCheapSortKey() returns a SortKeyComponentVector, but we need an array of
        // keys (or the sole part's key in cases where the sort pattern has only one part),
        // so we generate a call to sortKeyComponentVectorToArray() to perform the conversion.
        auto fullKeyExpr =
            b.makeFunction("sortKeyComponentVectorToArray", std::move(sortKeys.fullKeyExpr));

        return {SbExpr::makeSeq(std::move(fullKeyExpr)), useMK};
    } else {
        MONGO_UNREACHABLE;
    }
}

boost::optional<AddBlockExprs> generateAllAccumBlockInputExprsForOneAcc(
    StageBuilderState& state, const AccumulationStatement& accStmt, const PlanStageSlots& outputs) {
    auto accOp = AccumOp{accStmt};

    auto rootSlot = outputs.getResultObjIfExists();

    AccumInputsPtr inputs;

    // For $topN and $bottomN, we need to pass multiple SbExprs to buildAddExprs()
    // (an "input" expression and a "sortBy" expression).
    if (isTopBottomN(accStmt)) {
        auto specSlot = SbSlot{state.getSortSpecSlot(&accStmt)};

        inputs = std::make_unique<AddBlockTopBottomNInputs>(
            getBlockTopBottomNValueExpr(state, accStmt, outputs),
            getBlockTopBottomNSortByExpr(state, accStmt, outputs, SbExpr{specSlot}),
            SbExpr{specSlot});
    } else {
        // For all other accumulators, we call generateExpression() on 'argument' to create an
        // SbExpr and then we pass this SbExpr as the kInput arg to buildAddExprs().
        inputs = std::make_unique<AddSingleInput>(
            generateExpression(state, accStmt.expr.argument.get(), rootSlot, outputs));
    }

    return accOp.buildAddBlockExprs(state, std::move(inputs), outputs);
}

boost::optional<std::vector<AddBlockExprs>> generateAllAccumBlockInputExprs(
    StageBuilderState& state, const GroupNode& groupNode, const PlanStageSlots& outputs) {
    boost::optional<std::vector<AddBlockExprs>> blockAccumExprsVec;
    blockAccumExprsVec.emplace();

    for (const auto& accStmt : groupNode.accumulators) {
        // One 'accStmt' can be decomposed into multiple SBE accumulators, as is the case for the
        // $avg accumulator, which is implemented as a pair of sum and count accumulators that a
        // finalizing expression will later use to compute the resulting average.
        boost::optional<AddBlockExprs> blockAccumExprs =
            generateAllAccumBlockInputExprsForOneAcc(state, accStmt, outputs);

        if (!blockAccumExprs) {
            return boost::none;
        }

        blockAccumExprsVec->emplace_back(std::move(*blockAccumExprs));
    }

    return blockAccumExprsVec;
}

/**
 * Generate a vector of (inputSlot, mergingExpression) pairs. The slot (whose id is allocated by
 * this function) will be used to store spilled partial aggregate values that have been recovered
 * from disk and deserialized. The merging expression is an agg function which combines these
 * partial aggregates.
 *
 * Usually the returned vector will be of length one, but in the case of $avg, the MQL accumulation
 * statement is implemented by calculating two separate aggregates in the SBE plan, which are
 * finalized by custom glue code in the VM to produce the ultimate value.
 */
SbExprSlotVector generateMergingExpressionsForOneAcc(StageBuilderState& state,
                                                     const AccumulationStatement& accStmt,
                                                     int numInputSlots) {
    auto slotIdGenerator = state.slotIdGenerator;
    auto frameIdGenerator = state.frameIdGenerator;

    tassert(7039555, "'numInputSlots' must be positive", numInputSlots > 0);
    tassert(7039556, "expected non-null 'slotIdGenerator' pointer", slotIdGenerator);
    tassert(7039557, "expected non-null 'frameIdGenerator' pointer", frameIdGenerator);

    auto accOp = AccumOp{accStmt};

    SbSlotVector spillSlots;
    for (int i = 0; i < numInputSlots; ++i) {
        spillSlots.emplace_back(SbSlot{slotIdGenerator->generate()});
    }

    SbExpr::Vector mergingExprs = [&]() {
        AccumInputsPtr combineInputs;

        if (isTopBottomN(accStmt)) {
            auto sortSpec = SbExpr{SbSlot{state.getSortSpecSlot(&accStmt)}};
            combineInputs = std::make_unique<CombineAggsTopBottomNInputs>(std::move(sortSpec));
        }

        return accOp.buildCombineAggs(state, std::move(combineInputs), spillSlots);
    }();

    // Zip the slot vector and expression vector into a vector of pairs.
    tassert(7039550,
            "expected same number of slots and input exprs",
            spillSlots.size() == mergingExprs.size());
    SbExprSlotVector result;
    result.reserve(spillSlots.size());
    for (size_t i = 0; i < spillSlots.size(); ++i) {
        result.emplace_back(std::pair(std::move(mergingExprs[i]), spillSlots[i]));
    }
    return result;
}

/**
 * Generates all merging expressions needed for a 'groupNode' input.
 */
std::vector<SbExprSlotVector> generateAllMergingExprs(StageBuilderState& state,
                                                      const GroupNode& groupNode) {
    // Since partial accumulator state may be spilled to disk and then merged, we must construct not
    // only the basic agg expressions for each accumulator, but also agg expressions that are used
    // to combine partial aggregates that have been spilled to disk.
    std::vector<SbExprSlotVector> mergingExprs;

    for (const auto& accStmt : groupNode.accumulators) {  // 'accStmt' is per query acc operation
        AccumOp accOp = AccumOp{accStmt};
        size_t numAggs = accOp.getNumAggs();
        mergingExprs.emplace_back(generateMergingExpressionsForOneAcc(state, accStmt, numAggs));
    }

    return mergingExprs;
}

/**
 * Translates 'accStmt' into one or more 'SbBlockAggExpr's if-and-only-if the accumulator supports
 * block inputs. Returns boost:none otherwise.
 */
boost::optional<SbBlockAggExprVector> tryToGenerateOneBlockAccumulator(
    StageBuilderState& state,
    const AccumulationStatement& accStmt,
    AccumInputsPtr accInputExprs,
    boost::optional<SbSlot> initRootSlot,
    SbSlot bitmapInternalSlot) {
    SbExprBuilder b(state);
    auto accOp = AccumOp{accStmt};
    boost::optional<SbBlockAggExprVector> sbBlockAggExprs;
    sbBlockAggExprs.emplace();

    // Generate the agg expressions and blockAgg expressions.
    std::vector<BlockAggAndRowAgg> blockAggsAndRowAggs;

    boost::optional<std::vector<BlockAggAndRowAgg>> aggs =
        accOp.buildAddBlockAggs(state, std::move(accInputExprs), bitmapInternalSlot);

    // If we weren't able to generate block aggs, then we return boost::none to indicate failure.
    if (!aggs) {
        return boost::none;
    }

    blockAggsAndRowAggs = std::move(*aggs);

    // Generate the init expressions.
    SbExpr::Vector inits = [&]() {
        PlanStageSlots slots;
        if (initRootSlot) {
            slots.setResultObj(*initRootSlot);
        }

        AccumInputsPtr initInputs;

        if (isAccumulatorN(accStmt)) {
            auto expr =
                generateExpression(state, accStmt.expr.initializer.get(), initRootSlot, slots);

            initInputs =
                std::make_unique<InitAccumNInputs>(std::move(expr), b.makeBoolConstant(true));
        }

        return accOp.buildInitialize(state, std::move(initInputs));
    }();

    tassert(7567301,
            "The accumulation and initialization expression should have the same length",
            inits.size() == blockAggsAndRowAggs.size());

    // For each 'init' / 'blockAgg' / 'agg' expression tuple, wrap the expressions in
    // an SbBlockAggExpr and append the sbBlockAggExpr to 'sbBlockAggExprs'.
    for (size_t i = 0; i < blockAggsAndRowAggs.size(); i++) {
        SbExpr& init = inits[i];
        SbExpr& blockAgg = blockAggsAndRowAggs[i].blockAgg;
        SbExpr& rowAgg = blockAggsAndRowAggs[i].rowAgg;

        sbBlockAggExprs->emplace_back(
            SbBlockAggExpr{std::move(init), std::move(blockAgg), std::move(rowAgg)}, boost::none);
    }

    return sbBlockAggExprs;
}

/**
 * Helper to temporarily set a value in the current scope & reset it to its previous value on scope
 * exit.
 */
auto makeValueGuard(auto* dst, auto val) {
    return ScopeGuard{[dst, old = std::exchange(*dst, std::move(val))]() mutable {
        *dst = std::move(old);
    }};
}

SbHashAggAccumulatorVector generateScalarAccumulators(StageBuilderState& state,
                                                      const GroupNode& groupNode,
                                                      boost::optional<SbSlot> initRootSlot,
                                                      PlanStageSlots& outputs) {
    SbHashAggAccumulatorVector accumulatorList;
    auto rootSlot = outputs.getResultObjIfExists();

    bool enableSinglePurposeAccumulators =
        state.expCtx->getIfrContext().getSavedFlagValue(feature_flags::gFeatureFlagSbeAccumulators);

    for (const auto& accStmt : groupNode.accumulators) {
        AccumOp accOp(accStmt);

        if (enableSinglePurposeAccumulators && accOp.canBuildSinglePurposeAccumulator()) {
            SbSlot outSlot(state.slotId());
            SbSlot spillSlot(state.slotId());
            auto inputExpression =
                generateExpression(state, accStmt.expr.argument.get(), rootSlot, outputs);
            accumulatorList.emplace_back(
                (state.needsMerge && groupNode.willBeMerged)
                    ? accOp.buildSinglePurposeAccumulatorForMerge(state,
                                                                  std::move(inputExpression),
                                                                  accStmt.fieldName,
                                                                  std::move(outSlot),
                                                                  std::move(spillSlot))
                    : accOp.buildSinglePurposeAccumulator(state,
                                                          std::move(inputExpression),
                                                          accStmt.fieldName,
                                                          std::move(outSlot),
                                                          std::move(spillSlot)));

            continue;
        }

        auto inputs = [&]() -> std::pair<AccumInputsPtr, AccumInputsPtr> {
            if (isTopBottomN(accStmt)) {
                auto specSlot = SbSlot{state.getSortSpecSlot(&accStmt)};

                return {std::make_unique<AddTopBottomNInputs>(
                            getTopBottomNValueExpr(state, accStmt, outputs),
                            getTopBottomNSortByExpr(state, accStmt, outputs, SbExpr{specSlot}),
                            SbExpr{specSlot}),
                        std::make_unique<FinalizeTopBottomNInputs>(SbExpr{specSlot})};
            } else {
                return {std::make_unique<AddSingleInput>(generateExpression(
                            state, accStmt.expr.argument.get(), rootSlot, outputs)),
                        nullptr};
            }
        }();

        SbExpr::Vector aggs =
            accOp.buildAddAggs(state, accOp.buildAddExprs(state, std::move(inputs.first)));

        auto mergeExpressions =
            generateMergingExpressionsForOneAcc(state, accStmt, accOp.getNumAggs());

        tassert(8186814,
                "Expected aggregate to have same number of merge operators and accumulators",
                mergeExpressions.size() == aggs.size());

        // Generate the init expressions.
        SbExpr::Vector inits = [&]() {
            PlanStageSlots slots;
            if (initRootSlot) {
                slots.setResultObj(*initRootSlot);
            }

            AccumInputsPtr initInputs;

            if (isAccumulatorN(accStmt)) {
                auto expr =
                    generateExpression(state, accStmt.expr.initializer.get(), initRootSlot, slots);

                initInputs = std::make_unique<InitAccumNInputs>(
                    std::move(expr), SbExprBuilder{state}.makeBoolConstant(true));
            }

            return accOp.buildInitialize(state, std::move(initInputs));
        }();

        tassert(8186810,
                "Expected aggregate to have same number of initializers and accumulators",
                inits.size() == aggs.size());

        // The 'AccumOp::buildFinalize()' method uses the stage builder state to decide if it should
        // generate expressions for partial aggregates that the shards pass back to the router for
        // merging or for the final aggregated result. In some cases, however, a $group operation
        // that would require merging gets routed to just one shard, which should produce the final
        // output. In that case, we temporarily modify the stage builder state so that we get the
        // desired finalizer.
        const auto needsMergeGuard =
            makeValueGuard(&state.needsMerge, groupNode.willBeMerged && state.needsMerge);

        SbSlotVector aggSlots;
        for (size_t i = 0; i < aggs.size(); ++i) {
            aggSlots.emplace_back(state.slotId());
        }
        auto finalize = accOp.buildFinalize(state, std::move(inputs.second), aggSlots);
        if (!finalize) {
            // A null result (empty 'SbExpr') from 'buildFinalize()' means that the output of this
            // 'AccumulationStatement' should be the unmodified value from the output slot.
            finalize = SbExpr{aggSlots[0]};
        }

        for (size_t i = 0; i < aggs.size(); i++) {
            auto& [merge, spillSlot] = mergeExpressions[i];
            accumulatorList.emplace_back(SbHashAggAccumulator{
                .fieldName = accStmt.fieldName,
                .outSlot = aggSlots[i],
                .spillSlot = std::move(spillSlot),
                // We only need one finalizer per 'AccumulatorStatement' entry, so when we generate
                // more than one operator, we attach the one finalizer to the first one and null
                // expressions to the rest.
                .resultExpr = std::exchange(finalize, SbExpr{}),
                .implementation = SbHashAggCompiledAccumulator{.init = std::move(inits[i]),
                                                               .agg = std::move(aggs[i]),
                                                               .merge = std::move(merge)}});
        }
    }

    return accumulatorList;
}

/**
 * This function returns a vector of 'SbBlockAggExpr's that correspond to block-aware accumulators
 * for the 'groupNode' input. Each of the resulting 'SbBlockAggExpr' objects has an expression for
 * initializing the accumulator and two expressions for accumulating values: one scalar and one
 * block-based.
 *
 * Returns a value only if _all_ accumulators support block inputs; returns boost:none otherwise.
 */
boost::optional<std::vector<SbBlockAggExprVector>> tryToGenerateBlockAccumulators(
    StageBuilderState& state,
    const GroupNode& groupNode,
    std::vector<AccumInputsPtr> accInputExprsVec,
    boost::optional<SbSlot> initRootSlot,
    SbSlot bitmapInternalSlot) {
    // Loop over 'groupNode.accumulators' and populate 'SbBlockAggExprs'.
    boost::optional<std::vector<SbBlockAggExprVector>> sbBlockAggExprs;
    sbBlockAggExprs.emplace();

    size_t accIdx = 0;
    for (const auto& accStmt : groupNode.accumulators) {
        boost::optional<SbBlockAggExprVector> vec = tryToGenerateOneBlockAccumulator(
            state, accStmt, std::move(accInputExprsVec[accIdx]), initRootSlot, bitmapInternalSlot);

        // Return failure if this accumulator does not support block inputs.
        if (!vec.has_value()) {
            return boost::none;
        }
        sbBlockAggExprs->emplace_back(std::move(*vec));
        ++accIdx;
    }

    return sbBlockAggExprs;
}

SbExpr generateIdExpression(StageBuilderState& state,
                            SbSlotVector groupBySlots,
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

    return idFinalExpr;
}

/**
 * Builds the root project stage that produces one output group for each getNext and binds the group
 * key (the '_id' field in the resulting group document) and final accumulator outputs to their
 * respective slots.
 *
 * This overload of 'buildGroupFinalStage()' gets accumulator finalizers from the caller-provided
 * 'SbHashAggAccumulator' objects in the 'accumulatorList' vector.
 *
 * Returns a tuple containing the updated SBE stage tree, a list of output field names and a list of
 * output field slots (corresponding to the accumulators in 'groupNode'), and a new empty
 * PlanStageSlots object.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots> buildGroupFinalStage(
    StageBuilderState& state,
    SbStage groupStage,
    SbExpr idFinalExpr,
    PlanStageSlots outputs,  // input/output (moved in, move-returned in tuple)
    SbSlotVector& individualSlots,
    SbHashAggAccumulatorVector accumulatorList,
    const GroupNode& groupNode) {
    SbBuilder b(state, groupNode.nodeId());

    // Prepare to project 'idFinalExpr' to a slot.
    SbExprOptSlotVector projects;
    projects.emplace_back(std::move(idFinalExpr), boost::none);

    // Generate all the finalize expressions and prepare to project all these expressions
    // to slots.
    std::vector<std::string> fieldNames{"_id"};
    for (auto& accumulator : accumulatorList) {
        if (!accumulator.resultExpr.isNull()) {
            fieldNames.push_back(std::move(accumulator.fieldName));
            projects.emplace_back(std::move(accumulator.resultExpr), boost::none);
        }
    }

    // Project all the aforementioned expressions to slots.
    auto [retStage, finalSlots] = b.makeProject(
        buildVariableTypes(outputs, individualSlots), std::move(groupStage), std::move(projects));

    individualSlots.insert(individualSlots.end(), finalSlots.begin(), finalSlots.end());

    return {std::move(retStage), std::move(fieldNames), std::move(finalSlots), std::move(outputs)};
}

/**
 * Builds the root projection, as described in the above 'buildGroupFinalStage` overload.
 *
 * This overload of 'buildGroupFinalStage()' generates accumulator finalizers from the
 * 'AccumulatorStatement' objects in the 'groupNode' input.
 *
 * Returns a tuple containing the updated SBE stage tree, a list of output field names and a list of
 * output field slots (corresponding to the accumulators in 'groupNode'), and a new empty
 * PlanStageSlots object.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots> buildGroupFinalStage(
    StageBuilderState& state,
    SbStage groupStage,
    SbExpr idFinalExpr,
    PlanStageSlots outputs,  // input/output (moved in, move-returned in tuple)
    SbSlotVector& individualSlots,
    SbSlotVector groupOutSlots,
    const GroupNode& groupNode) {
    // This group may be fully pushed down to execute on a shard; if so it will not be
    // merged on the router, and should emit the final agg results (not partial values).
    // Temporarily override `needsMerge` with the value for this particular group.
    const auto needsMergeGuard =
        makeValueGuard(&state.needsMerge, groupNode.willBeMerged && state.needsMerge);
    SbBuilder b(state, groupNode.nodeId());

    const auto& accStmts = groupNode.accumulators;  // explicit accs in the original query

    std::vector<SbSlotVector> aggSlotsVec;
    auto groupOutSlotsIt = groupOutSlots.begin();

    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        auto accOp = AccumOp{accStmts[idxAcc]};
        size_t numAggs = accOp.getNumAggs();

        aggSlotsVec.emplace_back(SbSlotVector(groupOutSlotsIt, groupOutSlotsIt + numAggs));
        groupOutSlotsIt += numAggs;
    }

    // Prepare to project 'idFinalExpr' to a slot.
    SbExprOptSlotVector projects;
    projects.emplace_back(std::move(idFinalExpr), boost::none);

    // Generate all the finalize expressions and prepare to project all these expressions
    // to slots.
    std::vector<std::string> fieldNames{"_id"};

    // Because one 'accStmt' can operate on multiple 'aggSlot' slots, we need separate indexes to
    // track our positions in the 'accStmts' and 'aggSlotsVec' lists.
    size_t idxAccFirstSlot = 0;
    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        const AccumulationStatement& accStmt = accStmts[idxAcc];
        auto accOp = AccumOp{accStmt};

        // Gathers field names for the output object from accumulator statements.
        fieldNames.push_back(accStmts[idxAcc].fieldName);

        AccumInputsPtr finalizeInputs;

        if (isTopBottomN(accStmt)) {
            auto sortSpec = SbExpr{SbSlot{state.getSortSpecSlot(&accStmt)}};
            finalizeInputs = std::make_unique<FinalizeTopBottomNInputs>(std::move(sortSpec));
        }

        if (auto finalExpr =
                accOp.buildFinalize(state, std::move(finalizeInputs), aggSlotsVec[idxAcc]);
            finalExpr) {
            projects.emplace_back(std::move(finalExpr), boost::none);
        } else {
            // Some accumulators do not use a finalizer, because they return the value in the
            // accumulator as-is.
            projects.emplace_back(groupOutSlots[idxAccFirstSlot], boost::none);
        }

        idxAccFirstSlot += aggSlotsVec[idxAcc].size();
    }

    // Project all the aforementioned expressions to slots.
    auto [retStage, finalSlots] = b.makeProject(
        buildVariableTypes(outputs, individualSlots), std::move(groupStage), std::move(projects));

    individualSlots.insert(individualSlots.end(), finalSlots.begin(), finalSlots.end());

    return {std::move(retStage), std::move(fieldNames), std::move(finalSlots), std::move(outputs)};
}

/**
 * This function builds a BlockHashAggStage for 'groupNode'.
 *
 * Returns a tuple containing the updated SBE plan tree, the list of slots corresponding to the
 * group by inputs, and the list of accumulator output slots corresponding to the accumulators from
 * 'groupNode'.
 */
MONGO_COMPILER_NOINLINE
std::tuple<SbStage, SbSlotVector, SbSlotVector> buildGroupAggregationBlock(
    StageBuilderState& state,
    const PlanStageSlots& childOutputs,
    SbSlotVector individualSlots,
    SbStage stage,
    SbExpr::Vector groupByKeyExprs,
    std::vector<SbBlockAggExprVector> sbBlockAggExprs,
    std::vector<SbExprSlotVector> mergingExprs,
    std::vector<SbExpr::Vector> blockAccInputExprs,
    boost::optional<SbSlot> bitmapInternalSlot,
    const std::vector<SbSlotVector>& blockAccDataSlots,
    const GroupNode* groupNode) {
    constexpr auto kBlockSelectivityBitmap = PlanStageSlots::kBlockSelectivityBitmap;
    const PlanNodeId nodeId = groupNode->nodeId();
    SbBuilder b(state, nodeId);

    // Project the group by key expressions and any block accumulator input expressions to slots.
    SbExprOptSlotVector projects;
    for (auto& expr : groupByKeyExprs) {
        projects.emplace_back(std::move(expr), boost::none);
    }
    for (auto& exprsVec : blockAccInputExprs) {
        for (auto& expr : exprsVec) {
            projects.emplace_back(std::move(expr), boost::none);
        }
    }
    auto [outStage, outSlots] = b.makeProject(
        buildVariableTypes(childOutputs, individualSlots), std::move(stage), std::move(projects));
    stage = std::move(outStage);

    SbSlotVector groupBySlots;
    SbSlotVector flattenedBlockAccArgSlots;
    SbSlotVector flattenedBlockAccDataSlots;
    size_t numGroupByExprs = groupByKeyExprs.size();
    groupBySlots.reserve(numGroupByExprs);
    flattenedBlockAccArgSlots.reserve(outSlots.size() - numGroupByExprs);
    for (size_t i = 0; i < numGroupByExprs; ++i) {
        groupBySlots.emplace_back(outSlots[i]);
    }
    for (size_t i = numGroupByExprs; i < outSlots.size(); ++i) {
        flattenedBlockAccArgSlots.emplace_back(outSlots[i]);
    }
    for (const auto& slotsVec : blockAccDataSlots) {
        flattenedBlockAccDataSlots.insert(
            flattenedBlockAccDataSlots.end(), slotsVec.begin(), slotsVec.end());
    }
    individualSlots.insert(individualSlots.end(), groupBySlots.begin(), groupBySlots.end());
    individualSlots.insert(
        individualSlots.end(), flattenedBlockAccArgSlots.begin(), flattenedBlockAccArgSlots.end());

    // Builds a group stage with accumulator expressions and group-by slot(s).
    auto [hashAggStage, groupByOutSlots, aggSlots] = [&] {
        SbBlockAggExprVector flattenedSbBlockAggExprs;
        for (auto& vec : sbBlockAggExprs) {
            std::move(vec.begin(), vec.end(), std::back_inserter(flattenedSbBlockAggExprs));
        }

        SbExprSlotVector flattenedMergingExprs;
        for (auto& vec : mergingExprs) {
            std::move(vec.begin(), vec.end(), std::back_inserter(flattenedMergingExprs));
        }

        tassert(
            8448603, "Expected 'bitmapInternalSlot' to be defined", bitmapInternalSlot.has_value());

        return b.makeBlockHashAgg(buildVariableTypes(childOutputs, individualSlots),
                                  std::move(stage),
                                  groupBySlots,
                                  std::move(flattenedSbBlockAggExprs),
                                  childOutputs.get(kBlockSelectivityBitmap),
                                  flattenedBlockAccArgSlots,
                                  *bitmapInternalSlot,
                                  flattenedBlockAccDataSlots,
                                  std::move(flattenedMergingExprs));
    }();
    stage = std::move(hashAggStage);
    return {std::move(stage), std::move(groupByOutSlots), std::move(aggSlots)};
}  // buildGroupAggregationBlock

/**
 * This function builds a (scalar) HashAggStage for 'groupNode'.
 *
 * Returns a tuple containing the updated SBE plan tree, a list of slots corresponding to the group
 * by inputs, and a list of accumulator output slots corresponding to the accumulators from
 * 'groupNode'.
 */
MONGO_COMPILER_NOINLINE
std::tuple<SbStage, SbSlotVector, SbSlotVector> buildGroupAggregationScalar(
    StageBuilderState& state,
    const PlanStageSlots& childOutputs,
    SbSlotVector individualSlots,
    SbStage stage,
    SbExpr::Vector groupByKeyExprs,
    const SbHashAggAccumulatorVector& accumulatorList,
    const GroupNode* groupNode) {
    const PlanNodeId nodeId = groupNode->nodeId();
    SbBuilder b(state, nodeId);

    // Project the group by key expressions to slots.
    SbExprOptSlotVector projects;
    for (auto& expr : groupByKeyExprs) {
        projects.emplace_back(std::move(expr), boost::none);
    }
    auto [outStage, outSlots] = b.makeProject(
        buildVariableTypes(childOutputs, individualSlots), std::move(stage), std::move(projects));
    stage = std::move(outStage);

    SbSlotVector groupBySlots;
    size_t numGroupByExprs = groupByKeyExprs.size();
    groupBySlots.reserve(numGroupByExprs);
    for (size_t i = 0; i < numGroupByExprs; ++i) {
        groupBySlots.emplace_back(outSlots[i]);
    }
    individualSlots.insert(individualSlots.end(), groupBySlots.begin(), groupBySlots.end());

    // Builds a group stage with accumulator expressions and group-by slot(s).
    auto [hashAggStage, groupByOutSlots, aggSlots] = [&] {
        return b.makeHashAgg(buildVariableTypes(childOutputs, individualSlots),
                             std::move(stage),
                             groupBySlots,
                             accumulatorList,
                             state.getCollatorSlot());
    }();
    stage = std::move(hashAggStage);
    return {std::move(stage), std::move(groupByOutSlots), std::move(aggSlots)};
}  // buildGroupAggregationScalar

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
    SbExpr outputExpr = b.makeFunction(newObjFn, std::move(funcArgs));

    auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(outputExpr));
    stage = std::move(outStage);

    SbSlot slot = outSlots[0];
    slot.setTypeSignature(TypeSignature::kObjectType);

    return {std::move(stage), slot};
}

/**
 * This function generates the "root slot" for initializer expressions when it is needed. It is only
 * used in scalar (i.e. non-block processing) mode.
 *
 * 'individualSlots' is an output parameter. It is an empty vector on input.
 */
std::pair<SbStage, SbExpr::Vector> generateInitRootSlot(SbStage stage,
                                                        StageBuilderState& state,
                                                        const PlanStageSlots& childOutputs,
                                                        SbSlotVector& individualSlots,  // output
                                                        SbExpr::Vector groupByKeyExprs,
                                                        const GroupNode* groupNode,
                                                        boost::optional<SbSlot> slotIdForInitRoot) {
    const PlanNodeId nodeId = groupNode->nodeId();
    SbBuilder b(state, nodeId);
    ExpressionObject* idExprObj =
        dynamic_cast<ExpressionObject*>(groupNode->groupByExpression.get());
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
        for (const auto& e : groupByKeyExprs) {
            exprs.emplace_back(b.makeStrConstant(fieldNames[i]));
            exprs.emplace_back(e.clone());
            ++i;
        }

        groupByKeyExprs.clear();
        groupByKeyExprs.emplace_back(b.makeFunction("newObj"_sd, std::move(exprs)));

        idIsSingleKey = true;
    }

    SbExpr& groupByExpr = groupByKeyExprs[0];

    bool idIsKnownToBeObj = [&] {
        if (idExprObj != nullptr) {
            return true;
        } else if (groupByExpr.isConstantExpr()) {
            auto [tag, _] = groupByExpr.getConstantValue();
            return stage_builder::getTypeSignature(tag).isSubset(TypeSignature::kObjectType);
        }
        return false;
    }();

    // Project 'groupByExpr' to a slot.
    boost::optional<SbSlot> targetSlot = idIsKnownToBeObj ? slotIdForInitRoot : boost::none;
    auto [projectStage, projectOutSlots] =
        b.makeProject(buildVariableTypes(childOutputs),
                      std::move(stage),
                      std::pair(std::move(groupByExpr), targetSlot));
    stage = std::move(projectStage);

    groupByExpr = SbExpr{projectOutSlots[0]};
    individualSlots.emplace_back(projectOutSlots[0]);

    // As per the mql semantics add a project expression 'isObject(_id) ? _id : {}'
    // which will be provided as root to initializer expression.
    if (idIsKnownToBeObj) {
        // If we know '_id' is an object, then we can just use the slot as-is.
        return {std::move(stage), std::move(groupByKeyExprs)};
    } else {
        // If we're not sure whether '_id' is an object, then we need to project the
        // aforementioned expression to a slot and use that.
        auto [emptyObjTag, emptyObjVal] = sbe::value::makeNewObject();
        SbExpr idOrEmptyObjExpr = b.makeIf(b.makeFunction("isObject"_sd, groupByExpr.clone()),
                                           groupByExpr.clone(),
                                           b.makeConstant(emptyObjTag, emptyObjVal));

        auto [outStage, outSlots] =
            b.makeProject(buildVariableTypes(childOutputs, individualSlots),
                          std::move(stage),
                          SbExprOptSlotPair{std::move(idOrEmptyObjExpr), slotIdForInitRoot});
        stage = std::move(outStage);

        outSlots[0].setTypeSignature(TypeSignature::kObjectType);
        individualSlots.emplace_back(outSlots[0]);

        return {std::move(stage), std::move(groupByKeyExprs)};
    }
}  // generateInitRootSlot

/**
 * Checks whether 'groupNode' has any accumulator with a variable initializer, i.e. an initializer
 * that is non-null and non-constant.
 */
bool hasVariableAccInit(const GroupNode* groupNode) {
    for (const AccumulationStatement& accStmt : groupNode->accumulators) {
        if (!ExpressionConstant::isNullOrConstant(accStmt.expr.initializer)) {
            return true;
        }
    }
    return false;
}

/**
 * Checks if all accumulators support block mode.
 */
bool accsSupportBlockMode(const GroupNode* groupNode) {
    // Definitions of the accumulators from the query optimizer.
    const std::vector<AccumulationStatement>& accs = groupNode->accumulators;
    return std::all_of(accs.begin(), accs.end(), [&](auto&& acc) {
        auto accOp = AccumOp{acc};
        return accOp.hasBuildAddBlockExprs() && accOp.hasBuildAddBlockAggs();
    });
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
    uassert(
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

    const std::vector<AccumulationStatement>& accStmts = groupNode->accumulators;

    tassert(5851605,
            "The number of final slots must be as 1 (the final group-by slot) + the number of acc "
            "slots",
            finalSlots.size() == 1 + accStmts.size());

    for (size_t i = 0; i < fieldNames.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, fieldNames[i]), finalSlots[i]);
    }

    auto fieldNameSet = StringDataSet{fieldNames.begin(), fieldNames.end()};
    for (const auto& path : reqs.getFields()) {
        if (!fieldNameSet.count(getTopLevelField(path))) {
            auto nothingSlot = SbSlot{_state.getNothingSlot()};
            outputs.set(std::make_pair(PlanStageSlots::kField, path), nothingSlot);
        }
    }

    bool reqResultObj = reqs.hasResultObj();
    bool reqResultInfo = reqs.hasResultInfo();
    boost::optional<FieldEffects> effects;

    // If there is a ResultInfo req, check if this $group stage can participate with it.
    if (reqResultInfo) {
        const auto& reqTrackedFieldSet = reqs.getResultInfoTrackedFieldSet();
        const auto& reqEffects = reqs.getResultInfoEffects();

        // Get the effects of this $group stage.
        effects = getQsnInfo(root).effects;

        bool canParticipate = false;
        if (effects) {
            // Narrow 'effects' so that it only has effects applicable to fields in
            // 'reqTrackedFieldSet'.
            effects->narrow(reqTrackedFieldSet);

            if (auto composedEffects = composeEffectsForResultInfo(*effects, reqEffects)) {
                // If this group stage can participate with the result info req, then set
                // 'canParticipate' to true.
                canParticipate = true;
            }
        }

        if (!canParticipate) {
            // If this group stage cannot participate with the result info req, then we need to
            // produce a result object instead.
            reqResultObj = true;
            reqResultInfo = false;
        }
    }

    if (reqResultObj) {
        // Create a result object.
        auto [outStage, outSlot] =
            generateGroupResultObject(std::move(stage), _state, groupNode, fieldNames, finalSlots);
        stage = std::move(outStage);

        outputs.setResultObj(outSlot);
    } else if (reqResultInfo) {
        // Set the result base to be an empty object and add this group stage's effects to
        // the result info effects.
        outputs.setResultInfoBaseObj(SbSlot{_state.getEmptyObjSlot()});
        outputs.addEffectsToResultInfo(_state, reqs, *effects);
    }

    return {std::move(stage), std::move(outputs)};
}  // SlotBasedStageBuilder::buildGroup

/**
 * This function is called by buildGroup(), and it plus its helpers buildGroupImplBlock() and
 * buildGroupImplScalar() contain most of the implementation for $group.
 *
 * It takes the GroupNode, the child's SBE stage tree, and the PlanStageSlots generated by the child
 * as input, and it returns a tuple containing the updated SBE stage tree, a list of output field
 * names and a list of output field slots (corresponding to the accumulators from the GroupNode),
 * and a PlanStageSlots object indicating the outputs of this plan subtree's final stage.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots>
SlotBasedStageBuilder::buildGroupImpl(SbStage stage,  // moved in
                                      const PlanStageReqs& reqs,
                                      PlanStageSlots childOutputs,  // moved in
                                      const GroupNode* groupNode) {
    tassert(5851601, "GROUP should have had group-by key expression", groupNode->groupByExpression);
    // The purpose of these block delimiters is to kick 'groupFieldMap' off the stack after use.
    {
        // Collect all the ExpressionFieldPaths referenced by from 'groupNode'.
        StringMap<const ExpressionFieldPath*> groupFieldMap = collectFieldPaths(groupNode);

        // Evaluate all of the ExpressionFieldPaths in 'groupFieldMap', project the results to
        // slots, and put the slots into 'childOutputs' as kPathExpr slots.
        stage = makePathExprsAvailableInSlots(
            _state, *groupNode, std::move(stage), childOutputs, groupFieldMap);
    }

    // For the rest of stage building $group, we use separate helpers buildGroupImplBlock() for
    // block mode and buildGroupImplScalar() for scalar mode. We first try to use block mode, which
    // may fail due to a variety of things it does not support. If it fails we switch to scalar
    // mode, which should succeed.
    bool blockSucceeded = false;
    std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots> result =
        buildGroupImplBlock(std::move(stage), reqs, childOutputs, groupNode, blockSucceeded);
    if (!blockSucceeded) {
        // buildGroupImplBlock() will always move 'stage' back out so we can get it from 'result'.
        result = buildGroupImplScalar(
            std::move(std::get<0>(result)) /* stage */, reqs, childOutputs, groupNode);
    }
    return result;
}  // SlotBasedStageBuilder::buildGroupImpl

/**
 * Helper for SlotBasedStageBuilder::buildGroupImpl() that handles block processing (BP) mode. It
 * may not be possible to use BP because BP mode is not enabled or some required accumulators don't
 * yet support it or the algorithm is unable to vectorize some aspect of the stage.
 *
 * 'blockSucceeded' is an output parameter that tells whether stage building for BP mode succeeded.
 * If this is false, only the first element of the returned std::tuple will be populated to move
 * 'stage' back to the caller.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots>
SlotBasedStageBuilder::buildGroupImplBlock(SbStage stage,  // moved in
                                           const PlanStageReqs& reqs,
                                           const PlanStageSlots& childOutputs,
                                           const GroupNode* groupNode,
                                           bool& blockSucceeded) {  // output
    // If we abort building BP anywhere along the way, this tells the caller that BP can't be used.
    blockSucceeded = false;

    // Abort BP attempt immediately if any of these conditions hold:
    // - BP is not enabled
    // - Child does not produce block output
    // - Query uses a collator
    // - Any accumulator does not support block mode
    // - Any accumulator has a variable (i.e. non-null, non-constant) initializer expression
    if ((!feature_flags::gFeatureFlagSbeFull.isEnabled() &&
         !feature_flags::gFeatureFlagSbeBlockHashAgg.isEnabled()) ||
        !childOutputs.hasBlockOutput() || _state.getCollatorSlot() ||
        !accsSupportBlockMode(groupNode) || hasVariableAccInit(groupNode)) {
        // Aborting. Move 'stage' back out to caller.
        return {std::move(stage), std::vector<std::string>{}, SbSlotVector{}, PlanStageSlots{}};
    }

    // Generate non-block expressions for the group by keys.
    const boost::intrusive_ptr<Expression>& idExpr = groupNode->groupByExpression;
    SbExpr::Vector groupByKeyExprs = generateGroupByKeyExprs(_state, idExpr.get(), childOutputs);

    // Try to vectorize 'groupByKeyExprs'. This overwrites the original non-vectorized entries in
    // 'groupByKeyExprs' with vectorized ones, but some or all may fail.
    for (SbExpr& sbExpr : groupByKeyExprs) {
        sbExpr = buildVectorizedExpr(_state, std::move(sbExpr), childOutputs, false);
    }

    // Abort if any 'groupByKeyExprs' could not be vectorized.
    if (std::any_of(groupByKeyExprs.begin(), groupByKeyExprs.end(), [](const SbExpr& expr) {
            return expr.isNull();
        })) {
        // Aborting. Move 'stage' back out to caller.
        return {std::move(stage), std::vector<std::string>{}, SbSlotVector{}, PlanStageSlots{}};
    }

    // Try to generate block input expressions for all of the accumulators.
    boost::optional<std::vector<AddBlockExprs>> accumBlockInputExprsVec =
        generateAllAccumBlockInputExprs(_state, *groupNode, childOutputs);
    // Abort if generating block input expressions failed.
    if (!accumBlockInputExprsVec) {
        // Aborting. Move 'stage' back out to caller.
        return {std::move(stage), std::vector<std::string>{}, SbSlotVector{}, PlanStageSlots{}};
    }

    // Unpack 'accumBlockInputExprsVec' and populate 'accInputExprsVec', 'blockAccInputExprs', and
    // 'blockAccDataSlots' from its contents.
    boost::optional<std::vector<AccumInputsPtr>> accInputExprsVec;
    std::vector<SbExpr::Vector> blockAccInputExprs;
    std::vector<SbSlotVector> blockAccDataSlots;
    accInputExprsVec.emplace();
    for (auto& accumBlockExprs : *accumBlockInputExprsVec) {
        accInputExprsVec->emplace_back(std::move(accumBlockExprs.inputs));
        blockAccInputExprs.emplace_back(std::move(accumBlockExprs.exprs));
        blockAccDataSlots.emplace_back(std::move(accumBlockExprs.slots));
    }

    // Create a slot ID for the bitmap.
    SbSlot bitmapInternalSlot(_state.slotId());

    // Try to generate block sbBlockAggExprs for all the accumulators.
    boost::optional<std::vector<SbBlockAggExprVector>> sbBlockAggExprs;
    sbBlockAggExprs = tryToGenerateBlockAccumulators(_state,
                                                     *groupNode,
                                                     std::move(*accInputExprsVec),
                                                     boost::none /* initRootSlot */,
                                                     bitmapInternalSlot);
    // Abort if generating block sbBlockAggExprs failed.
    if (!sbBlockAggExprs) {
        // Aborting. Move 'stage' back out to caller.
        return {std::move(stage), std::vector<std::string>{}, SbSlotVector{}, PlanStageSlots{}};
    }
    // Assert that the 'blockAgg' field is non-null for all SbBlockAggExprs in 'SbBlockAggExprs'.
    const bool hasNullBlockAggs =
        std::any_of(sbBlockAggExprs->begin(), sbBlockAggExprs->end(), [](auto&& v) {
            return std::any_of(
                v.begin(), v.end(), [](auto&& e) { return e.first.blockAgg.isNull(); });
        });
    tassert(8751305, "Expected all blockAgg fields to be defined", !hasNullBlockAggs);

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // BELOW THIS POINT THERE ARE NO MORE ABORTS -- BLOCK PROCESSING IS SUPPORTED FOR THIS $group.
    ////////////////////////////////////////////////////////////////////////////////////////////////
    blockSucceeded = true;

    auto idExprObj = dynamic_cast<ExpressionObject*>(groupNode->groupByExpression.get());
    bool idIsSingleKey = idExprObj == nullptr;

    // Generate merging expressions for all the accumulators. These always use scalar mode.
    std::vector<SbExprSlotVector> mergingExprs = generateAllMergingExprs(_state, *groupNode);

    // Expression for the group ID if the key is a single constant and no accs had variable inits.
    SbExpr idConstantValue;

    // The 'individualSlots' vector is used to keep track of all the slots that are currently
    // "active" that are not present in 'childOutputs'. This vector is used together with
    // 'childOutputs' when we need to do constant-folding / type analysis and vectorization.
    SbSlotVector individualSlots;

    // Build the BlockHashAggStage.
    auto [outStage, groupByOutSlots, aggOutSlots] =
        buildGroupAggregationBlock(_state,
                                   childOutputs,
                                   std::move(individualSlots),
                                   std::move(stage),
                                   std::move(groupByKeyExprs),
                                   std::move(*sbBlockAggExprs),
                                   std::move(mergingExprs),
                                   std::move(blockAccInputExprs),
                                   bitmapInternalSlot,
                                   blockAccDataSlots,
                                   groupNode);
    stage = std::move(outStage);
    tassert(8448606,
            "Expected at least one group by slot or agg out slot",
            !groupByOutSlots.empty() || !aggOutSlots.empty());

    // After the HashAgg/BlockHashAgg stage, the only slots that are "active" are the group-by slots
    // ('groupByOutSlots') and the output slots for the accumulators from groupNode ('aggOutSlots').
    individualSlots = groupByOutSlots;
    individualSlots.insert(individualSlots.end(), aggOutSlots.begin(), aggOutSlots.end());

    // Outputs produced by the final $group stage that is added below by buildGroupFinalStage().
    PlanStageSlots outputs;
    // This stage re-maps the selectivity bitset slot.
    outputs.set(PlanStageSlots::kBlockSelectivityBitmap,
                childOutputs.get(PlanStageSlots::kBlockSelectivityBitmap));

    // The finalize step does not yet support block mode, so for now we unconditionally end the
    // block processing pipeline here.
    auto hashAggOutSlots = groupByOutSlots;
    hashAggOutSlots.insert(hashAggOutSlots.end(), aggOutSlots.begin(), aggOutSlots.end());

    auto [outStage2, blockToRowOutSlots] =
        buildBlockToRow(std::move(stage), _state, outputs, std::move(hashAggOutSlots));
    stage = std::move(outStage2);

    for (size_t i = 0; i < groupByOutSlots.size(); ++i) {
        groupByOutSlots[i] = blockToRowOutSlots[i];
    }
    for (size_t i = 0; i < aggOutSlots.size(); ++i) {
        size_t blockToRowOutSlotsIdx = groupByOutSlots.size() + i;
        aggOutSlots[i] = blockToRowOutSlots[blockToRowOutSlotsIdx];
    }

    // buildBlockToRow() just made a bunch of changes to 'groupByOutSlots' and 'aggOutSlots',
    // so we need to re-generate 'individualSlots'.
    individualSlots = groupByOutSlots;
    individualSlots.insert(individualSlots.end(), aggOutSlots.begin(), aggOutSlots.end());

    // Builds the final stage(s) over the collected accumulators. This always uses scalar mode.
    return buildGroupFinalStage(_state,
                                std::move(stage),
                                generateIdExpression(_state,
                                                     std::move(groupByOutSlots),
                                                     *groupNode,
                                                     idIsSingleKey,
                                                     std::move(idConstantValue)),
                                std::move(outputs),
                                individualSlots,
                                std::move(aggOutSlots),
                                *groupNode);
}  // SlotBasedStageBuilder::buildGroupImplBlock

/**
 * Helper for SlotBasedStageBuilder::buildGroupImpl() that handles scalar (aka non-block processing)
 * mode. This is called if SlotBasedStageBuilder::buildGroupImplBlock() reported that BP mode could
 * not be used, and it should always succeed.
 */
std::tuple<SbStage, std::vector<std::string>, SbSlotVector, PlanStageSlots>
SlotBasedStageBuilder::buildGroupImplScalar(SbStage stage,  // moved in
                                            const PlanStageReqs& reqs,
                                            PlanStageSlots& childOutputs,
                                            const GroupNode* groupNode) {
    // If the child produces block data, insert a BlockToRowStage to switch to scalar.
    if (childOutputs.hasBlockOutput()) {
        stage = buildBlockToRow(std::move(stage), _state, childOutputs);
    }

    // Generate non-block expressions for the group by keys.
    const boost::intrusive_ptr<Expression>& idExpr = groupNode->groupByExpression;
    SbExpr::Vector groupByKeyExprs = generateGroupByKeyExprs(_state, idExpr.get(), childOutputs);

    // Optimize the group by key expressions so the later isConstantExpr() can recognize more cases
    // where the group by expression is constant.
    auto varTypes = buildVariableTypes(childOutputs);
    for (auto& sbExpr : groupByKeyExprs) {
        sbExpr.optimize(_state, varTypes);
    }

    // If any acc has a variable initializer, we need to define a SlotId for the initializers.
    bool variableAccInit = hasVariableAccInit(groupNode);
    boost::optional<mongo::stage_builder::SbSlot> slotIdForInitRoot =
        variableAccInit ? boost::make_optional(SbSlot{_state.slotId()}) : boost::none;

    auto accumulatorList =
        generateScalarAccumulators(_state, *groupNode, slotIdForInitRoot, childOutputs);

    // The 'individualSlots' vector is used to keep track of all the slots that are currently
    // "active" that are not present in 'childOutputs'. This vector is used together with
    // 'childOutputs' when we need to do constant-folding / type analysis and vectorization.
    SbSlotVector individualSlots;

    auto idExprObj = dynamic_cast<ExpressionObject*>(groupNode->groupByExpression.get());
    bool idIsSingleKey = idExprObj == nullptr;
    if (variableAccInit) {
        idIsSingleKey = true;
        auto [outStage, outExprs] = generateInitRootSlot(std::move(stage),
                                                         _state,
                                                         childOutputs,
                                                         individualSlots,  // output
                                                         std::move(groupByKeyExprs),
                                                         groupNode,
                                                         slotIdForInitRoot);
        stage = std::move(outStage);
        groupByKeyExprs = std::move(outExprs);
    }

    // Expression for the group ID if the key is a single constant and no accs had variable inits.
    SbExpr idConstantValue;

    // If the group by key is a single constant and no acc had a variable initializer, set
    // 'idConstantValue' and clear the 'groupByKeyExprs' vector.
    if (idIsSingleKey && !variableAccInit && groupByKeyExprs[0].isConstantExpr()) {
        idConstantValue = std::move(groupByKeyExprs[0]);
        groupByKeyExprs.clear();
    }

    // Build the HashAggStage.
    auto [outStage, groupByOutSlots, aggOutSlots] =
        buildGroupAggregationScalar(_state,
                                    childOutputs,
                                    std::move(individualSlots),
                                    std::move(stage),
                                    std::move(groupByKeyExprs),
                                    accumulatorList,
                                    groupNode);
    stage = std::move(outStage);

    // After the HashAgg/BlockHashAgg stage, the only slots that are "active" are the group-by slots
    // ('groupByOutSlots') and the output slots for the accumulators from groupNode ('aggOutSlots').
    individualSlots = groupByOutSlots;
    individualSlots.insert(individualSlots.end(), aggOutSlots.begin(), aggOutSlots.end());

    // Outputs produced by the final $group stage that is added below by buildGroupFinalStage().
    PlanStageSlots outputs;

    // Builds the final stage(s) over the collected accumulators. This always uses scalar mode.
    return buildGroupFinalStage(_state,
                                std::move(stage),
                                generateIdExpression(_state,
                                                     std::move(groupByOutSlots),
                                                     *groupNode,
                                                     idIsSingleKey,
                                                     std::move(idConstantValue)),
                                std::move(outputs),
                                individualSlots,
                                std::move(accumulatorList),
                                *groupNode);
}  // SlotBasedStageBuilder::buildGroupImplScalar
}  // namespace mongo::stage_builder
