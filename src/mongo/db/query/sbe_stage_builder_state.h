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

#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <boost/optional/optional.hpp>
#include <memory>

#include "mongo/db/exec/sbe/values/slot.h"

namespace mongo {
class InListData;
class StringListSet;
class PlanYieldPolicySBE;
class AccumulationStatement;
struct WindowFunctionStatement;

namespace stage_builder {
struct Environment;
struct PlanStageStaticData;

static constexpr auto kNothingEnvSlotName = "nothing"_sd;

/**
 * Common parameters to SBE stage builder functions extracted into separate class to simplify
 * argument passing. Also contains a mapping of global variable ids to slot ids.
 */
struct StageBuilderState {
    using InListsSet = absl::flat_hash_set<InListData*>;
    using CollatorsMap = absl::flat_hash_map<const CollatorInterface*, const CollatorInterface*>;
    using SortSpecMap = absl::flat_hash_map<const void*, sbe::value::SlotId>;

    StageBuilderState(OperationContext* opCtx,
                      Environment& env,
                      PlanStageStaticData* data,
                      const Variables& variables,
                      PlanYieldPolicySBE* yieldPolicy,
                      sbe::value::SlotIdGenerator* slotIdGenerator,
                      sbe::value::FrameIdGenerator* frameIdGenerator,
                      sbe::value::SpoolIdGenerator* spoolIdGenerator,
                      InListsSet* inListsSet,
                      CollatorsMap* collatorsMap,
                      SortSpecMap* sortSpecMap,
                      boost::intrusive_ptr<ExpressionContext> expCtx,
                      bool needsMerge,
                      bool allowDiskUse)
        : slotIdGenerator{slotIdGenerator},
          frameIdGenerator{frameIdGenerator},
          spoolIdGenerator{spoolIdGenerator},
          inListsSet{inListsSet},
          collatorsMap{collatorsMap},
          sortSpecMap{sortSpecMap},
          opCtx{opCtx},
          env{env},
          data{data},
          variables{variables},
          yieldPolicy{yieldPolicy},
          expCtx{expCtx},
          needsMerge{needsMerge},
          allowDiskUse{allowDiskUse} {}

    StageBuilderState(const StageBuilderState& other) = delete;

    sbe::value::SlotId getGlobalVariableSlot(Variables::Id variableId);

    sbe::value::SlotId slotId() {
        return slotIdGenerator->generate();
    }

    sbe::FrameId frameId() {
        return frameIdGenerator->generate();
    }

    sbe::SpoolId spoolId() {
        return spoolIdGenerator->generate();
    }

    sbe::value::SlotId getNothingSlot();
    sbe::value::SlotId getSortSpecSlot(const AccumulationStatement* sortPattern);
    sbe::value::SlotId getSortSpecSlot(const WindowFunctionStatement* sortPattern);

    boost::optional<sbe::value::SlotId> getTimeZoneDBSlot();
    boost::optional<sbe::value::SlotId> getCollatorSlot();
    boost::optional<sbe::value::SlotId> getOplogTsSlot();
    boost::optional<sbe::value::SlotId> getBuiltinVarSlot(Variables::Id id);

    bool isNothingSlot(sbe::value::SlotId slot);

    /**
     * Given a CollatorInterface, returns a copy of the CollatorInterface that is owned by the
     * SBE plan currently being built. If 'coll' is already owned by the SBE plan being built,
     * then this method will simply return 'coll'.
     */
    const CollatorInterface* makeCollatorOwned(const CollatorInterface* coll);

    /**
     * Given an InListData 'inList', this method makes inList's BSON owned, it makes the inList's
     * Collator owned, it sorts and de-dups the inList's elements if needed, it initializes the
     * inList's hash set if needed, and it marks the 'inList' as "prepared".
     */
    InListData* prepareOwnedInList(const std::shared_ptr<InListData>& inList);

    /**
     * Register a Slot in the 'RuntimeEnvironment'. The newly registered Slot should be associated
     * with 'paramId' and tracked in the 'InputParamToSlotMap' for auto-parameterization use. The
     * slot is set to 'Nothing' on registration and will be populated with the real value when
     * preparing the SBE plan for execution.
     */
    sbe::value::SlotId registerInputParamSlot(MatchExpression::InputParamId paramId);

    sbe::value::SlotIdGenerator* const slotIdGenerator;
    sbe::value::FrameIdGenerator* const frameIdGenerator;
    sbe::value::SpoolIdGenerator* const spoolIdGenerator;

    InListsSet* const inListsSet;
    CollatorsMap* const collatorsMap;
    SortSpecMap* const sortSpecMap;

    OperationContext* const opCtx;
    Environment& env;
    PlanStageStaticData* const data;

    const Variables& variables;

    PlanYieldPolicySBE* const yieldPolicy{nullptr};

    boost::intrusive_ptr<ExpressionContext> expCtx;

    // When the mongos splits $group stage and sends it to shards, it adds 'needsMerge'/'fromMongs'
    // flags to true so that shards can sends special partial aggregation results to the mongos.
    bool needsMerge;

    // A flag to indicate the user allows disk use for spilling.
    bool allowDiskUse;

    StringMap<sbe::value::SlotId> stringConstantToSlotMap;
    SimpleBSONObjMap<sbe::value::SlotId> keyPatternToSlotMap;
};  // struct StageBuilderState
}  // namespace stage_builder
}  // namespace mongo
