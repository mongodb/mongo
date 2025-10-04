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

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"

#include <memory>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <boost/optional/optional.hpp>

namespace mongo {
class InMatchExpression;
class StringListSet;
class PlanYieldPolicySBE;
class AccumulationStatement;
struct WindowFunctionStatement;

namespace sbe {
class InList;
}

namespace stage_builder {
struct Environment;
struct PlanStageStaticData;

static constexpr auto kNothingEnvSlotName = "nothing"_sd;

/**
 * Common parameters to SBE stage builder functions extracted into separate class to simplify
 * argument passing. Also contains a mapping of global variable ids to slot ids.
 */
struct StageBuilderState {
    using InListsMap = absl::flat_hash_map<const InMatchExpression*, sbe::InList*>;
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
                      InListsMap* inListsMap,
                      CollatorsMap* collatorsMap,
                      SortSpecMap* sortSpecMap,
                      boost::intrusive_ptr<ExpressionContext> expCtx,
                      bool needsMerge,
                      bool allowDiskUse,
                      IncrementalFeatureRolloutContext& ifrContext)
        : slotIdGenerator{slotIdGenerator},
          frameIdGenerator{frameIdGenerator},
          spoolIdGenerator{spoolIdGenerator},
          inListsMap{inListsMap},
          collatorsMap{collatorsMap},
          sortSpecMap{sortSpecMap},
          opCtx{opCtx},
          env{env},
          data{data},
          variables{variables},
          yieldPolicy{yieldPolicy},
          expCtx{expCtx},
          needsMerge{needsMerge},
          allowDiskUse{allowDiskUse},
          ifrContext(ifrContext) {}

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
    sbe::value::SlotId getEmptyObjSlot();
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
    const CollatorInterface* makeOwnedCollator(const CollatorInterface* coll);

    /**
     * Given an in-list in the form of an InMatchExpression, this method builds an "owned" InList
     * that can be used to search the in-list's elements and returns a pointer to it. This InList
     * object will be owned by the PlanStageStaticData.
     *
     * If an InList has already been built for the specified InMatchExpression and is found in
     * 'inListsMap', this method will return a pointer to the same InList (instead of building
     * a new InList).
     */
    sbe::InList* makeOwnedInList(const InMatchExpression* ime);

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

    InListsMap* const inListsMap;
    CollatorsMap* const collatorsMap;
    SortSpecMap* const sortSpecMap;

    OperationContext* const opCtx;
    Environment& env;
    PlanStageStaticData* const data;

    const Variables& variables;

    PlanYieldPolicySBE* const yieldPolicy{nullptr};

    boost::intrusive_ptr<ExpressionContext> expCtx;

    // When the mongos splits $group stage and sends it to shards, it adds 'needsMerge'/'fromMongos'
    // flags to true so that shards can sends special partial aggregation results to the mongos.
    // However, as $group can now be pushed down entirely if it specifies the entire shard key in
    // _id, pipelines can exist with a fully pushed down $group followed by a partially
    // pushed down $group - only the latter is permitted to generate partial results.
    // To control this, generateGroupFinalStage may selectively override this to false
    // for groups which have been fully pushed down.
    bool needsMerge;

    // A flag to indicate the user allows disk use for spilling.
    bool allowDiskUse;

    IncrementalFeatureRolloutContext& ifrContext;

    SimpleBSONObjMap<sbe::value::SlotId> keyPatternToSlotMap;
};  // struct StageBuilderState
}  // namespace stage_builder
}  // namespace mongo
