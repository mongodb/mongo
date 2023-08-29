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
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/timeseries/timeseries_constants.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {
namespace sv = sbe::value;

std::vector<sv::CellBlock::PathRequest> getCellPathReqs(const UnpackTsBucketNode* unpackNode) {
    auto&& fieldSet = unpackNode->bucketSpec.fieldSet();
    auto&& computedMetaProjFields = unpackNode->bucketSpec.computedMetaProjFields();

    std::vector<sv::CellBlock::PathRequest> pathReqs;
    pathReqs.reserve(fieldSet.size() - computedMetaProjFields.size());
    for (auto&& field : fieldSet) {
        // The computed meta fields must not be included in cell path requests.
        if (computedMetaProjFields.find(field) != computedMetaProjFields.end()) {
            continue;
        }
        pathReqs.emplace_back(
            sv::CellBlock::PathRequest{{sv::CellBlock::Get{field}, sv::CellBlock::Id{}}});
    }
    return pathReqs;
}

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

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildUnpackTsBucket(const QuerySolutionNode* root,
                                           const PlanStageReqs& reqs) {
    const auto unpackNode = static_cast<const UnpackTsBucketNode*>(root);

    // Sets up request for child stage.
    auto childReqs = reqs.copy().set(kResult);
    if (auto&& computedMetas = unpackNode->bucketSpec.computedMetaProjFields();
        !computedMetas.empty()) {
        for (auto&& computedMeta : computedMetas) {
            childReqs.set(std::pair(PlanStageSlots::kField, computedMeta));
        }
    }
    // TODO SERVER-79700: Do not request the meta field if the bucket-level filter has dependency
    // on the 'meta' field. Also we should be able to avoid materialize the temporary BSON object
    // for the computed meta fields.
    if (unpackNode->includeMeta) {
        childReqs.set(std::pair(PlanStageSlots::kField, timeseries::kBucketMetaFieldName));
    }

    // Builds child stage.
    auto [childStage, childOutputs] = build(unpackNode->children[0].get(), childReqs);
    printPlan(*childStage);
    auto bucketSlot = childOutputs.get(kResult);

    // TODO SERVER-79699: Handle the 'wholeBucketFilter'.

    // Creates the TsBucketToCellBlockStage.
    const auto optMetaSlot = [&]() -> boost::optional<sbe::value::SlotId> {
        if (unpackNode->includeMeta) {
            return {_slotIdGenerator.generate()};
        } else {
            return boost::none;
        }
    }();
    auto pathReqs = getCellPathReqs(unpackNode);
    auto blockSlots = _slotIdGenerator.generateMultiple(pathReqs.size());
    std::unique_ptr<sbe::PlanStage> stage =
        std::make_unique<sbe::TsBucketToCellBlockStage>(std::move(childStage),
                                                        bucketSlot,
                                                        pathReqs,
                                                        blockSlots,
                                                        optMetaSlot,
                                                        unpackNode->bucketSpec.timeField(),
                                                        unpackNode->nodeId());
    printPlan(*stage);

    // Adds the BlockToRowStage.
    auto unpackedSlots = _slotIdGenerator.generateMultiple(blockSlots.size());
    stage = std::make_unique<sbe::BlockToRowStage>(std::move(stage),
                                                   blockSlots,
                                                   unpackedSlots,
                                                   boost::none, /* bitmap */
                                                   unpackNode->nodeId(),
                                                   _yieldPolicy);
    printPlan(*stage);

    // Begins populating our output map.
    PlanStageSlots outputs;
    // Adds slots unpacked from CellBlocks.
    for (size_t i = 0; i < pathReqs.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, getTopLevelField(pathReqs[i])),
                    unpackedSlots[i]);
    }
    // Adds slots for the computed meta fields.
    if (auto&& computedMetas = unpackNode->bucketSpec.computedMetaProjFields();
        !computedMetas.empty()) {
        for (auto&& computedMeta : computedMetas) {
            outputs.set(std::pair(PlanStageSlots::kField, computedMeta),
                        childOutputs.get(std::pair(PlanStageSlots::kField, computedMeta)));
        }
    }
    // Re-maps the "meta" field to the user-specified meta field.
    if (optMetaSlot) {
        auto metaField = unpackNode->bucketSpec.metaField();
        tassert(7969800, "'meta' field does not exist but requested", metaField);
        outputs.set(std::pair(PlanStageSlots::kField, *metaField), *optMetaSlot);
    }


    // Add filter stage(s) for the per-event filter.
    if (auto eventFilter = unpackNode->eventFilter.get()) {
        auto [eventFilterByPath, eventFilterResidual] =
            expression::splitMatchExpressionForColumns(eventFilter);
        {
            sbe::EExpression::Vector andBranches;
            for (auto& [_, filterMatchExpr] : eventFilterByPath) {
                auto eventFilterSbExpr = generateFilter(
                    _state, filterMatchExpr.get(), /*rootSlot*/ boost::none, &outputs);

                andBranches.push_back(eventFilterSbExpr.extractExpr(_state));
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
                stage = sbe::makeS<sbe::FilterStage<false>>(
                    std::move(stage), eventFilterSbExpr.extractExpr(_state), unpackNode->nodeId());
                printPlan(*stage);
            }
        }
    }

    if (reqs.has(PlanStageSlots::kResult)) {
        // Creates the result object if the caller requested a materialized 'kResult' object.
        auto resultSlot = _slotIdGenerator.generate();
        outputs.set(kResult, resultSlot);

        std::vector<std::string> objFields;
        sbe::value::SlotVector objSlots;
        // Includes 'cellPaths' in the result object.
        for (size_t i = 0; i < pathReqs.size(); ++i) {
            objFields.push_back(getTopLevelField(pathReqs[i]));
            objSlots.push_back(unpackedSlots[i]);
        }
        // Includes the computed meta fields in the result object.
        if (auto&& computedMetas = unpackNode->bucketSpec.computedMetaProjFields();
            !computedMetas.empty()) {
            for (auto&& computedMeta : computedMetas) {
                objFields.push_back(computedMeta);
                objSlots.push_back(
                    childOutputs.get(std::pair(PlanStageSlots::kField, computedMeta)));
            }
        }
        // Includes the user-level meta field in the result object.
        if (optMetaSlot) {
            objFields.push_back(*unpackNode->bucketSpec.metaField());
            objSlots.push_back(*optMetaSlot);
        }

        stage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(stage),
                                                  resultSlot,                  // objSlot
                                                  boost::none,                 // rootSlot
                                                  boost::none,                 // fieldBehavior
                                                  std::vector<std::string>{},  // fields
                                                  objFields,                   // projectFields
                                                  objSlots,                    // projectVars
                                                  true,                        // forceNewObject
                                                  false,                       // returnOldObject
                                                  unpackNode->nodeId());
        printPlan(*stage);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
