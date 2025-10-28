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

boost::optional<PlanStageReqs> makeExtractFieldPathsPlanStageReqs(
    StageBuilderState& state,
    const std::vector<const Expression*>& expressions,
    const PlanStageSlots& childStageOutputs) {
    if (!state.ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagExtractFieldPathsSbeStage)) {
        LOGV2_DEBUG(11087205,
                    3,
                    "ExpressionFieldPath rejected for ExtractFieldPathsStage",
                    "reason"_attr = "feature flag is disabled");
        return boost::none;
    }
    if (childStageOutputs.hasBlockOutput()) {
        LOGV2_DEBUG(11087206,
                    3,
                    "Child stage outputs rejected for ExtractFieldPathsStage",
                    "reason"_attr = "has block output");
        return boost::none;
    }
    bool ok = true;
    PlanStageReqs extractFieldPathsReqs;
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

    auto childStageOutputsData = childStageOutputs.getSlotNameToIdMap();
    for (const std::string& pathExpr : extractFieldPathsReqs.getPathExprs()) {
        FieldPath fieldPath{pathExpr};
        tassert(11163705,
                "expected child stage of extract_field_paths stage to have all required "
                "toplevel fields",
                childStageOutputs.has({PlanStageSlots::kField, fieldPath.getFieldName(0)}));
    }

    return boost::make_optional(extractFieldPathsReqs);
}

template <typename T>
sbe::value::Path toPath(const T& fullPath) {
    sbe::value::Path ret;

    FieldPath fieldPath{fullPath};
    for (size_t i = 0; i < fieldPath.getPathLength() - 1; ++i) {
        ret.emplace_back(sbe::value::Get{.field = std::string(fieldPath.getFieldName(i))});
        ret.emplace_back(sbe::value::Traverse{});
    }
    // Omit the Traverse for the last path component.
    if (fieldPath.getPathLength() != 0) {
        ret.emplace_back(sbe::value::Get{
            .field = std::string(fieldPath.getFieldName(fieldPath.getPathLength() - 1))});
    }
    ret.emplace_back(sbe::value::Id{});

    return ret;
}

std::pair<SbStage, PlanStageSlots> buildExtractFieldPaths(SbStage stage,
                                                          StageBuilderState& state,
                                                          const PlanStageSlots& childStageOutputs,
                                                          PlanStageReqs& extractFieldPathsReqs,
                                                          const PlanNodeId nodeId) {
    std::vector<std::pair<sbe::value::Path, sbe::value::SlotId>> outputs;

    PlanStageSlots extractionOutputs;
    for (const std::string& fullPath : extractFieldPathsReqs.getPathExprs()) {
        FieldPath fieldPath{fullPath};
        tassert(11087200,
                "extract_field_paths does not extract toplevel fields that already have slots",
                !childStageOutputs.has({PlanStageSlots::kField, fullPath}));
        // Create slot id for path.
        sbe::value::SlotId slot = state.slotId();
        outputs.push_back({toPath(fullPath), slot});
        extractionOutputs.set(std::pair(PlanStageSlots::kPathExpr, fullPath), SbSlot{slot});
    }
    tassert(10757507, "expected nonempty outputs", outputs.size() > 0);

    std::vector<std::pair<sbe::value::Path, sbe::value::SlotId>> inputs;
    // Extract fields from a set of toplevel field slots.
    for (auto& p : childStageOutputs.getSlotNameToIdMap()) {
        const PlanStageSlots::UnownedSlotName& slotName = p.first;
        if (slotName.first != PlanStageSlots::kField) {
            continue;
        }
        auto path = toPath(slotName.second);
        tassert(11163701,
                "Expected only toplevel paths as input to extract_field_paths stage",
                path.size() == 2);
        std::pair<sbe::value::Path, sbe::value::SlotId> input = {path, p.second.getId()};
        inputs.push_back(input);
    }
    tassert(11163700, "Expected nonempty inputs", !inputs.empty());

    return {sbe::makeS<sbe::ExtractFieldPathsStage>(std::move(stage), inputs, outputs, nodeId),
            extractionOutputs};
}
}  // namespace mongo::stage_builder
