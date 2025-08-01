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

#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/stage_builder/sbe/abt_defs.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_filter.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/stage_builder/sbe/vectorizer.h"
#include "mongo/db/timeseries/timeseries_constants.h"

#include <iostream>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {
namespace {
struct CellPathReqsRet {
    std::vector<sbe::value::CellBlock::PathRequest> topLevelPaths;
    std::vector<sbe::value::CellBlock::PathRequest> traversePaths;
};

// The set of fields specified in a bucket spec contains the fields that should be available after
// bucket-level processing _and_ unpacking of this bucket is done. This means that it includes the
// fields that are computed from the 'metaField' before unpacking but these fields don't correspond
// to any cell paths, even if they have the same names, and we must exclude them from the cell path
// requirements. Note, that the 'metaField' itself is never included into the bucket spec's
// fieldSet, its usage is tracked by 'unpackNode->includeMeta' instead.
CellPathReqsRet getCellPathReqs(const UnpackTsBucketNode* unpackNode) {
    const auto& fieldSet = unpackNode->bucketSpec.fieldSet();
    const auto& computedFromMeta = unpackNode->bucketSpec.computedMetaProjFields();

    CellPathReqsRet ret;
    for (const auto& field : fieldSet) {
        if (computedFromMeta.find(field) == computedFromMeta.end()) {
            // For each path requested by the query we generate a 'topLevelPath' version, which is
            // just the value of the top level field, with no traversal.
            ret.topLevelPaths.emplace_back(sbe::value::CellBlock::PathRequest(
                sbe::value::CellBlock::kProject,
                {sbe::value::CellBlock::Get{field}, sbe::value::CellBlock::Id{}}));
        }
    }

    // The event filter must work on top of "traversed" version of the data, which, when accessed,
    // has array elements flattened.
    if (unpackNode->eventFilter) {
        DepsTracker eventFilterDeps;
        ;
        dependency_analysis::addDependencies(unpackNode->eventFilter.get(), &eventFilterDeps);
        for (const auto& path : eventFilterDeps.fields) {
            auto rootField = std::string{FieldPath::extractFirstFieldFromDottedPath(path)};
            // Check that the collected path doesn't start from a metadata field, and that it's one
            // of the fields that the query uses.
            if (fieldSet.find(rootField) != fieldSet.end() &&
                computedFromMeta.find(rootField) == computedFromMeta.end()) {

                FieldPath fp(path);
                sbe::value::CellBlock::PathRequest pReq(sbe::value::CellBlock::kFilter);
                for (size_t i = 0; i < fp.getPathLength(); i++) {
                    pReq.path.insert(pReq.path.end(),
                                     {sbe::value::CellBlock::Get{std::string{fp.getFieldName(i)}},
                                      sbe::value::CellBlock::Traverse{}});
                }
                pReq.path.emplace_back(sbe::value::CellBlock::Id{});
                ret.traversePaths.emplace_back(std::move(pReq));
            }
        }
    }

    return ret;
}

// TODO SERVER-80243: Remove this function once the stage builder is stable.
constexpr bool kDebugPlan = false;
void printPlan(const sbe::PlanStage& stage) {
    if constexpr (!kDebugPlan) {
        return;
    }

    static sbe::DebugPrinter debugPrinter(true);
    std::cout << std::endl << debugPrinter.print(stage) << std::endl << std::endl;
}
}  // namespace

SbStage buildBlockToRow(SbStage stage, StageBuilderState& state, PlanStageSlots& outputs) {
    auto [outStage, _] = buildBlockToRow(std::move(stage), state, outputs, SbSlotVector{});
    return std::move(outStage);
}

std::pair<SbStage, SbSlotVector> buildBlockToRow(SbStage stage,
                                                 StageBuilderState& state,
                                                 PlanStageSlots& outputs,
                                                 SbSlotVector individualSlots) {
    // For this stage we output the 'topLevelSlots' (i.e. kField) and NOT the 'traversedSlots' (i.e.
    // kFilterCellField).
    using UnownedSlotName = PlanStageSlots::UnownedSlotName;

    SbBuilder b(state, stage->getCommonStats()->nodeId);

    SbSlotVector blockSlots;
    absl::flat_hash_map<sbe::value::SlotId, size_t> blockSlotIdToPosMap;

    std::vector<PlanStageSlots::OwnedSlotName> fieldNames, outputsToRemove;

    // Helper lambda for processing slots and adding them to 'blockSlots' and 'blockSlotIdToPosMap'
    // if appropriate. The lambda will return true if the slot was added to 'blockSlots' or it was
    // already present in 'blockSlots', otherwise the lambda will return false.
    auto processSlot = [&](SbSlot slot) -> bool {
        auto typeSig = slot.getTypeSignature();
        // If slot is a Block or Cell, add it to 'blockSlotIdToPosMap'.
        if (typeSig &&
            (TypeSignature::kBlockType.isSubset(*typeSig) ||
             TypeSignature::kCellType.isSubset(*typeSig))) {
            if (!blockSlotIdToPosMap.count(slot.getId())) {
                size_t pos = blockSlots.size();
                blockSlots.push_back(slot);
                blockSlotIdToPosMap.emplace(slot.getId(), pos);
            }
            return true;
        }
        return false;
    };

    for (const auto& [name, slot] : outputs.getAllNameSlotPairsInOrder()) {
        if (name.first == PlanStageSlots::kField) {
            if (processSlot(slot)) {
                fieldNames.push_back(PlanStageSlots::OwnedSlotName{name});
            }
        }
        if (name.first == PlanStageSlots::kFilterCellField) {
            outputsToRemove.push_back(PlanStageSlots::OwnedSlotName{name});
        }
    }

    for (const auto& slot : individualSlots) {
        processSlot(slot);
    }

    auto bitmapSlot = outputs.get(PlanStageSlots::kBlockSelectivityBitmap);

    // Remove all the slots that should not be propagated.
    outputs.clear(PlanStageSlots::kBlockSelectivityBitmap);
    for (const auto& name : outputsToRemove) {
        outputs.clear(name);
    }

    // If there aren't any required block slots, use the bitmap slot.
    if (blockSlots.empty()) {
        blockSlots.push_back(bitmapSlot);
    }

    // Add the BlockToRowStage.
    auto [outStage, unpackedSlots] = b.makeBlockToRow(std::move(stage), blockSlots, bitmapSlot);
    stage = std::move(outStage);

    printPlan(*stage);

    // Update 'outputs' after the BlockToRow stage.
    for (const auto& fieldName : fieldNames) {
        auto slot = outputs.get(fieldName);
        if (auto it = blockSlotIdToPosMap.find(slot.getId()); it != blockSlotIdToPosMap.end()) {
            size_t pos = it->second;
            outputs.set(fieldName, unpackedSlots[pos]);
        }
    }

    // Update 'individualSlots' after the BlockToRow stage.
    for (auto& slot : individualSlots) {
        if (auto it = blockSlotIdToPosMap.find(slot.getId()); it != blockSlotIdToPosMap.end()) {
            size_t pos = it->second;
            slot = unpackedSlots[pos];
        }
    }

    return {std::move(stage), std::move(individualSlots)};
}

SbExpr buildVectorizedExpr(StageBuilderState& state,
                           SbExpr scalarExpression,
                           const PlanStageSlots& outputs,
                           bool forFilterStage) {
    if (!scalarExpression || scalarExpression.isFinishedOptimizing()) {
        // If this SbExpr is null or if it's marked as "finished optimizing", then do nothing
        // and return.
        return {};
    }

    // If 'scalarExpression' is a constant expression, then don't bother populating
    // 'variableTypes' because optimize() doesn't need it.
    auto typesToExclude = TypeSignature::kBlockType.include(TypeSignature::kCellType);
    boost::optional<VariableTypes> variableTypes = !scalarExpression.isConstantExpr()
        ? boost::make_optional(excludeTypes(buildVariableTypes(outputs), typesToExclude))
        : boost::none;
    VariableTypes* varTypes = variableTypes ? &*variableTypes : nullptr;

    // Call optimize() with block variables exposed as scalar types.
    scalarExpression.optimize(state, varTypes);

    // If 'scalarExpression' is a local variable, there's nothing we can do, so return
    // a null SbExpr.
    if (scalarExpression.isLocalVarExpr()) {
        return {};
    }

    // If 'scalarExpression' is a constant expression, then it is compatible with block
    // processing, so we can just return it now without having do to any further processing.
    if (scalarExpression.isConstantExpr()) {
        return scalarExpression;
    }

    if (scalarExpression) {
        auto purpose = forFilterStage ? Vectorizer::Purpose::Filter : Vectorizer::Purpose::Project;
        const bool mayHaveCollation = state.getCollatorSlot().has_value();

        Vectorizer vectorizer(state.frameIdGenerator, purpose, mayHaveCollation);

        // If we have an active bitmap, let the vectorizer know.
        auto bitmapSlot = outputs.getIfExists(PlanStageSlots::kBlockSelectivityBitmap);

        Vectorizer::VariableTypes bindings;
        for (const SbSlot& slot : outputs.getAllSlotsInOrder()) {
            if (auto typeSig = slot.getTypeSignature()) {
                bindings.emplace(slot.toProjectionName(), std::make_pair(*typeSig, boost::none));
            }
        }

        auto abt = scalarExpression.extractABT();

        Vectorizer::Tree blockABT = vectorizer.vectorize(abt, bindings, bitmapSlot);

        if (blockABT.expr.has_value()) {
            // Move the ABT into a new SbExpr and call optimize() without the information about
            // the type of the slots, as they are now block variables that are not supported by
            // the type checker. Manually set the type signature of the SbExpr to be whatever
            // was reported by the vectorizer.
            auto e = SbExpr{std::move(*blockABT.expr)};
            e.optimize(state);
            e.setTypeSignature(blockABT.typeSignature);

            // Mark this SbExpr as "finished optimizing" so that it won't get optimized again
            // when lower() gets called.
            e.setFinishedOptimizing();

            return e;
        }
    }

    return {};
}

std::pair<SbStage, PlanStageSlots> SlotBasedStageBuilder::buildUnpackTsBucket(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    SbBuilder b(_state, root->nodeId());

    const auto unpackNode = static_cast<const UnpackTsBucketNode*>(root);

    // Setup the request for the child stage that should place the bucket to be unpacked into a
    // materialized result object.
    PlanStageReqs childReqs = reqs.copyForChild().clearAllFields().setResultObj();
    // Computing fields from 'meta' should have been pushed below unpacking as projection stages
    // over the buckets collection, so the child stage must be able to publish the slots.
    for (const auto& fieldName : unpackNode->bucketSpec.computedMetaProjFields()) {
        childReqs.set(std::pair(PlanStageSlots::kField, fieldName));
    }

    // We have no way to know whether the child stages would produce the 'meta' field at the bucket
    // level (e.g. they would, if there is a filter on 'meta'), but if we need the field after
    // unpacking we might as well request it from the child rather than populate it ourselves.
    const auto metaInBucket = std::pair(PlanStageSlots::kField, timeseries::kBucketMetaFieldName);
    if (unpackNode->includeMeta) {
        childReqs.set(metaInBucket);
    }

    // Build the child tree.
    auto [childStage, childOutputs] = build(unpackNode->children[0].get(), childReqs);

    // We'll publish to the 'outputs' all slots, produced by the tree built in this function, even
    // if they are not requested explicitly by the parent stage. There is no harm in over-
    // publishing but it's convenient to use unified 'outputs' while building the tree.
    // The set of the fields visible to the parent stage is ultimately defined by the 'unpackNode'.
    // However, the parent stage might requests field that are not published (e.g. field "b" in
    // pipeline like [{$project: {c: 1}},{$replaceRoot: {newRoot: {z: "$b"}}}]). We'll have to deal
    // with this if we are not producing a materialized result object (see later in this function).
    PlanStageSlots outputs;

    // Propagate the 'meta' and fields computed from 'meta' into the 'outputs'.
    if (unpackNode->includeMeta) {
        const boost::optional<std::string>& metaFieldName = unpackNode->bucketSpec.metaField();
        tassert(7969800, "'metaField' isn't defined but requested", metaFieldName);
        outputs.set(std::pair(PlanStageSlots::kField, *metaFieldName),
                    childOutputs.get(metaInBucket));
    }
    for (const auto& fieldName : unpackNode->bucketSpec.computedMetaProjFields()) {
        outputs.set(std::pair(PlanStageSlots::kField, fieldName),
                    childOutputs.get(std::pair(PlanStageSlots::kField, fieldName)));
    }
    auto bucketSlot = childOutputs.getResultObj();

    // The 'TsBucketToCellBlockStage' and 'BlockToRowStage' together transform a single bucket into
    // a sequence of "rows" with fields, extracted from the bucket's data. The stages between these
    // two do block processing over the cells.
    auto [topLevelReqs, traverseReqs] = getCellPathReqs(unpackNode);

    // Add a TsBucketToCellBlock stage. Among other things, this stage generates a "default" bitmap
    // of all 1s in to this slot. The bitmap represents which documents are present (1) and which
    // have been filtered (0). This bitmap is carried around until the block_to_row stage.
    auto [stage, bitmapSlot, topLevelSlots, traverseSlots] =
        b.makeTsBucketToCellBlock(std::move(childStage),
                                  bucketSlot,
                                  topLevelReqs,
                                  traverseReqs,
                                  unpackNode->bucketSpec.timeField());

    printPlan(*stage);

    outputs.set(PlanStageSlots::kBlockSelectivityBitmap, bitmapSlot);

    for (size_t i = 0; i < topLevelReqs.size(); ++i) {
        auto key = std::pair(PlanStageSlots::kField, topLevelReqs[i].getTopLevelField());
        SbSlot slot = topLevelSlots[i];
        outputs.set(key, slot);
    }

    for (size_t i = 0; i < traverseReqs.size(); ++i) {
        auto key = std::pair(PlanStageSlots::kFilterCellField, traverseReqs[i].getFullPath());
        SbSlot slot = traverseSlots[i];
        outputs.set(key, slot);
    }

    MatchExpression* eventFilter = unpackNode->eventFilter.get();

    // It's possible for the event filter to be applied on fields that aren't being unpacked (the
    // simplest case of such pipeline: [{$project: {x: 1}},{$match: {y: 42}}]). We'll stub out the
    // non-produced fields with the 'Nothing' slot.
    {
        DepsTracker eventFilterDeps;
        dependency_analysis::addDependencies(eventFilter, &eventFilterDeps);
        for (const std::string& eventFilterPath : eventFilterDeps.fields) {
            if (eventFilterPath.empty()) {
                continue;
            }
            const auto& name =
                std::pair(PlanStageSlots::kField, std::string{FieldPath(eventFilterPath).front()});
            if (!outputs.has(name)) {
                outputs.set(name, SbSlot{_state.getNothingSlot()});
            }
        }
    }

    if (eventFilter) {
        auto eventFilterSbExpr =
            generateFilter(_state, eventFilter, boost::none /* rootSlot */, outputs);

        auto [newStage, isVectorised] = buildVectorizedFilterExpr(
            std::move(stage), reqs, std::move(eventFilterSbExpr), outputs, unpackNode->nodeId());
        stage = std::move(newStage);

        if (!isVectorised) {
            // The last step was to convert the block to row. Generate the filter expression
            // again to use the scalar slots instead of the block slots.
            auto eventFilterSbExpr =
                generateFilter(_state, eventFilter, boost::none /* rootSlot */, outputs);
            if (!eventFilterSbExpr.isNull()) {
                stage = b.makeFilter(std::move(stage), std::move(eventFilterSbExpr));
                printPlan(*stage);
            }
        }
    } else {
        // Insert a BlockToRow stage and let the rest of the pipeline work on scalar values if:
        // - we are supposed to return a BSON result
        // - the caller doesn't support working on block values
        if (reqs.hasResult() || !reqs.getCanProcessBlockValues()) {
            stage = buildBlockToRow(std::move(stage), _state, outputs);
        }
    }

    // If the parent wants us to produce a result object, create an object with all published
    // fields.
    if (reqs.hasResult()) {
        SbExpr::Vector newBsonObjArgs;
        for (auto&& p : outputs.getAllNameSlotPairsInOrder()) {
            auto& name = p.first;
            auto& slot = p.second;
            if (name.first == PlanStageSlots::kField) {
                newBsonObjArgs.emplace_back(b.makeStrConstant(name.second));
                newBsonObjArgs.emplace_back(slot);
            }
        }

        auto newBsonObjExpr = b.makeFunction("newBsonObj"_sd, std::move(newBsonObjArgs));

        auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(newBsonObjExpr));
        stage = std::move(outStage);

        auto resultSlot = outSlots[0];

        outputs.setResultObj(resultSlot);
        outputs.clear(PlanStageSlots::kBlockSelectivityBitmap);
    } else {
        // As we are not producing a result record, we must fulfill all reqs in a way that would be
        // equivalent to fetching the same fields from the result object, that is, we'll map the
        // fields to the environtment's 'Nothing' slot.
        auto reqsWithResultObj = reqs.copyForChild().setResultObj();

        outputs.setMissingRequiredNamedSlotsToNothing(_state, reqsWithResultObj);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
