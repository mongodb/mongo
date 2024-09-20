/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/gen_eexpr_helpers.h"

#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/virtual_scan.h"

namespace mongo::stage_builder {
std::unique_ptr<sbe::EExpression> makeIf(std::unique_ptr<sbe::EExpression> condExpr,
                                         std::unique_ptr<sbe::EExpression> thenExpr,
                                         std::unique_ptr<sbe::EExpression> elseExpr) {
    return sbe::makeE<sbe::EIf>(std::move(condExpr), std::move(thenExpr), std::move(elseExpr));
}

std::unique_ptr<sbe::EExpression> makeLet(sbe::FrameId frameId,
                                          sbe::EExpression::Vector bindExprs,
                                          std::unique_ptr<sbe::EExpression> expr) {
    return sbe::makeE<sbe::ELocalBind>(frameId, std::move(bindExprs), std::move(expr));
}

std::unique_ptr<sbe::EExpression> makeLocalLambda(sbe::FrameId frameId,
                                                  std::unique_ptr<sbe::EExpression> expr) {
    return sbe::makeE<sbe::ELocalLambda>(frameId, std::move(expr));
}

std::unique_ptr<sbe::EExpression> makeNumericConvert(std::unique_ptr<sbe::EExpression> expr,
                                                     sbe::value::TypeTags tag) {
    return sbe::makeE<sbe::ENumericConvert>(std::move(expr), tag);
}

std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy,
    PlanNodeId planNodeId) {
    // The value passed in must be an array.
    invariant(sbe::value::isArray(arrTag));

    auto outputSlot = slotIdGenerator->generate();
    auto virtualScan = sbe::makeS<sbe::VirtualScanStage>(planNodeId, outputSlot, arrTag, arrVal);

    // Return the VirtualScanStage and its output slot.
    return {outputSlot, std::move(virtualScan)};
}

std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy,
    PlanNodeId planNodeId) {
    using namespace std::literals;

    invariant(numSlots >= 1);

    // Generate a mock scan with a single output slot.
    auto [scanSlot, scanStage] =
        generateVirtualScan(slotIdGenerator, arrTag, arrVal, yieldPolicy, planNodeId);

    // Create a ProjectStage that will read the data from 'scanStage' and split it up
    // across multiple output slots.
    sbe::value::SlotVector projectSlots;
    sbe::SlotExprPairVector projections;
    for (int32_t i = 0; i < numSlots; ++i) {
        auto indexExpr = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                    sbe::value::bitcastFrom<int32_t>(i));

        projectSlots.emplace_back(slotIdGenerator->generate());
        projections.emplace_back(
            projectSlots.back(),
            sbe::makeE<sbe::EFunction>(
                "getElement"_sd,
                sbe::makeEs(sbe::makeE<sbe::EVariable>(scanSlot), std::move(indexExpr))));
    }

    return {
        std::move(projectSlots),
        sbe::makeS<sbe::ProjectStage>(std::move(scanStage), std::move(projections), planNodeId)};
}
}  // namespace mongo::stage_builder
