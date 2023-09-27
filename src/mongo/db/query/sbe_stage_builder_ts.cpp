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
#include "mongo/db/query/sbe_stage_builder_vectorizer.h"
#include "mongo/db/timeseries/timeseries_constants.h"

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
// requirements.
CellPathReqsRet getCellPathReqs(const UnpackTsBucketNode* unpackNode) {
    const auto& fieldSet = unpackNode->bucketSpec.fieldSet();
    const auto& computedFromMeta = unpackNode->bucketSpec.computedMetaProjFields();

    CellPathReqsRet ret;
    ret.topLevelPaths.reserve(fieldSet.size() - computedFromMeta.size());
    ret.traversePaths.reserve(fieldSet.size() - computedFromMeta.size());
    for (const auto& field : fieldSet) {
        if (computedFromMeta.find(field) == computedFromMeta.end()) {
            // For each path we generated a "traversed" version, which, when accessed, has array
            // elements flattened. We also generate a 'topLevelPath' version, which is just the
            // value of the top level field, with no traversal.

            ret.traversePaths.emplace_back(sv::CellBlock::PathRequest{
                {sv::CellBlock::Get{field}, sv::CellBlock::Traverse{}, sv::CellBlock::Id{}}});

            ret.topLevelPaths.emplace_back(
                sv::CellBlock::PathRequest{{sv::CellBlock::Get{field}, sv::CellBlock::Id{}}});
        }
    }

    return ret;
};

const std::string& getTopLevelField(const sv::CellBlock::PathRequest& pathReq) {
    return std::get<sv::CellBlock::Get>(pathReq.path[0]).field;
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

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildUnpackTsBucket(const QuerySolutionNode* root,
                                           const PlanStageReqs& reqs) {
    const auto unpackNode = static_cast<const UnpackTsBucketNode*>(root);

    // Setup the request for the child stage that should place the bucket to be unpacked into the
    // kResult slot.
    PlanStageReqs childReqs = reqs.copy().clearAllFields().set(kResult);
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
    // If the parent stage requests fields that are not published (e.g. field "b" in pipeline like
    // [{$project: {a: 1}}, {$project: {x: "$b"}}]), we simply ignore such requests.
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

    // TODO SERVER-79699: Handle the 'wholeBucketFilter'.


    // The 'TsBucketToCellBlockStage' and 'BlockToRowStage' together transform a single bucket into
    // a sequence of "rows" with fields, extracted from the bucket's data. The stages between these
    // two to do block processing over the cells.
    auto [topLevelReqs, traverseReqs] = getCellPathReqs(unpackNode);
    invariant(topLevelReqs.size() == traverseReqs.size());
    auto allReqs = topLevelReqs;
    allReqs.insert(allReqs.end(), traverseReqs.begin(), traverseReqs.end());

    auto allCellSlots = _slotIdGenerator.generateMultiple(allReqs.size());
    auto topLevelSlots = sbe::value::SlotVector(allCellSlots.begin(),
                                                allCellSlots.begin() + allCellSlots.size() / 2);
    auto traversedCellSlots =
        sbe::value::SlotVector(allCellSlots.begin() + allCellSlots.size() / 2, allCellSlots.end());

    std::unique_ptr<sbe::PlanStage> stage =
        std::make_unique<sbe::TsBucketToCellBlockStage>(std::move(childStage),
                                                        bucketSlot.slotId,
                                                        allReqs,
                                                        allCellSlots,
                                                        boost::none /* metaField slot*/,
                                                        unpackNode->bucketSpec.timeField(),
                                                        unpackNode->nodeId());
    printPlan(*stage);

    // Adds slots from CellBlocks, but only the traversed ones which can be used for evaluating
    // $match.  Later we're going to reset the outputs to scalar slots anyway.
    for (size_t i = 0; i < topLevelReqs.size(); ++i) {
        auto field = getTopLevelField(topLevelReqs[i]);
        if (field == unpackNode->bucketSpec.timeField()) {
            outputs.set(std::make_pair(PlanStageSlots::kField, field),
                        TypedSlot{traversedCellSlots[i],
                                  TypeSignature::kCellType.include(TypeSignature::kDateTimeType)});
        } else {
            outputs.set(std::make_pair(PlanStageSlots::kField, field),
                        TypedSlot{traversedCellSlots[i],
                                  TypeSignature::kCellType.include(TypeSignature::kAnyScalarType)});
        }
    }

    boost::optional<sbe::value::SlotId> bitmapSlotId;
    MatchExpression* eventFilter = unpackNode->eventFilter.get();
    if (eventFilter) {
        auto eventFilterSbExpr =
            generateFilter(_state, eventFilter, /*rootSlot*/ boost::none, &outputs);
        if (eventFilterSbExpr.hasABT()) {
            auto abt = abt::unwrap(eventFilterSbExpr.extractABT());
            constantFold(abt, _state);

            Vectorizer vectorizer(_state.frameIdGenerator, Vectorizer::Purpose::Filter);
            Vectorizer::VariableTypes bindings;
            outputs.forEachSlot([&bindings](const TypedSlot& slot) {
                bindings.emplace(getABTVariableName(slot.slotId), slot.typeSignature);
            });
            Vectorizer::Tree blockABT = vectorizer.vectorize(abt, bindings);

            if (blockABT.expr.has_value()) {
                // We successfully created an expression working on the block values and
                // returning a block of boolean values; attach it to a project stage and use
                // the result as the bitmap for the BlockToRow stage.
                auto projExpr = abtToExpr(*blockABT.expr, _state);

                bitmapSlotId = _state.slotId();
                sbe::SlotExprPairVector projects;
                projects.emplace_back(*bitmapSlotId, std::move(projExpr.expr));

                stage = sbe::makeS<sbe::ProjectStage>(
                    std::move(stage), std::move(projects), unpackNode->nodeId());
                printPlan(*stage);

                // Reset the variable so that the filter is not generated as a stage in the
                // scalar section of the pipeline.
                eventFilter = nullptr;
            }
        }
    }

    auto unpackedSlots = _slotIdGenerator.generateMultiple(topLevelReqs.size());

    // Adds the BlockToRowStage.
    // For this stage we output the 'topLevelSlots' and NOT the 'traversedSlots'.
    stage = std::make_unique<sbe::BlockToRowStage>(std::move(stage),
                                                   topLevelSlots,
                                                   unpackedSlots,
                                                   bitmapSlotId,
                                                   unpackNode->nodeId(),
                                                   _yieldPolicy);
    printPlan(*stage);

    // After the BlockToRow stage, the fields are now scalar values, in a different slot.
    for (size_t i = 0; i < topLevelReqs.size(); ++i) {
        auto field = getTopLevelField(topLevelReqs[i]);
        if (field == unpackNode->bucketSpec.timeField()) {
            outputs.set(std::make_pair(PlanStageSlots::kField, field),
                        TypedSlot{unpackedSlots[i], TypeSignature::kDateTimeType});
        } else {
            outputs.set(std::make_pair(PlanStageSlots::kField, field),
                        TypedSlot{unpackedSlots[i], TypeSignature::kAnyScalarType});
        }
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
    if (reqs.has(PlanStageSlots::kResult)) {
        std::vector<std::string> fieldNames;
        sbe::value::SlotVector fieldSlots;
        outputs.forEachSlot([&](const PlanStageSlots::Name& name, const TypedSlot& slot) {
            if (name.first == PlanStageSlots::kField) {
                fieldNames.push_back(std::string{name.second});
                fieldSlots.push_back(slot.slotId);
            }
        });

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
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
