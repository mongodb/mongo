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

#include "mongo/db/query/ce/sampling_executor.h"

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::optimizer::ce {

SBESamplingExecutor::~SBESamplingExecutor() {}

std::pair<sbe::value::TypeTags, sbe::value::Value> SBESamplingExecutor::execute(
    const Metadata& metadata,
    const QueryParameterMap& queryParameters,
    const PlanAndProps& planAndProps) const {
    auto env = VariableEnvironment::build(planAndProps._node);
    SlotVarMap slotMap;
    auto runtimeEnvironment = std::make_unique<sbe::RuntimeEnvironment>();  // TODO Use factory
    boost::optional<sbe::value::SlotId> ridSlot;
    sbe::value::SlotIdGenerator ids;
    sbe::InputParamToSlotMap inputParamToSlotMap;

    SBENodeLowering g{
        env, *runtimeEnvironment, ids, inputParamToSlotMap, metadata, planAndProps._map};
    auto sbePlan = g.optimize(planAndProps._node, slotMap, ridSlot);
    tassert(6624261, "Unexpected rid slot", !ridSlot);

    // TODO: return errors instead of exceptions?
    uassert(6624244, "Lowering failed", sbePlan != nullptr);
    uassert(6624245, "Invalid slot map size", slotMap.size() == 1);

    // Bind query parameters to runtime slots.
    for (auto&& [paramId, slotId] : inputParamToSlotMap) {
        auto [paramTag, paramVal] = queryParameters.at(paramId).get();
        auto accessor = runtimeEnvironment->getAccessor(slotId);
        accessor->reset(false, paramTag, paramVal);
    }

    sbePlan->attachToOperationContext(_opCtx);
    sbe::CompileCtx ctx(std::move(runtimeEnvironment));
    sbePlan->prepare(ctx);

    std::vector<sbe::value::SlotAccessor*> accessors;
    for (auto& [name, slot] : slotMap) {
        accessors.emplace_back(sbePlan->getAccessor(ctx, slot));
    }

    sbePlan->open(false);
    ON_BLOCK_EXIT([&] { sbePlan->close(); });

    if (sbePlan->getNext() == sbe::PlanState::IS_EOF) {
        return {sbe::value::TypeTags::Nothing, 0};
    }

    auto result = accessors.at(0)->getCopyOfValue();
    tassert(8375701,
            "Sampling query returned more than one row",
            sbePlan->getNext() == sbe::PlanState::IS_EOF);
    return result;
}

}  // namespace mongo::optimizer::ce
