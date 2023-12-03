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
#include "mongo/db/matcher/expression_algo.h"
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
            ret.topLevelPaths.emplace_back(
                sv::CellBlock::PathRequest{{sv::CellBlock::Get{field}, sv::CellBlock::Id{}}});
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
                sv::CellBlock::PathRequest pReq;
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
        ret.topLevelPaths.push_back(sv::CellBlock::PathRequest{
            {sv::CellBlock::Get{unpackNode->bucketSpec.timeField()}, sv::CellBlock::Id{}}});
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

std::unique_ptr<sbe::EExpression> buildAndTree(sbe::EExpression::Vector& vec,
                                               size_t beginIdx,
                                               size_t endIdx) {
    if (beginIdx == endIdx) {
        return nullptr;
    }

    if (beginIdx + 1 == endIdx) {
        return std::move(vec[beginIdx]);
    }

    auto midPt = (beginIdx + endIdx) / 2;
    auto left = buildAndTree(vec, beginIdx, midPt);
    auto right = buildAndTree(vec, midPt, endIdx);

    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::Op::logicAnd, std::move(left), std::move(right));
}
}  // namespace

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::buildBlockToRow(
    std::unique_ptr<sbe::PlanStage> stage, PlanStageSlots& outputs) {
    // For this stage we output the 'topLevelSlots' (i.e. kField) and NOT the 'traversedSlots' (i.e.
    // kFilterCellField).
    sbe::value::SlotVector blockSlots;
    std::vector<PlanStageSlots::UnownedSlotName> blockSlotNames, outputsToRemove;
    outputsToRemove.push_back(PlanStageSlots::kBlockSelectivityBitmap);
    for (const auto& slot : outputs.getAllNamedSlotsInOrder()) {
        if (slot.first.first == PlanStageSlots::kField &&
            (TypeSignature::kBlockType.isSubset(slot.second.typeSignature) ||
             TypeSignature::kCellType.isSubset(slot.second.typeSignature))) {
            blockSlots.push_back(slot.second.slotId);
            blockSlotNames.push_back(slot.first);
        }
        if (slot.first.first == PlanStageSlots::kFilterCellField) {
            outputsToRemove.push_back(slot.first);
        }
    }
    tassert(7953900, "The list of block-value slots must not be empty", !blockSlots.empty());
    auto unpackedSlots = _slotIdGenerator.generateMultiple(blockSlots.size());
    // Adds the BlockToRowStage.
    PlanNodeId nodeId = stage->getCommonStats()->nodeId;
    stage = std::make_unique<sbe::BlockToRowStage>(
        std::move(stage),
        blockSlots,
        unpackedSlots,
        outputs.getSlotIfExists(PlanStageSlots::kBlockSelectivityBitmap),
        nodeId,
        _yieldPolicy);
    printPlan(*stage);

    // Remove all the slots that should not be propagated.
    for (size_t i = 0; i < outputsToRemove.size(); ++i) {
        outputs.clear(outputsToRemove[i]);
    }

    // After the BlockToRow stage, the block fields are now scalar values, in a different slot.
    for (size_t i = 0; i < blockSlotNames.size(); ++i) {
        outputs.set(blockSlotNames[i],
                    TypedSlot{unpackedSlots[i],
                              outputs.get(blockSlotNames[i])
                                  .typeSignature.exclude(TypeSignature::kBlockType)
                                  .exclude(TypeSignature::kCellType)});
    }

    return stage;
}

boost::optional<TypedExpression> SlotBasedStageBuilder::buildVectorizedExpr(SbExpr scalarExpression,
                                                                            PlanStageSlots& outputs,
                                                                            bool forFilterStage) {
    if (scalarExpression.hasABT()) {
        auto abt = abt::unwrap(scalarExpression.extractABT());
        // Prepare a temporary object to expose the block variables as scalar types.
        VariableTypes varTypes;

        for (const TypedSlot& slot : outputs.getAllSlotsInOrder()) {
            varTypes.emplace(getABTVariableName(slot.slotId),
                             slot.typeSignature.exclude(TypeSignature::kBlockType)
                                 .exclude(TypeSignature::kCellType));
        }

        constantFold(abt, _state, &varTypes);

        if (abt.is<optimizer::Constant>()) {
            // We consider constant expressions as compatible with block processing.
            auto [tag, value] = abt.cast<optimizer::Constant>()->get();
            return TypedExpression{sbe::makeE<sbe::EConstant>(tag, value), getTypeSignature(tag)};
        } else {
            Vectorizer vectorizer(_state.frameIdGenerator,
                                  forFilterStage ? Vectorizer::Purpose::Filter
                                                 : Vectorizer::Purpose::Project);

            // If we have an active bitmap, let the vectorizer know.
            auto bitmapSlot = outputs.getSlotIfExists(PlanStageSlots::kBlockSelectivityBitmap);

            Vectorizer::VariableTypes bindings;
            for (const TypedSlot& slot : outputs.getAllSlotsInOrder()) {
                bindings.emplace(getABTVariableName(slot.slotId),
                                 std::make_pair(slot.typeSignature, boost::none));
            }
            Vectorizer::Tree blockABT = vectorizer.vectorize(abt, bindings, bitmapSlot);

            if (blockABT.expr.has_value()) {
                // Run the conversion from ABT to EExpression without the information about the type
                // of the slots, as they are now block variables that are not supported by the type
                // checker. Report just the type of the root expression as reported by the
                // vectorizer.
                return TypedExpression{abtToExpr(*blockABT.expr, _state).expr,
                                       blockABT.typeSignature};
            }
        }
    }
    return boost::none;
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildUnpackTsBucket(const QuerySolutionNode* root,
                                           const PlanStageReqs& reqs) {
    const auto unpackNode = static_cast<const UnpackTsBucketNode*>(root);

    // Setup the request for the child stage that should place the bucket to be unpacked into the
    // kResult slot.
    PlanStageReqs childReqs = reqs.copyForChild().clearAllFields().clearMRInfo().setResult();
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
    // with this if we are not producing a 'kResult' (see later in this function).
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
    auto bucketSlot = childOutputs.get(kResult);

    // The 'TsBucketToCellBlockStage' and 'BlockToRowStage' together transform a single bucket into
    // a sequence of "rows" with fields, extracted from the bucket's data. The stages between these
    // two do block processing over the cells.
    auto [topLevelReqs, traverseReqs] = getCellPathReqs(unpackNode);
    auto allReqs = topLevelReqs;
    allReqs.insert(allReqs.end(), traverseReqs.begin(), traverseReqs.end());

    auto allCellSlots = _slotIdGenerator.generateMultiple(allReqs.size());
    auto traversedCellSlots =
        sbe::value::SlotVector(allCellSlots.begin() + topLevelReqs.size(), allCellSlots.end());

    std::unique_ptr<sbe::PlanStage> stage =
        std::make_unique<sbe::TsBucketToCellBlockStage>(std::move(childStage),
                                                        bucketSlot.slotId,
                                                        allReqs,
                                                        allCellSlots,
                                                        boost::none /* metaField slot*/,
                                                        unpackNode->bucketSpec.timeField(),
                                                        unpackNode->nodeId());
    printPlan(*stage);

    // Declare the top level fields produced as block values.
    for (size_t i = 0; i < topLevelReqs.size(); ++i) {
        auto field = getTopLevelField(topLevelReqs[i]);
        auto key = std::make_pair(PlanStageSlots::kField, field);
        if (field == unpackNode->bucketSpec.timeField()) {
            outputs.set(key,
                        TypedSlot{allCellSlots[i],
                                  TypeSignature::kCellType.include(TypeSignature::kDateTimeType)});
        } else {
            outputs.set(key,
                        TypedSlot{allCellSlots[i],
                                  TypeSignature::kCellType.include(TypeSignature::kAnyScalarType)});
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
                outputs.set(name, _state.env->getSlot(kNothingEnvSlotName));
            }
        }
    }

    boost::optional<sbe::value::SlotId> bitmapSlotId;
    if (eventFilter) {
        auto eventFilterSbExpr =
            generateFilter(_state, eventFilter, /*rootSlot*/ boost::none, &outputs);
        auto filterExpr = buildVectorizedExpr(std::move(eventFilterSbExpr), outputs, true);
        if (filterExpr.has_value()) {
            if (auto constExpr = filterExpr->expr->as<sbe::EConstant>(); constExpr) {
                auto [tag, val] = constExpr->getConstant();
                // The expression is a scalar constant, it must be a boolean value.
                tassert(7969850,
                        "Expected true or false value for filter",
                        tag == sbe::value::TypeTags::Boolean);
                if (sbe::value::bitcastTo<bool>(val)) {
                    eventFilter = nullptr;
                } else {
                    stage = makeS<sbe::FilterStage<true>>(
                        std::move(stage),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                   sbe::value::bitcastFrom<bool>(false)),
                        unpackNode->nodeId());
                }
            } else if (TypeSignature::kBlockType.include(TypeSignature::kBooleanType)
                           .isSubset(filterExpr->typeSignature)) {
                // We successfully created an expression working on the block values and
                // returning a block of boolean values; attach it to a project stage and use
                // the result as the bitmap for the BlockToRow stage.
                sbe::value::SlotId bitmapSlotId = _state.slotId();
                sbe::SlotExprPairVector projects;
                projects.emplace_back(bitmapSlotId, std::move(filterExpr->expr));

                stage = sbe::makeS<sbe::ProjectStage>(
                    std::move(stage), std::move(projects), unpackNode->nodeId());
                printPlan(*stage);

                // Add a filter stage that pulls new data if there isn't at least one 'true' value
                // in the produced bitmap.
                SbExprBuilder b(_state);
                auto filterSbExpr = b.makeNot(b.makeFunction("valueBlockNone"_sd,
                                                             b.makeVariable(SbVar{bitmapSlotId}),
                                                             b.makeBoolConstant(true)));
                stage = sbe::makeS<sbe::FilterStage<false>>(
                    std::move(stage), filterSbExpr.extractExpr(_state).expr, unpackNode->nodeId());
                printPlan(*stage);

                outputs.set(
                    PlanStageSlots::kBlockSelectivityBitmap,
                    TypedSlot{bitmapSlotId,
                              TypeSignature::kBlockType.include(TypeSignature::kBooleanType)});
                // Reset the variable so that the filter is not generated as a stage in the
                // scalar section of the pipeline.
                eventFilter = nullptr;
            }
        }
    }

    // Insert a BlockToRow stage and let the rest of the pipeline work on scalar values if:
    // - we have a filter that we could not vectorize
    // - we are supposed to return a BSON result
    // - the caller doesn't support working on block values
    if (eventFilter || reqs.hasResultOrMRInfo() || !reqs.getCanProcessBlockValues()) {
        stage = buildBlockToRow(std::move(stage), outputs);
    }

    // Add filter stage(s) for the per-event filter.
    if (eventFilter) {
        auto [eventFilterByPath, eventFilterResidual] =
            expression::splitMatchExpressionForColumns(eventFilter);
        {
            sbe::EExpression::Vector andBranches;
            for (auto& [_ /* path */, filterMatchExpr] : eventFilterByPath) {
                auto eventFilterSbExpr = generateFilter(
                    _state, filterMatchExpr.get(), /*rootSlot*/ boost::none, &outputs);

                andBranches.push_back(eventFilterSbExpr.extractExpr(_state).expr);
            }

            auto combinedFilter = buildAndTree(andBranches, 0, andBranches.size());

            if (combinedFilter) {
                stage = sbe::makeS<sbe::FilterStage<false>>(
                    std::move(stage), std::move(combinedFilter), unpackNode->nodeId());
                printPlan(*stage);
            }
        }

        // Adds a filter for the residual predicates.
        if (eventFilterResidual) {
            auto eventFilterSbExpr = generateFilter(
                _state, eventFilterResidual.get(), /*rootSlot*/ boost::none, &outputs);
            if (!eventFilterSbExpr.isNull()) {
                stage =
                    sbe::makeS<sbe::FilterStage<false>>(std::move(stage),
                                                        eventFilterSbExpr.extractExpr(_state).expr,
                                                        unpackNode->nodeId());
                printPlan(*stage);
            }
        }
    }

    // If the parent wants us to materialize kResult, create an object with all published fields.
    if (reqs.hasResultOrMRInfo()) {
        std::vector<std::string> fieldNames;
        sbe::value::SlotVector fieldSlots;

        // The outputs could contain the time field even when not requested, remove it.
        if (!unpackNode->bucketSpec.fieldSet().contains(unpackNode->bucketSpec.timeField())) {
            outputs.clear(
                std::make_pair(PlanStageSlots::kField, unpackNode->bucketSpec.timeField()));
        }

        for (auto&& p : outputs.getAllNamedSlotsInOrder()) {
            auto& name = p.first;
            auto& slot = p.second;
            if (name.first == PlanStageSlots::kField) {
                fieldNames.push_back(std::string{name.second});
                fieldSlots.push_back(slot.slotId);
            }
        }

        auto resultSlot = _slotIdGenerator.generate();
        outputs.set(kResult, resultSlot);

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
        // equivalent to fetching the same fields from 'kResult', that is, we'll map the fields to
        // the environtment's 'Nothing' slot.
        auto nothingSlot =
            TypedSlot{_state.env->getSlot(kNothingEnvSlotName), TypeSignature::kAnyScalarType};

        auto reqsWithoutMakeResultInfo = reqs.copyForChild().clearMRInfo().setResult();
        outputs.setMissingRequiredNamedSlots(reqsWithoutMakeResultInfo, nothingSlot);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
