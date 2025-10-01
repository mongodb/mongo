/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/gen_extract_field_paths.h"

#include "mongo/db/exec/sbe/stages/extract_field_paths.h"
#include "mongo/db/query/expression_walker.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {

bool eligibleForExtractFieldPathsStage(PlanStageSlots& childStageOutputs) {
    return !childStageOutputs.hasBlockOutput() && childStageOutputs.hasResultObj();
}

boost::optional<PlanStageReqs> makeExtractFieldPathsPlanStageReqs(
    StageBuilderState& state,
    const std::vector<const Expression*>& expressions,
    PlanStageSlots& childStageOutputs) {
    PlanStageReqs extractFieldPathsReqs;
    if (!state.ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagExtractFieldPathsSbeStage)) {
        return boost::none;
    }
    if (!eligibleForExtractFieldPathsStage(childStageOutputs)) {
        return boost::none;
    }
    bool ok = true;
    for (const Expression* expression : expressions) {
        if (!ok) {
            break;
        }
        FieldPathVisitor visitor([&](const ExpressionFieldPath* e) {
            if (!ok) {
                return;
            }
            if (e->isROOT()) {
                ok = false;
                LOGV2_DEBUG(11087201,
                            3,
                            "ExpressionFieldPath rejected for ExtractFieldPathsStage",
                            "fullPath"_attr = e->getFieldPath().fullPath(),
                            "reason"_attr = "ROOT");
                return;
            }
            if (e->getFieldPath().getPathLength() == 1) {
                // Typical field paths are prefixed with CURRENT, so this does not exclude toplevel
                // field accesses.
                LOGV2_DEBUG(11087202,
                            3,
                            "ExpressionFieldPath rejected for ExtractFieldPathsStage",
                            "fullPath"_attr = e->getFieldPath().fullPath(),
                            "reason"_attr = "path length 1");
                ok = false;
                return;
            }
            boost::optional<Variables::Id> varId = e->getVariableId();
            if (varId.has_value() && Variables::isBuiltin(*varId) &&
                ((*varId) != Variables::kRootId)) {
                LOGV2_DEBUG(11087203,
                            3,
                            "ExpressionFieldPath rejected for ExtractFieldPathsStage",
                            "fullPath"_attr = e->getFieldPath().fullPath(),
                            "reason"_attr = "path access on builtin variable");
                ok = false;
                return;
            }

            FieldPath fp = e->getFieldPathWithoutCurrentPrefix();
            // Don't extract fields already in slots.
            if (!childStageOutputs.has({PlanStageSlots::kField, fp.fullPath()})) {
                PlanStageReqs::OwnedSlotName slotName{PlanStageSlots::kPathExpr, fp.fullPath()};
                extractFieldPathsReqs.set(slotName);
            }
        });
        ExpressionWalker walker(&visitor, nullptr /*inVisitor*/, nullptr /*postVisitor*/);
        expression_walker::walk<const Expression>(expression, &walker);
    }
    if (!ok) {
        return boost::none;
    }
    if (extractFieldPathsReqs.size() == 0) {
        return boost::none;
    }
    return boost::make_optional(extractFieldPathsReqs);
}

std::pair<SbStage, PlanStageSlots> buildExtractFieldPaths(SbStage stage,
                                                          StageBuilderState& state,
                                                          const PlanStageSlots& childStageOutputs,
                                                          PlanStageReqs& extractFieldPathsReqs) {
    sbe::value::SlotVector outSlots;
    std::vector<sbe::value::CellBlock::Path> pathReqs;
    PlanStageSlots extractionOutputs;
    for (const std::string& fullPath : extractFieldPathsReqs.getPathExprs()) {
        FieldPath fieldPath{fullPath};
        tassert(11087200,
                "extract_field_paths does not extract toplevel fields that already have slots",
                !childStageOutputs.has({PlanStageSlots::kField, fullPath}));
        // Create path.
        sbe::value::CellBlock::Path path;
        for (size_t i = 0; i < fieldPath.getPathLength() - 1; ++i) {
            path.emplace_back(
                sbe::value::CellBlock::Get{.field = std::string(fieldPath.getFieldName(i))});
            path.emplace_back(sbe::value::CellBlock::Traverse{});
        }
        // Omit the Traverse for the last path component.
        path.emplace_back(sbe::value::CellBlock::Get{
            .field = std::string(fieldPath.getFieldName(fieldPath.getPathLength() - 1))});
        path.emplace_back(sbe::value::CellBlock::Id{});
        pathReqs.push_back(std::move(path));

        // Create slot id for path.
        sbe::value::SlotId slot = state.slotId();
        outSlots.emplace_back(slot);
        extractionOutputs.set(std::pair(PlanStageSlots::kPathExpr, fullPath), SbSlot{slot});
    }
    tassert(10757507, "expected nonempty outSlots", !outSlots.empty());
    auto childResultSlot = childStageOutputs.getResultObj();

    return {sbe::makeS<sbe::ExtractFieldPathsStage>(std::move(stage),
                                                    childResultSlot.getId(),
                                                    pathReqs,  // TODO this is by value
                                                    std::move(outSlots),
                                                    stage->getCommonStats()->nodeId),
            extractionOutputs};
}
}  // namespace mongo::stage_builder
