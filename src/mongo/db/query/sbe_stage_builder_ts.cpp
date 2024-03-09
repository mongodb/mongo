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

#include <iostream>
#include <utility>
#include <vector>

#include "mongo/db/exec/sbe/stages/block_to_row.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/ts_bucket_to_cell_block.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_impl.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr_helpers.h"
#include "mongo/db/query/sbe_stage_builder_vectorizer.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {
namespace sv = sbe::value;

namespace {
struct CellPathReqsRet {
    std::vector<sv::CellBlock::PathRequest> topLevelPaths;
    std::vector<sv::CellBlock::PathRequest> traversePaths;
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
            ret.topLevelPaths.emplace_back(sv::CellBlock::PathRequest(
                sv::CellBlock::kProject, {sv::CellBlock::Get{field}, sv::CellBlock::Id{}}));
        }
    }

    // The event filter must work on top of "traversed" version of the data, which, when accessed,
    // has array elements flattened.
    if (unpackNode->eventFilter) {
        DepsTracker eventFilterDeps = {};
        match_expression::addDependencies(unpackNode->eventFilter.get(), &eventFilterDeps);
        for (const auto& path : eventFilterDeps.fields) {
            auto rootField = FieldPath::extractFirstFieldFromDottedPath(path).toString();
            // Check that the collected path doesn't start from a metadata field, and that it's one
            // of the fields that the query uses.
            if (fieldSet.find(rootField) != fieldSet.end() &&
                computedFromMeta.find(rootField) == computedFromMeta.end()) {

                FieldPath fp(path);
                sv::CellBlock::PathRequest pReq(sv::CellBlock::kFilter);
                for (size_t i = 0; i < fp.getPathLength(); i++) {
                    pReq.path.insert(pReq.path.end(),
                                     {sv::CellBlock::Get{fp.getFieldName(i).toString()},
                                      sv::CellBlock::Traverse{}});
                }
                pReq.path.emplace_back(sv::CellBlock::Id{});
                ret.traversePaths.emplace_back(std::move(pReq));
            }
        }
    }

    // If there are no required paths, the parent is expecting the unpacking to produce the same
    // number of results as there are events in the bucket but it doesn't care about the result's
    // shape. For example, this comes up with "count-like" queries that for some reason failed to
    // optimize unpacking away completely. Ideally, we would check the bucket's count and produce
    // that many empty objects, but the block stages aren't setup to do this easily so we will
    // instead unpack the known-to-always-exist 'timeField' from the bucket without adding it to the
    // outputs.
    if (ret.topLevelPaths.empty()) {
        tassert(8032300,
                "Should have no traverse fields if there are no top-level fields",
                ret.traversePaths.empty());
        ret.topLevelPaths.push_back(sv::CellBlock::PathRequest(
            sv::CellBlock::kProject,
            {sv::CellBlock::Get{unpackNode->bucketSpec.timeField()}, sv::CellBlock::Id{}}));
    }

    return ret;
}

const std::string& getTopLevelField(const sv::CellBlock::PathRequest& pathReq) {
    return get<sv::CellBlock::Get>(pathReq.path[0]).field;
}

std::string getFullPath(const sv::CellBlock::PathRequest& pathReq) {
    StringBuilder sb;
    for (const auto& path : pathReq.path) {
        if (holds_alternative<sv::CellBlock::Get>(path)) {
            if (sb.len() != 0) {
                sb.append(".");
            }
            sb.append(get<sv::CellBlock::Get>(path).field);
        }
    }
    return sb.str();
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

std::unique_ptr<sbe::PlanStage> buildBlockToRow(std::unique_ptr<sbe::PlanStage> stage,
                                                StageBuilderState& state,
                                                PlanStageSlots& outputs) {
    auto [outStage, _] = buildBlockToRow(std::move(stage), state, outputs, TypedSlotVector{});
    return std::move(outStage);
}

std::pair<std::unique_ptr<sbe::PlanStage>, TypedSlotVector> buildBlockToRow(
    std::unique_ptr<sbe::PlanStage> stage,
    StageBuilderState& state,
    PlanStageSlots& outputs,
    TypedSlotVector individualSlots) {
    // For this stage we output the 'topLevelSlots' (i.e. kField) and NOT the 'traversedSlots' (i.e.
    // kFilterCellField).
    using UnownedSlotName = PlanStageSlots::UnownedSlotName;

    absl::flat_hash_map<sbe::value::SlotId, sbe::value::SlotId> blockIdToUnpackedIdMap;
    sbe::value::SlotVector blockSlots;
    sbe::value::SlotVector unpackedSlots;

    std::vector<PlanStageSlots::UnownedSlotName> fieldNames, outputsToRemove;
    outputsToRemove.push_back(PlanStageSlots::kBlockSelectivityBitmap);

    auto processSlot = [&](TypedSlot slot, boost::optional<UnownedSlotName> name) {
        auto typeSig = slot.getTypeSignature();
        // If slot is a Block or Cell, add a mapping for it to 'blockIdToUnpackedIdMap'.
        if (typeSig &&
            (TypeSignature::kBlockType.isSubset(*typeSig) ||
             TypeSignature::kCellType.isSubset(*typeSig))) {
            if (!blockIdToUnpackedIdMap.count(slot.getId())) {
                auto slotId = slot.getId();
                auto unpackedId = state.slotId();

                blockIdToUnpackedIdMap[slotId] = unpackedId;
                blockSlots.push_back(slotId);
                unpackedSlots.push_back(unpackedId);
            }
        }
    };

    for (const auto& [name, slot] : outputs.getAllNameSlotPairsInOrder()) {
        if (name.first == PlanStageSlots::kField) {
            fieldNames.push_back(name);
            processSlot(slot, name);
        }
        if (name.first == PlanStageSlots::kFilterCellField) {
            outputsToRemove.push_back(name);
        }
    }

    for (const auto& slot : individualSlots) {
        processSlot(slot, boost::none);
    }

    // If there aren't any required block slots, use the bitmap.
    if (blockSlots.empty()) {
        // If we have no block slots, tell block_to_row to unwind the bitset itself.
        auto slotId = outputs.get(PlanStageSlots::kBlockSelectivityBitmap).getId();

        blockSlots.push_back(slotId);
        unpackedSlots.push_back(state.slotId());
    }

    // Adds the BlockToRowStage.
    PlanNodeId nodeId = stage->getCommonStats()->nodeId;
    stage = std::make_unique<sbe::BlockToRowStage>(
        std::move(stage),
        blockSlots,
        unpackedSlots,
        outputs.get(PlanStageSlots::kBlockSelectivityBitmap).getId(),
        nodeId,
        state.yieldPolicy);
    printPlan(*stage);

    // Remove all the slots that should not be propagated.
    for (size_t i = 0; i < outputsToRemove.size(); ++i) {
        outputs.clear(outputsToRemove[i]);
    }

    // Update 'outputs' after the BlockToRow stage.
    for (const auto& fieldName : fieldNames) {
        auto slot = outputs.get(fieldName);
        auto it = blockIdToUnpackedIdMap.find(slot.getId());
        if (it != blockIdToUnpackedIdMap.end()) {
            // 'slot' and 'unpackedSlot' will have the same type except that unpackedSlot's
            // type will be scalar.
            auto typeSig = slot.getTypeSignature();
            auto unpackedTypeSig =
                typeSig->exclude(TypeSignature::kBlockType).exclude(TypeSignature::kCellType);

            auto unpackedSlot = TypedSlot{it->second, unpackedTypeSig};
            outputs.set(fieldName, unpackedSlot);
        }
    }

    // Update 'individualSlots' after the BlockToRow stage.
    for (auto& slot : individualSlots) {
        auto it = blockIdToUnpackedIdMap.find(slot.getId());
        if (it != blockIdToUnpackedIdMap.end()) {
            // 'slot' and 'unpackedSlot' will have the same type except that unpackedSlot's
            // type will be scalar.
            auto typeSig = slot.getTypeSignature();
            auto unpackedTypeSig =
                typeSig->exclude(TypeSignature::kBlockType).exclude(TypeSignature::kCellType);

            auto unpackedSlot = TypedSlot{it->second, unpackedTypeSig};
            slot = unpackedSlot;
        }
    }

    return {std::move(stage), std::move(individualSlots)};
}

SbExpr buildVectorizedExpr(StageBuilderState& state,
                           SbExpr scalarExpression,
                           PlanStageSlots& outputs,
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

    if (scalarExpression.canExtractABT()) {
        Vectorizer vectorizer(state.frameIdGenerator,
                              forFilterStage ? Vectorizer::Purpose::Filter
                                             : Vectorizer::Purpose::Project);

        // If we have an active bitmap, let the vectorizer know.
        auto bitmapSlot = outputs.getSlotIfExists(PlanStageSlots::kBlockSelectivityBitmap);

        Vectorizer::VariableTypes bindings;
        for (const TypedSlot& slot : outputs.getAllSlotsInOrder()) {
            if (auto typeSig = slot.getTypeSignature()) {
                bindings.emplace(getABTVariableName(slot), std::make_pair(*typeSig, boost::none));
            }
        }

        auto abt = abt::unwrap(scalarExpression.extractABT());

        Vectorizer::Tree blockABT = vectorizer.vectorize(abt, bindings, bitmapSlot);

        if (blockABT.expr.has_value()) {
            // Move the ABT into a new SbExpr and call optimize() without the information about
            // the type of the slots, as they are now block variables that are not supported by
            // the type checker. Manually set the type signature of the SbExpr to be whatever
            // was reported by the vectorizer.
            auto e = SbExpr{abt::wrap(std::move(*blockABT.expr))};
            e.optimize(state);
            e.setTypeSignature(blockABT.typeSignature);

            // Mark this SbExpr as "finished optimizing" so that it won't get optimized again
            // when extractExpr() gets called.
            e.setFinishedOptimizing();

            return e;
        }
    }

    return {};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildUnpackTsBucket(const QuerySolutionNode* root,
                                           const PlanStageReqs& reqs) {
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
    auto allReqs = topLevelReqs;
    allReqs.insert(allReqs.end(), traverseReqs.begin(), traverseReqs.end());

    auto allCellSlots = _slotIdGenerator.generateMultiple(allReqs.size());
    auto traversedCellSlots =
        sbe::value::SlotVector(allCellSlots.begin() + topLevelReqs.size(), allCellSlots.end());

    // The TsBucketToCellBlock stage generates a "default" bitmap of all 1s in to this slot.
    // The bitmap represents which documents are present (1) and which have been filtered (0).
    // This bitmap is carried around until the block_to_row stage.
    const auto bitmapSlotId = _slotIdGenerator.generate();

    std::unique_ptr<sbe::PlanStage> stage =
        std::make_unique<sbe::TsBucketToCellBlockStage>(std::move(childStage),
                                                        bucketSlot.slotId,
                                                        allReqs,
                                                        allCellSlots,
                                                        boost::none /* metaField slot*/,
                                                        bitmapSlotId,
                                                        unpackNode->bucketSpec.timeField(),
                                                        unpackNode->nodeId());
    outputs.set(PlanStageSlots::kBlockSelectivityBitmap, bitmapSlotId);
    printPlan(*stage);

    // Declare the top level fields produced as block values.
    for (size_t i = 0; i < topLevelReqs.size(); ++i) {
        auto field = getTopLevelField(topLevelReqs[i]);
        auto key = std::make_pair(PlanStageSlots::kField, field);
        TypedSlot slot;
        if (field == unpackNode->bucketSpec.timeField()) {
            slot = TypedSlot{allCellSlots[i],
                             TypeSignature::kCellType.include(TypeSignature::kDateTimeType)};
            outputs.set(key, slot);
        } else {
            slot = TypedSlot{allCellSlots[i],
                             TypeSignature::kCellType.include(TypeSignature::kAnyScalarType)};
            outputs.set(key, slot);
        }
    }
    // Declare the traversed fields which can be used for evaluating $match.
    for (size_t i = 0; i < traverseReqs.size(); ++i) {
        auto field = getFullPath(traverseReqs[i]);
        auto key = std::make_pair(PlanStageSlots::kFilterCellField, field);
        if (field == unpackNode->bucketSpec.timeField()) {
            outputs.set(key,
                        TypedSlot{traversedCellSlots[i],
                                  TypeSignature::kCellType.include(TypeSignature::kDateTimeType)});
        } else {
            outputs.set(key,
                        TypedSlot{traversedCellSlots[i],
                                  TypeSignature::kCellType.include(TypeSignature::kAnyScalarType)});
        }
    }

    MatchExpression* eventFilter = unpackNode->eventFilter.get();

    // It's possible for the event filter to be applied on fields that aren't being unpacked (the
    // simplest case of such pipeline: [{$project: {x: 1}},{$match: {y: 42}}]). We'll stub out the
    // non-produced fields with the 'Nothing' slot.
    {
        DepsTracker eventFilterDeps = {};
        match_expression::addDependencies(eventFilter, &eventFilterDeps);
        for (const std::string& eventFilterPath : eventFilterDeps.fields) {
            const auto& name =
                std::pair(PlanStageSlots::kField, FieldPath(eventFilterPath).front().toString());
            if (!outputs.has(name)) {
                outputs.set(name, _state.getNothingSlot());
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
                stage = sbe::makeS<sbe::FilterStage<false>>(
                    std::move(stage), eventFilterSbExpr.extractExpr(_state), unpackNode->nodeId());
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
        std::vector<std::string> fieldNames;
        sbe::value::SlotVector fieldSlots;

        // The outputs could contain the time field even when not requested, remove it.
        if (!unpackNode->bucketSpec.fieldSet().contains(unpackNode->bucketSpec.timeField())) {
            outputs.clear(
                std::make_pair(PlanStageSlots::kField, unpackNode->bucketSpec.timeField()));
        }

        for (auto&& p : outputs.getAllNameSlotPairsInOrder()) {
            auto& name = p.first;
            auto& slot = p.second;
            if (name.first == PlanStageSlots::kField) {
                fieldNames.push_back(std::string{name.second});
                fieldSlots.push_back(slot.slotId);
            }
        }

        auto resultSlot = _slotIdGenerator.generate();
        outputs.setResultObj(resultSlot);
        outputs.clear(PlanStageSlots::kBlockSelectivityBitmap);

        stage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(stage),
                                                  resultSlot,                  // objSlot
                                                  boost::none,                 // rootSlot
                                                  boost::none,                 // fieldBehavior
                                                  std::vector<std::string>{},  // fields
                                                  fieldNames,                  // projectFields
                                                  fieldSlots,                  // projectVars
                                                  true,                        // forceNewObject
                                                  false,                       // returnOldObject
                                                  unpackNode->nodeId());
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
